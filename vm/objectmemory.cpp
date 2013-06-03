#include <stdlib.h>
#include <iostream>
#include <iomanip>
#include <sys/time.h>

#include "config.h"
#include "vm.hpp"
#include "objectmemory.hpp"
#include "capi/tag.hpp"
#include "gc/marksweep.hpp"
#include "gc/baker.hpp"
#include "gc/immix.hpp"
#include "gc/inflated_headers.hpp"
#include "gc/walker.hpp"
#include "on_stack.hpp"

#include "config_parser.hpp"

#include "builtin/class.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/tuple.hpp"
#include "builtin/io.hpp"
#include "builtin/fiber.hpp"
#include "builtin/string.hpp"
#include "builtin/lookuptable.hpp"
#include "builtin/ffi_pointer.hpp"
#include "builtin/data.hpp"
#include "builtin/dir.hpp"
#include "builtin/array.hpp"
#include "builtin/thread.hpp"
#include "builtin/exception.hpp"

#include "capi/handles.hpp"
#include "configuration.hpp"

#include "global_cache.hpp"

#include "instruments/tooling.hpp"
#include "dtrace/dtrace.h"

// Used by XMALLOC at the bottom
static long gc_malloc_threshold = 0;
static long bytes_until_collection = 0;

namespace rubinius {

  Object* object_watch = 0;

  /* ObjectMemory methods */
  ObjectMemory::ObjectMemory(VM* state, Configuration& config)
    : young_(new BakerGC(this, config.gc_bytes))
    , mark_sweep_(new MarkSweepGC(this, config))
    , immix_(new ImmixGC(this))
    , inflated_headers_(new InflatedHeaders(state))
    , capi_handles_(new capi::Handles)
    , code_manager_(&state->shared)
    , mark_(1)
    , allow_gc_(true)
    , slab_size_(4096)
    , shared_(state->shared)

    , collect_young_now(false)
    , collect_mature_now(false)
    , root_state_(state)
    , last_object_id(1)
  {
    // TODO Not sure where this code should be...
#ifdef ENABLE_OBJECT_WATCH
    if(char* num = getenv("RBX_WATCH")) {
      object_watch = reinterpret_cast<Object*>(strtol(num, NULL, 10));
      std::cout << "Watching for " << object_watch << "\n";
    }
#endif

    large_object_threshold = config.gc_large_object;
    young_->set_lifetime(config.gc_lifetime);

    if(config.gc_autotune) young_->set_autotune();

    for(size_t i = 0; i < LastObjectType; i++) {
      type_info[i] = NULL;
    }

    TypeInfo::init(this);

    gc_malloc_threshold = config.gc_malloc_threshold;
    bytes_until_collection = gc_malloc_threshold;
  }

  ObjectMemory::~ObjectMemory() {
    mark_sweep_->free_objects();

    // @todo free immix data

    for(size_t i = 0; i < LastObjectType; i++) {
      if(type_info[i]) delete type_info[i];
    }

    delete immix_;
    delete mark_sweep_;
    delete young_;

    for(std::list<capi::GlobalHandle*>::iterator i = global_capi_handle_locations_.begin();
          i != global_capi_handle_locations_.end(); ++i) {
      delete *i;
    }
    global_capi_handle_locations_.clear();

    delete capi_handles_;
    // Must be last
    delete inflated_headers_;
  }

  void ObjectMemory::on_fork(STATE) {
    lock_init(state->vm());
    contention_lock_.init();
  }

  void ObjectMemory::assign_object_id(STATE, Object* obj) {
    // Double check we've got no id still after the lock.
    if(obj->object_id(state) > 0) return;

    obj->set_object_id(state, atomic::fetch_and_add(&last_object_id, (size_t)1));
  }

  bool ObjectMemory::inflate_lock_count_overflow(STATE, ObjectHeader* obj,
                                                 int count)
  {
    utilities::thread::SpinLock::LockGuard guard(inflation_lock_);

    HeaderWord orig = obj->header;

    if(orig.f.meaning == eAuxWordInflated) {
      return false;
    }

    uint32_t ih_header = 0;
    InflatedHeader* ih = inflated_headers_->allocate(obj, &ih_header);
    ih->update(state, orig);
    ih->initialize_mutex(state->vm()->thread_id(), count);
    ih->mark(this, mark_);

    while(!obj->set_inflated_header(state, ih_header, orig)) {
      orig = obj->header;

      if(orig.f.meaning == eAuxWordInflated) {
        return false;
      }
      ih->update(state, orig);
      ih->initialize_mutex(state->vm()->thread_id(), count);
    }
    return true;
  }

  LockStatus ObjectMemory::contend_for_lock(STATE, GCToken gct, CallFrame* call_frame,
                                            ObjectHeader* obj, size_t us, bool interrupt)
  {
    bool timed = false;
    bool timeout = false;
    struct timespec ts = {0,0};

    OnStack<1> os(state, obj);

    {
      GCLockGuard lg(state, gct, call_frame, contention_lock_);

      // We want to lock obj, but someone else has it locked.
      //
      // If the lock is already inflated, no problem, just lock it!

      // Be sure obj is updated by the GC while we're waiting for it

step1:
      // Only contend if the header is thin locked.
      // Ok, the header is not inflated, but we can't inflate it and take
      // the lock because the locking thread needs to do that, so indicate
      // that the object is being contended for and then wait on the
      // contention condvar until the object is unlocked.

      HeaderWord orig         = obj->header;
      HeaderWord new_val      = orig;
      orig.f.meaning          = eAuxWordLock;
      new_val.f.LockContended = 1;

      if(!obj->header.atomic_set(orig, new_val)) {
        if(obj->inflated_header_p()) {
          if(cDebugThreading) {
            std::cerr << "[LOCK " << state->vm()->thread_id()
              << " contend_for_lock error: object has been inflated.]" << std::endl;
          }
          return eLockError;
        }
        if(new_val.f.meaning != eAuxWordLock) {
          if(cDebugThreading) {
            std::cerr << "[LOCK " << state->vm()->thread_id()
              << " contend_for_lock error: not thin locked.]" << std::endl;
          }
          return eLockError;
        }

        // Something changed since we started to down this path,
        // start over.
        goto step1;
      }

      // Ok, we've registered the lock contention, now spin and wait
      // for the us to be told to retry.

      if(cDebugThreading) {
        std::cerr << "[LOCK " << state->vm()->thread_id() << " waiting on contention]" << std::endl;
      }

      if(us > 0) {
        timed = true;
        struct timeval tv;
        gettimeofday(&tv, NULL);

        ts.tv_sec = tv.tv_sec + (us / 1000000);
        ts.tv_nsec = (us % 1000000) * 1000;
      }

      state->vm()->set_sleeping();

      while(!obj->inflated_header_p()) {
        GCIndependent gc_guard(state, call_frame);

        if(timed) {
          timeout = (contention_var_.wait_until(contention_lock_, &ts) == utilities::thread::cTimedOut);
          if(timeout) break;
        } else {
          contention_var_.wait(contention_lock_);
        }

        if(cDebugThreading) {
          std::cerr << "[LOCK " << state->vm()->thread_id() << " notified of contention breakage]" << std::endl;
        }

        // Someone is interrupting us trying to lock.
        if(interrupt && state->check_local_interrupts()) {
          state->vm()->clear_check_local_interrupts();

          if(!state->vm()->interrupted_exception()->nil_p()) {
            if(cDebugThreading) {
              std::cerr << "[LOCK " << state->vm()->thread_id() << " detected interrupt]" << std::endl;
            }

            state->vm()->clear_sleeping();
            return eLockInterrupted;
          }
        }
      }

      state->vm()->clear_sleeping();

      if(cDebugThreading) {
        std::cerr << "[LOCK " << state->vm()->thread_id() << " contention broken]" << std::endl;
      }

      if(timeout) {
        if(cDebugThreading) {
          std::cerr << "[LOCK " << state->vm()->thread_id() << " contention timed out]" << std::endl;
        }

        return eLockTimeout;
      }
    } // contention_lock_ guard

    // We lock the InflatedHeader here rather than returning
    // and letting ObjectHeader::lock because the GC might have run
    // and we've used OnStack<> specificly to deal with that.
    //
    // ObjectHeader::lock doesn't use OnStack<>, it just is sure to
    // not access this if there is chance that a call blocked and GC'd
    // (which is true in the case of this function).

    InflatedHeader* ih = obj->inflated_header(state);

    if(timed) {
      return ih->lock_mutex_timed(state, gct, call_frame, obj, &ts, interrupt);
    } else {
      return ih->lock_mutex(state, gct, call_frame, obj, 0, interrupt);
    }
  }

  void ObjectMemory::release_contention(STATE, GCToken gct, CallFrame* call_frame) {
    GCLockGuard lg(state, gct, call_frame, contention_lock_);
    contention_var_.broadcast();
  }

  bool ObjectMemory::inflate_and_lock(STATE, ObjectHeader* obj) {
    utilities::thread::SpinLock::LockGuard guard(inflation_lock_);

    InflatedHeader* ih = 0;
    uint32_t ih_index = 0;
    int initial_count = 0;

    HeaderWord orig = obj->header;

    switch(orig.f.meaning) {
    case eAuxWordEmpty:
      // ERROR, we can not be here because it's empty. This is only to
      // be called when the header is already in use.
      return false;
    case eAuxWordObjID:
      // We could be have made a header before trying again, so
      // keep using the original one.
      ih = inflated_headers_->allocate(obj, &ih_index);
      ih->set_object_id(orig.f.aux_word);
      break;
    case eAuxWordLock:
      // We have to locking the object to inflate it, thats the law.
      if(orig.f.aux_word >> cAuxLockTIDShift != state->vm()->thread_id()) {
        return false;
      }

      ih = inflated_headers_->allocate(obj, &ih_index);
      initial_count = orig.f.aux_word & cAuxLockRecCountMask;
      break;
    case eAuxWordHandle:
      // Handle in use so inflate and update handle
      ih = inflated_headers_->allocate(obj, &ih_index);
      ih->set_handle(state, obj->handle(state));
      break;
    case eAuxWordInflated:
      // Already inflated. ERROR, let the caller sort it out.
      if(cDebugThreading) {
        std::cerr << "[LOCK " << state->vm()->thread_id() << " asked to inflated already inflated lock]" << std::endl;
      }
      return false;
    }

    ih->initialize_mutex(state->vm()->thread_id(), initial_count);
    ih->mark(this, mark_);

    while(!obj->set_inflated_header(state, ih_index, orig)) {
      // The header can't have been inflated by another thread, the
      // inflation process holds the OM lock.
      //
      // So some other bits must have changed, so lets just spin and
      // keep trying to update it.

      // Sanity check that the meaning is still the same, if not, then
      // something is really wrong.
      if(orig.f.meaning != obj->header.f.meaning) {
        if(cDebugThreading) {
          std::cerr << "[LOCK object header consistence error detected.]" << std::endl;
        }
        return false;
      }
      orig = obj->header;
      if(orig.f.meaning == eAuxWordInflated) {
        return false;
      }
    }

    return true;
  }

  bool ObjectMemory::inflate_for_contention(STATE, ObjectHeader* obj) {
    utilities::thread::SpinLock::LockGuard guard(inflation_lock_);

    for(;;) {
      HeaderWord orig = obj->header;

      InflatedHeader* ih = 0;
      uint32_t ih_header = 0;

      switch(orig.f.meaning) {
      case eAuxWordEmpty:
        ih = inflated_headers_->allocate(obj, &ih_header);
        break;
      case eAuxWordObjID:
        // We could be have made a header before trying again, so
        // keep using the original one.
        ih = inflated_headers_->allocate(obj, &ih_header);
        ih->set_object_id(orig.f.aux_word);
        break;
      case eAuxWordHandle:
        ih = inflated_headers_->allocate(obj, &ih_header);
        ih->set_handle(state, obj->handle(state));
        break;
      case eAuxWordLock:
        // We have to be locking the object to inflate it, thats the law.
        if(orig.f.aux_word >> cAuxLockTIDShift != state->vm()->thread_id()) {
          if(cDebugThreading) {
            std::cerr << "[LOCK " << state->vm()->thread_id() << " object locked by another thread while inflating for contention]" << std::endl;
          }
          return false;
        }
        if(cDebugThreading) {
          std::cerr << "[LOCK " << state->vm()->thread_id() << " being unlocked and inflated atomicly]" << std::endl;
        }

        ih = inflated_headers_->allocate(obj, &ih_header);
        break;
      case eAuxWordInflated:
        if(cDebugThreading) {
          std::cerr << "[LOCK " << state->vm()->thread_id() << " asked to inflated already inflated lock]" << std::endl;
        }
        return false;
      }

      ih->mark(this, mark_);

      // Try it all over again if it fails.
      if(!obj->set_inflated_header(state, ih_header, orig)) {
        ih->clear();
        continue;
      }

      obj->clear_lock_contended();

      if(cDebugThreading) {
        std::cerr << "[LOCK " << state->vm()->thread_id() << " inflated lock for contention.]" << std::endl;
      }

      // Now inflated but not locked, which is what we want.
      return true;
    }
  }

  bool ObjectMemory::refill_slab(STATE, gc::Slab& slab) {
    utilities::thread::SpinLock::LockGuard guard(allocation_lock_);

    Address addr = young_->allocate_for_slab(slab_size_);

    gc_stats.slab_allocated(slab.allocations(), slab.byte_used());

    if(addr) {
      slab.refill(addr, slab_size_);
      return true;
    } else {
      slab.refill(0, 0);
      return false;
    }
  }

  void ObjectMemory::set_young_lifetime(size_t age) {
    SYNC_TL;

    young_->set_lifetime(age);
  }

  void ObjectMemory::debug_marksweep(bool val) {
    SYNC_TL;

    if(val) {
      mark_sweep_->free_entries = false;
    } else {
      mark_sweep_->free_entries = true;
    }
  }

  bool ObjectMemory::valid_object_p(Object* obj) {
    if(obj->young_object_p()) {
      return young_->validate_object(obj) == cValid;
    } else if(obj->mature_object_p()) {
      return immix_->validate_object(obj) == cInImmix;
    } else {
      return false;
    }
  }

  /* Garbage collection */

  Object* ObjectMemory::promote_object(Object* obj) {

    size_t sz = obj->size_in_bytes(root_state_);

    Object* copy = immix_->move_object(obj, sz);

    gc_stats.promoted_object_allocated(sz);
    if(unlikely(!copy)) {
      copy = mark_sweep_->move_object(obj, sz, &collect_mature_now);
    }

#ifdef ENABLE_OBJECT_WATCH
    if(watched_p(obj)) {
      std::cout << "detected object " << obj << " during promotion.\n";
    }
#endif

    return copy;
  }

  void ObjectMemory::collect_maybe(STATE, GCToken gct, CallFrame* call_frame) {
    // Don't go any further unless we're allowed to GC.
    if(!can_gc()) return;

    while(!state->stop_the_world()) {
      state->checkpoint(gct, call_frame);

      // Someone else got to the GC before we did! No problem, all done!
      if(!collect_young_now && !collect_mature_now) return;
    }

    // Ok, everyone in stopped! LET'S GC!
    SYNC(state);

    state->shared().finalizer_handler()->start_collection(state);

    if(cDebugThreading) {
      std::cerr << std::endl << "[" << state
                << " WORLD beginning GC.]" << std::endl;
    }

    GCData gc_data(state->vm(), gct);

    uint64_t start_time = 0;

    if(collect_young_now) {
      if(state->shared().config.gc_show) {
        start_time = get_current_time();
      }

      YoungCollectStats stats;

      RUBINIUS_GC_BEGIN(0);
#ifdef RBX_PROFILER
      if(unlikely(state->vm()->tooling())) {
        tooling::GCEntry method(state, tooling::GCYoung);
        collect_young(state, &gc_data, &stats);
      } else {
        collect_young(state, &gc_data, &stats);
      }
#else
      collect_young(state, &gc_data, &stats);
#endif

      RUBINIUS_GC_END(0);

      if(state->shared().config.gc_show) {
        uint64_t fin_time = get_current_time();
        int diff = (fin_time - start_time) / 1000000;

        std::cerr << "[GC " << std::fixed << std::setprecision(1) << stats.percentage_used << "% "
                  << stats.promoted_objects << "/" << stats.excess_objects << " "
                  << stats.lifetime << " " << diff << "ms]" << std::endl;

        if(state->shared().config.gc_noisy) {
          std::cerr << "\a" << std::flush;
        }
      }
    }

    if(collect_mature_now) {
      size_t before_kb = 0;

      if(state->shared().config.gc_show) {
        start_time = get_current_time();
        before_kb = mature_bytes_allocated() / 1024;
      }

      RUBINIUS_GC_BEGIN(1);
#ifdef RBX_PROFILER
      if(unlikely(state->vm()->tooling())) {
        tooling::GCEntry method(state, tooling::GCMature);
        collect_mature(state, &gc_data);
      } else {
        collect_mature(state, &gc_data);
      }
#else
      collect_mature(state, &gc_data);
#endif

      RUBINIUS_GC_END(1);

      if(state->shared().config.gc_show) {
        uint64_t fin_time = get_current_time();
        int diff = (fin_time - start_time) / 1000000;
        size_t kb = mature_bytes_allocated() / 1024;
        std::cerr << "[Full GC " << before_kb << "kB => " << kb << "kB " << diff << "ms]" << std::endl;

        if(state->shared().config.gc_noisy) {
          std::cerr << "\a\a" << std::flush;
        }
      }
    }

    state->shared().finalizer_handler()->finish_collection(state);
    state->restart_world();

    UNSYNC;
  }

  void ObjectMemory::collect_young(STATE, GCData* data, YoungCollectStats* stats) {
#ifndef RBX_GC_STRESS_YOUNG
    collect_young_now = false;
#endif

    timer::Running<1000000> timer(gc_stats.total_young_collection_time,
                                  gc_stats.last_young_collection_time);

    young_->reset_stats();

    young_->collect(data, stats);

    prune_handles(data->handles(), data->cached_handles(), young_);
    gc_stats.young_collection_count++;

    data->global_cache()->prune_young();

    if(data->threads()) {
      for(std::list<ManagedThread*>::iterator i = data->threads()->begin();
          i != data->threads()->end();
          ++i) {
        gc::Slab& slab = (*i)->local_slab();

        gc_stats.slab_allocated(slab.allocations(), slab.byte_used());

        // Reset the slab to a size of 0 so that the thread has to do
        // an allocation to get a proper refill. This keeps the number
        // of threads in the system from starving the available
        // number of slabs.
        slab.refill(0, 0);
      }
    }

    young_->reset();
#ifdef RBX_GC_DEBUG
    young_->verify(data);
#endif
  }

  void ObjectMemory::collect_mature(STATE, GCData* data) {

    timer::Running<1000000> timer(gc_stats.total_full_collection_time,
                                  gc_stats.last_full_collection_time);
#ifndef RBX_GC_STRESS_MATURE
    collect_mature_now = false;
#endif

    code_manager_.clear_marks();
    clear_fiber_marks(data->threads());

    immix_->reset_stats();

    immix_->collect(data);

    code_manager_.sweep();

    data->global_cache()->prune_unmarked(mark());

    prune_handles(data->handles(), data->cached_handles(), NULL);

    // Have to do this after all things that check for mark bits is
    // done, as it free()s objects, invalidating mark bits.
    mark_sweep_->after_marked();

    inflated_headers_->deallocate_headers(mark());

#ifdef RBX_GC_DEBUG
    immix_->verify(data);
#endif

    rotate_mark();
    gc_stats.full_collection_count++;

  }

  void ObjectMemory::inflate_for_id(STATE, ObjectHeader* obj, uint32_t id) {
    utilities::thread::SpinLock::LockGuard guard(inflation_lock_);

    HeaderWord orig = obj->header;

    if(orig.f.meaning == eAuxWordInflated) {
      obj->inflated_header(state)->set_object_id(id);
      return;
    }

    uint32_t ih_index = 0;
    InflatedHeader* ih = inflated_headers_->allocate(obj, &ih_index);
    ih->update(state, orig);
    ih->set_object_id(id);
    ih->mark(this, mark_);

    while(!obj->set_inflated_header(state, ih_index, orig)) {
      orig = obj->header;

      if(orig.f.meaning == eAuxWordInflated) {
        obj->inflated_header(state)->set_object_id(id);
        ih->clear();
        return;
      }
      ih->update(state, orig);
      ih->set_object_id(id);
    }

  }

  void ObjectMemory::inflate_for_handle(STATE, ObjectHeader* obj, capi::Handle* handle) {
    utilities::thread::SpinLock::LockGuard guard(inflation_lock_);

    HeaderWord orig = obj->header;

    if(orig.f.meaning == eAuxWordInflated) {
      obj->inflated_header(state)->set_handle(state, handle);
      return;
    }

    uint32_t ih_index = 0;
    InflatedHeader* ih = inflated_headers_->allocate(obj, &ih_index);
    ih->update(state, orig);
    ih->set_handle(state, handle);
    ih->mark(this, mark_);

    while(!obj->set_inflated_header(state, ih_index, orig)) {
      orig = obj->header;

      if(orig.f.meaning == eAuxWordInflated) {
        obj->inflated_header(state)->set_handle(state, handle);
        ih->clear();
        return;
      }
      ih->update(state, orig);
      ih->set_handle(state, handle);
    }

  }

  void ObjectMemory::prune_handles(capi::Handles* handles, std::list<capi::Handle*>* cached, BakerGC* young) {
    handles->deallocate_handles(cached, mark(), young);
  }

  void ObjectMemory::clear_fiber_marks(std::list<ManagedThread*>* threads) {
    if(threads) {
      for(std::list<ManagedThread*>::iterator i = threads->begin();
          i != threads->end();
          ++i) {
        if(VM* vm = (*i)->as_vm()) {
          vm->gc_fiber_clear_mark();
        }
      }
    }
  }

  size_t ObjectMemory::mature_bytes_allocated() {
    return immix_->bytes_allocated() + mark_sweep_->allocated_bytes;
  }

  void ObjectMemory::add_type_info(TypeInfo* ti) {
    SYNC_TL;

    if(TypeInfo* current = type_info[ti->type]) {
      delete current;
    }
    type_info[ti->type] = ti;
  }

  Object* ObjectMemory::allocate_object(size_t bytes) {

    Object* obj;

    if(unlikely(bytes > large_object_threshold)) {
      obj = mark_sweep_->allocate(bytes, &collect_mature_now);
      if(unlikely(!obj)) return NULL;

      gc_stats.mature_object_allocated(bytes);

      if(collect_mature_now) shared_.gc_soon();

    } else {
      obj = young_->allocate(bytes, &collect_young_now);
      if(unlikely(obj == NULL)) {
        collect_young_now = true;
        shared_.gc_soon();

        obj = immix_->allocate(bytes);

        if(unlikely(!obj)) {
          obj = mark_sweep_->allocate(bytes, &collect_mature_now);
        }

        gc_stats.mature_object_allocated(bytes);

        if(collect_mature_now) shared_.gc_soon();
      } else {
        gc_stats.young_object_allocated(bytes);
      }
    }

#ifdef ENABLE_OBJECT_WATCH
    if(watched_p(obj)) {
      std::cout << "detected " << obj << " during allocation\n";
    }
#endif

    return obj;
  }

  Object* ObjectMemory::allocate_object_mature(size_t bytes) {

    Object* obj;

    if(bytes > large_object_threshold) {
      obj = mark_sweep_->allocate(bytes, &collect_mature_now);
      if(unlikely(!obj)) return NULL;
    } else {
      obj = immix_->allocate(bytes);

      if(unlikely(!obj)) {
        obj = mark_sweep_->allocate(bytes, &collect_mature_now);
      }

      gc_stats.mature_object_allocated(bytes);
    }

    if(collect_mature_now) shared_.gc_soon();

#ifdef ENABLE_OBJECT_WATCH
    if(watched_p(obj)) {
      std::cout << "detected " << obj << " during mature allocation\n";
    }
#endif

    return obj;
  }

  Object* ObjectMemory::new_object_typed_dirty(STATE, Class* cls, size_t bytes, object_type type) {
    utilities::thread::SpinLock::LockGuard guard(allocation_lock_);

    Object* obj;

    obj = allocate_object(bytes);
    if(unlikely(!obj)) return NULL;

    obj->set_obj_type(type);
    obj->klass(this, cls);
    obj->ivars(this, cNil);

    return obj;
  }

  Object* ObjectMemory::new_object_typed(STATE, Class* cls, size_t bytes, object_type type) {
    Object* obj = new_object_typed_dirty(state, cls, bytes, type);
    if(unlikely(!obj)) return NULL;

    obj->clear_fields(bytes);
    return obj;
  }

  Object* ObjectMemory::new_object_typed_mature_dirty(STATE, Class* cls, size_t bytes, object_type type) {
    utilities::thread::SpinLock::LockGuard guard(allocation_lock_);

    Object* obj;

    obj = allocate_object_mature(bytes);
    if(unlikely(!obj)) return NULL;

    obj->set_obj_type(type);
    obj->klass(this, cls);
    obj->ivars(this, cNil);

    return obj;
  }

  Object* ObjectMemory::new_object_typed_mature(STATE, Class* cls, size_t bytes, object_type type) {
    Object* obj = new_object_typed_mature_dirty(state, cls, bytes, type);
    if(unlikely(!obj)) return NULL;

    obj->clear_fields(bytes);
    return obj;
  }

  /* ONLY use to create Class, the first object. */
  Object* ObjectMemory::allocate_object_raw(size_t bytes) {

    Object* obj = mark_sweep_->allocate(bytes, &collect_mature_now);
    if(unlikely(!obj)) return NULL;

    gc_stats.mature_object_allocated(bytes);
    obj->clear_fields(bytes);
    return obj;
  }

  Object* ObjectMemory::new_object_typed_enduring_dirty(STATE, Class* cls, size_t bytes, object_type type) {
    utilities::thread::SpinLock::LockGuard guard(allocation_lock_);

    Object* obj = mark_sweep_->allocate(bytes, &collect_mature_now);
    if(unlikely(!obj)) return NULL;

    gc_stats.mature_object_allocated(bytes);

    if(collect_mature_now) shared_.gc_soon();

#ifdef ENABLE_OBJECT_WATCH
    if(watched_p(obj)) {
      std::cout << "detected " << obj << " during enduring allocation\n";
    }
#endif

    obj->set_obj_type(type);
    obj->klass(this, cls);
    obj->ivars(this, cNil);

    return obj;
  }

  Object* ObjectMemory::new_object_typed_enduring(STATE, Class* cls, size_t bytes, object_type type) {
    Object* obj = new_object_typed_enduring_dirty(state, cls, bytes, type);
    if(unlikely(!obj)) return NULL;

    obj->clear_fields(bytes);
    return obj;
  }

  TypeInfo* ObjectMemory::find_type_info(Object* obj) {
    return type_info[obj->type_id()];
  }

  ObjectPosition ObjectMemory::validate_object(Object* obj) {
    ObjectPosition pos;

    pos = young_->validate_object(obj);
    if(pos != cUnknown) return pos;

    pos = immix_->validate_object(obj);
    if(pos != cUnknown) return pos;

    return mark_sweep_->validate_object(obj);
  }

  bool ObjectMemory::valid_young_object_p(Object* obj) {
    return obj->young_object_p() && young_->in_current_p(obj);
  }

  void ObjectMemory::add_code_resource(CodeResource* cr) {
    SYNC_TL;

    code_manager_.add_resource(cr, &collect_mature_now);
  }

  void* ObjectMemory::young_start() {
    return young_->start_address();
  }

  void* ObjectMemory::yound_end() {
    return young_->last_address();
  }

  void ObjectMemory::needs_finalization(Object* obj, FinalizerFunction func) {
    if(FinalizerHandler* fh = shared_.finalizer_handler()) {
      fh->record(obj, func);
    }
  }

  void ObjectMemory::set_ruby_finalizer(Object* obj, Object* finalizer) {
    shared_.finalizer_handler()->set_ruby_finalizer(obj, finalizer);
  }

  capi::Handle* ObjectMemory::add_capi_handle(STATE, Object* obj) {
    if(!obj->reference_p()) {
      rubinius::bug("Trying to add a handle for a non reference");
    }
    uintptr_t handle_index = capi_handles_->allocate_index(state, obj);
    obj->set_handle_index(state, handle_index);
    return obj->handle(state);
  }

  void ObjectMemory::add_global_capi_handle_location(STATE, capi::Handle** loc,
                                               const char* file, int line) {
    SYNC(state);
    if(*loc && REFERENCE_P(*loc)) {
      if(!capi_handles_->validate(*loc)) {
        std::cerr << std::endl << "==================================== ERROR ====================================" << std::endl;
        std::cerr << "| An extension is trying to add an invalid handle at the following location:  |" << std::endl;
        std::ostringstream out;
        out << file << ":" << line;
        std::cerr << "| " << std::left << std::setw(75) << out.str() << " |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| An invalid handle means that it points to an invalid VALUE. This can happen |" << std::endl;
        std::cerr << "| when you haven't initialized the VALUE pointer yet, in which case we        |" << std::endl;
        std::cerr << "| suggest either initializing it properly or otherwise first initialize it to |" << std::endl;
        std::cerr << "| NULL if you can only set it to a proper VALUE pointer afterwards. Consider  |" << std::endl;
        std::cerr << "| the following example that could cause this problem:                        |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| VALUE ptr;                                                                  |" << std::endl;
        std::cerr << "| rb_gc_register_address(&ptr);                                               |" << std::endl;
        std::cerr << "| ptr = rb_str_new(\"test\");                                                   |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| Either change this register after initializing                              |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| VALUE ptr;                                                                  |" << std::endl;
        std::cerr << "| ptr = rb_str_new(\"test\");                                                   |" << std::endl;
        std::cerr << "| rb_gc_register_address(&ptr);                                               |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| Or initialize it with NULL:                                                 |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| VALUE ptr = NULL;                                                           |" << std::endl;
        std::cerr << "| rb_gc_register_address(&ptr);                                               |" << std::endl;
        std::cerr << "| ptr = rb_str_new(\"test\");                                                   |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| Please note that this is NOT a problem in Rubinius, but in the extension    |" << std::endl;
        std::cerr << "| that contains the given file above. A very common source of this problem is |" << std::endl;
        std::cerr << "| using older versions of therubyracer before 0.11.x. Please upgrade to at    |" << std::endl;
        std::cerr << "| least version 0.11.x if you're using therubyracer and encounter this        |" << std::endl;
        std::cerr << "| problem. For some more background information on why this is a problem      |" << std::endl;
        std::cerr << "| with therubyracer, you can read the following blog post:                    |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "| http://blog.thefrontside.net/2012/12/04/therubyracer-rides-again/           |" << std::endl;
        std::cerr << "|                                                                             |" << std::endl;
        std::cerr << "================================== ERROR ======================================" << std::endl;
        rubinius::bug("Halting due to invalid handle");
      }
    }

    capi::GlobalHandle* global_handle = new capi::GlobalHandle(loc, file, line);
    global_capi_handle_locations_.push_back(global_handle);
  }

  void ObjectMemory::del_global_capi_handle_location(STATE, capi::Handle** loc) {
    SYNC(state);

    for(std::list<capi::GlobalHandle*>::iterator i = global_capi_handle_locations_.begin();
        i != global_capi_handle_locations_.end(); ++i) {
      if((*i)->handle() == loc) {
        delete *i;
        global_capi_handle_locations_.erase(i);
        return;
      }
    }
    rubinius::bug("Removing handle not in the list");
  }

  void ObjectMemory::make_capi_handle_cached(STATE, capi::Handle* handle) {
    SYNC(state);
    cached_capi_handles_.push_back(handle);
  }

  size_t& ObjectMemory::loe_usage() {
    return mark_sweep_->allocated_bytes;
  }

  size_t& ObjectMemory::immix_usage() {
    return immix_->bytes_allocated();
  }

  size_t& ObjectMemory::code_usage() {
    return code_manager_.size();
  }

  void ObjectMemory::memstats() {
    int total = 0;

    int baker = root_state_->shared.config.gc_bytes * 2;
    total += baker;

    int immix = immix_->bytes_allocated();
    total += immix;

    int large = mark_sweep_->allocated_bytes;
    total += large;

    int code = code_manager_.size();
    total += code;

    int shared = root_state_->shared.size();
    total += shared;

    std::cout << "baker: " << baker << "\n";
    std::cout << "immix: " << immix << "\n";
    std::cout << "large: " << large << "\n"
              << "        objects: " << mark_sweep_->allocated_objects << "\n"
              << "        times: " << mark_sweep_->times_collected << "\n"
              << "        last_freed: " << mark_sweep_->last_freed << "\n";
    std::cout << " code: " << code << "\n";
    std::cout << "shared: " << shared << "\n";

    std::cout << "total: "
              << ((double)total / (1024 * 1024))
              << " M\n";

    std::cout << "CodeManager:\n";
    std::cout << "  total allocated: " << code_manager_.total_allocated() << "\n";
    std::cout << "      total freed: " << code_manager_.total_freed() << "\n";
  }

};


// The following memory functions are defined in ruby.h for use by C-API
// extensions, and also used by library code lifted from MRI (e.g. Oniguruma).
// They provide some book-keeping around memory usage for non-VM code, so that
// the garbage collector is run periodically in response to memory allocations
// in non-VM code.
// Without these  checks, memory can become exhausted without the VM being aware
// there is a problem. As this memory may only be being used by Ruby objects
// that have become garbage, performing a garbage collection periodically after
// a significant amount of memory has been malloc-ed should keep non-VM memory
// usage from growing uncontrollably.


void* XMALLOC(size_t bytes) {
  bytes_until_collection -= bytes;
  if(bytes_until_collection <= 0) {
    rubinius::VM::current()->run_gc_soon();
    bytes_until_collection = gc_malloc_threshold;
  }
  return malloc(bytes);
}

void XFREE(void* ptr) {
  free(ptr);
}

void* XREALLOC(void* ptr, size_t bytes) {
  bytes_until_collection -= bytes;
  if(bytes_until_collection <= 0) {
    rubinius::VM::current()->run_gc_soon();
    bytes_until_collection = gc_malloc_threshold;
  }

  return realloc(ptr, bytes);
}

void* XCALLOC(size_t items, size_t bytes_per) {
  size_t bytes = bytes_per * items;

  bytes_until_collection -= bytes;
  if(bytes_until_collection <= 0) {
    rubinius::VM::current()->run_gc_soon();
    bytes_until_collection = gc_malloc_threshold;
  }

  return calloc(items, bytes_per);
}

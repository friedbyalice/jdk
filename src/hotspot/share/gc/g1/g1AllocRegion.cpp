/*
 * Copyright (c) 2011, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/g1/g1AllocRegion.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1EvacStats.inline.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/align.hpp"

G1CollectedHeap* G1AllocRegion::_g1h = nullptr;
G1HeapRegion* G1AllocRegion::_dummy_region = nullptr;

void G1AllocRegion::setup(G1CollectedHeap* g1h, G1HeapRegion* dummy_region) {
  assert(_dummy_region == nullptr, "should be set once");
  assert(dummy_region != nullptr, "pre-condition");
  assert(dummy_region->free() == 0, "pre-condition");

  // Make sure that any allocation attempt on this region will fail
  // and will not trigger any asserts.
  DEBUG_ONLY(size_t assert_tmp);
  assert(dummy_region->par_allocate(1, 1, &assert_tmp) == nullptr, "should fail");

  _g1h = g1h;
  _dummy_region = dummy_region;
}

size_t G1AllocRegion::fill_up_remaining_space(G1HeapRegion* alloc_region) {
  assert(alloc_region != nullptr && alloc_region != _dummy_region,
         "pre-condition");
  size_t result = 0;

  // Other threads might still be trying to allocate using a CAS out
  // of the region we are trying to retire, as they can do so without
  // holding the lock. So, we first have to make sure that no one else
  // can allocate out of it by doing a maximal allocation. Even if our
  // CAS attempt fails a few times, we'll succeed sooner or later
  // given that failed CAS attempts mean that the region is getting
  // closed to being full.
  size_t free_word_size = alloc_region->free() / HeapWordSize;

  // This is the minimum free chunk we can turn into a dummy
  // object. If the free space falls below this, then no one can
  // allocate in this region anyway (all allocation requests will be
  // of a size larger than this) so we won't have to perform the dummy
  // allocation.
  size_t min_word_size_to_fill = CollectedHeap::min_fill_size();

  while (free_word_size >= min_word_size_to_fill) {
    HeapWord* dummy = par_allocate(alloc_region, free_word_size);
    if (dummy != nullptr) {
      // If the allocation was successful we should fill in the space. If the
      // allocation was in old any necessary BOT updates will be done.
      alloc_region->fill_with_dummy_object(dummy, free_word_size);
      alloc_region->set_pre_dummy_top(dummy);
      result += free_word_size * HeapWordSize;
      break;
    }

    free_word_size = alloc_region->free() / HeapWordSize;
    // It's also possible that someone else beats us to the
    // allocation and they fill up the region. In that case, we can
    // just get out of the loop.
  }
  result += alloc_region->free();

  assert(alloc_region->free() / HeapWordSize < min_word_size_to_fill,
         "post-condition");
  return result;
}

size_t G1AllocRegion::retire_internal(G1HeapRegion* alloc_region, bool fill_up) {
  // We never have to check whether the active region is empty or not,
  // and potentially free it if it is, given that it's guaranteed that
  // it will never be empty.
  size_t waste = 0;
  assert_alloc_region(!alloc_region->is_empty(),
                      "the alloc region should never be empty");

  if (fill_up) {
    waste = fill_up_remaining_space(alloc_region);
  }

  retire_region(alloc_region);

  return waste;
}

size_t G1AllocRegion::retire(bool fill_up) {
  assert_alloc_region(_alloc_region != nullptr, "not initialized properly");

  size_t waste = 0;

  trace("retiring");
  G1HeapRegion* alloc_region = _alloc_region;
  if (alloc_region != _dummy_region) {
    waste = retire_internal(alloc_region, fill_up);
    reset_alloc_region();
  }
  trace("retired");

  return waste;
}

HeapWord* G1AllocRegion::new_alloc_region_and_allocate(size_t word_size) {
  assert_alloc_region(_alloc_region == _dummy_region, "pre-condition");

  trace("attempting region allocation");
  G1HeapRegion* new_alloc_region = allocate_new_region(word_size);
  if (new_alloc_region != nullptr) {
    new_alloc_region->reset_pre_dummy_top();

    assert(new_alloc_region->is_empty(), "new regions should be empty");
    HeapWord* result = new_alloc_region->allocate(word_size);
    assert_alloc_region(result != nullptr, "the allocation should succeeded");

    OrderAccess::storestore();
    // Note that we first perform the allocation and then we store the
    // region in _alloc_region. This is the reason why an active region
    // can never be empty.
    update_alloc_region(new_alloc_region);
    trace("region allocation successful");
    return result;
  } else {
    trace("region allocation failed");
    return nullptr;
  }
  ShouldNotReachHere();
}

void G1AllocRegion::init() {
  trace("initializing");
  assert_alloc_region(_alloc_region == nullptr, "pre-condition");
  assert_alloc_region(_dummy_region != nullptr, "should have been set");
  _alloc_region = _dummy_region;
  _count = 0;
  trace("initialized");
}

void G1AllocRegion::set(G1HeapRegion* alloc_region) {
  trace("setting");
  assert_alloc_region(_alloc_region == _dummy_region && _count == 0, "pre-condition");

  update_alloc_region(alloc_region);
  trace("set");
}

void G1AllocRegion::update_alloc_region(G1HeapRegion* alloc_region) {
  trace("update");
  // We explicitly check that the region is not empty to make sure we
  // maintain the "the alloc region cannot be empty" invariant.
  assert_alloc_region(alloc_region != nullptr && !alloc_region->is_empty(), "pre-condition");

  _alloc_region = alloc_region;
  _count += 1;
  trace("updated");
}

G1HeapRegion* G1AllocRegion::release() {
  trace("releasing");
  G1HeapRegion* alloc_region = _alloc_region;
  retire(false /* fill_up */);
  assert_alloc_region(_alloc_region == _dummy_region, "post-condition of retire()");
  _alloc_region = nullptr;
  trace("released");
  return (alloc_region == _dummy_region) ? nullptr : alloc_region;
}

#ifndef PRODUCT
void G1AllocRegion::trace(const char* str, size_t min_word_size, size_t desired_word_size, size_t actual_word_size, HeapWord* result) {
  // All the calls to trace that set either just the size or the size
  // and the result are considered part of detailed tracing and are
  // skipped during other tracing.

  Log(gc, alloc, region) log;

  if (!log.is_debug()) {
    return;
  }

  bool detailed_info = log.is_trace();

  if ((actual_word_size == 0 && result == nullptr) || detailed_info) {
    LogStream ls_trace(log.trace());
    LogStream ls_debug(log.debug());
    outputStream* out = detailed_info ? &ls_trace : &ls_debug;

    out->print("%s: %u ", _name, _count);

    if (_alloc_region == nullptr) {
      out->print("null");
    } else if (_alloc_region == _dummy_region) {
      out->print("DUMMY");
    } else {
      out->print(HR_FORMAT, HR_FORMAT_PARAMS(_alloc_region));
    }

    out->print(" : %s", str);

    if (detailed_info) {
      if (result != nullptr) {
        out->print(" min %zu desired %zu actual %zu " PTR_FORMAT,
                   min_word_size, desired_word_size, actual_word_size, p2i(result));
      } else if (min_word_size != 0) {
        out->print(" min %zu desired %zu", min_word_size, desired_word_size);
      }
    }
    out->cr();
  }
}
#endif // PRODUCT

G1AllocRegion::G1AllocRegion(const char* name, uint node_index)
  : _alloc_region(nullptr),
    _count(0),
    _name(name),
    _node_index(node_index)
 { }

G1HeapRegion* MutatorAllocRegion::allocate_new_region(size_t word_size) {
  return _g1h->new_mutator_alloc_region(word_size, _node_index);
}

void MutatorAllocRegion::retire_region(G1HeapRegion* alloc_region) {
  _g1h->retire_mutator_alloc_region(alloc_region, alloc_region->used());
}

void MutatorAllocRegion::init() {
  assert(_retained_alloc_region == nullptr, "Pre-condition");
  G1AllocRegion::init();
  _wasted_bytes = 0;
}

bool MutatorAllocRegion::should_retain(G1HeapRegion* region) {
  size_t free_bytes = region->free();
  if (free_bytes < MinTLABSize) {
    return false;
  }

  if (_retained_alloc_region != nullptr &&
      free_bytes < _retained_alloc_region->free()) {
    return false;
  }

  return true;
}

size_t MutatorAllocRegion::retire(bool fill_up) {
  size_t waste = 0;
  trace("retiring");
  G1HeapRegion* current_region = get();
  if (current_region != nullptr) {
    // Retain the current region if it fits a TLAB and has more
    // free than the currently retained region.
    if (should_retain(current_region)) {
      trace("mutator retained");
      if (_retained_alloc_region != nullptr) {
        waste = retire_internal(_retained_alloc_region, true);
      }
      _retained_alloc_region = current_region;
    } else {
      waste = retire_internal(current_region, fill_up);
    }
    reset_alloc_region();
  }

  _wasted_bytes += waste;
  trace("retired");
  return waste;
}

size_t MutatorAllocRegion::used_in_alloc_regions() {
  size_t used = 0;
  G1HeapRegion* hr = get();
  if (hr != nullptr) {
    used += hr->used();
  }

  hr = _retained_alloc_region;
  if (hr != nullptr) {
    used += hr->used();
  }
  return used;
}

G1HeapRegion* MutatorAllocRegion::release() {
  G1HeapRegion* ret = G1AllocRegion::release();

  // The retained alloc region must be retired and this must be
  // done after the above call to release the mutator alloc region,
  // since it might update the _retained_alloc_region member.
  if (_retained_alloc_region != nullptr) {
    _wasted_bytes += retire_internal(_retained_alloc_region, false);
    _retained_alloc_region = nullptr;
  }
  log_debug(gc, alloc, region)("Mutator Allocation stats, regions: %u, wasted size: %zu%s (%4.1f%%)",
                               count(),
                               byte_size_in_proper_unit(_wasted_bytes),
                               proper_unit_for_byte_size(_wasted_bytes),
                               percent_of(_wasted_bytes, count() * G1HeapRegion::GrainBytes));
  return ret;
}

G1HeapRegion* G1GCAllocRegion::allocate_new_region(size_t word_size) {
  return _g1h->new_gc_alloc_region(word_size, _purpose, _node_index);
}

void G1GCAllocRegion::retire_region(G1HeapRegion* alloc_region) {
  assert(alloc_region->used() >= _used_bytes_before, "invariant");
  size_t allocated_bytes = alloc_region->used() - _used_bytes_before;
  _g1h->retire_gc_alloc_region(alloc_region, allocated_bytes, _purpose);
  _used_bytes_before = 0;
}

size_t G1GCAllocRegion::retire(bool fill_up) {
  G1HeapRegion* retired = get();
  size_t end_waste = G1AllocRegion::retire(fill_up);
  // Do not count retirement of the dummy allocation region.
  if (retired != nullptr) {
    _stats->add_region_end_waste(end_waste / HeapWordSize);
  }
  return end_waste;
}

void G1GCAllocRegion::reuse(G1HeapRegion* alloc_region) {
  _used_bytes_before = alloc_region->used();
  set(alloc_region);
}

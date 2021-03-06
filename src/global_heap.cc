// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2019 The Mesh Authors. All rights reserved.
// Use of this source code is governed by the Apache License,
// Version 2.0, that can be found in the LICENSE file.

#include "global_heap.h"

#include "meshing.h"
#include "runtime.h"

namespace mesh {

MiniHeap *GetMiniHeap(const MiniHeapID id) {
  hard_assert(id.hasValue());

  return runtime().heap().miniheapForID(id);
}

MiniHeapID GetMiniHeapID(const MiniHeap *mh) {
  if (unlikely(mh == nullptr)) {
    d_assert(false);
    return MiniHeapID{0};
  }

  return runtime().heap().miniheapIDFor(mh);
}

void *GlobalHeap::malloc(size_t sz) {
#ifndef NDEBUG
  if (unlikely(sz <= kMaxSize)) {
    abort();
  }
#endif

  const auto pageCount = PageCount(sz);

  // prevent integer underflows
  if (unlikely(pageCount * kPageSize > INT_MAX)) {
    return nullptr;
  }

  return pageAlignedAlloc(1, pageCount);
}

void GlobalHeap::free(void *ptr) {
  auto mh = miniheapFor(ptr);
  if (unlikely(!mh)) {
#ifndef NDEBUG
    if (ptr != nullptr) {
      debug("FIXME: free of untracked ptr %p", ptr);
    }
#endif
    return;
  }
  this->freeFor(mh, ptr);
}

void GlobalHeap::freeFor(MiniHeap *mh, void *ptr) {
  if (unlikely(ptr == nullptr)) {
    return;
  }

  if (unlikely(!mh)) {
    return;
  }

  bool shouldConsiderMesh = 0;
  {
    lock_guard<mutex> lock(_miniheapLock);

    // large objects are tracked with a miniheap per object and don't
    // trigger meshing, because they are multiples of the page size.
    // This can also include, for example, single page allocations w/
    // 16KB alignment.
    if (mh->maxCount() == 1) {
      freeMiniheapLocked(mh, false);
      return;
    }

    d_assert(mh->maxCount() > 1);

    _lastMeshEffective.store(1, std::memory_order::memory_order_release);
    mh->free(arenaBegin(), ptr);

    if (unlikely(mh->isMeshed())) {
      // our MiniHeap was meshed out from underneath us.  Grab the
      // global lock to synchronize with a concurrent mesh, and
      // re-update the bitmap
      mh = miniheapFor(ptr);
      hard_assert(!mh->isMeshed());
      mh->free(arenaBegin(), ptr);
    }

    const auto remaining = mh->inUseCount();
    shouldConsiderMesh = remaining > 0;

    const auto sizeClass = mh->sizeClass();

    // this may free the miniheap -- we can't safely access it after
    // this point.
    bool shouldFlush = _littleheaps[sizeClass].postFree(mh, remaining);
    mh = nullptr;

    if (unlikely(shouldFlush)) {
      flushBinLocked(sizeClass);
    }
  }

  if (shouldConsiderMesh) {
    maybeMesh();
  }
}

int GlobalHeap::mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
  unique_lock<mutex> lock(_miniheapLock);

  if (!oldp || !oldlenp || *oldlenp < sizeof(size_t))
    return -1;

  auto statp = reinterpret_cast<size_t *>(oldp);

  if (strcmp(name, "mesh.check_period") == 0) {
    *statp = _meshPeriod;
    if (!newp || newlen < sizeof(size_t))
      return -1;
    auto newVal = reinterpret_cast<size_t *>(newp);
    _meshPeriod = *newVal;
    // resetNextMeshCheck();
  } else if (strcmp(name, "mesh.scavenge") == 0) {
    lock.unlock();
    scavenge(true);
    lock.lock();
  } else if (strcmp(name, "mesh.compact") == 0) {
    meshAllSizeClasses();
    lock.unlock();
    scavenge(true);
    lock.lock();
  } else if (strcmp(name, "arena") == 0) {
    // not sure what this should do
  } else if (strcmp(name, "stats.resident") == 0) {
    auto pss = internal::measurePssKiB();
    // mesh::debug("measurePssKiB: %zu KiB", pss);

    *statp = pss * 1024;  // originally in KB
  } else if (strcmp(name, "stats.active") == 0) {
    // all miniheaps at least partially full
    size_t sz = 0;
    for (size_t i = 0; i < kNumBins; i++) {
      const auto count = _littleheaps[i].nonEmptyCount();
      if (count == 0)
        continue;
      sz += count * _littleheaps[i].objectSize() * _littleheaps[i].objectCount();
    }
    *statp = sz;
  } else if (strcmp(name, "stats.allocated") == 0) {
    // TODO: revisit this
    // same as active for us, for now -- memory not returned to the OS
    size_t sz = 0;
    for (size_t i = 0; i < kNumBins; i++) {
      const auto &bin = _littleheaps[i];
      const auto count = bin.nonEmptyCount();
      if (count == 0)
        continue;
      sz += bin.objectSize() * bin.allocatedObjectCount();
    }
    *statp = sz;
  }
  return 0;
}

void GlobalHeap::meshLocked(MiniHeap *dst, MiniHeap *&src) {
  const size_t dstSpanSize = dst->spanSize();
  const auto dstSpanStart = reinterpret_cast<void *>(dst->getSpanStart(arenaBegin()));

  src->forEachMeshed([&](const MiniHeap *mh) {
    // marks srcSpans read-only
    const auto srcSpan = reinterpret_cast<void *>(mh->getSpanStart(arenaBegin()));
    Super::beginMesh(dstSpanStart, srcSpan, dstSpanSize);
    return false;
  });

  // does the copying of objects and updating of span metadata
  dst->consume(arenaBegin(), src);
  d_assert(src->isMeshed());

  src->forEachMeshed([&](const MiniHeap *mh) {
    d_assert(mh->isMeshed());
    const auto srcSpan = reinterpret_cast<void *>(mh->getSpanStart(arenaBegin()));
    // frees physical memory + re-marks srcSpans as read/write
    Super::finalizeMesh(dstSpanStart, srcSpan, dstSpanSize);
    return false;
  });

  // make sure we adjust what bin the destination is in -- it might
  // now be full and not a candidate for meshing
  _littleheaps[dst->sizeClass()].postFree(dst, dst->inUseCount());
  untrackMiniheapLocked(src);
}

void GlobalHeap::meshAllSizeClasses() {
  Super::scavenge(false);

  if (!_lastMeshEffective.load(std::memory_order::memory_order_acquire)) {
    return;
  }

  if (Super::aboveMeshThreshold()) {
    return;
  }

  _lastMeshEffective = 1;

  // const auto start = time::now();
  size_t partialCount = 0;

  internal::vector<std::pair<MiniHeap *, MiniHeap *>> mergeSets;

  // first, clear out any free memory we might have
  for (size_t i = 0; i < kNumBins; i++) {
    flushBinLocked(i);
  }

  auto meshFound =
      function<void(std::pair<MiniHeap *, MiniHeap *> &&)>([&](std::pair<MiniHeap *, MiniHeap *> &&miniheaps) {
        if (std::get<0>(miniheaps)->isMeshingCandidate() && std::get<0>(miniheaps)->isMeshingCandidate())
          mergeSets.push_back(std::move(miniheaps));
      });

  for (size_t i = 0; i < kNumBins; i++) {
    partialCount += _littleheaps[i].partialSize();
    method::shiftedSplitting(_fastPrng, _littleheaps[i], meshFound);
  }

  // we consider this effective if more than ~ 1 MB saved
  _lastMeshEffective = mergeSets.size() > 256;

  if (mergeSets.size() == 0) {
    Super::scavenge(false);
    // debug("nothing to mesh.");
    return;
  }

  _stats.meshCount += mergeSets.size();

  for (auto &mergeSet : mergeSets) {
    // merge _into_ the one with a larger mesh count, potentially
    // swapping the order of the pair
    const auto aCount = std::get<0>(mergeSet)->meshCount();
    const auto bCount = std::get<1>(mergeSet)->meshCount();
    if (aCount + bCount > kMaxMeshes) {
      continue;
    } else if (aCount < bCount) {
      mergeSet = std::pair<MiniHeap *, MiniHeap *>(std::get<1>(mergeSet), std::get<0>(mergeSet));
    }

    meshLocked(std::get<0>(mergeSet), std::get<1>(mergeSet));
  }

  Super::scavenge(false);

  _lastMesh = time::now();

  // const std::chrono::duration<double> duration = _lastMesh - start;
  // debug("mesh took %f, found %zu", duration.count(), mergeSets.size());
}

void GlobalHeap::dumpStats(int level, bool beDetailed) const {
  if (level < 1)
    return;

  lock_guard<mutex> lock(_miniheapLock);

  const auto meshedPageHWM = meshedPageHighWaterMark();

  // debug("MESH COUNT:         %zu\n", (size_t)_stats.meshCount);
  // debug("Meshed MB (total):  %.1f\n", (size_t)_stats.meshCount * 4096.0 / 1024.0 / 1024.0);
  debug("Meshed pages HWM:   %zu\n", meshedPageHWM);
  debug("Meshed MB HWM:      %.1f\n", meshedPageHWM * 4096.0 / 1024.0 / 1024.0);
  // debug("Peak RSS reduction: %.2f\n", rssSavings);
  debug("MH Alloc Count:     %zu\n", (size_t)_stats.mhAllocCount);
  debug("MH Free  Count:     %zu\n", (size_t)_stats.mhFreeCount);
  debug("MH High Water Mark: %zu\n", (size_t)_stats.mhHighWaterMark);
  if (level > 1) {
    for (size_t i = 0; i < kNumBins; i++)
      _littleheaps[i].dumpStats(beDetailed);
  }
}
}  // namespace mesh

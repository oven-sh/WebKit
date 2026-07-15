# Container audit: freeing empty MarkedBlocks from the collector thread after the world resumes

Status: **the audit below is why the shipped design splits the free in half instead of making these
containers concurrent.** Read "Outcome" first; the rest is the evidence.

## Outcome

`~MarkedBlock::Handle` is one unsafe line and three safe ones:

```
removeBlock(this, WillDeleteBlock::Yes);   // ALL the container bookkeeping. Cheap: a bitscan + vector writes.
m_block->~MarkedBlock();                   // trivial
m_alignedMemoryAllocator->freeAlignedMemory(m_block);   // the madvise. THE COST. Thread-safe (see below).
heap.didFreeBlock(blockSize);              // a counter, RESOURCE_USAGE-only
```

Every hazard in this audit lives in `removeBlock`. None of it lives in `freeAlignedMemory`. So the
implementation does the bookkeeping half in the End phase, where the mutator is already quiesced and
the cost is a bit-scan, and defers the memory release to after `resumeThePeriphery()`, where the
expensive part runs concurrently with the mutator. By then the block is unlinked from every
directory, from `MarkedBlockSet`, and from every `IsoCellSet` — **nothing can reach it, so there is
no shared state left to race.** The containers below never need to become concurrent.

This also dodges the race that kills the "walk the directories after resume" design outright:
`m_requests.removeFirst()` (`Heap.cpp:1843`) runs *before* `resumeThePeriphery()` (`Heap.cpp:1893`),
so a queued collection can start the instant the mutator resumes — running `stopThePeriphery` →
`stopAllocating`, which repopulates every `m_lastActiveBlock` with a **not-inUse** block, while the
freer is walking `emptyBits & ~inUseBits`. No phase-ordering argument saves that; see the
`m_lastActiveBlock` section.

## Summary of the containers (why the above was necessary)

The concurrent design is not blocked by the allocator or by the block memory. It is blocked by four
bookkeeping containers whose current safety rests on a single sentence:

> `BlockDirectory::removeBlock()` asserts `assertIsMutatorOrMutatorIsStopped()`
> (`BlockDirectory.cpp:182`) — **either the world is stopped, or you are the API-lock-owning mutator
> thread.**

Every world-running reader of these containers satisfies that same predicate, so today *there is
only ever one thread*. The containers need no locks because they have never had a second writer.
A collector-thread writer after `resumeThePeriphery()` is that second writer, and it invalidates
the invariant for readers in four files at once, with **zero compile-time signal** — the assertion
is `ASSERT_ENABLED`-only (`BlockDirectory.cpp:554-565`), so release builds get no protection at all.

**The `IncrementalSweeper` precedent does not transfer.** It frees blocks with the world running
(`IncrementalSweeper.cpp:113-118`), which looks like the hard case already solved — but the sweeper
is a `JSRunLoopTimer` callback **on the mutator thread**. It is serialization by thread identity,
which is exactly what this change destroys.

## The containers

| # | Container | Owner | Guard today | Verdict |
|---|---|---|---|---|
| 1 | `m_blocks` (`MarkedBlockSet`: `HashSet` + `TinyBloomFilter`) | `MarkedSpace.h:225` | **none** | **Blocker** |
| 2 | `m_blocks` (`Vector<Handle*>`), `m_freeBlockIndices` | `BlockDirectory.h:172-173` | **none** (unannotated) | **Blocker** |
| 3 | `IsoCellSet::m_bits`, `m_blocksWithBits` | `IsoCellSet.h:89-90` | half-locked | **Blocker** |
| 4 | `MarkedSpace::m_capacity` | `MarkedSpace.h:217` | **none** | Blocker (lost update) |
| 5 | `Heap::m_blockBytesAllocated` | `Heap.h:991` | none | Minor (`RESOURCE_USAGE` only) |
| 6 | `AlignedMemoryAllocator::freeAlignedMemory` | 3 subclasses | thread-safe | **Safe** |

### 1. `MarkedBlockSet` — the widest blast radius

`MarkedSpace::freeBlock` does `m_blocks.remove(&block->block())` (`MarkedSpace.cpp:403`). `remove()`
can shrink the table and then `recomputeFilter()` walks the whole set (`MarkedBlockSet.h:57-63`).
Concurrent readers exist and are **unlocked**: conservative scanning (`ConservativeRoots.cpp:87,204`),
`HeapUtil.h:68`, `VMInspector.cpp:182`, and `SamplingProfiler.cpp:494`. A rehash under any of them is
a use-after-free on the backing table, not a stale read.

Two mitigating facts, both verified:

- **The sampling-profiler thread never touches this container.** `takeSample` carries raw `CalleeBits`
  and validates later *on the mutator thread* under `m_lock` (`SamplingProfiler.cpp:489-494`). Its
  own thread only consults `CodeBlockSet`, which *has* a lock. `CodeBlockSet` is therefore the
  in-tree precedent for exactly this problem.
- **Conservative scanning is world-stopped-only** (`ASSERT(m_heap.objectSpace().isMarking())`,
  `ConservativeRoots.cpp:89`). So it can stay lock-free, but only if we make this explicit:

> **Proposed invariant:** the collector free path never runs while the world is stopped, and
> conservative scanning never runs while the world is running. Disjoint by construction.

The filter itself is a non-issue: it only ever *gains* bits, so a stale read is a false positive the
subsequent `set.contains()` resolves.

### 2. `BlockDirectory::m_blocks` / `m_freeBlockIndices` — the asymmetry

```
Vector<MarkedBlock::Handle*> m_blocks;            // BlockDirectory.h:172 — NOT guarded
Vector<unsigned> m_freeBlockIndices;              // BlockDirectory.h:173 — NOT guarded
BlockDirectoryBits m_bits WTF_GUARDED_BY_LOCK(m_bitvectorLock);   // :177 — guarded
```

`addBlock` takes `Locker locker { m_bitvectorLock }` (`BlockDirectory.cpp:146`) and does **all** of
its work under it, including the `m_blocks.append()` that can **reallocate the buffer** and the
`m_bits.resize()`. `removeBlock` does **none** of its vector work under the lock:

```
187  subspace()->didRemoveBlock(block->index());     // unlocked
189  m_blocks[block->index()] = nullptr;             // unlocked
190  m_freeBlockIndices.append(block->index());      // unlocked  <- index published as free
193  Locker locker(bitvectorLock());                 // lock finally taken
194-197  clear every bit for the index
```

Three consequences:

- **Realloc race.** Mutator `addBlock` reallocs `m_blocks` while the collector writes `nullptr` into
  the freed old buffer.
- **Inverted publication order.** The index reaches the free list (`:190`) *before* its bits are
  cleared (`:194`). `addBlock` asserts the opposite (`:166-169`). A reader that takes the lock inside
  that window sees `canAllocate=true, inUse=false` and loads `m_blocks[i] == nullptr` — no reader
  null-checks a bitvector-derived index.
- **Word sharing.** All ten bit kinds for one index live in one `Segment`, one 32-bit word covers 32
  indices (`BlockDirectoryBits.h:73`). Any unlocked bit RMW corrupts 31 neighbours. Note
  `LocalAllocator.cpp:250` writes `setIsEden` with **no lock** and an admitting FIXME.

**Lock-ordering trap:** `removeBlock` calls `subspace()->didRemoveBlock()` at `:187`, and
`IsoCellSet::didRemoveBlock` acquires `m_bitvectorLock` *itself* (`IsoCellSet.cpp:108`). `WTF::Lock`
is not recursive, so naively wrapping `removeBlock`'s body in the lock **self-deadlocks**. This must
be resolved before the obvious fix is even available. (`releaseAssertAcquiredBitVectorLock()` at
`:192` is *not* an unlock — it is an empty static-analysis annotation, `BlockDirectory.h:103`.)

### 3. `IsoCellSet` — a genuinely lock-free hot reader

`IsoCellSet::add` (`IsoCellSetInlines.h:43-48`) loads `m_bits[blockIndex].get()` with **no lock** and
dereferences it. The bit-set itself is atomic (`concurrentTestAndSet`); the *pointer load* is not.
`IsoCellSet::didRemoveBlock` nulls that slot **outside** the lock (`IsoCellSet.cpp:111`), freeing the
`BitSet`. That is a UAF.

`add` really does run off the mutator: `GlobalExecutable::visitChildrenImpl` (`GlobalExecutable.cpp:51-52`)
and `FunctionExecutable::visitChildrenImpl` (`FunctionExecutable.cpp:106`) call it from **parallel
marker threads**.

Taking the lock in `add` is a hot-path regression. **Recommended instead: don't free the `BitSet` in
`didRemoveBlock` at all** — clear it and let the slot keep the allocation (indices are recycled via
`m_freeBlockIndices`). No reader ever dereferences freed memory, and the lock-free fast path
survives. Cost: one `BitSet<atomsPerBlock>` per retired index per set.

Only `IsoSubspace` overrides `didRemoveBlock`; `CompleteSubspace` inherits the empty base
(`Subspace.cpp:138-140`), so the majority of blocks are unaffected.

### 4. `MarkedSpace::m_capacity`

Plain `size_t` (`MarkedSpace.h:217`). `-=` in `freeBlock` (`:403`, would become collector) races `+=`
in `didAddBlock` (`:558`, mutator). Tearing is not the issue; the **lost update** is — the error is
cumulative and never healed, and `m_capacity` feeds GC sizing via `Heap::capacity()`. Make it
`std::atomic<size_t>`, relaxed.

### 6. The allocators are fine

All three `AlignedMemoryAllocator` subclasses (`FastMalloc`, `Gigacage`, `Structure` —
there is no `IsoAlignedMemoryAllocator` in this tree) are thread-safe and none assumes the mutator.
`StructureAlignedMemoryAllocator`'s system-heap fallback keeps its own bitmap but correctly guards it
with `m_lock`. Its `USE(MIMALLOC)` branch shares one process-wide `mi_heap_t`: a cross-thread
`mi_free` is legal (mimalloc routes it to the page's atomic `xthread_free` list), **but the block
does not return to the owning heap's free list immediately** — it is reclaimed at the owner's next
allocation. That defers reclaim relative to today's mutator-side free, which partially undercuts the
motivation and should be measured, not assumed.

## The `m_lastActiveBlock` claim — REFUTED

The plan states that `resumeAllocating()` only reinstates `m_lastActiveBlock`, "a block the allocator
claimed and still holds `inUse` on". **That premise is false.**

`LocalAllocator::stopAllocating` assigns `m_lastActiveBlock = m_currentBlock` (`LocalAllocator.cpp:83`)
*after* `MarkedBlock::Handle::stopAllocating` has already cleared the bit — its last statement is
`directory()->didFinishUsingBlock(this)` (`MarkedBlock.cpp:241`), i.e. `setIsInUse(handle, false)`.
The tree then asserts the whole `inUse` vector is empty right afterwards (`BlockDirectory.cpp:206-213`).
So `m_lastActiveBlock` is a raw pointer to a **not-inUse** block, fully eligible for any freer
selecting on `~inUseBits()`, and `resumeAllocating` dereferences it with no liveness check
(`LocalAllocator.cpp:94`).

**The conclusion survives, but for a different reason: phase ordering.** `prepareForAllocation()`
→ `LocalAllocator::reset()` nulls `m_lastActiveBlock` unconditionally (`LocalAllocator.cpp:47-53`),
and it runs at `Heap.cpp:1807`, immediately before the End-phase call site. That — not the claim
protocol — is what makes the scaffold safe, and it is why the scaffold **must** stay after
`prepareForAllocation()`. The comment at the call site now says so.

This matters for the concurrent design: once the world resumes, the next `stopTheWorld` repopulates
`m_lastActiveBlock` across every LocalAllocator with not-inUse blocks while a collector-thread freer
is walking `emptyBits() & ~inUseBits()`. **Fixing this structurally is a prerequisite**, not a
detail. The smaller of the two options: keep `inUse` set for `m_lastActiveBlock` (don't call
`didFinishUsingBlock` in `Handle::stopAllocating`; clear it in `resumeAllocating`/`reset` instead),
which also makes the bit mean what `BlockDirectoryBits.h:45` claims — "acts like a lock bit".

There is a second, narrower window of the same class: `Handle::resumeAllocating` can return early
without setting `inUse` (`MarkedBlock.cpp:275-281`) while `LocalAllocator::resumeAllocating`
unconditionally assigns `m_currentBlock` (`LocalAllocator.cpp:96`). Unreachable today only because
`endMarking` precedes `prepareForAllocation` inside `endPhase`. Nothing asserts it.

## What making the containers concurrent WOULD have cost (not done, and not needed)

Kept for the record, because if anyone later wants the full-concurrent walk, this is the bill:

1. Resolve the `didRemoveBlock` recursion (`WTF::Lock` is not recursive) — a `WTF_REQUIRES_LOCK`
   overload, deleting the inner `Locker`.
2. Annotate `m_blocks` / `m_freeBlockIndices` `WTF_GUARDED_BY_LOCK(m_bitvectorLock)` and let the
   compiler enumerate the fallout — that list *is* the work.
3. Reorder `removeBlock`: clear bits → null the slot → publish the index last.
4. Fix `m_lastActiveBlock` structurally (above).
5. Lock or defer `MarkedBlockSet::remove`; make `m_capacity` atomic.
6. Leave `IsoCellSet::add` lock-free by not freeing the `BitSet`.
7. Re-justify each of ~12 unlocked bit accessors whose only warrant is
   `assertSweeperIsSuspended()` / `assertIsMutatorOrMutatorIsStopped()` — both are claims about the
   *incremental sweeper* and the *mutator*, neither of which a collector-thread freer is.

Every race above is release-build-silent. TSan would be the gate, not assertions.

The split design pays none of this. Its only new invariant is "a deferred block is unreachable",
which is enforced by construction (it is unlinked before it is queued) and asserted at the drain
point (`ASSERT(m_deferredDestroyBlocks.isEmpty() || !heap().worldIsStopped())`).

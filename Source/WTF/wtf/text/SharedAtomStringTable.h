/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <atomic>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/AtomStringTable.h>

namespace WTF {

// Process-global sharded atom-string table (SPEC-vmstate §4).
//
// When enabled (sharedAtomStringTableEnabled()), every atomization, lookup,
// and removal path routes to one of the shards below instead of the
// per-WTF::Thread AtomStringTable (rule A1, SPEC-vmstate §4.3). Atoms remain
// pointer-comparable: hot property access never touches this table.
//
// Ordering contract (SPEC-vmstate §4.8 / §8, normative): the latch is flipped
// by WTF::enableSharedAtomStringTable(), called exactly once from
// JSC::initialize after Options::finalize. NO other thread — embedder thread
// or WTF/JSC-internal service thread alike — may atomize ANY string before
// JSC::initialize returns. "Before JSC::initialize returns" is a
// happens-before requirement, not wall clock: a thread that already existed
// when the latch flipped must synchronize-with the initializing thread
// (acquire on something the initializer released after JSC::initialize, e.g.
// the channel through which the embedder told it JSC is up) before its next
// atomize/lookUp of any string it did not already own pre-latch, AND before
// its next ref() or deref() of ANY WTF::String whatsoever — including strings
// it already owned pre-latch. The pre-latch-owned exemption applies to
// atomize/lookUp only, NEVER to ref/deref: AtomStringImpl::addSlowCase
// atomizes a caller-owned StringImpl IN PLACE, so a string a pre-latch thread
// co-owns can become a shard-resident atom after the latch without that
// thread doing anything; if that thread then performed the final deref with a
// stale latch read of 'false', the legacy relaxed path would destroy the
// string without removeDeadAtom, leaving a dangling shard entry (UAF).
// Ordinary cross-thread handoff channels (queues, locks, Thread creation)
// provide this edge for free; it is what makes the relaxed latch loads in
// sharedAtomStringTableEnabled() and StringImpl::deref() sound (F4 — see the
// coherence argument at sharedAtomStringTableEnabled() below).
// Pre-latch atoms of the initializing thread are migrated into the shards
// BEFORE the latch is published (enableSharedAtomStringTable() step (1):
// migrate-then-latch, so a latch observer can never shard-miss a live atom);
// atoms created on any other thread before the latch are a contract
// violation, detected fail-stop at that thread's death (invariant I17: in
// shared mode the AtomStringTable destructor RELEASE_ASSERTs emptiness).
//
// Lock ranking (SPEC-vmstate §7): shard locks are leaves. Nothing may be
// acquired while one is held (no JS-heap allocation, no GC, no JSC lock —
// invariant I7; the table only fastMallocs). Shard locks never nest with each
// other: shard choice is a pure function of the string hash (invariant I5),
// so equal strings always contend on exactly one lock.
class SharedAtomStringTable {
    WTF_MAKE_NONCOPYABLE(SharedAtomStringTable);
public:
    SharedAtomStringTable() = default;

    static constexpr unsigned shardCountLog2 = 7;     // 128 shards
    static constexpr unsigned shardCount = 1u << shardCountLog2;

    struct alignas(64) Shard {
        Lock lock;
        AtomStringTable::StringTableImpl table WTF_GUARDED_BY_LOCK(lock);

    private:
        // Pad so sizeof(Shard) >= 128: adjacent shards never share a cache
        // line, including on parts that prefetch 128-byte line pairs.
        // (Lower bound on payload; interior padding only grows the struct.)
        static constexpr size_t minimumPayloadBytes = sizeof(Lock) + sizeof(AtomStringTable::StringTableImpl);
        char m_padding[minimumPayloadBytes >= 128 ? 1 : 128 - minimumPayloadBytes] { };
    };
    static_assert(sizeof(Shard) >= 128, "shards must not share cache lines");
    static_assert(alignof(Shard) >= 64);

    // NeverDestroyed. DEFINED IN AtomStringTable.cpp, NOT in
    // SharedAtomStringTable.cpp, on purpose (link-soundness):
    // AtomStringImpl.cpp — an always-compiled WTF TU — calls this, but
    // SharedAtomStringTable.cpp only enters the build once the M1 build-file
    // hunk (INTEGRATE-vmstate.md) is applied. Defining it next to the latch
    // in AtomStringTable.cpp keeps the bare tree linking AND guarantees one
    // table instance per process even in DLL configurations (an inline
    // local-static could duplicate across DSO boundaries). The thread-safe
    // static init is forced before the latch is published
    // (enableSharedAtomStringTable()), so no post-latch reader races the
    // slow-path guard.
    WTF_EXPORT_PRIVATE static SharedAtomStringTable& singleton();

    // Shard selection MUST be this function — applied to the HashTranslator's
    // hash — from every entry path: add/lookUp/remove/addStatic/addSymbol,
    // removeDeadAtom, and §4.8 migration (invariant I5).
    ALWAYS_INLINE Shard& shardForHash(unsigned hash)
    {
        // StringHasher produces a 24-bit hash (top 8 bits masked off). The
        // HIGH bits of those 24 pick the shard; the per-shard HashTable
        // consumes the LOW bits for bucket selection (HashTable.h:676,732,772),
        // so reusing low bits here would make every key in a shard collide on
        // its initial probe. See SPEC-vmstate §4.2 / history R4.
        return m_shards[(hash >> (24 - shardCountLog2)) & (shardCount - 1)];
    }

    Shard m_shards[shardCount];
};

// Internal latch storage. Set exactly once by enableSharedAtomStringTable();
// immutable after. Defined in AtomStringTable.cpp so the AtomStringTable
// destructor (invariant I17) reads it from its own translation unit. Readers
// elsewhere should use sharedAtomStringTableEnabled().
WTF_EXPORT_PRIVATE extern std::atomic<bool> g_sharedAtomStringTableEnabled;

// Latch + migrate (§4.8); idempotent; init only. Out-of-line in
// SharedAtomStringTable.cpp — reachable only via JSC::initialize (the M3 hunk
// in INTEGRATE-vmstate.md) and the M14 unit tests, both of which arrive in
// the same manifest batch as the M1 build-file hunk that compiles that TU.
WTF_EXPORT_PRIVATE void enableSharedAtomStringTable();

// Header-inline ON PURPOSE (link-soundness, like singleton()'s placement in
// AtomStringTable.cpp: always-compiled TUs call this, and the defining TU for
// out-of-line code here would be SharedAtomStringTable.cpp, which is not in
// the build until M1 lands. A relaxed load of an exported atomic has no
// duplication hazard, so inline is safe AND keeps the query branch-cheap).
//
// F4 (SPEC-vmstate §4.6): relaxed is sound. The argument has two halves:
//
// (a) Threads created after the latch: thread creation synchronizes-with the
//     created thread, so the latch store happens-before everything the new
//     thread does, and write-read coherence ([intro.races]/18, which is keyed
//     on happens-before, not on the load's memory order) forbids even a
//     relaxed load from reading the stale 'false'.
//
// (b) Threads that existed BEFORE the latch (embedder / WTF service threads):
//     the §4.8 contract above requires such a thread to observe
//     initialization-complete through a release/acquire channel before its
//     next ref/deref of ANY WTF::String (no pre-latch-owned exemption for
//     ref/deref — addSlowCase atomizes co-owned strings in place, see the
//     contract comment) and before atomizing/looking up anything it did not
//     already own. Once that edge exists, the latch store happens-before this
//     load and coherence forces it to read 'true'. There is no contract-legal
//     execution in which a thread refs, derefs, atomizes, or holds a shard
//     atom yet reads a stale 'false' here.
inline bool sharedAtomStringTableEnabled()
{
    return g_sharedAtomStringTableEnabled.load(std::memory_order_relaxed);
}

} // namespace WTF

using WTF::SharedAtomStringTable;

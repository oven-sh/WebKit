/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <wtf/Locker.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/SharedAtomStringTable.h>
#include <wtf/text/WTFString.h>

namespace TestWebKitAPI {

// ---- I5: shard selection is a pure function of the hash (SPEC-vmstate §4.7).

TEST(WTF_SharedAtomStringTable, ShardSelectionPureFunction)
{
    auto& table = SharedAtomStringTable::singleton();
    static_assert(SharedAtomStringTable::shardCount == 128);
    static_assert(SharedAtomStringTable::shardCountLog2 == 7);

    // Same hash => same shard, and the shard is exactly
    // m_shards[(hash >> 17) & 127] (HIGH 7 of the 24-bit hash; §4.2).
    for (unsigned hash : { 0u, 1u, 0x1FFFFu, 0x20000u, 0xFFFFFFu, 0xABCDEFu, 0x800000u }) {
        auto& shard = table.shardForHash(hash);
        ASSERT_EQ(&shard, &table.shardForHash(hash));
        unsigned expectedIndex = (hash >> (24 - SharedAtomStringTable::shardCountLog2))
            & (SharedAtomStringTable::shardCount - 1);
        ASSERT_EQ(&shard, &table.m_shards[expectedIndex]);
    }
}

TEST(WTF_SharedAtomStringTable, ShardSelectionUsesHighBits)
{
    auto& table = SharedAtomStringTable::singleton();

    // Hashes differing only in the LOW 17 bits land on the SAME shard
    // (the per-shard HashTable consumes the low bits for buckets)...
    ASSERT_EQ(&table.shardForHash(0x000000u), &table.shardForHash(0x01FFFFu));
    ASSERT_EQ(&table.shardForHash(0x540000u), &table.shardForHash(0x55ABCDu));
    // ...while flipping bit 17 (lowest shard-selecting bit) changes the shard.
    ASSERT_NE(&table.shardForHash(0x000000u), &table.shardForHash(0x020000u));
    // Bits above the 24-bit hash domain are ignored by the mask.
    ASSERT_EQ(&table.shardForHash(0x00ABCDEFu), &table.shardForHash(0xFFABCDEFu));
}

TEST(WTF_SharedAtomStringTable, EqualStringsSameShard)
{
    auto& table = SharedAtomStringTable::singleton();

    // Two independently-built equal strings hash equally, hence always
    // contend on one lock (I5).
    String a = makeString("shard"_s, "-selection-probe"_s);
    String b = makeString("shard-selection"_s, "-probe"_s);
    ASSERT_NE(a.impl(), b.impl());
    ASSERT_EQ(a.impl()->hash(), b.impl()->hash());
    ASSERT_EQ(&table.shardForHash(a.impl()->hash()), &table.shardForHash(b.impl()->hash()));
}

TEST(WTF_SharedAtomStringTable, ShardLayoutNoFalseSharing)
{
    static_assert(sizeof(SharedAtomStringTable::Shard) >= 128);
    static_assert(alignof(SharedAtomStringTable::Shard) >= 64);
    auto& table = SharedAtomStringTable::singleton();
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&table.m_shards[0]) % 64, static_cast<uintptr_t>(0));
}

// ---- §4.8 migration + I17. Latching is process-global and irreversible, so
// this runs in a death-test child process (fork on POSIX; spawned re-exec on
// Windows). Everything inside the lambda executes in the child only.

TEST(WTF_SharedAtomStringTable, MigrationOnLatch)
{
    EXPECT_EXIT({
        // Pre-latch: atomize on the initializing thread.
        AtomString probe("WTFSharedAtomStringTableMigrationProbe"_s);
        StringImpl* probeImpl = probe.impl();
        RELEASE_ASSERT(probeImpl);
        RELEASE_ASSERT(probeImpl->isAtom());
        unsigned probeHash = probeImpl->existingHash();

        auto* threadTable = Thread::currentSingleton().atomStringTable();
        RELEASE_ASSERT(!threadTable->table().isEmpty());
        size_t preLatchCount = threadTable->table().size();

        RELEASE_ASSERT(!WTF::sharedAtomStringTableEnabled());
        WTF::enableSharedAtomStringTable();
        RELEASE_ASSERT(WTF::sharedAtomStringTableEnabled());

        // (2) Source set cleared, isAtom NOT cleared.
        RELEASE_ASSERT(threadTable->table().isEmpty());
        RELEASE_ASSERT(probeImpl->isAtom());

        // (1) Every pre-latch atom is now in shardForHash(existingHash()),
        // found by pointer identity; total migrated count matches.
        auto& shared = SharedAtomStringTable::singleton();
        {
            auto& shard = shared.shardForHash(probeHash);
            Locker locker { shard.lock };
            bool found = false;
            for (const auto& entry : shard.table) {
                if (entry.get() == probeImpl)
                    found = true;
            }
            RELEASE_ASSERT(found);
        }
        size_t migratedCount = 0;
        for (unsigned i = 0; i < SharedAtomStringTable::shardCount; ++i) {
            auto& shard = shared.m_shards[i];
            Locker locker { shard.lock };
            migratedCount += shard.table.size();
            // I5/§4.8: every migrated entry sits in its own hash's shard.
            for (const auto& entry : shard.table)
                RELEASE_ASSERT(&shared.shardForHash(entry.get()->existingHash()) == &shard);
        }
        RELEASE_ASSERT(migratedCount == preLatchCount);

        // Idempotent: second call is a no-op (would otherwise re-migrate an
        // empty table; must not assert or duplicate).
        WTF::enableSharedAtomStringTable();
        RELEASE_ASSERT(threadTable->table().isEmpty());

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// ---- Task 4 (W1 lifecycle, §4.4): no-resurrection destroy protocol,
// removeDeadAtom (§4.4.5), dead-entry stress (I1-I3, I6), static atoms (I19),
// I17 thread-death emptiness. All shared-mode: each test latches inside its
// own EXPECT_EXIT child (the parent process is never latched). Failures are
// RELEASE_ASSERTs in the child — a tripped assert crashes the child and fails
// ExitedWithCode(0). Under TSAN these are the W1 data-race gate; under ASAN
// the churn is a UAF probe for revival-at-0 / double-destroy / dangling shard
// entries.

// Simple reusable spin barrier; no WTF lock is taken so the synchronization
// under test is not masked.
class SpinBarrier {
    WTF_MAKE_NONCOPYABLE(SpinBarrier);
public:
    explicit SpinBarrier(unsigned total)
        : m_total(total)
    {
    }

    void arriveAndWait()
    {
        unsigned generation = m_generation.load(std::memory_order_acquire);
        if (m_count.fetch_add(1, std::memory_order_acq_rel) + 1 == m_total) {
            m_count.store(0, std::memory_order_relaxed);
            m_generation.fetch_add(1, std::memory_order_release);
        } else {
            while (m_generation.load(std::memory_order_acquire) == generation)
                Thread::yield();
        }
    }

private:
    const unsigned m_total;
    std::atomic<unsigned> m_count { 0 };
    std::atomic<unsigned> m_generation { 0 };
};

static std::span<const char> formatKey(char* buffer, size_t capacity, const char* prefix, unsigned index)
{
    int length = snprintf(buffer, capacity, "%s-%u", prefix, index);
    return std::span<const char> { buffer, static_cast<size_t>(length) };
}

// Deterministic single-threaded lifecycle (§4.4.3/§4.4.5): the final deref of
// an atom must remove its shard entry (via removeDeadAtom) and destroy it
// without the destructor touching the table; afterwards lookUp misses and a
// fresh add yields a live atom again.
TEST(WTF_SharedAtomStringTable, DeadAtomRemovedOnFinalDeref)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();
        static constexpr auto key = "sharedAtomLifecycleSingleThread"_span;

        {
            RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
            RELEASE_ASSERT(atom);
            RELEASE_ASSERT(atom->isAtom());
            RELEASE_ASSERT(!atom->isStatic());
            RELEASE_ASSERT(atom->refCount() >= 1);

            // While live, lookUp returns the same pointer (I1/I2).
            RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>(key));
            RELEASE_ASSERT(found.get() == atom.get());
            // The RefPtrs going out of scope run the §4.4.3 zero transition;
            // removeDeadAtom unhooks the shard entry and destroys the string.
        }

        // Entry removed: a lookup is a clean miss, never a dead hit (I3).
        RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));

        // Re-atomization works and produces a live atom.
        {
            RefPtr<AtomStringImpl> again = AtomStringImpl::add(key);
            RELEASE_ASSERT(again);
            RELEASE_ASSERT(again->isAtom());
            RELEASE_ASSERT(again->refCount() >= 1);
        }
        RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// I1: while multiple threads simultaneously hold references obtained by
// atomizing the same character sequence, they all hold the SAME pointer, and
// the refcount reflects every holder.
TEST(WTF_SharedAtomStringTable, LiveUniqueness)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 8;
        constexpr unsigned rounds = 256;

        SpinBarrier barrier(threadCount);
        Vector<RefPtr<AtomStringImpl>> results(threadCount);

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: live uniqueness"_s, [&, t] {
                char buffer[64];
                for (unsigned round = 0; round < rounds; ++round) {
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomLiveUnique", round);
                    results[t] = AtomStringImpl::add(key);
                    RELEASE_ASSERT(results[t]);
                    RELEASE_ASSERT(results[t]->isAtom());

                    barrier.arriveAndWait();

                    if (!t) {
                        // All threads hold a reference right now: exactly one
                        // live AtomStringImpl per character sequence (I1).
                        for (unsigned other = 1; other < threadCount; ++other)
                            RELEASE_ASSERT(results[other].get() == results[0].get());
                        RELEASE_ASSERT(results[0]->refCount() >= threadCount);
                    }

                    barrier.arriveAndWait();

                    // Everyone drops; the last deref of the round routes
                    // through removeDeadAtom (§4.4.5).
                    results[t] = nullptr;

                    barrier.arriveAndWait();
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// Dead-entry stress (I1/I3/I6): tight add/drop churn over a small key set
// from many threads, racing concurrent lookUps. Drives every §4.4 arm:
// tryRefAtom failure on a dying entry, dead-entry remove+reinsert in add,
// lookUp treating a dead hit as a miss, and removeDeadAtom skipping an entry
// a racing add already replaced (pointer mismatch).
TEST(WTF_SharedAtomStringTable, DeadEntryChurn)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned adderCount = 6;
        constexpr unsigned lookupCount = 2;
        constexpr unsigned keyCount = 4; // Few keys => maximal add/deref collisions.
        constexpr unsigned iterations = 20000;

        std::atomic<bool> done { false };

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < adderCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: churn adder"_s, [&, t] {
                char buffer[64];
                for (unsigned i = 0; i < iterations; ++i) {
                    unsigned keyIndex = (i + t) % keyCount;
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomChurn", keyIndex);
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
                    RELEASE_ASSERT(atom);
                    RELEASE_ASSERT(atom->isAtom());
                    RELEASE_ASSERT(atom->refCount());
                    RELEASE_ASSERT(WTF::equal(atom.get(), byteCast<Latin1Character>(key)));
                    // Drop immediately: with no other holder this is the zero
                    // transition -> removeDeadAtom (§4.4.5).
                    atom = nullptr;
                }
                done.store(true, std::memory_order_release);
            }));
        }
        for (unsigned t = 0; t < lookupCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: churn lookup"_s, [&, t] {
                char buffer[64];
                unsigned i = 0;
                while (!done.load(std::memory_order_acquire)) {
                    unsigned keyIndex = (i + t) % keyCount;
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomChurn", keyIndex);
                    RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>(key));
                    // A miss is fine (the atom may be dead or absent); a hit
                    // MUST be a live, correct atom — never a refcount-0
                    // corpse (I3).
                    if (found) {
                        RELEASE_ASSERT(found->refCount());
                        RELEASE_ASSERT(found->isAtom());
                        RELEASE_ASSERT(WTF::equal(found.get(), byteCast<Latin1Character>(key)));
                    }
                    found = nullptr;
                    ++i;
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        // Drain: all references dropped => every churn atom is dead and
        // unhooked; lookups miss cleanly (I2/I6 — a leaked shard entry or a
        // double removal would have crashed above or shows up as a dead hit).
        char buffer[64];
        for (unsigned keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
            auto key = formatKey(buffer, sizeof(buffer), "sharedAtomChurn", keyIndex);
            RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));
        }

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// Focused I6 amplifier: all threads churn ONE key with zero holders between
// iterations, maximizing the window where a refcount-0 entry is still table
// resident. Exercises tryRefAtom failure -> locked remove + fresh insert
// (§4.4.4) racing removeDeadAtom's pointer-identity removal (§4.4.5).
TEST(WTF_SharedAtomStringTable, SingleKeyDeathRace)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 8;
        constexpr unsigned iterations = 30000;
        constexpr auto key = "sharedAtomSingleKeyDeathRace"_span;

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: death race"_s, [&] {
                for (unsigned i = 0; i < iterations; ++i) {
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
                    RELEASE_ASSERT(atom);
                    RELEASE_ASSERT(atom->refCount());
                    RELEASE_ASSERT(WTF::equal(atom.get(), byteCast<Latin1Character>(key)));
                    atom = nullptr;
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// I19: a table-resident StaticStringImpl is returned BY POINTER to every
// thread, always survives tryRefAtom (statics rest at masked refcount 0 and
// never die), and is never evicted by the dead-entry replace arm or by
// removeDeadAtom — even under heavy same-table churn.
static StringImpl::StaticStringImpl s_sharedAtomStaticSurvivor { "sharedAtomStaticSurvivor", StringImpl::StringAtom };

TEST(WTF_SharedAtomStringTable, StaticAtomSurvives)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        StringImpl& staticImpl = s_sharedAtomStaticSurvivor;
        RELEASE_ASSERT(staticImpl.isStatic());
        RELEASE_ASSERT(staticImpl.isAtom());

        // Register the static in its shard; the returned atom IS the static.
        RefPtr<AtomStringImpl> registered = AtomStringImpl::add(s_sharedAtomStaticSurvivor);
        RELEASE_ASSERT(registered);
        RELEASE_ASSERT(static_cast<StringImpl*>(registered.get()) == &staticImpl);

        constexpr unsigned threadCount = 8;
        constexpr unsigned iterations = 10000;
        constexpr auto staticKey = "sharedAtomStaticSurvivor"_span;

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: static atom"_s, [&, t] {
                char buffer[64];
                for (unsigned i = 0; i < iterations; ++i) {
                    // Atomizing the static's characters from any thread
                    // returns the SAME pointer (I19) — via tryRefAtom's
                    // static fast path.
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(staticKey);
                    RELEASE_ASSERT(static_cast<StringImpl*>(atom.get()) == &staticImpl);

                    RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>(staticKey));
                    RELEASE_ASSERT(static_cast<StringImpl*>(found.get()) == &staticImpl);

                    // Churn dynamic atoms alongside so dead-entry removal and
                    // replacement runs hot while the static stays resident
                    // (the replace arm asserts !isStatic; removeDeadAtom
                    // never sees a static — statics never hit refcount 0).
                    unsigned keyIndex = (i + t) % 3;
                    auto churnKey = formatKey(buffer, sizeof(buffer), "sharedAtomStaticChurn", keyIndex);
                    RefPtr<AtomStringImpl> churn = AtomStringImpl::add(churnKey);
                    RELEASE_ASSERT(churn);
                    RELEASE_ASSERT(churn->refCount());
                    churn = nullptr;
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        // Still resident, still the same pointer.
        RefPtr<AtomStringImpl> after = AtomStringImpl::lookUp(byteCast<Latin1Character>(staticKey));
        RELEASE_ASSERT(static_cast<StringImpl*>(after.get()) == &staticImpl);

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// I17 positive coverage: post-latch, rule A1 keeps every per-thread
// AtomStringTable empty, so a raw WTF::Thread that atomizes heavily dies
// cleanly — its ~AtomStringTable RELEASE_ASSERT(m_table.isEmpty()) must NOT
// trip (it would crash this child and fail ExitedWithCode(0)).
TEST(WTF_SharedAtomStringTable, PerThreadTablesStayEmpty)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 4;
        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: I17"_s, [t] {
                char buffer[64];
                for (unsigned i = 0; i < 512; ++i) {
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomI17Probe", (i + t * 512));
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
                    RELEASE_ASSERT(atom);
                    RELEASE_ASSERT(atom->isAtom());
                }
                // A1: nothing above touched this thread's table.
                RELEASE_ASSERT(Thread::currentSingleton().atomStringTable()->table().isEmpty());
                // ~AtomStringTable runs at thread death and re-checks (I17).
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// ---- Task 9 additions (W1 coverage closure) ----

// §4.5: m_hashAndFlags is atomic with idempotent RMW flag writes. Racing
// lazy hashers against concurrent atomization of the SAME StringImpl must
// never drop a flag bit (a plain RMW could erase isAtom or the lazily
// stored hash) and the stored hash must equal the computed hash.
TEST(WTF_SharedAtomStringTable, ConcurrentLazyHashingAndAtomFlags)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 8;
        constexpr unsigned stringCount = 1024;
        constexpr unsigned rounds = 4;

        for (unsigned round = 0; round < rounds; ++round) {
            // Fresh, unhashed, not-yet-atomized strings.
            Vector<RefPtr<StringImpl>> strings;
            {
                char buffer[64];
                for (unsigned i = 0; i < stringCount; ++i) {
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomLazyHash", round * stringCount + i);
                    strings.append(StringImpl::create(byteCast<Latin1Character>(key)));
                    RELEASE_ASSERT(!strings.last()->isAtom());
                }
            }

            SpinBarrier barrier(threadCount);
            Vector<Ref<Thread>> threads;
            for (unsigned t = 0; t < threadCount; ++t) {
                threads.append(Thread::create("SharedAtomStringTable: lazy hash"_s, [&, t] {
                    barrier.arriveAndWait();
                    for (unsigned i = 0; i < stringCount; ++i) {
                        StringImpl* impl = strings[i].get();
                        if (t & 1) {
                            // Lazy hasher: fetch_or of the hash bits (§4.5).
                            unsigned hash = impl->hash();
                            RELEASE_ASSERT(hash == impl->existingHash());
                        } else {
                            // Atomizer: setIsAtom(true) is published under
                            // the shard lock (F1) and races the hashers'
                            // fetch_or on the same word.
                            RefPtr<AtomStringImpl> atom = AtomStringImpl::add(impl);
                            RELEASE_ASSERT(atom);
                            RELEASE_ASSERT(atom->isAtom());
                        }
                    }
                }));
            }
            for (auto& thread : threads)
                thread->waitForCompletion();

            // No dropped bits: every string is an atom AND carries exactly
            // the hash an identical fresh string computes.
            char buffer[64];
            for (unsigned i = 0; i < stringCount; ++i) {
                StringImpl* impl = strings[i].get();
                RELEASE_ASSERT(impl->isAtom());
                auto key = formatKey(buffer, sizeof(buffer), "sharedAtomLazyHash", round * stringCount + i);
                Ref<StringImpl> fresh = StringImpl::create(byteCast<Latin1Character>(key));
                RELEASE_ASSERT(impl->existingHash() == fresh->hash());
            }
            // Dropping the vector derefs each atom to zero ->
            // removeDeadAtom unhooks every shard entry (§4.4.5).
        }

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// ---- I4 face of R3 (legacy-mode sanity). Runs in the PARENT process, which
// is NEVER latched: with the latch off, atomization routes to the per-thread
// table exactly as today and the dormant shared table stays empty of it.
TEST(WTF_SharedAtomStringTable, LegacyModeUnaffected)
{
    RELEASE_ASSERT(!WTF::sharedAtomStringTableEnabled());

    AtomString atom("sharedAtomLegacyModeProbe"_s);
    StringImpl* impl = atom.impl();
    RELEASE_ASSERT(impl);
    RELEASE_ASSERT(impl->isAtom());

    // Legacy routing: the atom lives in THIS thread's per-thread table...
    bool inThreadTable = false;
    for (const auto& entry : Thread::currentSingleton().atomStringTable()->table()) {
        if (entry.get() == impl)
            inThreadTable = true;
    }
    ASSERT_TRUE(inThreadTable);

    // ...and in NO shard of the (dormant) shared table.
    auto& shared = SharedAtomStringTable::singleton();
    for (unsigned i = 0; i < SharedAtomStringTable::shardCount; ++i) {
        auto& shard = shared.m_shards[i];
        Locker locker { shard.lock };
        for (const auto& entry : shard.table)
            ASSERT_NE(static_cast<StringImpl*>(entry.get()), impl);
    }

    // Legacy lookUp still hits by pointer.
    RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>("sharedAtomLegacyModeProbe"_span));
    ASSERT_EQ(static_cast<StringImpl*>(found.get()), impl);
}

#if USE(BUN_JSC_ADDITIONS)
// ---- Round 4: setNeverAtomize() vs in-place atomization is total-ordered
// (StringImpl::trySetIsAtomIfAtomizable on the same atomic word). Invariant
// under the race: a string whose setNeverAtomize() returned true is NEVER
// shard-resident and never isAtom; if it returned false the string IS an
// atom (the atomization won). Either way every add() call still yields a
// usable atom with equal characters (copying fallback).
TEST(WTF_SharedAtomStringTable, NeverAtomizeVsInPlaceAtomizationRace)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();
        constexpr unsigned stringCount = 512;

        for (unsigned i = 0; i < stringCount; ++i) {
            char buffer[64];
            auto key = formatKey(buffer, sizeof(buffer), "sharedAtomNeverAtomize", i);
            Ref<StringImpl> victim = StringImpl::create(byteCast<Latin1Character>(key));
            StringImpl* impl = victim.ptr();

            std::atomic<int> flagWon { -1 };
            SpinBarrier barrier(2);
            RefPtr<AtomStringImpl> atom;
            auto atomizer = Thread::create("SharedAtomStringTable: atomizer"_s, [&] {
                barrier.arriveAndWait();
                atom = AtomStringImpl::add(impl);
            });
            auto marker = Thread::create("SharedAtomStringTable: marker"_s, [&] {
                barrier.arriveAndWait();
                flagWon.store(impl->setNeverAtomize() ? 1 : 0, std::memory_order_seq_cst);
            });
            atomizer->waitForCompletion();
            marker->waitForCompletion();

            RELEASE_ASSERT(atom);
            RELEASE_ASSERT(atom->isAtom());
            RELEASE_ASSERT(equal(*atom, *impl));
            if (flagWon.load() == 1) {
                // setNeverAtomize won: the victim must NOT be parked — the
                // atomizer fell back to a copying atom.
                RELEASE_ASSERT(!impl->isAtom());
                RELEASE_ASSERT(static_cast<StringImpl*>(atom.get()) != impl);
                RELEASE_ASSERT(!impl->canBecomeAtom());
                auto& shared = SharedAtomStringTable::singleton();
                auto& shard = shared.shardForHash(impl->hash());
                {
                    Locker locker { shard.lock };
                    for (const auto& entry : shard.table)
                        RELEASE_ASSERT(static_cast<StringImpl*>(entry.get()) != impl);
                }
                // Refuses forever after, even once an equal copy-atom exists.
                RELEASE_ASSERT(!impl->trySetIsAtomIfAtomizable());
            } else {
                // Atomization won: in-place atom; flag refused (returned 0).
                RELEASE_ASSERT(flagWon.load() == 0);
                RELEASE_ASSERT(impl->isAtom());
                RELEASE_ASSERT(impl->canBecomeAtom());
                RELEASE_ASSERT(static_cast<StringImpl*>(atom.get()) == impl);
            }
        }
        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}
#endif // USE(BUN_JSC_ADDITIONS)

} // namespace TestWebKitAPI

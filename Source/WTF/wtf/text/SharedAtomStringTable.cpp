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

#include "config.h"
#include <wtf/text/SharedAtomStringTable.h>

#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <wtf/text/StringImpl.h>

namespace WTF {

// Set exactly once by enableSharedAtomStringTable(); immutable after.
std::atomic<bool> g_sharedAtomStringTableEnabled { false };

// Out of line (not an inline local static) so there is exactly one table per
// process even when WTF is a DLL.
SharedAtomStringTable& SharedAtomStringTable::singleton()
{
    static NeverDestroyed<SharedAtomStringTable> table;
    return table;
}

void enableSharedAtomStringTable()
{
    // Idempotent; init only. Caller contract: runs on the JSC::initialize
    // thread, after Options::finalize, before any other thread atomizes
    // anything (embedder and internal service threads alike; GC/JIT/sampler
    // threads do not atomize). A breach is not detectable here: a pre-latch
    // atomizer on another thread leaves a non-empty per-thread table and
    // fail-stops at that thread's death (~AtomStringTable); a pre-latch
    // thread that derefs with a stale latch read is caught only by the
    // debug ASSERT in StringImpl::deref's legacy zero-transition arm.
    if (g_sharedAtomStringTableEnabled.load(std::memory_order_relaxed))
        return;

    // Force construction of the singleton before publishing the latch so no
    // post-latch reader races the (thread-safe, but slow-path) static init.
    auto& shared = SharedAtomStringTable::singleton();

    // Order: migrate, then latch, then clear the source table. Migration must
    // precede the latch: once the latch is visible, the final deref of a
    // not-yet-migrated atom takes derefSharedZero -> removeDeadAtom, misses
    // its shard and destroys the string while the per-thread table still
    // holds a raw entry to it, which this loop would then read. Migrating
    // while unlatched is unobservable: legacy paths never consult the shards
    // and this is the only thread that has atomized so far.
    //
    // Migrate every pre-latch atom of the initializing thread into its shard.
    // Without this, pre-latch atoms would be invisible to the shards (so
    // equal strings could atomize to two different pointers) and would
    // shard-miss at death. Entries are moved as-is: the shard table holds the
    // same non-owning StringEntry the per-thread table held; refcounts and
    // the isAtom bit are untouched. One shard lock at a time; shard locks are
    // leaves and never nest. The per-shard lock release also publishes each
    // migrated entry to any post-latch reader that acquires the same shard
    // lock.
    AtomStringTable* threadTable = Thread::currentSingleton().atomStringTable();
    auto& sourceTable = threadTable->table();
    for (const auto& entry : sourceTable) {
        StringImpl* string = entry.get();
        ASSERT(string);
        ASSERT(string->hasHash()); // Was inserted into a string-keyed table.
        auto& shard = shared.shardForHash(string->existingHash()); // Every translator's hash for a resident atom equals its stored hash.
        Locker locker { shard.lock };
        auto addResult = shard.table.add(entry);
        // Single-threaded init + empty shards: every migrated atom is new.
        ASSERT_UNUSED(addResult, addResult.isNewEntry);
    }

    // Publish the latch. Every shard already contains every live atom, so a
    // reader that observes the latch can never shard-miss a pre-latch atom.
    // Release pairs with the synchronization that delivers post-latch atoms
    // (and the fact of initialization) to other threads — see the comment at
    // sharedAtomStringTableEnabled() in SharedAtomStringTable.h.
    g_sharedAtomStringTableEnabled.store(true, std::memory_order_release);

    // Clear the source set; do NOT clear isAtom. From here on every per-thread
    // table stays empty (~AtomStringTable asserts it). The brief window where
    // entries exist in BOTH tables is harmless: the latch is already true, so
    // no path consults the per-thread table anymore, and the entries are
    // non-owning.
    sourceTable.clear();
}

} // namespace WTF

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

#include <wtf/Threading.h>
#include <wtf/text/StringImpl.h>

namespace WTF {

// NOTE (link-soundness): SharedAtomStringTable::singleton() is deliberately
// defined in AtomStringTable.cpp (always compiled) and
// sharedAtomStringTableEnabled() is header-inline, so always-compiled TUs
// (AtomStringImpl.cpp, StringImpl.h's deref) link WITHOUT this TU. This TU
// holds only enableSharedAtomStringTable(), whose sole callers — the M3
// JSC::initialize hunk and the M14 unit tests — land in the same
// INTEGRATE-vmstate.md batch as the M1 build-file hunk that compiles it.

void enableSharedAtomStringTable()
{
    // Idempotent; init only. Caller contract (§4.8/§8): runs on the
    // JSC::initialize thread, after Options::finalize, before any other
    // thread atomizes anything (binds embedder AND internal service threads;
    // GC/JIT/sampler threads do not atomize). Breaches are not assertable
    // here, and the downstream backstops are ASYMMETRIC (round 4):
    //  - ATOMIZE half: a pre-latch thread that atomized leaves a non-empty
    //    per-thread table and fail-stops at thread death (I17's
    //    RELEASE_ASSERT in ~AtomStringTable) — deterministic.
    //  - REF/DEREF half (a pre-latch thread whose stale latch read routes a
    //    final deref of a meanwhile-in-place-atomized string down the legacy
    //    destroy path, dangling a shard entry): NOT deterministically
    //    detectable. The only detector is the debug-only heuristic ASSERT in
    //    StringImpl::deref's legacy zero-transition arm (fires when the
    //    breach overlaps or follows the flip); release-mode breaches are
    //    silent UAF. The release-mode mitigation is the Bun embedder audit —
    //    cross-WS item 15 in docs/threads/INTEGRATE-vmstate.md.
    if (g_sharedAtomStringTableEnabled.load(std::memory_order_relaxed))
        return;

    // Force construction of the singleton before publishing the latch so no
    // post-latch reader races the (thread-safe, but slow-path) static init.
    auto& shared = SharedAtomStringTable::singleton();

    // (1) MIGRATE FIRST, THEN LATCH (§4.8). Order matters: if the latch were
    // published before migration, another thread performing the FINAL deref
    // of a not-yet-migrated pre-latch atom would take the shared path
    // (deref -> derefSharedZero -> removeDeadAtom), shard-miss, and destroy
    // the StringImpl while the per-thread source table still held a raw entry
    // to it — the migration loop below would then read freed memory and
    // insert a dangling pointer into a shard (silent corruption). Migrating
    // while still unlatched is unobservable: legacy paths never consult the
    // shards, the only atomizing thread pre-latch is this one (§4.8 contract),
    // and a racing cross-thread final deref in the pre-latch window takes the
    // legacy path, which fail-stops in AtomStringImpl::remove's
    // RELEASE_ASSERT exactly as it does on main today — no new hazard.
    //
    // Migrate every pre-latch atom of the initializing thread into its shard.
    // Without this, pre-latch atoms would be invisible to the shards
    // (duplicates breaking I1/I2) and would shard-miss at death. Entries are
    // moved as-is: the shard table holds the same non-owning StringEntry the
    // per-thread table held; refcounts and the isAtom bit are untouched.
    // One shard lock at a time — shard locks are leaves and never nest (§7).
    // The per-shard lock release also release-publishes each migrated entry
    // to any post-latch reader that acquires the same shard lock.
    AtomStringTable* threadTable = Thread::currentSingleton().atomStringTable();
    auto& sourceTable = threadTable->table();
    for (const auto& entry : sourceTable) {
        StringImpl* string = entry.get();
        ASSERT(string);
        ASSERT(string->hasHash()); // Was inserted into a string-keyed table.
        auto& shard = shared.shardForHash(string->existingHash()); // I5: HashTranslator hash == existingHash for resident atoms.
        Locker locker { shard.lock };
        auto addResult = shard.table.add(entry);
        // Single-threaded init + empty shards: every migrated atom is new.
        ASSERT_UNUSED(addResult, addResult.isNewEntry);
    }

    // (2) Publish the latch. Every shard already contains every live atom, so
    // a reader that observes the latch can never shard-miss a pre-latch atom.
    // Release pairs with the synchronization that delivers post-latch atoms
    // (and the fact of initialization) to other threads — see the F4 comment
    // at sharedAtomStringTableEnabled() in SharedAtomStringTable.h.
    g_sharedAtomStringTableEnabled.store(true, std::memory_order_release);

    // (3) Clear the source set; do NOT clear isAtom (§4.8). From here on,
    // rule A1 keeps every per-thread table empty (enforced by I17). The brief
    // window where entries exist in BOTH tables is harmless: the latch is
    // already true, so no path consults the per-thread table anymore, and the
    // entries are non-owning.
    sourceTable.clear();
}

} // namespace WTF

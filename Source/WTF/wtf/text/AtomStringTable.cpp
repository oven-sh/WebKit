/*
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2013 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Patrick Gansterer <paroga@paroga.com>
 * Copyright (C) 2012 Google Inc. All rights reserved.
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
#include <wtf/text/AtomStringTable.h>

#include <wtf/NeverDestroyed.h>
#include <wtf/text/SharedAtomStringTable.h>

namespace WTF {

// Latch for the process-global sharded atom table (SPEC-vmstate §3 R1).
// Set exactly once by enableSharedAtomStringTable(); immutable after.
// Defined here so this destructor reads it without crossing TUs.
std::atomic<bool> g_sharedAtomStringTableEnabled { false };

// Defined HERE — an always-compiled WTF TU — not in SharedAtomStringTable.cpp,
// which enters the build only with the M1 hunk (INTEGRATE-vmstate.md). This
// keeps the bare tree linking (AtomStringImpl.cpp calls singleton()) and
// guarantees exactly one table instance per process even in DLL builds. See
// the declaration comment in SharedAtomStringTable.h.
SharedAtomStringTable& SharedAtomStringTable::singleton()
{
    static NeverDestroyed<SharedAtomStringTable> table;
    return table;
}

AtomStringTable::~AtomStringTable()
{
    if (g_sharedAtomStringTableEnabled.load(std::memory_order_relaxed)) {
        // I17 (SPEC-vmstate §4.7, frozen): in shared mode every per-thread
        // table is empty from the latch on — §4.8 migrated the initializing
        // thread's entries and rule A1 (§4.3) keeps all tables empty. Skip the
        // setIsAtom(false) loop: stripping the flag here would let a
        // shard-resident string's deref-to-zero take the "!isAtom() => destroy
        // as today" path, bypassing removeDeadAtom and leaving a dangling
        // shard entry (UAF on the next lookup). A breach of the §4.8 ordering
        // contract (a non-initializing thread atomized pre-latch) fails
        // deterministically here instead.
        RELEASE_ASSERT(m_table.isEmpty());
        return;
    }

    for (const auto& string : m_table) {
        // Static strings are immortal and their atom flag was set at construction
        // time (via StringImpl::StringAtom), not by setIsAtom(). Skip them here
        // since setIsAtom() asserts !isStatic().
        if (!string->isStatic())
            string->setIsAtom(false);
    }
}

}

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

#pragma once

#include <atomic>
#include <wtf/CompactPtr.h>
#include <wtf/HashSet.h>
#include <wtf/Packed.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringImpl.h>

namespace WTF {

class AtomStringImpl;
class StringImpl;
class StringView;

class AtomStringTable {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AtomStringTable);
public:
    // If CompactPtr is 32bit, it is more efficient than PackedPtr (6 bytes).
    // We select underlying implementation based on CompactPtr's efficacy.
    using StringEntry = std::conditional_t<CompactPtrTraits<StringImpl>::is32Bit, CompactPtr<StringImpl>, PackedPtr<StringImpl>>;
    using StringTableImpl = UncheckedKeyHashSet<StringEntry>;

    WTF_EXPORT_PRIVATE ~AtomStringTable();

    StringTableImpl& table() LIFETIME_BOUND { return m_table; }
    void holdAtomOfStaticString(StringImpl& atom) { m_atomsOfStaticStrings.append(atom); }

#if USE(BUN_JSC_ADDITIONS)
    // Static atoms in read-only memory are in no table, and every thread has the same ones. A table that has no atom with the characters
    // asks this function for one before it makes its own. Null in a process without such atoms.
    using ReadOnlyAtomLookup = AtomStringImpl* (*)(StringView, unsigned hash);
    static ReadOnlyAtomLookup readOnlyAtomLookup() { return s_readOnlyAtomLookup.load(std::memory_order_relaxed); }
    // What the table of the calling thread made earlier with the characters of a read-only atom stops being an atom, as it does when its table dies.
    WTF_EXPORT_PRIVATE static void setReadOnlyAtomLookup(ReadOnlyAtomLookup);
#endif

private:
#if USE(BUN_JSC_ADDITIONS)
    WTF_EXPORT_PRIVATE static std::atomic<ReadOnlyAtomLookup> s_readOnlyAtomLookup;
#endif

    StringTableImpl m_table;
    Vector<Ref<StringImpl>> m_atomsOfStaticStrings;
};

}
using WTF::AtomStringTable;

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

#include <wtf/CompactPtr.h>
#include <wtf/HashSet.h>
#include <wtf/Packed.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringImpl.h>

namespace WTF {

class StringImpl;

class AtomStringTable {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AtomStringTable);
public:
    // If CompactPtr is 32bit, it is more efficient than PackedPtr (6 bytes).
    // We select underlying implementation based on CompactPtr's efficacy.
    using StringEntry = std::conditional_t<CompactPtrTraits<StringImpl>::is32Bit, CompactPtr<StringImpl>, PackedPtr<StringImpl>>;
    using StringTableImpl = UncheckedKeyHashSet<StringEntry>;

    WTF_EXPORT_PRIVATE ~AtomStringTable();

    StringTableImpl& table() LIFETIME_BOUND { return m_table; }

    // Atoms that exist before the thread does, are immortal, and are not in table() until they are first requested: strings that
    // are static (StringImpl::becomeStatic()) and flagged as atoms. This table is read-only, and is consulted when table() has no
    // match. It has to be set before the thread has an atom that could equal one of them.
    // All of the atoms are within range of one base address, and none are ever added. So this is an open-addressing hash table of
    // offsets from that base, in units of 8 bytes, where zero means an empty bucket.
    struct StaticAtoms {
        static constexpr unsigned shift = 3;
        const uint32_t* entries { nullptr };
        uint32_t mask { 0 }; // One less than how many places there are, which is a power of two.
        uintptr_t base { 0 };

        explicit operator bool() const { return !!entries; }
        static unsigned next(unsigned place, unsigned& probes, unsigned mask) { return (place + ++probes) & mask; }
        template<typename HashTranslator, typename T> StringImpl* find(const T& value) const
        {
            unsigned probes = 0;
            for (unsigned place = HashTranslator::hash(value) & mask;; place = next(place, probes, mask)) {
                uint32_t entry = entries[place];
                if (!entry)
                    return nullptr;
                auto* string = reinterpret_cast<StringImpl*>(base + (static_cast<uintptr_t>(entry) << shift));
                if (HashTranslator::equal(StringEntry { string }, value))
                    return string;
            }
        }
    };
    const StaticAtoms& staticAtoms() const LIFETIME_BOUND { return m_staticAtoms; }
    void setStaticAtoms(const StaticAtoms& atoms) { m_staticAtoms = atoms; }

private:
    StringTableImpl m_table;
    StaticAtoms m_staticAtoms;
};

}
using WTF::AtomStringTable;

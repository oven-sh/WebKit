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

    // Atoms that were there before the thread was, are there for good, and are not in table() until they are asked for: strings
    // that are static (StringImpl::becomeStatic()) and say that they are atoms. It is only looked in, when table() does not have
    // what is wanted. To be set before the thread has an atom that may be equal to one of them.
    const StringTableImpl* staticTable() const { return m_staticTable; }
    void setStaticTable(const StringTableImpl* table) { m_staticTable = table; }

private:
    StringTableImpl m_table;
    const StringTableImpl* m_staticTable { nullptr };
};

}
using WTF::AtomStringTable;

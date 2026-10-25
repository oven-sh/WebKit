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

#include <wtf/Threading.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/StringView.h>

namespace WTF {

#if USE(BUN_JSC_ADDITIONS)
constinit std::atomic<AtomStringTable::ReadOnlyAtomLookup> AtomStringTable::s_readOnlyAtomLookup { nullptr };

void AtomStringTable::setReadOnlyAtomLookup(ReadOnlyAtomLookup lookup)
{
    if (readOnlyAtomLookup() == lookup)
        return;
    if (lookup) {
        StringTableImpl& table = Thread::currentSingleton().atomStringTable()->table();
        auto isReplaced = [&](const StringEntry& entry) {
            SUPPRESS_UNCOUNTED_LOCAL StringImpl* string = entry.get();
            return !string->isStatic() && lookup(StringView(*string), string->existingHash());
        };
        bool replacesAny = false;
        for (const StringEntry& entry : table)
            replacesAny |= isReplaced(entry);
        if (replacesAny) {
            table.removeIf([&](const StringEntry& entry) {
                if (!isReplaced(entry))
                    return false;
                SUPPRESS_UNCOUNTED_LOCAL StringImpl* string = entry.get();
                string->setIsAtom(false);
                return true;
            });
        }
    }
    s_readOnlyAtomLookup.store(lookup, std::memory_order_release);
}
#endif

AtomStringTable::~AtomStringTable()
{
    for (const auto& string : m_table) {
        // Static strings are immortal and their atom flag was set at construction
        // time (via StringImpl::StringAtom), not by setIsAtom(). Skip them here
        // since setIsAtom() asserts !isStatic().
        if (!string->isStatic())
            string->setIsAtom(false);
    }
}

}

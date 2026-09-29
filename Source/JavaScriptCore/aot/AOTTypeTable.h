/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "Identifier.h"
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class VM;

namespace AOT {

// The types of the program, as whoever checked them hands them over. The text of the program says of a place which of them goes for it, by
// number (op_type_tag, Graph::typeTagOf()).
//
// An object type is a SHAPE: it has fields, each with a name, a type of its own and a slot. What makes that mean something is that the
// objects the program's literals make are laid out to suit. Each is made as one of a number of LAYOUTS, which says which properties it
// has, in which order, and in which slot each is; and the layouts are numbered so that those that have a given property in a given
// slot are next to each other. An object says which layout it is of (Structure::knownShape()) for as long as that is what it is: so
// whether it has the property there is a matter of whether a number is in a range. If it is not, nothing is known of the object,
// and it is dealt with as any other is. Nothing here has to be true for the program to do what it says.
class TypeTable {
    WTF_MAKE_TZONE_ALLOCATED(TypeTable);
    WTF_MAKE_NONCOPYABLE(TypeTable);
public:
    TypeTable() = default;

    // Options::aotTypeTable(). On the thread whose identifiers the program's are, before anything is compiled.
    JS_EXPORT_PRIVATE static void load(VM&);
    static const TypeTable* shared(); // Null: there is none.

    struct Field {
        uint16_t slot;
        bool isOptional;
        // The layouts that have the property in that slot; and ones that have no property of the name. None: 0, 0.
        uint16_t first;
        uint16_t last;
        uint16_t firstWithout;
        uint16_t lastWithout;
        uint32_t type;
    };
    // Of a type that is a shape, if there are layouts that have it in a slot.
    std::optional<Field> fieldOf(uint32_t type, UniquedStringImpl* name) const;

    struct Layout {
        uint32_t number { 0 };
        unsigned capacity { 0 }; // One more than the last slot.
        Vector<std::pair<UniquedStringImpl*, uint16_t>, 8> properties; // In the order they are added in.
    };
    // Of a type that says what a literal is made as.
    std::optional<Layout> layoutOf(uint32_t type) const;
    unsigned numberOfLayouts() const { return m_layouts.size() - 1; }
    // TEMPORARY-SHAPE-COUNTS: why nothing is made of an access that has this for a type, as a number. Zero: nothing is said.
    unsigned reasonOf(uint32_t type) const
    {
        auto words = record(type);
        return words.size() == 2 && words[0] == Tags && (words[1] >> 31) ? words[1] & 1023 : 0;
    }

private:
    enum Kind : uint32_t { Tags = 1, Shape, Array, Union, IsLayout };
    static constexpr unsigned wordsOfField = 7;

    std::span<const uint32_t> record(uint32_t type) const
    {
        if (!type || type >= m_types.size())
            return { };
        return m_words.span().subspan(m_types[type] + 1, m_words[m_types[type]]);
    }

    Vector<Identifier> m_names;
    // What an object that a literal makes has whether or not it has it itself. (With Options::useImmutableIntrinsics() that is all there is.)
    Vector<Identifier> m_namesOfObjectPrototype;
    Vector<uint32_t> m_words;
    Vector<uint32_t> m_layouts; // By number: where it is.
    Vector<uint32_t> m_types; // Likewise.
};

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

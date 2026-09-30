/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTType.h"
#include "Identifier.h"
#include "Structure.h"
#include <wtf/HashMap.h>
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

    // What is ever put in a slot, as far as whoever checked the types could tell.
    struct Holds {
        uint32_t kinds { 0 }; // The bits of a check (SoundTypeMaskBits). Zero: anything.
        // If not zero: what the bit for other objects stands for is objects born as one of these layouts, and no others.
        uint16_t first { 0 };
        uint16_t last { 0 };
        // A string there is an atom: its type is a union of string literals. So two of them are the same string if they are the same StringImpl.
        bool atoms { false };
        static Holds from(uint32_t kinds, uint32_t families) { return { kinds & ~SlotsOfBornObjects::stringsAreAtoms, static_cast<uint16_t>(families >> 16), static_cast<uint16_t>(families), !!(kinds & SlotsOfBornObjects::stringsAreAtoms) }; }
        uint16_t kindsAsHeld() const { return safeCast<uint16_t>(kinds | (atoms ? SlotsOfBornObjects::stringsAreAtoms : 0u)); } // SlotsOfBornObjects::Held::kinds
        bool saysSomething() const { return kinds; }
        unsigned kindsButForThoseBorn() const { return first ? kinds & ~MaskOtherObject : kinds; }
        Holds kindsOnly() const { return { kinds, 0, 0, atoms }; }
        // What is read from such a slot, if anything is there.
        Type type() const
        {
            if (!kinds)
                return TTop;
            Type result = typeAdmittedByMask(kindsButForThoseBorn());
            if (first)
                result |= typeOfObjectBornWithin(first, last);
            // (In a slot of a struct that says what it holds.)
            if (areStructs())
                result &= ~TInt32;
            return result;
        }
    };

    struct Field {
        uint16_t slot;
        bool isOptional;
        uint8_t inlineSlots { 255 }; // Of a struct: slots from that one on are outside the object.
        bool isInObject() const { return slot < inlineSlots; }
        bool mayBeEmpty { false }; // Of a struct: some object of the family may have nothing there, whatever this type says.
        // The layouts that have the property in that slot; and ones that have no property of the name. None: 0, 0.
        uint16_t first;
        uint16_t last;
        uint16_t firstWithout;
        uint16_t lastWithout;
        uint32_t type;
        Holds holds;
    };
    // Of a type that is a shape, if there are layouts that have it in a slot.
    std::optional<Field> fieldOf(uint32_t type, UniquedStringImpl* name) const;
    // Of structs: what goes for every object of the family, whatever type it is looked at as.
    std::optional<Field> fieldOfFamily(uint32_t family, UniquedStringImpl* name) const;

    // What is said at the `{` of the body of a class. Its instances are born into that family (zero: into none). A method of it that is CLOSED is one that nothing gets hold of but
    // reads that say so (classOfMethodGotAt()): so whoever calls it is known, all of them.
    bool isArray(uint32_t type) const { auto words = record(type); return words.size() == 2 && words[0] == Array; }
    bool isShape(uint32_t type) const { auto words = record(type); return words.size() >= 3 && words[0] == Shape; }
    bool isClass(uint32_t type) const { auto words = record(type); return words.size() >= 3 && words[0] == IsClass; }
    uint16_t familyOfInstancesOf(uint32_t classType) const
    {
        auto words = record(classType);
        return words.size() >= 3 && words[0] == IsClass && words[1] && isUsable(words[1]) ? safeCast<uint16_t>(words[1]) : uint16_t(0);
    }
    bool isClosedMethod(uint32_t classType, UniquedStringImpl* name) const;
    // Of a type that is a shape: reading the property gives one method and no other, which is closed; this is the class that declares it. Zero: there is no telling.
    uint32_t classOfMethodGotAt(uint32_t type, UniquedStringImpl* name) const;

    // Version 4 of the table: the objects are structs (SlotsOfBornObjects::Named). What an object was born as is the number of its family, so Field::first and
    // Field::last are that, as are those of Holds; every name of the family has a slot in every object of it, and an object that has no such property has nothing there.
    bool hasStructs() const { return m_hasStructs; }
    static bool areStructs() { return shared() && shared()->hasStructs(); }
    static bool areStructsToGoBy() { return areStructs() && !Options::aotAuditsTypes(); }
    unsigned numberOfFamilies() const { return m_families.size() - 1; }
    struct NameOfFamily {
        UniquedStringImpl* name;
        uint16_t slot;
        bool mayBeAbsent;
        Holds holds;
    };
    struct Family {
        unsigned capacity { 0 };
        unsigned inlineSlots { 0 };
        Vector<NameOfFamily, 8> names;
    };
    Family family(uint32_t number) const;
    bool isUsable(uint32_t family) const; // Its objects can be made.
    unsigned inlineSlotsOf(uint32_t family) const { return m_words[m_families[family]] >> 16 & 0xff; }
    bool isOpen(uint32_t family) const { return m_words[m_families[family]] >> 30 & 1; } // Code that knows nothing of the types may make objects of it.
    // Of a type that is a shape: whatever is of it was born into its family. So it is if the family is closed; and in one that is open, if nothing that came from elsewhere is seen to get to be of this type.
    bool isTakenAtItsWord(uint32_t type) const
    {
        auto words = record(type);
        if (!m_hasStructs || words.size() < 3 || words[0] != Shape || !isUsable(words[1] >> 16))
            return false;
        return !isOpen(words[1] >> 16) || (words[1] & 2);
    }
    // An object that is made with properties of these names, by whoever does not know what for, may yet be made one of a family: how many slots in the object that could take.
    unsigned inlineSlotsWantedBy(std::span<UniquedStringImpl* const> names) const;
    // Of what an op_new_object is said to make: a layout, or a shape. Zero: none, or no structs.
    uint16_t familyOfWhatIsMade(uint32_t type) const
    {
        if (auto layout = layoutOf(type))
            return layout->family;
        return familyOf(type);
    }
    // Of a type that is a shape. Zero: none, or no structs.
    uint16_t familyOf(uint32_t type) const
    {
        auto words = record(type);
        if (!m_hasStructs || words.size() < 3 || words[0] != Shape || !isUsable(words[1] >> 16))
            return 0;
        return words[1] >> 16;
    }

    struct Layout {
        uint32_t number { 0 };
        uint16_t family { 0 };
        unsigned inlineSlots { 0 }; // Of a struct.
        unsigned capacity { 0 }; // One more than the last slot.
        Vector<std::pair<UniquedStringImpl*, uint16_t>, 8> properties; // In the order they are added in.
        Vector<Holds, 8> holds; // Likewise.
    };
    // Of a type that says what a literal is made as.
    std::optional<Layout> layoutOf(uint32_t type) const;
    unsigned numberOfLayouts() const { return m_layouts.size() - 1; }
    Vector<Holds, 8> holdsOfSlots(uint32_t layout) const; // By slot.
    Vector<Holds, 8> holdsOfSlotsOfFamily(uint32_t family) const; // Likewise.
    // TEMPORARY-SHAPE-COUNTS: why nothing is made of an access that has this for a type, as a number. Zero: nothing is said.
    unsigned reasonOf(uint32_t type) const
    {
        auto words = record(type);
        return words.size() == 2 && words[0] == Tags && (words[1] >> 31) ? words[1] & 1023 : 0;
    }

private:
    enum Kind : uint32_t { Tags = 1, Shape, Array, Union, IsLayout, IsClass };
    static constexpr unsigned wordsOfField = 9;
    static constexpr unsigned wordsOfPropertyOfLayout = 4;

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
    Vector<uint32_t> m_families; // Likewise.
    UncheckedKeyHashMap<UniquedStringImpl*, Vector<uint32_t>> m_openFamiliesWithName; // In order.
    bool m_hasStructs { false };
    unsigned wordsBeforePropertiesOfLayout() const { return m_hasStructs ? 3 : 2; }
    Vector<uint32_t> m_types; // Likewise.
};

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

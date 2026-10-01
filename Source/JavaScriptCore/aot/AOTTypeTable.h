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
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class VM;

namespace AOT {

// The program's static types, as exported by the type checker. Bytecode refers to a type by its index in this table (op_type_tag,
// Graph::typeTagOf()).
//
// An object type is a Shape: a set of fields, each with a name, a type and a slot. Objects allocated by the program's literals and
// constructors are laid out to match. In the current table format (version 4) every object type belongs to a typed layout, which
// assigns each field name a fixed slot, and an object records its typed layout ID in its Structure.
//
// Older table versions number individual Layouts so that all layouts with a given property in a given slot are contiguous. A range
// check on Structure::knownShape() then tells whether an object has that property in that slot.
//
// The table is an optimization hint. An object that fails a layout check takes the generic path, so a wrong table cannot change
// what the program does.
class TypeTable {
    WTF_MAKE_TZONE_ALLOCATED(TypeTable);
    WTF_MAKE_NONCOPYABLE(TypeTable);
public:
    TypeTable() = default;

    // Loads the file named by Options::aotTypeTablePath(). Call before compilation starts, on the thread that owns the program's identifiers.
    JS_EXPORT_PRIVATE static void load(VM&);
    static const TypeTable* shared(); // Null if no table was loaded.

    struct Field;
    // Records that the program compares this field with a string literal. Called during the pre-compilation scan, from any thread.
    void noteComparedWithString(const Field&) const;
    // Call once after the scan. Marks every field recorded above as holding atom strings (FieldType::atoms).
    JS_EXPORT_PRIVATE static void finalizeAtomizedFields();

    // The set of values a field may hold, according to the type checker.
    struct FieldType {
        uint32_t kinds { 0 }; // SoundTypeMaskBits. Zero means unconstrained.
        // If nonzero, MaskOtherObject only admits objects whose typed layout ID is in [first, last].
        uint16_t first { 0 };
        uint16_t last { 0 };
        // Short strings stored here are atomized (TypedLayoutTable::stringsAreAtoms), so they can be compared by pointer. Set when the type
        // is a union of string literals or the program compares the field with string literals.
        bool atoms { false };
        static FieldType from(uint32_t kinds, uint32_t layoutIDs) { return { kinds & ~TypedLayoutTable::stringsAreAtoms, static_cast<uint16_t>(layoutIDs >> 16), static_cast<uint16_t>(layoutIDs), !!(kinds & TypedLayoutTable::stringsAreAtoms) }; }
        uint16_t packedKinds() const { return safeCast<uint16_t>(kinds | (atoms ? TypedLayoutTable::stringsAreAtoms : 0u)); } // As stored in TypedLayoutTable::FieldType::kinds.
        bool isConstrained() const { return kinds; }
        unsigned kindsExcludingTypedObjects() const { return first ? kinds & ~MaskOtherObject : kinds; }
        FieldType kindsOnly() const { return { kinds, 0, 0, atoms }; }
        // The type of a value read from a non-empty slot.
        Type type() const
        {
            if (!kinds)
                return TTop;
            Type result = typeAcceptedByMask(kindsExcludingTypedObjects());
            if (first)
                result |= typeOfObjectWithLayoutInRange(first, last);
            // Typed fields store every number as a double.
            if (hasTypedFields())
                result &= ~TInt32;
            if (atoms)
                result &= ~TShortOtherString;
            return result;
        }
    };

    struct Field {
        uint16_t slot;
        bool isOptional;
        uint8_t inlineSlots { 255 }; // Slots at or beyond this index are out of line.
        bool isInObject() const { return slot < inlineSlots; }
        bool mayBeEmpty { false }; // Some object with this layout may have an empty slot here, even if this type says the field is required.
        // The range of layouts that have the property in this slot, and the range that lack the property. 0, 0 means none.
        uint16_t first;
        uint16_t last;
        uint16_t firstWithout;
        uint16_t lastWithout;
        uint32_t type;
        FieldType fieldType;
        // TypedLayoutTable::Field::id, for layouts that use field IDs. An access must first check that Structure::fieldIDInSlot(slot) equals
        // this. Zero for other layouts.
        uint16_t id { 0 };
    };
    // `type` must be a Shape. Returns nothing if no layout has the property in a slot.
    std::optional<Field> fieldOf(uint32_t type, UniquedStringImpl* name) const;
    // What holds for every object with this typed layout, regardless of the static type it is accessed through.
    std::optional<Field> fieldOfLayout(uint32_t layoutID, UniquedStringImpl* name) const;

    // A class type is tagged at the `{` of the class body. It gives the typed layout of the class's instances (zero if none). A
    // non-escaping method is only ever read by accesses that classOfMethodReadBy() identifies, so all of its callers are known.
    bool isArray(uint32_t type) const { auto words = record(type); return words.size() == 2 && words[0] == Array; }
    bool isShape(uint32_t type) const { auto words = record(type); return words.size() >= 3 && words[0] == Shape; }
    bool isClass(uint32_t type) const { auto words = record(type); return words.size() >= 3 && words[0] == IsClass; }
    uint16_t layoutIDOfInstancesOf(uint32_t classType) const
    {
        auto words = record(classType);
        return words.size() >= 3 && words[0] == IsClass && words[1] && isUsable(words[1]) ? safeCast<uint16_t>(words[1]) : uint16_t(0);
    }
    bool isNonEscapingMethod(uint32_t classType, UniquedStringImpl* name) const;
    // `type` must be a Shape. If reading `name` always yields one particular non-escaping method, returns the class that declares it. Otherwise zero.
    uint32_t classOfMethodReadBy(uint32_t type, UniquedStringImpl* name) const;

    // Table version 4: typed layouts with typed fields (TypedLayoutTable::Field). Field::first, Field::last and the FieldType ranges are typed
    // layout IDs. Every field of a layout has a slot in every object with that layout; the slot is empty if the object lacks the property.
    bool tableHasTypedFields() const { return m_hasTypedFields; }
    static bool hasTypedFields() { return shared() && shared()->tableHasTypedFields(); }
    static bool typedFieldsAreEnforced() { return hasTypedFields() && !Options::auditAOTTypedFields(); }
    unsigned numberOfTypedLayouts() const { return m_typedLayouts.size() - 1; }
    struct LayoutField {
        UniquedStringImpl* name;
        uint16_t slot;
        bool mayBeAbsent;
        FieldType fieldType;
        uint16_t id; // See Field::id.
    };
    struct TypedLayout {
        unsigned capacity { 0 };
        unsigned inlineSlots { 0 };
        bool usesFieldIDs { false };
        Vector<LayoutField, 8> fields;
    };
    TypedLayout typedLayout(uint32_t number) const;
    bool isUsable(uint32_t layoutID) const; // Objects with this layout can be allocated.
    unsigned inlineSlotsOf(uint32_t layoutID) const { return m_words[m_typedLayouts[layoutID]] >> 16 & 0xff; }
    bool isOpen(uint32_t layoutID) const { return m_words[m_typedLayouts[layoutID]] >> 30 & 1; } // Untyped code may also create objects of this type.
    bool usesFieldIDs(uint32_t layoutID) const { return m_words[m_typedLayouts[layoutID]] >> 29 & 1; } // See TypedLayoutTable::usesFieldIDs().
    uint16_t idOfField(uint32_t layoutID, UniquedStringImpl* name) const { return m_idsOfFields.get({ layoutID, name }); } // Zero if it has none.
    // The field type of each slot, for an object with these properties in these slots. For layouts that use field IDs this depends on the names.
    Vector<FieldType, 8> fieldTypesBySlotOfLayout(uint32_t layoutID, std::span<UniquedStringImpl* const> names, std::span<const uint16_t> slots) const;
    // `type` must be a Shape. True if every value of this type is known to have been allocated with its typed layout, so no layout check is
    // needed. That holds for closed layouts, and for open ones when no untyped object can flow into this type.
    bool isTrusted(uint32_t type) const
    {
        auto words = record(type);
        if (!m_hasTypedFields || words.size() < 3 || words[0] != Shape || !isUsable(words[1] >> 16))
            return false;
        // With field IDs, knowing the layout says nothing about which field is in a slot.
        return !isOpen(words[1] >> 16) || ((words[1] & 2) && !usesFieldIDs(words[1] >> 16));
    }
    // Untyped code is allocating an object with these property names. Returns how many inline slots it would need if it were later converted to a typed layout.
    unsigned inlineSlotsNeededFor(std::span<UniquedStringImpl* const> names) const;
    // `type` is the tag on an op_new_object (a Layout or a Shape). Zero if it has no typed layout.
    uint16_t layoutIDOfAllocation(uint32_t type) const
    {
        if (auto layout = layoutOf(type))
            return layout->layoutID;
        return layoutIDOf(type);
    }
    // `type` must be a Shape. Zero if it has no typed layout.
    uint16_t layoutIDOf(uint32_t type) const
    {
        auto words = record(type);
        if (!m_hasTypedFields || words.size() < 3 || words[0] != Shape || !isUsable(words[1] >> 16))
            return 0;
        return words[1] >> 16;
    }

    struct Layout {
        uint32_t number { 0 };
        uint16_t layoutID { 0 };
        unsigned inlineSlots { 0 };
        unsigned capacity { 0 }; // Index of the last slot, plus one.
        Vector<std::pair<UniquedStringImpl*, uint16_t>, 8> properties; // In insertion order.
    };
    // `type` must be the tag on an object literal.
    std::optional<Layout> layoutOf(uint32_t type) const;
    unsigned numberOfLayouts() const { return m_layouts.size() - 1; }
    Vector<FieldType, 8> fieldTypesBySlot(uint32_t layout) const;
    Vector<FieldType, 8> fieldTypesBySlotOfLayout(uint32_t layoutID) const;
    // If the static type is a built-in class, returns the matching Receiver. The compiler treats this as a hint and still checks it. Zero if unknown.
    unsigned receiverHintOf(uint32_t type) const
    {
        auto words = record(type);
        return words.size() == 2 && words[0] == Tags && (words[1] >> 31) ? words[1] >> 10 & 15 : 0;
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
    // Properties every plain object inherits from Object.prototype. With VM::useImmutableIntrinsics this set cannot change.
    Vector<Identifier> m_namesOfObjectPrototype;
    Vector<uint32_t> m_words;
    Vector<uint32_t> m_layouts; // Offset into m_words, by layout number.
    Vector<uint32_t> m_typedLayouts; // Offset into m_words, by typed layout ID.
    UncheckedKeyHashMap<UniquedStringImpl*, Vector<uint32_t>> m_openLayoutsWithField; // Sorted.
    UncheckedKeyHashMap<std::pair<uint32_t, UniquedStringImpl*>, uint16_t> m_idsOfFields; // By typed layout ID and name. Fields beyond the ID space or beyond Structure::numberOfSlotsWithFieldIDs have no entry.
    bool m_hasTypedFields { false };
    mutable Lock m_lockOfFieldsCompared;
    mutable UncheckedKeyHashSet<uint64_t> m_fieldsCompared; // layoutID << 32 | slot << 16 | id.
    unsigned wordsBeforePropertiesOfLayout() const { return m_hasTypedFields ? 3 : 2; }
    Vector<uint32_t> m_types; // Offset into m_words, by type index.
};

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

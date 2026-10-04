/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTType.h"
#include "Identifier.h"
#include "Structure.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class UnlinkedCodeBlock;
class VM;

namespace AOT {

class TypeTable {
    WTF_MAKE_TZONE_ALLOCATED(TypeTable);
    WTF_MAKE_NONCOPYABLE(TypeTable);
public:
    TypeTable() = default;

    JS_EXPORT_PRIVATE static void load(VM&);
    static const TypeTable* shared();

    struct CodeBlockKey {
        uint32_t module;
        uint32_t start;
        uint32_t kind;
        UnlinkedCodeBlock* codeBlock;
    };
    JS_EXPORT_PRIVATE static void loadSiteTypes(std::span<const CodeBlockKey>);
    using SiteTypes = Vector<std::pair<uint32_t, uint32_t>>;
    const SiteTypes* siteTypesIn(UnlinkedCodeBlock* codeBlock) const
    {
        auto it = m_siteTypes.find(codeBlock);
        return it == m_siteTypes.end() ? nullptr : &it->value;
    }

    struct Field;
    void noteComparedWithString(const Field&) const;
    JS_EXPORT_PRIVATE static void finalizeAtomizedFields();

    struct FieldType {
        uint32_t kinds { 0 };
        uint16_t first { 0 };
        uint16_t last { 0 };
        bool atoms { false };
        static FieldType from(uint32_t kinds, uint32_t layoutIDs) { return { kinds & ~TypedLayoutTable::stringsAreAtoms, static_cast<uint16_t>(layoutIDs >> 16), static_cast<uint16_t>(layoutIDs), !!(kinds & TypedLayoutTable::stringsAreAtoms) }; }
        uint16_t packedKinds() const { return safeCast<uint16_t>(kinds | (atoms ? TypedLayoutTable::stringsAreAtoms : 0u)); }
        bool isConstrained() const { return kinds; }
        unsigned kindsExcludingTypedObjects() const { return first ? kinds & ~MaskOtherObject : kinds; }
        FieldType kindsOnly() const { return { kinds, 0, 0, atoms }; }
        Type type() const
        {
            if (!kinds)
                return TTop;
            Type result = typeAcceptedByMask(kindsExcludingTypedObjects());
            if (first)
                result |= objectTypeForLayoutRange(first, last);
            if (hasTypedFields())
                result &= ~TInt32;
            if (atoms)
                result &= ~TShortOtherString;
            return result;
        }
        Type typeOfStored(Type value) const
        {
            Type held = type();
            Type result = value & held;
            if (mayBe(value, TInt32))
                result |= held & TDouble;
            if (mayBe(value, TShortOtherString))
                result |= held & TAtomString;
            if (first && mayBe(value, TFinalObject))
                result |= held & TFinalObject;
            return result;
        }
    };

    struct Field {
        uint16_t slot;
        bool isOptional;
        uint8_t inlineSlots { 255 };
        bool isInObject() const { return slot < inlineSlots; }
        bool mayBeEmpty { false };
        uint16_t first;
        uint16_t last;
        uint16_t firstExcludedLayout;
        uint16_t lastExcludedLayout;
        uint32_t type;
        FieldType fieldType;
        uint16_t id { 0 };
    };
    std::optional<Field> fieldOf(uint32_t type, UniquedStringImpl* name) const;
    std::optional<Field> layoutField(uint32_t layoutID, UniquedStringImpl* name) const;

    bool isArray(uint32_t type) const { auto words = record(type); return words.size() == 2 && words[0] == Array; }
    bool isShape(uint32_t type) const { auto words = record(type); return words.size() >= 3 && words[0] == Shape; }
    bool isClass(uint32_t type) const { auto words = record(type); return words.size() >= 3 && words[0] == IsClass; }
    uint16_t instanceLayoutIDFor(uint32_t classType) const
    {
        auto words = record(classType);
        return words.size() >= 3 && words[0] == IsClass && words[1] && isUsable(words[1]) ? safeCast<uint16_t>(words[1]) : uint16_t(0);
    }
    bool isNonEscapingMethod(uint32_t classType, UniquedStringImpl* name) const;
    uint32_t methodClassReadBy(uint32_t type, UniquedStringImpl* name) const;

    bool tableHasTypedFields() const { return m_hasTypedFields; }
    static bool hasTypedFields() { return shared() && shared()->tableHasTypedFields(); }
    static uint32_t largestFieldID();
    static bool typedFieldsAreEnforced() { return hasTypedFields() && !Options::auditAOTTypedFields(); }
    unsigned numberOfTypedLayouts() const { return m_typedLayouts.size() - 1; }
    struct LayoutField {
        UniquedStringImpl* name;
        uint16_t slot;
        bool mayBeAbsent;
        FieldType fieldType;
        uint16_t id;
    };
    struct TypedLayout {
        unsigned capacity { 0 };
        unsigned inlineSlots { 0 };
        bool usesFieldIDs { false };
        bool isInstanceLayout { false };
        Vector<LayoutField, 8> fields;
    };
    TypedLayout typedLayout(uint32_t number) const;
    bool isUsable(uint32_t layoutID) const;
    bool isInstanceLayout(uint32_t layoutID) const { return m_instanceLayouts.get(layoutID); }
    bool isFilledAtBirth(uint32_t layoutID, unsigned slot) const { return slot < 64 && layoutID < m_slotsFilledAtBirth.size() && (m_slotsFilledAtBirth[layoutID] >> slot & 1); }
    unsigned inlineSlotsOf(uint32_t layoutID) const { return std::max<unsigned>(m_words[m_typedLayouts[layoutID]] >> 16 & 0xff, m_words[m_typedLayouts[layoutID]] & 0xffff); }
    bool isOpen(uint32_t layoutID) const { return m_words[m_typedLayouts[layoutID]] >> 30 & 1; }
    bool usesFieldIDs(uint32_t layoutID) const { return m_words[m_typedLayouts[layoutID]] >> 29 & 1; }
    uint16_t fieldID(uint32_t layoutID, UniquedStringImpl* name) const { return m_fieldIDs.get({ layoutID, name }); }
    Vector<FieldType, 8> layoutFieldTypesBySlot(uint32_t layoutID, std::span<UniquedStringImpl* const> names, std::span<const uint16_t> slots) const;
    bool isTrusted(uint32_t type) const
    {
        auto words = record(type);
        if (!m_hasTypedFields || words.size() < 3 || words[0] != Shape || !isUsable(words[1] >> 16))
            return false;
        return !isOpen(words[1] >> 16) || ((words[1] & 2) && !usesFieldIDs(words[1] >> 16));
    }
    unsigned inlineSlotsNeededFor(std::span<UniquedStringImpl* const> names) const;
    uint16_t allocationLayoutID(uint32_t type) const
    {
        if (auto layout = layoutOf(type))
            return layout->layoutID;
        return layoutIDOf(type);
    }
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
        unsigned capacity { 0 };
        Vector<std::pair<UniquedStringImpl*, uint16_t>, 8> properties;
    };
    std::optional<Layout> layoutOf(uint32_t type) const;
    unsigned numberOfLayouts() const { return m_layouts.size() - 1; }
    Vector<FieldType, 8> fieldTypesBySlot(uint32_t layout) const;
    Vector<FieldType, 8> layoutFieldTypesBySlot(uint32_t layoutID) const;
    std::optional<uint32_t> reasonForNoType(uint32_t type) const
    {
        auto words = record(type);
        if (words.size() == 2 && words[0] == Tags && (words[1] >> 31))
            return words[1] & 1023;
        return std::nullopt;
    }
    unsigned receiverHintOf(uint32_t type) const
    {
        auto words = record(type);
        return words.size() == 2 && words[0] == Tags && (words[1] >> 31) ? words[1] >> 10 & 15 : 0;
    }

private:
    enum Kind : uint32_t { Tags = 1, Shape, Array, Union, IsLayout, IsClass };
    static constexpr unsigned fieldWords = 9;
    static constexpr unsigned layoutPropertyWords = 4;

    std::span<const uint32_t> record(uint32_t type) const
    {
        if (!type || type >= m_types.size())
            return { };
        return m_words.span().subspan(m_types[type] + 1, m_words[m_types[type]]);
    }

    Vector<Identifier> m_names;
    Vector<Identifier> m_objectPrototypeNames;
    Vector<uint32_t> m_words;
    Vector<uint32_t> m_layouts;
    Vector<uint32_t> m_typedLayouts;
    BitVector m_instanceLayouts;
    Vector<uint64_t> m_slotsFilledAtBirth;
    UncheckedKeyHashMap<UniquedStringImpl*, Vector<uint32_t>> m_openLayoutsWithField;
    UncheckedKeyHashMap<std::pair<uint32_t, UniquedStringImpl*>, uint16_t> m_fieldIDs;
    bool m_hasTypedFields { false };
    uint32_t m_largestFieldID { 0 };
    mutable Lock m_fieldsComparedLock;
    mutable UncheckedKeyHashSet<uint64_t> m_fieldsCompared;
    unsigned layoutHeaderWords() const { return m_hasTypedFields ? 3 : 2; }
    Vector<uint32_t> m_types;
    UncheckedKeyHashMap<UnlinkedCodeBlock*, SiteTypes> m_siteTypes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

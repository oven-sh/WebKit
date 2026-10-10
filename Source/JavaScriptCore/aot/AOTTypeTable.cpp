/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTable.h"

#if ENABLE(AOT)

#include "AOTProgram.h"
#include "JSCInlines.h"
#include "Options.h"
#include "UnlinkedCodeBlock.h"
#include <map>
#include <wtf/FileSystem.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TypeTable);

static TypeTable* s_shared;

const TypeTable* TypeTable::shared()
{
    return s_shared;
}

uint32_t TypeTable::largestFieldID()
{
    return Options::useAOTTypedFields() && hasTypedFields() ? s_shared->m_largestFieldID : 0;
}

void TypeTable::load(VM& vm)
{
    if (s_shared || !Options::aotTypeTablePath())
        return;
    auto contents = FileSystem::readEntireFile(String { Options::aotTypeTablePath() });
    RELEASE_ASSERT_WITH_MESSAGE(contents, "The table of types cannot be read");
    auto bytes = contents->span();
    RELEASE_ASSERT(bytes.size() >= 12 && !(bytes.size() % 4) && !memcmp(bytes.data(), "TSTY", 4));
    size_t at = 4;
    auto word = [&] {
        RELEASE_ASSERT(at + 4 <= bytes.size());
        uint32_t result;
        memcpy(&result, bytes.data() + at, 4);
        at += 4;
        return result;
    };
    uint32_t version = word();
    RELEASE_ASSERT_WITH_MESSAGE(version == 3 || version == 4, "The table of types is of another version");
    auto table = makeUnique<TypeTable>();
    table->m_hasTypedFields = version == 4;
    for (uint32_t count = word(); count--;) {
        uint32_t length = word();
        RELEASE_ASSERT(at + length <= bytes.size());
        table->m_names.append(Identifier::fromString(vm, String::fromUTF8(bytes.subspan(at, length))));
        at += (length + 3) & ~3u;
    }
    for (ASCIILiteral name : { "constructor"_s, "__defineGetter__"_s, "__defineSetter__"_s, "hasOwnProperty"_s, "__lookupGetter__"_s, "__lookupSetter__"_s, "isPrototypeOf"_s, "propertyIsEnumerable"_s, "toString"_s, "valueOf"_s, "__proto__"_s, "toLocaleString"_s })
        table->m_objectPrototypeNames.append(Identifier::fromString(vm, name));
    size_t start = at;
    table->m_words.grow((bytes.size() - start) / 4);
    memcpy(table->m_words.mutableSpan().data(), bytes.data() + start, bytes.size() - start);
    auto here = [&] { return static_cast<uint32_t>((at - start) / 4); };
    table->m_layouts.append(0);
    for (uint32_t count = word(); count--;) {
        table->m_layouts.append(here());
        word();
        uint32_t properties = word();
        if (table->m_hasTypedFields)
            word();
        at += properties * layoutPropertyWords * 4;
    }
    table->m_typedLayouts.append(0);
    if (table->m_hasTypedFields) {
        for (uint32_t count = word(); count--;) {
            table->m_typedLayouts.append(here());
            word();
            at += word() * layoutPropertyWords * 4;
        }
    }
    table->m_types.append(0);
    for (uint32_t count = word(); count--;) {
        table->m_types.append(here());
        at += word() * 4;
    }
    RELEASE_ASSERT(at == bytes.size());
    for (uint32_t type = 1; type < table->m_types.size(); ++type) {
        if (uint16_t layoutID = table->instanceLayoutIDFor(type))
            table->m_instanceLayouts.set(layoutID);
    }
    table->m_slotsFilledAtBirth.fill(0, table->m_typedLayouts.size());
    BitVector layoutsWithShape;
    for (uint32_t type = 1; type < table->m_types.size(); ++type) {
        uint16_t layoutID = table->layoutIDOf(type);
        if (!layoutID || table->isOpen(layoutID))
            continue;
        auto words = table->record(type);
        uint64_t filled = 0;
        for (unsigned i = 0; i < words[2]; ++i) {
            auto field = words.subspan(3 + i * fieldWords, fieldWords);
            unsigned slot = field[1] & 0xffff;
            if (slot < 64 && !(field[1] >> 16 & (1 | 4 | 8)) && field[2] == layoutID && field[3] == layoutID)
                filled |= 1ull << slot;
        }
        table->m_slotsFilledAtBirth[layoutID] = layoutsWithShape.get(layoutID) ? table->m_slotsFilledAtBirth[layoutID] & filled : filled;
        layoutsWithShape.set(layoutID);
    }
    for (uint32_t number = 1; number < table->m_typedLayouts.size(); ++number) {
        if (!table->isUsable(number) || !table->isOpen(number))
            continue;
        auto words = table->m_words.span().subspan(table->m_typedLayouts[number]);
        for (unsigned i = 0; i < words[1]; ++i)
            table->m_openLayoutsWithField.add(table->m_names[words[2 + i * layoutPropertyWords]].impl(), Vector<uint32_t> { }).iterator->value.append(number);
    }
    {
        uint32_t last[Structure::numberOfSlotsWithFieldIDs] { };
        for (uint32_t number = 1; number < table->m_typedLayouts.size(); ++number) {
            if (!table->isUsable(number) || !table->usesFieldIDs(number))
                continue;
            auto words = table->m_words.span().subspan(table->m_typedLayouts[number]);
            for (unsigned i = 0; i < words[1]; ++i) {
                auto name = words.subspan(2 + i * layoutPropertyWords, layoutPropertyWords);
                unsigned slot = name[1] & 0xffff;
                if (slot < Structure::numberOfSlotsWithFieldIDs && last[slot] + 1 < Structure::ambiguousFieldID)
                    table->m_fieldIDs.add({ number, table->m_names[name[0]].impl() }, static_cast<uint16_t>(++last[slot]));
            }
        }
        table->m_largestFieldID = std::ranges::max(last);
    }
    RELEASE_ASSERT(table->numberOfLayouts() < std::numeric_limits<uint16_t>::max());
    s_shared = table.release();
}

static TypeTable::Field withId(uint16_t id, TypeTable::Field field)
{
    field.id = id;
    return field;
}

std::optional<TypeTable::Field> TypeTable::fieldOf(uint32_t type, UniquedStringImpl* name) const
{
    auto words = record(type);
    if (words.size() < 3 || words[0] != Shape)
        return std::nullopt;
    for (unsigned i = 0; i < words[2]; ++i) {
        auto field = words.subspan(3 + i * fieldWords, fieldWords);
        if (m_names[field[0]].impl() != name)
            continue;
        uint32_t bits = field[1] >> 16;
        if ((bits & 4) || !field[2])
            return std::nullopt;
        if (m_hasTypedFields && !isUsable(field[2]))
            return std::nullopt;
        bool isInherited = m_objectPrototypeNames.containsIf([&](const Identifier& inherited) { return inherited.impl() == name; });
        uint16_t id = 0;
        if (m_hasTypedFields && usesFieldIDs(field[2])) {
            id = m_fieldIDs.get({ field[2], name });
            if (!id || isInherited)
                return std::nullopt;
        }
        return withId(id, Field { static_cast<uint16_t>(field[1]), !!(bits & 1), m_hasTypedFields ? static_cast<uint8_t>(inlineSlotsOf(field[2])) : uint8_t(255), !!(bits & 8), safeCast<uint16_t>(field[2]), safeCast<uint16_t>(field[3]), isInherited ? uint16_t(0) : safeCast<uint16_t>(field[4]), isInherited ? uint16_t(0) : safeCast<uint16_t>(field[5]), field[6], FieldType::from(field[7], field[8]) });
    }
    return std::nullopt;
}

bool TypeTable::isUsable(uint32_t number) const
{
    if (!number || number >= m_typedLayouts.size())
        return false;
    auto words = m_words.span().subspan(m_typedLayouts[number]);
    return (words[0] & 0xffff) <= 255 && inlineSlotsOf(number) <= std::min<unsigned>(JSFinalObject::maxInlineCapacity, 255) && (words[0] & 0xffff) && words[1] <= 4095;
}

TypeTable::TypedLayout TypeTable::typedLayout(uint32_t number) const
{
    TypedLayout result;
    if (!isUsable(number))
        return result;
    auto words = m_words.span().subspan(m_typedLayouts[number]);
    result.capacity = words[0] & 0xffff;
    result.inlineSlots = inlineSlotsOf(number);
    result.usesFieldIDs = usesFieldIDs(number);
    result.isInstanceLayout = isInstanceLayout(number);
    for (unsigned i = 0; i < words[1]; ++i) {
        auto name = words.subspan(2 + i * layoutPropertyWords, layoutPropertyWords);
        result.fields.append({ m_names[name[0]].impl(), static_cast<uint16_t>(name[1]), !!(name[1] >> 16), FieldType::from(name[2], name[3]), m_fieldIDs.get({ number, m_names[name[0]].impl() }) });
    }
    return result;
}

Vector<TypeTable::FieldType, 8> TypeTable::fieldTypesBySlot(uint32_t number) const
{
    Vector<FieldType, 8> result;
    if (!number || number >= m_layouts.size())
        return result;
    auto layout = m_words.span().subspan(m_layouts[number]);
    result.grow(layout[0]);
    for (unsigned i = 0; i < layout[1]; ++i) {
        auto property = layout.subspan(layoutHeaderWords() + i * layoutPropertyWords, layoutPropertyWords);
        result[property[1]] = FieldType::from(property[2], property[3]);
    }
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

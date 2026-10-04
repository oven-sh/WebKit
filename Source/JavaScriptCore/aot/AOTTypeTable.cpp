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
    }
    RELEASE_ASSERT(table->numberOfLayouts() < std::numeric_limits<uint16_t>::max());
    s_shared = table.release();
}

void TypeTable::loadSiteTypes(std::span<const CodeBlockKey> codeBlocks)
{
    if (!s_shared || !Options::aotSiteTypesPath())
        return;
    size_t at = 0;
    auto refuseUnless = [&](bool condition, ASCIILiteral why) {
        if (condition) [[likely]]
            return;
        dataLogLn("AOT: the file of aotSiteTypesPath is refused at byte ", at, ": ", why);
        CRASH();
    };
    auto contents = FileSystem::readEntireFile(String { Options::aotSiteTypesPath() });
    refuseUnless(!!contents, "it cannot be read"_s);
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, UnlinkedCodeBlock*> codeBlockOfKey;
    for (auto& key : codeBlocks)
        codeBlockOfKey.emplace(std::tuple { key.module, key.start, key.kind }, key.codeBlock);
    auto bytes = contents->span();
    auto number = [&] {
        refuseUnless(at < bytes.size() && isASCIIDigit(bytes[at]), "a number is expected"_s);
        uint64_t result = 0;
        for (; at < bytes.size() && isASCIIDigit(bytes[at]); ++at)
            result = result * 10 + (bytes[at] - '0');
        return safeCast<uint32_t>(result);
    };
    auto skip = [&](char character) {
        refuseUnless(at < bytes.size() && bytes[at] == character, "a line is five fields separated by tabs and ends with a newline"_s);
        ++at;
    };
    UncheckedKeyHashMap<UnlinkedCodeBlock*, uint32_t> layoutOfThis;
    unsigned sites = 0;
    unsigned sitesInUnknownCode = 0;
    while (at < bytes.size()) {
        uint32_t module = number();
        skip('\t');
        uint32_t start = number();
        skip('\t');
        uint32_t kind = number();
        skip('\t');
        std::optional<uint32_t> offset;
        if (at < bytes.size() && bytes[at] == 't') {
            for (char character : { 't', 'h', 'i', 's' })
                skip(character);
        } else
            offset = number();
        skip('\t');
        uint32_t type = number();
        skip('\n');
        auto it = codeBlockOfKey.find({ module, start, kind });
        if (it == codeBlockOfKey.end()) {
            ++sitesInUnknownCode;
            continue;
        }
        ++sites;
        if (offset) {
            s_shared->m_siteTypes.add(it->second, SiteTypes { }).iterator->value.append({ *offset, type });
            continue;
        }
        refuseUnless(kind & 1, "the layout of this can only be given for construct code"_s);
        refuseUnless(layoutOfThis.add(it->second, type).iterator->value == type, "one function gets two layouts for this"_s);
        if (ProgramClasses* classes = programClasses())
            classes->noteThisIn(it->second, safeCast<uint16_t>(type));
    }
    for (auto& [codeBlock, siteTypes] : s_shared->m_siteTypes) {
        std::ranges::sort(siteTypes);
        for (size_t i = 1; i < siteTypes.size(); ++i)
            refuseUnless(siteTypes[i - 1].first != siteTypes[i].first, "one site has two lines"_s);
        size_t next = 0;
        for (const auto& instruction : codeBlock->instructions()) {
            if (next == siteTypes.size() || siteTypes[next].first != instruction.offset())
                continue;
            OpcodeID opcode = instruction->opcodeID();
            refuseUnless(opcode == op_new_object || opcode == op_get_by_id || opcode == op_put_by_id || opcode == op_get_length, "an offset is that of an instruction that takes no type: was the file made for another build of the program?"_s);
            ++next;
        }
        refuseUnless(next == siteTypes.size(), "an offset is not the start of an instruction: was the file made for another build of the program?"_s);
    }
    if (Options::verboseAOTCompilation())
        dataLogLn("AOT: ", sites, " sites have a type from outside; ", sitesInUnknownCode, " more are in code that is not compiled");
}

void TypeTable::noteComparedWithString(const Field& field) const
{
    if (!m_hasTypedFields || field.fieldType.atoms || !(field.fieldType.kinds & SoundTypeString))
        return;
    Locker locker { m_fieldsComparedLock };
    m_fieldsCompared.add(static_cast<uint64_t>(field.first) << 32 | static_cast<uint64_t>(field.slot) << 16 | field.id);
}

void TypeTable::finalizeAtomizedFields()
{
    TypeTable* table = s_shared;
    if (!table || !table->m_hasTypedFields || table->m_fieldsCompared.isEmpty())
        return;
    auto words = table->m_words.mutableSpan();
    UncheckedKeyHashSet<uint64_t> names;
    for (uint32_t number = 1; number < table->m_typedLayouts.size(); ++number) {
        if (!table->isUsable(number))
            continue;
        auto record = words.subspan(table->m_typedLayouts[number]);
        for (unsigned i = 0; i < record[1]; ++i) {
            auto name = record.subspan(2 + i * layoutPropertyWords, layoutPropertyWords);
            if (!(name[2] & SoundTypeString) || (name[2] & TypedLayoutTable::stringsAreAtoms))
                continue;
            uint16_t id = table->m_fieldIDs.get({ number, table->m_names[name[0]].impl() });
            if (!table->m_fieldsCompared.contains(static_cast<uint64_t>(number) << 32 | static_cast<uint64_t>(name[1] & 0xffff) << 16 | id))
                continue;
            name[2] |= TypedLayoutTable::stringsAreAtoms;
            names.add(static_cast<uint64_t>(number) << 32 | (name[0] + 1));
        }
    }
    for (uint32_t number = 1; number < table->m_layouts.size(); ++number) {
        auto layout = words.subspan(table->m_layouts[number]);
        for (unsigned i = 0; i < layout[1]; ++i) {
            auto property = layout.subspan(table->layoutHeaderWords() + i * layoutPropertyWords, layoutPropertyWords);
            if (names.contains(static_cast<uint64_t>(layout[2]) << 32 | (property[0] + 1)))
                property[2] |= TypedLayoutTable::stringsAreAtoms;
        }
    }
    for (uint32_t type = 1; type < table->m_types.size(); ++type) {
        auto record = words.subspan(table->m_types[type] + 1, words[table->m_types[type]]);
        if (record.size() < 3 || record[0] != Shape)
            continue;
        for (unsigned i = 0; i < record[2]; ++i) {
            auto field = record.subspan(3 + i * fieldWords, fieldWords);
            if (field[2] && names.contains(static_cast<uint64_t>(field[2]) << 32 | (field[0] + 1)))
                field[7] |= TypedLayoutTable::stringsAreAtoms;
        }
    }
    if (Options::verboseAOTCompilation())
        dataLogLn("AOT: ", names.size(), " fields that hold strings are compared with strings that the program spells out: the short strings there are atoms");
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

std::optional<TypeTable::Field> TypeTable::layoutField(uint32_t number, UniquedStringImpl* name) const
{
    if (!m_hasTypedFields || !number || number >= m_typedLayouts.size() || !isUsable(number))
        return std::nullopt;
    auto words = m_words.span().subspan(m_typedLayouts[number]);
    for (unsigned i = 0; i < words[1]; ++i) {
        auto entry = words.subspan(2 + i * layoutPropertyWords, layoutPropertyWords);
        if (m_names[entry[0]].impl() != name)
            continue;
        bool mayBeAbsent = !!(entry[1] >> 16);
        if (mayBeAbsent && m_objectPrototypeNames.containsIf([&](const Identifier& inherited) { return inherited.impl() == name; }))
            return std::nullopt;
        uint16_t id = 0;
        if (usesFieldIDs(number)) {
            id = m_fieldIDs.get({ number, name });
            if (!id)
                return std::nullopt;
        }
        return withId(id, Field { static_cast<uint16_t>(entry[1]), mayBeAbsent, static_cast<uint8_t>(inlineSlotsOf(number)), true, safeCast<uint16_t>(number), safeCast<uint16_t>(number), 0, 0, 0,
            FieldType::from(entry[2], entry[3]) });
    }
    return std::nullopt;
}

bool TypeTable::isNonEscapingMethod(uint32_t classType, UniquedStringImpl* name) const
{
    auto words = record(classType);
    if (words.size() < 3 || words[0] != IsClass)
        return false;
    for (unsigned i = 0; i < words[2]; ++i) {
        if (m_names[words[3 + i]].impl() == name)
            return true;
    }
    return false;
}

uint32_t TypeTable::methodClassReadBy(uint32_t type, UniquedStringImpl* name) const
{
    auto words = record(type);
    if (words.size() < 3 || words[0] != Shape)
        return 0;
    for (unsigned i = 0; i < words[2]; ++i) {
        auto field = words.subspan(3 + i * fieldWords, fieldWords);
        if (m_names[field[0]].impl() == name)
            return (field[1] >> 16 & 16) && isClass(field[6]) ? field[6] : 0;
    }
    return 0;
}

std::optional<TypeTable::Layout> TypeTable::layoutOf(uint32_t type) const
{
    auto words = record(type);
    if (words.size() != 2 || words[0] != IsLayout || !words[1] || words[1] >= m_layouts.size())
        return std::nullopt;
    Layout result;
    result.number = words[1];
    auto layout = m_words.span().subspan(m_layouts[result.number]);
    result.capacity = layout[0];
    if (m_hasTypedFields) {
        if (!isUsable(layout[2]))
            return std::nullopt;
        result.layoutID = safeCast<uint16_t>(layout[2]);
        result.inlineSlots = std::max<unsigned>(inlineSlotsOf(layout[2]), result.capacity);
    }
    for (unsigned i = 0; i < layout[1]; ++i) {
        auto property = layout.subspan(layoutHeaderWords() + i * layoutPropertyWords, layoutPropertyWords);
        result.properties.append({ m_names[property[0]].impl(), safeCast<uint16_t>(property[1]) });
    }
    return result;
}

unsigned TypeTable::inlineSlotsNeededFor(std::span<UniquedStringImpl* const> names) const
{
    if (names.empty() || m_openLayoutsWithField.isEmpty())
        return 0;
    const Vector<uint32_t>* fewest = nullptr;
    for (UniquedStringImpl* name : names) {
        auto it = m_openLayoutsWithField.find(name);
        if (it == m_openLayoutsWithField.end())
            return 0;
        if (!fewest || it->value.size() < fewest->size())
            fewest = &it->value;
    }
    unsigned result = 0;
    for (uint32_t layoutID : *fewest) {
        if (!usesFieldIDs(layoutID) && inlineSlotsOf(layoutID) <= result)
            continue;
        bool hasAll = true;
        for (UniquedStringImpl* name : names)
            hasAll &= std::ranges::binary_search(m_openLayoutsWithField.find(name)->value, layoutID);
        if (!hasAll)
            continue;
        unsigned wanted = inlineSlotsOf(layoutID);
        if (usesFieldIDs(layoutID)) {
            wanted = 0;
            for (UniquedStringImpl* name : names) {
                if (auto field = layoutField(layoutID, name))
                    wanted = std::max<unsigned>(wanted, field->slot + 1);
            }
        }
        result = std::max(result, wanted);
    }
    return result;
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

Vector<TypeTable::FieldType, 8> TypeTable::layoutFieldTypesBySlot(uint32_t number, std::span<UniquedStringImpl* const> names, std::span<const uint16_t> slots) const
{
    if (!isUsable(number) || !usesFieldIDs(number))
        return layoutFieldTypesBySlot(number);
    Vector<FieldType, 8> result;
    auto layout = this->typedLayout(number);
    for (unsigned i = 0; i < names.size() && i < slots.size(); ++i) {
        for (auto& name : layout.fields) {
            if (name.name != names[i] || name.slot != slots[i])
                continue;
            if (result.size() <= name.slot)
                result.grow(name.slot + 1);
            result[name.slot] = name.fieldType;
        }
    }
    return result;
}

Vector<TypeTable::FieldType, 8> TypeTable::layoutFieldTypesBySlot(uint32_t number) const
{
    Vector<FieldType, 8> result;
    auto layout = this->typedLayout(number);
    result.grow(layout.capacity);
    for (auto& name : layout.fields)
        result[name.slot] = name.fieldType;
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

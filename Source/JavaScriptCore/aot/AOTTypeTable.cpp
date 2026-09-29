/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTable.h"

#if ENABLE(FTL_JIT)

#include "JSCInlines.h"
#include "Options.h"
#include <wtf/FileSystem.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TypeTable);

static TypeTable* s_shared;

const TypeTable* TypeTable::shared()
{
    return s_shared;
}

// See ~/code/tmp/aot/tsfacts/totypes.ts for the format, for now.
void TypeTable::load(VM& vm)
{
    if (s_shared || !Options::aotTypeTable())
        return;
    auto contents = FileSystem::readEntireFile(String::fromUTF8(Options::aotTypeTable()));
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
    RELEASE_ASSERT_WITH_MESSAGE(word() == 3, "The table of types is of another version");
    auto table = makeUnique<TypeTable>();
    for (uint32_t count = word(); count--;) {
        uint32_t length = word();
        RELEASE_ASSERT(at + length <= bytes.size());
        table->m_names.append(Identifier::fromString(vm, String::fromUTF8(bytes.subspan(at, length))));
        at += (length + 3) & ~3u;
    }
    for (ASCIILiteral name : { "constructor"_s, "__defineGetter__"_s, "__defineSetter__"_s, "hasOwnProperty"_s, "__lookupGetter__"_s, "__lookupSetter__"_s, "isPrototypeOf"_s, "propertyIsEnumerable"_s, "toString"_s, "valueOf"_s, "__proto__"_s, "toLocaleString"_s })
        table->m_namesOfObjectPrototype.append(Identifier::fromString(vm, name));
    size_t start = at;
    table->m_words.grow((bytes.size() - start) / 4);
    memcpy(table->m_words.mutableSpan().data(), bytes.data() + start, bytes.size() - start);
    auto here = [&] { return static_cast<uint32_t>((at - start) / 4); };
    table->m_layouts.append(0);
    for (uint32_t count = word(); count--;) {
        table->m_layouts.append(here());
        word();
        at += word() * wordsOfPropertyOfLayout * 4;
    }
    table->m_types.append(0);
    for (uint32_t count = word(); count--;) {
        table->m_types.append(here());
        at += word() * 4;
    }
    RELEASE_ASSERT(at == bytes.size());
    RELEASE_ASSERT(table->numberOfLayouts() < std::numeric_limits<uint16_t>::max());
    s_shared = table.release();
}

std::optional<TypeTable::Field> TypeTable::fieldOf(uint32_t type, UniquedStringImpl* name) const
{
    auto words = record(type);
    if (words.size() < 3 || words[0] != Shape)
        return std::nullopt;
    for (unsigned i = 0; i < words[2]; ++i) {
        auto field = words.subspan(3 + i * wordsOfField, wordsOfField);
        if (m_names[field[0]].impl() != name)
            continue;
        uint32_t bits = field[1] >> 16;
        if ((bits & 4) || !field[2])
            return std::nullopt;
        bool isInherited = m_namesOfObjectPrototype.containsIf([&](const Identifier& inherited) { return inherited.impl() == name; });
        return Field { static_cast<uint16_t>(field[1]), !!(bits & 1), safeCast<uint16_t>(field[2]), safeCast<uint16_t>(field[3]), isInherited ? uint16_t(0) : safeCast<uint16_t>(field[4]), isInherited ? uint16_t(0) : safeCast<uint16_t>(field[5]), field[6], Holds { field[7], static_cast<uint16_t>(field[8] >> 16), static_cast<uint16_t>(field[8]) } };
    }
    return std::nullopt;
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
    for (unsigned i = 0; i < layout[1]; ++i) {
        auto property = layout.subspan(2 + i * wordsOfPropertyOfLayout, wordsOfPropertyOfLayout);
        result.properties.append({ m_names[property[0]].impl(), safeCast<uint16_t>(property[1]) });
        result.holds.append(Holds { property[2], static_cast<uint16_t>(property[3] >> 16), static_cast<uint16_t>(property[3]) });
    }
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

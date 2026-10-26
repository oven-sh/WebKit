/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRParserInternal.h"

#if ENABLE(AOT)

#include "AOTNameClass.h"
#include "AOTVector.h"
#include <limits>
#include <type_traits>
#include <wtf/MathExtras.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC { namespace AOT { namespace Typed {

static uint32_t hashOf(std::span<const uint8_t> bytes)
{
    return StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(bytes));
}

template<typename Element>
static Wire::Span appendTo(Vector<Element>& list, std::span<const Element> elements)
{
    Wire::Span span { static_cast<uint32_t>(list.size()), static_cast<uint32_t>(elements.size()) };
    list.append(elements);
    return span;
}

WireProgramBuilder::WireProgramBuilder(const BuiltinTable& builtins)
    : m_builtins(builtins)
{
}

uint32_t WireProgramBuilder::internString(std::span<const uint8_t> bytes)
{
    Vector<uint32_t, 1>& positions = m_strings.add(hashOf(bytes), Vector<uint32_t, 1>()).iterator->value;
    for (uint32_t position : positions) {
        const Wire::String& candidate = m_sections.strings[position];
        if (equalSpans(m_sections.bytes.subspan(candidate.offset, candidate.length), bytes))
            return position;
    }

    NameClass nameClass = classifyName(bytes);
    BuiltinName builtinName = m_builtins.nameOf(bytes);
    Wire::Span text = appendBytes(bytes);

    uint32_t position = static_cast<uint32_t>(m_sections.strings.size());
    m_sections.strings.append(Wire::String { text.start, text.count, static_cast<uint32_t>(nameClass), builtinName ? builtinName.index() : Wire::none });
    positions.append(position);
    return position;
}

template<typename WireRecord>
uint32_t WireProgramBuilder::internRecord(Vector<WireRecord>& list, PositionsByHash& positionsByHash, const WireRecord& record)
{
    static_assert(std::has_unique_object_representations_v<WireRecord>);

    std::span<const uint8_t> bytes = asByteSpan(record);
    Vector<uint32_t, 1>& positions = positionsByHash.add(hashOf(bytes), Vector<uint32_t, 1>()).iterator->value;
    for (uint32_t position : positions) {
        if (equalSpans(asByteSpan(list[position]), bytes))
            return position;
    }

    uint32_t position = static_cast<uint32_t>(list.size());
    list.append(record);
    positions.append(position);
    return position;
}

uint32_t WireProgramBuilder::internPropertyKey(const Wire::PropertyKey& key)
{
    return internRecord(m_sections.propertyKeys, m_propertyKeys, key);
}

uint32_t WireProgramBuilder::internType(const Wire::Type& type)
{
    return internRecord(m_sections.types, m_types, type);
}

Wire::Span WireProgramBuilder::appendWords(std::span<const uint32_t> words)
{
    return appendTo(m_sections.words, words);
}

Wire::Span WireProgramBuilder::appendBytes(std::span<const uint8_t> bytes)
{
    return appendTo(m_sections.bytes, bytes);
}

Wire::Span WireProgramBuilder::appendCode(std::span<const uint32_t> words)
{
    return appendTo(m_sections.code, words);
}

Vector<uint8_t> WireProgramBuilder::finish()
{
    Wire::Header header;
    header.magic = Wire::Magic;
    header.schemaHash = Wire::schemaHash;

    size_t size = sizeof(Wire::Header);
    m_sections.forEachList([&](ProgramList list, std::span<const uint8_t> records, uint32_t count) {
        header.sections[static_cast<size_t>(list)] = { static_cast<uint32_t>(size), count };
        size = roundUpToMultipleOf<sizeof(uint32_t)>(size + records.size());
    });
    size = roundUpToMultipleOf<sizeof(uint64_t)>(size);
    RELEASE_ASSERT(size <= std::numeric_limits<uint32_t>::max());
    header.sizeInBytes = static_cast<uint32_t>(size);

    Vector<uint8_t> buffer(FillWith { }, size, uint8_t { 0 });
    memcpySpan(buffer.mutableSpan(), asByteSpan(header));
    m_sections.forEachList([&](ProgramList list, std::span<const uint8_t> records, uint32_t) {
        if (!records.empty())
            memcpySpan(buffer.mutableSubspan(header.sections[static_cast<size_t>(list)].offsetInBytes), records);
    });
    return buffer;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

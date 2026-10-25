/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "YarrSharedTables.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if ENABLE(YARR_JIT)

namespace JSC { namespace Yarr {

struct BuiltInClassBitPlanes {
    std::array<CharacterClass::Table, 2> tables;
    Vector<uint8_t> bytes;
};

static_assert(std::tuple_size_v<decltype(BuiltInClassBitPlanes::tables)> <= std::numeric_limits<uint8_t>::digits);

static const BuiltInClassBitPlanes& builtInClassBitPlanes()
{
    static LazyNeverDestroyed<BuiltInClassBitPlanes> bitPlanes;
    static std::once_flag once;
    std::call_once(once, [] {
        bitPlanes.construct();
        bitPlanes->tables = { spacesCreate()->m_table, wordcharCreate()->m_table };
        bitPlanes->bytes.fill(0, CharacterClass::tableSize);
        for (unsigned bitPlane = 0; bitPlane < bitPlanes->tables.size(); ++bitPlane) {
            for (unsigned character = 0; character < CharacterClass::tableSize; ++character) {
                if (bitPlanes->tables[bitPlane][character])
                    bitPlanes->bytes[character] |= 1 << bitPlane;
            }
        }
    });
    return bitPlanes.get();
}

static std::span<const uint16_t> bmpCanonicalForms(CanonicalMode canonicalMode)
{
    static LazyNeverDestroyed<std::array<Vector<uint16_t>, 2>> tables;
    static std::once_flag once;
    std::call_once(once, [] {
        tables.construct();
        for (auto mode : { CanonicalMode::UCS2, CanonicalMode::Unicode }) {
            auto& table = tables.get()[static_cast<unsigned>(mode)];
            table.grow(0x10000);
            for (char32_t character = 0; character < 0x10000; ++character) {
                auto* info = canonicalRangeInfoFor(character, mode);
                char32_t form = character;
                switch (info->type) {
                case CanonicalizeUnique:
                case CanonicalizeRangeLo:
                    break;
                case CanonicalizeSet:
                    for (auto* set = canonicalCharacterSetInfo(info->value, mode); *set; ++set)
                        form = std::min(form, *set);
                    break;
                case CanonicalizeRangeHi:
                    form = character - info->value;
                    break;
                case CanonicalizeAlternatingAligned:
                    form = character | 1;
                    break;
                case CanonicalizeAlternatingUnaligned:
                    form = (character - 1) | 1;
                    break;
                }
                table[character] = static_cast<uint16_t>(form);
            }
        }
    });
    return tables.get()[static_cast<unsigned>(canonicalMode)].span();
}

std::span<const uint8_t> sharedTable(SharedTableID id)
{
    switch (id) {
    case SharedTableID::BuiltInClassBitPlanes:
        return builtInClassBitPlanes().bytes.span();
    case SharedTableID::Latin1Canonicalization:
        return asBytes(std::span { latin1CanonicalizationTable });
    case SharedTableID::BMPCanonicalFormsUCS2:
        return asBytes(bmpCanonicalForms(CanonicalMode::UCS2));
    case SharedTableID::BMPCanonicalFormsUnicode:
        return asBytes(bmpCanonicalForms(CanonicalMode::Unicode));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint8_t> builtInClassBitPlane(CharacterClass::Table table)
{
    auto& tables = builtInClassBitPlanes().tables;
    auto position = std::ranges::find(tables, table);
    if (position == tables.end())
        return std::nullopt;
    return static_cast<uint8_t>(position - tables.begin());
}

SharedTableID bmpCanonicalFormsTable(CanonicalMode canonicalMode)
{
    switch (canonicalMode) {
    case CanonicalMode::UCS2:
        return SharedTableID::BMPCanonicalFormsUCS2;
    case CanonicalMode::Unicode:
        return SharedTableID::BMPCanonicalFormsUnicode;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Vector<CanonicalizationRange>> computeRangeHiEntriesAboveBMP()
{
    Vector<CanonicalizationRange> result;
    for (size_t i = 0; i < UNICODE_CANONICALIZATION_RANGES; ++i) {
        auto& range = unicodeRangeInfo[i];
        if (range.end < 0x10000)
            continue;
        if (range.type == CanonicalizeRangeHi && range.begin >= 0x10000 + range.value)
            result.append(range);
        else if (range.type != CanonicalizeUnique && (range.type != CanonicalizeRangeLo || range.begin < 0x10000))
            return std::nullopt;
    }
    return result;
}

const std::optional<Vector<CanonicalizationRange>>& rangeHiEntriesAboveBMP()
{
    static LazyNeverDestroyed<std::optional<Vector<CanonicalizationRange>>> ranges;
    static std::once_flag once;
    std::call_once(once, [] {
        ranges.construct(computeRangeHiEntriesAboveBMP());
    });
    return ranges.get();
}

} } // namespace JSC::Yarr

#endif // ENABLE(YARR_JIT)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTPrefixCode.h"
#include "AOTSchemaPackedRecords.h"
#include "AOTVector.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

enum class SideTableByte : uint8_t {
    Flags,
    FrameChangesSize,
    FirstFrameChange,
    LaterFrameChange,
    Frame,
    FrameMapStart,
    FrameMapStartContinuation,
    FrameMapSize,
    HandlerRows,
    HandlerStart,
    HandlerSites,
    HandlerTarget,
    HandlerTargetContinuation,
    ConstructedCallee,
    CallSiteHead,
    CallSiteHeadContinuation,
    PrologueCheckCount,
    PrologueCall,
    PrologueCallContinuation,
    PrologueResume,
    SitesWithoutCode,
    InlineFrameCount,
    InlineFrameParent,
    InlineFunction,
    InlineFunctionContinuation,
    SitesWithDetails,
    SitesSkipped,
    SiteDetails,
    SiteDetailsContinuation,
    SiteDetailsAfterInlined,
    SiteDetailsAfterInlinedContinuation,
    CalledBuiltin,
    CalledBuiltinContinuation,
    Distance,
    DistanceContinuation,
    DistanceAfterDetails,
    DistanceAfterDetailsContinuation,
    Continuation,
};
static constexpr unsigned sideTableByteCount = std::to_underlying(SideTableByte::Continuation) + 1;

struct SideTableShortWords {
    static constexpr unsigned bits(unsigned code)
    {
        switch (static_cast<SideTableByte>(code)) {
        case SideTableByte::Distance:
        case SideTableByte::DistanceAfterDetails:
            return 10;
        case SideTableByte::LaterFrameChange:
        case SideTableByte::CallSiteHead:
        case SideTableByte::PrologueCall:
        case SideTableByte::InlineFunction:
        case SideTableByte::InlineFunctionContinuation:
        case SideTableByte::SiteDetails:
        case SideTableByte::SiteDetailsContinuation:
        case SideTableByte::SiteDetailsAfterInlined:
        case SideTableByte::SiteDetailsAfterInlinedContinuation:
            return 9;
        default:
            return 7;
        }
    }
};
using SideTableCodes = PrefixCodes<sideTableByteCount, SideTableShortWords>;

static constexpr size_t sideTablePadding = 8;

struct SiteCheckpoint {
    static constexpr unsigned sitesBetween = 64;
    static constexpr uint64_t numberFor(uint64_t numberOfSites) { return numberOfSites ? (numberOfSites - 1) / sitesBetween : 0; }
    static SiteCheckpoint at(const uint8_t* first, uint64_t index)
    {
        SiteCheckpoint result;
        memcpy(&result, first + index * sizeof(SiteCheckpoint), sizeof(SiteCheckpoint));
        return result;
    }

    uint32_t offset;
    uint32_t position;
    uint32_t frame;
};
static_assert(sizeof(SiteCheckpoint) == 12);

struct SiteDetailsWord {
    static constexpr unsigned maskOfKind = std::bit_ceil(std::to_underlying(CallSiteKind::AwaitedCall) + 1u) - 1;

    CallSiteKind kind() const { return static_cast<CallSiteKind>(bits & maskOfKind); }
    bool isInlined() const { return bits > maskOfKind; }

    uint64_t bits;
};

class SideTableSymbolWriter {
public:
    explicit SideTableSymbolWriter(PrefixSymbol* firstReservedSymbol)
        : m_symbols(firstReservedSymbol)
    {
    }

    PrefixSymbol* end() const { return m_symbols.end(); }
    ALWAYS_INLINE void append(SideTableByte kind, uint8_t byte) { m_symbols.append(std::to_underlying(kind), byte); }
    ALWAYS_INLINE void appendVarint(SideTableByte first, SideTableByte continuation, uint64_t value) { m_symbols.appendVarint(std::to_underlying(first), std::to_underlying(continuation), value); }
    ALWAYS_INLINE void appendVarint(SideTableByte first, uint64_t value) { appendVarint(first, SideTableByte::Continuation, value); }
    void append(std::span<const PrefixSymbol> symbols) { m_symbols.append(symbols); }

private:
    PrefixSymbolWriter m_symbols;
};

class SideTableReader {
public:
    SideTableReader(std::span<const uint8_t> section, const uint8_t* begin)
        : m_codes(section.data())
        , m_end(section.data() + section.size())
        , m_reader(begin, m_end)
    {
    }

    ALWAYS_INLINE uint8_t next(SideTableByte kind)
    {
        return static_cast<uint8_t>(m_reader.read(SideTableCodes::code(m_codes, std::to_underlying(kind))));
    }
    ALWAYS_INLINE uint64_t nextVarint(SideTableByte first, SideTableByte continuation = SideTableByte::Continuation)
    {
        uint8_t byte = next(first);
        uint64_t value = byte & 0x7f;
        for (unsigned shift = 7; byte & 0x80 && shift < 64; shift += 7) {
            byte = next(continuation);
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        }
        return value;
    }
    struct DistanceCodes {
        PrefixCodeView distance;
        PrefixCodeView distanceAfterDetails;
    };
    DistanceCodes distanceCodes() const
    {
        return { SideTableCodes::code(m_codes, std::to_underlying(SideTableByte::Distance)), SideTableCodes::code(m_codes, std::to_underlying(SideTableByte::DistanceAfterDetails)) };
    }
    ALWAYS_INLINE uint64_t nextDistance(const DistanceCodes& codes, bool followsDetails)
    {
        uint8_t byte = static_cast<uint8_t>(m_reader.read(followsDetails ? codes.distanceAfterDetails : codes.distance));
        if (!(byte & 0x80)) [[likely]]
            return byte;
        SideTableByte continuation = followsDetails ? SideTableByte::DistanceAfterDetailsContinuation : SideTableByte::DistanceContinuation;
        return (byte & 0x7f) | nextVarint(continuation, continuation) << 7;
    }
    const uint8_t* alignToByte() { return m_reader.alignToByte(); }
    SideTableReader at(const uint8_t* begin) const { return SideTableReader(std::span(m_codes, m_end), begin); }

private:
    const uint8_t* m_codes;
    const uint8_t* m_end;
    PrefixCodeReader m_reader;
};

struct HandlerCursor {
    ALWAYS_INLINE friend uint64_t readVarint(HandlerCursor& cursor)
    {
        switch (cursor.word++) {
        case 0:
            return cursor.reader.nextVarint(SideTableByte::HandlerStart);
        case 1:
            return cursor.reader.nextVarint(SideTableByte::HandlerSites);
        default:
            return cursor.reader.nextVarint(SideTableByte::HandlerTarget, SideTableByte::HandlerTargetContinuation);
        }
    }

    SideTableReader& reader;
    unsigned word { 0 };
};

class SiteDetailsReader {
public:
    ALWAYS_INLINE explicit SiteDetailsReader(SideTableReader& reader)
        : m_reader(reader)
        , m_numberLeft(static_cast<unsigned>(reader.nextVarint(SideTableByte::SitesWithDetails)))
    {
        skipSites();
    }

    bool isAtEnd() const { return !m_numberLeft; }
    bool isAt(unsigned site) const { return m_numberLeft && m_site == site; }
    unsigned site() const { return m_site; }
    ALWAYS_INLINE CallSiteDetails next()
    {
        CallSiteDetails details = CallSiteDetails::read(*this);
        --m_numberLeft;
        ++m_site;
        skipSites();
        return details;
    }

    ALWAYS_INLINE friend uint64_t readVarint(SiteDetailsReader& details)
    {
        SideTableReader& reader = details.m_reader;
        if (std::exchange(details.m_readsBuiltin, false))
            return reader.nextVarint(SideTableByte::CalledBuiltin, SideTableByte::CalledBuiltinContinuation);
        SiteDetailsWord word { details.m_followsInlined ? reader.nextVarint(SideTableByte::SiteDetailsAfterInlined, SideTableByte::SiteDetailsAfterInlinedContinuation)
                                                        : reader.nextVarint(SideTableByte::SiteDetails, SideTableByte::SiteDetailsContinuation) };
        details.m_followsInlined = word.isInlined();
        details.m_readsBuiltin = namesBuiltinOf(word.kind());
        return word.bits;
    }

private:
    ALWAYS_INLINE void skipSites()
    {
        if (m_numberLeft)
            m_site += static_cast<unsigned>(m_reader.nextVarint(SideTableByte::SitesSkipped));
    }

    SideTableReader& m_reader;
    unsigned m_numberLeft;
    unsigned m_site { 0 };
    bool m_followsInlined { false };
    bool m_readsBuiltin { false };
};

struct InlineFrameCursor {
    ALWAYS_INLINE friend uint64_t readVarint(InlineFrameCursor& cursor)
    {
        if (std::exchange(cursor.readsFunction, true))
            return cursor.functions.nextVarint(SideTableByte::InlineFunction, SideTableByte::InlineFunctionContinuation);
        return cursor.parents.nextVarint(SideTableByte::InlineFrameParent);
    }

    SideTableReader& parents;
    SideTableReader& functions;
    bool readsFunction { false };
};

struct SideTableFrame {
    ALWAYS_INLINE static SideTableFrame read(SideTableReader& reader, uint32_t offset)
    {
        SideTableFrame result;
        uint64_t size = reader.nextVarint(SideTableByte::FrameChangesSize);
        if (!size)
            return result;
        uint64_t position = offset / codeOffsetUnit;
        uint64_t change = 0;
        uint64_t distance = 0;
        unsigned shift = 0;
        SideTableByte kind = SideTableByte::FirstFrameChange;
        while (size--) {
            uint8_t byte = reader.next(kind);
            kind = SideTableByte::LaterFrameChange;
            distance |= static_cast<uint64_t>(byte & 0x7f) << (shift & 63);
            if (byte & 0x80) {
                shift += 7;
                continue;
            }
            change += distance;
            result.coversOffset ^= position >= change;
            distance = 0;
            shift = 0;
        }
        result.hasFrame = true;
        result.number = static_cast<uint32_t>(reader.nextVarint(SideTableByte::Frame));
        return result;
    }

    bool hasFrame { false };
    bool coversOffset { false };
    uint32_t number { 0 };
};

struct SideTableParts {
    JS_EXPORT_PRIVATE static SideTableParts read(ImageFunction, std::span<const uint8_t> section, uint32_t offset = 0);

    std::optional<uint32_t> constructedCalleeAt(uint32_t site) const;

    SideTableFrame frame;
    uint32_t firstFrameMapWord { 0 };
    uint32_t numberOfFrameMapWords { 0 };
    Vector<uint8_t, 16> constructedCallees;
    std::optional<SideTableReader> callSites;
    bool hasHandlers { false };
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTCodeSites.h"
#include "AOTImageForward.h"
#include "AOTLimits.h"
#include "AOTPrefixCode.h"
#include "AOTSchemaPackedRecords.h"
#include "AOTSideTableCodes.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include "HandlerInfo.h"
#include "LineColumn.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class FunctionNumbers;
class LinkedFunctions;
struct CompiledCode;
struct SiteDescription;
struct SiteHandler;

namespace Typed {
class CallingConventions;
}

struct CallSiteRecord {
    CallSiteIndex callSite;
    unsigned inlineFrame;
    bool isTailDeleted(bool calleeWasEntered) const { return isTailCall() && (calleeWasEntered || isInsideCallee()); }
    bool isConstruct() const { return isConstructOf(kind); }
    bool isTailCall() const { return isTailCallOf(kind); }
    bool isInsideCallee() const { return isInsideCalleeOf(kind); }
    bool isAwaited() const { return isAwaitedOf(kind); }

    CallSiteKind kind { CallSiteKind::Plain };
    BuiltinID calledBuiltin { };
    std::optional<CallSiteIndex> firstReadSite { };
};
static_assert(sizeof(CallSiteRecord) == 20);

inline constexpr unsigned siteNumberBits = 23;
inline constexpr unsigned maxFirstReadColumnDelta = (1u << (32 - siteNumberBits)) - 1;
static_assert(defaultLimitValue(LimitID::SitesPerFunction, hostArchitecture()) < 1u << siteNumberBits);
inline CallSiteIndex firstReadSiteInFrontOf(CallSiteIndex site, unsigned columnDelta) { return CallSiteIndex(site.bits() | columnDelta << siteNumberBits); }

struct InlineFrameRecord : InlineFrameRow {
    CallSiteIndex callSite;
};

struct UndecodedInlineFrames {
    std::optional<SideTableReader> parents;
    std::optional<SideTableReader> functions;
    uint64_t count { 0 };
};

class DecodedInlineFrames {
public:

private:
    friend class InlineFrameReader;

    struct FramesOfFunction {
        const Image* image { nullptr };
        uint32_t function { 0 };
        Vector<InlineFrameRecord> frames;
        UndecodedInlineFrames rest;
    };
    static constexpr size_t numberOfFunctions = 64;

    Vector<FramesOfFunction> m_functions;
};

class InlineFrameReader {
public:
    InlineFrameReader() = default;
    explicit InlineFrameReader(DecodedInlineFrames& decoded)
        : m_decoded(&decoded)
    {
    }

    const InlineFrameRecord& at(const Image& image, uint32_t function, unsigned frame)
    {
        if (m_image != &image || m_function != function || frame > m_frames.size())
            read(image, function, frame);
        return m_frames[frame - 1];
    }

private:
    void read(const Image&, uint32_t function, unsigned last);
    void decode(const Image&, uint32_t function, unsigned last);
    void keep();

    const Image* m_image { nullptr };
    std::optional<uint32_t> m_function;
    Vector<InlineFrameRecord, 32> m_frames;
    UndecodedInlineFrames m_rest;
    DecodedInlineFrames* m_decoded { nullptr };
};

class SiteFrames {
    WTF_MAKE_NONCOPYABLE(SiteFrames);
public:
    SiteFrames(const Image&, uint32_t function);
    unsigned next();
    void skip(uint64_t numberOfSites);

private:
    void startBlock(const uint8_t* begin);

    std::optional<SideTableReader> m_inlineFrameParents;
    std::optional<SideTableReader> m_sites;
    std::optional<SiteDetailsReader> m_details;
    const uint8_t* m_checkpoints { nullptr };
    const uint8_t* m_firstBlock { nullptr };
    uint64_t m_nextBlock { 0 };
    unsigned m_siteInBlock { 0 };
    uint64_t m_remainingSuspensionSites { 0 };
    uint64_t m_remainingInlineFrames { 0 };
    uint64_t m_remainingSitesWithCode { 0 };
    unsigned m_frame { 0 };
};

inline FunctionID functionInFramesOf(BuiltinID builtin, size_t numberOfFunctions) { return FunctionID(numberOfFunctions + builtin.index()); }

struct CallSiteStreamHead {
    struct Block {
        uint32_t numberOfSymbols;
        uint32_t offset;
        uint32_t frame;
    };

    uint32_t numberOfHeadSymbols;
    uint32_t numberOfParentSymbols;
    uint32_t numberOfFunctionSymbols;
    uint32_t numberOfBlocks;
    uint32_t hasInlineFrames;
};

Vector<Vector<PrefixSymbol>> encodeCallSites(const LinkedFunctions&, const FunctionNumbers&, const Typed::CallingConventions&, BuildContext&);
std::optional<uint32_t> endOfLastReturnSite(const CompiledCode&);

void* resumeAddressAfterStackCheck(const void* returnAddress);

struct HandlerRecord {
    friend bool operator==(const HandlerRecord&, const HandlerRecord&) = default;

    constexpr bool ignoresValue() const { return type == HandlerType::SynthesizedCatch; }

    uint32_t codeOffset;
    HandlerType type;
};

void encodeHandlers(Vector<uint8_t>& bytes, std::span<const SiteHandler>);
std::optional<HandlerRecord> findHandler(std::span<const uint8_t> handlers, CallSiteIndex);

static constexpr LineColumn superCallOfDefaultConstructor { 1, 28 };

static constexpr uint32_t firstLineOfEvaluatedStrings = 1u << 30;
static constexpr unsigned lineInEvaluatedStringBits = 16;

struct SourcePosition {
    friend bool operator==(const SourcePosition&, const SourcePosition&) = default;

    SourcePosition shown() const;

    const LineColumn& startOfExpression() const { return constructionStart.line && !startIsForCallers && !startIsWhereWritten ? constructionStart : lineColumn; }

    LineColumn lineColumn;
    ModuleID source;
    LineColumn constructionStart { 0, 0 };
    bool startIsForCallers { false };
    bool startIsWhereWritten { false };
    bool isInEvaluatedString { false };
};

inline SourcePosition SourcePosition::shown() const
{
    if (lineColumn.line < firstLineOfEvaluatedStrings) [[likely]]
        return *this;
    constexpr uint32_t mask = (1u << lineInEvaluatedStringBits) - 1;
    SourcePosition result = *this;
    result.lineColumn.line &= mask;
    result.constructionStart.line &= mask;
    result.isInEvaluatedString = true;
    return result;
}

static constexpr unsigned sitesBetweenPositionCheckpoints = 128;

struct OwnPosition {
    static OwnPosition of(const SiteDescription&);

    uint32_t line { 0 };
    uint32_t source { 0 };
};

std::optional<SourcePosition> decodePosition(const Image&, uint32_t function, const uint8_t* section, const uint8_t* entry, OwnPosition ofFunctionInFront, CallSiteIndex);
void forgetRememberedPositions();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

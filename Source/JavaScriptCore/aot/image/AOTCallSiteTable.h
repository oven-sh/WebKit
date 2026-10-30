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
#include "AOTSchemaPackedRecords.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include "HandlerInfo.h"
#include "LineColumn.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class LinkedFunctions;
struct CompiledCode;
struct SiteDescription;
struct SiteHandler;

struct CallSiteRecord {
    CallSiteIndex callSite;
    unsigned inlineFrame;
    // A function that makes a tail call leaves the stack when its callee begins.
    bool isTailDeleted(bool calleeWasEntered) const { return isTailCall() && (calleeWasEntered || isInsideCallee()); }
    bool isConstruct() const { return isConstructOf(kind); }
    bool isTailCall() const { return isTailCallOf(kind); }
    bool isInsideCallee() const { return isInsideCalleeOf(kind); }

    CallSiteKind kind { CallSiteKind::Plain };
    // What is called runs steps of this function, which has no frame of its own.
    BuiltinID calledBuiltin { };
    std::optional<CallSiteIndex> firstReadSite { };
};
static_assert(sizeof(CallSiteRecord) == 20);

// A first read without a site of its own is the site behind it and, above the bits of a site number, the columns between the two.
inline constexpr unsigned siteNumberBits = 23;
inline constexpr unsigned maxFirstReadColumnDelta = (1u << (32 - siteNumberBits)) - 1;
static_assert(defaultLimitValue(LimitID::SitesPerFunction, hostArchitecture()) < 1u << siteNumberBits);
inline CallSiteIndex firstReadSiteInFrontOf(CallSiteIndex site, unsigned columnDelta) { return CallSiteIndex(site.bits() | columnDelta << siteNumberBits); }

struct InlineFrameRecord : InlineFrameRow {
    CallSiteIndex callSite;
};

// A stack goes to and fro between a few functions. A reader that outlives one walk puts what it decodes here, and decodes each of them once.
class DecodedInlineFrames {
public:
    unsigned numberOfTimesDecoded() const { return m_numberOfTimesDecoded; }

private:
    friend class InlineFrameReader;

    struct FramesOfFunction {
        const Image* image { nullptr };
        uint32_t function { 0 };
        Vector<InlineFrameRecord> frames;
    };
    static constexpr size_t numberOfFunctions = 64;

    // Empty until a second function is asked for.
    Vector<FramesOfFunction> m_functions;
    unsigned m_numberOfTimesDecoded { 0 };
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
    DecodedInlineFrames* m_decoded { nullptr };
};

class SiteFrames {
public:
    SiteFrames(const Image&, uint32_t function);
    // Of the encoded call sites of one function. Without any, every site is in frame 0.
    explicit SiteFrames(const uint8_t* callSites);
    unsigned next();
    void skip(uint64_t numberOfSites);

private:
    const uint8_t* m_inlineFrames { nullptr };
    const uint8_t* m_sites { nullptr };
    uint64_t m_remainingSuspensionSites { 0 };
    uint64_t m_remainingInlineFrames { 0 };
    uint64_t m_remainingSitesWithCode { 0 };
    unsigned m_frame { 0 };
};

// Frames number the built-in functions after the program's functions. An inline frame with such a number stands for a built-in function whose steps were expanded into its caller: no site with code lies in it.
inline FunctionID functionInFramesOf(BuiltinID builtin, size_t numberOfFunctions) { return FunctionID(numberOfFunctions + builtin.index()); }
Vector<Vector<uint8_t>> encodeCallSites(const LinkedFunctions&, size_t numberOfFunctions, BuildContext&);
std::optional<uint32_t> endOfLastReturnSite(const CompiledCode&);

void* resumeAddressAfterStackCheck(const void* returnAddress);

// Where a call goes on when its callee returns a value that is not of the type it promises (PromisedResults). No exception is caught there. Compiled code has no use for the type that the engine gives this number.
inline constexpr HandlerType landingOfMisfit = HandlerType::SynthesizedFinally;

struct HandlerRecord {
    friend bool operator==(const HandlerRecord&, const HandlerRecord&) = default;

    constexpr bool ignoresValue() const { return type == HandlerType::SynthesizedCatch; }
    constexpr bool catchesExceptions() const { return type != landingOfMisfit; }

    uint32_t codeOffset;
    HandlerType type;
};

void encodeHandlers(Vector<uint8_t>& bytes, std::span<const SiteHandler>);
std::optional<HandlerRecord> findHandler(std::span<const uint8_t> handlers, CallSiteIndex);
bool catchesExceptions(const Image&, uint32_t function);

struct SourcePosition {
    friend bool operator==(const SourcePosition&, const SourcePosition&) = default;

    const LineColumn& startOfExpression() const { return constructionStart.line && !startIsForCallers ? constructionStart : lineColumn; }

    LineColumn lineColumn;
    ModuleID source;
    LineColumn constructionStart { 0, 0 };
    // `constructionStart` is no start of a construction: it is where the site is reported while something that it called runs.
    bool startIsForCallers { false };
};

static constexpr unsigned sitesBetweenPositionCheckpoints = 512;

struct OwnPosition {
    static OwnPosition of(const SiteDescription&);

    uint32_t line { 0 };
    uint32_t source { 0 };
};

void encodePositions(Vector<uint8_t>& stream, const SiteDescription&, const uint8_t* callSites, OwnPosition ofFunctionInFront);
std::optional<SourcePosition> decodePosition(const Image&, uint32_t function, const uint8_t* entry, OwnPosition ofFunctionInFront, CallSiteIndex);
void forgetRememberedPositions();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

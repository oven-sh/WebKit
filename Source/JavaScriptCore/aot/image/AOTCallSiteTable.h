/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTImageForward.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class LinkedFunctions;
struct CompiledCode;

struct CallSiteRecord {
    CallSiteIndex callSite;
    unsigned inlineFrame;
    bool isConstruct;
    bool isTailCall;
    std::optional<CallSiteIndex> firstReadSite;
};

struct InlineFrameRecord {
    unsigned parent;
    CallSiteIndex callSite;
    FunctionID function;
    bool isConstruct;
    bool isTailCall;
};

class InlineFrameReader {
public:
    const InlineFrameRecord& at(const Image& image, uint32_t function, unsigned frame)
    {
        if (m_image != &image || m_function != function || frame > m_frames.size())
            read(image, function, frame);
        return m_frames[frame - 1];
    }

private:
    void read(const Image&, uint32_t function, unsigned last);

    const Image* m_image { nullptr };
    std::optional<uint32_t> m_function;
    Vector<InlineFrameRecord, 32> m_frames;
};

class SiteFrames {
public:
    SiteFrames(const Image&, uint32_t function);
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

// A stream for each function. Empty for one without return sites and prologue checks.
Vector<Vector<uint8_t>> encodeCallSites(const LinkedFunctions&, BuildContext&);
std::optional<uint32_t> endOfLastReturnSite(const CompiledCode&);

void* resumeAddressAfterStackCheck(const void* returnAddress);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

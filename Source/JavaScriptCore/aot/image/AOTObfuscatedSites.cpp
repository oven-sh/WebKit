/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObfuscatedSites.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTLinkedFunctions.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/FastBitVector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

using Wire::ObfuscatedSiteRecord;
using Wire::TextRange;

static uint32_t numberOfFrames(const CompiledCode& code)
{
    return std::max<uint32_t>(code.sites.inlineFrames.size(), 1);
}

static StringID frameName(const Typed::Program& program, const CompiledCode& code, uint32_t frame)
{
    FunctionID function = findFunctionAsRead(&program, frame ? code.body.callees[code.sites.inlineFrames[frame].callee] : code.function);
    return function ? program.at(function).name : StringID();
}

static TextRange appendUTF8(Vector<uint8_t>& names, const Typed::StringTable& strings, StringID string)
{
    if (strings[string].empty())
        return { };
    uint32_t offset = static_cast<uint32_t>(names.size());
    if (strings.isAllASCII(string))
        names.append(strings[string]);
    else
        names.append(byteCast<uint8_t>(strings.toWTFString(string).utf8(StrictConversionReplacingUnpairedSurrogatesWithFFFD).span()));
    return { offset, static_cast<uint32_t>(names.size()) - offset };
}

static Vector<TextRange> encodeFrameNames(const LinkedFunctions& functions, const Typed::Program& program, const ParallelLoops& loops, Vector<uint8_t>& names)
{
    FastBitVector isFrameName(program.strings.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(functions.size()), [&](unsigned position) {
        const CompiledCode& code = functions[position];
        for (uint32_t frame = 0; frame < numberOfFrames(code); ++frame) {
            if (StringID name = frameName(program, code, frame))
                isFrameName.atomicSetAndCheck(name.index(), true);
        }
    });

    Vector<TextRange> rangeByString(program.strings.size());
    isFrameName.forEachSetBit([&](size_t string) {
        rangeByString[string] = appendUTF8(names, program.strings, StringID(string));
    });
    return rangeByString;
}

static void appendSiteRecords(Vector<ObfuscatedSiteRecord>& records, uint32_t position, const CompiledCode& code, const Typed::Program& program, const Vector<TextRange>& rangeByString)
{
    Vector<TextRange, 8> nameByFrame;
    for (uint32_t frame = 0; frame < numberOfFrames(code); ++frame) {
        StringID name = frameName(program, code, frame);
        nameByFrame.append(name ? rangeByString[name.index()] : TextRange { });
    }

    const Vector<SitePosition>& positions = code.sites.positions;
    size_t firstRecord = records.size();
    for (uint32_t site = 0; site < positions.size(); ++site) {
        const SitePosition& sitePosition = positions[site];
        records.append(ObfuscatedSiteRecord {
            .function = position,
            .site = site,
            .location = { sitePosition.source ? sitePosition.source.index() : Wire::none, sitePosition.sourceOffset },
            .name = nameByFrame[sitePosition.inlineFrame],
        });
    }
    for (const SpreadSite& item : code.sites.spreadSites)
        records[firstRecord + item.site].name = nameByFrame[positions[item.callSite].inlineFrame];
}

ObfuscatedSites collectObfuscatedSites(const LinkedFunctions& functions, const Typed::Program& program, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    ObfuscatedSites result;
    Vector<TextRange> rangeByString = encodeFrameNames(functions, program, loops, result.names);
    result.records = collectInParallel<ObfuscatedSiteRecord>(loops, static_cast<unsigned>(functions.size()), [&](unsigned position, Vector<ObfuscatedSiteRecord>& records) {
        appendSiteRecords(records, position, functions[position], program, rangeByString);
    });
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

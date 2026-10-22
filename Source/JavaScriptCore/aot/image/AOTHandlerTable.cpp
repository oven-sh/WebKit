/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHandlerTable.h"

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTVarint.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

static_assert(!static_cast<uint8_t>(HandlerType::Catch) && static_cast<uint8_t>(HandlerType::Finally) == 1 && static_cast<uint8_t>(HandlerType::SynthesizedCatch) == 2 && static_cast<uint8_t>(HandlerType::SynthesizedFinally) == 3);
static_assert(std::endian::native == std::endian::little);

static constexpr unsigned bitsPerType = 2;
static constexpr uint8_t typeMask = (1 << bitsPerType) - 1;
static constexpr uint8_t takesExceptionBit = 1 << 0;
static constexpr uint8_t ignoresValueBit = 1 << 1;

static_assert(static_cast<uint8_t>(HandlerType::Finally) == takesExceptionBit && static_cast<uint8_t>(HandlerType::SynthesizedCatch) == ignoresValueBit);
static_assert(HandlerRecord { 0, HandlerType::SynthesizedCatch }.ignoresValue() && !HandlerRecord { 0, HandlerType::Catch }.ignoresValue() && !HandlerRecord { 0, HandlerType::Finally }.ignoresValue());

namespace {

struct HandlerRange {
    uint32_t start;
    uint32_t end;
    uint32_t target;
    HandlerType type;
};

} // anonymous namespace

static Vector<HandlerRange> computeHandlerRanges(std::span<const SiteHandler> handlers)
{
    Vector<HandlerRange> ranges;
    for (auto& [site, codeOffset, type] : handlers) {
        if (!ranges.isEmpty() && ranges.last().end == site && ranges.last().target == codeOffset && ranges.last().type == type) {
            ranges.last().end = site + 1;
            continue;
        }
        ranges.append({ site, site + 1, codeOffset, type });
    }
    return ranges;
}

// The ranges of sites ascend.
void encodeHandlers(Vector<uint8_t>& bytes, std::span<const SiteHandler> handlers)
{
    uint32_t endOfLast = 0;
    int64_t targetOfLast = 0;
    for (auto& range : computeHandlerRanges(handlers)) {
        RELEASE_ASSERT(range.start >= endOfLast && range.end > range.start);
        appendVarint(bytes, range.start - endOfLast);
        appendVarint(bytes, range.end - range.start - 1);
        appendVarint(bytes, zigZagEncode(range.target - targetOfLast) << bitsPerType | static_cast<uint8_t>(range.type));
        endOfLast = range.end;
        targetOfLast = range.target;
    }
}

std::optional<HandlerRecord> findHandler(std::span<const uint8_t> handlers, CallSiteIndex site)
{
    const uint8_t* cursor = handlers.data();
    const uint8_t* end = cursor + handlers.size();
    uint64_t endOfLast = 0;
    int64_t target = 0;
    while (cursor < end) {
        uint64_t start = endOfLast + readVarint(cursor);
        endOfLast = start + readVarint(cursor) + 1;
        uint64_t word = readVarint(cursor);
        target += zigZagDecode(word >> bitsPerType);
        if (site.bits() < start)
            return std::nullopt;
        if (site.bits() < endOfLast)
            return HandlerRecord { static_cast<uint32_t>(target), static_cast<HandlerType>(word & typeMask) };
    }
    return std::nullopt;
}

bool catchesExceptions(const Image& image, uint32_t function)
{
    const ImageFunction& record = image.function(function);
    if (!record.has(ImageFunctionFlag::HasHandlers))
        return false;
    std::span<const uint8_t> handlers = SideTableParts::read(record, image.section<uint8_t>(ImageSection::SideTables).data()).handlers;
    const uint8_t* cursor = handlers.data();
    const uint8_t* end = cursor + handlers.size();
    while (cursor < end) {
        readVarint(cursor);
        readVarint(cursor);
        if (HandlerRecord { 0, static_cast<HandlerType>(readVarint(cursor) & typeMask) }.catchesExceptions())
            return true;
    }
    return false;
}

std::optional<HandlerRecord> Image::handlerAt(uint32_t function, CallSiteIndex site) const
{
    const ImageFunction& record = this->function(function);
    if (!record.has(ImageFunctionFlag::HasHandlers))
        return std::nullopt;
    return findHandler(SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables).data()).handlers, site);
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

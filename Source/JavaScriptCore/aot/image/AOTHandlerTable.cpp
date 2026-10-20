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

static constexpr uint8_t widthCodeMask = 0x3;
static constexpr uint8_t hasTypesMask = 1 << 2;
static constexpr unsigned countShift = 3;
static constexpr size_t maxCountInHead = UINT8_MAX >> countShift;

static constexpr unsigned bitsPerType = 2;
static constexpr unsigned typesPerByte = 8 / bitsPerType;
static constexpr uint8_t typeMask = (1 << bitsPerType) - 1;

namespace {

struct HandlerRange {
    uint32_t start;
    uint32_t end;
    uint32_t target;
    HandlerType type;
};

template<typename Number>
struct EncodedRange {
    Number start;
    Number end;
    Number target;
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

void encodeHandlers(Vector<uint8_t>& bytes, std::span<const SiteHandler> handlers)
{
    if (handlers.empty())
        return;
    Vector<HandlerRange> ranges = computeHandlerRanges(handlers);
    uint32_t greatest = 0;
    uint32_t endOfLast = 0;
    for (auto& range : ranges) {
        RELEASE_ASSERT(range.start >= endOfLast && range.end > range.start && range.type != HandlerType::SynthesizedFinally);
        greatest = std::max(greatest, std::max(range.end, range.target));
        endOfLast = range.end;
    }
    unsigned widthCode = greatest <= UINT8_MAX ? 0 : greatest <= UINT16_MAX ? 1 : 2;
    unsigned width = 1 << widthCode;
    bool hasTypes = ranges.containsIf([](auto& range) {
        return range.type != HandlerType::Catch;
    });
    bool isCountInHead = ranges.size() <= maxCountInHead;
    bytes.append(static_cast<uint8_t>(widthCode | (hasTypes ? hasTypesMask : 0) | (isCountInHead ? ranges.size() << countShift : 0)));
    if (!isCountInHead)
        appendVarint(bytes, ranges.size());
    for (auto& range : ranges) {
        for (uint32_t number : { range.start, range.end, range.target }) {
            for (unsigned i = 0; i < width; ++i)
                bytes.append(static_cast<uint8_t>(number >> 8 * i));
        }
    }
    if (hasTypes) {
        size_t startOfTypes = bytes.size();
        bytes.grow(startOfTypes + divideRoundedUp<size_t>(ranges.size(), typesPerByte));
        zeroSpan(bytes.mutableSpan().subspan(startOfTypes));
        for (size_t i = 0; i < ranges.size(); ++i)
            bytes[startOfTypes + i / typesPerByte] |= static_cast<uint8_t>(ranges[i].type) << bitsPerType * (i % typesPerByte);
    }
}

template<typename Number>
static std::optional<HandlerRecord> findHandlerInRanges(const uint8_t* ranges, size_t count, bool hasTypes, uint32_t site)
{
    static_assert(sizeof(EncodedRange<Number>) == 3 * sizeof(Number));
    auto rangeAt = [&](size_t index) {
        EncodedRange<Number> range;
        memcpy(&range, ranges + index * sizeof(range), sizeof(range));
        return range;
    };
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (rangeAt(middle).end > site)
            high = middle;
        else
            low = middle + 1;
    }
    if (low == count)
        return std::nullopt;
    EncodedRange<Number> range = rangeAt(low);
    if (range.start > site)
        return std::nullopt;
    HandlerType type = HandlerType::Catch;
    if (hasTypes) {
        const uint8_t* types = ranges + count * sizeof(range);
        type = static_cast<HandlerType>(types[low / typesPerByte] >> bitsPerType * (low % typesPerByte) & typeMask);
    }
    return HandlerRecord { range.target, type };
}

std::optional<HandlerRecord> findHandler(std::span<const uint8_t> handlers, CallSiteIndex site)
{
    if (handlers.empty())
        return std::nullopt;
    const uint8_t* cursor = handlers.data();
    uint8_t head = *cursor++;
    size_t count = head >> countShift;
    if (!count)
        count = static_cast<size_t>(readVarint(cursor));
    bool hasTypes = head & hasTypesMask;
    unsigned widthCode = head & widthCodeMask;
    if (!widthCode)
        return findHandlerInRanges<uint8_t>(cursor, count, hasTypes, site.bits());
    if (widthCode == 1)
        return findHandlerInRanges<uint16_t>(cursor, count, hasTypes, site.bits());
    return findHandlerInRanges<uint32_t>(cursor, count, hasTypes, site.bits());
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

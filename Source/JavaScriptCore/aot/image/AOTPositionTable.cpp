/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPositionTable.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTCompiledCode.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTVarint.h"
#include <limits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr uint64_t maxLineWord = zigZagEncode(std::numeric_limits<uint32_t>::max()) << 2 | 2;
static constexpr size_t maxPositionSize = sizeOfVarint(maxLineWord) + 2 * sizeOfVarint(std::numeric_limits<uint32_t>::max());
static_assert(sitesBetweenPositionCheckpoints * maxPositionSize <= std::numeric_limits<uint16_t>::max(), "a checkpoint stores the size of a run of positions in 16 bits");

namespace {

struct PositionState {
    int64_t line { 0 };
    int64_t column { 0 };
    uint32_t source { 0 };
};

class PositionEncoder {
public:
    explicit PositionEncoder(Vector<uint8_t>& stream)
        : m_stream(stream)
    {
    }

    void append(LineColumn position, uint32_t source)
    {
        if (position.line == m_line && source == m_source)
            appendVarint(m_stream, zigZagEncode(static_cast<int64_t>(position.column) - m_column) << 1 | 1);
        else {
            appendVarint(m_stream, zigZagEncode(static_cast<int64_t>(position.line) - m_line) << 2 | (source != m_source) << 1);
            if (source != m_source)
                appendVarint(m_stream, source);
            appendVarint(m_stream, position.column);
        }
        m_line = position.line;
        m_column = position.column;
        m_source = source;
    }

    PositionState state() const { return { m_line, m_column, m_source }; }
    void setState(const PositionState& state)
    {
        m_line = state.line;
        m_column = state.column;
        m_source = state.source;
    }

private:
    Vector<uint8_t>& m_stream;
    int64_t m_line { 0 };
    int64_t m_column { 0 };
    uint32_t m_source { 0 };
};

} // anonymous namespace

static uint32_t sourceNumber(const SitePosition& position)
{
    return position.source ? position.source.index() + 1 : 0;
}

void encodePositions(Vector<uint8_t>& stream, const SiteDescription& sites)
{
    RELEASE_ASSERT(!sites.positions.isEmpty());
    size_t count = sites.positions.size();
    size_t numberOfInlineFrames = sites.inlineFrames.isEmpty() ? 0 : sites.inlineFrames.size() - 1;
    size_t numberOfSitesWithoutCode = sites.numberOfSitesWithoutCode;
    RELEASE_ASSERT(sites.spreadSites.size() < count);
    size_t lastSiteWithCode = count - 1 - sites.spreadSites.size();
    RELEASE_ASSERT(numberOfInlineFrames <= numberOfSitesWithoutCode && numberOfSitesWithoutCode <= lastSiteWithCode);
    size_t numberOfSuspensionSites = numberOfSitesWithoutCode - numberOfInlineFrames;
    bool followsInlineFrames = sites.hasEncodedInlineFrames();
    // Has to match SiteFrames::next(), the decoder's only way to learn the frame of a site.
    auto stateFrame = [&](size_t site) -> uint32_t {
        if (!followsInlineFrames || site <= numberOfSuspensionSites || site > lastSiteWithCode)
            return 0;
        if (site <= numberOfSitesWithoutCode)
            return sites.inlineFrames[site - numberOfSuspensionSites].parent;
        return sites.positions[site].inlineFrame;
    };

    PositionState start { sites.positions[0].line, sites.positions[0].column, sourceNumber(sites.positions[0]) };
    appendVarint(stream, sites.positions[0].line);
    appendVarint(stream, sites.positions[0].column);
    appendVarint(stream, static_cast<uint64_t>(count) << 1 | followsInlineFrames);
    appendVarint(stream, start.source);
    Vector<uint8_t> positions;
    size_t lastRunEnd = 0;
    PositionEncoder encoder(positions);
    Vector<PositionState, 8> states;
    states.fill(start, followsInlineFrames ? numberOfInlineFrames + 1 : 1);
    for (size_t site = 1; site < count; ++site) {
        if (!(site % sitesBetweenPositionCheckpoints)) {
            size_t bytesSinceLast = positions.size() - lastRunEnd;
            ASSERT(bytesSinceLast <= sitesBetweenPositionCheckpoints * maxPositionSize);
            stream.append(static_cast<uint8_t>(bytesSinceLast));
            stream.append(static_cast<uint8_t>(bytesSinceLast >> 8));
            lastRunEnd = positions.size();
            states.fill(start);
        }
        auto& position = sites.positions[site];
        PositionState& state = states[stateFrame(site)];
        encoder.setState(state);
        encoder.append({ position.line, position.column }, sourceNumber(position));
        state = encoder.state();
    }
    stream.appendVector(positions);
}

static void readPosition(const uint8_t*& cursor, PositionState& state)
{
    uint64_t word = readVarint(cursor);
    if (word & 1)
        state.column += zigZagDecode(word >> 1);
    else {
        state.line += zigZagDecode(word >> 2);
        if (word & 2)
            state.source = static_cast<uint32_t>(readVarint(cursor));
        state.column = static_cast<int64_t>(readVarint(cursor));
    }
}

static SourcePosition sourcePositionFor(const PositionState& state)
{
    return { { static_cast<unsigned>(state.line), static_cast<unsigned>(state.column) }, state.source ? ModuleID(state.source - 1) : ModuleID() };
}

std::optional<SourcePosition> decodePosition(const Image& image, uint32_t function, const uint8_t* positions, CallSiteIndex site)
{
    const uint8_t* cursor = positions;
    PositionState start;
    start.line = static_cast<int64_t>(readVarint(cursor));
    start.column = static_cast<int64_t>(readVarint(cursor));
    uint64_t countAndFollowsInlineFrames = readVarint(cursor);
    start.source = static_cast<uint32_t>(readVarint(cursor));
    uint64_t count = countAndFollowsInlineFrames >> 1;
    bool followsInlineFrames = countAndFollowsInlineFrames & 1;

    uint64_t number = site.bits();
    if (number >= count)
        return std::nullopt;
    if (!number)
        return sourcePositionFor(start);

    constexpr uint64_t sitesBetween = sitesBetweenPositionCheckpoints;
    uint64_t numberOfCheckpoints = (count - 1) / sitesBetween;
    const uint8_t* checkpoints = cursor;
    cursor += numberOfCheckpoints * sizeof(uint16_t);
    uint64_t precedingCount = number / sitesBetween;
    for (uint64_t i = 0; i < precedingCount; ++i)
        cursor += checkpoints[2 * i] | checkpoints[2 * i + 1] << 8;
    uint64_t first = precedingCount ? precedingCount * sitesBetween : 1;

    std::optional<SiteFrames> frames;
    if (followsInlineFrames) {
        frames.emplace(image, function);
        frames->skip(first - 1);
    }
    Vector<PositionState, 8> states { start };
    unsigned frame = 0;
    for (uint64_t i = first; i <= number; ++i) {
        if (frames)
            frame = frames->next();
        while (frame >= states.size())
            states.append(start);
        readPosition(cursor, states[frame]);
    }
    return sourcePositionFor(states[frame]);
}

std::optional<SourcePosition> Image::positionOf(uint32_t function, CallSiteIndex site) const
{
    const ImageFunction& record = this->function(function);
    if (!record.has(ImageFunctionFlag::HasPositions))
        return std::nullopt;
    SideTableParts parts = SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables).data());
    return decodePosition(*this, function, parts.positions, site);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

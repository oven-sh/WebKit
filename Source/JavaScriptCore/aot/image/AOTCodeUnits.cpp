/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeUnits.h"

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "AirBasicBlock.h"
#include "AirInst.h"
#include "AirOpcodeUtils.h"
#include "B3Value.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

static constexpr uint32_t wordSize = sizeof(uint32_t);
static constexpr uint32_t maxUnitLength = std::numeric_limits<uint16_t>::max();
static constexpr uint16_t maxSplitPieceLength = maxUnitLength & ~(wordSize - 1);

namespace {

struct OffsetRange {
    uint32_t begin;
    uint32_t end;
    CodeUnit::Kind kind;
};

struct LinkedRanges {
    Vector<OffsetRange> recorded;
    Vector<OffsetRange> pins;
    Vector<OffsetRange> hotBlocks;
    Vector<OffsetRange> coldBlocksThatMayRun;
};

} // anonymous namespace

static constexpr GPRReg nearCallClobberedRegister()
{
#if CPU(X86_64)
    return MacroAssembler::stackPointerRegister;
#elif CPU(ARM64)
    return MacroAssembler::linkRegister;
#else
#error "Code units are defined for x86-64 and ARM64."
#endif
}

#if CPU(X86_64)
namespace {

struct StackPointerDisplacement {
    uint8_t position;
    uint8_t size;
};

} // anonymous namespace

// Knows the instructions below and nothing else. The answer holds only for bytes that are one whole instruction: every byte is accounted for.
static std::optional<StackPointerDisplacement> findStackPointerDisplacement(std::span<const uint8_t> bytes)
{
    size_t at = 0;
    auto next = [&]() -> std::optional<uint8_t> {
        if (at == bytes.size())
            return std::nullopt;
        return bytes[at++];
    };
    auto isInTwoByteMap = [](uint8_t opcode) {
        switch (opcode) {
        case 0x10: case 0x11: case 0x28: case 0x29: case 0x2a: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        case 0x51: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5a: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        case 0x6e: case 0x6f: case 0x7e: case 0x7f: case 0xaf: case 0xb6: case 0xb7: case 0xbe: case 0xbf: case 0xd6:
            return true;
        default:
            return false;
        }
    };

    bool hasOperandSizePrefix = false;
    std::optional<uint8_t> byte = next();
    for (; byte && (*byte == 0x66 || *byte == 0xf2 || *byte == 0xf3); byte = next())
        hasOperandSizePrefix |= *byte == 0x66;
    if (!byte)
        return std::nullopt;

    unsigned immediateSize = 0;
    if (*byte == 0xc5 || *byte == 0xc4) {
        // VEX. The bits for the index and the base are stored inverted, and the two-byte form has neither.
        if (*byte == 0xc4) {
            std::optional<uint8_t> second = next();
            if (!second || (*second & 0x7f) != 0x61)
                return std::nullopt;
        }
        if (!next())
            return std::nullopt;
        byte = next();
        if (!byte || !isInTwoByteMap(*byte))
            return std::nullopt;
    } else {
        if ((*byte & 0xf0) == 0x40) {
            if (*byte & 3)
                return std::nullopt;
            byte = next();
            if (!byte)
                return std::nullopt;
        }
        switch (*byte) {
        case 0x01: case 0x03: case 0x09: case 0x0b: case 0x21: case 0x23: case 0x29: case 0x2b: case 0x31: case 0x33: case 0x39: case 0x3b:
        case 0x63: case 0x85: case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8d:
            break;
        case 0x83: case 0xc6:
            immediateSize = 1;
            break;
        case 0x81: case 0xc7:
            immediateSize = hasOperandSizePrefix ? 2 : 4;
            break;
        case 0x0f:
            byte = next();
            if (!byte || !isInTwoByteMap(*byte))
                return std::nullopt;
            break;
        default:
            return std::nullopt;
        }
    }

    std::optional<uint8_t> modRM = next();
    std::optional<uint8_t> sib = next();
    if (!modRM || !sib || (*modRM & 7) != 4 || *sib != 0x24)
        return std::nullopt;
    unsigned displacementSize = *modRM >> 6 == 1 ? 1 : *modRM >> 6 == 2 ? 4 : 0;
    if (!displacementSize || at + displacementSize + immediateSize != bytes.size())
        return std::nullopt;
    return StackPointerDisplacement { static_cast<uint8_t>(at), static_cast<uint8_t>(displacementSize) };
}

static bool canAdjustForNearCall(std::span<const uint8_t> bytes)
{
    auto displacement = findStackPointerDisplacement(bytes);
    if (!displacement)
        return false;
    if (displacement->size == 1)
        return static_cast<int8_t>(bytes[displacement->position]) >= 0 && bytes[displacement->position] <= std::numeric_limits<int8_t>::max() - sizeof(void*);
    int32_t value;
    memcpySpan(asMutableByteSpan(value), bytes.subspan(displacement->position, sizeof(value)));
    return value >= 0 && value <= std::numeric_limits<int32_t>::max() - static_cast<int32_t>(sizeof(void*));
}
#endif

PackedCodeUnits::PackedCodeUnits(std::span<const CodeUnit> units)
{
    size_t numberOfBytes = 0;
    for (const CodeUnit& unit : units)
        numberOfBytes += unit.length && unit.length <= shortLengthMask ? 1 : 3;
    m_bytes.reserveInitialCapacity(numberOfBytes);
    for (const CodeUnit& unit : units)
        append(unit);
}

void PackedCodeUnits::append(const CodeUnit& unit)
{
    static_assert(static_cast<uint8_t>(CodeUnit::Kind::End) <= kindMask);
    RELEASE_ASSERT(unit.offset == m_codeSize);
    bool isShort = unit.length && unit.length <= shortLengthMask;
    m_bytes.append(static_cast<uint8_t>((isShort ? unit.length : 0) | static_cast<uint8_t>(unit.kind) << kindShift | (unit.mayRun ? mayRunBit : 0)));
    if (!isShort) {
        m_bytes.append(static_cast<uint8_t>(unit.length));
        m_bytes.append(static_cast<uint8_t>(unit.length >> 8));
    }
    ++m_size;
    m_codeSize = safeCast<uint32_t>(static_cast<uint64_t>(m_codeSize) + unit.length);
}

void PackedCodeUnits::prepend(std::span<const CodeUnit> units)
{
    PackedCodeUnits result(units);
    result.m_bytes.append(m_bytes.span());
    result.m_size += m_size;
    result.m_codeSize = safeCast<uint32_t>(static_cast<uint64_t>(result.m_codeSize) + m_codeSize);
    *this = WTF::move(result);
}

void PackedCodeUnits::shrinkCodeSize(uint32_t newCodeSize)
{
    Iterator unit = begin();
    uint32_t numberOfKeptBytes = 0;
    uint32_t numberOfKeptUnits = 0;
    for (; unit != end() && (*unit).offset + (*unit).length <= newCodeSize; ++unit) {
        numberOfKeptBytes += (*unit).length && (*unit).length <= shortLengthMask ? 1 : 3;
        ++numberOfKeptUnits;
    }
    if (unit == end())
        return;
    CodeUnit cut = *unit;
    m_bytes.shrink(numberOfKeptBytes);
    m_size = numberOfKeptUnits;
    m_codeSize = cut.offset;
    if (cut.offset < newCodeSize) {
        RELEASE_ASSERT(cut.kind == CodeUnit::Kind::Pinned);
        cut.length = safeCast<uint16_t>(newCodeSize - cut.offset);
        append(cut);
    }
    m_bytes.shrinkToFit();
}

CodeUnit PackedCodeUnits::last() const
{
    RELEASE_ASSERT(m_size);
    Iterator unit = begin();
    for (uint32_t index = 1; index < m_size; ++index)
        ++unit;
    return *unit;
}

Vector<CodeUnit> PackedCodeUnits::unpack() const
{
    Vector<CodeUnit> units;
    units.reserveInitialCapacity(m_size);
    for (CodeUnit unit : *this)
        units.append(unit);
    return units;
}

PackedCodeUnits::Iterator adjustForNearCall(std::span<uint8_t> bytes, PackedCodeUnits::Iterator from, PackedCodeUnits::Iterator end, uint32_t firstOffset)
{
#if CPU(X86_64)
    while (from != end && (*from).offset < firstOffset)
        ++from;
    for (auto position = from; position != end; ++position) {
        CodeUnit unit = *position;
        if (unit.offset >= firstOffset + bytes.size())
            break;
        if (unit.kind != CodeUnit::Kind::PlainOffStackPointer)
            continue;
        std::span<uint8_t> instruction = bytes.subspan(unit.offset - firstOffset, unit.length);
        RELEASE_ASSERT(canAdjustForNearCall(instruction));
        auto displacement = *findStackPointerDisplacement(instruction);
        if (displacement.size == 1) {
            instruction[displacement.position] += sizeof(void*);
            continue;
        }
        int32_t value;
        memcpySpan(asMutableByteSpan(value), instruction.subspan(displacement.position, sizeof(value)));
        value += sizeof(void*);
        memcpySpan(instruction.subspan(displacement.position, sizeof(value)), asByteSpan(value));
    }
#else
    UNUSED_PARAM(bytes);
    UNUSED_PARAM(end);
    UNUSED_PARAM(firstOffset);
#endif
    return from;
}

static CodeUnit::Kind kindOf(const B3::Air::Inst& inst)
{
    B3::Air::Opcode opcode = inst.kind.opcode;
    if (B3::Air::isReturn(opcode))
        return CodeUnit::Kind::End;
    switch (opcode) {
    case B3::Air::Oops:
        return CodeUnit::Kind::End;
    case B3::Air::Patch:
    case B3::Air::ColdCCall:
    case B3::Air::Shuffle:
    case B3::Air::EntrySwitch:
    case B3::Air::WasmBoundsCheck:
    case B3::Air::EnterFrame:
    case B3::Air::LeaveFrame:
        return CodeUnit::Kind::Pinned;
    default:
        break;
    }
    if (inst.kind.effects)
        return CodeUnit::Kind::Pinned;
    B3::Air::Tmp clobbered(nearCallClobberedRegister());
    CodeUnit::Kind kind = CodeUnit::Kind::Plain;
    for (const B3::Air::Arg& arg : inst.args()) {
        if (!arg.usesTmp(clobbered))
            continue;
        // link() looks at the bytes.
        if (!isX86_64() || !arg.isAddr() || kind != CodeUnit::Kind::Plain)
            return CodeUnit::Kind::Pinned;
        kind = CodeUnit::Kind::PlainOffStackPointer;
    }
    return kind;
}

// A fenced access on x86-64 has kind.effects too, and the address computation of a trapping access has the access as its origin.
static bool isTrappingAccess(const B3::Air::Inst& inst)
{
    return inst.kind.effects && inst.origin && inst.origin->traps();
}

void CodeUnitRecorder::setColdBlocks(BitVector&& coldAirBlocks, BitVector&& rarelyRunAirBlocks)
{
    m_coldAirBlocks = WTF::move(coldAirBlocks);
    m_rarelyRunAirBlocks = WTF::move(rarelyRunAirBlocks);
}

void CodeUnitRecorder::willGenerate(const B3::Air::BasicBlock& block)
{
    willGenerateBlock(m_coldAirBlocks.get(block.index()), m_rarelyRunAirBlocks.get(block.index()));
}

void CodeUnitRecorder::willGenerateBlock(bool isCold, bool isRarelyRun)
{
    if (!isCold)
        m_blockState = BlockState::StartsHotBlock;
    else
        m_blockState = isRarelyRun ? BlockState::InColdBlock : BlockState::StartsColdBlockThatMayRun;
}

void CodeUnitRecorder::addRange(MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind kind)
{
    if (kind == CodeUnit::Kind::Pinned) {
        m_boundaries.append(begin);
        return;
    }
    m_ranges.append({ begin, end, kind });
}

void CodeUnitRecorder::didGenerate(const B3::Air::Inst& inst, MacroAssembler::Label begin, MacroAssembler::Label end)
{
    if (begin == end)
        return;

    if (isTrappingAccess(inst))
        m_traps.append({ begin, inst.origin ? inst.origin->origin() : B3::Origin() });
    didGenerateInBlock(begin, end, kindOf(inst));
}

void CodeUnitRecorder::didGenerateInBlock(MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind kind)
{
    if (begin == end)
        return;

    // All of a hot block is pinned, whatever its instructions are.
    switch (m_blockState) {
    case BlockState::NoBlock:
    case BlockState::StartsHotBlock:
        m_hotBlockRanges.append({ begin, end });
        m_blockState = BlockState::InHotBlock;
        return;
    case BlockState::InHotBlock:
        m_hotBlockRanges.last().second = end;
        return;
    case BlockState::InColdBlock:
        break;
    case BlockState::StartsColdBlockThatMayRun:
        m_rangesOfColdBlocksThatMayRun.append({ begin, end });
        m_blockState = BlockState::InColdBlockThatMayRun;
        break;
    case BlockState::InColdBlockThatMayRun:
        m_rangesOfColdBlocksThatMayRun.last().second = end;
        break;
    }

    addRange(begin, end, kind);
}

void CodeUnitRecorder::addPlain(MacroAssembler::Label begin, MacroAssembler::Label end, const RegisterSet& named)
{
    addRange(begin, end, named.contains(nearCallClobberedRegister(), IgnoreVectors) ? CodeUnit::Kind::Pinned : CodeUnit::Kind::Plain);
}

void CodeUnitRecorder::addEnd(MacroAssembler::Label begin, MacroAssembler::Label end)
{
    addRange(begin, end, CodeUnit::Kind::End);
}

void CodeUnitRecorder::pin(MacroAssembler::Label begin, MacroAssembler::Label end)
{
    m_pins.append({ begin, end });
}

static CodeUnit exactUnitAt(uint32_t offset, RelocationKind relocationKind, CodeUnit::Kind kind)
{
    return { offset, safeCast<uint16_t>(MacroAssembler::relocationSize(relocationKind)), kind };
}

static Vector<CodeUnit> collectExactUnits(std::span<const Relocation> relocations, std::span<const LinkedSite> linkedSites, uint32_t size)
{
    Vector<CodeUnit> units;
    units.reserveInitialCapacity(relocations.size() + linkedSites.size());
    size_t nextRelocation = 0;
    size_t nextLinkedSite = 0;
    while (nextRelocation < relocations.size() || nextLinkedSite < linkedSites.size()) {
        bool relocationIsNext = nextLinkedSite == linkedSites.size()
            || (nextRelocation < relocations.size() && relocations[nextRelocation].offset < linkedSites[nextLinkedSite].offset);
        if (relocationIsNext) {
            const Relocation& relocation = relocations[nextRelocation++];
            units.append(exactUnitAt(relocation.offset, relocation.kind, relocation.kind == RelocationKind::Call ? CodeUnit::Kind::Call : CodeUnit::Kind::Pinned));
            continue;
        }
        const LinkedSite& site = linkedSites[nextLinkedSite++];
        units.append(exactUnitAt(site.offset, site.kind, CodeUnit::Kind::Pinned));
    }

    uint64_t end = 0;
    for (const CodeUnit& unit : units) {
        RELEASE_ASSERT(unit.length && unit.offset >= end);
        end = static_cast<uint64_t>(unit.offset) + unit.length;
        RELEASE_ASSERT(end <= size);
    }
    return units;
}

static void appendPieces(Vector<CodeUnit>& units, uint32_t begin, uint32_t end, CodeUnit::Kind kind, bool mayRun)
{
    uint32_t length = end - begin;
    if (kind == CodeUnit::Kind::Pinned && !units.isEmpty() && units.last().kind == CodeUnit::Kind::Pinned && units.last().length + length <= maxUnitLength) {
        units.last().length = static_cast<uint16_t>(units.last().length + length);
        return;
    }
    if (length <= maxUnitLength) [[likely]] {
        units.append(CodeUnit { begin, static_cast<uint16_t>(length), kind, mayRun && kind != CodeUnit::Kind::Pinned });
        return;
    }
    // No emitter recorded these cuts, so on x86-64 one may fall inside an instruction: no piece may be outlined on its own.
    for (; end - begin > maxSplitPieceLength; begin += maxSplitPieceLength)
        units.append(CodeUnit { begin, maxSplitPieceLength, CodeUnit::Kind::Pinned });
    units.append(CodeUnit { begin, static_cast<uint16_t>(end - begin), kind == CodeUnit::Kind::End ? kind : CodeUnit::Kind::Pinned });
}

static const OffsetRange* findRangeCovering(std::span<const OffsetRange> ranges, size_t& cursor, uint32_t offset)
{
    while (cursor < ranges.size() && ranges[cursor].end <= offset)
        ++cursor;
    return cursor < ranges.size() && ranges[cursor].begin <= offset ? &ranges[cursor] : nullptr;
}

static Vector<CodeUnit> tileCode(std::span<const uint32_t> cuts, std::span<const CodeUnit> exactUnits, const LinkedRanges& ranges, std::span<const uint8_t> bytes)
{
    Vector<CodeUnit> units;
    units.reserveInitialCapacity(cuts.size());
    size_t nextExactUnit = 0;
    size_t rangeCursor = 0;
    size_t pinCursor = 0;
    size_t hotBlockCursor = 0;
    size_t mayRunCursor = 0;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        uint32_t begin = cuts[i];
        uint32_t end = cuts[i + 1];
        const OffsetRange* range = findRangeCovering(ranges.recorded.span(), rangeCursor, begin);
        bool isHot = !!findRangeCovering(ranges.hotBlocks.span(), hotBlockCursor, begin);
        bool isPinnedByClient = !!findRangeCovering(ranges.pins.span(), pinCursor, begin);
        bool mayRun = !!findRangeCovering(ranges.coldBlocksThatMayRun.span(), mayRunCursor, begin);

        CodeUnit::Kind kind = CodeUnit::Kind::Pinned;
        if (nextExactUnit < exactUnits.size() && exactUnits[nextExactUnit].offset == begin) {
            const CodeUnit& unit = exactUnits[nextExactUnit++];
            RELEASE_ASSERT(unit.offset + unit.length == end);
            RELEASE_ASSERT(!range || unit.kind != CodeUnit::Kind::Call);
            kind = unit.kind;
        } else if (range && range->kind == CodeUnit::Kind::End && range->end == end)
            kind = CodeUnit::Kind::End;
        else if (range && range->kind == CodeUnit::Kind::Plain)
            kind = CodeUnit::Kind::Plain;
#if CPU(X86_64)
        else if (range && range->kind == CodeUnit::Kind::PlainOffStackPointer && !isHot && range->begin == begin && range->end == end && canAdjustForNearCall(bytes.subspan(begin, end - begin)))
            kind = CodeUnit::Kind::PlainOffStackPointer;
#endif
        appendPieces(units, begin, end, isHot || isPinnedByClient ? CodeUnit::Kind::Pinned : kind, mayRun);
    }
    RELEASE_ASSERT(nextExactUnit == exactUnits.size());
    return units;
}

static bool ascendStrictly(const auto& records, auto offset)
{
    return std::ranges::adjacent_find(records, std::ranges::greater_equal { }, offset) == records.end();
}

Vector<CodeUnit> CodeUnitRecorder::link(ImageLinkBuffer& buffer, std::span<const Relocation> relocations)
{
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    auto appendLinkedRange = [&](Vector<OffsetRange>& result, MacroAssembler::Label begin, MacroAssembler::Label end, CodeUnit::Kind kind) {
        OffsetRange range { linkBuffer.offsetOf(begin), linkBuffer.offsetOf(end), kind };
        RELEASE_ASSERT(range.begin <= range.end);
        if (range.begin != range.end)
            result.append(range);
    };

    LinkedRanges ranges;
    ranges.recorded.reserveInitialCapacity(m_ranges.size());
    // An instruction mostly begins where the one in front of it ends.
    MacroAssembler::Label lastEnd;
    uint32_t offsetOfLastEnd = 0;
    bool ascends = true;
    for (auto& range : m_ranges) {
        uint32_t begin = range.begin == lastEnd ? offsetOfLastEnd : linkBuffer.offsetOf(range.begin);
        ascends &= begin >= offsetOfLastEnd;
        lastEnd = range.end;
        offsetOfLastEnd = linkBuffer.offsetOf(range.end);
        RELEASE_ASSERT(begin <= offsetOfLastEnd);
        if (begin != offsetOfLastEnd)
            ranges.recorded.append(OffsetRange { begin, offsetOfLastEnd, range.kind });
    }
    if (!ascends) {
        AOT::sort(ranges.recorded, { }, &OffsetRange::begin);
        for (size_t i = 1; i < ranges.recorded.size(); ++i)
            RELEASE_ASSERT(ranges.recorded[i - 1].end <= ranges.recorded[i].begin);
    }

    for (auto& [begin, end] : m_pins)
        appendLinkedRange(ranges.pins, begin, end, CodeUnit::Kind::Pinned);
    if (!ascendStrictly(ranges.pins, &OffsetRange::begin))
        AOT::sort(ranges.pins, { }, &OffsetRange::begin);

    ranges.hotBlocks.reserveInitialCapacity(m_hotBlockRanges.size());
    for (auto& [begin, end] : m_hotBlockRanges)
        appendLinkedRange(ranges.hotBlocks, begin, end, CodeUnit::Kind::Pinned);
    for (size_t i = 1; i < ranges.hotBlocks.size(); ++i)
        RELEASE_ASSERT(ranges.hotBlocks[i - 1].end <= ranges.hotBlocks[i].begin);
    for (auto& [begin, end] : m_rangesOfColdBlocksThatMayRun)
        appendLinkedRange(ranges.coldBlocksThatMayRun, begin, end, CodeUnit::Kind::Plain);
    m_sizeOfHotBlocks = 0;
    for (auto& range : ranges.hotBlocks)
        m_sizeOfHotBlocks += range.end - range.begin;

    // Not linkBuffer.size(): LinkBuffer fills the storage that jump shortening and branch compaction leave over with nops.
    uint32_t size = safeCast<uint32_t>(buffer.bytes().size());
    const Vector<LinkedSite>& linkedSites = linkBuffer.linkedSites();
    Vector<CodeUnit> exactUnits = collectExactUnits(relocations, linkedSites.span(), size);

    BitVector isCut(static_cast<size_t>(size) + 1);
    auto addCut = [&](uint32_t offset) {
        RELEASE_ASSERT(offset <= size);
        isCut.quickSet(offset);
    };
    addCut(0);
    addCut(size);
    for (auto& boundary : m_boundaries)
        addCut(linkBuffer.offsetOf(boundary));
    for (auto* list : { &ranges.recorded, &ranges.pins }) {
        for (auto& range : *list) {
            addCut(range.begin);
            addCut(range.end);
        }
    }
    for (const CodeUnit& unit : exactUnits) {
        addCut(unit.offset);
        addCut(unit.offset + unit.length);
    }
    for (const LinkedSite& site : linkedSites)
        addCut(site.target);

    Vector<uint32_t> cuts;
    cuts.reserveInitialCapacity(isCut.bitCount());
    isCut.forEachSetBit([&](size_t offset) {
        cuts.append(static_cast<uint32_t>(offset));
    });

    return tileCode(cuts.span(), exactUnits.span(), ranges, buffer.bytes());
}

static bool unitsTileCode(std::span<const CodeUnit> units, size_t size)
{
    uint64_t end = 0;
    for (const CodeUnit& unit : units) {
        if (unit.offset != end || !unit.length)
            return false;
        end += unit.length;
    }
    return end == size;
}

static const CodeUnit* findUnitStartingAt(std::span<const CodeUnit> units, uint32_t offset)
{
    auto found = std::ranges::lower_bound(units, offset, { }, &CodeUnit::offset);
    return found != units.end() && found->offset == offset ? &*found : nullptr;
}

static bool isUnitBoundary(std::span<const CodeUnit> units, size_t size, uint32_t offset)
{
    return offset == size || findUnitStartingAt(units, offset);
}

static bool liesInPinnedUnits(std::span<const CodeUnit> units, uint32_t begin, uint32_t end)
{
    if (begin == end)
        return true;
    auto unit = std::ranges::upper_bound(units, begin, { }, &CodeUnit::offset);
    RELEASE_ASSERT(unit != units.begin());
    for (--unit; unit != units.end() && unit->offset < end; ++unit) {
        if (unit->kind != CodeUnit::Kind::Pinned)
            return false;
    }
    return true;
}

namespace {

class CodeOffsetVerifier {
public:
    CodeOffsetVerifier(std::span<const CodeUnit> units, size_t codeSize)
        : m_units(units)
        , m_codeSize(codeSize)
    {
    }

    void visit(uint32_t offset, CodeOffsetRole role, unsigned size)
    {
        if (m_error == CodeUnitsError::None)
            m_error = verify(offset, role, size);
    }

    CodeUnitsError finish() const
    {
        if (m_error != CodeUnitsError::None)
            return m_error;
        size_t numberOfCallUnits = static_cast<size_t>(std::ranges::count(m_units, CodeUnit::Kind::Call, &CodeUnit::kind));
        return m_numberOfCallUnitsVisited == numberOfCallUnits ? CodeUnitsError::None : CodeUnitsError::CallIsNotAUnit;
    }

private:
    bool splitsUnitInWindow(uint32_t offset) const
    {
        return !isUnitBoundary(m_units, m_codeSize, offset) && !liesInPinnedUnits(m_units, offset, offset + 1);
    }

    CodeUnitsError verifyCall(uint32_t offset, unsigned size)
    {
        const CodeUnit* unit = findUnitStartingAt(m_units, offset);
        if (unit && unit->kind == CodeUnit::Kind::Call && unit->length == size) {
            ++m_numberOfCallUnitsVisited;
            return CodeUnitsError::None;
        }
        return liesInPinnedUnits(m_units, offset, offset + size) ? CodeUnitsError::None : CodeUnitsError::CallIsNotAUnit;
    }

    CodeUnitsError verify(uint32_t offset, CodeOffsetRole role, unsigned size)
    {
        if (static_cast<uint64_t>(offset) + size > m_codeSize)
            return CodeUnitsError::OffsetOutsideCode;
        switch (role) {
        case CodeOffsetRole::Call:
            return verifyCall(offset, size);
        case CodeOffsetRole::Site:
            return liesInPinnedUnits(m_units, offset, offset + size) ? CodeUnitsError::None : CodeUnitsError::SiteIsNotPinned;
        case CodeOffsetRole::Target:
            return splitsUnitInWindow(offset) ? CodeUnitsError::TargetIsNotABoundary : CodeUnitsError::None;
        case CodeOffsetRole::ReturnAddress:
            return splitsUnitInWindow(offset) ? CodeUnitsError::ReturnAddressIsNotABoundary : CodeUnitsError::None;
        case CodeOffsetRole::Boundary:
            return CodeUnitsError::None;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::span<const CodeUnit> m_units;
    size_t m_codeSize;
    size_t m_numberOfCallUnitsVisited { 0 };
    CodeUnitsError m_error { CodeUnitsError::None };
};

} // anonymous namespace

CodeUnitsError verifyCodeUnits(const CompiledCode& code)
{
    const CodeBody& body = code.body;
    if (body.units.isEmpty())
        return CodeUnitsError::None;
    Vector<CodeUnit> unpackedUnits = body.units.unpack();
    std::span<const CodeUnit> units = unpackedUnits.span();
    if (!unitsTileCode(units, body.bytes.size()))
        return CodeUnitsError::UnitsDoNotTileCode;
    if (!ascendStrictly(body.relocations, &Relocation::offset) || !ascendStrictly(body.localSites, &LinkedSite::offset) || !ascendStrictly(body.returnSites, &ReturnSite::offset))
        return CodeUnitsError::OffsetsDoNotAscend;

    CodeOffsetVerifier verifier(units, body.bytes.size());
    forEachCodeOffset(code, [&](uint32_t offset, CodeOffsetRole role, unsigned size) {
        verifier.visit(offset, role, size);
    });
    return verifier.finish();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

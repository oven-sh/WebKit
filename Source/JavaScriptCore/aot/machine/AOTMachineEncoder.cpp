/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTMachineDescription.h"
#include "AOTMachineEncoderPairsOfLoads.h"
#include "AOTMachineFrameForms.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "DisallowMacroScratchRegisterUsage.h"
#include <algorithm>
#include <bit>

namespace JSC { namespace AOT { namespace Machine {

static NEVER_INLINE void encodeArgumentRegister(Encoder& encoder, const Step& step)
{
    RELEASE_ASSERT(step.destination.isRegister());
    Reg source = step.operation->argumentReg();
    Reg destination = step.destination.reg();
    if (source == destination)
        return;
    if (source.isGPR())
        encoder.m_jit.move(source.gpr(), destination.gpr());
    else
        encoder.m_jit.moveDouble(source.fpr(), destination.fpr());
}

static NEVER_INLINE void encodeOpaqueNumber(Encoder& encoder, const Step& step)
{
    RELEASE_ASSERT(step.sources[0].isRegister() && step.destination.isRegister());
    FPRReg source = step.sources[0].fpr();
    FPRReg destination = step.destination.fpr();
    if (source != destination)
        encoder.m_jit.moveDouble(source, destination);
}

static void noteStartOfRareCode(CCallHelpers& jit)
{
#if CPU(X86_64)
    jit.m_assembler.setStartOfRareCode(safeCast<uint32_t>(jit.m_assembler.codeSize()));
#else
    UNUSED_PARAM(jit);
#endif
}

static CodeOfBackEnd codeOfBackEndIn(StepKind kind)
{
    switch (kind) {
    case StepKind::Move:
        return CodeOfBackEnd::Move;
    case StepKind::Reload:
        return CodeOfBackEnd::Reload;
    case StepKind::Save:
        return CodeOfBackEnd::Save;
    case StepKind::Materialize:
        return CodeOfBackEnd::Materialize;
    case StepKind::Swap:
        return CodeOfBackEnd::Swap;
    case StepKind::KeepPointer:
        return CodeOfBackEnd::KeepPointer;
    case StepKind::Execute:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Origin originOfStepOfAllocator(const Encoder& encoder, uint32_t stepIndex)
{
    ASSERT(encoder.m_notesRangesOfOrigins);
    const Allocation& allocation = encoder.m_allocation;
    const BlockSteps& steps = allocation.stepsOf(encoder.m_block);
    const Step& step = allocation.step(stepIndex);
    CodeOfBackEnd code = codeOfBackEndIn(step.kind);
    if (step.kind == StepKind::Save && step.destination.kind() == Location::Outgoing)
        code = CodeOfBackEnd::StoreToOutgoingWord;
    if (step.kind == StepKind::Reload && step.sources[0].kind() == Location::Outgoing)
        code = CodeOfBackEnd::LoadFromOutgoingWord;
    if (stepIndex >= steps.beginOfEdgeMoves && stepIndex < steps.endOfEdgeMoves)
        return originOf(code, PlaceOfCodeOfBackEnd::Edge);

    uint32_t next = stepIndex + 1;
    while (next < steps.end && allocation.step(next).kind != StepKind::Execute)
        ++next;
    bool endsBlock = next >= steps.end;
    switch (allocation.step(endsBlock ? steps.terminator : next).operation->opcode()) {
    case Patch:
        return originOf(code, endsBlock ? PlaceOfCodeOfBackEnd::LastPatch : PlaceOfCodeOfBackEnd::Patch);
    case Check:
        return originOf(code, PlaceOfCodeOfBackEnd::Check);
    case Jump:
        return originOf(code, PlaceOfCodeOfBackEnd::Edge);
    case Branch:
    case Switch:
        return originOf(code, PlaceOfCodeOfBackEnd::Branch);
    case Return:
        return originOf(code, PlaceOfCodeOfBackEnd::Return);
    default:
        return originOf(code, PlaceOfCodeOfBackEnd::Operation);
    }
}

static Origin originOfFrameOrStore(const Encoder& encoder, CodeOfBackEnd code)
{
    return encoder.m_notesRangesOfOrigins ? originOf(code, PlaceOfCodeOfBackEnd::Nowhere) : Origin();
}

Encoder::Encoder(Procedure& proc, CCallHelpers& jit, RelocationRecorder& relocations)
    : m_proc(proc)
    , m_arena(proc.arena())
    , m_jit(jit)
    , m_relocations(relocations)
    , m_order(proc.order())
    , m_liveness(proc.liveness())
    , m_allocation(proc.allocation())
    , m_frame(proc.frame())
    , m_coverage(proc.frame().coverage())
    , m_notesRangesOfOrigins(proc.traits().notesRangesOfOrigins)
    , m_hasSpills(!proc.allocation().spills().empty())
    , m_hasFramedPart(proc.allocation().hasFramedPart())
    , m_places(proc.order().places())
    , m_stepsOfBlocks(proc.allocation().stepsOfBlocks())
    , m_steps(proc.allocation().steps().data())
    , m_spillsOfValues(proc.allocation().spillsOfValues())
    , m_valueFacts(proc.liveness().valueFacts())
    , m_labels(proc.arena().createSpan<MacroAssembler::Label>(proc.numberOfBlocks()))
    , m_blocksEntered(proc.arena().createSpan<const Block*>(proc.numberOfBlocks()))
    , m_nextLayoutIndexWithCode(WTF::unsafeMakeSpan(proc.arena().allocateArray<uint32_t>(proc.order().blocksInLayout().size() + 1), proc.order().blocksInLayout().size() + 1))
{
}

static bool blockThatJumpsOnlyJumpsOn(const Encoder& encoder, const Block* block)
{
    ASSERT(block->last()->opcode() == Jump);

    const Allocation& allocation = encoder.m_allocation;
    const BlockSteps& steps = encoder.stepsOf(block);
    if (steps.beginOfEdgeMoves != steps.endOfEdgeMoves)
        return false;
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        const Step& step = allocation.step(index);
        if (step.kind != StepKind::Execute)
            return false;
        switch (step.operation->opcode()) {
        case Nop:
        case Phi:
        case Upsilon:
            break;
        default:
            return false;
        }
        if (allocation.isSpilled(step.operation))
            return false;
    }
    if (block->isEntry() || block->isLandingPad())
        return false;
    const BlockPlace& place = encoder.placeOf(block);
    FrameAtBlock frame = encoder.frameAtBlock(block, place);
    if (frame.isSetUpAtHead || frame.isTornDownAtEnd || frame.returnsInPlaceOfJump)
        return false;
    if (encoder.m_hasFramedPart && allocation.isHeadOfFramedPart(block) && allocation.headOfFramedPartAbove(block).beginOfSteps != allocation.headOfFramedPartAbove(block).endOfSteps)
        return false;
    if (!encoder.m_hasSpills)
        return true;

    for (const Operation* phi : block->phis()) {
        if (allocation.isSpilled(phi) && !encoder.m_liveness.isDropped(phi))
            return false;
    }
    for (const Block* predecessor : place.predecessors) {
        const Operation* last = predecessor->last();
        if (last->opcode() == Patch && predecessor->successor(0).block() == block && allocation.isSpilled(last))
            return false;
    }
    return true;
}

static bool isUnlikelyInLayout(const BlockPlace& place) { return place.isRare() || place.layoutPart == LayoutPart::Rare; }

static bool blockOnlyMovesAndJumpsOn(const Encoder& encoder, const Block* block)
{
    ASSERT(block->last()->opcode() == Jump);

    const BlockSteps& steps = encoder.stepsOf(block);
    if (steps.beginOfEdgeMoves == steps.endOfEdgeMoves)
        return false;
    const BlockPlace& place = encoder.placeOf(block);
    if (block->isEntry() || block->isLandingPad() || !block->phis().empty())
        return false;
    const Allocation& allocation = encoder.m_allocation;
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        const Step& step = allocation.step(index);
        if (step.kind != StepKind::Execute)
            return false;
        switch (step.operation->opcode()) {
        case Nop:
        case Upsilon:
            break;
        default:
            return false;
        }
        if (allocation.isSpilled(step.operation))
            return false;
    }
    for (uint32_t index = steps.beginOfEdgeMoves; index < steps.endOfEdgeMoves; ++index) {
        switch (allocation.step(index).kind) {
        case StepKind::Move:
        case StepKind::Reload:
        case StepKind::Save:
        case StepKind::Materialize:
            break;
        default:
            return false;
        }
    }
    FrameAtBlock frame = encoder.frameAtBlock(block, place);
    if (frame.isSetUpAtHead || frame.returnsInPlaceOfJump)
        return false;
    return !(encoder.m_hasFramedPart && allocation.isHeadOfFramedPart(block));
}

void Encoder::findBlocksWithoutCode()
{
    UncheckedSpan<Block* const> layout = m_order.blocksInLayout();
    const uint32_t numberOfBlocksInLayout = static_cast<uint32_t>(layout.size());
    const bool someBlockMayRunWithoutFrame = m_coverage != FrameCoverage::WholeFunction;
    UncheckedSpan<const Block*> blocksEntered = m_blocksEntered;
    UncheckedSpan<uint32_t> nextLayoutIndexWithCode = m_nextLayoutIndexWithCode;
    ArenaVector<uint32_t> layoutIndexesOfBlocksThatOnlyJumpOn;
    ArenaVector<uint32_t> layoutIndexesOfMoveBlocks;
    for (uint32_t index = 0; index < numberOfBlocksInLayout; ++index) {
        const Block* block = layout[index];
        const Block* entered = block;
        const Operation* last = block->last();
        Opcode opcode = last->opcode();
        if (opcode == Jump) {
            if (blockThatJumpsOnlyJumpsOn(*this, block)) {
                entered = block->successor(0).block();
                layoutIndexesOfBlocksThatOnlyJumpOn.append(m_arena, index);
            } else if (blockOnlyMovesAndJumpsOn(*this, block))
                layoutIndexesOfMoveBlocks.append(m_arena, index);
        } else if (opcode == Patch) {
            if (m_hasSpills && block->numSuccessors() && m_allocation.isSpilled(last)) {
                if (!m_blocksThatStoreResultOfPatchAtHead.size())
                    m_blocksThatStoreResultOfPatchAtHead = BitSpan::create(m_arena, m_proc.numberOfBlocks());
                m_blocksThatStoreResultOfPatchAtHead.set(block->successor(0).block()->index());
            }
        } else if (opcode == Return && someBlockMayRunWithoutFrame && !block->isEntry() && !block->isLandingPad() && !blockRunsInFrame(block) && plannedBlockOnlyReturns(m_proc, block)) {
            if (!m_blocksThatOnlyReturnWithoutFrame.size())
                m_blocksThatOnlyReturnWithoutFrame = BitSpan::create(m_arena, m_proc.numberOfBlocks());
            m_blocksThatOnlyReturnWithoutFrame.set(block->index());
        }
        blocksEntered[block->index()] = entered;
        nextLayoutIndexWithCode[index] = index;
    }
    m_nextLayoutIndexWithCode[numberOfBlocksInLayout] = numberOfBlocksInLayout;
    m_numberOfBlocksWithCode = numberOfBlocksInLayout;

    for (uint32_t index : layoutIndexesOfBlocksThatOnlyJumpOn) {
        const Block* block = layout[index];
        const Block* entered = block;
        size_t steps = 0;
        while (m_blocksEntered[entered->index()] != entered && steps <= layout.size()) {
            entered = m_blocksEntered[entered->index()];
            ++steps;
        }
        if (steps > layout.size()) {
            m_blocksEntered[block->index()] = block;
            continue;
        }
        for (const Block* passed = block; passed != entered;)
            passed = std::exchange(m_blocksEntered[passed->index()], entered);
    }

    for (uint32_t place = layoutIndexesOfBlocksThatOnlyJumpOn.size(); place--;) {
        uint32_t index = layoutIndexesOfBlocksThatOnlyJumpOn[place];
        if (hasCode(layout[index]))
            continue;
        m_nextLayoutIndexWithCode[index] = m_nextLayoutIndexWithCode[index + 1];
        --m_numberOfBlocksWithCode;
    }
    if (layoutIndexesOfMoveBlocks.size() >= 2)
        removeRepeatedMoveBlocks(layoutIndexesOfMoveBlocks.span(), layoutIndexesOfBlocksThatOnlyJumpOn.span());
}

void Encoder::removeRepeatedMoveBlocks(std::span<const uint32_t> layoutIndexesOfMoveBlocks, std::span<const uint32_t> layoutIndexesOfBlocksThatOnlyJumpOn)
{
    struct Candidate {
        uint64_t key;
        uint32_t layoutIndex;
    };
    UncheckedSpan<Block* const> layout = m_order.blocksInLayout();
    ArenaVector<Candidate> candidates;
    candidates.reserve(m_arena, layoutIndexesOfMoveBlocks.size());
    auto wordOf = [](const Step& step) {
        uint64_t word = static_cast<uint64_t>(step.kind) << 56 ^ static_cast<uint64_t>(step.width) << 48 ^ static_cast<uint64_t>(std::bit_cast<uint32_t>(step.destination)) << 20 ^ std::bit_cast<uint32_t>(step.sources[0]);
        return step.kind == StepKind::Materialize ? word ^ static_cast<uint64_t>(step.operation->index()) * 0xff51afd7ed558ccdull : word;
    };
    for (uint32_t index : layoutIndexesOfMoveBlocks) {
        const Block* block = layout[index];
        if (m_blocksThatStoreResultOfPatchAtHead.size() && m_blocksThatStoreResultOfPatchAtHead.get(block->index()))
            continue;
        uint64_t key = blockEnteredThrough(block->successor(0).block())->index() * 2 + frameAtBlock(block).runsInFrame;
        const BlockSteps& steps = stepsOf(block);
        for (uint32_t step = steps.beginOfEdgeMoves; step < steps.endOfEdgeMoves; ++step)
            key = (key ^ wordOf(m_allocation.step(step))) * 0x9e3779b97f4a7c15ull;
        candidates.append(m_arena, Candidate { key, index });
    }
    if (candidates.size() < 2)
        return;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.key != b.key ? a.key < b.key : a.layoutIndex < b.layoutIndex;
    });
    auto repeats = [&](const Block* first, const Block* second) {
        if (blockEnteredThrough(first->successor(0).block()) != blockEnteredThrough(second->successor(0).block()))
            return false;
        if (frameAtBlock(first).runsInFrame != frameAtBlock(second).runsInFrame)
            return false;
        if (frameAtBlock(first).isTornDownAtEnd != frameAtBlock(second).isTornDownAtEnd)
            return false;
        const BlockSteps& stepsOfFirst = stepsOf(first);
        const BlockSteps& stepsOfSecond = stepsOf(second);
        const uint32_t numberOfMoves = stepsOfFirst.endOfEdgeMoves - stepsOfFirst.beginOfEdgeMoves;
        if (numberOfMoves != stepsOfSecond.endOfEdgeMoves - stepsOfSecond.beginOfEdgeMoves)
            return false;
        for (uint32_t index = 0; index < numberOfMoves; ++index) {
            const Step& a = m_allocation.step(stepsOfFirst.beginOfEdgeMoves + index);
            const Step& b = m_allocation.step(stepsOfSecond.beginOfEdgeMoves + index);
            if (a.kind != b.kind || a.width != b.width || a.destination != b.destination || a.sources[0] != b.sources[0])
                return false;
            if (a.kind == StepKind::Materialize && a.operation != b.operation)
                return false;
        }
        return true;
    };
    UncheckedSpan<const Block*> blocksInFront;
    auto findPrecedingBlocks = [&] {
        blocksInFront = m_arena.createSpan<const Block*>(layout.size());
        const Block* inFront = nullptr;
        for (uint32_t index = 0; index < layout.size(); ++index) {
            blocksInFront[index] = inFront;
            if (hasCode(layout[index]))
                inFront = layout[index];
        }
    };
    auto isFallenInto = [&](uint32_t index) {
        constexpr size_t successorsOfBranch = 2;
        const Block* inFront = blocksInFront[index];
        if (!inFront || !hasCode(inFront) || inFront->numSuccessors() > successorsOfBranch)
            return true;
        return std::ranges::any_of(inFront->successors(), [&](const Edge& edge) {
            return blockEnteredThrough(edge.block()) == layout[index];
        });
    };
    static constexpr unsigned rankOfBlockThatFallsIntoTarget = 3;
    auto rankAsKept = [&](uint32_t index) -> unsigned {
        const Block* block = layout[index];
        if (isUnlikelyInLayout(placeOf(block)))
            return 0;
        uint32_t next = m_nextLayoutIndexWithCode[index + 1];
        if (next < layout.size() && blockEnteredThrough(block->successor(0).block()) == layout[next])
            return rankOfBlockThatFallsIntoTarget;
        return isFallenInto(index) ? 2 : 1;
    };
    ArenaVector<uint32_t> layoutIndexesOfCopies;
    for (uint32_t first = 0; first < candidates.size();) {
        uint32_t end = first + 1;
        while (end < candidates.size() && candidates[end].key == candidates[first].key)
            ++end;
        if (end - first > 1) {
            if (blocksInFront.empty())
                findPrecedingBlocks();
            uint32_t kept = first;
            unsigned rankOfKept = rankAsKept(candidates[first].layoutIndex);
            for (uint32_t other = first + 1; other < end; ++other) {
                if (unsigned rank = rankAsKept(candidates[other].layoutIndex); rank > rankOfKept) {
                    kept = other;
                    rankOfKept = rank;
                }
            }
            const Block* blockKept = layout[candidates[kept].layoutIndex];
            for (uint32_t other = first; other < end; ++other) {
                const Block* block = layout[candidates[other].layoutIndex];
                if (other == kept || !repeats(blockKept, block) || (rankOfKept != rankOfBlockThatFallsIntoTarget && !isUnlikelyInLayout(placeOf(block)) && isFallenInto(candidates[other].layoutIndex)))
                    continue;
                m_blocksEntered[block->index()] = blockKept;
                --m_numberOfBlocksWithCode;
                layoutIndexesOfCopies.append(m_arena, candidates[other].layoutIndex);
            }
        }
        first = end;
    }
    if (layoutIndexesOfCopies.isEmpty())
        return;
    for (uint32_t index : layoutIndexesOfBlocksThatOnlyJumpOn) {
        const Block* entered = m_blocksEntered[layout[index]->index()];
        m_blocksEntered[layout[index]->index()] = m_blocksEntered[entered->index()];
    }
    std::sort(layoutIndexesOfCopies.begin(), layoutIndexesOfCopies.end(), std::greater<uint32_t>());
    for (uint32_t copy : layoutIndexesOfCopies) {
        const uint32_t next = m_nextLayoutIndexWithCode[copy + 1];
        for (uint32_t index = copy + 1; index-- && m_nextLayoutIndexWithCode[index] == copy;)
            m_nextLayoutIndexWithCode[index] = next;
    }
}

namespace {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

class OrderOfWriting {
public:
    explicit OrderOfWriting(Encoder& encoder)
        : m_encoder(encoder)
        , m_arena(encoder.m_arena)
        , m_order(encoder.m_order)
        , m_places(encoder.m_places)
        , m_stepsOfBlocks(encoder.m_stepsOfBlocks)
        , m_mayJumpOverUnlikelySides(encoder.m_proc.traits().mayJumpOverUnlikelySides)
        , m_layout(encoder.m_order.blocksInLayout())
        , m_blocksEntered(encoder.m_blocksEntered)
        , m_next(encoder.m_nextLayoutIndexWithCode)
        , m_written(encoder.m_blocksAsWritten)
    {
    }

    bool run();
    uint32_t beginOfRareCode() const { return m_beginOfRareCode; }

private:
    static constexpr uint32_t bytesGuessedForStep = 4;
    static constexpr uint32_t reach = 120;
    static constexpr uint32_t mostBytesOfPiece = 64;
    static constexpr uint32_t maxBytesAfterBranch = 32;
    static constexpr uint32_t noPiece = UINT32_MAX;

    struct Piece {
        uint32_t begin { 0 };
        uint32_t end { 0 };
        bool isRun { false };
        bool isEntered { false };
        bool isPlaced { false };
    };
    struct Entrance {
        uint32_t piece;
        uint32_t bytesOfPiece;
        uint32_t bytesInFront;
    };

    static uint32_t bytesGuessedFor(const BlockSteps& steps)
    {
        return (steps.end - steps.begin + steps.endOfEdgeMoves - steps.beginOfEdgeMoves + 1) * bytesGuessedForStep;
    }
    const BlockPlace& placeOf(const Block* block) const { return m_places[block->index()]; }
    const BlockPlace& placeEnteredThrough(const Block* block) const { return placeOf(m_blocksEntered[block->index()]); }
    bool fallsIntoNext(const Block* block, uint32_t next) const
    {
        for (const Edge& edge : block->successors()) {
            if (placeEnteredThrough(edge.block()).layoutIndex == next)
                return true;
        }
        return false;
    }
    uint32_t pieceOf(const BlockPlace& place) const
    {
        if (place.isRare())
            return place.rareRegion;
        if (place.layoutPart != LayoutPart::Unlikely)
            return noPiece;
        return m_runsOfUnlikelyBlocks[place.layoutIndex - m_beginOfUnlikelyPart];
    }
    uint32_t pieceAheadOfRarePart(uint32_t layoutIndex) const
    {
        ASSERT(layoutIndex < m_beginOfRarePart);
        uint32_t piece = layoutIndex < m_beginOfUnlikelyPart ? noPiece : m_runsOfUnlikelyBlocks[layoutIndex - m_beginOfUnlikelyPart];
        ASSERT(piece == pieceOf(placeOf(m_layout[layoutIndex])));
        return piece;
    }

    bool leadsBackAfterBranch(const Piece&, uint32_t next) const;
    void findPieces();
    uint32_t writeRun(uint32_t index, uint32_t limit, bool nothingFallsIn);
    void writePiece(Piece&);

    Encoder& m_encoder;
    Arena& m_arena;
    const Order& m_order;
    const BlockPlace* const m_places;
    const BlockSteps* const m_stepsOfBlocks;
    const bool m_mayJumpOverUnlikelySides;
    UncheckedSpan<Block* const> m_layout;
    UncheckedSpan<const Block* const> m_blocksEntered;
    UncheckedSpan<const uint32_t> m_next;
    ArenaVector<Block*>& m_written;
    ArenaVector<Piece> m_pieces;
    ArenaVector<Entrance> m_entrances;
    UncheckedSpan<uint32_t> m_runsOfUnlikelyBlocks;
    uint32_t m_beginOfUnlikelyPart { 0 };
    uint32_t m_beginOfRarePart { 0 };
    uint32_t m_beginOfRareCode { 0 };
    bool m_somethingIsPlaced { false };
};

void OrderOfWriting::findPieces()
{
    m_pieces.grow(m_arena, m_order.rareRegions().size());
    uint32_t beginOfRarePart = static_cast<uint32_t>(m_layout.size());
    for (; beginOfRarePart; --beginOfRarePart) {
        const BlockPlace& place = placeOf(m_layout[beginOfRarePart - 1]);
        if (place.layoutPart != LayoutPart::Rare)
            break;
        if (!place.isRare())
            continue;
        Piece& region = m_pieces[place.rareRegion];
        if (!region.end)
            region.end = beginOfRarePart;
        RELEASE_ASSERT(region.end == beginOfRarePart || region.begin == beginOfRarePart);
        region.begin = beginOfRarePart - 1;
    }
    m_beginOfRarePart = beginOfRarePart;

    m_beginOfUnlikelyPart = m_beginOfRarePart;
    while (m_beginOfUnlikelyPart && placeOf(m_layout[m_beginOfUnlikelyPart - 1]).layoutPart == LayoutPart::Unlikely)
        --m_beginOfUnlikelyPart;
    if (m_beginOfUnlikelyPart == m_beginOfRarePart)
        return;
    std::span<uint32_t> runsOfUnlikelyBlocks = m_arena.createSpan<uint32_t>(m_beginOfRarePart - m_beginOfUnlikelyPart);
    std::ranges::fill(runsOfUnlikelyBlocks, noPiece);
    m_runsOfUnlikelyBlocks = runsOfUnlikelyBlocks;

    uint32_t lastLikelyBlock = m_beginOfUnlikelyPart;
    while (lastLikelyBlock && !m_encoder.hasCode(m_layout[lastLikelyBlock - 1]))
        --lastLikelyBlock;
    uint32_t index = m_next[m_beginOfUnlikelyPart];
    bool nothingFallsIn = lastLikelyBlock && !fallsIntoNext(m_layout[lastLikelyBlock - 1], index);
    uint32_t beginOfRun = index;
    while (index < m_beginOfRarePart) {
        const Block* block = m_layout[index];
        index = m_next[index + 1];
        if (fallsIntoNext(block, index))
            continue;
        if (nothingFallsIn) {
            for (uint32_t inRun = beginOfRun; inRun < std::min(index, m_beginOfRarePart); ++inRun)
                m_runsOfUnlikelyBlocks[inRun - m_beginOfUnlikelyPart] = m_pieces.size();
            m_pieces.append(m_arena, Piece { beginOfRun, std::min(index, m_beginOfRarePart), true });
        }
        nothingFallsIn = true;
        beginOfRun = index;
    }
}

bool OrderOfWriting::leadsBackAfterBranch(const Piece& piece, uint32_t next) const
{
    uint32_t first = m_next[piece.begin];
    if (first >= piece.end || placeOf(m_layout[first]).predecessors.size() != 1 || m_encoder.frameAtBlock(m_layout[first]).isSetUpAtHead)
        return false;
    uint32_t last = first;
    for (uint32_t index = m_next[first + 1]; index < piece.end; index = m_next[index + 1])
        last = index;
    return fallsIntoNext(m_layout[last], next);
}

ALWAYS_INLINE_INTO_OTHER_FILES void OrderOfWriting::writePiece(Piece& piece)
{
    piece.isPlaced = true;
    m_somethingIsPlaced = true;
    if (piece.isRun) {
        writeRun(m_next[piece.begin], piece.end, true);
        return;
    }
    for (uint32_t index = m_next[piece.begin]; index < piece.end; index = m_next[index + 1])
        m_written.append(m_arena, m_layout[index]);
}

uint32_t OrderOfWriting::writeRun(uint32_t index, uint32_t limit, bool nothingFallsIn)
{
    const BlockPlace* const places = m_places;
    const BlockSteps* const stepsOfBlocks = m_stepsOfBlocks;
    const UncheckedSpan<Block* const> layout = m_layout;
    const UncheckedSpan<const Block* const> blocksEntered = m_blocksEntered;
    const UncheckedSpan<const uint32_t> next = m_next;
    ArenaVector<Block*>& written = m_written;
    const uint32_t beginOfRun = written.size();
    const size_t firstEntrance = m_entrances.size();
    uint32_t bytesOfRun = 0;
    bool somethingFallsOut = true;
    while (somethingFallsOut && index < limit) {
        Block* block = layout[index];
        written.append(m_arena, block);
        if (uint32_t number = pieceAheadOfRarePart(index); number != noPiece) {
            m_pieces[number].isEntered = true;
            m_pieces[number].isPlaced = true;
        }
        bytesOfRun += bytesGuessedFor(stepsOfBlocks[block->index()]);
        index = next[index + 1];
        somethingFallsOut = false;
        uint32_t pieceBehindBranch = noPiece;
        const bool takesPieceAfterBranch = m_mayJumpOverUnlikelySides && places[block->index()].loop == noLoop && index < limit && fallsIntoNext(block, index);
        for (const Edge& edge : block->successors()) {
            const BlockPlace& place = places[blocksEntered[edge.block()->index()]->index()];
            somethingFallsOut |= place.layoutIndex == index;
            if (place.layoutPart == LayoutPart::Likely) [[likely]]
                continue;
            uint32_t number = pieceOf(place);
            if (number == noPiece || m_pieces[number].isEntered || edge.block()->isLandingPad())
                continue;
            Piece& piece = m_pieces[number];
            piece.isEntered = true;
            uint32_t bytesOfPiece = 0;
            for (uint32_t inPiece = next[piece.begin]; inPiece < piece.end && bytesOfPiece <= mostBytesOfPiece; inPiece = next[inPiece + 1])
                bytesOfPiece += bytesGuessedFor(stepsOfBlocks[layout[inPiece]->index()]);
            if (bytesOfPiece > mostBytesOfPiece)
                continue;
            if (takesPieceAfterBranch && pieceBehindBranch == noPiece && bytesOfPiece <= maxBytesAfterBranch && leadsBackAfterBranch(piece, index)) {
                pieceBehindBranch = number;
                bytesOfRun += bytesOfPiece;
                continue;
            }
            m_entrances.append(m_arena, Entrance { number, bytesOfPiece, bytesOfRun });
        }
        if (pieceBehindBranch != noPiece) {
            Piece& piece = m_pieces[pieceBehindBranch];
            piece.isPlaced = true;
            m_somethingIsPlaced = true;
            for (uint32_t inPiece = next[piece.begin]; inPiece < piece.end; inPiece = next[inPiece + 1])
                written.append(m_arena, layout[inPiece]);
        }
    }
    const size_t endOfEntrances = m_entrances.size();
    if (firstEntrance == endOfEntrances)
        return index;

    const uint32_t endOfRun = m_written.size();
    if (nothingFallsIn) {
        uint32_t bytesOfPieces = 0;
        for (size_t number = firstEntrance; number < endOfEntrances; ++number) {
            Entrance entrance = m_entrances[number];
            if (entrance.bytesInFront * 2 > bytesOfRun)
                break;
            if (bytesOfPieces + entrance.bytesOfPiece + entrance.bytesInFront > reach)
                continue;
            bytesOfPieces += entrance.bytesOfPiece;
            writePiece(m_pieces[entrance.piece]);
        }
        std::rotate(m_written.begin() + beginOfRun, m_written.begin() + endOfRun, m_written.end());
    }
    if (!somethingFallsOut) {
        uint32_t bytesOfPieces = 0;
        for (size_t number = firstEntrance; number < endOfEntrances; ++number) {
            Entrance entrance = m_entrances[number];
            if (m_pieces[entrance.piece].isPlaced || bytesOfRun - entrance.bytesInFront + bytesOfPieces + entrance.bytesOfPiece > reach)
                continue;
            bytesOfPieces += entrance.bytesOfPiece;
            writePiece(m_pieces[entrance.piece]);
        }
    }
    m_entrances.shrink(firstEntrance);
    return index;
}

bool OrderOfWriting::run()
{
    findPieces();
    m_written.reserve(m_arena, m_layout.size());
    bool nothingFallsIn = false;
    for (uint32_t index = m_next[0]; index < m_beginOfRarePart;) {
        if (uint32_t piece = pieceAheadOfRarePart(index); piece != noPiece && m_pieces[piece].isPlaced) {
            index = m_next[m_pieces[piece].end];
            continue;
        }
        uint32_t sizeBefore = m_written.size();
        index = writeRun(index, m_beginOfRarePart, nothingFallsIn);
        ASSERT_UNUSED(sizeBefore, m_written.size() > sizeBefore);
        nothingFallsIn = true;
    }
    if (!m_somethingIsPlaced) {
        m_written.clear();
        return false;
    }

    m_beginOfRareCode = m_written.size();
    for (uint32_t index = m_next[m_beginOfRarePart]; index < m_layout.size(); index = m_next[index + 1]) {
        const BlockPlace& place = placeOf(m_layout[index]);
        if (!place.isRare() || !m_pieces[place.rareRegion].isPlaced)
            m_written.append(m_arena, m_layout[index]);
    }
    return true;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} // anonymous namespace

void Encoder::placeColdCodeNearItsJumps()
{
    if (!isX86_64() || m_proc.traits().fallsIntoWhatLiesBehind)
        return;
    if (m_allocation.steps().size() <= 30)
        return;
    if (m_order.rareRegions().empty() && m_order.of(m_order.blocksInLayout().back()).layoutPart == LayoutPart::Likely)
        return;

    OrderOfWriting order(*this);
    if (!order.run())
        return;
    m_beginOfRareCodeAsWritten = order.beginOfRareCode();
    const uint32_t numberOfBlocksAsWritten = m_blocksAsWritten.size();
    RELEASE_ASSERT(numberOfBlocksAsWritten && numberOfBlocksAsWritten == m_numberOfBlocksWithCode);
}

NEVER_INLINE void Encoder::findIndexesAsWritten() const
{
    const uint32_t numberOfBlocksAsWritten = m_blocksAsWritten.size();
    m_indexAsWritten = m_arena.createSpan<uint32_t>(m_proc.numberOfBlocks());
    const Block* first = m_blocksAsWritten[0];
    for (uint32_t index = 1; index < numberOfBlocksAsWritten; ++index) {
        const Block* block = m_blocksAsWritten[index];
        uint32_t& indexOfBlock = m_indexAsWritten[block->index()];
        RELEASE_ASSERT(!indexOfBlock && block != first);
        indexOfBlock = index;
    }
}

void Encoder::run()
{
    ASSERT(!m_proc.failure());
    DisallowMacroScratchRegisterUsage disallowScratch(m_jit);

    ASSERT(!m_order.blocksInLayout().empty());
    findBlocksWithoutCode();
    placeColdCodeNearItsJumps();
    m_pendingJumps.reserve(m_arena, m_numberOfBlocksWithCode);
    switch (m_coverage) {
    case FrameCoverage::None:
        encodeBlocksUnder<FrameCoverage::None>();
        break;
    case FrameCoverage::WholeFunction:
        encodeBlocksUnder<FrameCoverage::WholeFunction>();
        break;
    case FrameCoverage::RareRegions:
        encodeBlocksUnder<FrameCoverage::RareRegions>();
        break;
    case FrameCoverage::FramedPart:
        encodeBlocksUnder<FrameCoverage::FramedPart>();
        break;
    }
    m_block = nullptr;
    m_blockWrittenNext = nullptr;
    m_proc.m_numberOfOperationsEncoded += m_numberOfOperationsEncoded;

    if (!m_proc.m_rareCodeStart.isSet()) {
        m_proc.m_rareCodeStart = m_jit.labelIgnoringWatchpoints();
        noteStartOfRareCode(m_jit);
    }
    for (Entrypoint& entrypoint : m_proc.m_entrypoints) {
        entrypoint.label = *labelOf(entrypoint.block);
        RELEASE_ASSERT(entrypoint.label.isSet());
    }

    linkPendingJumps();
    runLatePaths();
    if (!m_proc.m_rangesOfOrigins.isEmpty())
        m_proc.m_rangesOfOrigins.last().end = m_jit.labelIgnoringWatchpoints();
    ASSERT(m_pendingJumps.isEmpty());

    for (const JumpTableRecord& table : m_proc.m_jumpTables) {
        for (const MacroAssembler::Label& target : table.targets)
            RELEASE_ASSERT(target.isSet());
    }

    if (m_readSlotTags & Frame::deadSlotTag) [[unlikely]]
        reportDeadSlotRead();
}

bool Encoder::fallsThroughToSlowCase(const Block* from, const Block* target) const
{
    ASSERT(from != m_block);
    ASSERT(hasCode(from));
    const BlockPlace& placeAsNamed = m_order.of(target);
    if (!placeAsNamed.isReached())
        return false;
    target = blockEnteredThrough(target);
    if (!m_blocksAsWritten.isEmpty()) {
        if (m_indexAsWritten.empty())
            findIndexesAsWritten();
        if (m_indexAsWritten[target->index()] != m_indexAsWritten[from->index()] + 1)
            return false;
    } else if (m_order.of(target).layoutIndex != m_nextLayoutIndexWithCode[m_order.of(from).layoutIndex + 1])
        return false;
    return frameAtBlock(from).isLeftInFrame() == frameAtBlock(target).isEnteredInFrame();
}

bool Encoder::isCall(const PatchOperation& patch)
{
    static constexpr int fewestRegisters = 8;
    return std::popcount(patch.lateClobberedMask()) >= fewestRegisters;
}

void Encoder::noteShortLoopsThatEndWith(const Block* block)
{
    static constexpr ptrdiff_t maxBytes = 128;
    for (const Edge& edge : block->successors()) {
        if (placeOf(edge.block()).loop != placeOf(block).loop)
            continue;
        const MacroAssembler::Label& begin = m_labels[blockEnteredThrough(edge.block())->index()];
        if (!begin.isSet())
            continue;
        MacroAssembler::Label end = m_jit.labelIgnoringWatchpoints();
        ptrdiff_t length = MacroAssembler::differenceBetween(begin, end);
        if (length <= 0 || length > maxBytes || (m_lastCall.isSet() && MacroAssembler::differenceBetween(begin, m_lastCall) >= 0))
            continue;
        ptrdiff_t lastBegin = m_proc.m_shortLoops.isEmpty() ? -1 : MacroAssembler::differenceBetween(begin, m_proc.m_shortLoops.last().begin);
        if (lastBegin > 0)
            continue;
        if (!lastBegin)
            m_proc.m_shortLoops.last().end = end;
        else
            m_proc.m_shortLoops.append(m_arena, ShortLoop { begin, end });
    }
}

template<FrameCoverage coverage>
ALWAYS_INLINE void Encoder::encodeBlockUnder(Block* block)
{
    const BlockPlace& place = placeOf(block);
    ASSERT(place.isReached());
    ASSERT(hasCode(block));
    MacroAssembler::Label& label = m_labels[block->index()];
    RELEASE_ASSERT(!label.isSet());

    m_block = block;
    label = m_jit.label();
    if (place.isRare() && !m_proc.m_rareCodeStart.isSet() && !m_isAmongOtherCode && !m_proc.traits().fallsIntoWhatLiesBehind) {
        m_proc.m_rareCodeStart = label;
        noteStartOfRareCode(m_jit);
    }

    const FrameAtBlock frame = frameAtBlockUnder<coverage>(block, place);
    m_blockIsLeftInFrame = frame.isLeftInFrame();
    noteOrigin(Origin());
    noteFrameState(frame.isEnteredInFrame());
    if (frame.isSetUpAtHead)
        setUpFrame(block);
    if (m_hasFramedPart && m_allocation.isHeadOfFramedPart(block)) {
        const Allocation::HeadOfFramedPart& head = m_allocation.headOfFramedPartAbove(block);
        if (m_spillsPushedAtSetup.isEmpty())
            encodeSteps(head.beginOfSteps, head.endOfSteps);
        else {
            for (uint32_t index = head.beginOfSteps; index < head.endOfSteps; ++index) {
                if (!setupHasPushed(m_allocation.step(index).destination.spillIndex()))
                    encodeSteps(index, index + 1);
            }
        }
    }
    if (m_hasSpills && (!block->phis().empty() || m_blocksThatStoreResultOfPatchAtHead.get(block->index())))
        storeSpilledValuesDefinedAtHead(block, place);

    const BlockSteps& steps = stepsOf(block);
    uint32_t beginOfSteps = steps.begin;
    if (frame.isSetUpAtHead && setupPushesSavesOfRareRegion(block)) {
        for (; beginOfSteps < steps.end; ++beginOfSteps) {
            const Step& step = m_allocation.step(beginOfSteps);
            if (step.kind != StepKind::Save || !step.destination.isSpill())
                break;
            uint32_t pushed = 0;
            while (pushed < m_spillsPushedAtSetup.size() && m_spillsPushedAtSetup[pushed] != step.destination.spillIndex())
                ++pushed;
            if (pushed < m_spillsPushedAtSetup.size())
                m_spillsPushedAtSetup[pushed] = noSpill;
            else
                encodeSteps(beginOfSteps, beginOfSteps + 1);
        }
        m_spillsPushedAtSetup.clear();
    }
    uint32_t endOfSteps = steps.end;
    uint32_t endOfEdgeMoves = steps.endOfEdgeMoves;
    std::span<const GPRReg> registersPoppedAtTeardown;
    if (frame.isTornDownAtEnd) {
        if (steps.beginOfEdgeMoves < steps.endOfEdgeMoves)
            registersPoppedAtTeardown = findRegistersThatTeardownPops(steps.beginOfEdgeMoves, steps.endOfEdgeMoves, endOfEdgeMoves);
        else
            registersPoppedAtTeardown = findRegistersThatTeardownPops(beginOfSteps, steps.end, endOfSteps);
    }
    if (beginOfSteps < endOfSteps)
        encodeSteps(beginOfSteps, endOfSteps);
    if (steps.beginOfEdgeMoves < endOfEdgeMoves)
        encodeSteps(steps.beginOfEdgeMoves, endOfEdgeMoves);

    RELEASE_ASSERT(steps.terminator != UINT32_MAX);
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const Step& terminator = m_steps[steps.terminator];
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    ASSERT(terminator.kind == StepKind::Execute);
    ASSERT(terminator.operation == block->last());

    if constexpr (coverage == FrameCoverage::RareRegions || coverage == FrameCoverage::FramedPart) {
        if (!frame.returnsInPlaceOfJump) {
            for (const Edge& edge : block->successors())
                RELEASE_ASSERT(frameAtBlockUnder<coverage>(edge.block(), placeOf(edge.block())).isEnteredInFrame() == m_blockIsLeftInFrame);
        }
    }
    if (frame.isTornDownAtEnd) {
        RELEASE_ASSERT(terminator.operation->isUnconditionalJump());
        ASSERT(m_frameIsSetUp);
        leaveFrameIfSetUp(registersPoppedAtTeardown);
    }

    noteOrigin(terminator.operation->origin(), terminator.operation->index());
    ++m_numberOfOperationsEncoded;
    if (frame.returnsInPlaceOfJump) {
        RELEASE_ASSERT(terminator.operation->isUnconditionalJump() && !m_frameIsSetUp);
        m_jit.ret();
        return;
    }
    if (isTerminal(terminator.operation->opcode())) [[likely]]
        encodeTerminatorInline(block, terminator);
    else
        encodeExecute(terminator);
    if (isX86_64() && m_mayEndLoop) [[unlikely]] {
        m_mayEndLoop = false;
        if (place.loop != noLoop && place.layoutPart == LayoutPart::Likely)
            noteShortLoopsThatEndWith(block);
    }
}

template<FrameCoverage coverage>
void Encoder::encodeBlocksUnder()
{
    if (m_blocksAsWritten.isEmpty()) {
        UncheckedSpan<Block* const> layout = m_order.blocksInLayout();
        const uint32_t numberOfBlocksInLayout = static_cast<uint32_t>(layout.size());
        for (uint32_t index = m_nextLayoutIndexWithCode[0]; index < numberOfBlocksInLayout;) {
            uint32_t next = m_nextLayoutIndexWithCode[index + 1];
            m_blockWrittenNext = next < numberOfBlocksInLayout ? layout[next] : nullptr;
            encodeBlockUnder<coverage>(layout[index]);
            index = next;
        }
        return;
    }

    const uint32_t numberOfBlocksAsWritten = m_blocksAsWritten.size();
    UncheckedSpan<Block* const> blocksAsWritten = m_blocksAsWritten.span();
    bool rangeOfRareCodeIsOpen = false;
    for (uint32_t index = 0; index < numberOfBlocksAsWritten; ++index) {
        Block* block = blocksAsWritten[index];
        m_isAmongOtherCode = index < m_beginOfRareCodeAsWritten;
        if (bool isRare = m_isAmongOtherCode && placeOf(block).isRare(); isRare != rangeOfRareCodeIsOpen) {
            rangeOfRareCodeIsOpen = isRare;
            MacroAssembler::Label here = m_jit.labelIgnoringWatchpoints();
            if (isRare)
                m_proc.m_rangesOfRareCodeAmongOtherCode.append(m_arena, RangeOfRareCode { here, MacroAssembler::Label() });
            else
                m_proc.m_rangesOfRareCodeAmongOtherCode.last().end = here;
        }
        m_blockWrittenNext = index + 1 < numberOfBlocksAsWritten ? blocksAsWritten[index + 1] : nullptr;
        encodeBlockUnder<coverage>(block);
    }
    if (rangeOfRareCodeIsOpen)
        m_proc.m_rangesOfRareCodeAmongOtherCode.last().end = m_jit.labelIgnoringWatchpoints();
    m_isAmongOtherCode = false;
}

static NEVER_INLINE PatchOperation* findCalleeThatDoesMoveOutOfResultItself(const Step* move, const Step* end, const Step* steps, const BlockSteps& stepsOfBlock, const Step*& moveIntoResult)
{
    const Step* const endOfRangeOfMove = end;
    const Step* intoResult = nullptr;
    const Reg result = Reg(GPRInfo::returnValueGPR);
    if (move->width != Width64 || !move->destination.isRegister())
        return nullptr;
    const Reg filled = move->destination.reg();
    RELEASE_ASSERT(stepsOfBlock.terminator != UINT32_MAX);
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const Step* const endOfBlock = steps + stepsOfBlock.end;
    const Step* const endOfEdgeMoves = steps + stepsOfBlock.endOfEdgeMoves;
    const Step* const terminator = steps + stepsOfBlock.terminator;
    for (const Step* next = move + 1;; ++next) {
        if (next == end) {
            if (end == endOfBlock && stepsOfBlock.beginOfEdgeMoves < stepsOfBlock.endOfEdgeMoves) {
                next = steps + stepsOfBlock.beginOfEdgeMoves;
                end = endOfEdgeMoves;
            } else if (end == endOfBlock || end == endOfEdgeMoves) {
                next = terminator;
                end = terminator + 1;
            } else
                return nullptr;
        }
        switch (next->kind) {
        case StepKind::Move:
            if (!next->sources[0].isRegister() || next->sources[0].reg() == filled)
                return nullptr;
            if (intoResult && next->sources[0].reg() == result)
                return nullptr;
            if (!intoResult && end == endOfRangeOfMove && next->width == Width64 && next->destination == Location::reg(result) && next->sources[0].reg() != result) {
                intoResult = next;
                continue;
            }
            [[fallthrough]];
        case StepKind::Reload:
        case StepKind::Materialize:
            if (!next->destination.isRegister() || next->destination.reg() == filled || next->destination.reg() == result)
                return nullptr;
            if (intoResult && next->destination.reg() == intoResult->sources[0].reg())
                return nullptr;
            continue;
        case StepKind::Execute: {
            if (next->operation->opcode() != Patch)
                return nullptr;
            auto* patch = static_cast<PatchOperation*>(next->operation);
            if (patch->registerThatCalleeFillsOutOfResult() != filled)
                return nullptr;
            if (patch->registerThatCalleeMovesIntoResult() != (intoResult ? intoResult->sources[0].reg() : Reg()))
                return nullptr;
            moveIntoResult = intoResult;
            return patch;
        }
        case StepKind::Save:
            if (!next->sources[0].isRegister() || next->sources[0].reg() == filled)
                return nullptr;
            if (intoResult && next->sources[0].reg() == result)
                return nullptr;
            continue;
        case StepKind::Swap:
        case StepKind::KeepPointer:
            return nullptr;
        }
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}

void Encoder::encodeSteps(uint32_t begin, uint32_t end)
{
    auto executesStore = [](const Step& candidate) {
        return candidate.kind == StepKind::Execute && candidate.operation->opcode() == Store;
    };

    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const Step* const endOfSteps = m_steps + end;
    const Step* moveIntoResultLeftToCallee = nullptr;
    for (const Step* current = m_steps + begin; current < endOfSteps; ++current) {
        const Step& step = *current;
        if (m_notesRangesOfOrigins) [[unlikely]] {
            if (step.kind == StepKind::Execute)
                noteOrigin(step.operation->origin(), step.operation->index());
            else
                noteOrigin(originOfStepOfAllocator(*this, static_cast<uint32_t>(current - m_steps)));
        }
        const Step* const next = current + 1;

        switch (step.kind) {
        case StepKind::Execute: {
            Opcode opcode = step.operation->opcode();
            if (opcode == Store && next < endOfSteps && executesStore(*next) && tryEncodePairOfStores(step, *next)) {
                m_numberOfOperationsEncoded += 2;
                ++current;
                continue;
            }
            if (isARM64() && opcode == Load && next < endOfSteps && tryEncodePairOfLoads(*this, step, *next)) {
                m_numberOfOperationsEncoded += 2;
                storeToSpillIfSpilled(step.operation);
                storeToSpillIfSpilled(next->operation);
                ++current;
                continue;
            }
            if (isX86_64() && opcode == Add && next < endOfSteps && tryEncodeSumAndConstant(step, *next)) {
                m_numberOfOperationsEncoded += 2;
                storeToSpillIfSpilled(next->operation);
                ++current;
                continue;
            }
            ++m_numberOfOperationsEncoded;
            encodeExecuteInline(step);
            storeToSpillIfSpilled(step.operation);
            continue;
        }
        case StepKind::Move:
            if (current == moveIntoResultLeftToCallee)
                continue;
            if (step.sources[0] == Location::reg(Reg(GPRInfo::returnValueGPR))) {
                if (PatchOperation* callee = findCalleeThatDoesMoveOutOfResultItself(current, endOfSteps, m_steps, stepsOf(m_block), moveIntoResultLeftToCallee)) {
                    callee->setLeavesMoveOutOfResultToCallee(true);
                    continue;
                }
            }
            encodeMove(step);
            continue;
        case StepKind::Reload:
            if (isARM64() && next < endOfSteps && next->kind == StepKind::Reload && tryEncodePairOfMoves(step, *next)) {
                ++current;
                continue;
            }
            encodeReload(step);
            continue;
        case StepKind::Save:
            if (isARM64() && next < endOfSteps && next->kind == StepKind::Save && tryEncodePairOfMoves(step, *next)) {
                ++current;
                continue;
            }
            encodeSave(step);
            continue;
        case StepKind::Materialize:
            encodeMaterialize(step.operation, step.destination.reg());
            continue;
        case StepKind::Swap:
            encodeSwap(step);
            continue;
        case StepKind::KeepPointer:
            encodeKeepPointer(step);
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}

void Encoder::encodeExecute(const Step& step)
{
    encodeExecuteInline(step);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::encodeExecuteInline(const Step& step)
{
    ASSERT(step.kind == StepKind::Execute);
    Operation* operation = step.operation;

    switch (operation->opcode()) {
    case Nop:
    case Phi:
    case Upsilon:
    case Extract:
        return;

    case ArgumentReg:
        encodeArgumentRegister(*this, step);
        return;

    case Opaque:
        if (operation->type().isFloat())
            encodeOpaqueNumber(*this, step);
        else
            encodeInteger(step);
        return;

    case Add:
    case Sub:
    case Mul:
    case Div:
    case Neg:
        if (operation->type().isFloat())
            encodeNumber(step);
        else
            encodeInteger(step);
        return;

    case MulHigh:
    case UMulHigh:
    case UDiv:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case RotR:
    case Clz:
    case PopCount:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
        encodeInteger(step);
        return;

    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
        encodeNumber(step);
        return;

    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
        encodeComparison(step);
        return;

    case Select:
        encodeSelect(step);
        return;

    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        encodeLoad(step);
        return;

    case Store8:
    case Store16:
    case Store:
        encodeStore(step);
        return;

    case Fence:
        encodeFence();
        return;

    case Patch: {
        PatchOperation* patch = operation->as<PatchOperation>();
        if (!patch->onlyKeepsChildrenAlive)
            encodePatch(step, patch);
        return;
    }

    case Check:
        encodeCheck(step, operation->as<PatchOperation>());
        return;

    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
        encodeTerminator(m_block, step);
        return;

    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    case Mod:
    case UMod:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Encoder::reportDeadSlotRead() const
{
    std::span<const SpillFacts> spills = m_allocation.spills();
    const Operation* valueOfSlot = nullptr;
    auto look = [&](Location location) {
        if (!valueOfSlot && location.isSpill() && spills[location.spillIndex()].isDeadSlot)
            valueOfSlot = spills[location.spillIndex()].value;
    };
    for (const Step& step : m_allocation.steps()) {
        for (Location source : step.sources)
            look(source);
        if (step.patchAssignment == UINT32_MAX)
            continue;
        std::span<const Location> locations = m_allocation.patchAssignment(step.patchAssignment).locations;
        for (Location operand : locations.subspan(std::min<size_t>(m_proc.resultCount(step.operation->type()), locations.size())))
            look(operand);
    }
    for (const SpillFacts& spill : spills) {
        if (!valueOfSlot && spill.isDeadSlot)
            valueOfSlot = spill.value;
    }
    m_proc.fail(FailureKind::WrongValueInSpillSlot, valueOfSlot);
}

void Encoder::storeToSpill(const Operation* value)
{
    SpillIndex spill = m_allocation.spillOf(value);
    ASSERT(spill != noSpill);
    Reg source = m_allocation.registerAtDefinitionOf(value);
    if (!source || m_allocation.spills()[spill].isStoredAtHeadOfFramedPart || m_allocation.spills()[spill].isDeadSlot || m_allocation.isWrittenByEdges(value))
        return;
    if (setupHasPushed(spill) && m_allocation.spills()[spill].value == value)
        return;

    RELEASE_ASSERT(m_frameIsSetUp);
    ASSERT(m_allocation.spills()[spill].width == widthOf(value->type()));
    noteOrigin(originOfFrameOrStore(*this, CodeOfBackEnd::StoreBehindDefinition));
    MacroAssembler::Address address = addressOf(Location::spill(spill), MemoryOfEncodedMove::IsWritten);
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    switch (value->type().kind()) {
    case Int32:
        m_jit.store32(source.gpr(), address);
        return;
    case Int64:
        m_jit.store64(source.gpr(), address);
        return;
    case Float:
        m_jit.storeFloat(source.fpr(), address);
        return;
    case Double:
        m_jit.storeDouble(source.fpr(), address);
        return;
    case Vector128:
        m_jit.storeVector(source.fpr(), address);
        return;
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::storeSpilledValuesDefinedAtHead(Block* block, const BlockPlace& place)
{
    for (Operation* phi : block->phis()) {
        if (m_allocation.isSpilled(phi) && !m_liveness.isDropped(phi))
            storeToSpill(phi);
    }

    if (block->isLandingPad() || !m_blocksThatStoreResultOfPatchAtHead.get(block->index()))
        return;
    std::span<Block* const> predecessors = place.predecessors;
    RELEASE_ASSERT(predecessors.size() == 1);
    Block* predecessor = predecessors[0];
    Operation* last = predecessor->last();
    RELEASE_ASSERT(last->opcode() == Patch && predecessor->successor(0).block() == block && m_allocation.isSpilled(last));
    storeToSpill(last);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::setUpFrame(const Block* block)
{
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    noteOrigin(originOfFrameOrStore(*this, CodeOfBackEnd::FrameSetup));
    emitFrameSetup(m_jit, m_proc, findRegistersThatSetupPushes(block));
    noteOrigin(Origin());
    noteFrameState(true);
}

void Encoder::leaveFrameIfSetUp(std::span<const GPRReg> registersPoppedOutOfWords)
{
    if (!m_frameIsSetUp)
        return;

    noteFrameState(false);
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    Origin originOfUser = m_origin;
    uint32_t operationOfUser = m_operationOfOrigin;
    noteOrigin(originOfFrameOrStore(*this, CodeOfBackEnd::FrameTeardown));
    if (registersPoppedOutOfWords.empty())
        emitFrameTeardown(m_jit, m_frame);
    else
        emitTeardownOfFrameThatPopsWords(m_jit, m_frame, registersPoppedOutOfWords);
    noteOrigin(originOfUser, operationOfUser);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Encoder::startOrEndFrameRange(bool isSetUp)
{
    ASSERT(m_frameRangeIsOpen != isSetUp);
    m_frameRangeIsOpen = isSetUp;
    MacroAssembler::Label here = m_jit.labelIgnoringWatchpoints();
    if (isSetUp)
        m_proc.m_frameRanges.append(m_arena, FrameRange { here, MacroAssembler::Label() });
    else
        m_proc.m_frameRanges.last().end = here;
}

void Encoder::noteOriginInRanges(Origin origin, uint32_t operation)
{
    ASSERT(m_notesRangesOfOrigins);
    m_origin = origin;
    m_operationOfOrigin = operation;

    ArenaVector<RangeOfOrigin>& ranges = m_proc.m_rangesOfOrigins;
    if (!ranges.isEmpty() && ranges.last().origin == origin && ranges.last().operation == operation && !isOriginOfBackEnd(origin))
        return;
    MacroAssembler::Label here = m_jit.labelIgnoringWatchpoints();
    if (!ranges.isEmpty())
        ranges.last().end = here;
    ranges.append(m_arena, RangeOfOrigin { here, MacroAssembler::Label(), origin, operation });
}

bool Encoder::blockRunsInFrame(const Block* block) const
{
    switch (m_frame.coverage()) {
    case FrameCoverage::None:
        return false;
    case FrameCoverage::WholeFunction:
        return true;
    case FrameCoverage::RareRegions: {
        const BlockPlace& place = m_order.of(block);
        return place.isRare() && m_allocation.needsFrameIn(place.rareRegion);
    }
    case FrameCoverage::FramedPart:
        return m_frame.holdsStepsOf(block);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Encoder::runLatePaths()
{
    for (uint32_t index = 0; index < m_latePaths.size(); ++index) {
        PendingLatePath pending = m_latePaths[index];
        noteFrameState(pending.frameIsSetUp);
        noteOrigin(pending.origin, pending.operation);
        pending.path->run(m_jit);
    }
    noteFrameState(false);
}

void Encoder::linkPendingJumps()
{
    for (const PendingJump& pending : m_pendingJumps) {
        RELEASE_ASSERT(pending.label->isSet());
        pending.jump.linkTo(*pending.label, &m_jit);
    }
    m_pendingJumps.clear();

    for (const PendingJumpTable& table : m_pendingJumpTables) {
        std::span<MacroAssembler::Label> labels = m_proc.m_jumpTables[table.index].targets;
        RELEASE_ASSERT(labels.size() == table.targets.size());
        for (size_t entry = 0; entry < labels.size(); ++entry) {
            labels[entry] = *labelOf(table.targets[entry]);
            RELEASE_ASSERT(labels[entry].isSet());
        }
    }
    m_pendingJumpTables.clear();
}

void Encoder::addPendingJumps(const MacroAssembler::JumpList& jumps, const Block* target)
{
    for (const MacroAssembler::Jump& jump : jumps.jumps())
        addPendingJump(jump, target);
}

void addLatePath(Encoder& encoder, LatePath* path)
{
    encoder.addLatePath(path);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)

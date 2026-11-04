/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AOTMachineDescription.h"
#include "AOTMachineEncoderPairsOfLoads.h"
#include "AOTMachineFrameForms.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "DisallowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Machine {


struct FrameAtBlock {
    // The state at the label of the block.
    bool isEnteredInFrame() const { return runsInFrame && !isSetUpAtHead; }
    // The state on the edges to its successors.
    bool isLeftInFrame() const { return runsInFrame && !isTornDownAtEnd; }

    bool runsInFrame;
    bool isSetUpAtHead;
    bool isTornDownAtEnd;
    bool returnsInPlaceOfJump;
};

static ALWAYS_INLINE FrameAtBlock frameAtBlock(const Encoder& encoder, const Block* block, const BlockPlace& place)
{
    switch (encoder.m_coverage) {
    case FrameCoverage::None:
        return { false, false, false, false };
    case FrameCoverage::WholeFunction:
        return { true, block->isEntry(), false, false };
    case FrameCoverage::RareRegions: {
        bool runsInFrame = place.isRare() && encoder.m_allocation.needsFrameIn(place.rareRegion);
        return { runsInFrame, runsInFrame && (block->isEntry() || place.entersRareRegion), runsInFrame && place.leavesRareRegion, false };
    }
    case FrameCoverage::FramedPart:
        return { encoder.m_frame.holdsStepsOf(block), encoder.m_frame.isSetUpAtHeadOf(block), encoder.m_frame.isTornDownAtEndOf(block), encoder.m_frame.returnsInPlaceOfJumpAtEndOf(block) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE FrameAtBlock frameAtBlock(const Encoder& encoder, const Block* block)
{
    return frameAtBlock(encoder, block, encoder.m_order.of(block));
}

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

// On x86-64 a jump that reaches far takes four bytes more, and most tests of a function jump to its rare code: X86JumpShortener::chainJumpsIntoRareCode().
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
    case StepKind::ClearSpill:
        return CodeOfBackEnd::ClearSpill;
    case StepKind::KeepPointer:
        return CodeOfBackEnd::KeepPointer;
    case StepKind::Execute:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Of a step that executes nothing.
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
    , m_labels(proc.arena().createSpan<MacroAssembler::Label>(proc.numberOfBlocks()))
    , m_blocksEntered(proc.arena().createSpan<const Block*>(proc.numberOfBlocks()))
    , m_nextLayoutIndexWithCode(proc.arena().createSpan<uint32_t>(proc.order().blocksInLayout().size() + 1))
{
}

static bool blockThatJumpsOnlyJumpsOn(const Encoder& encoder, const Block* block)
{
    ASSERT(block->last()->opcode() == Jump);

    // Most blocks are told apart by their steps.
    const Allocation& allocation = encoder.m_allocation;
    const BlockSteps& steps = allocation.stepsOf(block);
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
    const BlockPlace& place = encoder.m_order.of(block);
    FrameAtBlock frame = frameAtBlock(encoder, block, place);
    if (frame.isSetUpAtHead || frame.isTornDownAtEnd || frame.returnsInPlaceOfJump)
        return false;
    if (encoder.m_hasFramedPart && allocation.isHeadOfFramedPart(block) && allocation.headOfFramedPartAbove(block).beginOfSteps != allocation.headOfFramedPartAbove(block).endOfSteps)
        return false;
    if (!encoder.m_hasSpills)
        return true;

    // What storeSpilledValuesDefinedAtHead() stores.
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

void Encoder::findBlocksWithoutCode()
{
    UncheckedSpan<Block* const> layout = m_order.blocksInLayout();
    const uint32_t numberOfBlocksInLayout = static_cast<uint32_t>(layout.size());
    const bool someBlockMayRunWithoutFrame = m_coverage != FrameCoverage::WholeFunction;
    ArenaVector<uint32_t> layoutIndexesOfBlocksThatOnlyJumpOn;
    for (uint32_t index = 0; index < numberOfBlocksInLayout; ++index) {
        const Block* block = layout[index];
        const Block* entered = block;
        const Operation* last = block->last();
        Opcode opcode = last->opcode();
        if (opcode == Jump) {
            if (blockThatJumpsOnlyJumpsOn(*this, block)) {
                entered = block->successor(0).block();
                layoutIndexesOfBlocksThatOnlyJumpOn.append(m_arena, index);
            }
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
        m_blocksEntered[block->index()] = entered;
        m_nextLayoutIndexWithCode[index] = index;
    }
    m_nextLayoutIndexWithCode[numberOfBlocksInLayout] = numberOfBlocksInLayout;
    m_numberOfBlocksWithCode = numberOfBlocksInLayout;

    // Each ends at a block that has code. A ring of blocks that only jump on is a loop of the program: the block at which the way comes round keeps its jump.
    for (uint32_t index : layoutIndexesOfBlocksThatOnlyJumpOn) {
        const Block* block = layout[index];
        const Block* entered = block;
        size_t steps = 0;
        while (m_blocksEntered[entered->index()] != entered && steps <= layout.size()) {
            entered = m_blocksEntered[entered->index()];
            ++steps;
        }
        if (steps > layout.size())
            entered = block;
        m_blocksEntered[block->index()] = entered;
    }

    for (uint32_t place = layoutIndexesOfBlocksThatOnlyJumpOn.size(); place--;) {
        uint32_t index = layoutIndexesOfBlocksThatOnlyJumpOn[place];
        if (hasCode(layout[index]))
            continue;
        m_nextLayoutIndexWithCode[index] = m_nextLayoutIndexWithCode[index + 1];
        --m_numberOfBlocksWithCode;
    }
}

namespace {

// Encoder::placeColdCodeNearItsJumps().
class OrderOfWriting {
public:
    explicit OrderOfWriting(Encoder& encoder)
        : m_encoder(encoder)
        , m_arena(encoder.m_arena)
        , m_order(encoder.m_order)
        , m_allocation(encoder.m_allocation)
        , m_layout(encoder.m_order.blocksInLayout())
        , m_blocksEntered(encoder.m_blocksEntered)
        , m_next(encoder.m_nextLayoutIndexWithCode)
        , m_written(encoder.m_blocksAsWritten)
    {
    }

    // False where the layout stands.
    bool run();
    uint32_t beginOfRareCode() const { return m_beginOfRareCode; }

private:
    static constexpr uint32_t bytesGuessedForStep = 4;
    static constexpr uint32_t reach = 120;
    static constexpr uint32_t mostBytesOfPiece = 64;
    static constexpr uint32_t noPiece = UINT32_MAX;

    // What may be written anywhere: a rare region, or a run of unlikely blocks that nothing falls into and nothing falls out of.
    struct Piece {
        // In the layout.
        uint32_t begin { 0 };
        uint32_t end { 0 };
        bool isRun { false };
        bool isEntered { false };
        bool isPlaced { false };
    };
    struct Entrance {
        uint32_t piece;
        uint32_t bytesOfPiece;
        // From the head of the run to the end of the block that jumps.
        uint32_t bytesInFront;
    };

    uint32_t bytesGuessedFor(const Block* block) const
    {
        const BlockSteps& steps = m_allocation.stepsOf(block);
        return (steps.end - steps.begin + steps.endOfEdgeMoves - steps.beginOfEdgeMoves + 1) * bytesGuessedForStep;
    }
    const BlockPlace& placeEnteredThrough(const Block* block) const { return m_order.of(m_blocksEntered[block->index()]); }
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
        ASSERT(piece == pieceOf(m_order.of(m_layout[layoutIndex])));
        return piece;
    }

    void findPieces();
    // Writes the blocks from there on up to one that nothing falls out of, `limit` at the latest, and the pieces that find a place beside them. Returns where it has come to in the layout.
    uint32_t writeRun(uint32_t index, uint32_t limit, bool nothingFallsIn);
    void writePiece(Piece&);

    Encoder& m_encoder;
    Arena& m_arena;
    const Order& m_order;
    const Allocation& m_allocation;
    UncheckedSpan<Block* const> m_layout;
    UncheckedSpan<const Block* const> m_blocksEntered;
    UncheckedSpan<const uint32_t> m_next;
    ArenaVector<Block*>& m_written;
    ArenaVector<Piece> m_pieces;
    // A stack: each run that is being written has its own on top.
    ArenaVector<Entrance> m_entrances;
    // By the place in the unlikely part of the layout.
    UncheckedSpan<uint32_t> m_runsOfUnlikelyBlocks;
    uint32_t m_beginOfUnlikelyPart { 0 };
    uint32_t m_beginOfRarePart { 0 };
    uint32_t m_beginOfRareCode { 0 };
    bool m_somethingIsPlaced { false };
};

void OrderOfWriting::findPieces()
{
    m_pieces.grow(m_arena, m_order.rareRegions().size());
    m_beginOfRarePart = static_cast<uint32_t>(m_layout.size());
    for (; m_beginOfRarePart; --m_beginOfRarePart) {
        const BlockPlace& place = m_order.of(m_layout[m_beginOfRarePart - 1]);
        if (place.layoutPart != LayoutPart::Rare)
            break;
        if (!place.isRare())
            continue;
        Piece& region = m_pieces[place.rareRegion];
        if (!region.end)
            region.end = m_beginOfRarePart;
        RELEASE_ASSERT(region.end == m_beginOfRarePart || region.begin == m_beginOfRarePart);
        region.begin = m_beginOfRarePart - 1;
    }

    m_beginOfUnlikelyPart = m_beginOfRarePart;
    while (m_beginOfUnlikelyPart && m_order.of(m_layout[m_beginOfUnlikelyPart - 1]).layoutPart == LayoutPart::Unlikely)
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

void OrderOfWriting::writePiece(Piece& piece)
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
    const uint32_t beginOfRun = m_written.size();
    const size_t firstEntrance = m_entrances.size();
    uint32_t bytesOfRun = 0;
    bool somethingFallsOut = true;
    while (somethingFallsOut && index < limit) {
        Block* block = m_layout[index];
        m_written.append(m_arena, block);
        // Whoever jumps to the run from here on, a block of the run itself among them, finds it written.
        if (uint32_t number = pieceAheadOfRarePart(index); number != noPiece) {
            m_pieces[number].isEntered = true;
            m_pieces[number].isPlaced = true;
        }
        bytesOfRun += bytesGuessedFor(block);
        index = m_next[index + 1];
        somethingFallsOut = false;
        for (const Edge& edge : block->successors()) {
            const BlockPlace& place = placeEnteredThrough(edge.block());
            somethingFallsOut |= place.layoutIndex == index;
            if (place.layoutPart == LayoutPart::Likely) [[likely]]
                continue;
            uint32_t number = pieceOf(place);
            if (number == noPiece || m_pieces[number].isEntered || edge.block()->isLandingPad())
                continue;
            Piece& piece = m_pieces[number];
            piece.isEntered = true;
            uint32_t bytesOfPiece = 0;
            for (uint32_t inPiece = m_next[piece.begin]; inPiece < piece.end && bytesOfPiece <= mostBytesOfPiece; inPiece = m_next[inPiece + 1])
                bytesOfPiece += bytesGuessedFor(m_layout[inPiece]);
            if (bytesOfPiece <= mostBytesOfPiece)
                m_entrances.append(m_arena, Entrance { number, bytesOfPiece, bytesOfRun });
        }
    }
    const size_t endOfEntrances = m_entrances.size();
    if (firstEntrance == endOfEntrances)
        return index;

    const uint32_t endOfRun = m_written.size();
    if (nothingFallsIn) {
        // They are appended, and then change places with the run.
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
        const BlockPlace& place = m_order.of(m_layout[index]);
        if (!place.isRare() || !m_pieces[place.rareRegion].isPlaced)
            m_written.append(m_arena, m_layout[index]);
    }
    return true;
}

} // anonymous namespace

void Encoder::placeColdCodeNearItsJumps()
{
    if (!isX86_64() || m_proc.traits().fallsIntoWhatLiesBehind)
        return;
    // Every jump is short.
    if (m_allocation.steps().size() <= 30)
        return;
    if (m_order.rareRegions().empty() && m_order.of(m_order.blocksInLayout().back()).layoutPart == LayoutPart::Likely)
        return;

    OrderOfWriting order(*this);
    if (!order.run())
        return;
    m_beginOfRareCodeAsWritten = order.beginOfRareCode();
    // Each block with code is written once: a second copy takes the label, and the first falls into whatever comes to stand behind it.
    const uint32_t numberOfBlocksAsWritten = m_blocksAsWritten.size();
    RELEASE_ASSERT(numberOfBlocksAsWritten && numberOfBlocksAsWritten == m_numberOfBlocksWithCode);
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

    UncheckedSpan<Block* const> layout = m_order.blocksInLayout();
    ASSERT(!layout.empty());
    findBlocksWithoutCode();
    placeColdCodeNearItsJumps();
    m_pendingJumps.reserve(m_arena, m_numberOfBlocksWithCode);
    if (m_blocksAsWritten.isEmpty()) {
        const uint32_t numberOfBlocksInLayout = static_cast<uint32_t>(layout.size());
        for (uint32_t index = m_nextLayoutIndexWithCode[0]; index < numberOfBlocksInLayout;) {
            uint32_t next = m_nextLayoutIndexWithCode[index + 1];
            m_blockWrittenNext = next < numberOfBlocksInLayout ? layout[next] : nullptr;
            encodeBlock(layout[index]);
            index = next;
        }
    } else {
        const uint32_t numberOfBlocksAsWritten = m_blocksAsWritten.size();
        bool rangeOfRareCodeIsOpen = false;
        for (uint32_t index = 0; index < numberOfBlocksAsWritten; ++index) {
            m_isAmongOtherCode = index < m_beginOfRareCodeAsWritten;
            if (bool isRare = m_isAmongOtherCode && m_order.of(m_blocksAsWritten[index]).isRare(); isRare != rangeOfRareCodeIsOpen) {
                rangeOfRareCodeIsOpen = isRare;
                MacroAssembler::Label here = m_jit.labelIgnoringWatchpoints();
                if (isRare)
                    m_proc.m_rangesOfRareCodeAmongOtherCode.append(m_arena, RangeOfRareCode { here, MacroAssembler::Label() });
                else
                    m_proc.m_rangesOfRareCodeAmongOtherCode.last().end = here;
            }
            m_blockWrittenNext = index + 1 < numberOfBlocksAsWritten ? m_blocksAsWritten[index + 1] : nullptr;
            encodeBlock(m_blocksAsWritten[index]);
        }
        if (rangeOfRareCodeIsOpen)
            m_proc.m_rangesOfRareCodeAmongOtherCode.last().end = m_jit.labelIgnoringWatchpoints();
        m_isAmongOtherCode = false;
    }
    m_block = nullptr;
    m_blockWrittenNext = nullptr;

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
}

bool Encoder::fallsThroughToSlowCase(const Block* from, const Block* target) const
{
    if (from == m_block) [[likely]] {
        const Block* entered = blockEnteredThrough(target);
        if (!entered || entered != m_blockWrittenNext)
            return false;
        return m_blockIsLeftInFrame == frameAtBlock(*this, entered).isEnteredInFrame();
    }

    const BlockPlace& placeAsNamed = m_order.of(target);
    if (!placeAsNamed.isReached())
        return false;
    target = blockEnteredThrough(target);
    if (!m_blocksAsWritten.isEmpty()) {
        if (m_indexAsWritten[target->index()] != m_indexAsWritten[from->index()] + 1)
            return false;
    } else if (m_order.of(target).layoutIndex != m_nextLayoutIndexWithCode[m_order.of(from).layoutIndex + 1])
        return false;
    return frameAtBlock(*this, from).isLeftInFrame() == frameAtBlock(*this, target).isEnteredInFrame();
}

void Encoder::encodeBlock(Block* block)
{
    const BlockPlace& place = m_order.of(block);
    ASSERT(place.isReached());
    ASSERT(hasCode(block));
    MacroAssembler::Label& label = m_labels[block->index()];
    ASSERT(!label.isSet());

    m_block = block;
    label = m_jit.label();
    // Where the rare code comes first nothing lies behind its start but all of the function.
    if (place.isRare() && !m_proc.m_rareCodeStart.isSet() && !m_isAmongOtherCode && !m_proc.traits().fallsIntoWhatLiesBehind) {
        m_proc.m_rareCodeStart = label;
        noteStartOfRareCode(m_jit);
    }

    const FrameAtBlock frame = frameAtBlock(*this, block, place);
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

    const BlockSteps& steps = m_allocation.stepsOf(block);
    if (steps.begin < steps.end)
        encodeSteps(steps.begin, steps.end);
    if (steps.beginOfEdgeMoves < steps.endOfEdgeMoves)
        encodeSteps(steps.beginOfEdgeMoves, steps.endOfEdgeMoves);

    RELEASE_ASSERT(steps.terminator != UINT32_MAX);
    const Step& terminator = m_allocation.step(steps.terminator);
    ASSERT(terminator.kind == StepKind::Execute);
    ASSERT(terminator.operation == block->last());

    if ((m_coverage == FrameCoverage::RareRegions || m_coverage == FrameCoverage::FramedPart) && !frame.returnsInPlaceOfJump) {
        for (const Edge& edge : block->successors())
            RELEASE_ASSERT(frameAtBlock(*this, edge.block()).isEnteredInFrame() == m_blockIsLeftInFrame);
    }
    if (frame.isTornDownAtEnd) {
        // A Patch writes its own way out, which leaves no place for the teardown behind it.
        RELEASE_ASSERT(terminator.operation->isUnconditionalJump());
        ASSERT(m_frameIsSetUp);
        leaveFrameIfSetUp();
    }

    // A result of a Patch that ends the block is stored at the head of the first successor.
    noteOrigin(terminator.operation->origin(), terminator.operation->index());
    ++m_proc.m_numberOfOperationsEncoded;
    if (frame.returnsInPlaceOfJump) {
        RELEASE_ASSERT(terminator.operation->isUnconditionalJump() && !m_frameIsSetUp);
        m_jit.ret();
        return;
    }
    encodeExecute(terminator);
}

void Encoder::encodeSteps(uint32_t begin, uint32_t end)
{
    auto executesStore = [](const Step& candidate) {
        return candidate.kind == StepKind::Execute && candidate.operation->opcode() == Store;
    };

    for (uint32_t index = begin; index < end; ++index) {
        const Step& step = m_allocation.step(index);
        // An operation without position has the origin of whoever asked for it first in the function, so a Materialize is not booked to it.
        if (m_notesRangesOfOrigins) [[unlikely]] {
            if (step.kind == StepKind::Execute)
                noteOrigin(step.operation->origin(), step.operation->index());
            else
                noteOrigin(originOfStepOfAllocator(*this, index));
        }
        const Step* next = index + 1 < end ? &m_allocation.step(index + 1) : nullptr;

        switch (step.kind) {
        case StepKind::Execute: {
            Opcode opcode = step.operation->opcode();
            if (opcode == Store && next && executesStore(*next) && tryEncodePairOfStores(step, *next)) {
                m_proc.m_numberOfOperationsEncoded += 2;
                ++index;
                continue;
            }
            if (isARM64() && opcode == Load && next && tryEncodePairOfLoads(*this, step, *next)) {
                m_proc.m_numberOfOperationsEncoded += 2;
                storeToSpillIfSpilled(step.operation);
                storeToSpillIfSpilled(next->operation);
                ++index;
                continue;
            }
            if (isX86_64() && opcode == Add && next && tryEncodeSumAndConstant(step, *next)) {
                m_proc.m_numberOfOperationsEncoded += 2;
                storeToSpillIfSpilled(next->operation);
                ++index;
                continue;
            }
            ++m_proc.m_numberOfOperationsEncoded;
            encodeExecuteInline(step);
            storeToSpillIfSpilled(step.operation);
            continue;
        }
        case StepKind::Move:
            encodeMove(step);
            continue;
        case StepKind::Reload:
            if (isARM64() && next && next->kind == StepKind::Reload && tryEncodePairOfMoves(step, *next)) {
                ++index;
                continue;
            }
            encodeReload(step);
            continue;
        case StepKind::Save:
            if (isARM64() && next && next->kind == StepKind::Save && tryEncodePairOfMoves(step, *next)) {
                ++index;
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
        case StepKind::ClearSpill:
            encodeClearSpill(step);
            continue;
        case StepKind::KeepPointer:
            encodeKeepPointer(step);
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void Encoder::encodeExecute(const Step& step)
{
    encodeExecuteInline(step);
}

ALWAYS_INLINE void Encoder::encodeExecuteInline(const Step& step)
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

    // Made again at each use by a Materialize, or taken by an instruction as part of itself.
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    // The builder appends neither on any target.
    case Mod:
    case UMod:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Encoder::storeToSpill(const Operation* value)
{
    SpillIndex spill = m_allocation.spillOf(value);
    ASSERT(spill != noSpill);
    // Without a register at its definition, steps write the spill.
    Reg source = m_allocation.registerAtDefinitionOf(value);
    if (!source || m_allocation.spills()[spill].isStoredAtHeadOfFramedPart || setupHasPushed(spill))
        return;

    RELEASE_ASSERT(m_frameIsSetUp);
    ASSERT(m_allocation.spills()[spill].width == widthOf(value->type()));
    // Like a Save, the store is the allocator's.
    noteOrigin(originOfFrameOrStore(*this, CodeOfBackEnd::StoreBehindDefinition));
    MacroAssembler::Address address = addressOf(Location::spill(spill));
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
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Encoder::storeSpilledValuesDefinedAtHead(Block* block, const BlockPlace& place)
{
    for (Operation* phi : block->phis()) {
        if (m_allocation.isSpilled(phi) && !m_liveness.isDropped(phi))
            storeToSpill(phi);
    }

    // On the edge to a landing pad the results of the Patch do not exist.
    if (block->isLandingPad() || !m_blocksThatStoreResultOfPatchAtHead.get(block->index()))
        return;
    // On any other edge into the block the register holds something else.
    std::span<Block* const> predecessors = place.predecessors;
    RELEASE_ASSERT(predecessors.size() == 1);
    Block* predecessor = predecessors[0];
    Operation* last = predecessor->last();
    RELEASE_ASSERT(last->opcode() == Patch && predecessor->successor(0).block() == block && m_allocation.isSpilled(last));
    storeToSpill(last);
}

void Encoder::setUpFrame(const Block* block)
{
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    noteOrigin(originOfFrameOrStore(*this, CodeOfBackEnd::FrameSetup));
    emitFrameSetup(m_jit, m_proc, findRegistersThatSetupPushes(block));
    noteOrigin(Origin());
    noteFrameState(true);
}

void Encoder::leaveFrameIfSetUp()
{
    if (!m_frameIsSetUp)
        return;

    noteFrameState(false);
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    Origin originOfUser = m_origin;
    uint32_t operationOfUser = m_operationOfOrigin;
    noteOrigin(originOfFrameOrStore(*this, CodeOfBackEnd::FrameTeardown));
    emitFrameTeardown(m_jit, m_frame);
    noteOrigin(originOfUser, operationOfUser);
}

void Encoder::startOrEndFrameRange(bool isSetUp)
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
    // label() forgets what the scratch registers hold, and the bytes behind it would differ.
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
    // A late path may add late paths, which moves the list.
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

// A return is no longer than a jump to one.
void Encoder::jumpTo(const Block* target)
{
    if (!m_frameIsSetUp && m_blocksThatOnlyReturnWithoutFrame.get(blockEnteredThrough(target)->index())) {
        m_jit.ret();
        return;
    }
    addPendingJump(m_jit.jump(), target);
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

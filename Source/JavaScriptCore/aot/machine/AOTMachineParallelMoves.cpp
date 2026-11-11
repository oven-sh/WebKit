/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocation.h"
#include "AOTInliningAcrossFiles.h"

#if ENABLE(AOT)

#include <algorithm>
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static_assert(sizeof(Location) == sizeof(uint32_t));
static_assert(std::bit_cast<uint32_t>(Location::spill(1)) == (static_cast<uint32_t>(Location::Spill) << 28 | 1));
static_assert(Location::Register < Location::Spill && Location::Spill < Location::Outgoing);

static ALWAYS_INLINE bool isPlaceOfParallelMove(Location location)
{
    constexpr uint32_t kindsOfPlaces = 1u << Location::Register | 1u << Location::Spill | 1u << Location::Outgoing;
    return kindsOfPlaces >> location.kind() & 1;
}

static ALWAYS_INLINE uint64_t parallelMoveLocationKey(Location location)
{
    RELEASE_ASSERT(isPlaceOfParallelMove(location));
    return std::bit_cast<uint32_t>(location);
}

static ALWAYS_INLINE uint64_t parallelMoveSourceKey(const PendingMove& move)
{
    if (move.source.isImmediate())
        return uint64_t { 1 } << 32 | move.value->index();
    return parallelMoveLocationKey(move.source);
}

static constexpr Reg parallelMoveScratchRegister(Bank bank)
{
    return bank == Bank::GP ? Reg(scratchGPR) : Reg(scratchFPR);
}

enum class ParallelMoveRank : uint8_t {
    Save,
    Move,
    Load,
    ThroughScratch,
};
static constexpr unsigned numberOfParallelMoveRanks = 4;

static ALWAYS_INLINE ParallelMoveRank rankOfParallelMove(bool destinationIsRegister, bool valueIsInRegister)
{
    if (destinationIsRegister)
        return valueIsInRegister ? ParallelMoveRank::Move : ParallelMoveRank::Load;
    return valueIsInRegister ? ParallelMoveRank::Save : ParallelMoveRank::ThroughScratch;
}

static ALWAYS_INLINE Step& constructParallelMoveStep(Step* at, StepKind kind, Width width, Location destination, Location source, Operation* value)
{
    return *new (NotNull, at) Step { kind, width, UINT32_MAX, destination, { source }, value, nullptr };
}

class ParallelMoveResolver {
    WTF_MAKE_NONCOPYABLE(ParallelMoveResolver);
public:
    static constexpr uint32_t mostMovesOfSmallSet = 16;

    ParallelMoveResolver(Arena& arena, std::span<PendingMove> moves, ArenaVector<Step>& output)
        : m_arena(arena)
        , m_moves(moves)
        , m_output(output)
    {
    }

    void runOnSmallSet();
    void runOnLargeSet();

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr unsigned endOfRegisterIndexes = 1 << 7;

    struct Source {
        uint32_t numberOfPendingReaders;
        uint32_t writer;
        Reg holder;
        Width maxWidth;
        bool isReleased;
    };

    struct MoveState {
        uint32_t source;
        uint32_t overwrittenSource;
        bool isDone;
    };

    struct ReadyMoves {
        uint32_t* moves { nullptr };
        uint32_t next { 0 };
        uint32_t end { 0 };
    };

    Source& sourceOf(uint32_t moveIndex) { return m_sources[m_states[moveIndex].source]; }
    const PendingMove& moveAt(uint32_t moveIndex) const { return m_moves.data()[moveIndex]; }

    void groupMovesOfSmallSetBySource();
    void groupMovesBySource();
    void resolveGroupedMoves(uint32_t* readyMoves);
    void enqueue(uint32_t moveIndex);
    void performReadyMoves();
    void perform(uint32_t moveIndex);
    void release(Source&);
    Reg loadIntoScratch(const PendingMove&, uint32_t sourceIndex);
    void breakCycle(uint32_t first);
    Step& append(StepKind, const PendingMove&, Location destination, Location source);

    Arena& m_arena;
    std::span<PendingMove> m_moves;
    ArenaVector<Step>& m_output;
    Step* m_nextStep { nullptr };
    MoveState* m_states { nullptr };
    Source* m_sources { nullptr };
    ReadyMoves m_ready[numberOfParallelMoveRanks];
    uint32_t m_sourceInScratch[numberOfBanks] { none, none };
    Reg m_cycleScratch;
};

ALWAYS_INLINE void ParallelMoveResolver::runOnSmallSet()
{
    ASSERT(m_moves.size() <= mostMovesOfSmallSet);
    MoveState statesOfSmallSet[mostMovesOfSmallSet];
    alignas(Source) std::byte sourcesOfSmallSet[mostMovesOfSmallSet * sizeof(Source)];
    uint32_t readyMovesOfSmallSet[numberOfParallelMoveRanks * mostMovesOfSmallSet];
    m_states = statesOfSmallSet;
    m_sources = reinterpret_cast<Source*>(sourcesOfSmallSet);
    groupMovesOfSmallSetBySource();
    resolveGroupedMoves(readyMovesOfSmallSet);
}

ALWAYS_INLINE void ParallelMoveResolver::runOnLargeSet()
{
    const size_t numberOfMoves = m_moves.size();
    m_states = m_arena.allocateArray<MoveState>(numberOfMoves);
    m_sources = m_arena.allocateArray<Source>(numberOfMoves);
    groupMovesBySource();
    resolveGroupedMoves(m_arena.allocateArray<uint32_t>(numberOfMoves * numberOfParallelMoveRanks));
}

ALWAYS_INLINE void ParallelMoveResolver::resolveGroupedMoves(uint32_t* readyMoves)
{
    const uint32_t numberOfMoves = static_cast<uint32_t>(m_moves.size());
    const uint32_t mostSteps = 3 * numberOfMoves;
    Step* const firstStep = m_output.appendUninitialized(m_arena, mostSteps);
    m_nextStep = firstStep;

    for (unsigned rank = 0; rank < numberOfParallelMoveRanks; ++rank)
        m_ready[rank].moves = readyMoves + static_cast<size_t>(rank) * numberOfMoves;

    for (uint32_t index = 0; index < numberOfMoves; ++index) {
        if (m_states[index].overwrittenSource == none)
            enqueue(index);
    }
    performReadyMoves();

    for (uint32_t index = 0; index < numberOfMoves; ++index) {
        if (!m_states[index].isDone)
            breakCycle(index);
    }

    const uint32_t numberOfSteps = static_cast<uint32_t>(m_nextStep - firstStep);
    ASSERT(numberOfSteps <= mostSteps);
    m_output.shrink(m_output.size() - (mostSteps - numberOfSteps));
}

ALWAYS_INLINE void ParallelMoveResolver::groupMovesOfSmallSetBySource()
{
    const uint32_t numberOfMoves = static_cast<uint32_t>(m_moves.size());
    PendingMove* const moves = m_moves.data();
    MoveState* const states = m_states;
    Source* const sources = m_sources;

    uint64_t destinations[mostMovesOfSmallSet];
    for (uint32_t index = 0; index < numberOfMoves; ++index) {
        uint64_t destination = parallelMoveLocationKey(moves[index].destination);
        if (!index || destinations[index - 1] <= destination) {
            destinations[index] = destination;
            continue;
        }
        PendingMove move = moves[index];
        uint32_t place = index;
        do {
            destinations[place] = destinations[place - 1];
            moves[place] = moves[place - 1];
            --place;
        } while (place && destination < destinations[place - 1]);
        destinations[place] = destination;
        moves[place] = move;
    }

    RegisterMask registersWritten = 0;
    uint8_t writerOfRegister[endOfRegisterIndexes];
    uint32_t numberOfMovesIntoRegisters = 0;
    for (uint32_t index = 0; index < numberOfMoves; ++index) {
        const PendingMove& move = moves[index];
        ASSERT(move.width == Width32 || move.width == Width64);
        ASSERT(!move.destination.isRegister() || bankOf(move.destination.reg()) == bankOf(move.value->type()));
        ASSERT(!move.source.isRegister() || bankOf(move.source.reg()) == bankOf(move.value->type()));
        ASSERT(!move.source.isImmediate() || move.value->hasNoPosition());
        RELEASE_ASSERT(!index || moves[index - 1].destination != move.destination);
        states[index] = MoveState { none, none, false };
        if (move.destination.isRegister()) {
            Reg reg = move.destination.reg();
            registersWritten |= maskOf(reg);
            writerOfRegister[reg.index()] = static_cast<uint8_t>(index);
            ++numberOfMovesIntoRegisters;
        }
    }

    uint32_t numberOfSources = 0;
    auto addSource = [&](Reg holder, uint32_t writer) ALWAYS_INLINE_LAMBDA {
        uint32_t sourceIndex = numberOfSources++;
        new (NotNull, sources + sourceIndex) Source { 0, writer, holder, Width32, false };
        if (writer != none)
            states[writer].overwrittenSource = sourceIndex;
        return sourceIndex;
    };

    RegisterMask registersRead = 0;
    uint8_t sourceInRegister[endOfRegisterIndexes];
    uint64_t keysOutsideRegisters[mostMovesOfSmallSet];
    uint8_t sourcesOutsideRegisters[mostMovesOfSmallSet];
    uint32_t numberOfSourcesOutsideRegisters = 0;
    for (uint32_t index = 0; index < numberOfMoves; ++index) {
        const PendingMove& move = moves[index];
        uint32_t sourceIndex;
        if (move.source.isRegister()) {
            Reg reg = move.source.reg();
            RegisterMask mask = maskOf(reg);
            if (registersRead & mask)
                sourceIndex = sourceInRegister[reg.index()];
            else {
                registersRead |= mask;
                sourceIndex = addSource(reg, registersWritten & mask ? writerOfRegister[reg.index()] : none);
                sourceInRegister[reg.index()] = static_cast<uint8_t>(sourceIndex);
            }
        } else {
            uint64_t key = parallelMoveSourceKey(move);
            uint32_t known = 0;
            while (known < numberOfSourcesOutsideRegisters && keysOutsideRegisters[known] != key)
                ++known;
            if (known < numberOfSourcesOutsideRegisters)
                sourceIndex = sourcesOutsideRegisters[known];
            else {
                uint32_t writer = numberOfMovesIntoRegisters;
                while (writer < numberOfMoves && destinations[writer] != key)
                    ++writer;
                sourceIndex = addSource(Reg(), writer < numberOfMoves ? writer : none);
                keysOutsideRegisters[numberOfSourcesOutsideRegisters] = key;
                sourcesOutsideRegisters[numberOfSourcesOutsideRegisters++] = static_cast<uint8_t>(sourceIndex);
            }
        }

        Source& source = sources[sourceIndex];
        ++source.numberOfPendingReaders;
        source.maxWidth = std::max(source.maxWidth, move.width);
        states[index].source = sourceIndex;
    }
}

NEVER_INLINE void ParallelMoveResolver::groupMovesBySource()
{
    struct Reader {
        uint64_t key { 0 };
        uint32_t move { 0 };
    };

    uint32_t numberOfMoves = static_cast<uint32_t>(m_moves.size());
    std::sort(m_moves.begin(), m_moves.end(), [](const PendingMove& left, const PendingMove& right) {
        return parallelMoveLocationKey(left.destination) < parallelMoveLocationKey(right.destination);
    });

    std::span<Reader> readers = m_arena.createSpan<Reader>(numberOfMoves);
    for (uint32_t index = 0; index < numberOfMoves; ++index) {
        const PendingMove& move = m_moves[index];
        ASSERT(move.width == Width32 || move.width == Width64);
        ASSERT(!move.destination.isRegister() || bankOf(move.destination.reg()) == bankOf(move.value->type()));
        ASSERT(!move.source.isRegister() || bankOf(move.source.reg()) == bankOf(move.value->type()));
        ASSERT(!move.source.isImmediate() || move.value->hasNoPosition());
        RELEASE_ASSERT(!index || m_moves[index - 1].destination != move.destination);
        readers[index] = { parallelMoveSourceKey(move), index };
        m_states[index] = MoveState { none, none, false };
    }
    std::sort(readers.begin(), readers.end(), [](const Reader& left, const Reader& right) {
        return left.key != right.key ? left.key < right.key : left.move < right.move;
    });

    uint32_t numberOfSources = 0;
    uint32_t writer = 0;
    for (uint32_t index = 0; index < numberOfMoves; ++index) {
        const Reader& reader = readers[index];
        const PendingMove& move = m_moves[reader.move];
        if (!index || readers[index - 1].key != reader.key) {
            Source& source = *new (NotNull, m_sources + numberOfSources++) Source { 0, none, Reg(), Width32, false };
            if (move.source.isRegister())
                source.holder = move.source.reg();
            while (writer < numberOfMoves && parallelMoveLocationKey(m_moves[writer].destination) < reader.key)
                ++writer;
            if (writer < numberOfMoves && parallelMoveLocationKey(m_moves[writer].destination) == reader.key) {
                source.writer = writer;
                m_states[writer].overwrittenSource = numberOfSources - 1;
            }
        }

        uint32_t sourceIndex = numberOfSources - 1;
        Source& source = m_sources[sourceIndex];
        ++source.numberOfPendingReaders;
        source.maxWidth = std::max(source.maxWidth, move.width);
        m_states[reader.move].source = sourceIndex;
    }
}

ALWAYS_INLINE void ParallelMoveResolver::enqueue(uint32_t moveIndex)
{
    ParallelMoveRank rank = rankOfParallelMove(moveAt(moveIndex).destination.isRegister(), sourceOf(moveIndex).holder.isSet());
    ReadyMoves& ready = m_ready[static_cast<unsigned>(rank)];
    ready.moves[ready.end++] = moveIndex;
}

ALWAYS_INLINE void ParallelMoveResolver::performReadyMoves()
{
    for (unsigned rank = 0; rank < numberOfParallelMoveRanks;) {
        ReadyMoves& ready = m_ready[rank];
        if (ready.next == ready.end) {
            ++rank;
            continue;
        }
        perform(ready.moves[ready.next++]);
        rank = 0;
    }
}

ALWAYS_INLINE void ParallelMoveResolver::perform(uint32_t moveIndex)
{
    const PendingMove& move = moveAt(moveIndex);
    MoveState& state = m_states[moveIndex];
    Source& source = m_sources[state.source];
    ASSERT(!state.isDone);
    ASSERT(state.overwrittenSource == none || m_sources[state.overwrittenSource].isReleased);

    if (move.destination.isRegister()) {
        if (source.holder)
            append(StepKind::Move, move, move.destination, Location::reg(source.holder));
        else
            append(move.source.isImmediate() ? StepKind::Materialize : StepKind::Reload, move, move.destination, move.source);

        if (move.width == source.maxWidth) {
            source.holder = move.destination.reg();
            release(source);
        }
    } else {
        Reg holder = source.holder ? source.holder : loadIntoScratch(move, state.source);
        append(StepKind::Save, move, move.destination, Location::reg(holder));
    }

    state.isDone = true;
    ASSERT(source.numberOfPendingReaders);
    if (!--source.numberOfPendingReaders)
        release(source);
}

ALWAYS_INLINE_INTO_OTHER_FILES void ParallelMoveResolver::release(Source& source)
{
    if (source.isReleased)
        return;
    source.isReleased = true;
    if (source.writer != none)
        enqueue(source.writer);
}

ALWAYS_INLINE Reg ParallelMoveResolver::loadIntoScratch(const PendingMove& move, uint32_t sourceIndex)
{
    bool isRematerialized = move.source.isImmediate();
    Bank bank = bankOf(move.value->type());
    if (!isRematerialized && (isX86_64() || m_cycleScratch))
        bank = Bank::FP;
    Reg scratch = parallelMoveScratchRegister(bank);

    uint32_t& sourceInScratch = m_sourceInScratch[static_cast<unsigned>(bank)];
    if (sourceInScratch != sourceIndex) {
        Step& step = append(isRematerialized ? StepKind::Materialize : StepKind::Reload, move, Location::reg(scratch), move.source);
        step.width = m_sources[sourceIndex].maxWidth;
        sourceInScratch = sourceIndex;
    }
    return scratch;
}

ALWAYS_INLINE void ParallelMoveResolver::breakCycle(uint32_t first)
{
    uint32_t last = first;
    uint32_t memoryToMemoryMove = none;
    uint32_t numberOfMoves = 1;
    bool passesThroughMemory = false;
    for (;;) {
        const PendingMove& move = moveAt(last);
        const Source& source = sourceOf(last);
        RELEASE_ASSERT(!m_states[last].isDone && !source.isReleased && source.numberOfPendingReaders == 1 && source.writer != none);
        if (!move.destination.isRegister()) {
            passesThroughMemory = true;
            if (memoryToMemoryMove == none && !move.source.isRegister())
                memoryToMemoryMove = last;
        }
        if (source.writer == first)
            break;
        last = source.writer;
        ++numberOfMoves;
    }

    bool swapsInOneInstruction = isX86_64() && !passesThroughMemory && bankOf(moveAt(first).destination.reg()) == Bank::GP;
    bool rotatesThroughScratch = !swapsInOneInstruction && numberOfMoves > 2;
    if (!passesThroughMemory && !rotatesThroughScratch) {
        const PendingMove& lastMove = moveAt(last);
        for (uint32_t index = first; index != last; index = sourceOf(index).writer) {
            const PendingMove& move = moveAt(index);
            Step& step = append(StepKind::Swap, move, move.destination, move.source);
            step.width = std::max(move.width, lastMove.width);
            step.other = lastMove.value;
            m_states[index].isDone = true;
        }
        m_states[last].isDone = true;
        return;
    }

    uint32_t cycleBreaker = memoryToMemoryMove != none ? memoryToMemoryMove : last;
    const PendingMove& move = moveAt(cycleBreaker);
    Source& source = sourceOf(cycleBreaker);
    ASSERT(move.source.isRegister() || !move.destination.isRegister());
    Reg scratch = move.source.isRegister() ? parallelMoveScratchRegister(bankOf(move.source.reg())) : Reg(scratchGPR);
    append(move.source.isRegister() ? StepKind::Move : StepKind::Reload, move, Location::reg(scratch), move.source);
    source.holder = scratch;
    m_cycleScratch = scratch;
    release(source);
    performReadyMoves();
    m_cycleScratch = Reg();
    ASSERT(m_states[first].isDone && m_states[last].isDone);
}

ALWAYS_INLINE_INTO_OTHER_FILES Step& ParallelMoveResolver::append(StepKind kind, const PendingMove& move, Location destination, Location source)
{
    RegisterMask written = destination.isRegister() ? maskOf(destination.reg()) : 0;
    switch (kind) {
    case StepKind::Move:
    case StepKind::Reload:
    case StepKind::Save:
        break;
    case StepKind::Materialize:
        written |= maskOf(Reg(scratchGPR));
        break;
    case StepKind::Swap:
        written |= maskOf(Reg(scratchGPR)) | maskOf(Reg(scratchFPR));
        break;
    case StepKind::Execute:
    case StepKind::KeepPointer:
        RELEASE_ASSERT_NOT_REACHED();
    }
    if (written & (maskOf(Reg(scratchGPR)) | maskOf(Reg(scratchFPR)))) {
        RELEASE_ASSERT(!m_cycleScratch || !(written & maskOf(m_cycleScratch)));
        for (Bank bank : { Bank::GP, Bank::FP }) {
            if (written & maskOf(parallelMoveScratchRegister(bank)))
                m_sourceInScratch[static_cast<unsigned>(bank)] = none;
        }
    }

    return constructParallelMoveStep(m_nextStep++, kind, move.width, destination, source, move.value);
}

static NEVER_INLINE std::span<PendingMove> removeTrivialParallelMoves(std::span<PendingMove> moves)
{
    size_t newSize = 0;
    for (const PendingMove& move : moves) {
        if (!move.source) {
            Block* owner = move.value->owner();
            RELEASE_ASSERT(owner);
            owner->procedure().fail(FailureKind::WrongValueInRegister, move.value);
            continue;
        }
        if (move.source != move.destination)
            moves[newSize++] = move;
    }
    return moves.first(newSize);
}

static ALWAYS_INLINE void constructStepsOfIndependentParallelMove(Step*& next, const PendingMove& move)
{
    Operation* const value = move.value;
    const Location destination = move.destination;
    const Location source = move.source;
    const Width width = move.width;

    if (destination.isRegister()) {
        StepKind kind = StepKind::Move;
        if (!source.isRegister())
            kind = source.isImmediate() ? StepKind::Materialize : StepKind::Reload;
        constructParallelMoveStep(next++, kind, width, destination, source, value);
        return;
    }

    Location holder = source;
    if (!source.isRegister()) {
        bool isRematerialized = source.isImmediate();
        Bank bank = bankOf(value->type());
        if (!isRematerialized && isX86_64())
            bank = Bank::FP;
        holder = Location::reg(parallelMoveScratchRegister(bank));
        constructParallelMoveStep(next++, isRematerialized ? StepKind::Materialize : StepKind::Reload, std::max(Width32, width), holder, source, value);
    }
    constructParallelMoveStep(next++, StepKind::Save, width, destination, holder, value);
}

static ALWAYS_INLINE bool tryResolveIndependentParallelMoves(Arena& arena, std::span<const PendingMove> moves, ArenaVector<Step>& output)
{
    constexpr size_t mostMoves = 8;
    constexpr unsigned bitsOfIndex = 3;
    static_assert(mostMoves == 1 << bitsOfIndex);
    const size_t numberOfMoves = moves.size();
    if (numberOfMoves > mostMoves)
        return false;

    const PendingMove* const firstMove = moves.data();
    RegisterMask registersNamed = 0;
    uint64_t keysOutsideRegisters[2 * mostMoves];
    size_t numberOfKeysOutsideRegisters = 0;
    auto isNamedForFirstTime = [&](uint64_t key) ALWAYS_INLINE_LAMBDA {
        for (size_t earlier = 0; earlier < numberOfKeysOutsideRegisters; ++earlier) {
            if (keysOutsideRegisters[earlier] == key)
                return false;
        }
        keysOutsideRegisters[numberOfKeysOutsideRegisters++] = key;
        return true;
    };

    uint64_t order[mostMoves];
    for (size_t index = 0; index < numberOfMoves; ++index) {
        const PendingMove& move = firstMove[index];
        const uint64_t destination = parallelMoveLocationKey(move.destination);
        const bool destinationIsRegister = move.destination.isRegister();
        const bool sourceIsRegister = move.source.isRegister();
        if (destinationIsRegister) {
            RegisterMask mask = maskOf(move.destination.reg());
            if (registersNamed & mask)
                return false;
            registersNamed |= mask;
        } else if (!isNamedForFirstTime(destination))
            return false;
        if (sourceIsRegister) {
            RegisterMask mask = maskOf(move.source.reg());
            if (registersNamed & mask)
                return false;
            registersNamed |= mask;
        } else if (!isNamedForFirstTime(parallelMoveSourceKey(move)))
            return false;
        order[index] = (static_cast<uint64_t>(rankOfParallelMove(destinationIsRegister, sourceIsRegister)) << 32 | destination) << bitsOfIndex | index;
    }

    for (size_t index = 1; index < numberOfMoves; ++index) {
        uint64_t key = order[index];
        size_t place = index;
        for (; place && key < order[place - 1]; --place)
            order[place] = order[place - 1];
        order[place] = key;
    }

    const uint32_t mostSteps = 2 * static_cast<uint32_t>(numberOfMoves);
    Step* const firstStep = output.appendUninitialized(arena, mostSteps);
    Step* next = firstStep;
    for (size_t index = 0; index < numberOfMoves; ++index)
        constructStepsOfIndependentParallelMove(next, firstMove[order[index] & (mostMoves - 1)]);
    output.shrink(output.size() - (mostSteps - static_cast<uint32_t>(next - firstStep)));
    return true;
}

static NEVER_INLINE void resolveManyDependentParallelMoves(Arena& arena, std::span<PendingMove> moves, ArenaVector<Step>& output)
{
    ParallelMoveResolver resolver(arena, moves, output);
    resolver.runOnLargeSet();
}

static NEVER_INLINE void resolveDependentParallelMoves(Arena& arena, std::span<PendingMove> moves, ArenaVector<Step>& output)
{
    if (moves.size() > ParallelMoveResolver::mostMovesOfSmallSet) [[unlikely]] {
        resolveManyDependentParallelMoves(arena, moves, output);
        return;
    }
    ParallelMoveResolver resolver(arena, moves, output);
    resolver.runOnSmallSet();
}

static NEVER_INLINE void resolveSeveralParallelMoves(Arena& arena, std::span<PendingMove> moves, ArenaVector<Step>& output)
{
    for (const PendingMove& move : moves) {
        if (!move.source || move.source == move.destination) [[unlikely]] {
            moves = removeTrivialParallelMoves(moves);
            break;
        }
    }
    if (moves.empty())
        return;

    if (tryResolveIndependentParallelMoves(arena, moves, output))
        return;
    resolveDependentParallelMoves(arena, moves, output);
}

static NEVER_INLINE void resolveTwoParallelMoves(Arena& arena, std::span<PendingMove> moves, ArenaVector<Step>& output)
{
    const PendingMove& first = moves.data()[0];
    const PendingMove& second = moves.data()[1];
    if (!first.source || first.source == first.destination || !second.source || second.source == second.destination) [[unlikely]] {
        resolveSeveralParallelMoves(arena, moves, output);
        return;
    }

    const uint64_t destinationOfFirst = parallelMoveLocationKey(first.destination);
    const uint64_t destinationOfSecond = parallelMoveLocationKey(second.destination);
    const uint64_t sourceOfFirst = parallelMoveSourceKey(first);
    const uint64_t sourceOfSecond = parallelMoveSourceKey(second);
    const bool firstWritesWhatSecondReads = destinationOfFirst == sourceOfSecond;
    const bool secondWritesWhatFirstReads = destinationOfSecond == sourceOfFirst;
    const bool bothReadOneSource = sourceOfFirst == sourceOfSecond;
    const bool firstGoesIntoRegister = first.destination.isRegister();
    const bool secondGoesIntoRegister = second.destination.isRegister();
    const bool firstComesFromRegister = first.source.isRegister();
    const bool secondComesFromRegister = second.source.isRegister();

    if (destinationOfFirst != destinationOfSecond && !firstWritesWhatSecondReads && !secondWritesWhatFirstReads && !bothReadOneSource) {
        const uint64_t orderOfFirst = static_cast<uint64_t>(rankOfParallelMove(firstGoesIntoRegister, firstComesFromRegister)) << 32 | destinationOfFirst;
        const uint64_t orderOfSecond = static_cast<uint64_t>(rankOfParallelMove(secondGoesIntoRegister, secondComesFromRegister)) << 32 | destinationOfSecond;
        constexpr uint32_t mostSteps = 4;
        Step* const firstStep = output.appendUninitialized(arena, mostSteps);
        Step* next = firstStep;
        constructStepsOfIndependentParallelMove(next, orderOfSecond < orderOfFirst ? second : first);
        constructStepsOfIndependentParallelMove(next, orderOfSecond < orderOfFirst ? first : second);
        output.shrink(output.size() - (mostSteps - static_cast<uint32_t>(next - firstStep)));
        return;
    }
    if (destinationOfFirst == destinationOfSecond || !firstGoesIntoRegister || !secondGoesIntoRegister || !firstComesFromRegister || !secondComesFromRegister) {
        resolveDependentParallelMoves(arena, moves, output);
        return;
    }

    ASSERT(first.width == Width32 || first.width == Width64);
    ASSERT(second.width == Width32 || second.width == Width64);
    ASSERT(bankOf(first.destination.reg()) == bankOf(first.value->type()) && bankOf(first.source.reg()) == bankOf(first.value->type()));
    ASSERT(bankOf(second.destination.reg()) == bankOf(second.value->type()) && bankOf(second.source.reg()) == bankOf(second.value->type()));
    const PendingMove& lower = destinationOfSecond < destinationOfFirst ? second : first;
    const PendingMove& higher = destinationOfSecond < destinationOfFirst ? first : second;
    Step* const steps = output.appendUninitialized(arena, 2);
    if (bothReadOneSource) {
        constructParallelMoveStep(steps, StepKind::Move, lower.width, lower.destination, lower.source, lower.value);
        const bool lowerHasAllOfValue = lower.width == std::max(Width32, std::max(lower.width, higher.width));
        constructParallelMoveStep(steps + 1, StepKind::Move, higher.width, higher.destination, lowerHasAllOfValue ? lower.destination : higher.source, higher.value);
        return;
    }
    if (firstWritesWhatSecondReads && secondWritesWhatFirstReads) {
        constructParallelMoveStep(steps, StepKind::Swap, std::max(lower.width, higher.width), lower.destination, lower.source, lower.value).other = higher.value;
        output.shrink(output.size() - 1);
        return;
    }
    const PendingMove& reader = firstWritesWhatSecondReads ? second : first;
    const PendingMove& writer = firstWritesWhatSecondReads ? first : second;
    constructParallelMoveStep(steps, StepKind::Move, reader.width, reader.destination, reader.source, reader.value);
    constructParallelMoveStep(steps + 1, StepKind::Move, writer.width, writer.destination, writer.source, writer.value);
}

void resolveParallelMoves(Arena& arena, std::span<PendingMove> moves, ArenaVector<Step>& output)
{
    if (moves.size() == 1) {
        const PendingMove& move = *moves.data();
        if (move.source && move.source != move.destination) [[likely]] {
            RELEASE_ASSERT(isPlaceOfParallelMove(move.destination) && (move.source.isImmediate() || isPlaceOfParallelMove(move.source)));
            if (move.destination.isRegister()) {
                Step* next = output.appendUninitialized(arena);
                constructStepsOfIndependentParallelMove(next, move);
                return;
            }
            Step* const firstStep = output.appendUninitialized(arena, 2);
            Step* next = firstStep;
            constructStepsOfIndependentParallelMove(next, move);
            output.shrink(output.size() - (2 - static_cast<uint32_t>(next - firstStep)));
            return;
        }
    }
    if (moves.size() == 2) {
        resolveTwoParallelMoves(arena, moves, output);
        return;
    }
    resolveSeveralParallelMoves(arena, moves, output);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

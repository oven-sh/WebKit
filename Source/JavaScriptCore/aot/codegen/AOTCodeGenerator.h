/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCode.h"
#include "AOTEffects.h"
#include "AOTEmitterList.h"
#include "AOTFacts.h"
#include "AOTHeaps.h"
#include "AOTIRProgram.h"
#include "AOTOperand.h"
#include "AOTOperationContext.h"
#include "AOTPlan.h"
#include "AOTTypeArena.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class MentionList;
class StubCatalog;

namespace Typed {

struct CodeRecords;
struct FormAtPlace;

struct CodeGenerationInputs {
    const Program& program;
    const Facts& facts;
    const Plan& plan;
    const StubCatalog& stubCatalog;
    const AOTHeaps& heaps;
};

struct FunctionInputs {
    BuildContext& build;
    TypeArena& types;
    const CodeGenerationInputs& program;
    const Code& code;
};

struct PinnedValues {
    UncheckedKeyHashMap<LValue, LValue> wordsOfInt32Elements;
    LValue instance { nullptr };
    LValue instancePrefix { nullptr };
    LValue numberTag { nullptr };
    LValue notCellMask { nullptr };
    LValue cellCageBase { nullptr };
};

enum class GenerationResult : uint8_t { Failed, Generated };

inline Machine::Origin machineOriginOf(ValueID id)
{
    return Machine::Origin(id ? id.index() + 1 : 0);
}

inline constexpr uint32_t firstOriginOfTerminators = 0xFFFFFE00;

inline Machine::Origin machineOriginOf(TerminatorKind kind)
{
    return Machine::Origin(firstOriginOfTerminators + static_cast<uint32_t>(kind));
}

inline ValueID valueOfOrigin(Machine::Origin origin)
{
    return origin && origin.bits() < firstOriginOfTerminators ? ValueID(origin.bits() - 1) : ValueID();
}

class CodeGenerator {
    WTF_MAKE_NONCOPYABLE(CodeGenerator);
public:
    CodeGenerator(const FunctionInputs&, Machine::Procedure&, CodeRecords&);
    ~CodeGenerator();

    GenerationResult run();

    const FunctionInputs& inputs() const LIFETIME_BOUND { return m_inputs; }
    BuildContext& build() { return m_inputs.build; }
    const Program& program() const { return m_inputs.program.program; }
    const Facts& facts() const { return m_inputs.program.facts; }
    const Plan& plan() const { return m_inputs.program.plan; }
    const Code& code() const { return m_inputs.code; }
    BlockID singlePredecessorOf(BlockID block) const { return m_singlePredecessors[block.index()]; }
    ValueID testThatFoundElement;
    LValue elementFound { nullptr };
    CodeUnit unit() const { return code().unit(); }
    const Function& function() const;
    Machine::Procedure& procedure() { return m_proc; }
    const AOTHeaps& heaps() const { return m_inputs.program.heaps; }
    Output& out() LIFETIME_BOUND { return m_out; }
    PinnedValues& pinned() LIFETIME_BOUND { return m_pinned; }
    CodeRecords& records() { return m_records; }
    MentionList& mentions();
    ValueID operationBeingGenerated() const { return m_operation ? m_operation->valueID() : ValueID(); }
    BlockID blockBeingGenerated() const { return m_operation ? m_operation->block() : BlockID(); }

    const Operation& operationAt(ValueID id) const LIFETIME_BOUND { return code().at(id); }
    Opcode opcodeOf(ValueID id) const { return code().at(id).opcode; }
    TypeID typeIDOf(ValueID id) const { return code().at(id).type; }
    Representation representationOf(ValueID id) const { return code().at(id).representation; }
    Type typeOf(ValueID id) const { return m_inputs.types.at(typeIDOf(id)); }

    bool suspends() const { return m_proc.traits().savesFrameOnSuspend; }
    bool hasHandlers() const { return code().hasHandlers(); }
    bool isInLoop(BlockID block) const { return code().at(block).isInLoop; }
    bool isStoredIntoByName(ValueID) const;
    bool takesOpenForm(const OperationContext&, StateOfKnowledge = StateOfKnowledge::Always) const;
    bool takesFormOfLoops(const OperationContext&, StateOfKnowledge = StateOfKnowledge::Always) const;
    bool paysForCodeInLine(BlockID block) const { return isInLoop(block); }
    bool laterBarrierCovers(ValueID store) const { return code().marksOf(store).contains(SweepMark::LaterBarrierCovers); }
    bool ownerIsYoung(ValueID store) const { return code().marksOf(store).contains(SweepMark::OwnerIsYoung); }
    bool laterFenceCovers(ValueID allocation) const { return code().marksOf(allocation).contains(SweepMark::LaterFenceCovers); }
    bool isDeferredClosure(ValueID id) const { return isDeferredClosure(code().at(id)); }
    const PlacedSlot* slotOfScopeReadInPlaceOfClosure(ValueID id, const Operation& read) const
    {
        if (read.opcode != Opcode::GetDeferredClosureSlot) [[likely]]
            return nullptr;
        return slotOfScopeReadInPlaceOfClosureSlow(id, read);
    }
    const PlacedSlot* slotOfScopeOfClosureWithoutValue(ValueID closure) const { return closure == m_closureReadForItsScope ? m_slotOfScopeOfClosureRead : nullptr; }
    const Effects& effectsOf(const OperationContext& context LIFETIME_BOUND) const
    {
        if (!context.m_effects)
            computeEffectsOf(context);
        return *context.m_effects;
    }

    Operand get(ValueID);
    void verifyThatDefinitionDominates(ValueID, BlockID user);
    LValue wordOf(ValueID id) const { return m_values[id.index()]; }
    Operand getIn(ValueID, Representation);
    bool holdsAnyNaN(ValueID id) const { return m_followsNumbersOutOfBytes && m_valuesHoldingAnyNaN.quickGet(id.index()); }
    Operand getWithAnyNaN(ValueID);
    bool isCarriedBoxed(ValueID id) const { return m_carriesParametersBoxed && m_parametersCarriedBoxed.quickGet(id.index()); }
    ValueID sumBoxedBy(const Operation& box) const;
    Number settleNaNOf(const ValueContext&, Number);
    void define(const ValueContext&, Operand);
    void defineTest(const ValueContext&, Condition);
    void defineResults(const ValueContext&, std::span<const Operand>);
    Operand resultAt(ValueID producer, uint32_t index) const;
    void defineParameter(ValueID parameter, Operand);

    LBasicBlock blockFor(BlockID) const;
    BlockEdge edgeTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    void jumpTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    void passHandlerArguments(const Edge& handler);

    void generateBranchCondition(const TerminatorContext&, ValueID condition, BranchTargets);
    void generateReturnedCall(const TerminatorContext&, ValueID call);

    template<typename T> T& emitter();

    Subject subjectOf(const OperationContext&) const;
    void fail(const OperationContext&, InternalErrorKind, const String& detail = { });
    void failAtLimit(const OperationContext&, LimitID, uint64_t needed);
    bool hasFailed() const { return m_hasFailed; }

    class InitializationScope {
        WTF_MAKE_NONCOPYABLE(InitializationScope);
    public:
        explicit InitializationScope(CodeGenerator&);
        ~InitializationScope();

    private:
        CodeGenerator& m_generator;
    };

    bool isInitializingCell() const { return m_initializationDepth; }

private:
    struct EmittersMade {
#define AOT_DECLARE_EMITTER_POINTER(Class) Class* pointerTo##Class { nullptr };
        FOR_EACH_AOT_EMITTER(AOT_DECLARE_EMITTER_POINTER)
#undef AOT_DECLARE_EMITTER_POINTER
    };
    struct ResultRange {
        static constexpr uint32_t none = UINT32_MAX;

        explicit operator bool() const { return first != none; }

        uint32_t first { none };
        uint32_t count { 0 };
    };
    struct WayIntoBlock {
        BlockID to;
        BlockID from;
    };
    struct SubtreeOfDominators {
        uint32_t first;
        uint32_t size;
    };

    template<typename T> T& makeEmitter();
    bool isDeferredClosure(const Operation& definition) const
    {
        if (definition.opcode != Opcode::NewClosure && definition.opcode != Opcode::Narrowed) [[likely]]
            return false;
        return isDeferredClosureSlow(definition);
    }
    bool isDeferredClosureSlow(const Operation&) const;
    const PlacedSlot* slotOfScopeReadInPlaceOfClosureSlow(ValueID, const Operation&) const;
    ValueID m_closureReadForItsScope;
    const PlacedSlot* m_slotOfScopeOfClosureRead { nullptr };
    void computeEffectsOf(const OperationContext&) const;
    bool hasResults(size_t indexOfValue) const { return !m_resultRanges.empty() && m_resultRanges[indexOfValue]; }
    FormAtPlace sayFormNamedFor(const OperationContext&, StateOfKnowledge) const;
    bool recordSitesWithoutCode();
    void orderBlocks();
    bool isProvenToDominate(BlockID home, BlockID user) const { return m_userOfHomesProven == user && m_homesProven[home.index() % m_homesProven.size()] == home; }
    bool dominates(BlockID home, BlockID user);
    void findDominators();
    void numberDominatorTree();
    void createBlocks();
    void createPhis();
    bool isReadByBoxAlone(BlockID home, ValueID parameter, unsigned stepsLeft) const;
    bool waitsForBoxOfSum(ValueID, const Operation&) const;
    void followNumbersOutOfBytes();
    bool letsAnyNaNInto(ValueID parameter);
    bool goesOnBehindJump(size_t positionInOrder) const;
    void generateBlock(BlockID);
    ValueID testReadByBranchAlone(BlockID, const Terminator&) const;
    bool isAwaitedAtOnce(const Operation& call, std::span<const ValueID> valuesOfBlock, size_t index) const;
    void generateValue(BlockID, ValueID, const Operation&, bool isAwaitedAtOnce, std::span<const ValueID> operationsBehind);
    void generateTerminator(BlockID, const Block&, bool generatesCondition);
    void generate(const ValueContext&);
    void declareAccessesOf(const OperationContext& context)
    {
        if (m_checksAllocation) [[unlikely]]
            declareAccessesSlow(context);
    }
    void declareAccessesSlow(const OperationContext&);
    void dispatch(const ValueContext&);
    void dispatchWatched(const ValueContext&);
    template<Opcode> void dispatchOperation(const ValueContext&);
    void dispatch(const TerminatorContext&);

    const Edge* handlerOf(BlockID block) const
    {
        // What is made in a landing pad for the parameters of the handler must not lead back to the pad.
        if (m_isPassingHandlerArguments || m_isAheadOfWhatMayThrow)
            return nullptr;
        if (block == m_blockOfCachedHandler) [[likely]]
            return m_cachedHandler;
        return handlerEdgeOf(block, code().at(block));
    }
    const Edge* handlerEdgeOf(BlockID id, const Block& block) const
    {
        if (!block.handler) [[likely]]
            return nullptr;
        EdgeID handler = code().handlerOf(id);
        return handler ? &code().at(handler) : nullptr;
    }
    Operand get(ValueID, const Operation&);
    Operand makeConstant(ValueID id) { return makeConstant(id, code().at(id)); }
    Operand makeConstant(ValueID, const Operation&);
    void defineValue(ValueID, Representation ofRecord, Operand);
    void passArguments(const Edge&, std::span<const ValueID> parameters, std::span<const Operand> declaredResults);
    void passArgument(ValueID parameter, Operand);
    bool currentBlockIsOpen() const;
    struct SumOfIntegers {
        size_t indexOfParameter { 0 };
        ValueID sum;
        ValueID termAsNumber;
        ValueID term;
        LValue added { nullptr };
    };
    struct CountedLoop {
        size_t positionOfHead { 0 };
        Vector<BlockID, 4> turn;
        Vector<size_t, 4> positionsOfTurn;
        ValueID counter;
        ValueID bound;
        Opcode relation { Opcode::Less };
        int64_t step { 1 };
        int64_t mostTurns { 0 };
        ValueID poll;
        Vector<ValueID, 2> flatStrings;
        Vector<SumOfIntegers, 2> sumsOfIntegers;
    };
    std::optional<CountedLoop> findCountedLoopAt(size_t positionInOrder) const;
    void layOutTurns(const CountedLoop&);
    void keepArgumentsForNextTurn(const Edge&);
    bool leavesTurnLaidOut(BlockID target) const { return m_headOfTurnsLaidOut && target != m_headOfTurnsLaidOut && !std::ranges::contains(m_turnLaidOut, target); }
    void findSumsOfIntegers(CountedLoop&, const Edge& back) const;
    bool addToSumOfIntegers(ValueID);
    void failAtLimit(LimitID, uint64_t needed, const Subject&);

    const FunctionInputs m_inputs;
    Machine::Procedure& m_proc;
    CodeRecords& m_records;
    Output m_out;
    const bool m_checksAllocation;
    const bool m_checksEffects;
    const uint32_t m_rowThatSaysLess;
    bool m_watchesOperations { false };
    bool m_notesForms { false };
    PinnedValues m_pinned;
    std::span<LValue> m_values;
    std::span<ResultRange> m_resultRanges;
    Vector<Operand> m_results;
    std::span<LBasicBlock> m_blocks;
    bool m_nextBlockGoesOnHere { false };
    BlockID m_headOfTurnsLaidOut;
    Vector<Operand, 8> m_argumentsForNextTurn;
    std::span<const BlockID> m_turnLaidOut;
    WTF::Function<LBasicBlock()> m_blockLeavingTurnLaidOut;
    Vector<SumOfIntegers, 2> m_sumsOfIntegers;
    std::span<const BlockID> m_order;
    std::span<BlockID> m_singlePredecessors;
    std::span<ScopeFork> m_scopeAtEndOfBlock;
    BlockID m_onlyEntryBlock;
    Vector<WayIntoBlock, 16> m_waysIntoJoins;
    std::span<uint32_t> m_places;
    std::span<uint32_t> m_dominatorsByPlace;
    std::span<SubtreeOfDominators> m_subtreesByPlace;
    std::array<BlockID, 4> m_homesProven;
    BlockID m_userOfHomesProven;
    std::optional<DominatorsOfCode> m_dominators;
    BlockID m_blockOfCachedHandler;
    const Edge* m_cachedHandler { nullptr };
    const OperationContext* m_operation { nullptr };
    Operand* m_definitionTaken { nullptr };
    EmittersMade m_emitters;
    unsigned m_initializationDepth { 0 };
    bool m_isPassingHandlerArguments { false };
    bool m_isAheadOfWhatMayThrow { false };
    bool m_hasFailed { false };
    bool m_carriesParametersBoxed { false };
    BitVector m_parametersCarriedBoxed;
    bool m_followsNumbersOutOfBytes { false };
    BitVector m_valuesHoldingAnyNaN;
    BitVector m_valuesReadForBitsOfNaN;
    BitVector m_blocksBegun;
};

#define AOT_DEFINE_EMITTER_ACCESSOR(Class) \
    template<> Class& CodeGenerator::makeEmitter<Class>(); \
    template<> inline Class& CodeGenerator::emitter<Class>() \
    { \
        if (Class* made = m_emitters.pointerTo##Class) [[likely]] \
            return *made; \
        return makeEmitter<Class>(); \
    }
FOR_EACH_AOT_EMITTER(AOT_DEFINE_EMITTER_ACCESSOR)
#undef AOT_DEFINE_EMITTER_ACCESSOR

ValueID valueOwnedByTerminator(const Terminator&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

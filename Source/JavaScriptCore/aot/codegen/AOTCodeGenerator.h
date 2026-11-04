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

// Made once, in front of Compile. Every thread reads it and nothing changes it.
struct CodeGenerationInputs {
    const Program& program;
    const Facts& facts;
    const Plan& plan;
    const StubCatalog& stubCatalog;
    // One for a build. The driver owns it: PreparedProgram::heaps.
    const AOTHeaps& heaps;
};

struct FunctionInputs {
    BuildContext& build;
    TypeArena& types;
    const CodeGenerationInputs& program;
    // Swept.
    const Code& code;
};

struct PinnedValues {
    // An int32 read out of a list's storage, and the word it was read as: the same int32, boxed.
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

// Where ranges of origins are noted, what is appended for the end of a block has an origin by the kind of that end: the 256 below those of the back end.
inline constexpr uint32_t firstOriginOfTerminators = 0xFFFFFE00;

inline Machine::Origin machineOriginOf(TerminatorKind kind)
{
    return Machine::Origin(firstOriginOfTerminators + static_cast<uint32_t>(kind));
}

inline ValueID valueOfOrigin(Machine::Origin origin)
{
    return origin && origin.bits() < firstOriginOfTerminators ? ValueID(origin.bits() - 1) : ValueID();
}

// Appends the machine operations of one unit: one touch for each needed operation of Code, in an order in which a definition comes before its uses.
class CodeGenerator {
    WTF_MAKE_NONCOPYABLE(CodeGenerator);
public:
    CodeGenerator(const FunctionInputs&, Machine::Procedure&, CodeRecords&);
    ~CodeGenerator();

    GenerationResult run();
    // How often an operation has been handed to its emitter: each one that a block lists, as generate() comes to it. Not a constant, which lies in no block and is made at each use.
    uint32_t numberOfOperationsEmitted() const { return m_numberOfOperationsEmitted; }

    const FunctionInputs& inputs() const LIFETIME_BOUND { return m_inputs; }
    BuildContext& build() { return m_inputs.build; }
    const Program& program() const { return m_inputs.program.program; }
    const Facts& facts() const { return m_inputs.program.facts; }
    const Plan& plan() const { return m_inputs.program.plan; }
    const Code& code() const { return m_inputs.code; }
    // Null: several edges enter the block, or none.
    BlockID singlePredecessorOf(BlockID block) const { return m_singlePredecessors[block.index()]; }
    // The last HasContiguousIndex that a Branch asked, and the element that it loaded: ArrayIntrinsicEmitter::elementFoundByTestInFront(). An emitter holds nothing itself.
    ValueID testThatFoundElement;
    LValue elementFound { nullptr };
    CodeUnit unit() const { return code().unit(); }
    // The record of the function in the program's tables: flags, captures, declared types. It has no blocks.
    const Function& function() const;
    Machine::Procedure& procedure() { return m_proc; }
    const AOTHeaps& heaps() const { return m_inputs.program.heaps; }
    Output& out() LIFETIME_BOUND { return m_out; }
    PinnedValues& pinned() LIFETIME_BOUND { return m_pinned; }
    CodeRecords& records() { return m_records; }
    MentionList& mentions();
    ValueID operationBeingGenerated() const { return m_operation ? m_operation->valueID() : ValueID(); }
    // The block of the operation or terminator that is being generated. None between them.
    BlockID blockBeingGenerated() const { return m_operation ? m_operation->block() : BlockID(); }

    const Operation& operationAt(ValueID id) const LIFETIME_BOUND { return code().at(id); }
    Opcode opcodeOf(ValueID id) const { return code().at(id).opcode; }
    TypeID typeIDOf(ValueID id) const { return code().at(id).type; }
    Representation representationOf(ValueID id) const { return code().at(id).representation; }
    // The proven type: TypeArena::at(), which answers by value because a record of the thread's arena moves when the arena grows.
    Type typeOf(ValueID id) const { return m_inputs.types.at(typeIDOf(id)); }

    // machineTraitsOf() decides: the body of an async function in which nothing suspends does not.
    bool suspends() const { return m_proc.traits().savesFrameOnSuspend; }
    bool hasHandlers() const { return code().hasHandlers(); }
    bool isInLoop(BlockID block) const { return code().at(block).isInLoop; }
    // `const copy = list.slice(); copy.pos = 0`: in the block of the value, or of a parameter that jumps alone hand it on to.
    bool isStoredIntoByName(ValueID) const;
    // The one question about the form of an operation: in line, or the call that does the same? The schema answers, by the opcode and by what the emitter knows: the columns form, openUnder, closedUnder, runtimeUnder.
    // Of where the operation stands only a loop of the unit counts, and only for a row that lists Loop: openIn, closedIn, runtimeIn. Whoever asks has said the form as well: Output::noteForm().
    // The questions below ask more about the place than the design allows. `testaot bytes-by-form` lists each form that one of them has picked.
    bool takesOpenForm(const OperationContext&, StateOfKnowledge = StateOfKnowledge::Always) const;
    // The same question of the same row, for an operation whose form in a loop is its form elsewhere with more of it in line: has the loop picked?
    bool takesFormOfLoops(const OperationContext&, StateOfKnowledge = StateOfKnowledge::Always) const;
    bool paysForCodeInLine(BlockID block) const { return isInLoop(block); }
    bool laterBarrierCovers(ValueID store) const { return code().marksOf(store).contains(SweepMark::LaterBarrierCovers); }
    bool laterFenceCovers(ValueID allocation) const { return code().marksOf(allocation).contains(SweepMark::LaterFenceCovers); }
    // NewClosure in ClosureForm::Deferred: its one user is a store that writes nothing, because the slot already names the function.
    bool isDeferredClosure(ValueID id) const { return isDeferredClosure(code().at(id)); }
    // GetDeferredClosureSlot whose one reader is the GetCapture of a call through the slot. The object keeps the scope of its closures, so nothing is loaded out of the slot and nothing is made: the read has no code,
    // and the GetCapture loads the field of the scope. Null: any other operation. leaveClosuresUnmadeInChoices() says what an object with a scope promises, and what one without it holds in the slot.
    const PlacedSlot* slotOfScopeReadInPlaceOfClosure(ValueID id, const Operation& read) const
    {
        if (read.opcode != Opcode::GetDeferredClosureSlot) [[likely]]
            return nullptr;
        return slotOfScopeReadInPlaceOfClosureSlow(id, read);
    }
    // Of the GetCapture right behind such a read. Null: the closure has a value.
    const PlacedSlot* slotOfScopeOfClosureWithoutValue(ValueID closure) const { return closure == m_closureReadForItsScope ? m_slotOfScopeOfClosureRead : nullptr; }
    // What the schema declares for the opcode, less what Operation::traits() rules out for this operation.
    const Effects& effectsOf(const OperationContext& context LIFETIME_BOUND) const
    {
        if (!context.m_effects)
            computeEffectsOf(context);
        return *context.m_effects;
    }

    // An operand as it was defined, which is the form in which its user takes it. A constant is made here, at each use: Output shares it within a scope.
    Operand get(ValueID);
    // In every build: the block of a definition dominates the block of whoever asks for the value. Nothing behind this notices a breach: on some way the code would read a register that holds something else.
    void verifyThatDefinitionDominates(ValueID, BlockID user);
    // Of what lies in a block. False for what is not generated yet, for what its user generates, and for a producer that holds none of its results.
    bool hasWordOf(ValueID id) const { return !!m_values[id.index()]; }
    // For whoever compares the word and reads nothing through it. Null where hasWordOf() is false.
    LValue wordOf(ValueID id) const { return m_values[id.index()]; }
    // Fails the build unless the operand is in that form or in another spelling of the same word.
    Operand getIn(ValueID, Representation);
    // The code of an operation once more, where Output stands, while another operation or a terminator is generated. What get() answers for the operation stays as it is.
    // For whoever knows that its operands are in sight there and that it gives there what it gave where it stands.
    Operand rematerialize(ValueID id) { return makeConstant(id); }
    // The word of the value may spell any NaN: it was read out of bytes, or arithmetic made it of such a word. get() makes that the one NaN for whoever asks, each time.
    bool holdsAnyNaN(ValueID id) const { return m_followsNumbersOutOfBytes && m_valuesHoldingAnyNaN.quickGet(id.index()); }
    // The word as it is, for the code of an operation of which takesAnyNaN() holds.
    Operand getWithAnyNaN(ValueID);
    // A parameter of a block that is written as a Double and that a Box alone reads lies in its register boxed, and get() answers so. Each edge boxes what it hands over in the form that it has there:
    // an integer that was made a number for the sake of the parameter is boxed as the integer that it is.
    bool isCarriedBoxed(ValueID id) const { return m_carriesParametersBoxed && m_parametersCarriedBoxed.quickGet(id.index()); }
    // `pos++`, `pos += 2` on a number that lies boxed and is stored boxed again: an Add, Sub, Increment or Decrement in OperatorForm::Numbers that the Box alone reads, each of whose terms it alone reads:
    // the Unbox of a boxed number, the IntegerToNumber of an int32, a written int32. All stand in one block. They are generated as one where the Box stands: OperatorEmitter::boxedSum(). Int32s are added as int32s.
    // Null: the Box boxes no such sum.
    ValueID sumBoxedBy(const Operation& box) const;
    // For what the operation being generated has read out of bytes or made of words that holdAnyNaN(): what it is to define.
    // The word as it is where everyone who reads the value takesAnyNaN() or is an edge, and else with the one NaN.
    Number settleNaNOf(const ValueContext&, Number);
    void define(const ValueContext&, Operand);
    void defineTest(const ValueContext&, Condition);
    void defineResults(const ValueContext&, std::span<const Operand>);
    Operand resultAt(ValueID producer, uint32_t index) const;
    void defineParameter(ValueID parameter, Operand);

    LBasicBlock blockFor(BlockID) const;
    // The parameters of the target that the arguments of the edge do not fill take the declared results, in their order.
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
    bool isPassingHandlerArguments() const { return m_isPassingHandlerArguments; }

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
    // `from` is null for an entry.
    struct WayIntoBlock {
        BlockID to;
        BlockID from;
    };
    // The dominator tree is numbered in preorder: what a block dominates has the numbers from its own on, as many as its subtree has blocks.
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
    // False: it does not, or `home` is not reached.
    bool dominates(BlockID home, BlockID user);
    void findDominators();
    void numberDominatorTree();
    void createBlocks();
    void createPhis();
    // The Box stands at the head of the block of the parameter, or the block hands the parameter on, by its Jump alone, to a parameter of which this holds.
    bool isReadByBoxAlone(BlockID home, ValueID parameter, unsigned stepsLeft) const;
    // Of what is kept as a Double: it is the sum or a term of sumBoxedBy() some Box, and has no code of its own.
    bool waitsForBoxOfSum(ValueID, const Operation&) const;
    // Only where Code::readsNumbersOutOfBytes(): who reads which value for the bits of its NaN, and which parameters of loop heads such bytes come round to.
    void followNumbersOutOfBytes();
    // The edge hands the parameter a word that holdsAnyNaN(). True: as it is, and the parameter holdsAnyNaN() from here on.
    bool letsAnyNaNInto(ValueID parameter);
    // Nothing enters the block but the Jump that ends the block in front of it in m_order, which runs as often and hands it nothing in a register.
    // It gets no machine block of its own: its code goes on where the Jump would stand. So a test at its end is fused with what the block in front has computed.
    bool goesOnBehindJump(size_t positionInOrder) const;
    void generateBlock(BlockID);
    ValueID testReadByBranchAlone(BlockID, const Terminator&) const;
    bool isAwaitedAtOnce(const Operation& call, std::span<const ValueID> valuesOfBlock, size_t index) const;
    void generateValue(BlockID, ValueID, const Operation&, bool isAwaitedAtOnce, std::span<const ValueID> operationsBehind);
    void generateTerminator(BlockID, const Block&, bool generatesCondition);
    void generate(const ValueContext&);
    // In debug and ASAN builds: holds the loads and stores that the emitters append for the operation to effectsOf(), through Builder::setDeclaredAccesses().
    void declareAccessesOf(const OperationContext& context)
    {
        if (m_checksAllocation) [[unlikely]]
            declareAccessesSlow(context);
    }
    void declareAccessesSlow(const OperationContext&);
    void dispatch(const ValueContext&);
    template<Opcode> void dispatchOperation(const ValueContext&);
    void dispatch(const TerminatorContext&);

    const Edge* handlerOf(BlockID block) const
    {
        // What is made in a landing pad for the parameters of the handler must not lead back to the pad.
        // Ahead of whatever may throw only a failure to allocate could take the edge, and the walk has built what the edge brings under no handler.
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
    // `sum += view[i]`: a parameter of the head, a number, that a turn reads only to add an integer to it, so small that all turns together stay far below 2^53.
    // From a sum that is whole and as small, every sum on the way is an integer that a number holds exactly: the integers may be added by themselves and the whole added to the sum once.
    struct SumOfIntegers {
        size_t indexOfParameter { 0 };
        ValueID sum;
        ValueID termAsNumber;
        ValueID term;
        // In the turns laid out: what they have added so far, an Int64.
        LValue added { nullptr };
    };
    // `for (; i < n; i++)`, `for (; i >= 0; i -= 2)`: the head polls and holds a counter against a bound from in front of the loop, and a turn ends by stepping the counter towards the bound by a constant.
    struct CountedLoop {
        size_t positionOfHead { 0 };
        // The blocks on the ways from the head's test to the one edge back, as they stand in the order, and where they stand there. Whatever else a turn leads to, it leaves them.
        Vector<BlockID, 4> turn;
        Vector<size_t, 4> positionsOfTurn;
        ValueID counter;
        ValueID bound;
        // Less, LessEq, Greater or GreaterEq of the counter and the bound.
        Opcode relation { Opcode::Less };
        int64_t step { 1 };
        // By the types of the counter and the bound: no more.
        int64_t mostTurns { 0 };
        ValueID poll;
        // Made by FlattenString in front of the loop, and read by the code unit in a turn.
        Vector<ValueID, 2> flatStrings;
        Vector<SumOfIntegers, 2> sumsOfIntegers;
    };
    std::optional<CountedLoop> findCountedLoopAt(size_t positionInOrder) const;
    void layOutTurns(const CountedLoop&);
    void keepArgumentsForNextTurn(const Edge&);
    bool leavesTurnLaidOut(BlockID target) const { return m_headOfTurnsLaidOut && target != m_headOfTurnsLaidOut && !std::ranges::contains(m_turnLaidOut, target); }
    void findSumsOfIntegers(CountedLoop&, const Edge& back) const;
    // True: the operation belongs to such a sum, and has been seen to.
    bool addToSumOfIntegers(ValueID);
    void failAtLimit(LimitID, uint64_t needed, const Subject&);

    const FunctionInputs m_inputs;
    Machine::Procedure& m_proc;
    CodeRecords& m_records;
    Output m_out;
    const bool m_checksAllocation;
    bool m_notesForms { false };
    PinnedValues m_pinned;
    // By the index of the value. Of a constant that takes no code: the machine constant, once it has been made.
    std::span<LValue> m_values;
    std::span<ResultRange> m_resultRanges;
    Vector<Operand> m_results;
    // By the index of the block. Null for a block that nothing reaches, and for one that goesOnBehindJump() until the Jump is come to.
    std::span<LBasicBlock> m_blocks;
    // From the Jump in front of a block that goesOnBehindJump() to the head of that block.
    bool m_nextBlockGoesOnHere { false };
    // The head of the loop whose turns are being laid out one behind the other: the edge back to it leads on into the next of them.
    BlockID m_headOfTurnsLaidOut;
    // By parameter of that head.
    Vector<Operand, 8> m_argumentsForNextTurn;
    std::span<const BlockID> m_turnLaidOut;
    // Where an edge goes that leaves the turn being laid out: to the written loop, with what the turn began with. The written loop takes that turn again, which writes nothing, and leaves by the edge as it is written.
    WTF::Function<LBasicBlock()> m_blockLeavingTurnLaidOut;
    Vector<SumOfIntegers, 2> m_sumsOfIntegers;
    // The reached blocks, each behind the blocks that dominate it.
    std::span<const BlockID> m_order;
    // By the index of the block. The block that the one edge into a block comes out of. Null for a block that is entered in any other way: by several edges, by a throw, from outside.
    std::span<BlockID> m_singlePredecessors;
    // By the index of the block, once it is generated: the scope of Output behind its terminator.
    std::span<ScopeFork> m_scopeAtEndOfBlock;
    BlockID m_onlyEntryBlock;
    BlockID m_blockEnteringFile;
    // Every way into each reached block that has no single predecessor.
    Vector<WayIntoBlock, 16> m_waysIntoJoins;
    // By the index of the block: its position in m_order plus one, and zero for a block that is not in it. Place 0 is a root above the entries.
    // Empty, as the next, until an operand whose block is neither the only entry nor found a few single predecessors above its user.
    std::span<uint32_t> m_places;
    // By place: the place of the immediate dominator.
    std::span<uint32_t> m_dominatorsByPlace;
    // By place. Empty until a way up the immediate dominators is long.
    std::span<SubtreeOfDominators> m_subtreesByPlace;
    // Blocks that dominates() has answered for, by their index modulo the size.
    std::array<BlockID, 4> m_homesProven;
    BlockID m_userOfHomesProven;
    // Made to confirm that a definition does not dominate.
    std::optional<DominatorsOfCode> m_dominators;
    BlockID m_blockOfCachedHandler;
    const Edge* m_cachedHandler { nullptr };
    const OperationContext* m_operation { nullptr };
    // Takes what the operation that is being generated defines, in place of the table of values.
    Operand* m_definitionTaken { nullptr };
    EmittersMade m_emitters;
    unsigned m_initializationDepth { 0 };
    uint32_t m_numberOfOperationsEmitted { 0 };
    bool m_isPassingHandlerArguments { false };
    // The arguments of a handler edge stand in front of the first operation of the block that may throw: Code::addHandlerEdge(). What stands ahead of that operation may be one of them.
    bool m_isAheadOfWhatMayThrow { false };
    bool m_hasFailed { false };
    bool m_carriesParametersBoxed { false };
    // By the index of the value, where m_carriesParametersBoxed.
    BitVector m_parametersCarriedBoxed;
    bool m_followsNumbersOutOfBytes { false };
    // By the index of the value, where m_followsNumbersOutOfBytes.
    BitVector m_valuesHoldingAnyNaN;
    // Some operation or terminator reads the value and takesAnyNaN() does not hold of it.
    BitVector m_valuesReadForBitsOfNaN;
    // By the index of the block: generateBlock() has come to it, so something may have read its parameters.
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

// The last operation of the block, which the terminator generates itself: the condition of TestAndBranch, the call of ReturnCall.
ValueID valueOwnedByTerminator(const Terminator&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

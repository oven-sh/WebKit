/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
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
#include "AOTRepresentationRules.h"
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
    MentionList& mentions() { return m_mentions; }
    ValueID operationBeingGenerated() const { return m_operation ? m_operation->valueID() : ValueID(); }
    const OperationContext* contextBeingGenerated() const { return m_operation; }
    BlockID blockBeingGenerated() const { return m_operation ? m_operation->block() : BlockID(); }

    const Operation& operationAt(ValueID id) const LIFETIME_BOUND { return code().at(id); }
    Opcode opcodeOf(ValueID id) const { return code().at(id).opcode; }
    TypeID typeIDOf(ValueID id) const { return code().at(id).type; }
    Representation representationOf(ValueID id) const { return code().at(id).representation; }
    Type typeOf(ValueID id) const { return m_inputs.types.at(typeIDOf(id)); }

    bool suspends() const { return m_proc.traits().savesFrameOnSuspend; }
    bool hasHandlers() const { return code().hasHandlers(); }
    bool isLayingOutTurns() const { return !!m_headOfTurnsLaidOut; }
    bool leavesTurnLaidOut(BlockID target) const { return m_headOfTurnsLaidOut && target != m_headOfTurnsLaidOut && !std::ranges::contains(m_turnLaidOut, target); }
    bool isInLoop(BlockID block) const { return code().at(block).isInLoop; }
    bool mayStayRope(ValueID flatString) const { return m_ropeTolerantStrings.contains(flatString); }
    void forEachCarryingLoop(ValueID flatString, NOESCAPE const Invocable<void(BlockID head, LValue characters, LValue implFlags)> auto& functor) const
    {
        for (const LoopCarriedString& carried : m_loopCarriedStrings) {
            if (carried.string == flatString)
                functor(carried.head, carried.characters, carried.implFlags);
        }
    }
    bool isStoredIntoByName(ValueID) const;
    bool takesOpenForm(const OperationContext&, StateOfKnowledge = StateOfKnowledge::Always) const;
    bool takesFormOfLoops(const OperationContext&, StateOfKnowledge = StateOfKnowledge::Always) const;
    bool paysForCodeInLine(BlockID block) const { return isInLoop(block); }
    bool runsRepeatedly(BlockID block) const { return isInLoop(block) || isFunctionCalledRepeatedly(); }
    bool isFunctionCalledRepeatedly() const;
    bool isInRecursion(BlockID block) const { return code().callsItself() && code().at(block).frequency != BlockFrequency::Rare; }
    bool usesLoopForms(BlockID block) const { return isInLoop(block) && code().at(block).frequency != BlockFrequency::Rare; }
    bool laterBarrierCovers(ValueID store) const { return code().marksOf(store).contains(SweepMark::LaterBarrierCovers); }
    bool barrierIsCountedOn(ValueID store) const { return code().barrierIsCountedOn(store); }
    void noteThatOwnerIsRememberedBehind(ValueID store)
    {
        if (barrierIsCountedOn(store)) [[unlikely]]
            m_storesBehindWhichOwnerIsRemembered.set(store.index());
    }
    void noteFirstStores(ValueID allocation, std::span<const ValueID> stores)
    {
        for (ValueID store : stores.first(stores.size() - 1))
            m_storesWithLaterOrderWord.set(store.index());
        m_lastFirstStores.append({ stores.back(), allocation });
    }
    bool laterStoreWritesOrderWord(ValueID store) const { return m_storesWithLaterOrderWord.get(store.index()); }
    ValueID takeAllocationFilledBy(ValueID store)
    {
        size_t index = m_lastFirstStores.findIf([&](const std::pair<ValueID, ValueID>& each) { return each.first == store; });
        if (index == notFound)
            return { };
        ValueID allocation = m_lastFirstStores[index].second;
        m_lastFirstStores.removeAt(index);
        return allocation;
    }
    bool ownerIsYoung(ValueID store) const { return !m_keepsValuesInHeapFrame && code().marksOf(store).contains(SweepMark::OwnerIsYoung); }
    bool laterFenceCovers(ValueID allocation) const { return !m_keepsValuesInHeapFrame && code().marksOf(allocation).contains(SweepMark::LaterFenceCovers); }
    bool isDeferredClosure(ValueID id) const { return isDeferredClosure(code().at(id)); }
    std::optional<ArrayStorage> kindOfListFoundInFrontOfTurnsLaidOut(ValueID list) const;
    bool liesInsideByQuestionInFrontOfTurnsLaidOut(ValueID read) const;
    bool leavesHoleToTestBehind(const ValueContext& read);
    bool deferCheckToTruthTest(const ValueContext& check);
    bool throwsRuntimeErrorImmediately(const Block&, std::span<const ValueID> errorAndFollowingOperations);
    ValueID checkDeferredToTruthTest(ValueID test) const { return m_truthTestWithCheck == test ? m_checkDeferredToTruthTest : ValueID(); }
    bool holeIsLeftToTestBehind(ValueID read) const { return m_elementsWhoseHolesAreLeftToTestsBehind.get(read.index()); }
    void noteNumberReadByPresenceTest(ValueID read, LValue number)
    {
        takeNumberReadByPresenceTest(read);
        m_numbersReadByPresenceTests.append({ read, number });
    }
    LValue takeNumberReadByPresenceTest(ValueID read)
    {
        LValue number = nullptr;
        m_numbersReadByPresenceTests.removeAllMatching([&](const std::pair<ValueID, LValue>& noted) {
            if (noted.first != read)
                return false;
            number = noted.second;
            return true;
        });
        return number;
    }
    LValue& wordWhoseHoleIsLeftToTestAtWork() { return m_wordWhoseHoleIsLeftToTestAtWork; }
    bool holdsByQuestionInFrontOfTurnsLaidOut(ValueID relation) const { return m_relationsAskedInFrontOfTurnsLaidOut.contains(relation); }
    std::optional<TypedArrayType> kindOfViewFoundInFrontOfTurnsLaidOut(ValueID view) const;
    LValue holdsDoublesAsReadInFrontOfTurnsLaidOut(ValueID list) const;
    bool isGenerated(ValueID id) const { return m_values[id.index()]; }
    LBasicBlock blockThatHoleLeavesTurnLaidOutBy();
    LBasicBlock optionalNumberHoleExit();
    void noteWordOfElementInTurnLaidOut(ValueID read, LValue word) { m_wordsOfElementsInTurnLaidOut.append({ read, word }); }
    bool asksBehindTurnsWhetherElementIsThere(ValueID read) const { return m_asksElementsBehindTurns && isTermOfSumOfElements(read); }
    void noteBoxedWordOfElementInTurns(LValue word) { m_boxedWordsOfElementsInTurns = m_boxedWordsOfElementsInTurns ? m_out.bitAnd(m_boxedWordsOfElementsInTurns, word) : word; }
    const PlacedSlot* slotOfScopeReadInPlaceOfClosure(ValueID id, const Operation& read) const
    {
        if (read.opcode != Opcode::GetDeferredClosureSlot) [[likely]]
            return nullptr;
        return slotOfScopeReadInPlaceOfClosureSlow(id, read);
    }
    void scheduleClosureLifetimeEnd(ValueID closure, ValueID call) { m_scheduledLifetimeEnds.append({ closure, call }); }
    const PlacedSlot* slotOfScopeOfClosureWithoutValue(ValueID closure) const { return closure == m_closureReadForItsScope ? m_slotOfScopeOfClosureRead : nullptr; }
    const Effects& effectsOf(const OperationContext& context LIFETIME_BOUND) const
    {
        if (!context.m_hasEffects)
            computeEffectsOf(context);
        return context.m_effects;
    }

    Operand get(ValueID id) { return get(id, code().at(id)); }
    LValue wordOf(ValueID id) const { return m_values[id.index()]; }
    void nameAgain(ValueID id, LValue lowered) { m_values[id.index()] = lowered; }
    bool keepsValuesInHeapFrame() const { return m_keepsValuesInHeapFrame; }
    bool checksEffects() const { return m_checksEffects; }
    void noteCopiesOfBigInts() { m_copiesBigIntsInRareBlocks = true; }
    ValueID asyncResultReturnedByStub(const Block&) const;
    ValueContext contextOf(BlockID block, ValueID id) const { return ValueContext(block, id, code().at(id), nullptr); }
    bool isConversionLeftToConcatenation(ValueID id) const { return id && id == m_conversionLeftToConcatenation; }
    bool isProductLeftToRemainder(ValueID id) const { return !m_productsLeftToRemainders.isEmpty() && m_productsLeftToRemainders.contains(id); }
    Operand generateProductHere(BlockID, ValueID product);
    void copyBigIntsToHeap(BlockID);
    template<typename Functor>
    LValue withConversionLeftToConcatenation(const OperationContext& concatenation, const Functor& functor)
    {
        const ValueID id = m_conversionLeftToConcatenation;
        RELEASE_ASSERT(id);
        ValueContext conversion(concatenation.block(), id, code().at(id), handlerOf(concatenation.block()));
        Effects both = effectsOf(conversion);
        both.merge(effectsOf(concatenation));
        conversion.m_effects = both;
        declareAccessesOf(conversion);
        LValue result = functor(conversion);
        declareAccessesOf(concatenation);
        return result;
    }
    ValueID bodyEnteredAfterNewPromise(const Block&);
    ValueID fulfilledPromiseReturnedByStub(const Block&) const;
    std::span<const BlockID> order() const { return m_order.span(); }
    BlockID onlyPredecessorOf(BlockID block) const { return m_singlePredecessors[block.index()]; }
    bool blockDominates(BlockID above, BlockID below);
    BlockID nearestDominatorOf(BlockID);
    Operand getIn(ValueID id, Representation wanted)
    {
        if (wanted == Representation::Void)
            return { wanted, nullptr };
        const Operation& operation = code().at(id);
        if (isSameWord(operation.representation, wanted)) [[likely]]
            return { wanted, get(id, operation).value };
        return makeInWantedForm(id, operation, wanted);
    }
    bool holdsAnyNaN(ValueID id) const { return m_followsNumbersOutOfBytes && m_valuesHoldingAnyNaN.quickGet(id.index()); }
    Operand getWithAnyNaN(ValueID);
    bool isCarriedBoxed(ValueID id) const { return m_carriesParametersBoxed && m_parametersCarriedBoxed.quickGet(id.index()); }
    bool rotatesUnrolledIterationNumbers() const { return m_rotatesUnrolledIterationNumbers; }
    LValue numberOfInteger(LValue);
    LValue loopCarriedNumberOf(ValueID conversion) const
    {
        return m_loopCarriedNumbers.isEmpty() ? nullptr : m_phisOfLoopCarriedNumbers.get(conversion.index());
    }
    void findBoxesLeftToHandlers();
    uint32_t findLoopExitThatRenames(std::span<const ValueID>) const;
    bool isLeftToHandlers(ValueID id) const { return m_leavesBoxesToHandlers && m_boxesLeftToHandlers.get(id.index()); }
    Operand boxForHandler(ValueID box);
    ALWAYS_INLINE ValueID sumBoxedBy(const Operation& box) const
    {
        if (box.opcode != Opcode::Box || box.representation != Representation::JSValue || m_followsNumbersOutOfBytes)
            return { };
        if (code().at(box.valueOperand()).representation != Representation::Double) [[likely]]
            return { };
        return sumBoxedBySlow(box);
    }
    bool waitsForBoxOfSum(ValueID, const Operation&) const;
    bool isLeftToBackEdge(ValueID, const Operation&) const;
    Number settleNaNOf(const ValueContext&, Number);
    void noteThatNumberIsNoNaN(const ValueContext&);
    bool takesNothingForNaN(const ValueContext&);
    void define(const ValueContext& value, Operand result)
    {
        RELEASE_ASSERT(!value.isBranchCondition());
        defineValue(value.id(), value.representation(), result);
    }
    bool isOnlyBoxed(ValueID id) const
    {
        ValueID user = code().useCountOf(id) == 1 ? code().onlyUserOf(id) : ValueID();
        return user && !m_keepsValuesInHeapFrame && code().at(user).opcode == Opcode::Box && code().at(user).representation == Representation::JSValue;
    }
    bool isOnlyUsedAsIndex(ValueID id) const
    {
        const Operation& unboxed = code().at(id);
        if (unboxed.opcode != Opcode::Unbox || unboxed.representation != Representation::Double || m_keepsValuesInHeapFrame || code().useCountOf(id) != 1 || !code().onlyUserOf(id))
            return false;
        const Operation& access = code().at(code().onlyUserOf(id));
        if ((access.opcode != Opcode::GetTypedArrayElement && access.opcode != Opcode::PutTypedArrayElement) || access.positionOperand() != id || access.block() != unboxed.block() || access.isInsideView() || access.level() == KnowledgeLevel::Expected)
            return false;
        return code().at(unboxed.valueOperand()).representation == Representation::JSValue && !isCarriedBoxed(unboxed.valueOperand());
    }
    void defineBoxed(const ValueContext& value, Boxed result)
    {
        RELEASE_ASSERT(!value.isBranchCondition());
        defineValue(value.id(), Representation::JSValue, result);
        m_valuesDefinedBoxed.set(value.id().index());
    }
    void forgetBoxed(ValueID id)
    {
        if (m_valuesDefinedBoxed.get(id.index())) [[unlikely]]
            m_valuesDefinedBoxed.clear(id.index());
    }
    std::optional<Boxed> findBoxed(ValueID id) const { return m_valuesDefinedBoxed.get(id.index()) && m_values[id.index()] ? std::optional { Boxed(m_values[id.index()]) } : std::nullopt; }
    void defineTest(const ValueContext&, Condition);
    void defineResults(const ValueContext&, std::span<const Operand>);
    Operand resultAt(ValueID producer, uint32_t index) const;
    void defineParameter(ValueID parameter, Operand);
    bool goesOnInBlockOfEntry(BlockID id) const { return id == m_onlyEntryBlock && !m_firstBlockHasOtherWaysIn; }
    void defineParameterAsPassed(ValueID parameter, Operand);
    void goOnInBlockOfEntry(BlockID);

    LBasicBlock blockFor(BlockID) const;
    BlockEdge edgeTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    std::optional<BlockEdge> edgeThatBringsNothing(EdgeID);
    BlockEdge edgeWithoutArguments(const Edge&);
    std::optional<BlockEdge> edgeAfterLastStep(const ValueContext& step, uint32_t indexOfDone);
    void findStepsReadElsewhere();
    void expectOtherJumps(BlockID block) { m_blocksWithOtherJumps.set(block.index()); }
    ValueID testOfBranch(BlockID id) const { return testReadByBranchAlone(id, code().at(id).terminator); }
    bool isTakenIntoSwitchInFront(BlockID block) const { return m_switchesTakenIntoSwitchInFront.get(block.index()); }
    void noteSwitchTakenIntoSwitchInFront(BlockID block) { m_switchesTakenIntoSwitchInFront.set(block.index()); }
    void jumpTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    ALWAYS_INLINE bool mayHaveUnlikelySide(const TerminatorContext& terminator) const
    {
        static constexpr OpcodeSet testsWithUnlikelySide { Opcode::LogicalNot, Opcode::ToBoolean, Opcode::StrictEq, Opcode::Eq, Opcode::Parameter };
        return terminator.kind() == TerminatorKind::Branch && testsWithUnlikelySide.contains(opcodeOf(terminator->conditionOperand()));
    }
    void markUnlikelySide(const TerminatorContext&, BranchTargets&) const;
    void passHandlerArguments(const Edge& handler);

    void generateBranchCondition(const TerminatorContext&, ValueID condition, BranchTargets);

    struct ComparisonWithLiteral {
        BlockID block;
        ValueID subject;
        StringID literal;
    };
    struct LiteralChain {
        Vector<ComparisonWithLiteral, 8> comparisons;
        Vector<std::pair<BlockID, ValueID>, 4> names;
    };
    std::optional<LiteralChain> findLiteralChain(BlockID) const;
    BlockEdge edgeOutOf(BlockID, uint32_t targetIndex);
    void generateName(BlockID, ValueID);
    ValueID readLeftToBranchOnItsTruth(BlockID id, std::span<const ValueID> values, ValueID test)
    {
        if (!test || values.size() < 2 || values.back() != test)
            return { };
        return readLeftToBranchOnItsTruthSlow(id, values, test);
    }
    ValueID readLeftToBranchOnItsTruthSlow(BlockID, std::span<const ValueID> values, ValueID test);
    struct WaysOfTruth {
        BranchTargets targets;
        bool areOfObjectAndNothing { false };
    };
    std::optional<WaysOfTruth> waysOfTruthOf(const ValueContext& read) const;
    bool isReadAgainBehindItsOwnTruthAlone(const ValueContext& read) const;
    bool isGuardedByUndefinedTest(const ValueContext& read) const;
    void generateReturnedCall(const TerminatorContext&, ValueID call);

    template<typename T> T& emitter();

    Subject subjectOf(const OperationContext&) const;
    COLD NEVER_INLINE void fail(const OperationContext&, InternalErrorKind, const String& detail = { });
    void failAtLimit(const OperationContext&, LimitID, uint64_t needed);
    bool hasFailed() const { return m_hasFailed; }

    enum class FillsCellOfHeap : bool { No, Yes };
    class InitializationScope {
        WTF_MAKE_NONCOPYABLE(InitializationScope);
    public:
        explicit InitializationScope(CodeGenerator&, FillsCellOfHeap = FillsCellOfHeap::Yes);
        ~InitializationScope();

    private:
        CodeGenerator& m_generator;
    };

    bool isInitializingCell() const { return m_initializationDepth; }
    bool& owesFenceBehindStores() { return m_owesFenceBehindStores; }

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
    using OperationDispatchers = std::array<void (*)(CodeGenerator&, const ValueContext&), numberOfEnumerators<Opcode>>;
    using TerminatorDispatchers = std::array<void (*)(CodeGenerator&, const TerminatorContext&), numberOfEnumerators<TerminatorKind>>;

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
public:
    ValueID m_reservationThatBirthHasServed;
    ValueID m_literalLeftToSpreadRightBehind;
    LValue bearLiteralAsWritten(BlockID, ValueID literal);
    std::span<const std::pair<ValueID, ValueID>> usersOfLiteralInBlock(BlockID, ValueID literal);
    bool isLiteralStoredIntoByName(BlockID, ValueID literal);
private:
    void listUsersOfLiterals(BlockID);
    BlockID m_blockWhoseUsersOfLiteralsAreListed;
    Vector<std::pair<ValueID, ValueID>> m_usersOfLiteralsInBlock;
    Vector<ValueID> m_literalsStoredIntoByName;
    struct LoopCarriedString {
        ValueID string;
        BlockID head;
        LValue characters;
        LValue implFlags;
    };
    struct LoopCarriedNumber {
        ValueID parameter;
        BlockID head;
        ValueID conversion;
        size_t place;
        LValue phi;
    };
    BlockID usualEntranceOf(BlockID) const;
    void findKeysCheckedTwice();
    void noteIndexOfKey(ValueID);
    void findRopeTolerantStrings();
    void findLoopCarriedNumbers();
    Vector<LoopCarriedNumber, 4> m_loopCarriedNumbers;
    using IndexMap = UncheckedKeyHashMap<uint32_t, size_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    IndexMap m_firstLoopCarriedNumbers;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_phisOfLoopCarriedNumbers;
    Vector<ValueID, 4> m_ropeTolerantStrings;
    Vector<LoopCarriedString, 4> m_loopCarriedStrings;
public:
private:
    Vector<std::pair<ValueID, ValueID>, 2> m_scheduledLifetimeEnds;
    const PlacedSlot* m_slotOfScopeOfClosureRead { nullptr };
    void computeEffectsOf(const OperationContext&) const;
    bool hasResults(size_t indexOfValue) const { return !m_resultRanges.empty() && m_resultRanges[indexOfValue]; }
    FormAtPlace sayFormNamedFor(const OperationContext&, StateOfKnowledge) const;
    bool recordSitesWithoutCode();
    void orderBlocks();
    bool isProvenToDominate(BlockID home, BlockID user) const { return m_userOfHomesProven == user && m_homesProven[home.index() % m_homesProven.size()] == home; }
    void verifyThatDefinitionDominates(ValueID, BlockID home, BlockID user);
    void verifyThatKeptValueIsNamedInThisStretch(ValueID);
    bool dominates(BlockID home, BlockID user);
    void findDominators();
    void numberDominatorTree();
    void createBlocks();
    void createPhis();
    bool isReadByBoxAlone(BlockID home, ValueID parameter, unsigned stepsLeft) const;
    ValueID sumBoxedBySlow(const Operation& box) const;
    void followNumbersOutOfBytes();
    bool letsAnyNaNInto(ValueID parameter);
    bool goesOnBehindJump(size_t positionInOrder) const;
    bool leavesOutBlockThatNothingLeadsTo(BlockID);
    bool isWordBits(ValueID) const;
    std::span<const ValueID> valuesWithBitsAtFirstUses(BlockID, std::span<const ValueID>, Vector<ValueID>& inOrderOfUses);
    void generateBlock(BlockID id) { generateBlock(id, code().at(id)); }
    void generateBlock(BlockID, const Block&);
    void enterBlockOfBodyWithHeapFrame(BlockID, const Block&);
    ValueID testReadByBranchAlone(BlockID, const Terminator&) const;
    std::optional<uint32_t> constantMask(ValueID) const;
    enum class LooksBehindJoins : bool { No, Yes };
    std::optional<bool> unlikelyAnswer(ValueID test, LooksBehindJoins) const;
    std::optional<bool> unlikelyJoinResult(ValueID parameter) const;
    bool isInEqualityChain(ValueID test, bool isNegated) const;
    bool isOnUnlikelySide(BlockID) const;
    ValueID testReadByBranchBehindJumpAlone(BlockID, const Block&) const;
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
    static const OperationDispatchers& operationDispatchers();
    void dispatch(const ValueContext& value) { m_operationDispatchers[static_cast<size_t>(value.opcode())](*this, value); }
    void dispatchWatched(const ValueContext&);
    template<Opcode> void dispatchOperation(const ValueContext&);
    static const TerminatorDispatchers& terminatorDispatchers();
    void dispatch(const TerminatorContext& terminator) { m_terminatorDispatchers[static_cast<size_t>(terminator.kind())](*this, terminator); }
    template<TerminatorKind> void dispatchTerminator(const TerminatorContext&);

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
    Operand get(ValueID id, const Operation& operation)
    {
        Representation representation = operation.representation;
        BlockID home = operation.block();
        BlockID user = blockBeingGenerated();
        bool isNumber = representation == Representation::Double || representation == Representation::OptionalDouble;
        bool hasValue = representation != Representation::Unchosen && representation != Representation::Void && home;
        if (hasValue && !isNumber && !m_keepsValuesInHeapFrame && (user == home || home == m_onlyEntryBlock || isProvenToDominate(home, user))) [[likely]] {
            if (LValue lowered = m_values[id.index()]) [[likely]]
                return { representation, lowered };
        }
        return getSlow(id, operation);
    }
    BitVector m_elementsWhoseHolesAreLeftToTestsBehind;
    ValueID m_truthTestWithCheck;
    ValueID m_checkDeferredToTruthTest;
    LValue m_wordWhoseHoleIsLeftToTestAtWork { nullptr };
    UncheckedKeyHashMap<uint32_t, ValueID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_elementsMadePresentAtHeadsOfBlocks;
    Operand getSlow(ValueID, const Operation&);
    Operand zeroInPlaceOfValueOfFailedCode(Representation);
    Operand getNumberInFormKept(ValueID, LValue lowered);
    COLD NEVER_INLINE void failAtNumberOrNothingReadForItsBits(ValueID);
    COLD NEVER_INLINE void reportBoxedNumberUse(ValueID);
    Operand makeInWantedForm(ValueID, const Operation&, Representation wanted);
    Operand makeConstant(ValueID, const Operation&);
    void defineValue(ValueID id, Representation representation, Operand operand)
    {
        if (m_definitionTaken) [[unlikely]] {
            *m_definitionTaken = operand;
            return;
        }
        RELEASE_ASSERT(operand.representation == representation);
        if (operand.representation == Representation::Void)
            return;
        LValue& lowered = m_values[id.index()];
        RELEASE_ASSERT(!lowered && operand.value && operand.value->type() == machineTypeOf(operand.representation));
        lowered = operand.value;
    }
    BlockEdge edgeLeavingTurnLaidOut(const Edge&);
    BlockEdge edgeThatPassesArguments(const TerminatorContext&, const Edge&, std::span<const Operand> declaredResults);
    void passArguments(const Edge&, std::span<const ValueID> parameters, std::span<const Operand> declaredResults);
    void passArgument(ValueID parameter, Operand);
    void failToPassArgument(ValueID parameter, Representation found, Representation wanted);
    bool currentBlockIsOpen() const;
    struct SumOfIntegers {
        size_t indexOfParameter { 0 };
        ValueID sum;
        ValueID termAsNumber;
        ValueID term;
        LValue added { nullptr };
        bool termIsElementOfList { false };
        LValue base { nullptr };
        LValue takenOver { nullptr };
    };
    struct BoundAskedInFront {
        ValueID read;
        ValueID length;
        int64_t most { 0 };
    };
    struct ListAskedForKind {
        ValueID list;
        ArrayStorage kind { ArrayStorage::Other };
        bool isStoredInto { false };
        bool hasOneKind { false };
        bool otherKindsAreNotAsked { false };
    };
    struct KindToldToReads {
        ValueID list;
        ValueID holdsDoubles;
        uint32_t numberOfReads { 0 };
    };
    struct LoopCarriedValueStore {
        size_t indexOfParameter { 0 };
        ValueID loopCarriedValue;
        ValueID box;
        ValueID store;
    };
    struct SteppedIndex {
        size_t indexOfParameter { 0 };
        int64_t step { 0 };
        int64_t offset { 0 };
        ValueID length;
    };
    struct CountedLoop {
        size_t positionOfHead { 0 };
        Vector<BlockID, 4> turn;
        Vector<size_t, 4> positionsOfTurn;
        ValueID counter;
        ValueID bound;
        Opcode relation { Opcode::Less };
        int64_t step { 1 };
        bool testsAtEnd { false };
        int64_t addedBeforeTest { 0 };
        int64_t mostTurns { 0 };
        BlockID asking;
        bool mayComeBack { false };
        bool carriesJoinedValue { false };
        ValueID poll;
        Vector<ValueID, 2> flatStrings;
        bool turnsForWideTextsFollow { false };
        bool asksTextsForWide { false };
        Vector<SumOfIntegers, 2> sumsOfIntegers;
        Vector<LoopCarriedValueStore, 2> loopCarriedValueStores;
        Vector<ListAskedForKind, 2> listsAskedForKind;
        bool asksListsForAnotherKind() const { return std::ranges::any_of(listsAskedForKind, [](const ListAskedForKind& asked) { return !asked.hasOneKind; }); }
        Vector<ValueID, 2> listsWhoseKindIsRead;
        Vector<KindToldToReads, 2> kindsToldToReads;
        bool secondTurnsGoByKindsRead { false };
        Vector<ValueID, 2> viewsAskedForKind;
        Vector<BoundAskedInFront, 2> boundsAskedInFront;
        Vector<ValueID, 2> relationsAskedInFront;
        Vector<ValueID, 2> lengthsHeldAgainstBound;
        Vector<SteppedIndex, 2> steppedIndexes;
        unsigned numberOfOperations { 0 };
        Vector<ValueID, 2> accessesUnderCounterBelowBound;
        Vector<ValueID, 2> relationsOfCounterBelowBound;
        TypedArrayType kindAskedOfView { NotTypedArray };
        bool holesLeaveTurns { false };
        bool storesIntoSlots { false };
        bool storesIntoLists { false };
        bool rotatesNumbers { false };
        bool asksListsReadAndListsStoredInto { false };
        bool sumsAreTakenOverBehindStretches { false };
        bool layoutWithoutQuestionFollows { false };
    };
    bool findStoresOfWhatComesRound(CountedLoop&, const Edge& back) const;
    bool carriesOnlyCounter(const CountedLoop&, ValueID value, ValueID carried, unsigned depth) const;
    void storeLoopCarriedValue(const CountedLoop&, const LoopCarriedValueStore&, LValue);
    void versionIterationsByKind(CountedLoop&) const;
    struct LimitOfWrittenLoop {
        BlockID asking;
        Opcode relation { Opcode::Less };
        int64_t step { 1 };
        size_t indexOfCounter { 0 };
        LValue bound { nullptr };
        LValue limit { nullptr };
        LValue paidUpTo { nullptr };
        LValue paidAtEntry { nullptr };
        LBasicBlock entry { nullptr };
        Vector<LValue, 8> phisOfEntry;
        Vector<LValue, 8> phisOfWrittenHead;
    };
    struct RingOfLoopLaidOut {
        LBasicBlock entry { nullptr };
        LBasicBlock otherKind { nullptr };
        LBasicBlock intoWritten { nullptr };
        LBasicBlock writtenHead { nullptr };
        LValue bound { nullptr };
        LValue lastStart { nullptr };
        LValue limit { nullptr };
        LValue paidIntoWritten { nullptr };
        LValue paidUpTo { nullptr };
        Vector<LValue, 8> phisOfHead;
        Vector<LValue, 8> phisOfEntry;
        Vector<LValue, 8> phisIntoWritten;
        Vector<LValue, 8> phisOfWrittenHead;
        bool nextMayOpenAnother { false };
        bool tripCountIsChecked { false };
    };
    std::optional<RingOfLoopLaidOut> m_ringOfLoopLaidOut;
    struct LayoutsInRow {
        unsigned left { 0 };
        LBasicBlock headBehindAll { nullptr };
        Vector<LValue, 8> phisBehindAll;
    };
    std::optional<LayoutsInRow> m_layoutsInRow;
    bool someHoleLeavesTurn(const CountedLoop&) const;
    bool someUnusualTurnComesRound(const CountedLoop&) const;
    LValue askCounter(Opcode relation, LValue counter, LValue limit);
    void askLimitOfWrittenLoop(const TerminatorContext&);
    std::optional<LimitOfWrittenLoop> m_limitOfWrittenLoop;
    std::optional<CountedLoop> findCountedLoopAt(size_t positionInOrder) const;
    bool leavesKindToTurnsLaidOut(BlockID, ValueID, const Operation&) const;
    size_t m_positionInOrder { 0 };
    void layOutTurns(const CountedLoop&);
    struct LoopExit {
        BlockID block;
        uint32_t enclosing;
        Vector<ValueID, 4> values;
        Vector<LValue, 4> phis;
        Vector<LValue, 4> outerValues;
    };
    struct LoopParameterReader {
        ValueID parameter;
        BlockID block;
    };
    static constexpr uint32_t noLoopExit = UINT32_MAX;
    void findLoopExits(const CountedLoop&);
    void findLoopParameterReaders();
    bool findValuesPassedToExits(const CountedLoop&, std::span<const BlockID> blocksOfLoop, std::span<const BlockID> exits, std::span<const BlockID> directExits);
    Vector<ValueID, 8> findValuesReadOutside(std::span<const BlockID> blocksOfLoop, std::span<const ValueID> candidates) const;
    const LoopExit* findLoopExit(BlockID) const;
    void passValuesToLoopExit(BlockID);
    bool isPassedToExit(const Edge&, ValueID parameter) const;
    void leaveLoop(const Edge&, std::span<const LValue> valuesOfParameters);
    void noteScopeOfLoopExit(BlockID);
    void enterScopeOfLoopExit(BlockID);
    void emitFirstConditions(const CountedLoop&);
    Vector<LoopExit, 4> m_loopExits;
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_loopExitsByBlock;
    Vector<uint32_t> m_scopesByPlace;
    uint32_t m_currentLoopExit { noLoopExit };
    bool m_hasReadersOfLoopParameters { false };
    Vector<LoopParameterReader> m_readersOfLoopParameters;
    Vector<BlockID, 4> m_exitsOfUnrolledLoop;
    WTF::Function<LBasicBlock(const Edge&)> m_blockLeavingLoop;
    bool asksTextsOfLoopForWidth(const CountedLoop&);
    void layOutTurnsForOtherKindOfStorage(CountedLoop&, bool ofListsStoredInto = true);
    uint32_t findSharedViewKinds(const CountedLoop&) const;
    void layOutTurnsForEachKindOfView(CountedLoop&);
    void keepArgumentsForNextTurn(const Edge&);
    void findSumsOfIntegers(CountedLoop&, const Edge& back) const;
    bool addToSumOfIntegers(ValueID);
    bool isTermOfSumOfElements(ValueID read) const { return std::ranges::any_of(m_sumsOfIntegers, [&](const SumOfIntegers& sum) { return sum.termIsElementOfList && sum.term == read; }); }
    void failAtLimit(LimitID, uint64_t needed, const Subject&);

    const FunctionInputs m_inputs;
    Machine::Procedure& m_proc;
    CodeRecords& m_records;
    MentionList& m_mentions;
    const TypingContext m_typing;
    const OperationDispatchers& m_operationDispatchers;
    const TerminatorDispatchers& m_terminatorDispatchers;
    Output m_out;
    const bool m_checksAllocation;
    const bool m_checksEffects;
    const uint32_t m_understatedEffects;
    bool m_watchesOperations { false };
    bool m_notesForms { false };
    bool m_keepsValuesInHeapFrame { false };
    ValueID m_conversionLeftToConcatenation;
    Vector<ValueID> m_productsLeftToRemainders;
    Vector<std::pair<BlockID, ValueID>> m_productsMadeInRareBlocks;
    bool m_copiesBigIntsInRareBlocks { false };
    Vector<std::pair<ValueID, LValue>> m_bigIntsInFrame;
    BitVector m_switchesTakenIntoSwitchInFront;
    BitVector m_blocksWithOtherJumps;
    bool m_looksBehindEachOperation { false };
    BitVector m_storesBehindWhichOwnerIsRemembered;
    BitVector m_storesWithLaterOrderWord;
    Vector<std::pair<ValueID, ValueID>, 2> m_lastFirstStores;
    PinnedValues m_pinned;
    std::span<LValue> m_values;
    std::span<ResultRange> m_resultRanges;
    Vector<Operand> m_results;
    Machine::UncheckedSpan<LBasicBlock> m_blocks;
    bool m_nextBlockGoesOnHere { false };
    Vector<LBasicBlock, 4> m_blocksLeftOut;
    BlockID m_headOfTurnsLaidOut;
    mutable size_t m_cachedLoopHeaderPosition { std::numeric_limits<size_t>::max() };
    mutable size_t m_cachedLoopQueryPosition { std::numeric_limits<size_t>::max() };
    mutable std::optional<CountedLoop> m_cachedCountedLoop;
    mutable std::optional<bool> m_isFunctionCalledRepeatedly;
    ValueID m_readLeftToBranch;
    Vector<Operand, 8> m_argumentsForNextTurn;
    std::span<const BlockID> m_turnLaidOut;
    WTF::Function<LBasicBlock()> m_blockLeavingTurnLaidOut;
    WTF::Function<LBasicBlock()> m_blockEndingLoop;
    Vector<BlockID, 16> m_unrolledLoopBlocks;
    Vector<BlockID, 16> collectLoopBlocks(const CountedLoop&) const;
    LBasicBlock exitEdgeBlock(const Edge&);
    Vector<SumOfIntegers, 2> m_sumsOfIntegers;
    Vector<ValueID, 4> m_valuesLeftOutOfTurnsLaidOut;
    Vector<ValueID, 4> m_stepsLeftToBackEdge;
    BitVector m_boxesLeftToHandlers;
    bool m_leavesBoxesToHandlers { false };
    std::span<const ListAskedForKind> m_listsAskedForKindInFrontOfTurnsLaidOut;
    std::span<const BoundAskedInFront> m_boundsAskedInFrontOfTurnsLaidOut;
    Vector<ValueID, 2> m_relationsAskedInFrontOfTurnsLaidOut;
    std::span<const ValueID> m_accessesUnderCounterBelowBoundOfTurnsLaidOut;
    Vector<ValueID, 2> m_lengthsHeldAgainstBoundByLayoutThatAsked;
    std::optional<int64_t> mostOfKeyCutByWrittenMask(ValueID key) const;
    std::span<const ValueID> m_viewsAskedForKindInFrontOfTurnsLaidOut;
    TypedArrayType m_kindOfViewFoundInFrontOfTurnsLaidOut { NotTypedArray };
    Vector<std::pair<ValueID, LValue>, 2> m_kindsReadInFrontOfTurnsLaidOut;
    bool m_holesLeaveTurnsLaidOut { false };
    bool m_rotatesUnrolledIterationNumbers { false };
    Vector<std::pair<LValue, LValue>, 8> m_numbersOfIntegers;
    bool m_optionalNumbersLeaveAtHoles { false };
    bool m_asksElementsBehindTurns { false };
    LValue m_boxedWordsOfElementsInTurns { nullptr };
    Vector<std::pair<ValueID, LValue>, 2> m_wordsOfElementsInTurnLaidOut;
    Vector<std::pair<ValueID, LValue>, 2> m_numbersReadByPresenceTests;
    Machine::UncheckedSpan<const BlockID> m_order;
    Machine::UncheckedSpan<BlockID> m_singlePredecessors;
    Machine::UncheckedSpan<ScopeFork> m_scopeAtEndOfBlock;
    BlockID m_onlyEntryBlock;
    bool m_firstBlockHasOtherWaysIn { false };
    Vector<WayIntoBlock, 16> m_waysIntoJoins;
    Machine::UncheckedSpan<uint32_t> m_places;
    Machine::UncheckedSpan<uint32_t> m_dominatorsByPlace;
    Machine::UncheckedSpan<SubtreeOfDominators> m_subtreesByPlace;
    std::array<BlockID, 4> m_homesProven;
    BlockID m_userOfHomesProven;
    std::optional<DominatorsOfCode> m_dominators;
    std::optional<BitVector> m_stepsReadElsewhere;
    BlockID m_blockOfCachedHandler;
    const Edge* m_cachedHandler { nullptr };
    const OperationContext* m_operation { nullptr };
    Operand* m_definitionTaken { nullptr };
    EmittersMade m_emitters;
    unsigned m_initializationDepth { 0 };
    bool m_owesFenceBehindStores { false };
    bool m_isPassingHandlerArguments { false };
    bool m_isAheadOfWhatMayThrow { false };
    bool m_hasFailed { false };
    bool m_carriesParametersBoxed { false };
    BitVector m_parametersCarriedBoxed;
    BitVector m_valuesDefinedBoxed;
    bool m_hasKeysAskedTwice { false };
    BitVector m_keysAskedTwice;
    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blocksOfFirstAccesses;
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

inline ValueID valueOwnedByTerminator(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::TestAndBranch:
        return terminator.conditionOperand();
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTBody.h"
#include "AOTBodyIndex.h"
#include "AOTCode.h"
#include "AOTCompileUnit.h"
#include "AOTDiagnostics.h"
#include "AOTMechanisms.h"
#include "AOTWalkLinks.h"
#include "AOTWalkLoopsOverGeneratorsOfUnit.h"
#include "AOTWalkCoroutineState.h"
#include "AOTWalkState.h"
#include <array>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/IterationStatus.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class Walker;

class NamesOfWay {
public:
    Reference nameIn(uint32_t slot) const { return slot < m_words.size() ? Reference::fromBits(~m_words[slot]) : Reference(); }
    void setNameIn(uint32_t slot, Reference name)
    {
        if (m_words.size() <= slot)
            m_words.grow(0, slot + 1);
        m_words.mutableAt(slot) = ~name.bits();
    }
    void clear() { m_words.clear(); }
    void forEachSlotNamedOtherwiseBy(const NamesOfWay& other, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        const NamesOfWay& longer = m_words.size() < other.m_words.size() ? other : *this;
        const NamesOfWay& shorter = &longer == this ? other : *this;
        longer.m_words.forEachStretchNotSharedWith(shorter.m_words, [&](uint32_t begin, uint32_t end) {
            for (uint32_t slot = begin; slot < end; ++slot) {
                if (longer.nameIn(slot) != shorter.nameIn(slot))
                    functor(slot);
            }
        });
    }

private:
    SharedArray<uint32_t, 32> m_words;
};

bool buildDeferredDefinition(Walker&, const BodyValue&);
OpcodeSet operationsThatMayKeepEnvironments();
inline bool mayKeepEnvironment(Opcode opcode)
{
    static const OpcodeSet opcodes = operationsThatMayKeepEnvironments();
    return opcodes.contains(opcode);
}
void markEnvironmentsThatOperationKeeps(Walker&, const Draft&);
bool isHandedEnvironmentWithByte(const Walker&, FunctionID);
void noteEnvironmentThatUnitMakes(Walker&, const Draft& allocation);
void handBackEnvironmentsAtReturn(Walker&, BlockID inputBlock);
void markEnvironmentsThatCoroutineIsHanded(Walker&);
void takeWhatIsNoLongerCaptured(Walker&);
void stopAtSecondEntryOfMaker(Walker&);
bool leaveEnvironmentAmongVariables(Walker&, const BodyValue&);
void storeValuesNoLongerCaptured(Walker&, const BodyValue&);
Reference wordInPlaceOfRemovedCapture(Walker&, FunctionID, uint32_t capture);
Reference valueNoLongerCaptured(Walker&, FunctionID, uint32_t capture, Origin);
bool isVariableFilledAtFirstRead(const Plan&, VariableID);
bool takeObjectOfDeferredVariable(Walker&, VariableID, Reference held, Origin);
bool foldOperation(Walker&, const BodyValue&);
bool buildFreshObject(Walker&, const BodyValue&);
bool callBuiltin(Walker&, const BodyValue&);
void nameMethodOfReceiverAsMade(Walker&, const BodyValue& call, Draft&);
void sayWhatIsExpectedOfComparedOperands(Walker&, const BodyValue& equality, Draft&);
inline constexpr OpcodeSet opcodesThatCallFunctionGivenToVariableTakes { Opcode::Call };
AOT_LINK_OF_WALK(callFunctionGivenToVariable, opcodesThatCallFunctionGivenToVariableTakes)
inline constexpr OpcodeSet opcodesThatOpenGuessAtElementOfNumbersTakes { Opcode::ElemGet };
AOT_LINK_OF_WALK(openGuessAtElementOfNumbers, opcodesThatOpenGuessAtElementOfNumbersTakes)
bool inlineCallee(Walker&, const BodyValue&);
bool callDirectly(Walker&, const BodyValue&);
bool buildTypedOperation(Walker&, const BodyValue&);
enum class GuessOpened : uint8_t {
    None,
    AsOneOperation,
    WithArms,
};
TypeID typeTestedInPlaceOfWrittenType(Walker&, FunctionID, uint32_t entryParameter, TypeID tested);
GuessOpened openGuessAt(Walker&, const BodyValue&);
bool openGuessAtAllocation(Walker&, const BodyValue&);
bool replaceOperation(Walker&, const BodyValue&);

std::optional<uint32_t> onlyTargetKnownToFolding(Walker&, const BodyTerminator&);
bool buildTypedAccess(Walker&, const BodyValue&);
bool buildTypedElementAccess(Walker&, const BodyValue&);
bool buildTypedOperator(Walker&, const BodyValue&);
bool buildTypedConstruction(Walker&, const BodyValue&);
std::optional<uint32_t> positionOfTagAmongArguments(const Walker&, FunctionID constructor);
bool everyReceiverKeepsFieldInSlotThatIsNeverEmpty(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID);
TypeID typeOfPairsThatStepsHandOut(Walker&, TypeID iterable);
bool listsAreIteratedUnseen(Walker&);
inline constexpr OpcodeSet opcodesThatOpenLoopOverIteratorThatEngineMakesTakes { Opcode::PropGet };
AOT_LINK_OF_WALK(openLoopOverIteratorThatEngineMakes, opcodesThatOpenLoopOverIteratorThatEngineMakesTakes)
AOT_LINK_OF_WALK(openTestOfListAtReadOfMethod, opcodesThatOpenLoopOverIteratorThatEngineMakesTakes)
bool isListOfEntriesTakenApartAtOnce(Walker&, const BodyValue& call);
bool buildTypedIteration(Walker&, const BodyValue&);
bool replaceIteration(Walker&, const BodyValue&);
bool replaceCoroutineOperation(Walker&, const BodyValue&);
bool replaceCallDoneInPlace(Walker&, const BodyValue&);
bool replaceCallOfCompiledBuiltin(Walker&, const BodyValue&);
bool isCallForwardedInPlace(Walker&, const BodyValue&);
bool forwardCallOfBuiltin(Walker&, const BodyValue&);
bool forwardCallBehindTestOfCallee(Walker&, const BodyValue&);
class InputsOfTypedOperation;
using TypesOfInputs = ScopedLambda<TypeID(ValueID)>;
struct GuessCountedOnByLooks {
    ValueID operation;
    ValueID operand;
    TypeID expected;
};
bool buildFormOnShapes(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool mayAccessThroughSetOfLayouts(const Walker&, LayoutSetID);
bool paysToFindOffsetByShape(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID, bool isStore);
TypeID typeOfObjectsMadeBy(const Walker&, FunctionID constructor);
TypeID typeOfObjectsMadeThroughEntryOf(const Walker&, FunctionID constructor);
bool chooseLayoutByTagAtConstruction(Walker&, const BodyValue& construction, FunctionID constructor, LayoutID& constructed);
Reference buildThisByTag(Walker&, FunctionID constructor, Reference prototype, Reference tag, Origin);
void refineExpectationOfValue(Walker&, ValueID input, TypeID& found, bool& isDeclared);
void bindNamesBehindFailedTest(Walker&, Reference value, Reference test, uint32_t operandIndexOfTest, std::span<const ValueID> names, Origin);
Reference findCaptureAtHand(Walker&, FunctionID callee, uint32_t capture, Reference closure);
Reference valueOfStableBinding(Walker&, const BodyValue& read);
Reference valueOfStableVariable(Walker&, VariableID, TypeID widest);
void noteValueOfStableBinding(Walker&, VariableID, Reference value);
void noteValueOfStableBinding(Walker&, Reference environment, FieldReference, Reference value);
Reference objectOfVariablePutOff(Walker&, VariableID);
void noteObjectOfVariablePutOff(Walker&, VariableID, Reference object);
Reference findScopeOfClosureReadOffObjectInSight(const Walker&, FunctionID, Reference closure);
bool isClosureLeftUnmade(const Walker&, Reference);
Vector<Reference, 2> collectWhatCopyAloneHolds(Walker&, const BodyValue& call);
void letGoWhatCopyHeld(Walker&, std::span<const Reference> held, Origin);
FunctionID findLibraryFunctionBehindBuiltin(const Walker&, TypeID typeOfCallee);
struct LibraryFunctionsBehindBuiltin {
    FunctionID enteredByValue;
    Vector<FunctionID, 2> named;
};
LibraryFunctionsBehindBuiltin findLibraryFunctionsBehindBuiltin(const Walker&, TypeID typeOfCallee);
bool takesNothingOfFunctionObject(const Program&, FunctionID);
FunctionID findLibraryFunctionOfRowsReachedBy(Walker&, const BodyValue& call);
bool isLeafHandedToMethodOfLists(const Walker&, TypeID handedOver);
bool mayBeLeafHandedToMethodOfLists(const Walker&, TypeID handedOver);
PropertyKeyID keyOfSymbolMadeOnce(Walker&, const BodyValue&);
bool tableOfImageHoldsData(Walker&, DataID);

PropertyKeyID keyThatAccessGoesBy(const Walker&, const BodyValue& access, PropertyKeyID* chosenAndNotDerived = nullptr);
inline constexpr OpcodeSet accessesUnderComputedKeys { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };

class AccessUnderProvenKey {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AccessUnderProvenKey);
    WTF_MAKE_NONCOPYABLE(AccessUnderProvenKey);
public:
    AccessUnderProvenKey(const BodyValue& written, PropertyKeyID);

    ValueID id() const { return m_id; }
    PropertyKeyID key() const { return PropertyKeyID::fromBits(m_entryOfKey - 1); }
    BodyValue value() const LIFETIME_BOUND { return BodyValue::operation(m_words.data(), &m_unit, m_id); }

private:
    std::array<uint32_t, BodyValue::numberOfHeadWords + 6> m_words { };
    uint32_t m_entryOfKey { 0 };
    UnitIDRemap m_unit;
    ValueID m_id;
};

void takeArrivals(Walker&);
void markEscapingOperands(Walker&, const Draft&, const Effects* foundByCaller);
void noteFreshList(Walker&, Reference list);
void noteListMadeByCaller(Walker&, Reference list);
void noteListAsMade(Walker&, Reference list);
bool isListAsMade(Walker&, Reference list);
std::optional<uint32_t> lengthOfFreshList(Walker&, Reference list);
Reference elementOfFreshList(Walker&, Reference list, uint32_t position);
std::optional<Vector<Reference, 8>> elementsOfFreshList(Walker&, Reference list);
std::optional<uint32_t> lengthOfListInSight(Walker&, Reference list);
Reference elementOfListInSight(Walker&, Reference list, uint32_t position);
std::optional<Draft> draftOfCallWithListWrittenOut(Walker&, const BodyValue& call);
bool isFreshListAsMade(Walker&, Reference list);
void noteFreshObject(Walker&, Reference object, size_t numberOfFieldsWritten);
struct PropertyOfFreshObject {
    FieldReference field;
    Reference value;
};
std::optional<Vector<PropertyOfFreshObject, 8>> propertiesOfFreshObject(Walker&, Reference object);
Reference lengthOfFreshArguments(Walker&, Reference arguments);
Reference elementOfFreshArguments(Walker&, Reference arguments, uint32_t position);
struct ListBehindParametersOfArguments {
    Reference list;
    uint32_t firstPosition { 0 };
};
inline constexpr uint32_t mostParametersOfArgumentsAskedOneByOne = 4;
bool positionsOfFreshArgumentsAreInSight(Walker&, Reference arguments, int64_t lowest);
std::optional<ListBehindParametersOfArguments> listBehindParametersOfFreshArguments(Walker&, Reference arguments, bool forItsOwnElementsAlone = false);
Reference listStandingForFreshArguments(Walker&, Reference arguments);
std::optional<Vector<Reference, 8>> elementsOfFreshArguments(Walker&, Reference arguments);
struct SliceOfElementsInSight {
    const BodyValue& call;
    Reference sliced;
    bool isListByItsRow { false };
    std::span<const Reference> bounds;
    Origin origin;
    ExpectedType expected;
    bool resultIsReturnedAtOnce { false };
};
Reference buildSliceOfElementsInSight(Walker&, const SliceOfElementsInSight&);
bool readLeavesFreshObjectAsItIs(Walker&, const BodyValue& read, uint32_t operandIndex, Reference object, TypeID typeOfKey);
struct LoopPreview;
struct ListTestedInFrontOfLoop;
void hoistInvariants(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
void enterHandler(Walker&, BlockID inputHandler);
Representation representationOfNumber(const Walker&, TypeID);

bool walkRampOfCoroutine(Walker&);
Reference addDriverParameterOfCoroutine(Walker&, BlockID entry);
Reference convertReturnedValueOfCoroutine(Walker&, Reference returned, Origin);
Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin);

void checkInt32Promise(Walker&, const DeclaredType&, ValueID entering, Origin);

bool isTextBuiltOfParts(Walker&, Reference text);

struct StepOfListLiteral {
    bool isDone { false };
    ValueID element;
};
std::optional<StepOfListLiteral> stepOfListLiteral(Walker&, const BodyIndex&, const BodyValue& step);
bool isPartOfIterationOverListLiteral(Walker&, const BodyIndex&, const BodyValue&);

FunctionID findOnlyCallee(Walker&, const BodyValue& call, Reference callee);
FunctionID findOnlyCallee(Walker&, const BodyValue& call, TypeID typeOfCallee);
bool makesThisOfConstruction(const Walker&, FunctionID constructor);
FunctionID findFunctionBehindCompiledBuiltin(const Walker&, TypeID typeOfCallee);
bool callAccessor(Walker&, const BodyValue& access, FunctionID accessor, Reference thisValue, Reference stored, Reference closure = { });
bool accessorAdmitsDirectCall(Walker&, FunctionID accessor);
bool canCallCompilerHelper(Walker&, FunctionID helper);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);
enum class MayCopyFunctionCalledDirectly : bool { No, Yes };
bool callFunctionDirectly(Walker&, const BodyValue& operation, FunctionID, Reference thisValue, std::span<const Reference> arguments, MayCopyFunctionCalledDirectly = MayCopyFunctionCalledDirectly::Yes);
bool callGetterExpectedAtRead(Walker&, const BodyValue& read);
bool askGuardsAboutDeclaredMember(Walker&, const BodyValue&);
LayoutSetID receiversWhoseCellsAnswerForOwnProperty(Walker&, ValueID input, PropertyKeyID);
bool cellAnswersForOwnProperty(Walker&, TypeID typeOfObject, PropertyKeyID);
bool recordsDeclareMemberAboveObjectsOf(Walker&, TypeID typeOfObjects, PropertyKeyID);
bool findGuardsOfMethodAboveObjectsOf(Walker&, TypeID typeOfObjects, PropertyKeyID, Vector<MemberConditionRecord, 4>&);
FunctionID findGetterOfRead(Walker&, Opcode, TypeID lookedAt, PropertyKeyID);
bool callForwardedDirectly(Walker&, const BodyValue& call, const Draft& forwarded);
bool listOfSuperCallCanBeHandedOver(Walker&, const BodyValue& call, FunctionID constructor);
bool listSpreadBehindArgumentsIsRestListOf(Walker&, const BodyValue& call, FunctionID callee);
bool spreadAppendsElementsOfListAsMade(Walker&, ValueID spreadValue);
bool callBuiltinForwarded(Walker&, const BodyValue& call, const Draft& forwarded);
TailCallRequirement requirementOfTailCall(const Walker&, Reference call);
bool calleeOfTailCallComesBack(const Walker&, FunctionID callee, std::span<const Reference> arguments);
bool passOnOwnArgumentsByCount(Walker&, const BodyValue& call, const Draft& written, Reference callee, Reference thisValueOrNull, Reference arguments);

Reference callSlowVersion(Walker&, const Draft& site, FunctionID callee, std::span<const Reference> entryArguments, bool isConstruction);

struct PlaceOfCall {
    FunctionID caller;
    bool isInLoop { false };
    const BodyIndex* body { nullptr };
    ValueID call;
};
struct TextOfCopyRead {
    Vector<uint32_t, 2> turnsOfLoops;
    BitVector isEntered;
    BitVector leadsToTakenAlone;
    BitVector leadsToNotTakenAlone;
};
std::optional<TextOfCopyRead> turnsOfLoopsOfCopyWrittenOut(const Walker&, FunctionID callee, std::span<const std::optional<int64_t>> integersHandedOver, std::span<const std::optional<uint32_t>> lengthsOfListsHandedOver);
struct TurnsOfCopyWrittenOut {
    std::optional<unsigned> entryParameterOfList;
    uint32_t firstArgumentListed { 0 };
    TextOfCopyRead text;
};
std::optional<TurnsOfCopyWrittenOut> turnsOfCopyWrittenOutAt(const Walker&, const Draft& site, FunctionID callee, const PlaceOfCall&);
struct ForOfByText {
    ValueID open;
    ValueID step;
};
std::optional<ForOfByText> findForOfSteppedAtHead(const BodyIndex&, LoopID);
bool openOverListAsMadeGoesByIndex(Walker&, const BodyIndex&, ValueID open);
ValueID openOfCleanupThatOnlyThrowsOn(const BodyIndex&, BodyBlock);
bool mayCopyCallee(const Walker&, const Draft& site, FunctionID callee, const PlaceOfCall&);
bool shouldCopyCallee(const Walker&, const Draft& site, FunctionID callee, std::span<const Reference> handedOver, bool callsParameterOfCopy = false);
Vector<Reference, 8> operandsHandedToCopy(const Draft& site, const Function& callee);

struct NameOfGrowingText {
    ValueID value;
    ValueID version;
};
struct GrowingText {
    BlockID loopHead;
    uint32_t parameterIndex { 0 };
    Vector<NameOfGrowingText, 8> names;
    Vector<ValueID, 2> parametersOfInnerHeads;
    bool isOnlyConcatenated { false };
    bool takesTextOfJSONStringify { false };
    bool standsUnderHandlers { false };
};
struct TextInStringBuilder {
    GrowingText web;
    uint32_t frameIndex { 0 };
    Reference builder;
    bool beganEmpty { false };
    bool liesInFrame { false };
    Reference stringOfState;
    Reference lengthOfState;
    BlockID blockOfState;
};

struct PlacesOfTextsInStringBuilders {
    using Ends = HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    Ends endsByName;
    Ends endsByLoopHead;
    Vector<uint32_t> endsBeforeInSameLoop;
    Vector<uint32_t, 4> endsByFrame;
};

class Walker {
    WTF_MAKE_NONCOPYABLE(Walker);
public:
    Walker(const WalkInputs&, TypeArena&, Arrivals&);
    ~Walker();

    bool walk(CodeUnit);
    bool standInFrontOfBodyOf(FunctionID);
    Code& code() LIFETIME_BOUND { return m_code; }
    struct MethodGivenUnderKey {
        BuiltinClassID builtinClass;
        BuiltinID method;
    };
    UncheckedKeyHashMap<uint64_t, Vector<MethodGivenUnderKey, 2>> methodsGivenUnderKeys;
    struct ReceiverMadeInBody {
        ValueID made;
        uint8_t classesItStaysAsMadeOf;
    };
    UncheckedKeyHashMap<uint32_t, Vector<ReceiverMadeInBody, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> receiversMadeInBodies;
    struct GettersExpectedInFunction {
        FunctionID function;
        std::span<const LinkedGetterAtRead> reads;
    };
    GettersExpectedInFunction gettersExpectedInFunction;
    const Code& code() const LIFETIME_BOUND { return m_code; }

    const Program& program() const { return m_inputs.program; }
    const Facts& facts() const { return m_inputs.facts; }
    const Plan& plan() const { return m_inputs.plan; }
    TypeArena& types() const { return m_types; }
    Arrivals& arrivals() { return m_arrivals; }
    WalkState& state() { return m_state; }
    const WalkState& state() const { return m_state; }
    void noteEnvironmentField(Reference environment, FieldReference, Reference value);
    bool has(Mechanism mechanism) const { return m_mechanisms.contains(mechanism); }
    bool hidesNames() const { return m_inputs.options.hidesNames; }
    RegExpSummaries* regExpSummaries() const { return m_inputs.regExpSummaries; }
    FormatPatternPlans* formatPatternPlans() const { return m_inputs.formatPatternPlans; }
    ScratchRealm* scratchRealm() const { return m_inputs.scratchRealm; }
    CodeUnit unit() const { return m_code.unit(); }
    TypeID typeArrivingAtEntryParameter(unsigned entryParameter, bool believesCallers = true);
    bool opensGuesses() const { return m_opensGuesses; }
    Origin originOfOperation() const { return m_origin; }
    ALWAYS_INLINE FunctionID currentFunction() const { return m_frames.last().function; }
    const Body& currentBody() const;
    ALWAYS_INLINE const BodyIndex& currentBodyIndex() const { return *m_frames.last().index; }
    ALWAYS_INLINE bool isInLoop() const
    {
        const Frame& frame = m_frames.last();
        if (frame.loopDepthAtCall)
            return true;
        if (!frame.index || !frame.block)
            return false;
        return frame.index->loopDepthOf(frame.block) > (frame.standsInFrontOfHead ? 1u : 0u);
    }
    bool isBeingWalked(FunctionID) const;
    TypeID leastTypeOfHeadParameter(FunctionID, BlockID inputHead, uint32_t parameter) const;
    bool isLoopThatMayDoAnything(FunctionID, BlockID inputHead) const;
    size_t depthOfCopies() const { return m_frames.size() - 1; }
    TypeID typeExpectedOfAnswerOfCopy() const { return m_frames.last().typeExpectedOfAnswer; }
    bool answerOfCopyIsDeclared() const { return m_frames.last().answerIsDeclared; }
    bool copyHandsBack(ValueID);
    std::optional<uint32_t> entryParameterOfUnitBoundTo(Reference value) const
    {
        const Frame& ofUnit = m_frames.first();
        uint32_t count = std::min<uint32_t>(program().at(ofUnit.function).numberOfEntryParameters(), static_cast<uint32_t>(ofUnit.references.size()));
        for (uint32_t index = 0; value && index < count; ++index) {
            if (ofUnit.references[index] == value)
                return index;
        }
        return std::nullopt;
    }
    uint32_t operationsCopiedAtWrittenCall() const { return m_operationsCopiedAtWrittenCall; }
    std::span<const Reference> closuresThatAllocationAloneTakes() const LIFETIME_BOUND { return m_closuresThatAllocationAloneTakes.span(); }
    BlockID currentBlock() const { return m_block; }
    Origin originOf(const BodyValue& value) const
    {
        Origin origin = value.origin();
        origin.inlineFrame = m_frames.last().inlineFrame;
        return origin;
    }

    ALWAYS_INLINE Reference referenceOf(ValueID input) const
    {
        const Frame& frame = m_frames.last();
        Reference bound = frame.references[input.index()];
        if ((frame.firstNarrowerName == noNarrowerName || !(frame.narrowedValues & Frame::bitOfNarrowedValue(bound))) && !m_numbersOfStates.newestCheckedNameOf(bound) && (bound.isRecord() || bound)) [[likely]]
            return bound;
        return narrowestReferenceOf(input);
    }
    Reference referenceBoundTo(ValueID input) const { return referenceBoundIn(m_frames.last(), input); }
    Reference nameLastBoundTo(ValueID input) const { return m_frames.last().lastNameOf(input); }
    ALWAYS_INLINE void bind(ValueID input, Reference reference)
    {
        Frame& frame = m_frames.last();
        Reference& bound = frame.references[input.index()];
        if (m_numberOfClosedArms) [[unlikely]]
            m_bindings.append(Binding { static_cast<uint32_t>(m_frames.size() - 1), input, bound });
        if (frame.valuesBuiltTwice) [[unlikely]]
            frame.valuesBuiltTwice->noteBoundOnWay(input, reference);
        bound = reference;
    }
    ALWAYS_INLINE TypeID typeOf(Reference reference) const
    {
        if (hasTextsInStringBuilders()) [[unlikely]]
            return typeOfTextOrRecord(reference);
        return typeAsBuilt(reference);
    }
    ALWAYS_INLINE bool nothingIsProvenOf(Reference reference) const { return reference && reference.isValue() && m_types.saysNothing(typeOf(reference)); }
    ALWAYS_INLINE Representation representationOf(Reference reference) const
    {
        if (!reference || !reference.isRecord())
            return Representation::Unchosen;
        return m_code.at(reference.id()).representation;
    }
    void letLeave(Reference reference) { letLeave(m_state, reference); }

    BodyValue asWalked(const BodyValue& written) { return accessesUnderComputedKeys.contains(written.opcode()) ? accessAsWalked(written) : written; }

    struct IterationByIndex {
        enum class Contents : uint8_t { List, View, Map, Set, Text };
        Reference list;
        Reference position;
        Reference table;
        Reference state;
        Reference iterator;
        Reference next;
        ValueID literal;
        Vector<Reference, 4> takenAtOpen;
        bool isTakenAtOpen { false };
        BlockID inputHead;
        BlockID head;
        uint32_t inlineFrame { 0 };
        BuiltinClassID builtinClass;
        Contents contents { Contents::List };
        JSC::IterationKind kind { JSC::IterationKind::Values };
        bool mayGoByProtocol { false };
        bool isOpenAtEveryStepAndClose { false };
        bool stateHoldsVerdictOnNext { false };
        bool holdsFlatEntries { false };
        Reference valueBesideKey;
        enum class OtherForm : uint8_t {
            None,
            Enumeration,
            LoopOverInstance,
            PiecesOfSplit,
            Protocol,
        };
        OtherForm otherForm { OtherForm::None };
        Reference subject;
        Reference count;
        ValueID newestStep;
        Reference foundAt;
        Reference keyForAccesses;
        bool isAsMadeForGood { false };
        bool isDeadBehindLoop { false };
        bool goesThroughListByIndexAlone() const { return !mayGoByProtocol && !isTakenAtOpen && !holdsFlatEntries && !literal && !table && contents == Contents::List && otherForm == OtherForm::None; }
        TypeID keysProven;
        TypeID valuesProven;
        TypeID elementsProven;
    };
    static constexpr int64_t positionBehindEveryEnd = int64_t { 1 } << 35;
    IterationByIndex& iterationByIndex(Reference state) LIFETIME_BOUND { return m_iterationsByIndex[state.object().index()]; }
    Reference addIterationByIndex(IterationByIndex&&);
    std::optional<bool>& listsAreIteratedByIndex() { return m_listsAreIteratedByIndex; }

    Draft draftOf(const BodyValue&);
    Reference build(Draft&&, const Effects* foundByCaller = nullptr);
    Reference constant(Draft&&);
    Reference wordThatSaysNothing(TypeID);
    Reference cellThatRunTimeKeeps(Draft&);
    bool copy(const BodyValue&);
    void chooseConservativeFields(Draft&);

    BlockID addBlock(BlockFrequency = BlockFrequency::Normal);
    void jumpTo(BlockID next, std::span<const Reference> arguments = { });
    void branch(Reference condition, BlockID taken, BlockID notTaken);
    void switchOn(Reference subject, std::span<const int64_t> cases, std::span<const BlockID> targets, BlockID otherwise);
    Reference tryCallInline(TerminatorDraft&&, BlockID done, BlockID miss, const Effects* foundByCaller = nullptr);
    void continueIn(BlockID);
    Reference addParameter(BlockID, TypeID, Representation);
    Representation formOfParameter(TypeID, std::span<const Representation> arguments);
    struct ArmThatMeets {
        BlockID last;
        Vector<Reference, 8> values;
    };
    bool letArmsMeet(std::span<const ArmThatMeets>, BlockID meeting, std::span<Reference> met);
    void endWithReturn(std::span<const Reference>, Origin);
    void endWithThrow(Reference, Origin);
    void endWithRethrow(Reference exception, Origin);
    void endAsUnreachable(Origin);
    void holdWhatBodyReadsOfItsOwnScope(const BodyValue& read, Reference environment, Reference found);
    void noteStoreIntoSlotStoredFirst(const BodyValue& store, Reference environment);
    struct EdgeToTargetOfBody {
        BlockID inputBlock;
        uint32_t targetIndex { 0 };
    };
    void endBlockLeadingTo(TerminatorDraft&&, std::span<const EdgeToTargetOfBody>);
    bool unitReturnsAtOnce(const Draft& draft) const { return draft.resultIsReturnedAtOnce && (m_frames.size() == 1 || m_frames.last().returnsInPlaceOfUnit); }
    void returnAtOnce(Reference call);

    void walkStretch(BlockID inputBlock, uint32_t begin, uint32_t end);
    bool endArmWithReturnOf(BlockID inputBlock);
    bool endArmWithBranchOf(BlockID inputBlock);
    bool armsMayEndWithBranchBehind(const BodyValue& operation);
    bool throwsPastCleanupOf(BodyBlock block) const { return throwsPastHandlerOf(block); }
    bool endArmWithBranchBehind(const BodyValue& operation);
    bool armsTakeBranchForProofOf(BlockID inputBlock, uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter);
    uint32_t endOfStretchBeingWalked() const { return m_frames.last().endOfStretch; }
    uint32_t beginOfStretchBeingWalked() const { return m_frames.last().beginOfStretch; }
    bool walksStretchInsideAnother() const { return m_frames.last().numberOfStretchesBeingWalked > 1 || isWalkingBlockForEachWay(m_frames.size() - 1); }
    struct PlaceTakenByEndOfBlock {
        BlockID block;
        uint32_t place { 0 };
    };
    PlaceTakenByEndOfBlock& placeTakenByEndOfBlockAskedLast() { return m_frames.last().placeTakenByEndOfBlockAskedLast; }
    void noteGuessOpened() { ++m_numberOfGuessesOpened; }
    void noteFillersOfAllocation(Reference allocation, Vector<FieldIndex, 2>&& fields) { m_fillersOfAllocations.set(allocation.id().index(), WTF::move(fields)); }
    std::span<const FieldIndex> fillersOfAllocation(Reference allocation) const LIFETIME_BOUND
    {
        auto found = m_fillersOfAllocations.find(allocation.id().index());
        return found == m_fillersOfAllocations.end() ? std::span<const FieldIndex>() : found->value.span();
    }
    struct RememberedExpectation {
        TypeID type;
        bool isDeclared { false };
        bool isHeld { false };
        bool isKnown { false };
    };
    RememberedExpectation& rememberedExpectationOf(ValueID);
    std::optional<RememberedExpectation> wholeExpectationKeptOf(ValueID parameter) const
    {
        const auto& kept = m_frames.last().wholeExpectations;
        auto found = kept.isEmpty() ? kept.end() : kept.find(parameter.index());
        return found == kept.end() ? std::nullopt : std::optional(found->value);
    }
    void keepWholeExpectationOf(ValueID parameter, RememberedExpectation whole) { m_frames.last().wholeExpectations.set(parameter.index(), whole); }
    Vector<ValueID, 16>* expectationsRememberedForLook() { return m_expectationsRememberedForLook; }
    void setExpectationsRememberedForLook(Vector<ValueID, 16>* list) { m_expectationsRememberedForLook = list; }
    const ScopedLambda<TypeID(ValueID)>* typesOfInputsInLook() const { return m_typesOfInputsInLook; }
    void setTypesOfInputsInLook(const ScopedLambda<TypeID(ValueID)>* types) { m_typesOfInputsInLook = types; }
    bool lookHasAskedAboutUnbuiltValue() const { return m_lookHasAskedAboutUnbuiltValue; }
    void setLookHasAskedAboutUnbuiltValue(bool has) const { m_lookHasAskedAboutUnbuiltValue = has; }
    std::span<const PropertyKeyID> namesUsedOff(ValueID);
    std::span<const uint64_t> castsOf(ValueID);
    enum class RoundKeptByWalker : uint8_t { OnlyLayoutWithAllNames, ShapesOfLayouts, DescribesBagOfOptions, OnlyLayoutWithName };
    static bool keepsRoundsOver(const TypeArena&, LayoutSetID);
    std::optional<uint32_t> roundKept(RoundKeptByWalker, LayoutSetID, uint32_t second) const;
    void keepRound(RoundKeptByWalker, LayoutSetID, uint32_t second, uint32_t answer);
    uint32_t numberOfListOfNames(std::span<const PropertyKeyID>);
    std::span<const BodyIncomingEdge> incomingEdgesOf(const BodyIndex&, BlockID);
    uint32_t numberOfGuessesOpened() const { return m_numberOfGuessesOpened; }
    uint32_t numberOfLoopsBuiltTwice() const { return m_numberOfLoopsBuiltTwice; }
    uint32_t numberOfAssignmentsWrittenOut() const { return m_numberOfAssignmentsWrittenOut; }
    void noteAssignmentWrittenOut() { ++m_numberOfAssignmentsWrittenOut; }
    uint32_t numberOfJoinsWalkedForEachWay() const { return m_numberOfJoinsWalkedForEachWay; }
    uint32_t numberOfTurnsThatBeginAgain() const { return m_numberOfTurnsThatBeginAgain; }
    void noteSliceMadeOfElementsInSight() { ++m_numberOfSlicesMadeOfElementsInSight; }
    uint32_t numberOfSlicesMadeOfElementsInSight() const { return m_numberOfSlicesMadeOfElementsInSight; }
    struct CountOfUnit {
        StatisticID statistic;
        uint32_t amount;
    };
    void stopAtMaker(Origin);
    bool constantEndsWhereItLeaves(ValueID written, DataID) const;
    void showThatObjectHoldsWhatIsHeldFilled(Reference object, LayoutID);
    Vector<FieldIndex, 8> fieldsHeldFilledThatAreNotShown(Reference object, LayoutID);
    struct SurveyOfConstant {
        bool tableOfImageHoldsIt { true };
        bool allocationsAloneMakeIt { true };
        uint32_t numberOfNodes { 0 };
    };
    using SurveysOfConstants = HashMap<uint32_t, SurveyOfConstant, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    SurveysOfConstants& surveysOfConstants() { return m_surveysOfConstants; }
    bool constantListsFieldsHeldFilled(DataID, RootOfConstant);
    SetForScope<bool> makingObjectsOfConstant() { return SetForScope(m_isMakingObjectsOfConstant, true); }
    void count(StatisticID statistic, uint32_t amount = 1)
    {
        for (CountOfUnit& count : m_countsOfUnit) {
            if (count.statistic == statistic) {
                count.amount += amount;
                return;
            }
        }
        m_countsOfUnit.append({ statistic, amount });
    }
    std::span<const CountOfUnit> countsOfUnit() const LIFETIME_BOUND { return m_countsOfUnit.span(); }
    uint32_t numberOfValuesWalked() const { return m_numberOfValuesWalked; }
    bool checksProofs() const { return m_positionsWithoutProofs && m_inputs.options.sourcesOfProofsChecked == everySourceOfProofs && (unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody); }
    bool checksProofsOf(ProofSource source) const { return m_positionsWithoutProofs && (m_inputs.options.sourcesOfProofsChecked & bitOfSourceOfProofs(source)); }
    bool testsEveryPosition() const { return m_inputs.options.testsEveryPosition; }
    bool takesPositionsToLieInside() const { return m_inputs.options.takesPositionsToLieInside; }
    bool takesCalleesToKeepNothing() const { return m_inputs.options.takesCalleesToKeepNothing; }
    void checkProof(Reference value, TypeID withoutProof, ProofSource, Origin);
    void checkProof(Reference value, TypeID proven, TypeID withoutProof, ProofSource, Origin);
    void checkProofOfCopy(Reference value, TypeID proven, ProofSource, Origin);
    Reference proveOrTrap(Reference value, TypeID tested, ProofSource, Origin);
    TypeID typeThatUnitMayTestFor(TypeID tested, bool mayBringClasses = true, bool asOfUnitWithoutReach = false);
    void checkThatSlotTakes(Reference value, TypeID taken, Origin);
    void holdStoredValuesToSlots(const Draft&);
    void holdStoredValuesToPlaces(const Draft&);
    void holdToTypeOfPlace(Reference value, TypeID held, ProofSource, Origin);
    void holdSlotsOfScopesThatLeaveAt(const Vector<SummaryScopeStoredFirst>&, uint32_t position, bool standsBehindOperationsOfBlock);
    void holdVariableOfAnotherFileStoredFirst(VariableID);
    void holdVariablesStoredFirstWhereFunctionsAreUsed(std::span<const LinkedVariableStoredFirst> claims, std::span<const FunctionID> used, VariableID storedByOperation);
    void holdSlotStoredFirst(Reference environment, FieldReference, Reference found);
    bool makeTestOnTheWayAskInFull(Reference value, TypeID taken);
    TypeID typeThatDoorsHoldOfSlotRead(const Operation& read);
    bool makeObjectOfSlotReadShown(Reference value, TypeID taken);
    void buildCheckOfProof(Reference value, TypeID proven, TypeID withoutProof, ProofSource, Origin, bool mayBringClasses = false);
    void buildCheckThatTestHolds(Reference test, ProofSource, Origin);
    bool splitNullishOffWhatContainerHasJustGiven(Reference given, TypeID rest, Draft&& throwsOffNullish);
    bool letTestInFrontOfBranchAskWhatContainerHasJustGiven(const BodyValue& test);
    TypeID typeShown(Reference);
    bool isShown(Reference value, TypeID within) { return m_types.isSubset(typeShown(value), within); }
    TypeID typeThatStoreTakesOnWordOfWalk(const Draft& store);
    void checkProofOfResultOfCopy(Reference value, TypeID proven, Origin origin)
    {
        if (value && value.isValue() && (m_code.at(value.id()).opcode == Opcode::AwaitCallBySavingFrame || m_code.at(value.id()).opcode == Opcode::AwaitBySavingFrame))
            return;
        checkProofOfCopy(value, proven, ProofSource::ResultOfCall, origin);
    }
    void checkFactsRememberedOfCell(Reference object, Origin);
    void checkElementsRememberedOfList(Reference list, std::span<const Reference> elements, Origin);
    uint32_t numberOfProofsChecked(ProofSource source) const { return m_numbersOfProofsChecked[static_cast<unsigned>(source)]; }
    uint32_t numberOfProofsThatNoTestTells() const { return m_numberOfProofsThatNoTestTells; }
    uint32_t numberOfProofsThatFormTells() const { return m_numberOfProofsThatFormTells; }
    uint32_t numberOfDoorsThatCanNeverPass() const { return m_numberOfDoorsThatCanNeverPass; }
    TypeID typeThatFormSays(Reference value);
    uint32_t numberOfFreshCellsLeftAloneAtReturns() const { return m_numberOfFreshCellsLeftAloneAtReturns; }
    class ClosedArm;
    class OpenArm;
    class ProofsHoldEverywhere;

    struct FormAskedAhead {
        Opcode opcode { Opcode::Undefined };
        KnowledgeLevel level { KnowledgeLevel::Unknown };
        FunctionID function;
    };
    struct KeptForBuild {
        virtual ~KeptForBuild() = default;
    };
    std::unique_ptr<KeptForBuild>& keptByTypedAccesses() { return m_keptByTypedAccesses; }
    CoroutineStateOfUnit& coroutineState() { return m_coroutineState; }
    const CoroutineStateOfUnit& coroutineState() const { return m_coroutineState; }
    LoopsOverGeneratorsOfUnit& loopsOverGenerators() { return m_loopsOverGenerators; }
    struct EnvironmentCellsOfUnit {
        struct Name {
            const BodyIndex* body { nullptr };
            uint32_t copy { 0 };
            ValueID name;
            BlockID making;
            bool hasSeveralRecords { false };
        };
        bool hasMadeSome { false };
        bool mayNameSome { false };
        Vector<Name, 4> names;
    };
    EnvironmentCellsOfUnit& environmentCellsOfUnit() { return m_environmentCellsOfUnit; }
    std::unique_ptr<KeptForBuild>& keptByStrings() { return m_keptByStrings; }
    std::unique_ptr<KeptForBuild>& keptByPositionsInTexts() { return m_keptByPositionsInTexts; }
    std::unique_ptr<KeptForBuild>& looksKeptInFrame() { return m_frames.last().looksKept; }

    std::optional<uint32_t> innermostOpenLoop() const;
    bool standsInFrontOfOpenLoop(uint32_t indexOfLoop, Reference) const;
    Reference buildInFrontOfOpenLoop(uint32_t indexOfLoop, Draft&&, const Effects* foundByCaller = nullptr);
    std::optional<uint32_t> outermostLoopToMakeTextInFrontOf(std::span<const Reference> operands, const Effects& ofText) const;

    void noteFormAskedAhead(Opcode opcode, KnowledgeLevel level, FunctionID function = { }) { m_formAskedAhead = { opcode, level, function }; }
    FormAskedAhead takeFormAskedAhead() { return std::exchange(m_formAskedAhead, { }); }

    bool walkInline(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft* writtenOut = nullptr);
    void noteThatCopyMakesNewList(FunctionID callee, Reference receiver);
    bool takeWordThatCopyMakesNewList(Reference receiver);

    void leaveArrivals(FunctionID callee, std::span<const Reference> arguments);

    void fail(ASCIILiteral what);
    void report(const UserDiagnostic& diagnostic) { m_reports.append(diagnostic); }
    Vector<UserDiagnostic> takeReports() { return std::exchange(m_reports, { }); }
    bool hasFailed() const { return m_hasFailed; }
    ASCIILiteral failure() const { return m_failure; }
    ALWAYS_INLINE bool isUnreachable() const { return !m_block || m_code.at(m_block).hasEnded(); }

    TypeID resultTypeOf(const Draft&);
    bool standsUnderHandler() const { return isUnderHandler(); }
    void setResultOfCallBeingTyped(TypeID type) { m_resultOfCallBeingTyped = type; }
    TypeID exchangeResultOfReadBeingTyped(TypeID type) { return std::exchange(m_resultOfReadBeingTyped, type); }
    TypeID exchangeContentsBeingTyped(TypeID type) { return std::exchange(m_contentsBeingTyped, type); }
    const PositionTypes& positions() const { return *m_positions; }
    const PositionTypes& heldPositions() const { return *m_heldPositions; }
    TypeID withoutFunctionsThatMayNotBeMentioned(TypeID);
    const PositionTypes& positionsKnowingNothing() const { return *m_positionsKnowingNothing; }
    TypingContext typing() const { return { program(), m_types, *m_positions, TypeLevel::Proven }; }

    bool hasTextsInStringBuilders() const { return !m_textsInStringBuilders.isEmpty(); }
    bool edgeLeavesLoopBeingBuiltTwice(BlockID to) const { return leavesLoopBeingBuiltTwice(m_frames.size() - 1, to); }
    bool mayBeginTurnAgainInClosedCopy() const;
    bool looksCountOnTurnsThatBeginAgain(BlockID inputHead) const;
    bool layFrameForLookIntoCopy(FunctionID callee, const BodyIndex&);
    void takeAwayFrameForLookIntoCopy();
    bool standsInTurnThatLooksCountOn() const;
    bool looksAskOnlyWhetherTurnsRunOtherCode(BlockID inputHead) const { return m_inputHeadAskedAboutTypedCopy == inputHead; }
    bool looksTakeFormsBehindLooksAsBuilt() const { return m_looksTakeFormsBehindLooksAsBuilt; }
    void setLooksTakeFormsBehindLooksAsBuilt(bool take) { m_looksTakeFormsBehindLooksAsBuilt = take; }
    void beginTurnAgainInClosedCopy();
    bool standsInFrontOfTypedCopy(Reference) const;
    bool isBuiltInTurnThatMayBeginAgain(Reference) const;
    bool branchInFrontOfTypedCopy(Reference test, bool passesWithYes);
    class InFrontOfTypedCopy;
    bool hasURLsThatAreNeverMade() const { return m_hasURLsThatAreNeverMade; }
    void noteURLThatIsNeverMade() { m_hasURLsThatAreNeverMade = true; }
    Vector<TextInStringBuilder, 2>& textsInStringBuilders() { return m_textsInStringBuilders; }
    size_t endOfTextsInStringBuildersNamedBy(ValueID);
    Vector<uint32_t, 8> placesOfTextsInStringBuildersLeftOnEdge(BlockID from, BlockID to);
    size_t indexOfFrameOnTop() const { return m_frames.size() - 1; }
    BlockID inputBlockBeingWalked() const { return m_frames.last().block; }
    bool isLiveAtHeadOf(ValueID, BlockID inputBlock);
    void bindAnewAtEveryBlock(ValueID);
    bool isBoundAnewAtEveryBlock(ValueID value) const { return m_frames.last().isBuiltTwice(value); }

private:
    friend class ClosedArm;
    friend class InFrontOfTypedCopy;
    friend class OpenArm;
    friend class ProofsHoldEverywhere;
    friend class ArgumentsMadeWhereTheyLeave;
    friend class KnownThreading;

    static constexpr uint32_t noOpenEdge = UINT32_MAX;

    struct OpenEdge {
        EdgeID edge;
        BlockID block;
        uint32_t next { noOpenEdge };
        Vector<Reference, 4> arguments;
        Vector<TypeID, 4> types;
        WalkState state;
        NamesOfWay names;
        bool isLedByClosedArm { false };
    };

    static constexpr uint32_t noNarrowerName = UINT32_MAX;
    static constexpr uint32_t narrowerNamesOfDominator = UINT32_MAX - 1;

    struct TurnsOfForInWrittenOut {
        size_t place { 0 };
        ValueID step;
        Reference enumerator;
        Reference object;
        bool enumeratorIsAsked { false };
        Reference namesCountedByOpen;
        uint32_t numberOfNames { 0 };
        Vector<uint32_t, 4> edgesIntoLoopAsWritten;
    };

    struct InputBlock {
        uint32_t firstOpenEdge { noOpenEdge };
        uint32_t firstNarrowerName { narrowerNamesOfDominator };
        BlockID head;
        bool isEntered { false };
    };

    struct StateOfHandler {
        BlockID handler;
        WalkState state;
        WalkState::RecordsLastMet lastMet;
    };

    struct PendingBlock {
        BlockID block;
        Vector<ValueID, 4> parameters;
        WalkState state;
        uint32_t numberOfArrivals { 0 };
        bool hasState { false };
        bool parametersAreSet { false };
        bool isMadeByAddBlock { false };
    };

    class CountOfFrame {
        WTF_MAKE_NONCOPYABLE(CountOfFrame);
    public:
        CountOfFrame() = default;
        CountOfFrame(CountOfFrame&& other)
            : m_numberOfFrames(std::exchange(other.m_numberOfFrames, nullptr))
        {
        }
        CountOfFrame& operator=(CountOfFrame&& other)
        {
            std::swap(m_numberOfFrames, other.m_numberOfFrames);
            return *this;
        }
        ~CountOfFrame()
        {
            if (m_numberOfFrames)
                --*m_numberOfFrames;
        }

        explicit operator bool() const { return m_numberOfFrames; }
        void count(uint32_t& numberOfFrames)
        {
            ASSERT(!m_numberOfFrames);
            m_numberOfFrames = &numberOfFrames;
            ++numberOfFrames;
        }

    private:
        uint32_t* m_numberOfFrames { nullptr };
    };

    struct Frame {
        FunctionID function;
        const Body* body { nullptr };
        const BodyIndex* index { nullptr };
        std::span<const LinkedVariableStoredFirst> variablesStoredFirst;
        Vector<Reference> references;
        Vector<RememberedExpectation> expectations;
        UncheckedKeyHashMap<uint32_t, RememberedExpectation, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> wholeExpectations;
        uint32_t loopDepthAtCall { 0 };
        // Of a copy in line: Code::numberOfValues() when the frame was laid. What stands behind it was built for the copy.
        uint32_t numberOfValuesBuiltBefore { UINT32_MAX };
        TypeID typeExpectedOfAnswer;
        bool answerIsDeclared { false };
        ValueID valueHandedBack;
        bool hasLookedForValueHandedBack { false };
        uint32_t inlineFrame { 0 };
        bool returnsInPlaceOfUnit { false };
        bool callsAreCountedByPlan { false };
        struct ReturnOfCopy {
            BlockID block;
            Reference value;
            WalkState state;
            bool isUnderHandlerOfCallee { false };
            bool isLedByClosedArm { false };
        };
        Vector<ReturnOfCopy, 2>* returnsOfCopy { nullptr };
        ValueID callCopied;
        bool isCopiedByLinkOfChain { false };

        BlockID block;
        mutable BlockID returnDominator;
        uint32_t endOfValues { 0 };
        uint32_t walkedUpTo { 0 };
        uint32_t endOfStretch { 0 };
        uint32_t beginOfStretch { 0 };
        uint32_t numberOfStretchesBeingWalked { 0 };
        PlaceTakenByEndOfBlock placeTakenByEndOfBlockAskedLast;
        bool hasHandler { false };
        static constexpr uint32_t notAskedYet = UINT32_MAX;
        mutable uint32_t numberOfFramesUpToHandlerBelow { notAskedYet };
        mutable CountOfFrame countAmongFramesOfFunction;
        uint32_t firstOperationToWalk { 0 };
        bool hasPassedByWhatFeedsThrow { false };
        bool standsInFrontOfHead { false };
        struct NarrowerName {
            Reference value;
            Reference name;
            uint32_t next { noNarrowerName };
        };
        Vector<NarrowerName> narrowerNames;
        uint64_t narrowedValues { 0 };
        static constexpr uint64_t bitOfNarrowedValue(Reference value) { return 1ull << ((value.bits() * 0x9E3779B1u) >> 26); }
        uint32_t firstNarrowerName { noNarrowerName };
        static constexpr uint32_t lengthOfLongListOfNarrowerNames = 32;
        bool listOfNarrowerNamesIsLong { false };
        struct PlaceOfNarrowerName {
            uint32_t length { 1 };
            uint32_t jump { noNarrowerName };
        };
        Vector<PlaceOfNarrowerName> placesOfNarrowerNames;
        bool findsNarrowerNamesByReference { false };
        UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numbersOfNarrowedReferences;
        struct NarrowerNameInTables {
            uint32_t numberOfValue { 0 };
            uint32_t numberOfName { 0 };
            uint32_t nextOfValue { noNarrowerName };
            uint32_t nextUnderName { noNarrowerName };
        };
        mutable Vector<NarrowerNameInTables> narrowerNamesInTables;
        struct FirstNarrowerNames {
            uint32_t ofValue { noNarrowerName };
            uint32_t underName { noNarrowerName };
        };
        mutable Vector<FirstNarrowerNames> firstNarrowerNamesInTables;
        mutable uint32_t listInTables { noNarrowerName };

        uint32_t lengthOfNarrowerNames(uint32_t first) const { return first == noNarrowerName ? 0 : placesOfNarrowerNames[first].length; }
        void numberReferencesOfNarrowerName(uint32_t place);
        void bringTablesOfNarrowerNamesTo(uint32_t first) const;
        void addNarrowerName(uint32_t& first, Reference value, Reference name);
        enum class NarrowerNamesAsked : uint8_t { OfValue, UnderName };
        template<NarrowerNamesAsked, typename Functor> void forEachNarrowerName(Reference, const Functor&) const;
        Reference narrowestNameInLongList(Reference) const;
        LoopID loopLookedAt;
        uint32_t firstValueLookedAt { 0 };
        Vector<TypeID> typesOfLooks;
        std::unique_ptr<KeptForBuild> looksKept;
        struct ValuesBuiltTwice {
            WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValuesBuiltTwice);
            struct Slot {
                ValueID value;
                uint32_t ordinal { 0 };
                Reference onlyName;
                bool hasSeveralNames { false };
                void noteName(Reference name)
                {
                    hasSeveralNames |= onlyName && onlyName != name;
                    onlyName = name;
                }
            };
            struct NamesGivenForGood {
                BlockID block;
                Vector<ValueID, 4> values;
            };
            static constexpr uint32_t noSlot = UINT32_MAX;
            uint32_t slotOf(ValueID value) const { return slotNumbers[value.index()] - 1; }
            uint32_t ensureSlotOf(ValueID value)
            {
                uint32_t& slotNumber = slotNumbers[value.index()];
                if (!slotNumber) {
                    slots.append(Slot { value });
                    slotNumber = static_cast<uint32_t>(slots.size());
                }
                return slotNumber - 1;
            }
            bool contains(ValueID value) const
            {
                uint32_t slot = slotOf(value);
                return slot != noSlot && slots[slot].ordinal;
            }
            void remove(ValueID value) { slots[slotOf(value)].ordinal = 0; }
            Reference nameOn(const NamesOfWay& names, ValueID value) const
            {
                uint32_t slot = slotOf(value);
                return slot == noSlot ? Reference() : names.nameIn(slot);
            }
            void nameOtherwiseOn(NamesOfWay& names, ValueID value, Reference name)
            {
                uint32_t slot = slotOf(value);
                names.setNameIn(slot, name);
                slots[slot].hasSeveralNames = true;
                boundOnWay.append(value);
            }
            void noteBoundOnWay(ValueID value, Reference name)
            {
                uint32_t slot = slotOf(value);
                if (slot == noSlot)
                    return;
                boundOnWay.append(value);
                if (name)
                    slots[slot].noteName(name);
                else if (namesOfWay.nameIn(slot))
                    namesOfWay.setNameIn(slot, { });
            }

            Vector<Slot, 8> slots;
            Vector<uint32_t> slotNumbers;
            Vector<uint32_t, 8> slotsListedAnew;
            Vector<std::pair<uint32_t, uint32_t>, 8> slotsByLastUse;
            uint32_t numberOfListings { 0 };
            NamesOfWay namesOfWay;
            BlockID blockThatWayEnters;
            Vector<ValueID, 8> boundOnWay;
            Vector<NamesGivenForGood, 1> namesGivenForGood;
        };
        std::unique_ptr<ValuesBuiltTwice> valuesBuiltTwice;
        ValuesBuiltTwice& ensureValuesBuiltTwice()
        {
            if (!valuesBuiltTwice) {
                valuesBuiltTwice = makeUnique<ValuesBuiltTwice>();
                valuesBuiltTwice->slotNumbers.fill(0, references.size());
            }
            return *valuesBuiltTwice;
        }
        bool isBuiltTwice(ValueID value) const { return valuesBuiltTwice && valuesBuiltTwice->contains(value); }
        bool listAsBuiltTwice(ValueID value)
        {
            ValuesBuiltTwice& listed = ensureValuesBuiltTwice();
            uint32_t slot = listed.ensureSlotOf(value);
            if (listed.slots[slot].ordinal)
                return false;
            listed.slots[slot].ordinal = ++listed.numberOfListings;
            listed.slotsListedAnew.append(slot);
            if (Reference name = references[value.index()]) {
                listed.boundOnWay.append(value);
                listed.slots[slot].noteName(name);
            }
            return true;
        }
        Reference lastNameOf(ValueID value) const
        {
            Reference bound = references[value.index()];
            return bound || !isBuiltTwice(value) ? bound : valuesBuiltTwice->nameOn(valuesBuiltTwice->namesOfWay, value);
        }
        Vector<LoopID, 2> loopsBeingBuiltTwice;
        BlockID headOfTurnsWrittenOut;
        TurnsOfForInWrittenOut* turnsOfForIn { nullptr };
        Vector<ValueID, 4> valuesListedForTurnsWrittenOut;
        ValueID stepWhosePositionEdgesBring;
        size_t placeOfForInWhosePositionEdgesBring { 0 };
        Vector<BlockID, 2> blocksWalkedForEachWay;
        Vector<uint32_t> placesOfOpenLoops;
        std::optional<Vector<BlockID, 4>> handlerBlocks;
        Vector<InputBlock> blocks;
        std::array<Reference, 2> booleansOfUnit { };
        Vector<StateOfHandler, 1> statesOfHandlers;
        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesOfStatesOfHandlers;
        size_t placeOfStateOfHandler(BlockID handler) const;
        StateOfHandler* findStateOfHandler(BlockID handler);
        void addStateOfHandler(BlockID handler, WalkState&&);
        bool takeStateOfHandler(BlockID handler, WalkState&);
        Vector<std::unique_ptr<AccessUnderProvenKey>> accessesUnderProvenKeys;
    };

    class Frames {
    public:
        size_t size() const { return m_frames.size(); }
        size_t capacity() const { return m_frames.capacity(); }
        bool isEmpty() const { return m_frames.isEmpty(); }
        Frame& operator[](size_t index) LIFETIME_BOUND { return m_frames[index]; }
        const Frame& operator[](size_t index) const LIFETIME_BOUND { return m_frames[index]; }
        Frame& first() LIFETIME_BOUND { return m_frames.first(); }
        const Frame& first() const LIFETIME_BOUND { return m_frames.first(); }
        ALWAYS_INLINE Frame& last() LIFETIME_BOUND { return *m_last; }
        ALWAYS_INLINE const Frame& last() const LIFETIME_BOUND { return *m_last; }
        Frame* begin() LIFETIME_BOUND { return m_frames.begin(); }
        Frame* end() LIFETIME_BOUND { return m_frames.end(); }
        const Frame* begin() const LIFETIME_BOUND { return m_frames.begin(); }
        const Frame* end() const LIFETIME_BOUND { return m_frames.end(); }
        bool containsIf(NOESCAPE const Invocable<bool(const Frame&)> auto& matches) const { return m_frames.containsIf(matches); }

        void reserveCapacity(size_t capacity)
        {
            m_frames.reserveCapacity(capacity);
            findLast();
        }
        void append(Frame&& frame)
        {
            m_frames.append(WTF::move(frame));
            findLast();
        }
        void removeLast()
        {
            m_frames.removeLast();
            findLast();
        }
        void shrink(size_t size)
        {
            m_frames.shrink(size);
            findLast();
        }

    private:
        void findLast() { m_last = m_frames.isEmpty() ? nullptr : &m_frames.last(); }

        Vector<Frame, 4> m_frames;
        Frame* m_last { nullptr };
    };

    struct Binding {
        uint32_t frame { 0 };
        ValueID input;
        Reference previous;
    };

    void beginUnit(CodeUnit);
    bool walkBody();
    BodyValue accessAsWalked(const BodyValue& written, PropertyKeyID* chosenAndNotDerived = nullptr);
    BodyValue accessByName(const BodyValue& written, PropertyKeyID);
    enum class KeyOfAccess : uint8_t { IsAnyValue, IsSymbol, ArmsHaveWalkedAccess };
    KeyOfAccess compareKeyOfAccessWithSymbol(const BodyValue& written, PropertyKeyID chosen);
    void stopUnlessValueIsOf(Reference value, TypeID tested, ProofSource);
    ValueID m_accessWhoseKeyIsTested;
    size_t m_depthOfCopiesAroundThatAccess { 0 };
    void walkBlock(BlockID inputBlock);
    void walkOperation(const BodyValue&);
    bool walkThroughChain(const BodyValue&);
    template<Opcode known> bool walkThroughChainOf(const BodyValue& walked);
    void walkTerminator(const BodyTerminator&);
    void walkJump(const BodyTerminator&);
    void endWithSwitchOverOwnBlocks(TerminatorDraft&, std::span<const BlockID> targets, BlockID otherwise);
    void takeNarrowerNamesPast(std::span<const BlockID> blocksPassedBy);
    void enterJoin(BlockID inputBlock, InputBlock&, const BodyBlock&);
    void enterLoopHead(BlockID inputBlock);
    void enterHandlerBlock(BlockID inputBlock);

    void takeEntryParameters();
    void checkProofsOfWhatIsStoredBy(const Draft&);
    bool allocationHoldsWhatIsHeldFilled(const Draft& allocation);
    void showThatLiteralHoldsWhatIsHeldFilled(const BodyValue& finishOrData);
    void showThatMakersThatLeaveAtHoldWhatIsHeldFilled(const Vector<SummaryMakerThatEndsWhereItLeaves>&, uint32_t position, bool standsBehindOperationsOfBlock);
    void stopAtMakerUnless(Reference holds, Origin);
    void checkProofsOfOperandsOfDirectCall(const Draft&);
    void checkProofsOfWhatComesThroughFusedEntry();
    void checkProofThatSlotIsFilled(const Draft& read);
    bool checkWhatIsRememberedOf(const BodyValue& read);
    void checkProofOfRecord(ValueID record, const Draft&, TypeID proven);
    std::optional<Origin> leaveCheckOfWhatContainerHasJustGivenToTestBehind(Reference given);
    Reference holdResultToItsType(Reference value, Origin);
    void holdOperandsToWhatCalleeBelieves(const Draft&);
    void checkThatReadGivesValue(Reference value, ProofSource, Origin);
    void beginModuleInBody();
    ALWAYS_INLINE void standIn(BlockID inputBlock)
    {
        if (m_frames.last().block != inputBlock)
            standInOtherBlock(inputBlock);
    }
    void standInOtherBlock(BlockID inputBlock);
    void standIn(Frame&, const BodyBlock&, InputBlock&);
    void noteStateAtHeadForHandler();
    void noteStateAtThrowForHandler(std::span<const Reference> thrown = { });

    ALWAYS_INLINE InputBlock& inputBlock(Frame& frame, BlockID block)
    {
        if (frame.blocks.isEmpty()) [[unlikely]]
            frame.blocks.fill(InputBlock { }, frame.index->numberOfBlocks());
        return frame.blocks[block.index()];
    }
    ALWAYS_INLINE InputBlock& inputBlock(size_t frameIndex, BlockID block) { return inputBlock(m_frames[frameIndex], block); }
    uint32_t addOpenEdge(size_t frameIndex, BlockID target);
    uint32_t addOpenEdge(InputBlock& target);
    Vector<uint32_t, 4> takeOpenEdges(BlockID inputBlock);
    void releaseOpenEdge(uint32_t);
    BlockID addBlockFor(BlockID inputBlock, BlockRole = BlockRole::Ordinary, BlockFrequency = BlockFrequency::Normal);
    void closeEdgesInto(BlockID, BlockID inputBlock, std::span<const uint32_t> edges, bool isHandler);
    void closeEdgeWithArguments(EdgeID, BlockID to, std::span<const Reference> arguments);
    void closeBackEdge(EdgeID, BlockID head, std::span<const Reference> arguments);
    bool walkUnitAgainWithWiderHead(BlockID inputHead, uint32_t parameter, TypeID previewed, TypeID found);
    bool walkUnitAgainWithWiderHeadOfCode(BlockID head, size_t parameter, TypeID previewed, TypeID found);
    bool walkUnitAgainWithLoopsThatMayDoAnything();
    bool looksAtLoopsAroundHaveCounted(const Draft&, const Effects&);
    Opcode m_opcodeBeingWalked { Opcode::Undefined };
    ValueID m_valueBeingWalked;
    bool m_isMakingObjectsOfConstant { false };
    void giveUpWalkOfUnit();
    struct LoopOfWalkGivenUp {
        FunctionID function;
        BlockID inputHead;
        bool mayDoAnything { false };
        Vector<TypeID, 8> leastTypes;
        Vector<uint8_t, 8> numbersOfWalksGivenUp;
    };
    LoopOfWalkGivenUp& loopOfWalkGivenUp(FunctionID, BlockID inputHead);
    Vector<LoopOfWalkGivenUp, 2> m_loopsOfWalksGivenUp;
    bool m_walksUnitAgain { false };
    void endWithJumpInto(BlockID to, std::span<const Reference> arguments);

    struct OpenLoop {
        BlockID blockInFront;
        BlockID head;
        Vector<ValueID, 8> entering;
        Vector<BlockID, 2> earlierBlocksInFront;
        Vector<ValueID, 8> builtInFront;
        Vector<ValueID> tableOfBuiltInFront;
        Effects effects;
        mutable BlockID backEdgeDominator;
        uint32_t frame { 0 };
        LoopID loop;
        uint32_t firstRecord { 0 };
        Origin origin { };
        ValueID poll;
        ValueID pollInFront;
        uint32_t numberOfEdgesThatCameRound { 0 };
        bool likelyTurnMayCollect { false };
        Vector<ValueID, 2> pollsInFrontOfLoopsInside;
    };
    void openLoop(OpenLoop&&);
    void closeNewestLoop();
    bool leaveBlockAnotherWay(const BodyTerminator&);
    std::optional<uint32_t> onlyTargetWhereAllComeToOnePlace(const BodyTerminator&) const;
    bool branchOnTestBelowNegations(const BodyTerminator&);
    std::optional<uint32_t> m_edgeOfCodeBeingLed;
    bool handlerInForceIgnoresWhatIsThrown() const;
    bool jumpToHandlerInPlaceOfThrow(const BodyTerminator&);
    void walkOperationsOf(BlockID inputBlock);
    bool enterBehindHeadOfHandlerBlock(BlockID inputBlock, std::span<const uint32_t> edges);
    void noteRecordBuiltForPolls(ValueID, const Draft&, const Effects&);
    void buildPollInFrontOfLoop(BlockID inputHead, const LoopPreview&);
    void settleFateOfPoll(const OpenLoop&);
    void noteEdgeCameRound(BlockID head);
    ValueID m_pollInFrontOfLoopBeingEntered;
    template<typename Functor> void forEachOpenLoopAround(uint32_t numberOfLoops, size_t firstFrame, const Functor&) const;
    uint32_t numberOfOpenLoopsAround(size_t frameIndex, BlockID inputBlock) const;
    void leaveBlockOpenInFrontOfLoop(BlockID inputHead, BlockID head, std::span<const Reference> entering, const Effects& effectsOfLoop);
    void endBlocksInFrontOfLoops(size_t frameIndex, BlockID next);
    bool standsInFrontOfLoop(uint32_t indexOfLoop, Reference) const;
    std::optional<uint32_t> findLoopToBuildInFrontOf(const Draft&, const Effects&) const;
    struct HoistPoint {
        size_t frameIndex;
        BlockID inputBlock;
        BlockID block;
    };
    HoistPoint currentHoistPoint() const;
    HoistPoint hoistPointInFrontOf(uint32_t loopIndex) const;
    bool executesOnEveryIteration(uint32_t loopIndex, const HoistPoint&) const;
    ValueID findSameInFrontOfLoop(uint32_t indexOfLoop, const Draft&) const;
    Reference appendConversionAmongOpenLoops(Opcode, Representation to, Reference operand, TypeID proven, Origin);
    void noteBuiltInFrontOfLoop(uint32_t indexOfLoop, ValueID);
    void endBlocksInFrontOfLoopsFrom(size_t numberOfLoopsKept);

    bool buildLoopTwice(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
    bool buildLoopForEachMember(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
    bool mayBuildLoopSeveralTimes(BlockID inputHead);
    std::optional<Vector<ValueID, 8>> findValuesOfLoopReadBehindIt(BlockID inputHead);
    void forgetValuesOfLoop(BlockID inputHead);
    struct ClosedCopyShared {
        size_t frameIndex { 0 };
        BlockID inputHead;
        BlockID start;
        BlockID entry;
        bool isAboutToBeTaken { false };
    };
    Vector<ClosedCopyShared, 2> m_closedCopiesShared;
    bool writeOutTurnsOfLoop(BlockID inputHead, std::span<const Reference> entering);
    bool writeOutTurnsOfForIn(BlockID inputHead, std::span<const Reference> entering);
    bool writeOutTurnsOfForOf(BlockID inputHead, std::span<const Reference> entering);
    void beginTurnsWrittenOut(BlockID inputHead);
    void walkTurnsWrittenOut(BlockID inputHead, std::span<const Reference> entering, uint32_t numberOfWalks, TurnsOfForInWrittenOut*);
    void leadIntoLoopAsWrittenBehindTurns(TurnsOfForInWrittenOut&, BlockID inputHead, std::span<const Reference> arriving, Reference position);
    void standAtHeadOfTurnOfForIn(BlockID inputHead);
    std::span<const BlockID> handlerBlocksOfFrameOnTop();
    struct WayIntoClosedCopy {
        size_t frameIndex { 0 };
        BlockID inputHead;
        BlockID entry;
        Vector<Reference, 8> heldAtHead { };
        BlockID startFromInFront { };
        Vector<GuessCountedOnByLooks, 4> guessesCountedOn { };
        bool looksCountOnTurnsThatBeginAgain { false };
        bool isEntered { false };
        uint32_t numberOfTurn { 0 };
    };
    Vector<WayIntoClosedCopy, 2> m_waysIntoClosedCopies;
    uint32_t m_numberOfTypedCopiesEntered { 0 };
    BlockID m_inputHeadAskedAboutTypedCopy;
    std::unique_ptr<LoopPreview> m_previewOfTypedCopyAboutToBeEntered;
    Vector<ValueID, 16>* m_expectationsRememberedForLook { nullptr };
    const ScopedLambda<TypeID(ValueID)>* m_typesOfInputsInLook { nullptr };
    mutable bool m_lookHasAskedAboutUnbuiltValue { false };
    bool m_looksTakeFormsBehindLooksAsBuilt { false };
    uint32_t m_numberOfFramesLaidForLooks { 0 };
    uint32_t m_operationsCopiedAtWrittenCallInWalk { 0 };
    void noteHeadOfTurn(BlockID inputHead);
    void proveWhatLooksCountOnAt(const BodyValue&);
    std::unique_ptr<LoopPreview> previewTypedCopy(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, std::span<const ListTestedInFrontOfLoop> lists, std::unique_ptr<LoopPreview> madeWithListsNoted);
    bool turnsOfTypedCopyMayBeginAgain(BlockID inputHead, const LoopPreview&) const;
    std::optional<uint32_t> openLoopOfTypedCopy() const;
    ValueID findReadAskedAtHeadOfLoop(BlockID inputHead, std::span<const std::pair<Reference, TypeID>> tested);
    bool askObjectAtHeadOfTypedCopy(const BodyValue& read);
    struct IterationsSteppedAtHead {
        Vector<std::pair<uint32_t, IterationByIndex>, 2> asTheyEntered;
    };
    bool iterationsSteppedAtMayBeBuiltTwice(BlockID inputHead) const;
    bool iterationsSteppedAtHandOverPositionsAlone(BlockID inputHead) const;
    IterationsSteppedAtHead takeIterationsSteppedAt(BlockID inputHead) const;
    void putBackIterationsSteppedAt(const IterationsSteppedAtHead&);
    void walkCopyOfLoop(BlockID inputHead, std::span<const Reference> entering);
    bool leavesLoopBeingBuiltTwice(size_t frameIndex, BlockID to) const;
    void takeNamesOntoEdge(OpenEdge&, size_t frameIndex);
    void takeNamesOfWay(BlockID inputBlock, NamesOfWay&&);
    void forgetNamesOfWay(Frame&);
    bool isNamedByWaysInto(const Frame&, ValueID, BlockID inputBlock) const;
    Reference nameBroughtByWay(const Frame&, ValueID) const;
    ALWAYS_INLINE Reference referenceBoundIn(const Frame& frame, ValueID input) const
    {
        Reference bound = frame.references[input.index()];
        if (!bound && frame.valuesBuiltTwice) [[unlikely]]
            return nameBroughtByWay(frame, input);
        return bound;
    }
    bool isWithoutNameBehindLoopBuiltTwice(ValueID);
    bool isWalkingBlockForEachWay(size_t frameIndex) const;
    bool walkBlockApartFromClosedWays(BlockID inputBlock);
    bool letReturnsOfCopyWalkEndOfBlockApart(ValueID call, std::span<Frame::ReturnOfCopy* const> passing, std::span<Frame::ReturnOfCopy* const> closed);
    void walkEndOfBlockBehindReturnsOfCopy(ValueID call);
    bool goOnBehindReturnsOfCopy(const BodyValue& call, const Function&, const LinkedFunction&, const Vector<Reference, 2>& heldByCallAlone, Origin originOfCall, Vector<Frame::ReturnOfCopy, 2>& returns);
    bool walkBlockForEachWayThatBringsObjectInSight(BlockID inputBlock);
    bool inlineCalleeAsLinkOfChain(const BodyValue& call);
    bool inlineCalleeAsLinkOfChainOfItsOpcodes(const BodyValue& call);
    bool isCopiedByLinkOfChain(const BodyValue& call) const { return m_callCopiedByLinkOfChain == call.id() && m_numberOfFramesAtCallCopiedByLinkOfChain == m_frames.size(); }
    ValueID m_callCopiedByLinkOfChain;
    size_t m_numberOfFramesAtCallCopiedByLinkOfChain { 0 };
    bool looksAtLoopsAroundHaveSeen(const Effects&) const;
    void leadEdge(EdgeID, BlockID from, const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    void leadHandlerEdge();
    ALWAYS_INLINE bool isUnderHandler() const
    {
        if (m_block && m_code.at(m_block).handler)
            return true;
        return numberOfFramesUpToHandlerInForce();
    }
    ALWAYS_INLINE size_t numberOfFramesUpToHandlerInForce() const
    {
        size_t numberOfFrames = m_frames.size();
        if (!numberOfFrames)
            return 0;
        const Frame& frame = m_frames.last();
        if (frame.hasHandler)
            return numberOfFrames;
        uint32_t below = frame.numberOfFramesUpToHandlerBelow;
        return below != Frame::notAskedYet ? below : findFramesUpToHandlersBelow();
    }
    size_t findFramesUpToHandlersBelow() const;
    std::optional<size_t> findTextHeldInFrame(size_t frameIndex, ValueID) const;
    bool leadHandlerEdgeBehindStringsOfTexts(size_t frameIndex, BodyTarget, Vector<Reference, 4>& arguments, Vector<TypeID, 4>& types);
    void bindTextsAtHeadOfHandlerBlock(BlockID inputBlock);
    Reference narrowOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&);
    Reference booleanAskedByBranchOnEdge(BlockID from, uint32_t targetIndex, Reference value);
    void nameBooleansAskedByBranch(BlockID from, const BodyTerminator&, uint32_t targetIndex, BlockID to);
    std::optional<Draft> draftOfNarrowedOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&, Reference holderOfSubject = { });
    bool narrowHolderOfTestedFieldOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&, BlockID to);
    struct MembersByTag {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(MembersByTag);
        UncheckedKeyHashMap<uint64_t, Vector<LayoutID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> members;
    };
    const MembersByTag* findMembersByTag(LayoutSetID, const Operation& read);
    void shareMembersByTag(LayoutSetID whole, LayoutSetID part, PropertyKeyID);
    Vector<LayoutID, 4> membersThatHoldTag(const MembersByTag&, LayoutSetID, uint64_t tag);
    std::optional<TerminatorDraft> draftOfSwitchOverShapes(const TerminatorDraft& overTags, Vector<uint32_t, 16>& casesLeft);
    bool endBlockWithSwitchOverShapes(const BodyTerminator&);
    void askShapeInPlaceOfTagAtBranch(const BodyTerminator&);
    bool isNameOfCellAskedForShape(Reference name, ValueID asked) const;
    std::optional<Draft> draftOfHolderNarrowedByShape(BlockID from, uint32_t targetIndex, Reference holder, Reference test);
    Vector<std::unique_ptr<MembersByTag>> m_listsOfMembersByTag;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_membersByTagOfSets;
    bool isReadOfSlotOffHolder(Reference read, Reference holder, PropertyKeyID) const;
public:
    Reference buildTestOfTagAsTestOfShape(const BodyValue& comparison);
    bool isAskedByBranchOfItsBlock(const BodyValue& comparison) const;
    Reference tagReadInFrontOfTest(const BodyValue& read);
private:
    void noteNarrowerName(BlockID to, Reference value, Reference name);
    bool endBlockInAnotherWay(const BodyTerminator&);
    bool endBlockWithSwitchOverAlternatives(const BodyTerminator&);
    void endWithoutEdges(TerminatorDraft&);

    void walkReturn(const BodyTerminator&);
    void noteReturnOfCopy(const BodyTerminator&);
    void returnFromUnit(Reference returned);
    void endLivesOfCellsThatHaveNotLeft(std::span<const Reference> handedBack, Origin, uint32_t numberOfValuesBuiltBefore);
    bool staysTailCall(Reference call) const;
    void noteDirectCallForStackTests(const Draft&);
    bool callsOfCopyAreCountedForStackTests(const BodyValue& call, FunctionID callee) const;
    bool throwsPastHandlerOf(BodyBlock) const;

    PendingBlock* findPendingBlock(BlockID);
    PendingBlock& pendingBlock(BlockID);
    void settleParametersOf(BlockID);
    void arriveAt(BlockID, WalkState&&);

    ALWAYS_INLINE OperandTypes typesOfOperands(const DraftWords& words)
    {
        std::span<const Reference> operands = words.operands();
        TypeID* types = m_operandTypes.resizeWithoutWords(static_cast<uint32_t>(operands.size()));
        for (Reference operand : operands)
            *types++ = typeAsBuilt(operand);
        return { m_operandTypes.span() };
    }
    TypeID resultTypeOf(const Draft&, bool& listsOwnTypes);
    TypeID resultTypeByStepsOf(const TypingContext&, const Draft&, uint32_t steps, bool& listsOwnTypes);
    TypeID typeOfNarrowedDraft(const Draft&);
    static std::array<uint32_t, numberOfOpcodes> findStepsOfBuildByOpcode();
    ALWAYS_INLINE uint32_t stepsOfBuildOf(Opcode opcode) const { return m_stepsOfBuildByOpcode[static_cast<unsigned>(opcode)]; }
    ALWAYS_INLINE TypeID ownTypeOf(Reference reference) const { return reference && reference.isRecord() ? m_code.at(reference.id()).type : TypeID(); }
    ALWAYS_INLINE TypeID typeAsBuilt(Reference reference) const
    {
        if (!reference || !reference.isRecord())
            return { };
        if (reference.isValue()) {
            if (TypeID shape = m_state.shapeOf(reference))
                return shape;
        }
        return m_code.at(reference.id()).type;
    }
    TypeID typeOfTextOrRecord(Reference) const;
    Reference narrowestReferenceOf(ValueID input) const;
    bool checkOperandKinds(const TerminatorDraft&);
    bool operandsAreAsTaken(const TerminatorDraft&) const;
    static bool isDeclaredPure(TerminatorKind);
    template<typename DraftType> void askTruthOfOperandsNotProvenBoolean(DraftType&);
    TypeID typeOfResultAskedFor(const Draft& resultAt);
    TypeID typeOfNewTargetBehind(Reference prototype) const;
    TypeID joinTypes(TypeID, TypeID);
    Representation boxedFormOf(TypeID);
    Reference referenceTo(ValueID id) const { return m_code.isConstant(id) ? Reference::constant(id) : Reference::value(id); }
    Reference convert(Reference, Representation wanted, Origin);
    Reference tryBuildConversionOnStraightRoad(Opcode, Representation to, Reference, Origin);
    Reference readVariableWhereItIsTaken(Reference constant, Origin);
    Reference convertWithoutCode(Reference, Representation wanted);
    template<typename DraftType> void convertOperands(DraftType&);
    void letLeave(WalkState&, Reference) const;
    void addSharedConstant(unsigned hashOfWords, uint32_t place);
    ValueID findCopyOfConstant(ValueID, Representation) const;
    void noteCopyOfConstant(ValueID, Representation, ValueID copy);

    const WalkInputs& m_inputs;
    TypeArena& m_types;
    Arrivals& m_arrivals;
    const Mechanisms m_mechanismsOfBuild;
    Mechanisms m_mechanisms;
    const std::array<uint32_t, numberOfOpcodes> m_stepsOfBuildByOpcode = findStepsOfBuildByOpcode();
    Code m_code;
    NumbersOfWalkStates m_numbersOfStates;
    AnswersOfWalk m_answersOfStates;
    WalkState m_state;

    BlockID m_block;
    mutable Vector<uint32_t> m_numbersOfFramesOfFunctions;
    Frames m_frames;
    FunctionID m_copyThatMakesNewList;
    ValueID m_receiverOfCopyThatMakesNewList;
    size_t m_depthOfCopyThatMakesNewList { 0 };
    bool m_opensGuesses { true };
    PrintStream* const m_explainsClosedAccessesTo;
    uint32_t m_numberOfGuessesOpened { 0 };
    uint32_t m_numberOfLoopsBuiltTwice { 0 };
    uint32_t m_numberOfAssignmentsWrittenOut { 0 };
    uint32_t m_numberOfJoinsWalkedForEachWay { 0 };
    uint32_t m_numberOfTurnsThatBeginAgain { 0 };
    UncheckedKeyHashMap<uint32_t, Vector<FieldIndex, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fillersOfAllocations;
    uint32_t m_numberOfSlicesMadeOfElementsInSight { 0 };
    Vector<CountOfUnit, 4> m_countsOfUnit;
    uint32_t m_numberOfValuesWalked { 0 };
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_roundsKept;
    Vector<Vector<PropertyKeyID>> m_listsOfNames;
    HashMap<uint64_t, Vector<uint32_t, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_numbersOfListsOfNames;
    std::array<uint32_t, numberOfEnumerators<ProofSource>> m_numbersOfProofsChecked { };
    uint32_t m_numberOfProofsThatNoTestTells { 0 };
    uint32_t m_numberOfProofsThatFormTells { 0 };
    uint32_t m_numberOfDoorsThatCanNeverPass { 0 };
    uint32_t m_numberOfFreshCellsLeftAloneAtReturns { 0 };
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsInReachOfUnit;
    SurveysOfConstants m_surveysOfConstants;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constantsThatListFieldsHeldFilled;
    bool m_callIsAccountedFor { false };
    FunctionID m_calleeAccountedFor;
    bool m_hasFailed { false };

    std::unique_ptr<PositionTypes> m_positionsOfBuild;
    std::unique_ptr<PositionTypes> m_positionsOfSlowBody;
    const PositionTypes* m_positions { nullptr };
    std::unique_ptr<PositionTypes> m_positionsKnowingNothing;
    std::unique_ptr<PositionTypes> m_heldPositions;
    TypeID m_resultOfCallBeingTyped;
    TypeID m_resultOfReadBeingTyped;
    TypeID m_contentsBeingTyped;
    std::unique_ptr<PositionTypes> m_positionsWithoutProofs;
    void forEachValueThatTypeShownLeansOn(ValueID, NOESCAPE const Invocable<void(ValueID)> auto&) const;
    TypeID typeShownAtOnce(ValueID) const;
    bool entryParameterIsShown(const Block&, ValueID) const;
    TypeID computeTypeShown(ValueID);
    HashMap<uint32_t, TypeID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typesShown;
    HashMap<uint32_t, TypeID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typesAsked;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_headsOfClosedLoops;
    ASCIILiteral m_failure;
    Vector<UserDiagnostic> m_reports;
    Origin m_origin { };
    Vector<OpenEdge> m_openEdges;
    uint32_t m_firstFreeOpenEdge { noOpenEdge };
    Vector<PendingBlock> m_pendingBlocks;
    Vector<Binding> m_bindings;
    uint32_t m_numberOfClosedArms { 0 };
    ShortList<TypeID, 16> m_operandTypes;
    Vector<uint32_t, 16> m_sharedConstants;
    bool m_holdsReadsOfVariablesAsConstants { false };
    uint32_t m_numberOfSharedConstants { 0 };
    Vector<std::pair<TypeID, Reference>, 2> m_wordsThatSayNothing;
    struct NarrowedTypedLast {
        Reference value;
        Reference test;
        BlockID block;
        TypeID typeOfValue;
        TypeID type;
        uint8_t flags { 0 };
        uint8_t numberOfPayloadWords { 0 };
        std::array<uint32_t, 4> payload { };
    };
    NarrowedTypedLast m_narrowedTypedLast;
    struct CopyOfConstant {
        uint32_t constantAndForm { UINT32_MAX };
        ValueID copy;
    };
    Vector<CopyOfConstant, 16> m_copiesOfConstants;
    uint32_t m_numberOfCopiesOfConstants { 0 };
    Vector<IterationByIndex, 4> m_iterationsByIndex;
    HashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_iterationsOfHeads;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_inputHeadsOfIterations;
    static uint64_t inputHeadInFrame(uint32_t inlineFrame, BlockID inputHead) { return static_cast<uint64_t>(inlineFrame) << 32 | inputHead.index(); }
    uint32_t m_numberOfIterationsInFrontOfTheirLoops { 0 };
    std::optional<bool> m_listsAreIteratedByIndex;
    bool m_isLeadingHandlerEdge { false };
    Vector<OpenLoop, 4> m_openLoops;
    Vector<uint32_t> m_placesOfLoopsBehindBlocks;
    uint32_t m_numberOfLoopsThatMayBeLeft { UINT32_MAX };
    uint32_t m_numberOfLoopsAroundEdgeBeingLed { UINT32_MAX };
    uint32_t m_numberOfScopesWhereProofsHoldEverywhere { 0 };
    FormAskedAhead m_formAskedAhead;
    std::unique_ptr<KeptForBuild> m_keptByTypedAccesses;
    CoroutineStateOfUnit m_coroutineState;
    LoopsOverGeneratorsOfUnit m_loopsOverGenerators;
    EnvironmentCellsOfUnit m_environmentCellsOfUnit;
    std::unique_ptr<KeptForBuild> m_keptByStrings;
    std::unique_ptr<KeptForBuild> m_keptByPositionsInTexts;
    mutable BodyLiveness m_liveness;
    uint32_t m_operationsCopiedAtWrittenCall { 0 };
    void noteClosuresThatAllocationAloneTakes(const BodyValue& allocation);
    Vector<Reference, 8> m_closuresThatAllocationAloneTakes;
    uint32_t m_numberOfCopiesBuiltForSize { 0 };
    bool m_isWalkingAccess { false };
    Vector<TextInStringBuilder, 2> m_textsInStringBuilders;
    const PlacesOfTextsInStringBuilders& placesOfTextsInStringBuilders();
    PlacesOfTextsInStringBuilders m_placesOfTextsInStringBuilders;
    bool m_hasURLsThatAreNeverMade { false };
};

class Walker::ClosedArm {
    WTF_MAKE_NONCOPYABLE(ClosedArm);
public:
    explicit ClosedArm(Walker&, bool beginsTurnAgainWhereItMay = false);
    ~ClosedArm();

private:
    Walker& m_walker;
    size_t m_firstBinding;
    bool m_openedGuesses;
};

class Walker::InFrontOfTypedCopy {
    WTF_MAKE_NONCOPYABLE(InFrontOfTypedCopy);
public:
    InFrontOfTypedCopy(Walker&, bool isInFront);
    ~InFrontOfTypedCopy();

private:
    Walker& m_walker;
    bool m_isInFront;
    BlockID m_blockInside;
    uint32_t m_numberOfLoopsThatMightBeLeft { 0 };
};

class Walker::OpenArm {
    WTF_MAKE_NONCOPYABLE(OpenArm);
public:
    explicit OpenArm(Walker& walker)
        : m_walker(walker)
        , m_openedGuesses(std::exchange(walker.m_opensGuesses, true))
    {
    }
    ~OpenArm() { m_walker.m_opensGuesses = m_openedGuesses; }

private:
    Walker& m_walker;
    bool m_openedGuesses;
};

class Walker::ProofsHoldEverywhere {
    WTF_MAKE_NONCOPYABLE(ProofsHoldEverywhere);
public:
    explicit ProofsHoldEverywhere(Walker&);
    ~ProofsHoldEverywhere();

private:
    Walker& m_walker;
};

struct LoopPreview {
    Effects effects;
    Vector<GuessCountedOnByLooks, 4> guessesCountedOn;
    ValueID firstOperationThatRunsOtherCode;
    bool otherCodeRunsInLoopInside { false };
    Vector<TypeID, 8> typesOfParameters;
    Vector<bool, 8> parametersSteppedOnDoubles;
    bool hasLetMentionsLeave { false };
    Vector<TypeID> typesOfValues;
    uint32_t firstValue { 0 };
    bool typesOfValuesLieInFrame { false };
};
void noteTypesOfLooksLieInFrame(Walker&);
std::optional<std::array<BodyEdgeResult, 2>> boundsProvenOnEdge(const BodyIndex&, const BodyTerminator&, uint32_t targetIndex);
ValueID relationThatNarrowsBehind(const Walker&, const BodyTerminator&);
LoopPreview previewLoop(Walker&, BlockID inputHead, std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround = { });
TypeID widestTypeOfOperation(Walker&, const BodyValue&);

enum class TypedForm : uint8_t {
    None,
    Built,
    BuiltAsCall,
    BuiltBehindFilledSlot,
    BuiltBehindEmptySlot,
    BuiltBehindLookAtCell,
    BuiltBehindFilledSlotAndLookAtCell,
    BuiltOnObjectAsMade,
};
using TypesOfInputs = ScopedLambda<TypeID(ValueID)>;
TypedForm typedFormOf(Walker&, const BodyValue&, const TypesOfInputs&);
std::optional<Effects> effectsOfTypedFormAhead(Walker&, const BodyValue&, const TypesOfInputs&, FunctionID* halfRunBehindLooks = nullptr);
bool maySuspendAsWritten(const BodyValue&);

class InputsOfTypedOperation {
public:
    InputsOfTypedOperation(const Walker& walker, const TypesOfInputs* typesAskedAbout)
        : m_walker(walker)
        , m_typesAskedAbout(typesAskedAbout)
    {
    }

    bool areOnlyTypes() const { return m_typesAskedAbout; }
    ALWAYS_INLINE TypeID typeOf(ValueID input) const { return m_typesAskedAbout ? (*m_typesAskedAbout)(input) : m_walker.typeOf(m_walker.referenceOf(input)); }

private:
    const Walker& m_walker;
    const TypesOfInputs* m_typesAskedAbout;
};

enum class MembersBehindWatch : bool { AreOpen, AreAsBuilt };
BuiltinID findConstantMemberFrom(const Program&, const Facts&, BuiltinID start, PropertyKeyID, MembersBehindWatch = MembersBehindWatch::AreOpen);
void noteObjectPassedTestOfBeingAsMade(Walker&, Reference object, Reference test, uint32_t numberOfValuesInFrontOfTest);
BuiltinID findConstantMemberOnceForBuild(const Walker&, BuiltinID start, PropertyKeyID);
BuiltinID findConstantMemberReadOff(const Walker&, TypeID object, PropertyKeyID);
BuiltinID findConstantMemberReadOff(Walker&, Reference object, PropertyKeyID);
TypedForm buildReadOfBuiltinMember(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object);
bool readsOffEntryParameterThatStaysAsMade(Walker&, const BodyValue& access);
bool viewMayFollowSharedMemoryThatGrows(const Walker&, Reference view);
bool receiverStaysAsMadeByLink(Walker&, const BodyValue& operation, bool* isSaidByBodyAlone = nullptr);
bool linksWordCanBeHeldAgainst(const Walker&, Reference object);
void holdLinksWordAgainstObjectAsMade(Walker&, Reference object, Origin, bool isSaidByBodyAlone = false);
bool readsOffListThatStaysAsMadeByLink(Walker&, const BodyValue& read);
BuiltinClassID classOfIterableThatStaysAsMadeByLink(Walker&, const BodyValue& operation, ValueID iterable);
bool indexesAreAbsentAboveLists(const Walker&);
Reference buildReadOfElementOffExpectedList(Walker&, const BodyValue& read);
TypeID typeOfElementInsideViewOfNumbers(Walker&, TypeID typeOfView, TypeID typeOfPosition);
Reference buildReadOfElementOfViewBehindTestOfLength(Walker&, const BodyValue& read, Reference view, Origin, BlockID whereItLiesOutside);
bool splitNullishOffReceiverOfAccess(Walker&, const BodyValue& access);
void buildFirstReadOfNamedBuiltin(Walker&, const BodyValue& named);
bool firstReadOfNamedBuiltinIsSeen(const Walker&, const BodyValue& named);
bool buildInstanceOfBuiltin(Walker&, const BodyValue& test, const InputsOfTypedOperation&);
Reference buildTestOfObjectAsMade(Walker&, Reference object, Origin);
TypeID typeFoundOnObjectAsMade(Walker&, const BodyValue& read, TypeID typeOfObject);
TypeID typeOfMethodFoundBehindLooks(Walker&, const BodyValue& read, TypeID typeOfObject);
bool readOffObjectAsMadeCallsGetter(Walker&, const BodyValue& read, TypeID typeOfObject);
enum class BuiltinCallAskedAbout : uint8_t {
    None,
    OnAnyOperands,
    ByOneRow,
};
BuiltinCallAskedAbout wouldCallBuiltin(Walker&, const BodyValue& call, const TypesOfInputs&, bool* standsInLine = nullptr);
std::optional<Effects> effectsOfCallByOneRowAhead(Walker&, const BodyValue& call, const TypesOfInputs&);

bool testOfBodyHoldsPositionBelowLengthOfText(Walker&, const BodyIndex&, BlockID block, ValueID text, ValueID position);
bool testOfBodyHoldsPositionOfCharacterReadBelowLength(Walker&, const BodyValue& read);
TypeID typeOfReadInsideTextAhead(Walker&, const BodyIndex&, const BodyValue& call, const TypesOfInputs&);
const Walker::IterationByIndex* findListSteppedByIndexAlone(Walker&, const BodyIndex&, const BodyValue& step);
TypeID typeOfStepOverListAhead(Walker&, const BodyIndex&, const BodyValue& step, bool& countsOnRowInFront);
std::optional<Effects> effectsOfStepOverListAhead(Walker&, const BodyIndex&, const BodyValue& step, bool& countsOnRowInFront);
TypeID typeOfStepOverTextAhead(Walker&, const BodyIndex&, const BodyValue& step, const TypesOfInputs&);
bool cellAnswersWhetherValueIsArray(const Walker&, TypeID asked);
bool isLookAtCellInFrontOfIsArray(const Walker&, const BodyIndex&, const BodyValue& test);
TypeID typeOfIsArrayAhead(Walker&, const BodyIndex&, const BodyValue& call, const TypesOfInputs&);
bool iterationGoesByIndexWithoutTest(Walker&, TypeID iterated);

Reference buildRegExpOfKnownTexts(Walker&, Reference pattern, Reference flagsOrNull, Origin);
struct RegExpWrittenForCall {
    Reference object;
    StringID pattern;
    OptionSet<RegExpFlags> flags;
    Origin whereWritten;
};
std::optional<RegExpWrittenForCall> findRegExpWrittenForCall(Walker&, const BodyValue& call, ValueID operand);
Reference buildRegExpSharedForCall(Walker&, BuiltinOverloadID, BuiltinOperand, const RegExpWrittenForCall&);
Reference buildMatchByCodeOfRegExp(Walker&, const BodyValue& call, BuiltinOverloadID, const RegExpWrittenForCall&, Reference objectOfCall, Reference string, Origin, ExpectedType);

enum class TypedAccess : uint8_t {
    ReadsFilledSlot = 1 << 0,
    StoresIntoFilledSlot = 1 << 1,
    ReadsSlotThatMayBeEmpty = 1 << 2,
    StoresIntoSlotThatMayBeEmpty = 1 << 3,
    DefinesSlotThatMayBeEmpty = 1 << 4,
    FindsNoProperty = 1 << 5,
    FindsMethodAbove = 1 << 6,
};
bool possibleChangesForbid(OptionSet<LayoutChange> possibleChanges, TypedAccess, OptionSet<FactOfCell> knownOfCell = { }, bool guardOfReadsHolds = false);
MemberFound findMemberAboveObjectsOf(const Program&, const LinkTables&, LayoutID, PropertyKeyID);
MemberFound findMemberOfNamedObject(const Program&, const LinkTables&, const ObjectRecord&, PropertyKeyID);
LayoutID classOfClassObject(const Program&, FunctionID);
LayoutID classProvenAbove(const Program&, const LinkTables&, LayoutID classLayout);
TypeID typeReadByNameOffObjectsOf(const Program&, const LinkTables&, const Plan*, TypeArena&, LayoutID, PropertyKeyID);
PropertyKeyID keyOfAccessToSlot(const Program&, const BodyValue& access);
ASCIILiteral whyAccessByNameHasNoTypedForm(Walker&, const BodyValue&, TypeID typeOfObject);
void explainClosedAccess(Walker&, const BodyValue&, PrintStream&);
Reference buildTestOfSlot(Walker&, const BodyValue& access, Reference object);
Reference buildReadThatTestsShapeAndSlot(Walker&, const BodyValue& read, Reference object, TypeID guessed);
Reference buildReadThatTestsShapeAndCell(Walker&, const BodyValue& read, Reference object, TypeID guessed);
bool markOfCellAnswersForRead(Walker&, const BodyValue& read, TypeID typeOfObject);
bool storeMaySetMarkOfCell(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID, TypeID typeOfStored);
bool typedStoreAheadMaySetMarkOfCell(Walker&, TypeID typeOfObject, PropertyKeyID, TypeID typeOfStored);
bool buildStoreThatTestsForItself(Walker&, const BodyValue& store, Reference object, TypeID guessed);
Reference readSlotFoundFilled(Walker&, const BodyValue& access, Reference object);
Reference valueHeldOfFilledSlot(Walker&, const BodyValue& access, Reference object);
TypeID typeFoundInFilledSlot(Walker&, const BodyValue& read, TypeID typeOfObject);
TypeID typeSeenToArriveInFilledSlot(Walker&, const BodyValue& read, TypeID typeOfObject);
TypeID typeTakenBySlotOfStore(Walker&, const BodyValue& store, TypeID typeOfObject);
void noteSlotFoundEmpty(Walker&, const BodyValue& access, Reference object);
void lookAtCellInFrontOfStores(Walker&, std::span<const ValueID> stores, Reference object, NOESCAPE const ScopedLambda<void(Reference test)>& goOnWhereTestPasses);
std::optional<uint32_t> findLikelyOrderWordInFrontOfStores(Walker&, ValueID firstOfStretch, std::span<const ValueID> stores, Reference object, TypeID typeOfObject = { });
void testOrderWordInFrontOfStores(Walker&, ValueID firstStore, Reference object, uint32_t orderWord, NOESCAPE const ScopedLambda<void(Reference test)>& goOnWhereTestPasses);
bool looksAtSlotOfObjectBeingMade(const Walker&, const BodyValue& access);
bool isBeingMadeByBodyBeingRead(const Walker&, ValueID object);
void noteThisAtEntry(Walker&, Reference thisValue);

struct ExpectationOfValue {
    TypeID type;
    bool isDeclared { false };
};
ExpectationOfValue findExpectation(Walker&, ValueID input);
ExpectationOfValue expectationOfAnswerOfCallBeingCopied(Walker&, const BodyValue& call);
void forEachTypeThatGuessWouldProveAt(Walker&, const BodyValue&, const TypesOfInputs&, NOESCAPE const ScopedLambda<void(ValueID operand, TypeID expected, TypeID proven)>&);
void proveExpectedTypeOrBeginTurnAgain(Walker&, ValueID input, TypeID expected, Origin);
TypeID typeThatTypeTestAsksForWhereExpected(Walker&, Reference value, TypeID expected);
TypeID typeThatLookAtCalledSlotWouldProve(Walker&, const BodyValue& read, TypeID typeOfObject);
bool guessesMayTypeOperationInCleanTurn(Walker&, const BodyValue&, const TypesOfInputs&);
bool shouldOpenGuess(const Walker&, Reference value, TypeID expected, bool isDeclaredInSource, uint32_t numberOfTypedOperations);
void openGuess(Walker&, Reference value, TypeID expected, BlockID inputBlock, uint32_t begin, uint32_t end);

struct GuessInFrontOfLoop {
    Reference value;
    TypeID tested;
    LayoutSetID layouts;
    Vector<ValueID, 2> names;
};
Vector<GuessInFrontOfLoop, 4> findGuessesInFrontOfLoop(Walker&, BlockID inputHead, const LoopPreview&);
Vector<GuessInFrontOfLoop, 4> findMembersToldApartInFrontOfLoop(Walker&, BlockID inputHead, const LoopPreview&);
Reference buildTestInFrontOfLoop(Walker&, const GuessInFrontOfLoop&, Origin);
Reference buildProofInFrontOfLoop(Walker&, const GuessInFrontOfLoop&, Reference test, Origin);

Vector<TypeID, 8> typesBehindParameterGuard(Walker&, FunctionID);

bool walkEntryUnit(Walker&, CodeUnit);

} } } // namespace JSC::AOT::Typed

#include "AOTWalkCopies.h"

#include "AOTWalkLinksOfChains.h"
#include "AOTWalkForInLinks.h"

#endif // ENABLE(AOT)

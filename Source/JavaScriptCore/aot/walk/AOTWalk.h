/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTBitVector.h"
#include "AOTBody.h"
#include "AOTBodyIndex.h"
#include "AOTCode.h"
#include "AOTCompileUnit.h"
#include "AOTDiagnostics.h"
#include "AOTLazyMap.h"
#include "AOTMechanisms.h"
#include "AOTPromotedEnvironments.h"
#include "AOTWalkLinks.h"
#include "AOTWalkLoopsOverGeneratorsOfUnit.h"
#include "AOTWalkGeneratorsInLoops.h"
#include "AOTWalkCoroutineState.h"
#include "AOTSumOverRecords.h"
#include "AOTWalkState.h"
#include <array>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/IterationStatus.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class PrimitiveKnownToFolding;
class BodyEdges;
class UsersOfValuesInCountersOfLoops;
class Walker;

class TypeWithoutProof {
public:
    consteval TypeWithoutProof(TypeID writtenInTable)
        : m_type(writtenInTable)
    {
    }

    static TypeWithoutProof toldByForm(Representation form) { return TypeWithoutProof(widestTypeOf(form), FoundWhileWalking { }); }
    static TypeWithoutProof ofEntryParameter(Walker&, unsigned entryParameter);
    static TypeWithoutProof ofRecord(Walker&, const Draft&, ValueID record);
    static TypeWithoutProof restingOnBelief(TypeID type) { return TypeWithoutProof(type, FoundWhileWalking { }); }
    TypeWithoutProof metWithWhatFormTells(TypeArena&, Representation) const;
    TypeID type() const { return m_type; }

private:
    struct FoundWhileWalking { };
    TypeWithoutProof(TypeID type, FoundWhileWalking)
        : m_type(type)
    {
    }

    TypeID m_type;
};

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
Reference wordInPlaceOfRemovedCapture(Walker&, FunctionID, uint32_t capture);
Reference valueNoLongerCaptured(Walker&, FunctionID, uint32_t capture, Origin, Reference closure = { });
bool isVariableFilledAtFirstRead(const Plan&, VariableID);
bool takeObjectOfDeferredVariable(Walker&, VariableID, Reference held, Origin);
bool foldOperation(Walker&, const BodyValue&);
bool buildFreshObject(Walker&, const BodyValue&);
bool callBuiltin(Walker&, const BodyValue&);
void nameMethodOfReceiverAsMade(Walker&, const BodyValue& call, Draft&);
Reference findElidableSpreadSource(Walker&, const BodyValue& call, Reference copy);
bool goOverListInPlaceOfCallOfBuiltin(Walker&, const BodyValue& call, Draft& written);
bool isAskedForNameThatNoBuiltinAnswers(Walker&, ValueID);
bool isExpectedToBeArgumentsObject(Walker&, ValueID);
bool isErrorMessageExpected(Walker&, ValueID receiver, PropertyKeyID);
bool isShapedLikeLayoutsThatItCanNeverBe(Walker&, ValueID);
bool isDirectlyRecursive(const Program&, const BodyIndex&, FunctionID);
bool callsItself(const Walker&, const BodyIndex&, FunctionID);
bool isRecursiveThroughPrototype(const Walker&, const BodyIndex&, FunctionID);
TypeID typeThatMapIsExpectedToGive(Walker&, const BodyValue& call);
bool hasFewExpectedCallees(Walker&, const BodyValue& call);
bool isMathCall(const Walker&, const BodyIndex&, const BodyValue& operation);
void sayWhatIsExpectedOfComparedOperands(Walker&, const BodyValue& equality, Draft&);
bool expectsComparedLiteral(Walker&, const BodyValue& equality);
void sayWhatIsExpectedOfValueAskedForTruth(Walker&, const BodyValue& question, Draft&);
BlockID nearestBlockAboveThatBranchesOnTruthOf(const BodyIndex&, ValueID asked, BlockID below);
bool outermostLoopStoresWhatIsRead(const BodyIndex&, LoopID outermost, const BodyValue& read);
inline constexpr OpcodeSet opcodesThatCallFunctionGivenToVariableTakes { Opcode::Call };
AOT_LINK_OF_WALK(callFunctionGivenToVariable, opcodesThatCallFunctionGivenToVariableTakes)
inline constexpr OpcodeSet opcodesThatOpenGuessAtElementOfNumbersTakes { Opcode::ElemGet };
AOT_LINK_OF_WALK(openGuessAtElementOfNumbers, opcodesThatOpenGuessAtElementOfNumbersTakes)
inline constexpr OpcodeSet opcodesThatGuardSpliceCallTakes { Opcode::Call };
AOT_LINK_OF_WALK(guardSpliceCall, opcodesThatGuardSpliceCallTakes)
bool inlineCallee(Walker&, const BodyValue&);
bool callDirectly(Walker&, const BodyValue&);
bool buildTypedOperation(Walker&, const BodyValue&);
bool someLoopGivesSlowVersionSomethingToReckon(const BodyIndex&);
enum class GuessOpened : uint8_t {
    None,
    AsOneOperation,
    WithArms,
};
TypeID typeTestedInPlaceOfWrittenType(Walker&, FunctionID, uint32_t entryParameter, TypeID tested);
TypeID withoutOmittedNullish(Walker&, FunctionID, uint32_t entryParameter, TypeID);
GuessOpened openGuessAt(Walker&, const BodyValue&);
inline constexpr OpcodeSet opcodesThatAskWhetherHolderOfStoreInClosedArmOfLoopIsListTakes { Opcode::ElemSet };
AOT_LINK_OF_WALK(askWhetherHolderOfStoreInClosedArmOfLoopIsList, opcodesThatAskWhetherHolderOfStoreInClosedArmOfLoopIsListTakes)
bool openGuessAtAllocation(Walker&, const BodyValue&);
bool replaceOperation(Walker&, const BodyValue&);

std::optional<uint32_t> onlyTargetKnownToFolding(Walker&, const BodyTerminator&);
std::optional<uint32_t> onlyTargetOfWrittenConstants(Walker&, const BodyTerminator&);
bool buildTypedAccess(Walker&, const BodyValue&);
bool buildTypedElementAccess(Walker&, const BodyValue&);
bool buildLiteralLoopFills(Walker&, const BodyValue&);
AOT_LINK_OF_WALK(buildPushOntoLiteralThatLoopFills, opcodesThatBuildPushOntoLiteralThatLoopFillsTakes)
AOT_LINK_OF_WALK(hearRaiseWherePushIsBuiltAsWritten, opcodesThatHearRaiseWherePushIsBuiltAsWrittenTakes)
inline constexpr OpcodeSet opcodesThatBuildTypedOperatorTakes = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise }) | allEqualities | allRelations
    | OpcodeSet { Opcode::ToNumber, Opcode::ToNumeric, Opcode::ToString, Opcode::ToBoolean };
bool buildTypedOperator(Walker&, const BodyValue&);
bool buildTypedConstruction(Walker&, const BodyValue&);
std::optional<uint32_t> positionOfTagAmongArguments(const Walker&, FunctionID constructor);
bool everyReceiverKeepsFieldInSlotThatIsNeverEmpty(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID);
TypeID entryPairType(Walker&, TypeID iterable);
bool isArrayIteratorProtocolIntact(Walker&);
inline constexpr OpcodeSet opcodesThatOpenLoopOverIteratorThatEngineMakesTakes { Opcode::PropGet };
AOT_LINK_OF_WALK(openLoopOverIteratorThatEngineMakes, opcodesThatOpenLoopOverIteratorThatEngineMakesTakes)
AOT_LINK_OF_WALK(openTestOfListAtReadOfMethod, opcodesThatOpenLoopOverIteratorThatEngineMakesTakes)
AOT_LINK_OF_WALK(appendMembersInPlaceOfIteratorThatEngineMakes, opcodesThatOpenLoopOverIteratorThatEngineMakesTakes)
inline constexpr OpcodeSet opcodesThatCopySetByArrayFromTakes { Opcode::IsPristineArray };
AOT_LINK_OF_WALK(copySetByArrayFrom, opcodesThatCopySetByArrayFromTakes)
AOT_LINK_OF_WALK(takeFirstStepOfIteratorThatEngineMakes, opcodesThatOpenLoopOverIteratorThatEngineMakesTakes)
bool isListOfEntriesTakenApartAtOnce(Walker&, const BodyValue& call);
bool buildTypedIteration(Walker&, const BodyValue&);
bool replaceIteration(Walker&, const BodyValue&);
bool replaceCoroutineOperation(Walker&, const BodyValue&);
bool replaceCallDoneInPlace(Walker&, const BodyValue&);
bool replaceCallOfCompiledBuiltin(Walker&, const BodyValue&);
bool isCallForwardedInPlace(Walker&, const BodyValue&);
bool forwardCallOfBuiltin(Walker&, const BodyValue&);
bool forwardCallBehindTestOfCallee(Walker&, const BodyValue&);
bool forwardConstructionBehindTestOfTarget(Walker&, const BodyValue&);
bool callTargetOfBoundFunctionBehindTests(Walker&, const BodyValue&);
bool callIsExpectedToEnterTargetOfBoundFunction(Walker&, const BodyValue&);
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
void refineExpectationOfValue(Walker&, ValueID input, TypeID& found, bool& isDeclared, bool restsOnNamesUsedAlone);
void bindNamesBehindFailedTest(Walker&, Reference value, Reference test, uint32_t operandIndexOfTest, std::span<const ValueID> names, Origin);
Reference findCaptureAtHand(Walker&, FunctionID callee, uint32_t capture, Reference closure);
Reference valueOfStableBinding(Walker&, const BodyValue& read);
Reference valueOfStableBinding(Walker&, const BodyValue& read, Reference environment);
bool isLoopInvariantScope(const Program&, const BodyIndex&, ValueID scope, uint32_t firstValueOfLoop);
bool isSameScope(const Program&, const BodyIndex&, ValueID, ValueID);
Reference loopInvariantScope(Walker&, const BodyIndex&, ValueID scope, uint32_t firstValueOfLoop);
Draft draftScopeRead(Walker&, const BodyValue& read, Reference environment);
void hoistModuleLinks(Walker&, BlockID inputHead);
bool capturesOnlyModuleScopes(const Walker&, FunctionID);
bool hasKnownSingleInstance(const Walker&, ModuleID);
struct SingleInstance {
    Reference instance;
    Reference hasSeveral;
    Reference isSingleInstance;
};
std::optional<SingleInstance> findSingleInstance(Walker&, ModuleID, Origin);
Reference instanceAfterTest(Walker&, ModuleID, const SingleInstance&, Origin);
Vector<Reference, 2> capturedModuleScopes(Walker&, FunctionID, Reference instance, Origin);
Reference buildClosureOverScopes(Walker&, FunctionID, std::span<const Reference> scopes, Origin);
Reference buildClosureOverInstance(Walker&, FunctionID, Reference instance, Origin);
FunctionID findModuleScopeGetter(Walker&, const BodyValue& read);
bool callSingleInstanceGetter(Walker&, const BodyValue& read);
Reference valueOfStableVariable(Walker&, VariableID, TypeID widest);
void noteValueOfStableBinding(Walker&, VariableID, Reference value);
bool numberReadOutOfVariableStays(const Walker&, VariableID);
void noteValueOfStableBinding(Walker&, Reference environment, FieldReference, Reference value);
Reference objectOfVariablePutOff(Walker&, VariableID);
bool objectOfVariablePutOffStays(const Walker&, VariableID);
void noteObjectOfVariablePutOff(Walker&, VariableID, Reference object);
Reference findScopeOfClosureReadOffObjectInSight(const Walker&, FunctionID, Reference closure);
bool isClosureLeftUnmade(const Walker&, Reference);
FunctionID findLibraryFunctionBehindBuiltin(const Walker&, TypeID typeOfCallee);
struct LibraryFunctionsBehindBuiltin {
    FunctionID enteredByValue;
    Vector<FunctionID, 2> named;
    bool someSpecialCaseNamesFunction { false };
};
LibraryFunctionsBehindBuiltin findLibraryFunctionsBehindBuiltin(const Walker&, TypeID typeOfCallee);
bool takesNothingOfFunctionObject(const Program&, FunctionID);
enum class RowsThatNameLibraryFunction : bool { Any, SpecialCasesAlone };
FunctionID findLibraryFunctionOfRowsReachedBy(Walker&, const BodyValue& call, RowsThatNameLibraryFunction = RowsThatNameLibraryFunction::Any);
TypeID findTypeOfResultOfRowsReachedBy(Walker&, const BodyValue& call);
BuiltinOverloadID findMatchingOverload(Walker&, const BodyValue& call);
bool isLeafHandedToMethodOfLists(const Walker&, TypeID argumentType);
bool isBuiltinCalledWithoutReceiver(const Walker&, TypeID argumentType);
PropertyKeyID keyOfSymbolMadeOnce(Walker&, const BodyValue&);
bool tableOfImageHoldsData(Walker&, DataID);
ArrayStorage askBirthOfListForStorage(Walker&, Reference list, ArrayStorage wanted, bool widerListRefusesStore = false);
ArrayStorage askBirthOfListForStorageThatTakes(Walker&, Reference list, Reference stored, bool widerListRefusesStore = false);

PropertyKeyID keyThatAccessGoesBy(const Walker&, const BodyValue& access, PropertyKeyID* chosenAndNotDerived = nullptr);
PropertyKeyID nameOfTextThatAccessMayGoBy(const Walker&, const BodyValue& access, StringID text);
inline constexpr OpcodeSet accessesUnderComputedKeys { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };

class AccessUnderProvenKey {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AccessUnderProvenKey);
    WTF_MAKE_NONCOPYABLE(AccessUnderProvenKey);
public:
    AccessUnderProvenKey(const BodyValue& written, PropertyKeyID);

    BodyValue value() const LIFETIME_BOUND { return BodyValue::operation(m_words.data(), &m_unit, m_id); }

private:
    std::array<uint32_t, BodyValue::numberOfHeadWords + 6> m_words { };
    uint32_t m_entryOfKey { 0 };
    UnitIDRemap m_unit;
    ValueID m_id;
};

void takeArrivals(Walker&);
void markEscapingOperands(Walker&, const Draft&, const Effects* foundByCaller);
bool objectPrototypeMayGainName(const Program&);
bool byteTellsOfNamesAddedToObjectPrototype(const Program&);
bool nothingButOwnFieldsAnswersFor(const Program&, const Operation& made);
void noteFreshList(Walker&, Reference list);
void noteListMadeByCaller(Walker&, Reference list);
void markObjectPristine(Walker&, Reference list);
bool isKnownPristine(Walker&, Reference list);
std::optional<uint32_t> lengthOfFreshList(Walker&, Reference list);
Reference elementOfFreshList(Walker&, Reference list, uint32_t position);
std::optional<Vector<Reference, 8>> elementsOfFreshList(Walker&, Reference list);
std::optional<uint32_t> lengthOfListInSight(Walker&, Reference list);
Reference elementOfListInSight(Walker&, Reference list, uint32_t position);
std::optional<Draft> draftOfCallWithListWrittenOut(Walker&, const BodyValue& call);
bool isFreshPristineArray(Walker&, Reference list);
bool isMadeByCallThatListsEntriesFlat(Walker&, Reference list);
void noteFreshObject(Walker&, Reference object, size_t numberOfFieldsWritten);
struct PropertyOfFreshObject {
    FieldReference field;
    Reference value;
};
std::optional<Vector<PropertyOfFreshObject, 8>> propertiesOfFreshObject(Walker&, Reference object);
struct ObjectsThatMeet {
    struct Slot {
        FieldReference field;
        Reference position;
        Reference valueOnEveryWay;
        Vector<Reference, 4> valuesOfWays;
    };
    TypeID shape;
    OptionSet<CellFact> facts;
    uint32_t orderWord { 0 };
    Reference lengthOfLists;
    Vector<Slot, 8> slots;
};
bool isListOfKnownLength(Walker&, const WalkState&, Reference list);
bool orderWordContainsField(const Walker&, Reference object, uint32_t orderWord, FieldIndex);
std::optional<ObjectsThatMeet> findObjectsThatMeetAsOne(Walker&, std::span<const WalkState* const> ways, std::span<const Reference> arguments, uint32_t firstRecordBehindParting, const ScopedLambda<bool(size_t way, ValueID cell)>* canMergeOlder = nullptr);
void noteObjectsThatHaveMetAsOne(Walker&, Reference parameter, const ObjectsThatMeet&, std::span<const Reference> parametersOfSlots);
Reference lengthOfFreshArguments(Walker&, Reference arguments);
Reference variableOfMappedArguments(Walker&, Reference arguments, uint32_t parameter);
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
struct AccessesNearCounter;
struct EndLoopTest;
struct EqualityLoopTest;
struct StringsFinishedOnEdge;
struct LoopPreview;
struct HoistedArrayGuard;
struct SlotOfLoop {
    static constexpr uint32_t mostFieldsOfScope = 512;
    std::pair<ValueID, uint32_t> order() const
    {
        if (variable)
            return { holder, variable.index() << 1 };
        return { holder, field.layout ? (field.layout.index() * mostFieldsOfScope + field.field) << 1 | 1 : key.bits() };
    }

    ValueID holder;
    PropertyKeyID key;
    bool isInt32Position { false };
    bool isPosition { false };
    bool hasLeftIntegers { false };
    bool isSteppedByOne { false };
    bool onlyGrows { false };
    bool hasUnknownSteps { false };
    Range stepsOfTurn { Range::empty() };
    bool isFilledOnce { false };
    bool isComputedFromItself { false };
    VariableID variable { };
    FieldReference field { };
    TypeID typeOfScope { };
};
struct CapturedCounter {
    FieldReference field;
    Range integers { Range::empty() };
    ValueID parameter { };
    bool hasLeftIntegers { false };
};
void hoistInvariants(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
void enterHandler(Walker&, BlockID inputHandler);
Representation representationOfNumber(const Walker&, TypeID);

bool walkRampOfCoroutine(Walker&);
bool runsBodyAtDelegatedCall(const Walker&, CodeUnit);
bool buildRampUnlessDelegated(Walker&, std::span<const Reference> parameters);
bool callDelegate(Walker&, const BodyValue& call);
Reference addDriverParameterOfCoroutine(Walker&, BlockID entry);
Reference convertReturnedValueOfCoroutine(Walker&, Reference returned, Origin);
Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, Origin);
Vector<Reference, 8> heldForCells(Walker&, std::span<const Reference> held);

void checkInt32Promise(Walker&, const DeclaredType&, ValueID entering, Origin);

bool isTextBuiltOfParts(Walker&, Reference text);

struct StepOfListLiteral {
    bool isDone { false };
    ValueID element;
};

FunctionID findOnlyCallee(Walker&, const BodyValue& call, Reference callee);
FunctionID findOnlyCallee(Walker&, const BodyValue& call, TypeID typeOfCallee);
bool makesThisOfConstruction(const Walker&, FunctionID constructor);
FunctionID findFunctionBehindCompiledBuiltin(const Walker&, TypeID typeOfCallee);
bool callAccessor(Walker&, const BodyValue& access, FunctionID accessor, Reference thisValue, Reference stored, Reference closure = { });
bool accessorAdmitsDirectCall(Walker&, FunctionID accessor);
bool callTrapOfProxy(Walker&, const BodyValue& access, FunctionID trap, Reference handler, std::span<const Reference> arguments, Reference closure);
bool callMethodOfProtocol(Walker&, const BodyValue& step, FunctionID method, Reference thisValue, Reference closure);
bool canCallCompilerHelper(Walker&, FunctionID helper);
bool mayCopyFusedChain(Walker&, FunctionID text);
bool isPureCallback(const Walker&, FunctionID);
bool copyFusedChain(Walker&, const BodyValue& lastCall, FunctionID text, std::span<const Reference> arguments, BlockID& asWritten);
bool fuseChainOfCallbacks(Walker&, const BodyValue& firstCall, BuiltinID firstMethod, size_t fewestLinks);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);
enum class MayCopyFunctionCalledDirectly : bool { No, Yes };
bool callFunctionDirectly(Walker&, const BodyValue& operation, FunctionID, Reference thisValue, std::span<const Reference> arguments, MayCopyFunctionCalledDirectly = MayCopyFunctionCalledDirectly::Yes, FunctionID calledWhereNoCopyIsMade = { });
bool callGetterExpectedAtRead(Walker&, const BodyValue& read);
AOT_LINK_OF_WALK(leaveReadOfMethodToTableCall, opcodesThatLeaveReadOfMethodToTableCallTakes)
AOT_LINK_OF_WALK(callMethodThroughTable, opcodesThatCallMethodThroughTableTakes)
AOT_LINK_OF_WALK(callMethodsBehindTestsOfShape, opcodesThatCallMethodsBehindTestsOfShapeTakes)
bool callMethodsOfExpectedReceiversBehindTestsOfShape(Walker&, const BodyValue& read);
bool callThroughSuperBehindGuards(Walker&, const BodyValue& superBase);
inline constexpr OpcodeSet opcodesThatReadBehindTestsOfShapeTakes { Opcode::PropGet };
AOT_LINK_OF_WALK(readBehindTestsOfShape, opcodesThatReadBehindTestsOfShapeTakes)
GuessOpened testNameAfterShapeTests(Walker&, const BodyValue& question);
bool areReceiversFilledAtAllReads(Walker&, TypeID receivers, PropertyKeyID);
bool askGuardsAboutDeclaredMember(Walker&, const BodyValue&);
BuiltinID findMethodThatSomeClassGivesUnderKey(Walker&, TypeID receiver, PropertyKeyID, unsigned numberOfArguments);
bool askWhetherMemberOfBuiltinIsAsBuilt(Walker&, const BodyValue&);
bool askCellWhetherItIsPlainInstance(Walker&, const BodyValue&);
bool askCellWhetherItIsAsBorn(Walker&, const BodyValue&);
bool askGuardsWhetherPrototypeChainIsPlain(Walker&, const BodyValue&);
void noteInstanceOfResult(Walker&, const BodyValue& test, bool passed);
void beginGuardsOfClassesAtEndOfModule(Walker&, Origin);
TypeID objectsExpectedOfOperandOfQuestion(Walker&, ValueID input);
FunctionID functionExpectedOf(Walker&, ValueID input);
bool findConditionsOfOrdinaryInstanceOf(const Walker&, TypeID typeOfConstructor, Vector<MemberConditionRecord, 16>& conditions);
bool canDecideInstanceOfByHierarchy(const Walker&, TypeID typeOfConstructor);
bool askCellAndGuardsWhetherNoMemberIsFound(Walker&, const BodyValue&);
OptionSet<CellFact> factsThatSlotOperationLeansOn(Walker&, const Draft&);
OptionSet<CellFact> knownCellFacts(const Walker&, Reference object);
void askWhatLookupLeansOn(Walker&, const Draft& lookup);
LayoutSetID receiversWhoseCellsAnswerForOwnProperty(Walker&, ValueID input, PropertyKeyID);
bool cellAnswersForOwnProperty(Walker&, TypeID typeOfObject, PropertyKeyID);
TypeID objectsAboveWhichRecordsDeclareMember(Walker&, TypeID typeOfObjects, PropertyKeyID, LayoutID declaredAtOrAbove = { });
bool isNameOutsideFields(Walker&, TypeID typeOfObjects, PropertyKeyID);
FunctionID findOnlyMethodThatRecordsDeclareAbove(Walker&, TypeID declaring, PropertyKeyID);
bool findGuardsOfMethodAboveObjectsOf(Walker&, TypeID typeOfObjects, PropertyKeyID, Vector<MemberConditionRecord, 4>&);
bool instancesShadowMethod(const Walker&, TypeID typeOfObjects, PropertyKeyID);
enum class AssignedMemberChoice : uint8_t { WhenUnconditional, Always, Never };
FunctionID findMethodAfterEndedGuard(Walker&, TypeID typeOfObjects, PropertyKeyID, bool& isReplaced, FunctionID* written);
std::optional<AssignedMemberChoice> findOtherMemberChoice(Walker&, const BodyValue& read, TypeID typeOfObjects, bool isTypeKnown);
bool isAccessorReplaced(Walker&, TypeID typeOfObjects, PropertyKeyID);
bool callForwardedDirectly(Walker&, const BodyValue& call, const Draft& forwarded);
bool constructForwardedDirectly(Walker&, const BodyValue& call, const Draft& forwarded);
bool inlineConstructionHandedOn(Walker&, const BodyValue& call, const Draft& site, FunctionID callee);
bool listOfSuperCallCanBeHandedOver(Walker&, const BodyValue& call, FunctionID constructor);
bool listSpreadBehindArgumentsIsRestListOf(Walker&, const BodyValue& call, FunctionID callee);
bool hasKnownSpreadElements(Walker&, const BodyValue& call, ValueID before);
bool spreadAppendsElementsOfListAsMade(Walker&, ValueID spreadValue);
void elideSpreadCopy(Walker&, const BodyValue& call, Draft& written);
bool callBuiltinForwarded(Walker&, const BodyValue& call, const Draft& forwarded);
TailCallRequirement requirementOfTailCall(const Walker&, Reference call);
bool calleeOfTailCallComesBack(const Walker&, FunctionID callee, std::span<const Reference> arguments);
bool passOnOwnArgumentsByCount(Walker&, const BodyValue& call, const Draft& written, Reference callee, Reference thisValueOrNull, Reference arguments);

Reference callInPlaceOfCopy(Walker&, const Draft& site, FunctionID callee, std::span<const Reference> entryArguments, bool isConstruction, bool entersAsCallWould = false);
bool unitMayLeaveByDirectCallOf(Walker&, FunctionID callee);
bool canTailCallMethodTable(Walker&, uint32_t selector, std::span<const FunctionID> methods);
bool unitTakesWhatCopyIsHanded(Walker&, FunctionID callee, std::span<const Reference> entryArguments);

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
    uint32_t numberOfOperations { 0 };
};
std::optional<TextOfCopyRead> turnsOfLoopsOfCopyWrittenOut(const Walker&, FunctionID callee, std::span<const std::optional<int64_t>> integersHandedOver, std::span<const std::optional<uint32_t>> lengthsOfListsHandedOver);
uint32_t sizeOfCopyThatWritesOutLoopsOverListsHandedIn(const Walker&, FunctionID callee);
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
ValueID findOpeningThatAloneTakesParameter(const BodyIndex&, ValueID parameter);
bool isIteratorOfTableJustMade(const Walker&, Reference);
bool openOverListAsMadeGoesByIndex(Walker&, const BodyIndex&, ValueID open);
ValueID iteratorClosedLeavingFunction(const BodyIndex&, BodyBlock);
ValueID iteratorClosedLeavingLoop(const BodyIndex&, BodyBlock, LoopID);
bool mayCopyCallee(const Walker&, const Draft& site, FunctionID callee, const PlaceOfCall&);
bool canInlineSingleCallLoop(const Walker&, FunctionID callee, const PlaceOfCall&, const std::span<const Reference>* passedOperands);
bool shouldCopyCallee(const Walker&, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands, bool callsParameterOfCopy = false);
bool copyOfCalleeCalledInLoopPays(const Walker&, FunctionID callee, size_t numberOfArguments);
bool copyOfCalleePaysAt(const Walker&, FunctionID callee, size_t numberOfArguments, const PlaceOfCall&);
bool callsFewHeldClosures(Walker&, const BodyValue& call);
bool callsTableCallees(Walker&, const BodyValue& call);
TypeID fewClosuresWhoseCopiesPayInLoop(Walker&, TypeID held, size_t numberOfArguments, size_t fewest = 2);
Vector<Reference, 8> collectPassedOperands(const Draft& site, const Function& callee);

struct NameOfGrowingText {
    ValueID value;
    ValueID version;
};
struct GrowingText {
    BlockID loopHead;
    uint32_t parameterIndex { 0 };
    Vector<NameOfGrowingText, 8> names;
    Vector<ValueID, 2> parametersOfInnerHeads;
    Vector<ValueID, 2> parametersThatReadText;
    bool isOnlyConcatenated { false };
    bool standsUnderHandlers { false };
    bool beginsAnewOnSomeEdge { false };
};
struct StringInBuilder {
    GrowingText web;
    uint32_t frameIndex { 0 };
    Reference builder;
    bool beganEmpty { false };
    bool beganAsText { true };
    bool liesInFrame { false };
    bool waitsForNextLoop { false };
    bool isNoLongerNeeded { false };
    BlockID headThatTookIt;
    Reference stringOfState;
    Reference lengthOfState;
    BlockID blockOfState;
};

class TypesOfValuesOfLoop {
public:
    bool isEmpty() const { return !m_size; }
    size_t size() const { return m_size; }
    TypeID operator[](size_t place) const { return m_placeInNest + place < m_types.size() ? TypeID::fromBits(~m_types[static_cast<uint32_t>(m_placeInNest + place)]) : TypeID(); }
    bool holds(size_t place) const { return holdsInOneRun(place, 1); }
    bool holdsInOneRun(size_t place, size_t length) const
    {
        size_t low = 0;
        for (size_t high = m_runs.size(); low < high;) {
            size_t middle = (low + high) / 2;
            if (m_runs[middle].first <= place)
                low = middle + 1;
            else
                high = middle;
        }
        if (!low)
            return false;
        const Run& run = m_runs[low - 1];
        return place - run.first < run.length && length <= run.length - (place - run.first);
    }
    void set(size_t place, TypeID type)
    {
        ASSERT(holds(place));
        uint32_t index = static_cast<uint32_t>(m_placeInNest + place);
        if (m_types[index] != ~type.bits())
            m_types.mutableAt(index) = ~type.bits();
    }
    void setEach(size_t place, size_t length, NOESCAPE const Invocable<TypeID(size_t place)> auto& typeAt)
    {
        ASSERT(!length || holdsInOneRun(place, length));
        for (size_t end = place + length; place < end;) {
            for (uint32_t& slot : m_types.mutableStretchAt(static_cast<uint32_t>(m_placeInNest + place), static_cast<uint32_t>(end - place)))
                slot = ~typeAt(place++).bits();
        }
    }
    void clearAndBeginAt(uint32_t firstValue, uint32_t firstValueOfNest)
    {
        m_runs.shrink(0);
        m_types.clear();
        m_sumsWithoutType.shrink(0);
        m_size = 0;
        m_firstValueOfNest = firstValueOfNest;
        m_placeInNest = firstValue - firstValueOfNest;
    }
    Vector<uint32_t>& sumsWithoutType() { return m_sumsWithoutType; }
    const Vector<uint32_t>& sumsWithoutType() const { return m_sumsWithoutType; }
    bool holdsAt(size_t place, const TypesOfValuesOfLoop& other) const
    {
        RELEASE_ASSERT(m_firstValueOfNest == other.m_firstValueOfNest && m_placeInNest + place == other.m_placeInNest);
        return std::ranges::all_of(other.m_runs, [&](const Run& run) {
            uint32_t first = other.m_placeInNest + run.first;
            return holdsInOneRun(place + run.first, run.length) && m_types.isSameIn(other.m_types, first, first + run.length);
        });
    }
    bool holdsRunsOf(const TypesOfValuesOfLoop& inside) const
    {
        return inside.m_runs.isEmpty() || (m_firstValueOfNest == inside.m_firstValueOfNest && inside.m_placeInNest >= m_placeInNest && std::ranges::all_of(inside.m_runs, [&](const Run& run) {
            return holdsInOneRun(inside.m_placeInNest - m_placeInNest + run.first, run.length);
        }));
    }
    void takeWhole(const TypesOfValuesOfLoop& inside)
    {
        RELEASE_ASSERT(holdsRunsOf(inside));
        for (const Run& run : inside.m_runs)
            m_types.takeStretchOf(inside.m_types, inside.m_placeInNest + run.first, inside.m_placeInNest + run.first + run.length);
    }
    void takeWhereNoTypeIsNoted(const TypesOfValuesOfLoop& inside)
    {
        RELEASE_ASSERT(holdsRunsOf(inside));
        for (const Run& run : inside.m_runs) {
            uint32_t first = inside.m_placeInNest + run.first;
            for (uint32_t index = first; index < first + run.length; ++index) {
                if (!m_types[index] && inside.m_types[index])
                    m_types.mutableAt(index) = inside.m_types[index];
            }
        }
    }
    void appendRun(uint32_t first, uint32_t length)
    {
        RELEASE_ASSERT(first >= m_size);
        if (!length)
            return;
        if (!m_runs.isEmpty() && m_runs.last().first + m_runs.last().length == first)
            m_runs.last().length += length;
        else
            m_runs.append(Run { first, length });
        m_size = first + length;
    }
    void makeRoomForRuns(uint32_t numberOfValuesOfNest, const TypesOfValuesOfLoop* insideTakenWhole)
    {
        RELEASE_ASSERT(m_placeInNest + m_size <= numberOfValuesOfNest && (!insideTakenWhole || (holdsRunsOf(*insideTakenWhole) && insideTakenWhole->m_types.size() == numberOfValuesOfNest)));
        if (insideTakenWhole)
            m_types = insideTakenWhole->m_types;
        else
            m_types.fill(0, numberOfValuesOfNest);
    }

private:
    struct Run {
        uint32_t first;
        uint32_t length;
    };
    Vector<Run, 2> m_runs;
    SharedArray<uint32_t, 0> m_types;
    Vector<uint32_t> m_sumsWithoutType;
    uint32_t m_size { 0 };
    uint32_t m_firstValueOfNest { 0 };
    uint32_t m_placeInNest { 0 };
};
struct LoopLookedAt {
    LoopID loop;
    uint32_t firstValue { 0 };
    std::shared_ptr<TypesOfValuesOfLoop> types;
};

template<typename T, size_t inlineCapacity>
ALWAYS_INLINE void moveWhereSanitizerWatches(Vector<T, inlineCapacity>& list)
{
#if ASSERT_ENABLED || ASAN_ENABLED
    constexpr size_t longestListThatMoves = 64;
    if (list.size() > longestListThatMoves || list.size() == list.capacity())
        return;
    Vector<T, inlineCapacity> moved;
    moved.reserveInitialCapacity(std::max<size_t>(list.capacity(), inlineCapacity + 1));
    for (T& held : list)
        moved.append(WTF::move(held));
    list = WTF::move(moved);
#else
    UNUSED_PARAM(list);
#endif
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<typename T, size_t inlineCapacity = 0>
class ListPutBackByLog {
public:
    size_t size() const { return m_list.size(); }
    bool isEmpty() const { return m_list.isEmpty(); }
    const T& operator[](size_t place) const LIFETIME_BOUND { return m_list[place]; }
    T& operator[](size_t place) LIFETIME_BOUND
    {
        T& element = m_list[place];
        if (m_serialOfNewestMark > m_serialsLogged.span().data()[place]) [[unlikely]]
            log(place);
        return element;
    }
    void append(T&& value)
    {
        moveWhereSanitizerWatches(m_list);
        m_list.append(WTF::move(value));
        m_serialsLogged.append(m_serialOfNewestMark);
    }
    void constructAndAppend()
    {
        moveWhereSanitizerWatches(m_list);
        m_list.constructAndAppend();
        m_serialsLogged.append(m_serialOfNewestMark);
    }
    T& last() LIFETIME_BOUND { return (*this)[m_list.size() - 1]; }
    const T& last() const LIFETIME_BOUND { return m_list.last(); }
    void removeLast()
    {
        if (m_serialOfNewestMark)
            m_log.append(Logged { static_cast<uint32_t>(m_list.size() - 1), m_serialsLogged.last(), WTF::move(m_list.last()), true });
        m_list.removeLast();
        m_serialsLogged.removeLast();
    }
    auto begin() const LIFETIME_BOUND { return m_list.begin(); }
    auto end() const LIFETIME_BOUND { return m_list.end(); }
    void clear()
    {
        m_list.shrink(0);
        m_serialsLogged.shrink(0);
        m_log.shrink(0);
        m_serialOfNewestMark = 0;
    }
    void takeRoomOf(ListPutBackByLog& former)
    {
        takeRoom(m_list, former.m_list);
        takeRoom(m_serialsLogged, former.m_serialsLogged);
        takeRoom(m_log, former.m_log);
    }
    void setSerialOfNewestMark(uint32_t serial)
    {
        m_serialOfNewestMark = serial;
        if (!serial)
            m_log.shrink(0);
    }
    size_t sizeOfLog() const { return m_log.size(); }
    void putBack(size_t size, size_t sizeOfLog)
    {
        while (m_log.size() > sizeOfLog) {
            Logged logged = m_log.takeLast();
            if (logged.place >= size)
                continue;
            if (logged.wasRemoved) {
                m_list.shrink(logged.place);
                m_serialsLogged.shrink(logged.place);
                m_list.append(WTF::move(logged.before));
                m_serialsLogged.append(logged.serialBefore);
                continue;
            }
            m_list[logged.place] = WTF::move(logged.before);
            m_serialsLogged[logged.place] = logged.serialBefore;
        }
        RELEASE_ASSERT(m_list.size() >= size);
        m_list.shrink(size);
        m_serialsLogged.shrink(size);
    }

private:
    struct Logged {
        uint32_t place { 0 };
        uint32_t serialBefore { 0 };
        T before;
        bool wasRemoved { false };
    };
    NEVER_INLINE void log(size_t place)
    {
        m_log.append(Logged { static_cast<uint32_t>(place), m_serialsLogged[place], m_list[place], false });
        m_serialsLogged[place] = m_serialOfNewestMark;
    }

    Vector<T, inlineCapacity> m_list;
    Vector<uint32_t, inlineCapacity> m_serialsLogged;
    Vector<Logged> m_log;
    uint32_t m_serialOfNewestMark { 0 };
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

template<typename T, size_t inlineCapacity = 0>
class ListChangedOnlyByName : public ListPutBackByLog<T, inlineCapacity> {
    using Base = ListPutBackByLog<T, inlineCapacity>;
public:
    const T& operator[](size_t place) const LIFETIME_BOUND { return Base::operator[](place); }
    const T& last() const LIFETIME_BOUND { return Base::last(); }
    T& changeable(size_t place) LIFETIME_BOUND { return Base::operator[](place); }
    T& changeableLast() LIFETIME_BOUND { return Base::last(); }
};

struct StringInBuilderIndex {
    using Ends = HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    Ends endsByName;
    Ends endsByLoopHead;
    Vector<uint32_t> endsBeforeInSameLoop;
    Vector<uint32_t, 4> endsByFrame;
    struct Before {
        uint32_t endOfFrame { 0 };
        uint32_t numberOfNamesBefore { 0 };
    };
    Vector<Before> before;
    Vector<std::pair<uint64_t, uint32_t>> endsByNameBefore;
};

struct KeptBetweenUnits;

class BlockBeingBuilt {
public:
    explicit BlockBeingBuilt(const Code& code)
        : m_code(&code)
    {
    }

    ALWAYS_INLINE BlockBeingBuilt& operator=(BlockID id)
    {
        m_id = id;
        m_record = id ? &m_code->at(id) : nullptr;
        return *this;
    }
    ALWAYS_INLINE operator BlockID() const { return m_id; }
    ALWAYS_INLINE explicit operator bool() const { return !!m_record; }
    ALWAYS_INLINE uint32_t index() const { return m_id.index(); }
    ALWAYS_INLINE bool hasEndedOrIsNone() const { return !m_record || m_record->hasEnded(); }

private:
    const Code* m_code;
    const Block* m_record { nullptr };
    BlockID m_id;
};

struct OtherClassBelowMixin {
    Reference value;
    TypeID heirs;
    TypeID others;
    TypeID tested;
    LayoutSetID layouts;
    Vector<ValueID, 2> names;
};

class Walker {
    WTF_MAKE_NONCOPYABLE(Walker);
    friend struct KeptBetweenUnits;
    friend class TypeWithoutProof;
public:
    Walker(const WalkInputs&, TypeArena&, Arrivals&, KeptBetweenUnits&);
    ~Walker();

    void takeRoomOf(Walker& former);
    bool hasBegunUnit() const { return m_hasBegunUnit; }
    bool walk(CodeUnit, WalkMode = WalkMode::AsBuilt);
    bool hasLastWalksOfLoopsToWalkAlone() const;
    bool walkLastWalksOfLoopsAlone(Walker& first);
    bool standInFrontOfBodyOf(FunctionID);
    Code& code() LIFETIME_BOUND { return m_code; }
    struct MethodGivenUnderKey {
        BuiltinClassID builtinClass;
        BuiltinID method;
    };
    UncheckedKeyHashMap<uint64_t, Vector<MethodGivenUnderKey, 2>>& methodsGivenUnderKeys;
    struct ReceiverMadeInBody {
        ValueID made;
        uint8_t classesItStaysAsMadeOf;
        uint32_t firstPlaceWhereItIsHandedOn;
    };
    UncheckedKeyHashMap<uint32_t, Vector<ReceiverMadeInBody, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>& receiversMadeInBodies;
    struct GettersExpectedInFunction {
        FunctionID function;
        std::span<const LinkedGetterAtRead> reads;
    };
    GettersExpectedInFunction& gettersExpectedInFunction;
    const Code& code() const LIFETIME_BOUND { return m_code; }

    const Program& program() const { return m_inputs.program; }
    const Facts& facts() const { return m_inputs.facts; }
    const Plan& plan() const { return m_inputs.plan; }
    TypeArena& types() const { return m_types; }
    const Arrivals& arrivals() const { return m_arrivals; }
    static constexpr uint32_t maxNestingDepth = 1024;
    bool isNestedTooDeeply() const { return m_nestingDepth >= maxNestingDepth; }
    bool isQuietBeforeInitialYield(FunctionID generatorFunction);
    bool loopCarriesValuesOfConsumerRound(BlockID inputHead) const;
    std::optional<uint32_t> iterationsTakenByPattern(LoopID) const;
    bool isConsumerSmallEnoughToVersion(BlockID inputHead, uint64_t largestLoop) const;
    WalkState& state() { return m_state; }
    const WalkState& state() const { return m_state; }
    void noteEnvironmentField(Reference environment, FieldReference, Reference value);
    void noteThatUnitMakesScope() { m_unitHasMadeScopes = true; }
    bool unitHasMadeScopes() const { return m_unitHasMadeScopes; }
    void noteStoreIntoUnmadeRecord() { m_state.noteStoreIntoUnmadeRecord(++m_numberOfStoresIntoUnmadeRecords); }
    bool closuresHoldScopes() const { return !m_scopesThatClosuresHold.isEmpty(); }
    void listScopesThatValueNames(Reference, Vector<Reference, 2>&) const;
    bool operationOnlyBorrowsScope(const Draft&, Reference scope, uint32_t operandIndex, bool isHeldByClosure);
    Reference buildCallThatMayBorrowScopes(Draft&&, const Effects* foundByCaller);
    bool has(Mechanism mechanism) const { return m_mechanisms.contains(mechanism); }
    bool reckonsOnWhatItHasBuilt() const { return has(Mechanism::TypedOperations) || (unit().role == CodeRole::SlowBody && has(Mechanism::UnboxedNumbers)); }
    bool hidesNames() const { return m_inputs.options.hidesNames; }
    RegExpSummaries* regExpSummaries() const { return m_inputs.regExpSummaries; }
    FormatPatternPlans* formatPatternPlans() const { return m_inputs.formatPatternPlans; }
    ScratchRealm* scratchRealm() const { return m_inputs.scratchRealm; }
    CodeUnit unit() const { return m_code.unit(); }
    TypeID typeArrivingAtEntryParameter(unsigned entryParameter) { return typeArrivingAtEntryParameter(unit().function, entryParameter); }
    TypeID typeArrivingAtEntryParameter(FunctionID, unsigned entryParameter);
    bool opensGuesses() const { return m_opensGuesses; }
    bool standsInArmOfRowOfShapeTests() const { return m_numberOfFramesAtRowOfShapeTests; }
    bool standsInClosedArm() const { return m_numberOfClosedArms; }
    struct StretchWithArmsAsWritten {
        BlockID asWritten;
        uint32_t restartPoint { 0 };
        size_t numberOfFrames { 0 };
        ValueID row;
        Reference list;
        uint32_t first { 0 };
        uint32_t size { 0 };
        BlockID restAsWritten;
        uint32_t placeOfRest { 0 };
        Vector<Reference, 16> boundInFrontOfRest;
        bool restartsIterationOnFailure { false };
    };
    struct StoresIntoRowsOfBlock {
        const BodyIndex* body { nullptr };
        FunctionID function;
        BlockID block;
        Vector<std::pair<uint64_t, uint32_t>, 8> stores;
        Vector<uint32_t, 4> placesInNoStretch;
        bool accessesSeveralArrays { false };
    };
    StoresIntoRowsOfBlock& storesIntoRowsOfBlockOfFrame()
    {
        if (m_storesIntoRowsOfBlocks.size() < m_frames.size())
            m_storesIntoRowsOfBlocks.grow(m_frames.size());
        return m_storesIntoRowsOfBlocks[m_frames.size() - 1];
    }
    struct UsersWithinBlock {
        const BodyIndex* body { nullptr };
        FunctionID function;
        BlockID block;
        Vector<uint64_t, 8> pairsOfValueAndUser;
    };
    const BitVector& valuesTakenForKeysInCurrentBody();
    bool storesByName(const BodyIndex&, const BodyValue& access);
    bool updatesFieldInLoop(const BodyIndex&, LoopID);
    bool storesSumOfWords(const BodyIndex&, LoopID, PropertyKeyID);
    struct ElementReads {
        uint32_t loop { 0 };
        uint32_t list { 0 };
        uint32_t key { 0 };
        uint32_t all { 0 };
        uint32_t keeping { 0 };
    };
    const ElementReads* findElementReads(const BodyValue& read);
    struct ElementAccess {
        uint32_t array { 0 };
        uint32_t access { 0 };
    };
    std::span<const ElementAccess> elementAccessesOf(ValueID array);
    template<typename Fill> const BitVector& numbersInListOfWrittenObject(const void* list, const Fill& fill) const
    {
        return m_numbersInListsOfWrittenObjects.ensure(list, [&] {
            BitVector numbers;
            fill(numbers);
            return numbers;
        }).iterator->value;
    }
    uint32_t lastStoreByKeyOfLoopInCurrentBody(LoopID);
    std::span<const uint32_t> pushesOntoLiteralsOf(LoopID);
    bool isPushOntoLiteral(const BodyValue& call);
    bool pushesOntoField(const BodyIndex&);
    ValueID findReturnedElement(const BodyIndex&);
    bool isPopFromLiteral(const BodyValue& call);
    struct ListAccessesInBlock {
        BlockID block;
        Vector<ValueID, 2> accesses;
    };
    std::span<const ListAccessesInBlock> listAccessesOfLoop(LoopID);
    bool isOutermostPushPopLoop(LoopID);
    std::span<const std::pair<uint64_t, uint32_t>> laterStoresUnderSameKeyInBlockInCurrentBody(const BodyValue& first);
    bool textOfOutermostLoopStoresUnderNameInCurrentBody(LoopID outermost, PropertyKeyID name);
    bool someElementOfLoopMeetsWhatGoesRoundInCurrentBody(LoopID);
    TypeID literalElementTypeInLoops(ValueID literal);
    bool loopsPushNumbersOnto(ValueID literal);
    static bool findsElementThatMeetsWhatGoesRound(const BodyIndex&, LoopID, OpcodeSet operatorsOfRunningValues);
    const BitVector& listsThatCallsAloneTakeInCurrentBody();
    struct UsersOfValuesOfLoop {
        const BodyIndex* body { nullptr };
        FunctionID function;
        BlockID head;
        std::shared_ptr<const UsersOfValuesInCountersOfLoops> users;
    };
    UsersOfValuesOfLoop& usersOfValuesOfLoopOfFrame()
    {
        if (m_usersOfValuesOfLoops.size() < m_frames.size())
            m_usersOfValuesOfLoops.grow(m_frames.size());
        return m_usersOfValuesOfLoops[m_frames.size() - 1];
    }
    UsersWithinBlock& usersWithinBlockOfFrame()
    {
        if (m_usersWithinBlocks.size() < m_frames.size())
            m_usersWithinBlocks.grow(m_frames.size());
        return m_usersWithinBlocks[m_frames.size() - 1];
    }
    bool standsInStretchWithArmsAsWritten() const { return !!m_stretchWithArmsAsWritten.asWritten; }
    StretchWithArmsAsWritten& stretchWithArmsAsWritten() { return m_stretchWithArmsAsWritten; }
    bool stretchMayRunAsWrittenInstead() const;
    void runStretchAsWrittenInstead();
    uint32_t markRestartPoint();
    void letRestOfStretchRunAsWrittenBehindFirstStore(ValueID store);
    bool standsAtFrameOfRowOfShapeTests() const { return m_numberOfFramesAtRowOfShapeTests == m_frames.size(); }
    size_t& numberOfFramesAtRowOfShapeTests() { return m_numberOfFramesAtRowOfShapeTests; }
    struct ClosedArmOfRowOfShapeTests {
        BlockID block;
        ValueID firstOperationOfArms;
        size_t depthOfCopies { 0 };
    };
    ClosedArmOfRowOfShapeTests& closedArmOfRowOfShapeTests() { return m_closedArmOfRowOfShapeTests; }
    BlockID closedArmOfRowOfShapeTestsAt(ValueID firstOperation) const
    {
        bool standsAtHeadOfArm = m_closedArmOfRowOfShapeTests.block && m_closedArmOfRowOfShapeTests.firstOperationOfArms == firstOperation && m_closedArmOfRowOfShapeTests.depthOfCopies == depthOfCopies();
        return standsAtHeadOfArm ? m_closedArmOfRowOfShapeTests.block : BlockID();
    }
    Origin originOfOperation() const { return m_origin; }
    ALWAYS_INLINE FunctionID currentFunction() const { return m_frames.last().function; }
    Reference findEnclosingReceiver(Reference receiver) const;
    bool callsEnclosingReceiver(FunctionID callee, LayoutID receiver) const;
    bool callsEnclosingReceiver(FunctionID callee, Reference receiver) const;
    bool isInCallbackCopy() const { return m_frames.last().isInCallbackCopy; }
    ALWAYS_INLINE bool summaryListsEscapes() const
    {
        const Frame& frame = m_frames.last();
        if (!frame.summaryListsEscapes) [[unlikely]]
            frame.summaryListsEscapes = summaryListsEscapes(frame.function);
        return *frame.summaryListsEscapes;
    }
    bool summaryListsEscapes(FunctionID) const;
    const Body& currentBody() const;
    ALWAYS_INLINE const BodyIndex& currentBodyIndex() const { return *m_frames.last().index; }
    ALWAYS_INLINE BlockID inputBlockOf(ValueID input) const
    {
        const Frame& frame = m_frames.last();
        return frame.block && frame.index->isInBlock(input, frame.block) ? frame.block : frame.index->blockOf(input);
    }
    Vector<uint16_t>& operatorsFoundBehindValuesForGuesses() { return m_frames.last().operatorsFoundBehindValuesForGuesses; }
    ALWAYS_INLINE uint32_t loopDepthInCodeOf(BlockID block) const
    {
        const Frame& frame = m_frames.last();
        if (frame.blockOfLoopDepth != block) {
            frame.blockOfLoopDepth = block;
            frame.loopDepth = frame.index->loopDepthOf(block);
        }
        uint32_t depth = frame.loopDepth;
        bool isWrittenOut = depth && frame.fullyUnrolledHeader && frame.index->isInLoop(block, frame.index->loopOf(frame.fullyUnrolledHeader));
        return depth - isWrittenOut;
    }
    ALWAYS_INLINE bool isInLoop() const
    {
        const Frame& frame = m_frames.last();
        if (frame.loopDepthAtCall)
            return true;
        if (frame.blockOfLoopDepth == frame.block && !frame.loopDepth) [[likely]]
            return false;
        return isInLoopOfBodyAtWork();
    }
    ALWAYS_INLINE bool isInLoopOfBodyAtWork() const
    {
        const Frame& frame = m_frames.last();
        return frame.index && frame.block && loopDepthInCodeOf(frame.block) > (frame.isInPreheader ? 1u : 0u);
    }
    bool walksCopyForWhatIsNoBuiltin() const { return m_walksCopyForWhatIsNoBuiltin; }
    bool textNamesHolderThatIsNoBuiltin(ValueID written) const { return m_walksCopyForWhatIsNoBuiltin && textNamesHolderOfCopyAtWork(written); }
    bool isHolderThatIsNoBuiltin(Reference value) const { return m_walksCopyForWhatIsNoBuiltin && isHolderOfCopyAtWork(value); }
    bool textNamesHolderOfCopyAtWork(ValueID written) const;
    bool isHolderOfCopyAtWork(Reference) const;
    bool isBeingWalked(FunctionID) const;
    bool isBeingConstructed(FunctionID) const;
    bool isCurrentFrameRecursive() const { return isFrameRecursive(m_frames.last()); }
    bool isOutsideLoopsOfAsyncBody() const;
    bool runsInIterations() const { return isInLoop() || facts().linked().functionsCalledInLoopsOfOwnFile.get(unit().function.index()); }
    bool isInRepeatedCode() const { return isInLoop() || isCurrentFrameRecursive() || (currentFunction() && facts().linked().function(currentFunction()).isIterationCallback); }
    void leaveCompletedDelegation(BlockID inputHead, ValueID step, Reference hasCompleted, Reference result);
    TypeID backEdgeCheckType(BlockID inputHead, uint32_t parameter) const;
    bool needsBackEdgeRecheck(BlockID head, uint32_t parameter, Reference loopCarriedValue);
    TypeID typeAskedOfWhatOtherCodeBringsRound(BlockID head, TypeID ofParameter, TypeID comingRound);
    size_t depthOfCopies() const { return m_frames.size() - 1; }
    bool isLoopBeingVersioned(LoopID loop) const { return m_frames.last().loopsBeingVersioned.contains(loop); }
    struct CallOfCopy {
        FunctionID caller;
        const BodyIndex* index { nullptr };
        ValueID call;
    };
    std::optional<CallOfCopy> callOfCopy(size_t framesOutward = 0) const;
    Reference referenceInCaller(ValueID value, size_t framesOutward = 0) const { return referenceBoundIn(m_frames[m_frames.size() - 2 - framesOutward], value); }
    std::pair<FunctionID, ValueID> callThatFrameAtWorkCopies() const { return m_frames.size() < 2 ? std::pair<FunctionID, ValueID>() : std::pair<FunctionID, ValueID>(m_frames[m_frames.size() - 2].function, m_frames.last().callCopied); }
    enum class UseOfTextAsKey : uint8_t { NotKnown, None, AnyKey, NameOfProperty };
    UseOfTextAsKey useOfAnswerOfCopyAsKey() const { return m_frames.last().useOfAnswerAsKey; }
    bool walksOwnBodyOfUnit() const { return m_frames.size() == 1; }
    bool walksSpecializedBody() const { return m_frames.size() == 2 && m_frames[1].isSpecializedBody; }
    bool isInRecursiveCopy() const { return m_frames.size() >= 2 && (m_frames.last().isSpecializedBody || m_frames.last().canCallInstead); }
    BlockID elementGuardFallback();
    bool canGuardRecursion();
    bool shouldSpecializeBody();
    void walkSpecializedBody();
    TypeID typeExpectedOfAnswerOfCopy() const { return m_frames.last().typeExpectedOfAnswer; }
    struct ExpectationOfArgument {
        uint32_t frameOfCopy { 0 };
        uint32_t entryParameter { 0 };
        TypeID type;
        bool isDeclared { false };
    };
    bool someCopyExpectsSomethingOfArguments() const { return !m_expectedOfArgumentsOfCopies.isEmpty(); }
    const ExpectationOfArgument* expectedOfArgumentOfCopy(uint32_t entryParameter) const
    {
        for (const ExpectationOfArgument& expected : m_expectedOfArgumentsOfCopies) {
            if (expected.frameOfCopy + 1 == m_frames.size() && expected.entryParameter == entryParameter)
                return &expected;
        }
        return nullptr;
    }
    bool answerOfCopyIsDeclared() const { return m_frames.last().answerIsDeclared; }
    bool copyHandsBack(ValueID);
    bool questionOfHeadWasAskedAbove(const BodyValue& call, FunctionID callee, const BitVector& inlinedHeadBlocks);
    struct QuestionAskedByCall {
        ValueID object;
        PropertyKeyID key;
        int64_t mask { 0 };
        ValueID call;
    };
    Reference nameNarrowedBehindTests(Reference) const;
    bool callCopiedAloneTakesWhatWasMadeForIt(Reference) const;
    bool callCopiedAloneTakesRegExp(Reference) const;
    bool carriesSubstring(const BodyIndex&, BlockID inputHead, uint32_t position);
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
    size_t numberOfValuesThatBirthListsFromBehind() const { return m_valuesWrittenBehindBirth.size(); }
    bool wasBuiltInCurrentFrame(ValueID value) const { return m_frames.size() == 1 || value.index() >= m_frames.last().numberOfValuesBuiltBefore; }
    struct ValueDefinedBehindBirth {
        FieldIndex field;
        Reference value;
    };
    Vector<ValueDefinedBehindBirth, 8> takeWhatIsDefinedBehindBirthOf(LayoutID layout) { return layout == m_layoutOfBirthThatListsWhatIsBehind ? std::exchange(m_valuesDefinedBehindBirth, { }) : Vector<ValueDefinedBehindBirth, 8>(); }
    BlockID currentBlock() const { return m_block; }
    static uint16_t initialPrimitiveOf(const Program&, VariableID);
    uint32_t inlineFrameAtWork() const { return m_frames.last().inlineFrame; }
    bool isInGeneratorConsumer() const { return !m_generatorsWalkedInConsumers.isEmpty(); }
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
    struct Binding {
        uint32_t frame { 0 };
        ValueID input;
        Reference previous;
    };
    void leaveQuestionOfTruthToItsOwnTests(ValueID question)
    {
        m_questionOfTruthLeftToItsOwnTests = question;
        m_frameOfThatQuestionOfTruth = static_cast<uint32_t>(m_frames.size());
    }
    bool takeQuestionOfTruthLeftToItsOwnTests(ValueID question) { return std::exchange(m_questionOfTruthLeftToItsOwnTests, ValueID()) == question && m_frameOfThatQuestionOfTruth == m_frames.size(); }
    Reference referenceBoundTo(ValueID input) const { return referenceBoundIn(m_frames.last(), input); }
    Reference nameLastBoundTo(ValueID input) const { return m_frames.last().lastNameOf(input); }
    ALWAYS_INLINE void bind(ValueID input, Reference reference)
    {
        Frame& frame = m_frames.last();
        Reference& bound = frame.references.placeOf<NamesOfValues::Writer::LogsWhatStoodThere>(input.index());
        if (m_numberOfClosedArms || m_trialVisitDepth) [[unlikely]]
            m_bindings.append(Binding { static_cast<uint32_t>(m_frames.size() - 1), input, bound });
        if (frame.duplicatedValues) [[unlikely]]
            frame.duplicatedValues->noteBoundOnWay(input, reference);
        bound = reference;
    }
    class StandIn {
        WTF_MAKE_NONCOPYABLE(StandIn);
    public:
        StandIn(Walker& walker, ValueID input, Reference reference)
            : m_walker(walker)
            , m_kept { static_cast<uint32_t>(walker.m_frames.size() - 1), input, walker.referenceBoundTo(input) }
        {
            walker.m_valuesStoodInFor.append(m_kept);
            walker.bind(input, reference);
        }
        ~StandIn()
        {
            m_walker.m_valuesStoodInFor.removeLast();
            if (m_kept.frame + 1 == m_walker.m_frames.size())
                m_walker.bind(m_kept.input, m_kept.previous);
        }

    private:
        Walker& m_walker;
        Binding m_kept;
    };
    Reference nameBehindStandIn(size_t frameIndex, ValueID input, Reference bound) const
    {
        if (m_inputs.options.handlerEdgesTakeWhatStandsIn) [[unlikely]]
            return bound;
        for (const Binding& kept : m_valuesStoodInFor) {
            if (kept.frame == frameIndex && kept.input == input) [[unlikely]]
                return kept.previous;
        }
        return bound;
    }
    bool standsInForValueOfFrame() const { return m_valuesStoodInFor.containsIf([&](const Binding& kept) { return kept.frame + 1 == m_frames.size(); }); }
    ALWAYS_INLINE TypeID ownTypeOf(Reference reference) const { return reference && reference.isRecord() ? m_code.at(reference.id()).type : TypeID(); }
    ALWAYS_INLINE TypeID typeOf(Reference reference) const
    {
        if (hasStringsInBuilders()) [[unlikely]]
            return typeOfTextOrRecord(reference);
        return typeAsBuilt(reference);
    }
    ALWAYS_INLINE bool hasUninformativeType(Reference reference) const { return reference && reference.isValue() && m_types.isUninformative(typeOf(reference)); }
    ALWAYS_INLINE Representation representationOf(Reference reference) const
    {
        if (!reference || !reference.isRecord())
            return Representation::Unchosen;
        return m_code.at(reference.id()).representation;
    }
    void letLeave(Reference reference)
    {
        if (unitHasMadeScopes() && !m_trialVisits.isEmpty())
            noteThatWalkLetsScopesNamedLeave(reference);
        letLeave(m_state, reference);
    }
    void noteThatWalkLetsScopesNamedLeave(Reference);
    void noteThatWalkLetsScopeLeave(Reference scope);

    ALWAYS_INLINE BodyValue asWalked(BodyValue written)
    {
        if (accessesUnderComputedKeys.contains(written.opcode())) [[unlikely]] {
            const BodyValue access = written;
            return accessAsWalked(access);
        }
        return written;
    }

    struct IndexedIteration {
        enum class Contents : uint8_t { List, View, Map, Set, Text };
        Reference list;
        Reference arrayBeforeJoin;
        Reference position;
        Reference table;
        Reference state;
        Reference iterator;
        Reference next;
        ValueID literal;
        Vector<Reference, 4> precomputedSteps;
        bool hasPrecomputedSteps { false };
        Reference restList;
        ValueID restArray;
        bool stepsOfPatternStandInRow { false };
        BlockID inputHead;
        BlockID head;
        uint32_t inlineFrame { 0 };
        BuiltinClassID builtinClass;
        Contents contents { Contents::List };
        JSC::IterationKind kind { JSC::IterationKind::Values };
        bool hasProtocolFallback { false };
        bool canSkipDoneChecks { false };
        Reference wayToStep;
        bool stepsMatches { false };
        Reference goesByDriver;
        Reference stepsGenerator;
        bool isProtocolOnly { false };
        bool isAsync { false };
        bool isSteppedByCallsAlone { false };
        bool stateHoldsVerdictOnNext { false };
        Reference listMadeForState;
        bool isOverFlatEntries { false };
        Reference valueBesideKey;
        bool isEntryPairElided { false };
        Reference keyOfPair;
        Reference valueOfPair;
        enum class OtherForm : uint8_t {
            None,
            Enumeration,
            LoopOverInstance,
            PiecesOfSplit,
            MatchesOfRegExp,
        };
        OtherForm otherForm { OtherForm::None };
        Reference subject;
        Reference count;
        ValueID newestStep;
        Reference foundAt;
        Reference keyForAccesses;
        bool isAsMadeForGood { false };
        bool listHasNoHoles { false };
        bool isDeadAfterLoop { false };
        bool isStringOrArray { false };
        bool isArrayOrMap { false };
        bool isDelegating { false };
        LayoutID subclass;
        void forEachValueHeld(NOESCAPE const Invocable<void(Reference)> auto& functor) const
        {
            for (Reference value : { list, position, table, state, iterator, next, wayToStep, goesByDriver, stepsGenerator, valueBesideKey, keyOfPair, valueOfPair, subject, count, foundAt, keyForAccesses, restList })
                functor(value);
            for (Reference value : precomputedSteps)
                functor(value);
        }
        bool isIndexedArrayIteration() const { return !hasProtocolFallback && !hasPrecomputedSteps && !isOverFlatEntries && !literal && !table && contents == Contents::List && otherForm == OtherForm::None; }
        bool isIndexedTypedArrayIteration() const { return list && !hasProtocolFallback && !hasPrecomputedSteps && !isOverFlatEntries && !literal && !table && contents == Contents::View && otherForm == OtherForm::None; }
        bool isIndexedStringIteration() const { return list && !hasProtocolFallback && !hasPrecomputedSteps && !isOverFlatEntries && !literal && !table && contents == Contents::Text && otherForm == OtherForm::None; }
        TypeID keysProven;
        TypeID valuesProven;
        TypeID elementsProven;
    };
    static constexpr int64_t doneIterationPosition = int64_t { 1 } << 35;
    IndexedIteration& indexedIteration(Reference state) LIFETIME_BOUND { return m_indexedIterations[state.object().index()]; }
    Reference addIndexedIteration(IndexedIteration&&);
    struct CopiedCollection {
        uint32_t inlineFrame { 0 };
        ValueID call;
        BuiltinClassID builtinClass;
        IndexedIteration::Contents contents { IndexedIteration::Contents::Set };
        JSC::IterationKind kind { JSC::IterationKind::Values };

        friend bool operator==(const CopiedCollection&, const CopiedCollection&) = default;
    };
    void addCopiedCollection(const CopiedCollection& copied) { m_copiedCollections.appendIfNotContains(copied); }
    std::span<const CopiedCollection> copiedCollections() const LIFETIME_BOUND { return m_copiedCollections.span(); }
    std::optional<bool>& isArrayIteratorProtocolIntact() { return m_isArrayIteratorProtocolIntact; }
    ALWAYS_INLINE bool objectPrototypeMayGainName() const
    {
        if (!m_objectPrototypeMayGainName) [[unlikely]]
            m_objectPrototypeMayGainName = Typed::objectPrototypeMayGainName(program());
        return *m_objectPrototypeMayGainName;
    }

    FunctionID calleeListedAt(FunctionID, ValueID site) const;
    Draft draftOf(const BodyValue&);
    Reference build(Draft&&, const Effects* foundByCaller = nullptr);
    Reference buildTestOfCheck(Draft&&);
    bool comesFromInFrontOfLoopAround(Reference) const;
    bool wasBuiltInPlaceSince(uint32_t numberOfRecordsInFront, Reference) const;
    void checkThatMemoryAnswersAsBelieved(Reference believed, Draft&& readAgain);
    void checkThatValuesAreTheSame(Reference found, Reference believed, Origin);
    Reference constant(Draft&&);
    Reference constant(PrimitiveKnownToFolding);
    Reference onlyValueOfType(TypeID);
    Reference unspecifiedConstant(TypeID);
    void storeParametersNoLongerCaptured();
    ALWAYS_INLINE void storeValueBehindItsDefinition(ValueID definition)
    {
        if (!m_storesOfCapturedValues.empty() && m_frames.last().isOfUnit) [[unlikely]]
            storeValueBehindItsDefinitionInMaker(definition);
    }
    Reference cellThatRunTimeKeeps(Draft&);
    bool copy(const BodyValue&);
    static constexpr OpcodeSet opcodesThatNeedChoices = Operation::opcodesWithForm | OpcodeSet {
        Opcode::Data, Opcode::NewClosure, Opcode::NewObject, Opcode::NewArray, Opcode::ForInOpen, Opcode::ForInStep, Opcode::ResultAt, Opcode::CallDirect,
        Opcode::TryPutByValDirect, Opcode::TryFillNewArray, Opcode::GetSuperBase
    };
    ALWAYS_INLINE void chooseConservativeFields(Draft& draft)
    {
        if (opcodesThatNeedChoices.contains(draft.opcode))
            chooseConservativeFieldsOfOpcodeThatNeedsChoices(draft);
    }
    void chooseConservativeFieldsOfOpcodeThatNeedsChoices(Draft&);

    BlockID addBlock(BlockFrequency = BlockFrequency::Normal);
    void jumpTo(BlockID next, std::span<const Reference> arguments = { });
    void branch(Reference condition, BlockID taken, BlockID notTaken, bool shouldRecordEdgeOutcomes = false);
    void branchOnTest(Reference test, BlockID taken, BlockID notTaken) { branch(test, taken, notTaken, true); }
    std::optional<PassedTest> continueIfCheckPasses(Reference test, BlockID whereItFails, bool passesWithYes = true);
    std::optional<PassedTest> findPassedTest(Reference test, bool passesWithYes = true);
    bool stillHolds(const PassedTest&);
    std::optional<PassedTest> findUpperBoundProof(Reference value, Reference bound);
    std::optional<PassedTest> findNonNegativeProof(Reference value);
    std::optional<PassedTest> findWhatHoldsPositionInsideList(Reference list, Reference position);
    std::optional<PassedTest> findWhatHoldsPositionInsideListMadeInSight(Reference list, Reference position);
    std::optional<PassedTest> findWhatHoldsWordInsideListMadeByWalk(Reference listInParameter, Reference listAsMade, Reference word);
    std::optional<PassedTest> boundsProofAfterComparison(Reference list, Reference position, Reference length, Reference lengthCheck);
    std::optional<PassedTest> boundsProofAfterExtension(Reference list, Reference position, Reference raise);
    std::optional<PassedTest> showElementPresentBehindTest(Reference list, Reference position, const std::optional<PassedTest>&);
    std::optional<PassedTest> findElementFoundAfter(Reference list, Reference position);
    std::optional<PassedTest> showPositionInside(const GroundsOfPosition&);
    void noteLoweredCounter(const BodyValue& access);
    void noteLoweredCounterAfterLoop(ValueID counter, BlockID head, size_t place);
    std::optional<PassedTest> findLoweredCounterProof(Reference view, Reference position, Reference length);
    std::optional<PassedTest> findSteppedKeyProof(Reference list, Reference position, Reference length);
    std::optional<PassedTest> provePositionAfterCounterStart(Reference list, Reference position, Reference length);
    void noteSteppedKeyLimits(size_t placeOfTrial, ValueID test, bool isOnTakenEdge);
    Reference findLoweredCounterStart(Reference position, int64_t& offset) const;
    bool isUnderLoweredCounter(Reference position) const
    {
        int64_t offset = 0;
        return !!findLoweredCounterStart(position, offset);
    }
    std::optional<PassedTest> boundsProofFromChecks(Reference view, Reference position, Reference length, const PassedTest& fromZeroUp, const PassedTest& below);
    std::optional<PassedTest> showPositionInsideBehindTestOfSum(Reference list, Reference position, Reference length);
    void switchOn(Reference subject, std::span<const int64_t> cases, std::span<const BlockID> targets, BlockID otherwise);
    void switchOnString(Reference subject, std::span<const StringID>, std::span<const BlockID> targets, BlockID otherwise, Origin);

    Reference tryCallInline(TerminatorDraft&&, BlockID done, BlockID miss, const Effects* foundByCaller = nullptr);
    void continueIn(BlockID);
    void leaveBlockPending();
    void releaseEndedPendingBlocks();
    Reference addParameter(BlockID, TypeID, Representation);
    Representation formOfParameter(TypeID, std::span<const Representation> arguments);
    struct JoinArm {
        BlockID last;
        Vector<Reference, 8> values;
    };
    static constexpr uint32_t mergesNoObjects = UINT32_MAX;
    bool joinArms(std::span<const JoinArm>, BlockID meeting, std::span<Reference> met, uint32_t firstRecordOfArms = mergesNoObjects);
    struct StretchInArms {
        BlockID inputBlock;
        uint32_t begin { 0 };
        uint32_t end { 0 };
        bool isReturnedAtOnce { false };
        Vector<ValueID, 8> mentionedBehind;
        uint32_t firstRecord { 0 };
    };
    using ArmsThatMeet = Vector<JoinArm, 4>;
    StretchInArms stretchInArms(BlockID inputBlock, uint32_t begin, uint32_t end) const;
    void repeatOperation(BlockID inputBlock, uint32_t place);
    void listArm(const StretchInArms&, ArmsThatMeet&);
    void walkClosedArm(BlockID closed, const StretchInArms&, ArmsThatMeet&);
    bool joinArms(const StretchInArms&, const ArmsThatMeet&, BlockID meeting);
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
    bool unitReturnsAtOnce(const Draft& draft) const { return draft.resultIsReturnedAtOnce && (walksOwnBodyOfUnit() || m_frames.last().returnsInPlaceOfUnit); }
    void returnAtOnce(Reference call);

    void walkStretch(BlockID inputBlock, uint32_t begin, uint32_t end);
    bool hasWalkedBeyond(const BodyValue& operation) const;
    struct StaticFieldSeenInVariable {
        VariableID variable;
        Reference held;
        BlockID blockWhereSeen;
        uint32_t firstOperationBehind { 0 };
        ValueID readThatNamesIt;
        uint32_t inlineFrameOfRead { 0 };
    };
    StaticFieldSeenInVariable*& staticFieldSeenInVariableByArm() { return m_staticFieldSeenInVariableByArm; }
    struct ReadArm {
        size_t frameIndex { 0 };
        BlockID inputBlock;
        ValueID first;
        BlockID asWritten { };
        uint32_t end { 0 };
        uint32_t firstUnseen { 0 };
        bool isLeft { false };
        bool isStraight { false };
        uint32_t nestingDepth { 0 };
        struct PendingStore {
            Reference array;
            Reference position;
            Reference value;
            PassedTest inside;
            Origin origin;
        };
        Vector<PendingStore, 16> pendingStores { };
    };
    ReadArm*& readArm() { return m_readArm; }
    struct InBoundsPath {
        struct Exit {
            uint32_t begin { 0 };
            BlockID slowPath;
            Vector<std::pair<ValueID, Reference>, 8> needed { };
        };
        size_t frameIndex { 0 };
        BlockID inputBlock;
        uint32_t begin { 0 };
        uint32_t end { 0 };
        const StretchInArms* stretch { nullptr };
        Vector<Exit, 8> exits { };
        struct Group {
            VariableID variable;
            ValueID root;
            Reference rootHeld;
            uint32_t shift { 0 };
            Reference first;
            Reference length;
            Range offsets;
            std::optional<PassedTest> fromZeroUp;
            std::optional<PassedTest> below;
        };
        Vector<Group, 4> groups { };
        struct CheckedAccess {
            ValueID access;
            Reference key;
            Reference length;
            std::optional<PassedTest> below;
        };
        CheckedAccess checked { };
    };
    InBoundsPath*& inBoundsPath() { return m_inBoundsPath; }
    uint32_t& coldCopyDepth() { return m_coldCopyDepth; }
    uint32_t nestingDepth() const { return m_nestingDepth; }
    struct BlockCheckedForArithmetic {
        const BodyIndex* index { nullptr };
        BlockID block;
        bool isArithmetic { false };
    };
    BlockCheckedForArithmetic& blockCheckedForArithmetic() { return m_blockCheckedForArithmetic; }
    struct LoopsStoringByName {
        const BodyIndex* index { nullptr };
        BitVector loops { };
        BlockID blockWithoutArm { };
        uint32_t firstReadWithoutArm { 0 };
    };
    LoopsStoringByName& loopsStoringByName() { return m_loopCheckedForStores; }
    size_t currentFrameIndex() const { return m_frames.size() - 1; }
    void walkArmAsWritten(const StretchInArms&, ArmsThatMeet&, NOESCAPE const Invocable<void()> auto&);
    bool isVersioningDryRun() const { return m_isVersioningDryRun; }
    void noteOperatorMadeInFrontOfLoop(ValueID made, Reference name) { m_operatorsMadeInFrontOfLoops.append({ static_cast<uint32_t>(m_frames.size()), made, name }); }
    Reference nameOfOperatorMadeInFrontOfLoop(ValueID operation) const
    {
        for (const OperatorMadeInFrontOfLoop& made : m_operatorsMadeInFrontOfLoops | std::views::reverse) {
            if (made.operation == operation && made.numberOfFrames == m_frames.size())
                return made.name;
        }
        return { };
    }
    bool isMadeInFrontOfLoop(ValueID operation) const { return !!nameOfOperatorMadeInFrontOfLoop(operation); }
    size_t numberOfHoistedOperators() const { return m_operatorsMadeInFrontOfLoops.size(); }
    void forgetOperatorsMadeAhead(size_t kept) { m_operatorsMadeInFrontOfLoops.shrink(kept); }
    struct HoistedRow {
        uint32_t numberOfFrames { 0 };
        BlockID inputHead;
        ValueID read;
        Reference holder;
        Reference key;
        Reference row;
        bool isRejected { false };
        Vector<ValueID, 2> fieldReads;
        Vector<Reference, 1> outerFields;
        Reference fieldBase;
        uint32_t earlierRow { 0 };
        Vector<uint32_t, 2> earlierFields;
    };
    class HoistedRows {
    public:
        bool isEmpty() const { return m_rows.isEmpty(); }
        size_t size() const { return m_rows.size(); }
        std::span<HoistedRow> rows() { return m_rows.mutableSpan(); }
        const HoistedRow* find(ValueID read, size_t numberOfFrames) const
        {
            uint32_t place = m_places.get(placeKey(read, numberOfFrames, false));
            return place ? &m_rows[place - 1] : nullptr;
        }
        const HoistedRow* findByField(ValueID read, size_t numberOfFrames) const
        {
            uint32_t place = m_places.get(placeKey(read, numberOfFrames, true));
            return place ? &m_rows[place - 1] : nullptr;
        }
        void append(HoistedRow&& row)
        {
            uint32_t place = m_rows.size() + 1;
            row.earlierRow = exchangePlace(placeKey(row.read, row.numberOfFrames, false), place);
            row.earlierFields.shrink(0);
            for (ValueID field : row.fieldReads)
                row.earlierFields.append(exchangePlace(placeKey(field, row.numberOfFrames, true), place));
            m_rows.append(WTF::move(row));
        }
        void shrink(size_t size)
        {
            while (m_rows.size() > size) {
                HoistedRow row = m_rows.takeLast();
                for (size_t field = row.fieldReads.size(); field--;)
                    exchangePlace(placeKey(row.fieldReads[field], row.numberOfFrames, true), row.earlierFields[field]);
                exchangePlace(placeKey(row.read, row.numberOfFrames, false), row.earlierRow);
            }
        }

    private:
        static uint64_t placeKey(ValueID read, size_t numberOfFrames, bool isField) { return static_cast<uint64_t>(numberOfFrames) << 33 | static_cast<uint64_t>(isField) << 32 | read.index(); }
        uint32_t exchangePlace(uint64_t key, uint32_t place) { return std::exchange(m_places.add(key, 0).iterator->value, place); }

        Vector<HoistedRow, 2> m_rows;
        HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_places;
    };
    struct FieldArrayRead {
        Reference holder;
        PropertyKeyID key;
        FieldReference field;
        Reference array;
    };
    void addFieldArrayRead(const FieldArrayRead& read) { m_fieldArrayReads.append(read); }
    bool isFieldArrayRead(Reference array) const { return m_fieldArrayReads.containsIf([&](const FieldArrayRead& read) { return read.array == array; }); }
    void noteFieldArrayProof(Reference array, Reference proof);
    void noteFieldArrayHolder(Reference holder, Reference proof);
    std::span<const FieldArrayRead> fieldArrayReads() const { return m_fieldArrayReads.span(); }
    std::span<HoistedRow> hoistedRowsOf(BlockID inputHead)
    {
        std::span<HoistedRow> rows = m_hoistedRows.rows();
        size_t begin = rows.size();
        while (begin && rows[begin - 1].inputHead == inputHead && rows[begin - 1].numberOfFrames == m_frames.size())
            --begin;
        return rows.subspan(begin);
    }
    Reference findHoistedRow(ValueID read) const
    {
        const HoistedRow* row = m_hoistedRows.find(read, m_frames.size());
        return row && !row->isRejected ? row->row : Reference();
    }
    bool hasHoistedFields() const { return m_hasHoistedFields; }
    Reference findHoistedField(ValueID fieldRead) const
    {
        const HoistedRow* row = m_hoistedRows.findByField(fieldRead, m_frames.size());
        if (!row || row->isRejected)
            return { };
        size_t place = row->fieldReads.find(fieldRead);
        return place < row->outerFields.size() ? row->outerFields[place] : row->holder;
    }
    void addHoistedRow(HoistedRow&& row)
    {
        row.numberOfFrames = m_frames.size();
        m_hoistedRows.append(WTF::move(row));
    }
    void threadReturnOf(BlockID inputBlock);
    bool threadBranchOf(BlockID inputBlock);
    Reference buildWithInlineLookup(Draft&&);
    bool canThreadBranchAfter(const BodyValue& operation);
    bool canFollowNotTakenEdge(const BodyTerminator&);
    void followNotTakenEdge(const BodyTerminator&);
    bool canThreadBranchAfterMatch(const BodyValue& operation);
    bool blockEndKeepsStringBuilders(uint32_t begin);
    bool threadBranchAfter(const BodyValue& operation);
    bool canThreadBranchFrom(ValueID first);
    bool threadBranchFrom(ValueID first);
    bool armsMayTakeBranchOf(BlockID inputBlock, uint32_t end, Reference value) const;
    bool armsTakeBranchForProofOf(BlockID inputBlock, uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter);
    uint32_t endOfStretchBeingWalked() const { return m_frames.last().endOfStretch; }
    uint32_t beginOfStretchBeingWalked() const { return m_frames.last().beginOfStretch; }
    bool isAccessorDefinedByNext(ValueID definition) const
    {
        const Frame& frame = m_frames.last();
        return definition && frame.lastAccessorDefinedByNext && frame.firstAccessorDefinedByNext.index() <= definition.index() && definition.index() <= frame.lastAccessorDefinedByNext.index();
    }
    void noteAccessorDefinedByNext(ValueID definition, ValueID precedingNeighbor)
    {
        Frame& frame = m_frames.last();
        if (!isAccessorDefinedByNext(precedingNeighbor))
            frame.firstAccessorDefinedByNext = definition;
        frame.lastAccessorDefinedByNext = definition;
    }
    void noteAccessorDefinedItself(ValueID definition, ValueID precedingNeighbor)
    {
        if (isAccessorDefinedByNext(definition))
            m_frames.last().lastAccessorDefinedByNext = isAccessorDefinedByNext(precedingNeighbor) ? precedingNeighbor : ValueID();
    }
    bool armHoldsKeyOf(ValueID access) const;
    bool isWalkingForThisWay(BlockID inputBlock) const { return m_frames.last().block == inputBlock && isTailDuplicatingBlock(m_frames.size() - 1); }
    BlockID unrolledLoopHeader() const { return m_frames.last().fullyUnrolledHeader; }
    bool isSteppedUpOnBackEdge(ValueID counter, NOESCAPE const Invocable<bool()> auto& ask)
    {
        return m_steppedCounters.ensure(std::pair { &currentBodyIndex(), counter.index() }, ask).iterator->value;
    }
    template<typename Ask> uint8_t zeroSignUse(const BodyIndex& index, ValueID value, const Ask& ask)
    {
        return m_unsignedZeroValues.ensure(std::pair { &index, value.index() }, ask).iterator->value;
    }
    bool turnWrittenOutHoldsKeyOf(ValueID access, LoopID loopOfKey) const { return m_valueBeingWalked == access && m_frames.last().fullyUnrolledHeader && m_frames.last().index->loopOf(m_frames.last().fullyUnrolledHeader) == loopOfKey; }
    bool mayWalkMethodGenerator(const BodyValue& read, FunctionID method);
    void walkIterationAsWritten(BlockID start, const BodyValue& read);
    Vector<std::pair<uint32_t, ValueID>, 2> m_generatorMethodsTested;
    ValueID m_generatorReadTested;
    bool walksStretchInsideAnother() const { return m_frames.last().numberOfStretchesBeingWalked > 1 || isTailDuplicatingBlock(m_frames.size() - 1); }
    bool someFrameWalksStretchInsideAnother() const
    {
        for (size_t frame = 0; frame < m_frames.size(); ++frame) {
            if (m_frames[frame].numberOfStretchesBeingWalked > 1 || isTailDuplicatingBlock(frame))
                return true;
        }
        return false;
    }
    struct PlaceTakenByEndOfBlock {
        BlockID block;
        uint32_t place { 0 };
    };
    PlaceTakenByEndOfBlock& placeTakenByEndOfBlockAskedLast() { return m_frames.last().placeTakenByEndOfBlockAskedLast; }
    void recordSpeculation() { ++m_speculationCount; }
    bool isInTrialVisit() const { return !m_trialVisits.isEmpty(); }
    bool isInTrialVisitOfCurrentFrame() const { return !m_trialVisits.isEmpty() && m_trialVisits.last().frameIndex == m_frames.size() - 1; }
    void noteTypeOfSumLeftInArm(ValueID sum);
    void noteWhetherOperationWritesWhatTurnHasMade(bool writesThatAlone) { m_operationWritesWhatTurnHasMade = writesThatAlone; }
    struct ReadWhoseGuessIsOpen {
        size_t depthOfCopies { 0 };
        ValueID read;
        bool operator==(const ReadWhoseGuessIsOpen&) const = default;
    };
    ReadWhoseGuessIsOpen& readWhoseGuessIsOpen() { return m_readWhoseGuessIsOpen; }
    Reference& guardedReadArray() { return m_guardedReadArray; }
    bool& isInArrayArms() { return m_isInArrayArms; }
    struct MethodChain {
        size_t depthOfCopies { std::numeric_limits<size_t>::max() };
        ValueID openRead;
        ValueID lastCall;
    };
    MethodChain& currentMethodChain() { return m_currentMethodChain; }
    void recordHoistedArrayGuard() { ++m_hoistedArrayGuardCount; }
    void noteLengthOfViewAskedForWrittenPlace(Reference view);
    void noteListFromInFrontTakenApart(Reference list);
    void recordHoistedElementCheck() { ++m_hoistedElementCheckCount; }
    void recordHoistedNumberRead() { ++m_hoistedNumberReadCount; }
    void recordNumberReadNearStores() { ++m_numberReadNearStoresCount; }
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
        bool restsOnNamesUsedAlone { false };
    };
    RememberedExpectation& rememberedExpectationOf(ValueID);
    TypeID shapeRememberedOf(ValueID value) const
    {
        const auto& shapes = m_frames.last().shapesOfValues;
        auto found = shapes.isEmpty() ? shapes.end() : shapes.find(value.index());
        return found == shapes.end() ? TypeID() : TypeID::fromBits(found->value);
    }
    bool someValueOfFrameHasShape() const { return !m_frames.last().shapesOfValues.isEmpty(); }
    void rememberShapeOf(ValueID value, TypeID shapedLike) { m_frames.last().shapesOfValues.set(value.index(), shapedLike.bits()); }
    void forgetShapeOf(ValueID value)
    {
        if (auto& shapes = m_frames.last().shapesOfValues; !shapes.isEmpty())
            shapes.remove(value.index());
    }
    TypeID closuresThatCopiedCallHandsByItsText(uint32_t entryParameter) const;
    ALWAYS_INLINE RememberedExpectation expectationRememberedOf(ValueID value) const
    {
        const auto& remembered = m_frames.last().expectations;
        return remembered.isEmpty() ? RememberedExpectation { } : remembered[value.index()];
    }
    std::optional<RememberedExpectation> wholeExpectationKeptOf(ValueID parameter) const
    {
        const auto& kept = m_frames.last().wholeExpectations;
        auto found = kept.isEmpty() ? kept.end() : kept.find(parameter.index());
        return found == kept.end() ? std::nullopt : std::optional(found->value);
    }
    void keepWholeExpectationOf(ValueID parameter, RememberedExpectation whole) { m_frames.last().wholeExpectations.set(parameter.index(), whole); }
    ValueID whereUsesOfTextEnd(ValueID text, bool goOnThroughJoins) const
    {
        const auto& ends = m_frames.last().endsOfUsesOfTexts;
        auto found = ends.isEmpty() ? ends.end() : ends.find(text.index() * 2 + goOnThroughJoins);
        return found == ends.end() ? ValueID() : ValueID(found->value);
    }
    void noteWhereUsesOfTextEnd(ValueID text, bool goOnThroughJoins, ValueID end) { m_frames.last().endsOfUsesOfTexts.set(text.index() * 2 + goOnThroughJoins, end.index()); }
    Vector<ValueID, 16>* expectationsRememberedForLook() { return m_expectationsRememberedForLook; }
    void setExpectationsRememberedForLook(Vector<ValueID, 16>* list) { m_expectationsRememberedForLook = list; }
    const ScopedLambda<TypeID(ValueID)>* typesOfInputsInLook() const { return m_typesOfInputsInLook; }
    void setTypesOfInputsInLook(const ScopedLambda<TypeID(ValueID)>* types) { m_typesOfInputsInLook = types; }
    bool lookHasAskedAboutUnbuiltValue() const { return m_lookHasAskedAboutUnbuiltValue; }
    void setLookHasAskedAboutUnbuiltValue(bool has) const { m_lookHasAskedAboutUnbuiltValue = has; }
    std::span<const PropertyKeyID> namesUsedOff(ValueID);
    using NamesGatheredForRoots = HashMap<std::pair<const KeptWithBodyIndex*, uint32_t>, Vector<PropertyKeyID>>;
    NamesGatheredForRoots& namesGatheredForRoots() { return m_namesGatheredForRoots; }
    bool appendNamesUsedOffViewsOf(ValueID, Vector<PropertyKeyID, 16>&);
    std::span<const uint64_t> castsOf(ValueID);
    enum class RoundKeptByWalker : uint8_t { OnlyLayoutWithAllNames, ShapesOfLayouts, DescribesBagOfOptions, OnlyLayoutWithName, RecordsThatFileWrites, LiteralsWithoutNames, AreAllOfCreatedObjects, PrivateNameHolders, MemberLacksNames };
    static bool keepsRoundsOver(const TypeArena&, LayoutSetID);
    std::optional<uint32_t> roundKept(RoundKeptByWalker, LayoutSetID, uint32_t second) const;
    void keepRound(RoundKeptByWalker, LayoutSetID, uint32_t second, uint32_t answer);
    uint32_t numberOfListOfNames(std::span<const PropertyKeyID>);
    std::optional<uint32_t> numberOfLongListOfNamesUsedOff(ValueID, std::span<const PropertyKeyID>);
    bool everyNameOfLongListIsPlainFieldOf(uint32_t numberOfList, LayoutID, NOESCAPE const ScopedLambda<bool()>& ask);
    std::span<const LayoutID> fewInstancesInSightWithAllNames(std::span<const PropertyKeyID>);
    LayoutID onlyLayoutInSightWithAllNames(std::span<const PropertyKeyID>, bool recordsAnswer, LayoutID beside = { }, std::optional<uint32_t> numberOfLongList = std::nullopt);
    std::span<const BodyIncomingEdge> incomingEdgesOf(const BodyIndex&, BlockID);
    struct KeptForInt32Promises;
    KeptForInt32Promises& keptForInt32Promises();
    uint32_t speculationCount() const { return m_speculationCount; }
    uint32_t versionedLoopCount() const { return m_versionedLoopCount; }
    bool exchangeMakesLetterOfStep(bool makes) { return std::exchange(m_makesLetterOfStep, makes); }
    uint32_t loopVisitCount() const { return m_loopVisitCount; }
    uint32_t undoneLoopVisitCount() const { return m_undoneLoopVisitCount; }
    bool sumsAreNotAskedInRow() const { return m_sumsAreNotAskedInRow; }
    uint32_t unguardedSumLoopCount() const { return m_unguardedSumLoops.size(); }
    Reference elementReadAhead(ValueID read) const;
    Reference boundElementReadBy(const BodyValue& read, Reference view);
    bool isInVersionForOtherClass() const { return m_otherClassBelowMixin.others && isInUnswitchedVersion(); }
    TypeID speculatedWithOtherClass(Reference value, TypeID expected) const { return m_otherClassBelowMixin.others && value == m_otherClassBelowMixin.value && expected == m_otherClassBelowMixin.heirs ? m_otherClassBelowMixin.others : expected; }
    bool keysAreReadOffLookInRow() const { return m_keysAreReadOffLookInRow || m_walksCopyThatAsksCountersAlone; }
    bool hoistsConditionalArrays() const { return m_hoistsConditionalArrays; }
    void noteKeyTakenForCounterAskedInFront() { m_keyWasTakenForCounterAskedInFront = true; }
    struct ComputedComparison {
        ValueID parameter;
        ValueID relation;
        ValueID bound;
    };
    std::span<const ComputedComparison> computedComparisonsOf(ValueID parameter);
    uint32_t numberOfRowsWhoseKeysAreReadOffLook() const { return m_rowsWhoseKeysAreReadOffLook.size(); }
    uint32_t abortedLoopVisitCount() const { return m_abortedLoopVisitCount; }
    uint32_t predictedLoopEffectsCount() const { return m_predictedLoopEffectsCount; }
    uint32_t refutedLoopEffectsCount() const { return m_refutedLoopEffectsCount; }
    uint32_t failedLoopVersioningCount() const { return m_failedLoopVersioningCount; }
    uint32_t expandedObjectAssignCount() const { return m_expandedObjectAssignCount; }
    void recordExpandedObjectAssign() { ++m_expandedObjectAssignCount; }
    struct KeysStoredInFrontOfRow {
        const BodyIndex* body { nullptr };
        ValueID written;
        LayoutID layout;
        BlockID blockOfRow;
        uint32_t numberOfValuesGoneThrough { 0 };
        Vector<PropertyKeyID, 32> keys;
        Vector<FieldIndex, 32> fields;
    };
    KeysStoredInFrontOfRow& keysStoredInFrontOfLastRow() { return m_keysStoredInFrontOfLastRow; }
    using ValuesThatEveryWayHandsToParameters = HashMap<uint64_t, ValueID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    ValuesThatEveryWayHandsToParameters& valuesThatEveryWayHandsToParameters() { return m_valuesThatEveryWayHandsToParameters; }
    struct AccessesOfSlotsInLoop {
        const BodyIndex* body { nullptr };
        LoopID loop;
        HashMap<uint64_t, Vector<ValueID, 2>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> underObjectAndName;
        HashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> turnsTakeForInt32;
    };
    AccessesOfSlotsInLoop& accessesOfSlotsInLoopListedLast() { return m_accessesOfSlotsInLoopListedLast; }
    uint32_t duplicatedJoinCount() const { return m_duplicatedJoinCount; }
    uint32_t threadedConditionCount() const { return m_threadedConditionCount; }
    uint32_t fieldCalleeSpeculationCount() const { return m_fieldCalleeSpeculationCount; }
    void recordFieldCalleeSpeculation() { ++m_fieldCalleeSpeculationCount; }
    uint32_t genericInstanceCheckCount() const { return m_genericInstanceCheckCount; }
    void recordPlainInstanceCheck(bool isAskedOfCell) { m_genericInstanceCheckCount += isAskedOfCell ? -1 : 1; }
    uint32_t genericMemberLookupCount() const { return m_genericMemberLookupCount; }
    uint32_t mergedFreshObjectCount() const { return m_mergedFreshObjectCount; }
    bool isMergedFreshObject(ValueID parameter) const { return m_mergedFreshObjects.get(parameter.index()); }
    void noteWaysApart(uint32_t firstRecord)
    {
        inputBlock(m_frames.size() - 1, m_frames.last().block).firstSeparatePathRecord = firstRecord;
    }
    void noteReleasableArrayMade() { m_hasMadeReleasableArrays = true; }
    void recordMergedFreshObject(ValueID parameter)
    {
        ++m_mergedFreshObjectCount;
        m_mergedFreshObjects.set(parameter.index());
    }
    uint32_t iterationRestartCount() const { return m_iterationRestartCount; }
    uint32_t undoneStoreCount() const { return m_undoneStoreCount; }
    uint32_t emptySlotCaseCount() const { return m_emptySlotCaseCount; }
    void recordEmptySlotCase() { ++m_emptySlotCaseCount; }
    void recordFoldedSlice() { ++m_foldedSliceCount; }
    uint32_t foldedSliceCount() const { return m_foldedSliceCount; }
    struct CountOfUnit {
        StatisticID statistic;
        uint32_t amount;
    };
    void stopAtMaker(Origin);
    void stopWhereWordOfLinkIsFalse(Origin);
    bool constantEndsWhereItLeaves(ValueID written, DataID) const;
    void showThatObjectHoldsWhatIsHeldFilled(Reference object, LayoutID, bool mayBeFilledByCopies = false);
    bool makerLacksWhatIsHeldFilled(const Draft& allocation) { return !allocationHoldsWhatIsHeldFilled(allocation); }
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
    std::span<const CountOfUnit> countsOfUnit() const LIFETIME_BOUND
    {
        ASSERT(m_newestWalkerOfWorker == this);
        return m_countsOfUnit.span();
    }
    uint32_t numberOfValuesWalked() const { return m_numberOfValuesWalked; }
    uint32_t numberOfValuesWalkedAndKept() const { return m_numberOfValuesWalkedAndKept; }
    bool walksBodyWhoseProofsAreChecked() const { return unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody || m_restArrayAllocator; }
    ALWAYS_INLINE bool checksProofs() const { return m_positionsWithoutProofs && m_inputs.options.sourcesOfProofsChecked == everySourceOfProofs && walksBodyWhoseProofsAreChecked(); }
    bool checksProofsOf(ProofSource source) const { return m_positionsWithoutProofs && (m_inputs.options.sourcesOfProofsChecked & bitOfSourceOfProofs(source)); }
    bool checksWalk() const { return m_inputs.options.checksWalk; }
    bool hasUnprovenSecondCounter();
    void refuseSecondCounters() { m_refusesSecondCounters = true; }
    bool verifiesCode() const { return m_inputs.options.verifiesCode; }
    void noteReadAnsweredByHoleReadAsNaN(ValueID read, Reference answer);
    void noteHoleForCaller(const CallOfCopy&, Reference answer);
    void noteValueThatFoldedArithmeticGives(Reference given)
    {
        if (verifiesCode() && given.isValue() && !m_code.inputsOfHolesReadAsNaN().empty()) [[unlikely]]
            m_code.noteValueThatFoldedArithmeticGives(given.id());
    }
    void askGuardsAgainWhereTheyAreLeanedOn(std::span<const MemberConditionRecord>, Origin);
    void askByteOfMemberAgainWhereNoteIsLeanedOn(BuiltinID);
    void verifyPristineObject(Reference object);
    void verifyRememberedTest(Draft&& test);
    void askQuestionOfTruthAgainWhereWaysAnswerIt(const BodyValue& question);
    void askObjectAgainForFunctionThatNameFinds(Reference object, PropertyKeyID, FunctionID, Origin, Reference onlyWhere = { }, bool isGetter = false);
    void askClassAgainForChainAbovePrototype(Reference classObject, FunctionID constructor, Origin, Reference onlyWhere);
    void askObjectAgainForGetterThatNameFinds(const BodyValue& read, Reference object, FunctionID getter);
    bool checksBounds() const { return m_inputs.options.checksBounds; }
    bool shouldAssumeIndexesInBounds() const { return m_inputs.options.shouldAssumeIndexesInBounds; }
    bool shouldDropLengthCheckedReads() const { return m_inputs.options.shouldDropLengthCheckedReads; }
    bool shouldAssumeInvariantLoopBounds() const { return m_inputs.options.shouldAssumeInvariantLoopBounds; }
    bool shouldAssumeInvariantArrayStorage() const { return m_inputs.options.shouldAssumeInvariantArrayStorage; }
    bool shouldMisidentifyHoistedReadOperands() const { return m_inputs.options.shouldMisidentifyHoistedReadOperands; }
    bool shouldAssumeUInt32Dividends() const { return m_inputs.options.shouldAssumeUInt32Dividends; }
    bool shouldMisroundLoopBounds() const { return m_inputs.options.shouldMisroundLoopBounds; }
    bool shouldAssumeGuardsAndFacts() const { return m_inputs.options.shouldAssumeGuardsAndFacts; }
    bool checksHoistedCalls() const { return m_inputs.options.checksHoistedCalls && checksProofs(); }
    const HostStringEncoders* stringEncoders() const { return m_inputs.options.stringEncoders; }
    bool shouldAssumeInlineBuiltinsPure() const { return m_inputs.options.shouldAssumeInlineBuiltinsPure; }
    bool shouldAssumeNoNaNStores() const { return m_inputs.options.shouldAssumeNoNaNStores; }
    bool shouldSkipCapacityChecks() const { return m_inputs.options.shouldSkipCapacityChecks; }
    bool shouldSkipLengthChecks() const { return m_inputs.options.shouldSkipLengthChecks; }
    bool shouldIgnoreLengthStores() const { return m_inputs.options.shouldIgnoreLengthStores; }
    bool shouldIgnorePrototypeReads() const { return m_inputs.options.shouldIgnorePrototypeReads; }
    bool shouldAssumeEnumeratorCurrent() const { return m_inputs.options.shouldAssumeEnumeratorCurrent; }
    bool shouldIgnoreAlternativeOverloads() const { return m_inputs.options.shouldIgnoreAlternativeOverloads; }
    bool shouldIgnoreFreshArrayEscapes() const { return m_inputs.options.shouldIgnoreFreshArrayEscapes; }
    bool shouldOverstateBoundArgumentCount() const { return m_inputs.options.shouldOverstateBoundArgumentCount; }
    std::optional<PassedTest> findNonNegativeDifferenceTest(Reference position);
    std::optional<PassedTest> findWhatHoldsPositionBelowRoomOfView(Reference view, Reference position);
    struct PlacesSeenToStandInStretches {
        const BodyIndex* index { nullptr };
        BlockID block;
        uint32_t from { 0 };
        uint32_t to { 0 };
    };
    PlacesSeenToStandInStretches& placesSeenToStandInStretches() { return m_placesSeenToStandInStretches; }
    uint32_t& numberOfStretchesBehindBytesOfMembers() { return m_numberOfStretchesBehindBytesOfMembers; }
    bool shouldAssumeCalleesRetainNothing() const { return m_inputs.options.shouldAssumeCalleesRetainNothing; }
    bool framesKeepFields() const;
    bool isStableFieldOf(LayoutSetID receivers, PropertyKeyID, bool holdsDeclaredType) const;
    bool readsStableField(const Draft& read) const;
    Reference valueOfStableField(const Draft& read, TypeID proven) const;
    void noteStableField(const Draft& read, Reference value);
    void noteStableProof(Reference object, PropertyKeyID, Reference proof);
    ALWAYS_INLINE void applyUnderstatedEffects(const Draft& draft, Effects& effects) const
    {
        if (m_inputs.options.understatedEffects) [[unlikely]]
            applyUnderstatedEffectsSlow(draft, effects);
    }
    void applyUnderstatedEffectsSlow(const Draft&, Effects&) const;
    bool shouldFailSpeculations() const { return m_inputs.options.shouldFailSpeculations; }
    bool everyClosureStandsBehindCopies() const { return m_inputs.options.everyClosureStandsBehindCopies; }
    bool shouldIgnoreIterationEffects() const { return m_inputs.options.shouldIgnoreIterationEffects; }
    bool netOfEffectsWatchesTurns() const { return m_inputs.options.netOfEffectsWatchesTurns; }
    bool ignoresExpectedTypes() const { return m_inputs.options.ignoresExpectedTypes; }
    bool shufflesExpectedTypes() const { return m_inputs.options.shufflesExpectedTypes; }
    bool judgesInt32Promises() const { return m_inputs.options.judgesInt32Promises && m_way == WalkMode::AsBuilt && !m_frames.containsIf([](const Frame& frame) { return frame.isWalkedWhereItsGuardFailed; }); }
    uint32_t drawFor(DrawnFor question, uint32_t first, uint32_t second) const { return drawBySeed(m_inputs.options.shuffleSeed, question, first, second); }
    enum class ChoiceOfCost : uint8_t { AsItPays, Yes, No };
    bool drawsNoChoices() const { return !m_inputs.options.shufflesChoices; }
    ChoiceOfCost drawChoice(DrawnFor question, uint32_t first, uint32_t second, bool takesYes = true) const
    {
        if (!m_inputs.options.shufflesChoices) [[likely]]
            return ChoiceOfCost::AsItPays;
        ChoiceOfCost drawn = static_cast<ChoiceOfCost>(drawFor(question, first, second) % 3);
        if (drawn == ChoiceOfCost::Yes && !takesYes)
            drawn = ChoiceOfCost::AsItPays;
        if (drawn != ChoiceOfCost::AsItPays)
            const_cast<Walker*>(this)->count(StatisticID::ChoicesOfCostDrawn);
        return drawn;
    }
    Reference drawWhetherGuessFails(Origin);
    void letGuessFailUnasked(BlockID& whereGuessFails, Origin);
    void checkProof(Reference value, TypeWithoutProof, ProofSource, Origin);
    void checkProof(Reference value, TypeID proven, TypeWithoutProof, ProofSource, Origin);
    void checkProofOfCopy(Reference value, TypeID proven, ProofSource, Origin);
    void checkRawNumberThatFoldRestsOn(Reference number, Origin);
    Reference proveOrTrap(Reference value, TypeID tested, ProofSource, Origin);
    TypeID typeThatUnitMayTestFor(TypeID tested, bool mayBringClasses = true, bool asOfUnitWithoutReach = false);
    void checkThatSlotTakes(Reference value, TypeID taken, Origin);
    void holdStoredValuesToSlots(const Draft&);
    void holdStoredValuesToPlaces(const Draft&);
    void proveThatFormOfVariableTakesStoredValue(Draft&);
    void proveThatSlotsOfScopeTakeStoredValues(Draft&);
    void holdToTypeOfPlace(Reference value, TypeID held, ProofSource, Origin, bool isConstantWrittenInBody = false);
    void holdSlotsOfScopesThatLeaveAt(std::span<const SummaryScopeStoredFirst>, uint32_t position, bool standsBehindOperationsOfBlock);
    void holdVariableOfAnotherFileStoredFirst(VariableID);
    void holdVariablesStoredFirstWhereFunctionsAreUsed(std::span<const LinkedVariableStoredFirst> claims, std::span<const FunctionID> used, VariableID storedByOperation);
    void holdSlotStoredFirst(Reference environment, FieldReference, Reference found);
    void holdWhatIsFoundInPlace(Reference found, TypeID held, ProofSource);
    void checkHoistedRead(Reference found, TypeID held, ProofSource);
    bool makeTestOnTheWayAskInFull(Reference value, TypeID taken);
    TypeID typeThatDoorsHoldOfSlotRead(const Operation& read);
    bool makeObjectOfSlotReadShown(Reference value, TypeID taken);
    void buildCheckOfProof(Reference value, TypeID proven, TypeWithoutProof, ProofSource, Origin, bool mayBringClasses = false, bool isNotShownToFit = false, bool isConstantWrittenInBody = false, bool standsInFrontOfStoreOfValue = false);
    void buildCheckThatTestHolds(Reference test, ProofSource, Origin);
    std::optional<Origin> leaveCheckOfWhatContainerHasJustGivenToTestBehind(Reference given);
    void makeTypeTestAskInFull(Reference test) { m_code.makeTypeTestAskInFull(test.id()); }
    BlockID splitNumberFromContainer(Reference given, Reference& number);
    bool splitNullishOffWhatContainerHasJustGiven(Reference given, TypeID rest, Draft&& throwsOffNullish, const Effects* ofThrow = nullptr);
    bool letTestInFrontOfBranchAskWhatContainerHasJustGiven(const BodyValue& test);
    TypeID typeShown(Reference);
    bool isShownAndReadThrough(Reference, TypeID shown, TypeID held);
    bool isShown(Reference value, TypeID within) { return m_types.isSubset(typeShown(value), within); }
    void noteProvenJoinType(Reference join, TypeID type) { setTypeAsked(join.id(), type); }
    TypeID typeThatStoreTakesOnWordOfWalk(const Draft& store);
    bool holdsNaNForUndefined(Reference returned) { return returned && returned.isValue() && m_code.at(returned.id()).opcode == Opcode::GetArrayElement && m_code.at(returned.id()).readsHoleAsNaN(); }
    void checkProofOfResultOfCopy(Reference value, TypeID proven, Origin origin, bool someReturnHoldsNaNForUndefined)
    {
        if (value && value.isValue() && (m_code.at(value.id()).opcode == Opcode::AwaitCallBySavingFrame || m_code.at(value.id()).opcode == Opcode::AwaitBySavingFrame))
            return;
        if (someReturnHoldsNaNForUndefined && proven && (m_types.at(proven).primitives & Primitive::Undefined))
            proven = m_types.unionOf(proven, TypeTable::number);
        checkProofOfCopy(value, proven, ProofSource::ResultOfCall, origin);
    }
    void checkFactsRememberedOfCell(Reference object, Origin, OptionSet<CellFact> leanedOn = { });
    void checkWhatIsInSightOfList(Reference list, Origin);
    uint32_t numberOfProofsChecked(ProofSource source) const { return m_numbersOfProofsChecked[static_cast<unsigned>(source)]; }
    uint32_t untestableProofCount() const { return m_untestableProofCount; }
    uint32_t impliedProofCount() const { return m_impliedProofCount; }
    uint32_t unsatisfiableProofCheckCount() const { return m_unsatisfiableProofCheckCount; }
    uint32_t foldedProofCheckCount() const { return m_foldedProofCheckCount; }
    TypeID typeThatFormSays(Reference value);
    uint32_t skippedFreshCellCount() const { return m_skippedFreshCellCount; }
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
    void noteSlotRead(Reference object, PropertyKeyID, Reference read);
    void noteSlotStore(Reference object, PropertyKeyID, Reference before, Reference stored, bool isFirstFill = false);
    void noteScopeAllocatedBefore(TypeID, uint32_t loop);
    void requestCarriedField(Reference object, PropertyKeyID, Reference before, VariableID staticField = { }, Reference stored = { });
    Reference readCarriedField(const Draft& read, FieldReference);
    Reference readCarriedStaticField(Reference object, VariableID, PropertyKeyID, Reference before, Reference isAbsentBefore, Origin);
    Reference readInitializedStaticField(Reference object, VariableID, PropertyKeyID, Origin);
    bool& closedArmKeepsSites() { return m_closedArmKeepsSites; }
    bool& checksBitsOfUnknownValues() { return m_checksBitsOfUnknownValues; }
    AssignedMemberChoice& assignedMemberChoice() { return m_assignedMemberChoice; }
    AssignedMemberChoice assignedMemberChoice() const { return m_assignedMemberChoice; }
    bool isBuildingInsideLoop() const { return std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft); }
    BlockID lastReturnDominator(const BodyIndex&) const;
    bool isDoneInIterations() const { return isInLoop() || facts().linked().functionsDoneInIterations.get(currentFunction().index()) || facts().linked().functionsDoneInIterations.get(unit().function.index()); }
    std::unique_ptr<KeptForBuild>& keptByTypedAccesses() { return m_keptByTypedAccesses; }
    std::unique_ptr<KeptForBuild>& keptByUnionTableCalls() { return m_keptByUnionTableCalls; }
    std::unique_ptr<KeptForBuild>& keptByCopiesOwnKeys() { return m_keptByCopiesOwnKeys; }
    std::unique_ptr<KeptForBuild>& keptByPlainInstances() const { return m_keptByPlainInstances; }
    std::unique_ptr<KeptForBuild>& keptByInstanceOf() const { return m_keptByInstanceOf; }
    std::unique_ptr<KeptForBuild>& keptByInlining() const { return m_keptByInlining; }
    CoroutineStateOfUnit& coroutineState() { return m_coroutineState; }
    const CoroutineStateOfUnit& coroutineState() const { return m_coroutineState; }
    LoopsOverGeneratorsOfUnit& loopsOverGenerators() { return m_loopsOverGenerators; }
    struct EnvironmentCellsOfUnit {
        bool hasMadeSome { false };
        bool mayNameSome { false };
    };
    EnvironmentCellsOfUnit& environmentCellsOfUnit() { return m_environmentCellsOfUnit; }
    struct EnvironmentMadeInFrame {
        ValueID name;
        BlockID making;
        bool hasSeveralRecords { false };
    };
    Vector<EnvironmentMadeInFrame, 2>& environmentsMadeInFrame() { return m_frames.last().environmentsMade; }
    std::unique_ptr<KeptForBuild>& keptByStrings() { return m_keptByStrings; }
    std::unique_ptr<KeptForBuild>& keptByScalarReplacement() const { return m_keptByScalarReplacement; }
    std::unique_ptr<KeptForBuild>& keptByPositionsInTexts() { return m_keptByPositionsInTexts; }

    std::optional<uint32_t> innermostOpenLoop() const;
    std::optional<uint32_t> constantTripCountOf(uint32_t openLoop) const;
    BlockID headOfOpenLoop(uint32_t indexOfLoop) const { return m_openLoops[indexOfLoop].head; }
    bool standsInFrontOfOpenLoop(uint32_t indexOfLoop, Reference) const;
    bool comparisonWithTextLeavesLoop(const Draft&) const;
    Reference buildInFrontOfOpenLoop(uint32_t indexOfLoop, Draft&&, const Effects* foundByCaller = nullptr);
    bool isInvariantInInnermostLoop(Reference) const;
    bool isLookupLoopInvariant(std::span<const Reference> operands, const Effects& ofLookup) const;
    std::optional<uint32_t> outermostLoopToMakeTextInFrontOf(std::span<const Reference> operands, const Effects& ofText) const;
    bool hasVisibleWork(uint32_t loopIndex);
    Reference flatStringBeforeLoop(uint32_t loopIndex, Reference string);
    bool isReachedByEachIteration(uint32_t loopIndex) const;
    std::optional<uint32_t> outermostLoopWithoutVisibleWork(uint32_t outermost);
    Reference buildLoopEntryCondition(uint32_t indexOfLoop);
    bool tryBuildFirstArrivalConditions(uint32_t loopIndex, Vector<Reference, 2>& conditions);
    bool isFlattenedBeforeLoop(uint32_t loopIndex, Reference string, std::span<const Reference> conditions) const;
    bool isInSightWhereWalkBuilds(Reference) const;
    bool dominatesLoopVersions(Reference) const;
    bool executesInEverySpecializedIteration() const;
    bool tryPreparePreheaderForCall();
    Reference buildOnFirstArrival(ValueID site, const Draft& call, const Effects&);
    Reference buildOnFirstArrival(ValueID site, TypeID resultType, Origin, NOESCAPE const ScopedLambda<Reference()>& buildResult);
    static constexpr uint32_t noCounter = std::numeric_limits<uint32_t>::max();
    struct ElementRecurrence {
        ValueID store;
        ValueID read;
        uint32_t counterPosition;
        int32_t offset;
    };
    Reference findElementAtConstant(Reference array, int64_t position) const;
    Reference headerArrayOfVariable(FunctionID callee, ValueID read);
    struct CarriedElementNote {
        Reference array;
        Reference parameter;
        uint64_t noted;
    };
    Vector<CarriedElementNote, 2> listCarriedElementNotes(Reference array, int64_t storedPosition) const;
    void restoreCarriedElementNotes(std::span<const CarriedElementNote>);
    bool mayCarryElements() const { return !m_carriedResultRequests.isEmpty(); }
    std::span<const ElementRecurrence> elementRecurrences();
    void carryStoredElement(const BodyValue& store, Reference array, Reference stored);
    std::optional<uint32_t> outermostLoopToLookUpInFrontOf(std::span<const Reference> operands, const Effects& ofLookup);
    std::optional<uint32_t> outermostLoopForBuiltinPreheaderRead();
    void holdLookupInFrontAgainstLookupInTurn(Reference believed, Draft&& lookedUpAgain, const Effects& ofLookup, ProofSource = ProofSource::RememberedFact);
    void holdValueRememberedAgainstValueAtSite(Reference believed, Draft&& readAgain);

    void noteFormAskedAhead(Opcode opcode, KnowledgeLevel level, FunctionID function = { }) { m_formAskedAhead = { opcode, level, function }; }
    FormAskedAhead takeFormAskedAhead() { return std::exchange(m_formAskedAhead, { }); }

    bool walkInline(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft* writtenOut = nullptr, const BitVector* inlinedHeadBlocks = nullptr);
    using RestArrayAllocator = ScopedLambda<Reference(unsigned entryParameter)>;
    bool visitBodyFromEntry(std::span<const Reference> entryArguments, BlockID whereListsAreMade, const RestArrayAllocator&);
    bool walksBodyBehindEntry() const { return m_restArrayAllocator && m_frames.last().isOfUnit; }
    Reference argumentOfEntry(uint32_t position, Origin);
    Reference rematerializeHiddenRestArray();
    void endLifetimesAtThrow(std::span<const Reference> thrown);
    bool walkInlineUnderHandlerMade(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft& writtenOut, BlockID handler);
    bool walkInlineMethodThatProtocolCalls(const BodyValue& step, FunctionID callee, std::span<const Reference> arguments);
    void noteThatCopyMakesNewList(FunctionID callee, Reference receiver, bool runOverBoxedValuesTakesListFirst = false);
    bool runOverBoxedValuesTakesNewListOfCopyFirst() const { return m_runOverBoxedValuesTakesNewListOfCopyFirst; }
    void noteListThatRunOverBoxedValuesTakesFirst(ValueID list) { m_listThatRunOverBoxedValuesTakesFirst = list; }
    bool runOverBoxedValuesTakesFirst(ValueID list) const { return list && list == m_listThatRunOverBoxedValuesTakesFirst; }
    bool takeWordThatCopyMakesNewList(Reference receiver);

    void leaveArrivals(FunctionID callee, std::span<const Reference> arguments);
    void leaveAnythingAt(FunctionID callee);
    struct ArrivalHeld {
        enum class Kind : uint8_t { Proven, UndefinedFrom, Anything, MayBeEnteredByValue };
        FunctionID callee;
        uint32_t entryParameter { 0 };
        TypeID proven;
        Kind kind { Kind::Proven };
    };
    std::span<const ArrivalHeld> arrivalsHeld() const LIFETIME_BOUND { return m_arrivalsHeld.span(); }
    void handOverArrivals();

    COLD NEVER_INLINE void fail(ASCIILiteral what, InternalErrorKind = InternalErrorKind::Unknown);
    COLD NEVER_INLINE void noteOperationBuiltWhereCodeIsVerified(ValueID, const Draft&);
    void sayFailure();
    void report(const UserDiagnostic& diagnostic) { m_reports.append(diagnostic); }
    Vector<UserDiagnostic> takeReports() { return std::exchange(m_reports, { }); }
    bool hasFailed() const { return m_hasFailed; }
    bool hasHandlerMadeOverCopies() const { return !!m_handlerMadeOverCopies; }
    ASCIILiteral failure() const { return m_failure; }
    InternalErrorKind kindOfFailure() const { return m_kindOfFailure; }
    bool failureOutlivesTries() const { return m_hasFailed && (m_kindOfFailure == InternalErrorKind::RecursiveSpeculation || m_kindOfFailure == InternalErrorKind::StaleHoistedLoopBound || m_kindOfFailure == InternalErrorKind::HandlerEdgePassesPlaceholder || m_kindOfFailure == InternalErrorKind::OpcodeWithoutCode || m_kindOfFailure == InternalErrorKind::LoopSummaryMismatch || m_kindOfFailure == InternalErrorKind::HoistedOperandNotInvariant || m_kindOfFailure == InternalErrorKind::IterationCannotBeUndone); }
    ALWAYS_INLINE bool isUnreachable() const { return m_block.hasEndedOrIsNone(); }

    TypeID resultTypeOf(const Draft&);
    bool standsUnderHandler() const { return isUnderHandler(); }
    void buildsCallWithoutReturn(bool builds) { m_buildsCallWithoutReturn = builds; }
    bool readsNameSteppedInLoop(ValueID operand);
    bool isUnderCatch() const { return isUnderHandler() && isUnderHandlerOtherThanCleanups(); }
    void setResultOfCallBeingTyped(TypeID type) { m_resultOfCallBeingTyped = type; }
    TypeID asFarAsUnitMayAsk(TypeID claimed);
    TypeID typeOfLibraryObject(BuiltinID);
    TypeID exchangeResultOfReadBeingTyped(TypeID type) { return std::exchange(m_resultOfReadBeingTyped, asFarAsUnitMayAsk(type)); }
    TypeID exchangeContentsBeingTyped(TypeID type) { return std::exchange(m_contentsBeingTyped, asFarAsUnitMayAsk(type)); }
    Reference buildReadTypedByWhatIsRemembered(Draft&& read, TypeID remembered, TypeID shownOfWhatIsRemembered)
    {
        SetForScope type(m_resultOfReadBeingTyped, asFarAsUnitMayAsk(remembered));
        SetForScope isRemembered(m_resultOfReadIsRemembered, true);
        Reference built = build(WTF::move(read));
        if (built && built.isValue())
            setTypeAsked(built.id(), asFarAsUnitMayAsk(shownOfWhatIsRemembered));
        return built;
    }
    const PositionTypes& positions() const { return *m_positions; }
    const PositionTypes& positionsKnowingWhatFunctionsHandBack() const { return *m_positionsKnowingWhatFunctionsHandBack; }
    const PositionTypes& positionsOf(CodeRole role) const { return role == CodeRole::SlowBody && !facts().beforeEvaluation().holdsIn(unit().function) ? *m_positionsOfSlowBody : *m_positionsOfBuild; }
    const LinkedFirstStores& firstStores() const { return m_noTextWasEvaluated ? facts().beforeEvaluation().firstStores : facts().linked(); }
    bool comparesElementsOfViews() const { return m_comparesElementsOfViews; }
    void noteComparisonOfElementsOfViews() { m_comparesElementsOfViews = true; }
    bool hasInBoundsPaths() const { return m_hasInBoundsPaths; }
    void noteInBoundsPath() { m_hasInBoundsPaths = true; }
    bool mayEnterAfterGuardOf(FunctionID callee) const { return m_noTextWasEvaluated || !facts().beforeEvaluation().holdsIn(callee); }
    const PositionTypes& heldPositions() const { return *m_heldPositions; }
    TypeID withoutFunctionsThatMayNotBeMentioned(TypeID);
    const PositionTypes& positionsKnowingNothing() const { return *m_positionsKnowingNothing; }
    TypingContext typing() const { return { program(), m_types, *m_positions, TypeLevel::Proven }; }

    bool hasStringsInBuilders() const { return !m_stringsInBuilders.isEmpty(); }
    bool exitsLoopBeingVersioned(BlockID to) const { return exitsLoopBeingVersioned(m_frames.size() - 1, to); }
    bool canRestartIteration() const;
    bool isIterationQuiet() const;
    struct NumberAskedBesideNumber {
        uint32_t operandIndex { 0 };
        Reference proven;
        explicit operator bool() const { return !!proven; }
    };
    NumberAskedBesideNumber askValueBesideNumberForNumber(const BodyValue&);
    bool testOfTypeLooksAtNoShape(TypeID tested) const
    {
        const Type& type = m_types.at(tested);
        return !type.layouts || type.hasOnlyClosures();
    }
    void noteThatFrontAsksWhatNothingSpeaksFor() { m_versionedLoops.last().frontAsksWhatNothingSpeaksFor = true; }
    enum class QuestionIsAsked : bool { InCopiedCalleesAlone, Anywhere };
    bool leavesOutQuestionThatEveryTurnWouldAsk(QuestionIsAsked = QuestionIsAsked::InCopiedCalleesAlone) const;
    bool walksCopyWithoutQuestionsOfEveryTurn() const { return m_walksCopyWithoutQuestionsOfEveryTurn; }
    bool walksCopyWithoutSomeIterationGuesses() const { return m_walksCopyWithoutSomeIterationGuesses; }
    bool mayCheckOutsideCopy() const { return !m_versionedLoops.isEmpty() && m_versionedLoops.last().startOfCopyWithoutSomeIterationGuesses; }
    bool walksCopyForAnotherKindOfStorage() const { return m_walksCopyForAnotherKindOfStorage; }
    bool currentGenericVersionSpeculates() const { return !m_versionedLoops.isEmpty() && m_versionedLoops.last().genericVersionSpeculates; }
    bool isIterationEffectFree() const;
    bool isUncaughtIterationEffectFree() const;
    std::optional<PassedTest> tryGuardArrayAtHeader(const BodyValue& read);
    bool isArrayGuardedAtHeader(Reference list) const { return !m_versionedLoops.isEmpty() && list && m_versionedLoops.last().arrayGuardedAtHeader == list; }
    uint32_t takePendingUndoableStore() { return std::exchange(m_pendingUndoableStore, 0); }
    bool isUndoableStoreIntoIterationObject(uint32_t place) const { return m_undoableStores[place].isIntoIterationObject; }
    bool canRecordUndoableStore() const;
    void recordUndoableElementStore(Reference list, Reference position, Reference held, ArrayStorage);
    void recordUndoableGlobalStore(PropertyKeyID, Reference address, Reference held);
    void recordUndoableStaticStore(VariableID, PropertyKeyID, Reference classObject, Reference held);
    void recordUndoableKeyedStore(Reference address, Reference object, Reference held, PropertyKeyID name);
    void recordUndoableScopeStore(Reference scope, FieldReference, Reference held);
    void recordUndoableVariableStore(VariableID, Reference held);
    bool isUndoingStores() const { return m_isUndoingStores; }
    bool mayUndoCopiedStore() const { return canRecordUndoableStore() && m_frames.size() - 1 > m_versionedLoops.last().frameIndex; }
    bool canHoistGlobalAddress() const;
    void recordUndoableFieldStore(Reference heir, LayoutID, uint32_t field, Reference held);
    uint32_t iterationRestartPoint() const { return m_versionedLoops.last().restartPoint; }
    bool isVariableStoreUndoable(VariableID) const;
    bool canGuardAfterStores() const;
    bool isOnSomePathsOfIteration() { return isOnIterationSidePath(); }
    bool mayLeaveCopyForCall() const;
    ValueID openOfPassedGenerator(FunctionID callee, uint32_t entryParameter) const;
    bool fileHoldsGenerators() const;
    bool walkInlineFusedChain(const BodyValue& call, FunctionID text, std::span<const Reference> arguments, const Draft& site, BlockID& asWritten);
    bool isInFusedChain() const { return m_frameOfFusedChain || m_fusesNoChain; }
    bool isInCopyOfFusedChain() const { return m_frameOfFusedChain; }
    bool isInCopyOfKey() const { return m_frameOfFusedChain && m_frames.size() > m_frameOfFusedChain && m_frames[m_frameOfFusedChain].keyOfComparator; }
    Reference narrowInFusedChain(Reference, TypeID tested, Origin);
    bool walkInlineKey(const BodyValue& call, FunctionID comparator, std::span<const Reference> arguments, const Draft& site, ValueID key);
    bool& fusesNoChain() { return m_fusesNoChain; }
    bool& exitsChainAtGenericLoops() { return m_exitsChainAtGenericLoop; }
    bool exitsChainAtGenericLoop(size_t frameIndex) const { return m_exitsChainAtGenericLoop && frameIndex + 1 == m_frameOfFusedChain; }
    ArrayStorage& storageOfFusedChain() { return m_storageOfFusedChain; }
    bool appendsDoublesInFusedChain() const { return m_storageOfFusedChain == ArrayStorage::Double && m_frames.size() == m_frameOfFusedChain; }
    bool copyIsWalkedWhereItsGuardFailed() const { return m_frames.last().isWalkedWhereItsGuardFailed; }
    ALWAYS_INLINE bool tryLeaveInlinedHead() { return m_frames.last().inlinedHeadBlocks && tryLeaveInlinedHeadSlow(); }
    bool tryLeaveInlinedHeadSlow();
    BlockID blockWhereCopyLeavesForCall();
    bool canUseIterationGuards(BlockID inputHead);
    void noteLoopWhosePushFillsLiteral(BlockID inputHead) { m_loopsWhosePushFillsLiteral.appendIfNotContains(std::pair { currentFunction(), inputHead }); }
    bool pushOfLoopFillsLiteral(BlockID inputHead) const { return m_loopsWhosePushFillsLiteral.contains(std::pair { currentFunction(), inputHead }); }
    bool someOperationOfRowThatRanOtherCodeIsCall(const BodyIndex&) const;
    bool canBuildIterationGuard() const;
    bool canRestartAtThrow() const;
    bool handlersRunOtherCode() const { return m_handlersRunOtherCode; }
    bool canBuildIterationGuardAfterStores() const;
    bool isInLoopOfIteration() const;
    bool loopPreservesViews() const;
    bool turnHoldsInnerLoops() const;
    bool hasDirectElementStoreInLoop() const;
    bool looksAskOnlyWhetherTurnsRunOtherCode(BlockID inputHead) const { return m_restartablePreviewHeader == inputHead; }
    bool looksTakeFormsBehindLooksAsBuilt() const { return m_looksTakeFormsBehindLooksAsBuilt; }
    bool otherHoldersAreAskedForListsInCopyOf(BlockID inputHead) const { return !m_sharedGenericVersions.isEmpty() && m_sharedGenericVersions.last().frameIndex == m_frames.size() - 1 && m_sharedGenericVersions.last().inputHead == inputHead && m_sharedGenericVersions.last().otherHoldersAreAskedForLists; }
    bool boundIsAskedForTextInCopyOf(BlockID inputHead) const { return !m_sharedGenericVersions.isEmpty() && m_sharedGenericVersions.last().frameIndex == m_frames.size() - 1 && m_sharedGenericVersions.last().inputHead == inputHead && m_sharedGenericVersions.last().boundIsAskedForText; }
    bool purchaseAsksWhetherTurnsCanBeQuiet() const { return m_purchaseAsksWhetherTurnsCanBeQuiet; }
    bool readStandsUnderHandler() const { return isUnderHandler(); }
    bool isInSpecializedIteration() const { return !m_versionedLoops.isEmpty() && m_versionedLoops.last().isEntered; }
    bool someCallerLoop(NOESCAPE const Invocable<bool(BlockID head)> auto& asks) const
    {
        return std::ranges::any_of(m_versionedLoops, [&](const VersionedLoop& loop) { return loop.frameIndex + 1 != m_frames.size() && asks(loop.inputHead); });
    }
    bool specializesLoopOfCurrentBody(BlockID inputHead) const { return specializesLoop(currentFrameIndex(), inputHead); }
    bool hasCallInSpecializedIteration() const { return !m_versionedLoops.isEmpty() && m_versionedLoops.last().hasCallInIteration; }
    bool hasUninlinedCallInIterations() const { return !m_versionedLoops.isEmpty() && m_versionedLoops.last().hasUninlinedCall; }
    bool readsAskSitesThoughCallStays() const { return m_readsAskSitesThoughCallStaysOfRow; }
    void noteReadKeptFromItsSiteBecauseCallStays() { m_readWasKeptFromItsSiteBecauseCallStays = true; }
    bool summaryHasUninlinedCall() const { return m_summaryHasUninlinedCall; }
    static BitVector findLoopsThatReckonWithElements(const BodyIndex&);
    static bool exceedsVersionedLoopSize(const BodyIndex&, LoopID);
    void setLooksTakeFormsBehindLooksAsBuilt(bool take) { m_looksTakeFormsBehindLooksAsBuilt = take; }
    void restartIteration();
    void restartIterationAtThrow();
    void saveUndoableStoreState(uint32_t store) { m_undoableStoreStates.append({ store, m_state }); }
    void recordUndoableAppend(Reference array, Reference position, Reference held, Reference length, ArrayStorage, uint32_t previous);
    enum class IterationPurity : uint8_t { EffectFree, KnownEffectsOnly, KnownEffectsOnlyWithInlinees };
    bool isInvariantInSpecializedVersion(Reference, IterationPurity = IterationPurity::EffectFree) const;
    bool tryHoistEntryTest(IterationPurity = IterationPurity::EffectFree);
    bool canTestStoredSlotBeforeLoop(Reference object, PropertyKeyID, IterationPurity);
    bool isHoistedDiamondParameter(Reference) const;
    Reference buildCarriedElement(const BodyValue& read, Reference array, IterationPurity, bool mayBeInt32, NOESCAPE const ScopedLambda<Reference(Reference position, bool isInt32, bool isGuardedAtHeader)>& readInPreheader);
    IterationPurity arrayGuardPurity() const { return isIterationEffectFree() ? IterationPurity::EffectFree : IterationPurity::KnownEffectsOnly; }
    Reference& receiverOfLargerMethod() { return m_receiverOfLargerMethod; }
    bool wasAskedForStorageOfNumbersInVain(Reference list) const { return list && list == m_listAskedForStorageOfNumbersInVain; }
    bool walksCopyOverBoxedList() const { return !!m_listAskedForStorageOfNumbersInVain; }
    bool walksNumericListCopy() const { return m_walksNumericListCopy; }
    bool walksOtherKindOf(BlockID inputHead) const { return m_walksCopyForAnotherKindOfStorage && m_loopOfOtherKind == std::pair { static_cast<uint32_t>(m_frames.size()), inputHead }; }
    bool walksLoopOverBoxedNumbers(BlockID inputHead) const { return m_walksLoopOverBoxedNumbers && walksOtherKindOf(inputHead); }
    ArrayStorage requestedLiteralStorage() const { return m_requestedLiteralStorage; }
    ArrayStorage& requestedLiteralStorage() { return m_requestedLiteralStorage; }
    bool iterationsPreservePrototypeChains() const;
    bool iterationsPreserveViewElements() const;
    bool iterationsKeepStaticFields() const;
    bool iterationsStoreUnderKey(PropertyKeyID) const;
    bool iterationsPreserveMemberGuards(IterationPurity = IterationPurity::EffectFree) const;
    bool canHoistNamedLookup(PropertyKeyID) const;
    bool canReadNamedInPreheader(PropertyKeyID) const;
    bool canReadInPreheader(const LocationMasks& reads) const;
    Reference lookupThatAnswers(Reference) const;
    Reference lookUpAfterLookup(Reference earlier, Draft&& lookup);
    bool isGuardedByEquality(uint32_t loopIndex) const;
    bool canHoistKeyedLookup(Opcode lookup) const;
    bool canHoistKeyedAddress(bool takesFunctions) const;
    bool canHoistLookup(const LocationMasks& reads) const;
    bool isResultUsedAsNumber() const;
    bool iterationsPreserveVariable(VariableID) const;
    Reference readVariableInPreheader(VariableID, TypeID tested, Origin);
    bool isBuildingInPlace() const { return m_isBuildingInPlace; }
    bool standsInCopyOfLoopBuiltForEachMember() const { return !m_sharedGenericVersions.isEmpty(); }
    struct RowFoundUnderKey {
        Reference holder;
        Reference key;
        Reference row;
        VariableID variableOfHolder;
        VariableID variableOfKey;
        ValueID holderField;
        Reference fieldHolder;
    };
    const RowFoundUnderKey& rowOfCopyFoundUnderKey() const { return m_rowOfCopyFoundUnderKey; }
    bool isBuiltInRestartableIteration(Reference) const;
    bool isAllocatedInRestartableIteration(Reference object) const;
    enum class GuardedIterations : uint8_t { All, AllBesideCopy, Some, SomeWithUnguardedVersion };
    bool tryBranchInPreheader(Reference test, bool passesWithYes, IterationPurity = IterationPurity::EffectFree, GuardedIterations = GuardedIterations::All);
    bool isSpecializedLoop(uint32_t loopIndex, IterationPurity purity) const { return specializedOpenLoop(purity) == loopIndex; }
    std::optional<PassedTest> tryHoistConditionalCheck(Reference test, bool passesWithYes, IterationPurity = IterationPurity::EffectFree, GuardedIterations = GuardedIterations::SomeWithUnguardedVersion);
    bool iterationsPreserveArrayGuards(Reference list, IterationPurity = IterationPurity::EffectFree);
    bool hasElementArithmeticInLoop(BlockID inputHead);
    bool mayVersionLoopUnderHandler(BlockID inputHead);
    bool outerHandlerReadsLoop(BlockID inputHead);
    bool hasOnlyOuterHandlers() const;
    bool canExitCurrentHandlers() const;
    bool hasElementComparisonInLoop(BlockID inputHead);
    std::span<const uint64_t> holdersFromInFrontThatLoopReadsUnderIndexes(BlockID inputHead);
    bool shouldAssumeLocalArrayReceivers() const { return m_inputs.options.shouldAssumeLocalArrayReceivers; }
    bool shouldAssumeNumericAddends() const { return m_inputs.options.shouldAssumeNumericAddends; }
    bool shouldAssumeArithmeticUsers() const { return m_inputs.options.shouldAssumeArithmeticUsers; }
    bool shouldAssumePermanentlyPristineIterables() const { return m_inputs.options.shouldAssumePermanentlyPristineIterables; }
    bool shouldAssumePristineDestructuring() const { return m_inputs.options.shouldAssumePristineDestructuring; }
    bool shouldSkipPairGuard() const { return m_inputs.options.shouldSkipPairGuard; }
    bool shouldAssumePristineReceivers() const { return m_inputs.options.shouldAssumePristineReceivers; }
    bool shouldSkipStorageChecks() const { return m_inputs.options.shouldSkipStorageChecks; }
    bool shouldAssumeVariablesInitialized() const { return m_inputs.options.shouldAssumeVariablesInitialized; }
    bool shouldAssumeSlotsInitialized() const { return m_inputs.options.shouldAssumeSlotsInitialized; }
    bool shouldIgnoreVariableChecks() const { return m_inputs.options.shouldIgnoreVariableChecks; }
    bool isInIterationAssumedRestartable() const { return m_inputs.options.shouldAssumeIterationsRestartable && !m_versionedLoops.isEmpty() && m_versionedLoops.last().isEntered && !isUnreachable() && m_versionedLoops.last().canUseIterationGuards; }
    bool shouldSkipSuperMethodGuard() const { return m_inputs.options.shouldSkipSuperMethodGuard; }
    bool shouldSkipHierarchyGuards() const { return m_inputs.options.shouldSkipHierarchyGuards; }
    bool shouldAllowMarkedObjects() const { return m_inputs.options.shouldAllowMarkedObjects; }
    bool shouldDropLastCase() const { return m_inputs.options.shouldDropLastCase; }
    bool shouldSkipClassGuards() const { return m_inputs.options.shouldSkipClassGuards; }
    bool shouldIgnoreSiblingPrototypes() const { return m_inputs.options.shouldIgnoreSiblingPrototypes; }
    bool shouldSkipPrototypeChecks() const { return m_inputs.options.shouldSkipPrototypeChecks; }
    bool shouldAssumeLayoutPrototype() const { return m_inputs.options.shouldAssumeLayoutPrototype; }
    bool shouldSkipSuperclassCheck() const { return m_inputs.options.shouldSkipSuperclassCheck; }
    bool shouldAssumeInvariantOperands() const { return m_inputs.options.shouldAssumeInvariantOperands; }
    bool shouldSkipSuperPrototypeGuard() const { return m_inputs.options.shouldSkipSuperPrototypeGuard; }
    std::optional<BlockID> innermostTrialVisitHeader() const { return !m_trialVisits.isEmpty() && m_trialVisits.last().frameIndex == m_frames.size() - 1 ? std::optional(m_trialVisits.last().inputHead) : std::nullopt; }
    using AllocationsUsedInLoop = Vector<uint32_t, 4>;
    AllocationsUsedInLoop allocationsUsedInLoop(BlockID inputHead);
    enum class IsListedAllocation : uint8_t { No, Yes };
    std::optional<IsListedAllocation> classifyAgainstAllocations(const AllocationsUsedInLoop&, Reference) const;
    bool iterationsSpareArray(uint32_t loopIndex, const Draft&, const LocationMasks& reads) const;
    bool iterationsPreserveArrayLength(Reference array, IterationPurity) const;
    bool iterationsPreserveTypedArrays(IterationPurity) const;
    bool areElementStoresNumeric(IterationPurity = IterationPurity::EffectFree) const;
    bool hasNoElementStores() const;
    bool textsOfLoopStayOutOfBuilders(BlockID inputHead) const;
    bool hasOpenTrialVisit(size_t frameIndex, BlockID inputHead) const;
    void walkLoopAgainWithoutBuilders(const StringInBuilder&);
    class SpecializedPreheaderScope;
    bool hasURLsThatAreNeverMade() const { return m_hasURLsThatAreNeverMade; }
    void noteURLThatIsNeverMade() { m_hasURLsThatAreNeverMade = true; }
    ListPutBackByLog<StringInBuilder, 2>& stringsInBuilders() { return m_stringsInBuilders; }
    bool keepsLogsForUndo() const { return m_serialOfNewestMark; }
    size_t stringInBuilderSearchEnd(ValueID);
    void noteParametersThatReadText(std::span<const ValueID> parameters)
    {
        for (ValueID parameter : parameters)
            m_parametersThatSomeScanFoundToReadTexts.set(parameter.index());
    }
    bool someScanFoundParameterToReadText(ValueID parameter) const { return m_parametersThatSomeScanFoundToReadTexts.get(parameter.index()); }
    Vector<uint32_t, 8> stringsInBuildersAtLoopExit(BlockID from, BlockID to);
    size_t topFrameIndex() const { return m_frames.size() - 1; }
    BlockID inputBlockBeingWalked() const { return m_frames.last().block; }
    bool isLiveAtHeadOf(ValueID, BlockID inputBlock);
    void bindAnewAtEveryBlock(ValueID);
    bool isBoundAnewAtEveryBlock(ValueID value) const { return m_frames.last().isDuplicated(value); }

private:
    friend class ClosedArm;
    friend class SpecializedPreheaderScope;
    friend class OpenArm;
    friend class ProofsHoldEverywhere;
    friend class ArgumentsMadeWhereTheyLeave;
    friend class KnownThreading;

    static constexpr uint32_t noOpenEdge = UINT32_MAX;

    struct FieldOfScope {
        Reference scope;
        FieldReference field;
        TypeID type;
        Representation form { Representation::JSValue };
        Reference parameter;
        Vector<Reference, 4> values;
        TypeID shown;
        bool isHeld { false };
        bool isSteppedByOne { false };
        bool isComputedFromItself { false };
        Vector<ValueID, 2> addedInTurns { };
        bool isSteppedConditionally { false };
        VariableID variable { };
        bool isSteppedOrAddedTo { false };
        Vector<ValueID, 2> storedInTurns { };
        Vector<ValueID, 2> readsTakenBySums { };
        bool isReadBySumsAlone { false };
        ValueID anticipatedRead { };
        PropertyKeyID slotKey { };
        LayoutSetID receivers { };
        bool slotIsInt32Position { false };
        ValueID slotHolder { };
        bool mayStartAsInteger { false };
        bool startsAsInteger { false };
        bool slotIsSteppedByOne { false };
        bool slotOnlyGrows { false };
        Range slotSteps { Range::empty() };
        std::optional<uint32_t> positionOfHolder { };
        bool isOfMergedObject { false };
        Reference element { };
        TypeID typeOfHolders { };
        Range startingIntegers { Range::empty() };
        Reference carriedResult { };
        Reference carriedArray { };
    };
    Reference valueKnownOf(const FieldOfScope& field) const
    {
        if (field.variable)
            return m_state.variable(field.variable);
        if (field.element)
            return m_state.element(field.scope, field.element);
        return field.slotKey ? m_state.slot(field.scope, field.field) : m_state.environmentField(field.scope, field.field);
    }
    struct SlotAccess {
        ValueID object;
        PropertyKeyID key;
        ValueID before;
        ValueID stored;
        bool staysInt32 { true };
        bool isStep { true };
        bool isStepUp { true };
        bool isFirstFill { false };
        bool isInInnerLoop { false };
    };
    static bool isFilledOnceInLoop(const SlotOfLoop&);
    VariableID variableOfSlotHolder(const SlotOfLoop&, uint32_t firstValueOfLoop) const;
    bool guardFilledSlot(const SlotOfLoop&, uint32_t firstValueOfLoop, const ScopedLambda<BlockID()>& empty);
    bool mayGuardFilledSlot(const SlotOfLoop&, uint32_t firstValueOfLoop);
    void guardSlotBeforeLoop(const SlotOfLoop&, uint32_t firstValueOfLoop, BlockID firstIteration);
    void findLoopSlots(std::span<const SlotOfLoop>, uint32_t firstValueOfLoop, Vector<FieldOfScope, 4>&, bool shouldReadBeforeHeader);
    void findSlotsStoredByCopies(std::span<const SlotOfLoop>, const Effects& ofTurns, Vector<FieldOfScope, 4>&, bool shouldReadBeforeHeader);
    void guardIntegerSlots(BlockID inputHead, const LoopPreview&);
    void noteAccessByCopy(const Draft&);
    FieldOfScope loopCarriedObjectSlot(Reference object, TypeID typeOfObjects, uint32_t position, PropertyKeyID);
    TypeID typeOfFieldInEveryHolder(const FieldOfScope&);
    void findSlotsOfObjectsThatComeRound(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<FieldOfScope, 4>&);
    Reference valueCarriedByLoop(const FieldOfScope&, std::span<const Reference> arguments);
    bool mayBeInDeadZone(const FieldOfScope&) const;
    TypeID backEdgeTypeOfVariable(BlockID inputHead, const FieldOfScope&) const;
    bool namesFreshCell(const WalkState&, Reference) const;
    bool isEnvironmentAllocation(Reference) const;
    void noteFieldsOfScopes(std::span<const FieldOfScope> fields)
    {
        for (const FieldOfScope& field : fields)
            noteFieldOfScope(field, field.parameter);
    }
    bool mayMeetUnmadeRecords(const WalkState& state) const { return m_numberOfStoresIntoUnmadeRecords && state.holdsFreshObjects() && runsInIterations(); }
    bool mayMeetUnmadeRecords(std::span<const uint32_t> edges) const;
    void findFieldsOfScopesThatMeet(std::span<const WalkState* const>, Vector<FieldOfScope, 4>&);
    Reference carriedResultOf(const WalkState&, Reference headerParameter) const;
    void findCarriedResultsThatMeet(std::span<const WalkState* const>, Vector<FieldOfScope, 4>&);
    void findFieldsOfScopesInFrontOfLoop(const LoopPreview&, Vector<FieldOfScope, 4>&);
    void findFieldsOfScopesFromOutsideThatQuietTurnsStore(BlockID inputHead, const Effects& ofTurns, std::span<const SlotOfLoop>, Vector<FieldOfScope, 4>&, bool shouldReadBeforeHeader);
    bool mayHandFieldOfScopeToBlock(FieldOfScope&, BlockID inputHeadOfLoopEntered = { });
    void keepFieldsOfScopesThatLoopWrites(BlockID inputHead, Vector<FieldOfScope, 4>&);
    Draft draftCapturedVariableRead(Reference scope, FieldReference);
    Draft draftVariableRead(const FieldOfScope&);
    enum class ReadBeforeHeaderUse : uint8_t { LoopCarriedValue, GuardOperand };
    Draft draftReadBeforeHeader(const FieldOfScope&, ReadBeforeHeaderUse, bool moduleMayBeUnevaluated = false);
    bool carriedFieldsPartWays(std::span<const uint32_t> passing, std::span<const uint32_t> closed);
    void noteFieldOfScope(const FieldOfScope&, Reference value);
    void renameCarriedFields(Reference value, Reference name);
    bool narrowCarriedFields(EdgeID&, BlockID from, BlockID to, uint32_t targetIndex);
    bool isFitForParameterOfField(const FieldOfScope&, Reference value);
    Reference fitForParameterOfField(const FieldOfScope&, Reference known);
    bool bringFieldsOfScopesRound(EdgeID&, BlockID head, Vector<Reference, 8>& arguments);
    void addScopeFieldParameters(BlockID entry, const LoopPreview&);
    void addParametersForSlotsOfObjects(BlockID entry, BlockID inputHead, const LoopPreview&, std::span<const Reference> parameters);
    void noteWhatParameterOfFieldIsShownToBe(const FieldOfScope&);
    void noteFieldsOfScopesThatBlockTakes(BlockID);
    void forgetAllButFreshObjectsAndFieldsThatBlockTakes(BlockID);
    size_t numberOfFieldsOfScopesThatBlockTakes(BlockID) const;

    struct ScopeLentToCall {
        Reference scope;
        Reference copy;
        Vector<FieldIndex, 4> fieldsAccessed;
        Vector<FieldIndex, 4> fieldsStored;
    };
    struct AccessesOfScopeByFunction {
        bool areAllItsMentions { false };
        Vector<FieldReference, 2> fieldsAccessed;
        Vector<FieldReference, 2> fieldsStored;
    };
    const AccessesOfScopeByFunction& accessesOfScopeByFunction(FunctionID, uint32_t entryParameter);
    bool functionOnlyReadsAndWritesScopeAt(FunctionID function, uint32_t entryParameter) { return accessesOfScopeByFunction(function, entryParameter).areAllItsMentions; }
    bool unitKeepsScopesToItself() const;
    template<typename Functor> bool everyOpenLoopAround(const Functor& holdsOfLoopAt) const;
    void endFieldsOfScopesThatTurnsStore(const LoopPreview&);
    void enterGenericVersion(BlockID entry, const LoopPreview&);
    Reference buildCopyOfScope(const ScopeLentToCall&, Origin);
    void lendCopiesOfScopesTo(Draft& call, Vector<ScopeLentToCall, 1>&);
    void takeBackScopesLentToCall(std::span<const ScopeLentToCall>, const Effects* ofCall, Origin);

    struct OpenEdge {
        OpenEdge() { }

        EdgeID edge;
        BlockID block;
        uint32_t next { noOpenEdge };
        Vector<Reference, 4> arguments;
        Vector<TypeID, 4> types;
        WalkState state;
        NamesOfWay names;
        BlockID inputBlockLeft;
        bool isLedByClosedArm { false };
        bool comesFromClosedArm { false };
        bool isLedByClosedArmThatKeepsSites { false };
    };

    static constexpr uint32_t noNarrowerName = UINT32_MAX;
    static constexpr uint32_t narrowerNamesOfDominator = UINT32_MAX - 1;

    struct UnrolledForInIterations {
        struct Arm {
            Reference object;
            Vector<Reference, 4> names;
            BlockID whereTestsPassed;
            WalkState stateWhereTestsPassed;
        };
        size_t place { 0 };
        ValueID step;
        ValueID list;
        uint32_t counter { 0 };
        Reference enumerator;
        Reference object;
        bool enumeratorIsAsked { false };
        Reference namesCountedByOpen;
        uint32_t numberOfNames { 0 };
        Reference firstPosition;
        Vector<Arm, 2> arms;
        const Arm* armBeingWalked { nullptr };
        Vector<uint32_t, 4> edgesIntoLoopAsWritten;
    };

    struct InputBlock {
        uint32_t firstOpenEdge { noOpenEdge };
        uint32_t firstNarrowerName { narrowerNamesOfDominator };
        uint32_t numberOfRecordsBehindEnd { UINT32_MAX };
        uint32_t firstSeparatePathRecord { UINT32_MAX };
        uint32_t serialLogged { 0 };
        BlockID head;
        bool hasEqualityTest : 1 { false };
        bool counterStaysWithinBound : 1 { false };
        bool isGuardedByEndTest : 1 { false };
        bool isEntered { false };
        bool handlerIsClassified { false };
        bool handlerOnlyClosesIterations { false };
        bool isAsNew() const { return firstOpenEdge == noOpenEdge && firstNarrowerName == narrowerNamesOfDominator && !head && !isEntered; }
    };

    struct StateOfHandler {
        BlockID handler;
        WalkState state;
        WalkState::RecordsLastMet lastMet;
        uint32_t serialLogged { 0 };
    };

    struct PendingBlock {
        PendingBlock() { }

        BlockID block;
        Vector<ValueID, 4> parameters;
        WalkState state;
        uint32_t numberOfArrivals { 0 };
        bool hasState { false };
        bool parametersAreSet { false };
        bool isMadeByAddBlock { false };

        bool hasLostState() const { return numberOfArrivals && !hasState; }
        template<typename State> void addArrival(State&& arriving)
        {
            bool knowsNothing = hasLostState();
            ++numberOfArrivals;
            if (std::exchange(hasState, true)) {
                state.meetWith(arriving);
                return;
            }
            state = std::forward<State>(arriving);
            if (knowsNothing)
                state.clear();
        }
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

    class NamesOfValues {
    public:
        enum class Writer : uint8_t {
            LogsWhatStoodThere,
            PutsBackWhatLogHolds,
            LaysFrame,
            ClosesEdgesIntoBlock,
            ForgetsValuesThatNobodyReadsAnyMore,
            ForgetsValuesOfBlockWalkedApart,
            ForgetsAnswerOfCopy,
            ForgetsNamesOfWay,
            ForgetsValuesOfLoop,
            TakesNamesOfTurnWrittenOut,
            PreparesFrontOfBuilds,
            UndoesTypedBuild,
            StandsBesidePlainWalk,
            VersioningDryRun,
        };
        size_t size() const { return m_names.size(); }
        const Reference& operator[](size_t value) const LIFETIME_BOUND { return m_names[value]; }
        std::span<const Reference> span() const LIFETIME_BOUND { return m_names.span(); }
        template<Writer> Reference& placeOf(size_t value) LIFETIME_BOUND { return m_names[value]; }
        void lay(size_t numberOfValues) { m_names.fill(Reference(), numberOfValues); }
        void shrink(size_t numberOfValues) { m_names.shrink(numberOfValues); }

    private:
        Vector<Reference, 32> m_names;
    };

    struct UseInLoops {
        enum class Kind : uint8_t { Operand, Argument, TakenAsItIs };
        Kind kind;
        BlockID block;
        uint32_t user;
        uint32_t place;
        uint32_t older;
    };
    struct UsesInLoops {
        Vector<std::pair<ValueID, uint32_t>> steps;
        Vector<uint32_t> newestStepOfLoop;
        Vector<bool> nestsWithListedSteps;
        Vector<bool> nestsWithListedUses;
        Vector<UseInLoops> uses;
        Vector<uint32_t> newestUseOf;
        Vector<uint32_t> madeOf;
    };

    struct Frame {
        Frame() { }

        FunctionID function;
        FunctionID functionWithUnenteredBlocks;
        const Body* body { nullptr };
        const BodyIndex* index { nullptr };
        std::span<const BlockID> blocksNeverEntered;
        std::unique_ptr<UsesInLoops> usesInLoops;
        std::unique_ptr<Vector<std::pair<ValueID, BlockID>>> arithmeticInLoops;
        Vector<uint16_t> operatorsFoundBehindValuesForGuesses;
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numbersOfLongListsOfNamesOfRoots;
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> endsOfUsesOfTexts;
        BitVector loopsInTrialVisits;
        BitVector loopsWithBlocksTouched;
        std::span<const LinkedVariableStoredFirst> variablesStoredFirst;
        Vector<EnvironmentMadeInFrame, 2> environmentsMade;
        NamesOfValues references;
        Vector<RememberedExpectation, 32> expectations;
        UncheckedKeyHashMap<uint32_t, RememberedExpectation, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> wholeExpectations;
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> shapesOfValues;
        uint32_t loopDepthAtCall { 0 };
        bool isInCallbackCopy { false };
        mutable BlockID blockOfLoopDepth;
        mutable uint32_t loopDepth { 0 };
        mutable std::optional<bool> isRecursive;
        mutable std::optional<bool> keepsFields;
        mutable std::optional<bool> summaryListsEscapes;
        uint32_t numberOfValuesBuiltBefore { UINT32_MAX };
        TypeID typeExpectedOfAnswer;
        bool answerIsDeclared { false };
        UseOfTextAsKey useOfAnswerAsKey { UseOfTextAsKey::NotKnown };
        ValueID valueHandedBack;
        bool hasLookedForValueHandedBack { false };
        uint32_t inlineFrame { 0 };
        ValueID keyOfComparator;
        bool returnsInPlaceOfUnit { false };
        bool isOfUnit { false };
        bool isSpecializedBody { false };
        bool canCallInstead { false };
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
        uint32_t inlineeRestartPoint { 0 };
        uint32_t displacedRestartMark { 0 };
        BlockID whereCopyLeavesForCall;
        Reference generatorMadeAgain;
        bool mayLeaveForCall { false };
        bool isWalkedWhereItsGuardFailed { false };
        const BitVector* inlinedHeadBlocks { nullptr };
        BlockID whereHeadEnds;

        BlockID block;
        bool blockFollowsClosedArms { false };
        mutable BlockID returnDominator;
        mutable BlockID exitDominator;
        uint32_t endOfValues { 0 };
        uint32_t firstValueListedByBirthInFront { 0 };
        uint32_t endOfValuesListedByBirthInFront { 0 };
        uint32_t walkedUpTo { 0 };
        uint32_t endOfStretch { 0 };
        uint32_t beginOfStretch { 0 };
        ValueID firstAccessorDefinedByNext;
        ValueID lastAccessorDefinedByNext;
        uint32_t numberOfStretchesBeingWalked { 0 };
        uint32_t currentBlockEndCount { 0 };
        PlaceTakenByEndOfBlock placeTakenByEndOfBlockAskedLast;
        bool hasHandler { false };
        bool handlerOnlyClosesIterations { false };
        BlockID handlerOfCodeBelow;
        static constexpr uint32_t notAskedYet = UINT32_MAX;
        mutable uint32_t numberOfFramesUpToHandlerBelow { notAskedYet };
        mutable CountOfFrame countAmongFramesOfFunction;
        uint32_t firstOperationToWalk { 0 };
        bool hasPassedByWhatFeedsThrow { false };
        bool isInPreheader { false };
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
            uint64_t values { 0 };
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
        uint64_t valuesOfNarrowerNames(uint32_t first) const { return first == noNarrowerName ? 0 : placesOfNarrowerNames[first].values; }
        void numberReferencesOfNarrowerName(uint32_t place);
        void bringTablesOfNarrowerNamesTo(uint32_t first) const;
        void addNarrowerName(uint32_t& first, Reference value, Reference name);
        enum class NarrowerNamesAsked : uint8_t { OfValue, UnderName };
        template<NarrowerNamesAsked, typename Functor> void forEachNarrowerName(Reference, const Functor&) const;
        Reference narrowestNameInLongList(Reference) const;
        bool isNarrowerNameInLongList(Reference) const;
        LoopLookedAt lookedAt;
        struct DuplicatedValues {
            WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(DuplicatedValues);
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
            void remove(ValueID value) { slots.mutableAt(slotOf(value)).ordinal = 0; }
            Reference nameOn(const NamesOfWay& names, ValueID value) const
            {
                uint32_t slot = slotOf(value);
                return slot == noSlot ? Reference() : names.nameIn(slot);
            }
            void nameOtherwiseOn(NamesOfWay& names, ValueID value, Reference name)
            {
                uint32_t slot = slotOf(value);
                names.setNameIn(slot, name);
                noteSeveralNamesIn(slot);
                boundOnWay.append(value);
            }
            ALWAYS_INLINE void noteBoundOnWay(ValueID value, Reference name)
            {
                if (uint32_t slot = slotOf(value); slot != noSlot)
                    noteBoundOnWayIn(slot, value, name);
            }
            NEVER_INLINE void noteBoundOnWayIn(uint32_t slot, ValueID value, Reference name)
            {
                boundOnWay.append(value);
                if (name)
                    noteNameIn(slot, name);
                else if (namesOfWay.nameIn(slot))
                    namesOfWay.setNameIn(slot, { });
            }

            void noteNameIn(uint32_t slot, Reference name)
            {
                if (slots[slot].onlyName != name)
                    slots.mutableAt(slot).noteName(name);
            }
            void noteSeveralNamesIn(uint32_t slot)
            {
                if (!slots[slot].hasSeveralNames)
                    slots.mutableAt(slot).hasSeveralNames = true;
            }

            std::unique_ptr<DuplicatedValues> copyForUndo()
            {
                Vector<uint32_t> numbers = WTF::move(slotNumbers);
                auto copy = makeUnique<DuplicatedValues>(*this);
                slotNumbers = WTF::move(numbers);
                return copy;
            }
            void putBack(const DuplicatedValues& before)
            {
                Vector<uint32_t> numbers = WTF::move(slotNumbers);
                for (uint32_t slot = before.slots.size(); slot < slots.size(); ++slot)
                    numbers[slots[slot].value.index()] = 0;
                *this = before;
                slotNumbers = WTF::move(numbers);
            }

            SharedArray<Slot, 8> slots;
            Vector<uint32_t> slotNumbers;
            Vector<uint32_t, 8> slotsListedAnew;
            Vector<std::pair<uint32_t, uint32_t>, 8> slotsByLastUse;
            uint32_t numberOfListings { 0 };
            Vector<std::pair<BlockID, ValueID>, 2> valuesBoxedOnEdgesIntoHandlers;
            Vector<std::pair<LoopID, BlockID>, 2> cleanupsWalkedBesideCopiesOfLoops;
            NamesOfWay namesOfWay;
            BlockID blockThatWayEnters;
            Vector<ValueID, 8> boundOnWay;
            Vector<NamesGivenForGood, 1> namesGivenForGood;
        };
        std::unique_ptr<DuplicatedValues> duplicatedValues;
        DuplicatedValues& ensureDuplicatedValues()
        {
            if (!duplicatedValues) {
                duplicatedValues = makeUnique<DuplicatedValues>();
                duplicatedValues->slotNumbers.fill(0, references.size());
            }
            return *duplicatedValues;
        }
        bool isDuplicated(ValueID value) const { return duplicatedValues && duplicatedValues->contains(value); }
        bool addDuplicatedValue(ValueID value)
        {
            DuplicatedValues& listed = ensureDuplicatedValues();
            uint32_t slot = listed.ensureSlotOf(value);
            if (listed.slots[slot].ordinal)
                return false;
            listed.slots.mutableAt(slot).ordinal = ++listed.numberOfListings;
            listed.slotsListedAnew.append(slot);
            if (Reference name = references[value.index()]) {
                listed.boundOnWay.append(value);
                listed.noteNameIn(slot, name);
            }
            return true;
        }
        Reference lastNameOf(ValueID value) const
        {
            Reference bound = references[value.index()];
            return bound || !isDuplicated(value) ? bound : duplicatedValues->nameOn(duplicatedValues->namesOfWay, value);
        }
        Vector<LoopID, 2> loopsBeingVersioned;
        BlockID fullyUnrolledHeader;
        UnrolledForInIterations* turnsOfForIn { nullptr };
        Vector<ValueID, 4> valuesListedForTurnsWrittenOut;
        ValueID stepWhosePositionEdgesBring;
        size_t placeOfForInWhosePositionEdgesBring { 0 };
        Vector<BlockID, 2> blocksBeingTailDuplicated;
        Vector<uint32_t> placesOfOpenLoops;
        std::optional<Vector<BlockID, 4>> handlerBlocks;
        Vector<uint32_t> numberOfHandlersThatRead;
        Vector<Vector<bool, 8>> parametersHandedRoundUnchangedByLoop;
        BitVector outermostLoopsLookedThrough;
        std::shared_ptr<BodyEdges> bodyEdges;
        Vector<InputBlock, 8> blocks;
        std::array<Reference, 2> booleansOfUnit { };
        Vector<StateOfHandler, 1> statesOfHandlers;
        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesOfStatesOfHandlers;
        size_t placeOfStateOfHandler(BlockID handler) const;
        StateOfHandler* findStateOfHandler(BlockID handler);
        void addStateOfHandler(BlockID handler, WalkState&&);
        bool takeStateOfHandler(BlockID handler, WalkState&);
        HashMap<uint64_t, std::unique_ptr<AccessUnderProvenKey>> accessesUnderProvenKeys;
        BlockID handlerBindingsBlock;
        bool hasPreheaderHandlerBindings { false };
        Vector<std::pair<ValueID, Reference>> handlerBindings;
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

        void takeRoomOf(Frames& former)
        {
            takeRoom(m_frames, former.m_frames);
            former.findLast();
            findLast();
        }
        void reserveCapacity(size_t capacity)
        {
            m_frames.reserveCapacity(capacity);
            findLast();
        }
        void append(Frame&& frame)
        {
            moveWhereSanitizerWatches(m_frames);
            m_frames.append(WTF::move(frame));
            findLast();
        }
        Frame& appendNew() LIFETIME_BOUND
        {
            moveWhereSanitizerWatches(m_frames);
            m_frames.constructAndAppend();
            findLast();
            return *m_last;
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

    void beginUnit(CodeUnit);
    bool walkBody();
    bool walkDelegatedBody();
    bool walkBodyBehindItsParameters();
    void allocateRestArrays();
    bool leaveArgumentsObjectToWhereListsAreMade(const BodyValue&);
    bool verifyThatCountStoodForLengthInFrontOfMaking();
    Vector<ValueID, 1> m_argumentsObjectsPutOffBehindEntry;
    Vector<BlockID, 4> m_blocksBuiltWhereCountStoodForLength;
    BlockID m_blockBuiltWhereListsWereMade;
    BlockID m_restArrayAllocationBlock;
    const RestArrayAllocator* m_restArrayAllocator { nullptr };
    BodyValue accessAsWalked(const BodyValue& written, PropertyKeyID* chosenAndNotDerived = nullptr);
    BodyValue accessByName(const BodyValue& written, PropertyKeyID);
    enum class KeyOfAccess : uint8_t { IsAnyValue, IsSymbol, ArmsHaveWalkedAccess };
    KeyOfAccess compareKeyOfAccessWithSymbol(const BodyValue& written, PropertyKeyID chosen);
    KeyOfAccess tellKeyOfAccessApartByItsTexts(const BodyValue& written);
    ValueID m_accessWhoseKeyArmHolds;
    uint32_t m_endOfKeyHeldByArm { 0 };
    size_t m_depthOfCopiesAroundKeyHeldByArm { 0 };
    void stopUnlessValueIsOf(Reference value, TypeID tested, ProofSource);
    ValueID m_accessWhoseKeyIsTested;
    ValueID m_questionOfTruthLeftToItsOwnTests;
    struct LoopEntryCondition {
        ValueID written;
        Reference left;
        Reference right;
        bool isNegated { false };
    };
    std::optional<LoopEntryCondition> findLoopEntryCondition(uint32_t indexOfLoop) const;
    std::optional<LoopEntryCondition> findConditionOnEntry(uint32_t indexOfLoop, ValueID asked) const;
    Reference buildConditionOnEntry(uint32_t indexOfLoop, const LoopEntryCondition&, uint32_t inFrontOf);
    struct HoistedDiamond {
        BlockID inputJoin;
        uint32_t placeOfLoop { 0 };
        BlockID resumeBlock;
        WalkState resumeState;
        uint32_t firstRecord { 0 };
    };
    std::optional<HoistedDiamond> m_hoistedDiamond;
    void beginHoistingConstantDiamond(const BodyTerminator& branch, Reference condition);
    bool isBuildingInPreheader() const { return m_hoistedDiamond || (m_block.index() < m_preheaderDepths.size() && m_preheaderDepths[m_block.index()]); }
    bool areOperandsLoopInvariant(const Draft&) const;
    bool isOperandLoopInvariant(Reference) const;
    void finishHoistingConstantDiamond(BlockID inputBlock);
    ValueID m_threadedCondition;
    bool m_threadedConditionValue { false };
    size_t m_threadedConditionFrame { 0 };
    uint32_t m_frameOfThatQuestionOfTruth { 0 };
    size_t m_depthOfCopiesAroundThatAccess { 0 };
    void walkBlock(BlockID inputBlock, bool isWalkedOnPaths = false);
    void walkOperation(const BodyValue&);
    struct RecordCounts {
        uint32_t values { 0 };
        uint32_t operationsSeenIfRunAgain { 0 };
        uint32_t operationsThatAskGuardsAgain { 0 };
    };
    RecordCounts recordCounts() const { return { m_code.numberOfValues(), m_numberOfOperationsBuiltThatAreSeenIfRunAgain, m_numberOfRecordsBuiltToAskGuardsAgain }; }
    void walkReadTypedByLink(const BodyValue&, uint32_t steps, FunctionID, uint32_t numberThatMayEnterByValueInFront);
    void materializeOperandsAndWalk(const BodyValue&, uint32_t steps, FunctionID, uint32_t numberThatMayEnterByValueInFront);
    void walkMaterializedOperation(const BodyValue&, uint32_t steps, FunctionID, uint32_t numberThatMayEnterByValueInFront, bool isAnsweredByBuilder, bool hasBoundOperation);
    bool checkWalkedOperation(const BodyValue&, const RecordCounts& before, bool isAnsweredByBuilder);
    void holdReadToRecordMadeForIt(size_t frameIndex, ValueID read);
    bool walkThroughChain(const BodyValue&);
    bool walkThroughChainAfterPreparing(const BodyValue&);
    using ChainOfOpcode = bool (Walker::*)(const BodyValue&);
    static ChainOfOpcode chainOf(Opcode);
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
    void finishBirthWhereLiteralEnds(const BodyValue& end);
    void showThatMakersThatLeaveAtHoldWhatIsHeldFilled(std::span<const SummaryMakerThatEndsWhereItLeaves>, uint32_t position, bool standsBehindOperationsOfBlock);
    void stopAtMakerUnless(Reference holds, Origin);
    void checkProofsOfOperandsOfDirectCall(const Draft&);
    void checkProofsOfWhatComesThroughFusedEntry();
    void checkProofThatSlotIsFilled(const Draft& read);
    bool checkWhatIsRememberedOf(const BodyValue& read);
    void askObjectAgainWhereStateHasAnswered(const BodyValue& read, PropertyKeyID);
    bool isTypedByWhatLinkSaysOfContents(const Draft&) const;
    ProofSource sourceOfProofOf(const Draft&) const;
    void checkProofOfRecord(ValueID record, const Draft&, TypeID proven);
    TypeID typeThatCallByValueIsShownToGive(const auto& call, std::span<const TypeID> shownOfOperands);
    void askWhatCallByValueGives(ValueID call, const Draft&, TypeID proven);
    Reference holdResultToItsType(Reference value, Origin);
    void holdOperandsToWhatCalleeBelieves(const Draft&);
    void checkThatReadGivesValue(Reference value, ProofSource, Origin);
    void beginModuleInBody();
    ALWAYS_INLINE void setCurrentInputBlock(BlockID inputBlock)
    {
        if (m_frames.last().block != inputBlock)
            switchToInputBlock(inputBlock);
    }
    void switchToInputBlock(BlockID inputBlock);
    void setCurrentInputBlock(Frame&, const BodyBlock&, InputBlock&);
    void noteStateAtHeadForHandler();
    bool isWhatHeadNamesForHandler(size_t frameIndex, ValueID argument, Reference handed);
    void noteStateAtThrowForHandler(std::span<const Reference> thrown = { });

    ALWAYS_INLINE void logInputBlockOnceForMark(Frame& frame, BlockID block)
    {
        InputBlock& record = frame.blocks[block.index()];
        if (m_serialOfNewestMark <= record.serialLogged)
            return;
        m_inputBlocksInTries.append(InputBlockInTry { static_cast<uint32_t>(&frame - m_frames.begin()), block, record });
        record.serialLogged = m_serialOfNewestMark;
    }
    ALWAYS_INLINE InputBlock& inputBlock(Frame& frame, BlockID block)
    {
        if (frame.blocks.isEmpty()) [[unlikely]]
            frame.blocks.fill(InputBlock { }, frame.index->numberOfBlocks());
        if (m_trialVisitDepth) [[unlikely]]
            logInputBlockOnceForMark(frame, block);
        noteThatBlockIsTouched(frame, block);
        return frame.blocks[block.index()];
    }
    static ALWAYS_INLINE void noteThatBlockIsTouched(Frame& frame, BlockID block)
    {
        for (LoopID loop = frame.index->loopOf(block); loop && !frame.loopsWithBlocksTouched.set(loop.index()); loop = frame.index->loop(loop).parent) { }
    }
    template<typename OfBlock, typename OfLoop> static void forEachBlockAndLoopRightIn(const BodyIndex& index, LoopID loop, const OfBlock& ofBlock, OfLoop&& ofLoop)
    {
        std::span<const BlockID> members = index.membersOf(loop);
        for (size_t place = 0; place < members.size();) {
            LoopID holder = index.loopOf(members[place]);
            bool standsInRow = holder != loop && index.loop(holder).firstMember == index.loop(loop).firstMember + place;
            if (standsInRow && ofLoop(holder))
                place += index.loop(holder).numberOfMembers;
            else
                ofBlock(members[place++]);
        }
    }
    ALWAYS_INLINE InputBlock& inputBlock(size_t frameIndex, BlockID block) { return inputBlock(m_frames[frameIndex], block); }
    uint32_t addOpenEdge(size_t frameIndex, BlockID target);
    uint32_t addOpenEdge(InputBlock& target);
    Vector<uint32_t, 4> takeOpenEdges(BlockID inputBlock);
    void releaseOpenEdge(uint32_t);
    BlockID addBlockFor(BlockID inputBlock, BlockRole = BlockRole::Ordinary, BlockFrequency = BlockFrequency::Normal);
    void closeEdgesInto(BlockID, BlockID inputBlock, std::span<const uint32_t> edges, bool isHandler);
    struct ObjectsMetAtJoin {
        uint32_t placeAmongParameters { 0 };
        uint32_t firstParameterOfSlots { 0 };
        uint32_t numberOfParametersOfSlots { 0 };
        ObjectsThatMeet objects;
    };
    void findObjectsThatMeetAt(BlockID block, BlockID inputBlock, std::span<const uint32_t> edges, std::span<const uint32_t> joined, std::span<const uint32_t> joinedNames, Vector<ValueID, 8>& parameters, Vector<ObjectsMetAtJoin, 1>& found);
    Reference addParameterForSlotOfObjectsThatMeet(BlockID, const ObjectsThatMeet::Slot&);
    void closeBackEdge(EdgeID, BlockID head, std::span<const Reference> arguments);
    bool looksAtLoopsAroundHaveCounted(const Draft&, const Effects&);
    Opcode m_opcodeBeingWalked { Opcode::Undefined };
    ValueID m_valueBeingWalked;
    bool m_isMakingObjectsOfConstant { false };
    struct UnswitchCandidate {
        FunctionID function;
        BlockID block;
        uint32_t quietTarget { 0 };
        LocationMasks reads { };
        bool isHoistable { true };
        bool asksForInt32s { false };
        bool testsForOtherClass { false };
    };
    struct BuildsBought {
        Effects effects { Effects::none() };
        Vector<TypeID, 8> typesOfParameters;
        TypesOfValuesOfLoop typesOfValues;
        uint32_t firstValue { 0 };
        Vector<TypeID, 8> entered;
        bool isQuietOutsideArms { false };
        bool looksAtStaticField { false };
        Effects effectsOfQuietDrafts { Effects::none() };
        Vector<GuessCountedOnByLooks, 4> tests;
        bool firstOtherCodeRanInCopy { false };
        bool hasUninlinedCall { false };
        bool asksLengthsOfViewsFromInFrontForWrittenPlaces { false };
        bool takesApartListsFromInFront { false };
        bool handlersRunOtherCode { false };
        Vector<ValueID, 4> operationsThatRanOtherCode;
        Vector<MemberConditionRecord, 2> guardsOfStoresAsked;
        uint32_t numberOfEdgesThatCameRound { 0 };
        bool keepsPlainWalk { false };
    };
    struct MergedHeaderObject;
    struct StoresIntoScopes {
        bool areKnown { false };
        bool everyFieldIsTakenForStored { false };
        bool everyFieldWasTakenForStored { false };
        Vector<FieldReference, 4> fields;
        Vector<FieldReference, 4> fieldsThatCallsStore;
        Vector<SlotOfLoop, 2> slots;
        Vector<CapturedCounter, 2> counters;
        Vector<ValueID, 2> unmergedHolders;
        Vector<ValueID, 2> widenedHolders;
        std::shared_ptr<const Vector<MergedHeaderObject, 1>> mergedObjects;
    };
    struct CarriedResult {
        ValueID site;
        TypeID type;
        bool isElement { false };
        bool isStruck { false };
        Representation form { Representation::JSValue };
        std::optional<int32_t> constantPosition { };
        FieldReference field { };
        PropertyKeyID fieldKey { };
        VariableID staticField { };
        FunctionID copiedCallee { };
        bool isField() const { return !!fieldKey; }
    };
    struct LoopSummary {
        std::shared_ptr<const BuildsBought> bought;
        std::shared_ptr<const LoopSummary> summaryAtLastVisit;
        Vector<TypeID, 8> types;
        Vector<uint8_t, 8> rungs;
        Effects effects { Effects::none() };
        uint32_t trialVisitCount { 0 };
        bool isPessimistic { false };
        bool knowsEffects { false };
        TypesOfValuesOfLoop typesOfValues;
        uint32_t firstValue { 0 };
        uint32_t numberOfRecordsWhenTypesOfValuesWereNoted { 0 };
        bool hasAskedAboutTexts { false };
        bool hasAskedAboutNarrowerForm { false };
        bool hasTestedElementsInside { false };
        bool hasNarrowedSums { false };
        Vector<bool, 8> steppedOnDoubles;
        Vector<bool, 8> startIsWiderThanWhatEnters;
        Vector<TypeID, 8> enteredWhereStartWasStruck;
        Vector<TypeID, 8> enteredFromHeadsAroundWhenItFitted;
        Vector<TypeID, 8> cameRoundBesideParametersWhenItFitted;
        struct StartBelowCarriedType {
            uint32_t parameter;
            TypeID start;
            TypeID from;
            uint8_t rung;
        };
        Vector<StartBelowCarriedType, 2> startsBelowCarriedTypes;
        uint64_t carriedStartsStruck { 0 };
        bool narrowerStartHasMisfitted { false };
        bool didNarrowAssumedEffects { false };
        bool hasFittedOnce { false };
        bool severalBuildsAreStruck { false };
        bool isUnswitchRejected { false };
        bool isInt32FallbackRejected { false };
        bool isOtherClassRejected { false };
        bool isReentryRejected { false };
        bool hasSideExits { false };
        Vector<TypeID, 8> typesAtReentry;
        uint64_t rarelyWidenedParameters { 0 };
        uint64_t overreadWidenedParameters { 0 };
        uint64_t sumsThatStartAtNumberThatEnters { 0 };
        Vector<ValueID, 2> mentionsThatLeave;
        StoresIntoScopes storesIntoScopes;
        bool listsAreStruck { false };
        bool isInt32VersionRejected { false };
        bool otherCodeStaysInTypedBuild { false };
        bool otherCodeIsKeptOutOfFirstCopyInVain { false };
        bool otherCodeStayedInVain { false };
        bool readsAskSitesThoughCallStays { false };
        bool counterWasSteppedDown { false };
        bool counterWasRaised { false };
        bool secondCountersAreStruck { false };
        uint64_t sumsStruck { 0 };
        bool effectsAreGuessed { false };
        bool effectsAreGuessedByTextOfLoop { false };
        bool hasFailedWhereItMisfitted { false };
        uint8_t whatTurnsShowed { 0 };
        LocationMasks placesThatQuietTurnsWrite;
    };
    static void restoreSummary(LoopSummary&, const LoopSummary& walked);
    NEVER_INLINE bool tryRestoreLastVisitSummary(size_t place, bool isWalkedInBuild);
    NEVER_INLINE void recordSummaryAtLastVisit(size_t place, bool isWalkedInBuild, const std::shared_ptr<const LoopSummary>&);
    struct FreshObjectKeptAtHead {
        ValueID mention;
        Reference object;
    };
    static constexpr ValueID everyFreshObject { };
    struct RecordThatMakes {
        uint32_t maker;
        uint32_t made;
    };
    struct TrialVisit {
        size_t frameIndex { 0 };
        BlockID inputHead;
        uint32_t summaryIndex { 0 };
        Effects done { Effects::none() };
        bool misfits { false };
        bool requestsCarriedResult { false };
        Vector<ValueID, 2> widenedHolders;
        uint64_t parametersThatClimbed { 0 };
        Effects assumed { };
        uint32_t firstRecord { 0 };
        Effects doneOutsideArms { Effects::none() };
        Effects doneByQuietDrafts { Effects::none() };
        uint32_t numberOfRecordsAtHead { 0 };
        std::optional<AllocationsUsedInLoop> othersAreSparedAtHead;
        Vector<uint32_t, 2> listsMadeHereThatAreSparedAtHead;
        Vector<uint32_t, 2> objectsMadeInFrontThatAreWritten;
        LocationMasks writtenWithoutOtherCode;
        Vector<LocationMasks, 4> readsThatCouldStandInFront;
        Vector<TypeID, 8> entered;
        Vector<TypeID, 8> cameRound;
        Vector<Vector<ValueID, 2>, 8> valuesThatCameRound;
        Vector<TypeID, 8> cameRoundBesideItself;
        Vector<bool, 8> startsStruck;
        bool startIsNarrowerThanRowHeld { false };
        Vector<TypeID, 8> madeInTurns;
        Vector<Vector<ValueID, 2>, 8> headParametersThatCameRound;
        Vector<RecordThatMakes, 8> recordsThatMakeWhatCameRound;
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> recordsWhoseMakersAreNoted;
        bool somethingIsWantedInside { false };
        struct NarrowingThatWaits {
            uint32_t firstRecordOfNest { 0 };
            uint32_t ownSummaryIndex { 0 };
            size_t firstHeadParameter { 0 };
            size_t endOfHeadParameters { 0 };
        };
        Vector<NarrowingThatWaits, 1> narrowingsThatWait;
        size_t numberOfHeadParametersWithSourcesInFront { 0 };
        Vector<ValueID, 2> loweredCounters;
        Vector<ValueID, 2> raisedCounters;
        Vector<SecondCounter, 2> secondCounters;
        bool firstOtherCodeRanInCopy { false };
        bool hasUninlinedCall { false };
        bool asksLengthsOfViewsFromInFrontForWrittenPlaces { false };
        bool takesApartListsFromInFront { false };
        bool handlersRunOtherCode { false };
        uint32_t numberOfEdgesThatCameRound { 0 };
        Vector<ValueID, 8> headParametersThatEnter;
        Vector<TypeID, 8> enteredBesideHeadParameters;
        std::shared_ptr<const LoopSummary> assumedSummary;
        Vector<ValueID, 4> operationsThatRanOtherCode;
        Vector<MemberConditionRecord, 2> guardsOfStoresAsked;
        Vector<LayoutID, 2> classesOfBirths;
        Vector<LayoutID, 2> classesWhoseGuardsTurnBegins;
        bool callOnUsualWayComesRound { false };
        Vector<FreshObjectKeptAtHead, 2> freshObjectsKept;
        bool freshObjectHasLeft { false };
        Vector<GuessCountedOnByLooks, 4> testsSeen;
        HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> operandsOfTestsSeen;
        bool looksAtStaticField { false };
        bool knowsWhatIsAssumed { false };
        bool wantsStateInFront { false };
        std::shared_ptr<WalkState> stateInFront;
        bool keepsScopesAtHead { false };
        Vector<FieldReference, 4> fieldsOfScopesStored;
        Vector<FieldReference, 4> fieldsOfScopesEnded;
        Vector<CapturedCounter, 2> counters;
        Vector<SlotAccess, 2> slotAccesses;
        Vector<SlotOfLoop> placesStoredByCopies;
        Vector<SlotOfLoop> placesReadByCopies;
        Vector<TypeID> scopesAllocatedBeforeLoop;
        struct SlotOfHeadParameter {
            uint32_t position { 0 };
            PropertyKeyID key;
            uint32_t numberOfEdgesThatKnowIt { 0 };
        };
        Vector<SlotOfHeadParameter, 2> slotsOfHeadParameters;
        Vector<std::pair<ValueID, uint32_t>, 4> positions;
        struct TypeLeftByTurnsWrittenOut {
            uint32_t value { 0 };
            TypeID type;
        };
        Vector<TypeLeftByTurnsWrittenOut> typesLeftByTurnsWrittenOut;
        Vector<TypeLeftByTurnsWrittenOut> typesOfSumsLeftInArms;
    };
    class TrialVisitStack {
        using Visits = Vector<std::unique_ptr<TrialVisit>, 4>;

        template<typename Visit>
        struct Iterator {
            using value_type = TrialVisit;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            using reference = Visit&;
            using pointer = Visit*;

            const std::unique_ptr<TrialVisit>* place { nullptr };

            Visit& operator*() const { return **place; }
            Visit* operator->() const { return place->get(); }
            Iterator& operator++()
            {
                ++place;
                return *this;
            }
            Iterator operator++(int) { return Iterator { place++ }; }
            bool operator==(const Iterator&) const = default;
        };

    public:
        size_t size() const { return m_visits.size(); }
        bool isEmpty() const { return m_visits.isEmpty(); }
        TrialVisit& operator[](size_t place) LIFETIME_BOUND { return *m_visits[place]; }
        const TrialVisit& operator[](size_t place) const LIFETIME_BOUND { return *m_visits[place]; }
        TrialVisit& last() LIFETIME_BOUND { return *m_visits.last(); }
        const TrialVisit& last() const LIFETIME_BOUND { return *m_visits.last(); }
        Iterator<TrialVisit> begin() LIFETIME_BOUND { return { m_visits.begin() }; }
        Iterator<TrialVisit> end() LIFETIME_BOUND { return { m_visits.end() }; }
        Iterator<const TrialVisit> begin() const LIFETIME_BOUND { return { m_visits.begin() }; }
        Iterator<const TrialVisit> end() const LIFETIME_BOUND { return { m_visits.end() }; }

        bool containsIf(NOESCAPE const Invocable<bool(const TrialVisit&)> auto& matches) const
        {
            return m_visits.containsIf([&](const auto& visit) { return matches(*visit); });
        }
        size_t reverseFindIf(NOESCAPE const Invocable<bool(const TrialVisit&)> auto& matches) const
        {
            return m_visits.reverseFindIf([&](const auto& visit) { return matches(*visit); });
        }
        size_t placeOf(const TrialVisit& visit) const
        {
            return m_visits.findIf([&](const auto& open) { return open.get() == &visit; });
        }
        TrialVisit& open(size_t frameIndex, BlockID inputHead, uint32_t summaryIndex)
        {
            m_visits.append(makeUniqueWithoutFastMallocCheck<TrialVisit>());
            TrialVisit& visit = last();
            visit.frameIndex = frameIndex;
            visit.inputHead = inputHead;
            visit.summaryIndex = summaryIndex;
            return visit;
        }
        std::unique_ptr<TrialVisit> takeLast() { return m_visits.takeLast(); }
        void shrink(size_t size) { m_visits.shrink(size); }

    private:
        Visits m_visits;
    };
    struct SnapshotForTry;
    Vector<LoopSummary> m_loopSummaries;
    using LoopSummaryKey = std::pair<uint64_t, uint64_t>;
    LoopSummaryKey keyOfCopyOfLoop(size_t frameIndex, BlockID inputHead) const;
    static LoopSummaryKey loopSummaryKey(LoopSummaryKey copyOfLoop, uint32_t numberOfEntriesBefore);
    HashMap<LoopSummaryKey, uint32_t, DefaultHash<LoopSummaryKey>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_entriesOfCopiesOfLoops;
    Vector<LoopSummaryKey>& m_entriesOfCopiesOfLoopsInTries;
    HashMap<LoopSummaryKey, uint32_t, DefaultHash<LoopSummaryKey>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_loopSummaryIndices;
    decltype(m_loopSummaryIndices) m_summariesRevisitedForCheck;
    Vector<std::shared_ptr<const LoopSummary>> m_summariesAtLastBuildVisit;
    decltype(m_loopSummaryIndices) m_unguardedSumLoops;
    decltype(m_loopSummaryIndices) m_rowsWhoseKeysAreReadOffLook;
    decltype(m_loopSummaryIndices) m_loopsWithoutFieldBounds;
    TrialVisitStack& m_trialVisits;
    Vector<ArrivalHeld> m_arrivalsHeld;
    uint32_t m_trialVisitDepth { 0 };
    uint32_t m_nestingDepth { 0 };
    uint32_t m_numberOfTryWhoseWalkIsCutShort { 0 };
    uint32_t m_numberOfMarksTaken { 0 };
    uint32_t m_serialOfNewestMark { 0 };
    void setSerialOfNewestMark(uint32_t serial)
    {
        m_serialOfNewestMark = serial;
        m_openEdges.setSerialOfNewestMark(serial);
        m_indexedIterations.setSerialOfNewestMark(serial);
        m_stringsInBuilders.setSerialOfNewestMark(serial);
        m_openLoops.setSerialOfNewestMark(serial);
    }
    struct StateOfHandlerInTry {
        uint32_t frame { 0 };
        BlockID handler;
        std::optional<StateOfHandler> before;
    };
    Vector<StateOfHandlerInTry>& m_statesOfHandlersInTries;
    void keepStateOfHandlerForUndo(size_t frameIndex, BlockID handler);
    struct PlaceOfOpenLoopInTry {
        uint32_t frame { 0 };
        LoopID loop;
        uint32_t before { 0 };
    };
    Vector<PlaceOfOpenLoopInTry>& m_placesOfOpenLoopsInTries;
    Vector<std::pair<uint32_t, BlockID>, 4> m_loopsInFinalBuild;
    struct ConstantTripLoop {
        size_t frameIndex { 0 };
        LoopID loop;
        BlockID lastDominator;
    };
    Vector<ConstantTripLoop, 2> m_constantTripLoops;
    BlockID blockAroundConstantTrips(size_t frameIndex, BlockID, LoopID around) const;
    struct InputBlockInTry {
        uint32_t frame { 0 };
        BlockID block;
        InputBlock before;
    };
    Vector<InputBlockInTry>& m_inputBlocksInTries;
    struct ExpectationInTry {
        uint32_t frame { 0 };
        ValueID value;
        RememberedExpectation before;
    };
    Vector<ExpectationInTry>& m_expectationsInTries;
    struct FrameLogSizes {
        size_t bindings { 0 };
        size_t inputBlocks { 0 };
        size_t expectations { 0 };
        size_t statesOfHandlers { 0 };
        size_t placesOfOpenLoops { 0 };
    };
    Vector<FrameLogSizes, 1>& m_frameLogSizes;
    void renameFrameInTries(size_t from, size_t to);
    TypeID typeInRowOfValueCarriedRound(BlockID inputHead, size_t placeAmongParameters, TypeID entering);
    bool m_buysTwoBuilds { false };
public:
    void noteTestSeenInTries(Reference value, ValueID operation = { }, TypeID tested = { });
    void noteGuardOfStoresAskedInTries(const MemberConditionRecord&);
    bool isAllocatedInTrialIteration(Reference object) const { return !m_trialVisits.isEmpty() && object && object.isValue() && object.id().index() >= m_trialVisits.last().firstRecord; }
    void noteBirthInTries(LayoutID classLayout);
    void noteGuardsBegunInTries(LayoutID classLayout);
    bool turnBeginsGuardsOf(LayoutID classLayout) const { return !m_trialVisits.isEmpty() && m_trialVisits.last().classesWhoseGuardsTurnBegins.contains(classLayout); }
    bool turnMakesObjectAtOrBelow(LayoutID classLayout) const;
    void noteClassesOfBirthsAtOrBelowInTries(LayoutID classLayout);
    void noteStaticFieldInTries()
    {
        if (!m_trialVisits.isEmpty())
            m_trialVisits.last().looksAtStaticField = true;
    }
    void noteMemberAskedByItsByteInTries(const BodyValue& read, TypeID typeOfHolder);
    std::span<const ValueID> operationsThatRanOtherCodeOfRow() const LIFETIME_BOUND { return m_operationsThatRanOtherCodeOfRow.span(); }
    const LoopPreview* previewOutOfRow() const { return m_previewOutOfRow; }
    bool listsAreStruckInRow() const { return m_listsAreStruckInRow; }
    bool isInt32VersionRejected() const { return m_isInt32VersionRejected; }
    bool isInUnswitchedVersion() const { return !m_versionedLoops.isEmpty() && !m_versionedLoops.last().unswitchCandidates.empty(); }
    bool isQuietVersionOfUnswitchLoop() const;
    bool isQuietIterationOfUnswitchLoop() const;
    bool isInInt32Version() const { return isInUnswitchedVersion() && std::ranges::all_of(m_versionedLoops.last().unswitchCandidates, &UnswitchCandidate::asksForInt32s); }
    void noteInt32Candidate();
    void noteInt32Guard();
    bool hasUnrolledInnerLoops(BlockID inputHead);
    void noteInt32Version() { m_hasInt32Version = true; }
    const Effects& effectsOfQuietDraftsOfRow() const { return m_effectsOutsideArmsOfRow; }
    class HeardAsArm {
    public:
        explicit HeardAsArm(Walker& walker) : m_walker(walker) { ++walker.m_numberOfFormsHeardAsArms; }
        ~HeardAsArm() { --m_walker.m_numberOfFormsHeardAsArms; }
    private:
        Walker& m_walker;
    };
    class WeighsWhatIsWritten {
    public:
        WeighsWhatIsWritten(Walker& walker, bool isWritten) : m_scope(walker.m_weighsWhatIsWritten, isWritten) { }
    private:
        SetForScope<bool> m_scope;
    };
    bool weighsWhatIsWritten() const { return m_weighsWhatIsWritten; }
private:
    bool m_weighsWhatIsWritten { false };
    uint32_t m_numberOfFormsHeardAsArms { 0 };
    bool m_isWalkingLastAlone { false };
    Vector<ValueID, 4> m_operationsThatRanOtherCodeOfRow;
    Vector<MemberConditionRecord, 2> m_guardsOfStoresAskedOfRow;
    const LoopPreview* m_previewOutOfRow { nullptr };
    bool m_listsAreStruckInRow { false };
    bool m_isInt32VersionRejected { false };
    bool m_hasInt32Version { false };
    bool m_otherCodeStaysInTypedBuildOfRow { false };
    bool m_otherCodeIsKeptOutOfFirstCopyInVainOfRow { false };
    bool m_otherCodeStayedInVainOfRow { false };
    bool m_readsAskSitesThoughCallStaysOfRow { false };
    bool m_sumsAreNotAskedInRow { false };
    bool m_keysAreReadOffLookInRow { false };
    bool m_skipsFieldBoundsInVersion { false };
    bool m_skipsFieldBounds { false };
    bool m_typedVersionChecksFieldBound { false };
    bool m_keyWasTakenForCounterAskedInFront { false };
    bool m_walksCopyThatAsksCountersAlone { false };
    bool m_hoistsConditionalArrays { false };
    bool m_typedBuildEndedListsTestedInFront { false };
    BlockID m_headOfTypedBuildOverListsTestedInFront;
    size_t m_frameOfTypedBuildOverListsTestedInFront { 0 };
    Vector<uint32_t, 2> m_listsMadeHereOfTypedBuildOverListsTestedInFront;
    Vector<std::pair<uint32_t, uint32_t>, 4> m_distinctAllocations;
    Effects m_effectsOfLoopKeptLast { Effects::none() };
    Vector<MemberConditionRecord, 2> m_guardsOfStoresAskedInLoopKeptLast;
    bool m_typedBuildAsksGuardsThatItWasBoughtFor { false };
    Vector<ValueID, 2> m_inheritedEscapingUses;
    StoresIntoScopes m_inheritedScopeStores;
    BlockID m_inheritedSummaryHeader;
    size_t m_inheritedSummaryFrame { 0 };
    Vector<TypeID, 8> m_inheritedEntryTypes;
    Vector<TypeID, 8> m_inheritedParameterTypes;
    Vector<uint8_t, 8> m_inheritedWideningRungs;
    uint64_t m_inheritedRarelyWidenedParameters { 0 };
    uint64_t m_inheritedOverreadWidenedParameters { 0 };
    Vector<ValueID, 8>& m_operationsBeingWalkedInFrames;
    bool m_firstOtherCodeRanInCopyOfRow { false };
    bool m_walksCopyWhereItsGuardFailed { false };
    bool leaveFusedChain(const Draft&, const Effects&);
    bool exitFusedChainUnlessHidden(const Effects&);
    bool fillsListOfFusedChain(const Draft&, const Effects&) const;
    bool isElementReadByName(Reference argument, const BodyIndex& callback, uint32_t parameter) const;
    bool mayLeaveFusedChainHere() const { return m_frameOfFusedChain && !isUnreachable() && m_numberOfLoopsThatMayBeLeft >= m_openLoops.size(); }
    BlockID blockWhereFusedChainIsLeft();
    Reference requireCoercibleInFusedChain(const Draft&);
    void returnKeyOfComparator(ValueID key);
    BlockID* m_armOfNextFusedChain { nullptr };
    ValueID m_keyOfNextCopy;
    uint32_t m_frameOfFusedChain { 0 };
    bool m_fusesNoChain { false };
    bool m_exitsChainAtGenericLoop { false };
    ArrayStorage m_storageOfFusedChain { ArrayStorage::Other };
    bool m_nextCopyIsOfMethodThatProtocolCalls { false };
    LazyMap<HashMap<const BodyIndex*, Vector<QuestionAskedByCall>>> m_questionsAskedByCallsOfEntryBlocks;
    Vector<ExpectationOfArgument, 2> m_expectedOfArgumentsOfCopies;
    bool m_summaryHasUninlinedCall { false };
    bool m_turnsOfRowAskLengthsOfViewsFromInFrontForWrittenPlaces { false };
    bool m_turnsOfRowTakeApartListsFromInFront { false };
    bool m_versionIterationsCheckStaticFields { false };
    bool m_handlersRunOtherCode { false };
    bool m_everyTurnOfTypedBuildLeaves { false };
    bool m_readWasKeptFromItsSiteBecauseCallStays { false };
    bool m_turnOfTypedBuildLeavesWithoutTest { false };
    bool m_innerLoopExitsOnLikelyPath { false };
    bool m_typedBuildTestsNothing { false };
    bool m_typedBuildIsLeftAtOtherCodeOnUsualWay { false };
    bool m_sumOfTypedBuildComesRoundUnproven { false };
    bool m_rowSaysTurnsAreQuietOutsideArms { false };
    Effects m_effectsOutsideArmsOfRow { Effects::none() };
    Vector<GuessCountedOnByLooks, 4> m_testsSeenOfRow;
    const ScopedLambda<void()>* m_missingTestFinder { nullptr };
    void findMissingTestsOfRow()
    {
        if (const ScopedLambda<void()>* finder = std::exchange(m_missingTestFinder, nullptr))
            (*finder)();
    }
    std::optional<Effects> m_effectsSeededForNextRow;
    bool m_isVersioningDryRun { false };
    struct OperatorMadeInFrontOfLoop {
        uint32_t numberOfFrames { 0 };
        ValueID operation;
        Reference name;
    };
    Vector<OperatorMadeInFrontOfLoop, 2> m_operatorsMadeInFrontOfLoops;
    HoistedRows m_hoistedRows;
    Vector<FieldArrayRead, 2> m_fieldArrayReads;
    uint64_t m_ordinalOfCopyOfLoop { 0 };
    bool m_walksCopyWithoutSomeIterationGuesses { false };
    bool m_walksCopyWithoutQuestionsOfEveryTurn { false };
    bool m_walksCopyForAnotherKindOfStorage { false };
    bool m_walksLoopOverBoxedNumbers { false };
    bool runTrialVisits(BlockID inputHead, std::span<const Reference> entering);
    LoopPreview makePreviewFromSummary(BlockID inputHead, std::span<const TypeID> entryTypes, const EqualityLoopTest*);
    TypeID climbLadderOfLoop(LoopSummary&, size_t parameter, TypeID assumed, TypeID found, bool isFirstInTry);
    bool noteWhatCameRoundInTry(BlockID head, size_t parameter, TypeID assumed, TypeID found, size_t numberOfTriesAround = std::numeric_limits<size_t>::max(), TypeID* enteredUnderGuessedStart = nullptr);
    const TrialVisit* tryOfHeadParameter(ValueID headParameter, size_t& parameter) const;
    bool startIsGuessedInWalk(const TrialVisit&, size_t parameter) const;
    void startAtCarriedType(LoopSummary&, BlockID inputHead, uint32_t parameter, std::span<const TypeID> entryTypes);
    TypeID typeCarriedFrom(const TrialVisit&, size_t parameter, TypeID start) const;
    uint64_t carriedStartsInVain(const TrialVisit&);
    TypeID climbHeadAroundWith(const TrialVisit&, size_t numberOfTriesAround, size_t parameter, TypeID found);
    void noteOwnParametersThatMeetInValue(TrialVisit&, size_t parameter, ValueID, BlockID head);
    bool walkAroundIsUndone() const;
    bool isWrittenInTopFrame(const Draft& draft) const { return draft.inputValueGiven && m_origin.inlineFrame == m_frames.last().inlineFrame; }
    bool takesWhatRowOfInputOperandCannotGive(const Draft&) const;
    ASCIILiteral m_whatJudgeOfOperandsRefused;
    void letParametersHoldWhatTheyTakeRound(LoopSummary&, const TrialVisit&, BlockID head);
    void noteAllThatComesRoundInTry(BlockID head, size_t parameter, TypeID found, Reference);
    uint64_t findRarelyWidenedParameters(const TrialVisit&, const LoopSummary&, BlockID head);
    uint64_t findOverreadWidenedParameters(const TrialVisit&, const LoopSummary&, BlockID head) const;
    struct SourcesOfHeadParameter {
        TypeID made;
        Vector<ValueID, 2> headParametersHandedOn;
        uint32_t summaryIndex { 0 };
        uint32_t parameter { 0 };
        uint32_t place { 0 };
    };
    HashMap<uint32_t, SourcesOfHeadParameter, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_sourcesOfHeadParameters;
    ValueID headParameterBehind(Reference) const;
    TypeID typeThatComesRoundBesideParameterItself(ValueID headParameter, Reference value) const;
    void noteHeadParametersThatEnter(TrialVisit&, std::span<const Reference> entering) const;
    void handUpWhatCameRoundBesideParameters(const TrialVisit&);
    struct WhatMeetsInParameter {
        ValueID headParameter;
        TypeID beside;
        uint32_t firstRecord { 0 };
    };
    mutable HashMap<uint32_t, WhatMeetsInParameter, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_whatMeetsInParameters;
    template<typename Functor> void forEachValueThatMeetsIn(ValueID, BlockID openHead, uint32_t firstRecord, const Functor&) const;
    void noteSourcesOfHeadParameters(size_t frameIndex, BlockID inputHead, uint32_t summaryIndex, const TrialVisit&, std::span<const Reference> entering);
    Vector<uint32_t>& m_headParametersWithSources;
    Vector<TypeID, 16> typesThatReachHeadParametersOfNest(uint32_t firstRecordOfNest, size_t firstHeadParameter, size_t endOfHeadParameters);
    template<typename IsRoot, typename TypeAtStart, typename Take> static void carryTypesAlongRoad(TypeArena&, std::span<const RecordThatMakes> road, uint32_t numberOfRecords, const IsRoot&, const TypeAtStart&, const Take&);
    void narrowRowsInsideTo(const TrialVisit::NarrowingThatWaits&);
    bool narrowerTypeOfHeadPaysWalk(TypeID assumed, TypeID narrowed, bool turnsCheckTheirArithmetic);
    bool narrowSumsToNumbers(BlockID inputHead, LoopSummary&, const TrialVisit&);
    bool turnsCheckArithmeticOnIntegers(BlockID inputHead);
    void narrowSummaryToObservedTypes(LoopSummary&, const TrialVisit&);
    bool anyElementProvenInBoundsSince(const LoopSummary&, const TrialVisit&) const;
    Vector<bool, 8> findParametersSteppedOnDoubles(BlockID inputHead, const LoopSummary&);
    Vector<bool, 8> findParametersSteppedOnDoubles(BlockID inputHead, const LoopPreview&);
    Vector<bool, 8> findParametersSteppedOnDoubles(BlockID inputHead, bool isBeforeVisit, const ScopedLambda<TypeID(ValueID)>& typeInRow);
    struct MentionInLoop {
        ValueID value;
        bool isReadAlone { false };
        bool mayOnlyBorrowScope { false };
    };
    std::span<const MentionInLoop> mentionsBeforeLoop(LoopID);
    void letMentionsOfLoopLeave(BlockID inputHead, std::span<const ValueID> mentionsThatLeave = { }, Vector<FreshObjectKeptAtHead, 2>* kept = nullptr, bool keepsScopes = true);
    struct BelievedAheadOfEdges {
        BlockID block;
        WalkState state;
    };
    bool m_leadsIntoPlainWalkThatIsKept { false };
    Vector<BelievedAheadOfEdges, 2> m_believedAheadOfEdges;
    void standAheadOfEdges(BlockID);
    ALWAYS_INLINE void holdWhatArrives(BlockID to)
    {
        BeliefAheadOfEdges belief = m_code.at(to).beliefAheadOfEdges;
        if (belief != BeliefAheadOfEdges::NoEdgeIsAwaited && belief != BeliefAheadOfEdges::Nothing) [[unlikely]]
            holdWhatArrivesWhereEdgesAreAwaited(to);
    }
    void holdWhatArrivesWhereEdgesAreAwaited(BlockID to);
    void forgetWhatWasBelievedAt(BlockID);
    TrialVisit* findTryOfBlockAheadOfEdges(BlockID);
    void loseObjectKeptAtHead(TrialVisit&, const FreshObjectKeptAtHead&);
    void handBackScopesThatBuildLetsLeave(std::span<const ValueID> mentionsThatLeave);
    bool mentionMayOnlyBorrowScopes(const BodyValue&, uint32_t operandIndex);
    void keepScopesAtMentions(Vector<ValueID, 4>& mentions, std::span<const ValueID> mentionsThatLeave, Vector<FreshObjectKeptAtHead, 2>& kept);
    void handScopesKeptAtHeadToPreview(const BodyIndex&, BlockID inputHead, const StoresIntoScopes&, std::span<const FreshObjectKeptAtHead>, LoopPreview&);
    bool learnWhatTurnsStoreIntoScopes(LoopSummary&, const TrialVisit&, bool walkIsWhole);
    void noteCapturedCounter(TrialVisit&, const FieldOfScope&, Reference comingRound);
    bool recordLoopSlots(LoopSummary&, TrialVisit&);
    bool recordSlotsStoredByCopies(LoopSummary&, TrialVisit&);
    void noteSlotOfHeadParameter(ValueID parameter, PropertyKeyID);
    void noteSlotOfParameter(ValueID parameter, PropertyKeyID);
    Reference readSlotOnEdge(EdgeID&, BlockID to, const FieldOfScope&, Reference object);
    void findSlotsOfObjectsThatMeet(std::span<const uint32_t> edges, std::span<const uint32_t> joined, std::span<const uint32_t> joinedNames, std::span<const ValueID> parameters, std::span<const ObjectsMetAtJoin>, Vector<FieldOfScope, 4>&);
    void noteSlotsThatComeRound(TrialVisit&, std::span<const Reference> arguments);
    bool specializesLoop(size_t frameIndex, BlockID inputHead) const { return !m_versionedLoops.isEmpty() && m_versionedLoops.last().frameIndex == frameIndex && m_versionedLoops.last().inputHead == inputHead; }
    void inheritMergedObjects(BlockID inputHead, std::span<const Reference> entering, Vector<MergedHeaderObject, 1>&);
    Vector<Reference, 8> allocateMergedObjectsAtRestart(std::span<const Reference> valuesAtHeader);
    Vector<Reference, 8> allocateMergedObjectsIn(std::span<const Reference>);
    bool isNamedAtHead(const WalkState&, ValueID cell, BlockID inputBlock, const ScopedLambda<Reference(ValueID)>& nameOf);
    bool canTakeOverObject(ValueID cell) const;
    bool isPassedToCopiesAlone(ValueID cell) const;
    void keepCopiesTakingOver(ValueID cell);
    bool addElementsOfMergedArray(const MergedHeaderObject&, TypeID, std::span<const Reference> entering, bool isWidened, Vector<FieldOfScope, 4>& known);
    void findMergedHeaderObjects(BlockID inputHead, std::span<const Reference> entering, Vector<MergedHeaderObject, 1>&);
    TypeID typeOfMergedSlot(const FieldOfScope&, bool isWidened);
    void widenMergedObject(TrialVisit&, uint32_t position);
    bool readsMergedElements(BlockID head, uint32_t position) const;
    void addMergedObjectSlots(const LoopPreview&, std::span<const Reference> entering, Vector<MergedHeaderObject, 1>&, Vector<FieldOfScope, 4>& known);
    void dropIncompleteMergedObjects(Vector<MergedHeaderObject, 1>&, Vector<FieldOfScope, 4>& known);
    void noteMergedHeaderObjects(BlockID head, Vector<MergedHeaderObject, 1>&&);
    bool mayNameObjectsInGenerators() const;
    void bindMergedHeaderObjects(BlockID head);
    bool usesBoundHeaderParameters() const;
    void bindMergedJoinObjects(BlockID inputBlock, std::span<const uint32_t> joined, std::span<const ValueID> parameters, std::span<const ObjectsMetAtJoin>);
    void allocateMergedObjectsOnEdge(EdgeID&, BlockID to, BlockID inputBlock, const OpenEdge&, std::span<const uint32_t> joined, std::span<const uint32_t> joinedNames, std::span<const ObjectsMetAtJoin>, Vector<Reference, 8>& arguments);
    ValueID parameterOfHeaderAllocation(BlockID head, ValueID name) const;
    void checkMergedHeaderObjects(BlockID head, std::span<const Reference> arguments);
    void collectMergedObjectSlots(LoopSummary&, BlockID inputHead, BlockID head);
    void updateMergedObjects(LoopSummary&, std::span<const Reference> entering);
    bool learnSlotsOfObjectsThatComeRound(LoopSummary&, const TrialVisit&);
    bool canCarrySlotType(TypeID type) { return m_types.isSubset(type, m_types.withPrimitives(TypeTable::number, Primitive::Undefined)) && formOfParameter(type, { }) != Representation::JSValue; }
    void notePositions(const BodyValue&);
    bool wordCutsWalkAtThisHearing();
    ALWAYS_INLINE bool walkIsCutShortHere()
    {
        if (!m_numberOfTryWhoseWalkIsCutShort || m_hasFailed) [[likely]]
            return false;
        m_hasFailed = true;
        endAsUnreachable(m_origin);
        return true;
    }
public:
    void recordTrialEffectsOfWriter(const Effects&, const std::optional<Vector<uint32_t, 2>>& objectsMadeHere);
    ALWAYS_INLINE void recordTrialEffects(const Effects& built)
    {
        if (m_trialVisits.isEmpty())
            return;
        if (m_inputs.options.loopVisitAbortPeriod && wordCutsWalkAtThisHearing()) [[unlikely]] {
            Effects builtAndUnknownCode = Effects::unknownCode();
            builtAndUnknownCode.merge(built);
            builtAndUnknownCode.maySuspend = built.maySuspend;
            recordTrialEffectsSlow(builtAndUnknownCode);
            return;
        }
        if (!built.isPure())
            recordTrialEffectsSlow(built);
    }
private:
    void noteUnswitchCandidate(const BodyTerminator& branch);
    bool mayUnswitchLoop(BlockID inputHead, OtherClassBelowMixin&);
    BitVector findLoopsToUnswitch(FunctionID, const BodyIndex&);
    bool m_buildsCallWithoutReturn { false };
    HashMap<const BodyIndex*, BitVector> m_loopsToUnswitch;
    LazyMap<HashMap<const BodyIndex*, Vector<uint64_t>>> m_holdersStoredByName;
    const Vector<uint64_t>& holdersStoredByName(const BodyIndex&);
    struct SteppedName {
        ValueID holder;
        PropertyKeyID key;
        BlockID block;
    };
    LazyMap<HashMap<const BodyIndex*, Vector<SteppedName>>> m_namesSteppedInLoops;
    LazyMap<HashMap<const BodyIndex*, Vector<ElementReads>>> m_elementReadsOfLoops;
    LazyMap<HashMap<const BodyIndex*, Vector<ElementAccess>>> m_elementAccesses;
    HashSet<std::pair<uint32_t, uint32_t>, DefaultHash<std::pair<uint32_t, uint32_t>>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_rejectedUnswitchLoops;
    HashMap<LoopSummaryKey, std::shared_ptr<const LoopSummary>, DefaultHash<LoopSummaryKey>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_readerLoopSummaries;
    std::pair<uint32_t, BlockID> m_loopRestartingOuterIteration { };
    bool m_innerLoopHasEffects { false };
    bool restartsOuterIteration(BlockID inputHead) const { return m_loopRestartingOuterIteration.second && m_loopRestartingOuterIteration == std::pair { static_cast<uint32_t>(m_frames.size() - 1), inputHead }; }
    bool mayRestartOuterIteration(BlockID inputHead);
    LoopSummaryKey loopKeyAcrossCopies(size_t frameIndex, BlockID inputHead);
    bool isRecordUnswitchable(uint32_t openLoop, ValueID record, unsigned depth, LocationMasks& reads) const;
    bool isConditionUnswitchable(uint32_t openLoop, ValueID, unsigned depth, LocationMasks& reads) const;
    bool isQuietOnceUnswitched(LoopID, std::span<const ValueID> ranOtherCode, std::span<const UnswitchCandidate>, Vector<ValueID, 4>& operationsOutsideCandidates) const;
    enum class UnswitchedIterations : bool { AsWalked, AreQuiet };
    bool canHoistUnswitchRead(uint32_t openLoop, ValueID record, unsigned depth, UnswitchedIterations = UnswitchedIterations::AsWalked) const;
    bool canHoistUnswitchCondition(uint32_t openLoop, ValueID, unsigned depth, UnswitchedIterations) const;
    Reference buildUnswitchRead(uint32_t openLoop, ValueID record, unsigned depth);
    ValueID siteReadBehind(ValueID record) const;
    bool canHoistSiteRead(uint32_t openLoop, ValueID record, unsigned depth, UnswitchedIterations = UnswitchedIterations::AsWalked) const;
    void hoistSiteReads(uint32_t openLoop, ValueID, unsigned depth);
    Reference buildUnswitchCondition(uint32_t openLoop, ValueID, unsigned depth);
    Reference hoistUnswitchCondition(uint32_t openLoop, ValueID);
    bool tryUnswitchBranch(const BodyTerminator&);
    void cutUnswitchedWalkShort();
    struct UnswitchCollector {
        size_t frameIndex { 0 };
        BlockID inputHead;
        Vector<UnswitchCandidate, 2> candidates;
        bool collectsInt32s { false };
        bool collectsBranches { true };
        OtherClassBelowMixin otherClass { };
        bool holdsLoops { false };
    };
    Vector<UnswitchCollector, 2> m_unswitchCollectors;
    std::span<const UnswitchCandidate> m_unswitchCandidates;
    OtherClassBelowMixin m_otherClassBelowMixin;
    BlockID m_unswitchFallbackStart;
    bool m_isUnswitchedVersionQuiet { false };
    bool m_hasUnswitchFailed { false };
    NEVER_INLINE void recordTrialEffectsSlow(const Effects& effects)
    {
        if (!effects.mayRunOtherCode()) {
            TrialVisit& innermost = m_trialVisits.last();
            innermost.done.merge(effects);
            innermost.writtenWithoutOtherCode.merge(effects.writes);
            if (!m_numberOfClosedArms && !m_numberOfFormsHeardAsArms) {
                innermost.doneOutsideArms.merge(effects);
                innermost.doneByQuietDrafts.merge(effects);
            }
            return;
        }
        bool isInHandler = m_block && m_code.at(m_block).frequency == BlockFrequency::Unlikely;
        for (TrialVisit& trialVisit : m_trialVisits) {
            trialVisit.done.merge(effects);
            trialVisit.handlersRunOtherCode |= isInHandler && (&trialVisit == &m_trialVisits.last());
            if (effects.mayRunOtherCode() && trialVisit.knowsWhatIsAssumed && !trialVisit.assumed.mayRunOtherCode() && !m_numberOfTryWhoseWalkIsCutShort && !m_hasFailed)
                m_numberOfTryWhoseWalkIsCutShort = static_cast<uint32_t>(m_trialVisits.placeOf(trialVisit)) + 1;
            if (!m_numberOfClosedArms && !m_numberOfFormsHeardAsArms) {
                if (effects.mayRunOtherCode() && trialVisit.frameIndex < m_operationsBeingWalkedInFrames.size()) {
                    ValueID operation = m_operationsBeingWalkedInFrames[trialVisit.frameIndex];
                    if (!trialVisit.doneOutsideArms.mayRunOtherCode())
                        trialVisit.firstOtherCodeRanInCopy = m_frames.size() - 1 > trialVisit.frameIndex;
                    ValueID innermost = m_frames.size() <= m_operationsBeingWalkedInFrames.size() ? m_operationsBeingWalkedInFrames[m_frames.size() - 1] : ValueID();
                    trialVisit.hasUninlinedCall |= !innermost || innermost.index() >= currentBodyIndex().numberOfValues() || (!currentBodyIndex().isParameter(innermost) && effectRuleOf(currentBodyIndex().opcodeOf(innermost)) == EffectRule::Call && !hasFewExpectedCallees(*this, currentBodyIndex().value(innermost)) && !isMathCall(*this, currentBodyIndex(), currentBodyIndex().value(innermost)));
                    if (trialVisit.operationsThatRanOtherCode.isEmpty() || trialVisit.operationsThatRanOtherCode.last() != operation)
                        trialVisit.operationsThatRanOtherCode.append(operation);
                }
                trialVisit.doneOutsideArms.merge(effects);
                if (!effects.mayRunOtherCode())
                    trialVisit.doneByQuietDrafts.merge(effects);
            }
        }
    }
    void layBlockOnEdge(EdgeID&, BlockID to);
    bool passesUnescapedObject(std::span<const Reference> arguments) const;
    bool makeReadyToCross(BlockID to, std::span<const Reference> arguments, bool mayBuild, ArgumentsOfEdge&);
    void crossEdge(EdgeID, BlockID to, std::span<const Reference> arguments);
    bool canEnterWith(BlockID entry, std::span<const Reference> besideFieldsOfScopes) const;
    void enterWithFieldsOfScopes(BlockID entry, std::span<const Reference> besideFieldsOfScopes);
    bool canEnterGenericVersion(BlockID head, std::span<const Reference> besideFieldsOfScopes);
    void jumpToGenericVersion(BlockID head, std::span<const Reference> besideFieldsOfScopes);
    Reference askAgainWhereItComesRound(EdgeID&, BlockID head, TypeID heldTo, std::span<const Reference> besideFieldsOfScopes, const ScopedLambda<Reference()>& whatComesRound);

    using ValuesBuiltInFront = SharedArray<uint32_t, 8>;
    struct OpenLoop {
        BlockID preheaderBlock;
        BlockID head;
        std::shared_ptr<const ArgumentsOfEdge> entering;
        std::span<const ValueID> recordsEntering() const { return entering ? entering->span() : std::span<const ValueID>(); }
        Vector<BlockID, 2> earlierPreheaderBlocks;
        ValuesBuiltInFront builtInFront;
        SharedArray<uint32_t, 0, 8> tableOfBuiltInFront;
        Effects effects;
        mutable BlockID backEdgeDominator;
        mutable BlockID exitDominator;
        mutable bool hasFoundExitDominator { false };
        bool hasEntryTestInPreheader { false };
        bool hasImplicitExit { false };
        uint32_t implicitExitScanEnd { 0 };
        bool hasVisibleWork { false };
        uint32_t visibleWorkScanEnd { 0 };
        bool hasBuiltinRead { false };
        uint32_t builtinReadScanEnd { 0 };
        EdgeID guardExitScanEnd;
        uint32_t carriedResultCount { 0 };
        uint32_t carriedResultsSummary { 0 };
        Vector<uint32_t, 2> carriedSlotsBuilt { };
        struct CarriedElement {
            uint32_t slot;
            uint32_t counterPosition;
            int32_t offset;
            Reference array;
            bool isRead { false };
            bool isMissingOnBackEdge { false };
        };
        Vector<CarriedElement, 1> carriedElements { };
        struct CarriedField {
            uint32_t slot;
            Reference object;
            LayoutSetID receivers;
        };
        Vector<CarriedField, 1> carriedFields { };
        std::shared_ptr<const WalkState> stateAtHeader { };
        uint32_t frame { 0 };
        LoopID loop;
        uint32_t firstRecord { 0 };
        Origin origin { };
        ValueID poll;
        ValueID pollInFront;
        uint32_t numberOfEdgesThatCameRound { 0 };
        bool likelyTurnMayCollect { false };
        Vector<ValueID, 2> lettersMadeInLikelyTurns;
        Vector<ValueID, 2> pollsInFrontOfLoopsInside;
        struct EdgeHeldBack {
            EdgeID edge;
            ArgumentsOfEdge arguments;
        };
        BlockID firstReloadBlock;
        Vector<BlockID, 2> reloadBlocks;
        ArgumentsOfEdge heldByFront;
        BlockID reloadFallbackBlock;
        Vector<EdgeHeldBack, 2> edgesHeldBack;
        uint32_t restartPoint { 0 };
        Reference effectsCheckBeginning { };
        mutable bool holdsReadsInFront { false };
        bool watchesTurns { false };
        uint32_t summaryIndexPlusOne { 0 };
        LocationMasks placesThatQuietTurnsWrite;
        struct LookupAfterLookup {
            ValueID earlier;
            PropertyKeyID key;
            ValueID result;
        };
        Vector<LookupAfterLookup> lookupsAfterLookups;
    };
    static constexpr uint8_t someWayRoundDidNothing = 1;
    static constexpr uint8_t someReadCouldStandInFront = 2;
    static constexpr uint8_t wasWalkedAgainForFront = 4;
    void noteWhatTurnsOfLoopShow(const OpenLoop&, uint8_t shown);
    size_t m_numberOfFieldsOfScopesOfHeadBeingOpened { 0 };
    uint32_t m_pendingCarriedResultCount { 0 };
    HashMap<uint32_t, Vector<CarriedResult>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_carriedResultRequests;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_loopsWhoseTextsStayOutOfBuilders;
    BitVector blocksThatSomeQuietTurnPassesThrough(BlockID inputHead, std::span<const ValueID> ranOtherCode);
    bool wayBehindOperationComesRound(BlockID inputHead, ValueID operation) const;
    void noteCallOnUsualWayInTries(const Effects& heard);
    bool isCallQuietForArguments(FunctionID callee) const;
    bool someTextGrowsInQuietTurn(BlockID inputHead, std::span<const GrowingText>, const TrialVisit&);
    bool someIterationLeavesStringsUnchanged(BlockID inputHead, std::span<const GrowingText>);
    void keepTextsOfLoopOutOfBuilders(BlockID inputHead);
    void endStringsInBuildersFrom(size_t numberOfTexts);
    void letGoOfTextsThatLoopTook(BlockID inputHead);
    bool m_walkOfLoopBeganTexts { false };
    void letFrontOfQuietTurnsFailUnasked(uint32_t indexOfLoop);
    bool quietTurnsWroteMoreThanRowSays(LoopSummary&, const TrialVisit&);
    bool frontOfQuietTurnsIsWorthAWalk(LoopSummary&, const TrialVisit&);
    void noteReadThatCouldStandInFront(const OpenLoop&, const LocationMasks& reads) const;
    void layFrontOfQuietTurns(OpenLoop&, BlockID inputHead);
    void noteHeadOfQuietTurns(BlockID head);
    bool mayReadInFrontOfQuietTurns(uint32_t indexOfLoop) const;
    bool isReadForCall(const Draft&) const;
    bool hasOnlyKnownEffectsInIteration(uint32_t restartPoint) const;
    bool holdBackEdgeOfTurnThatDidSomething(EdgeID&, BlockID head, std::span<const Reference> arguments);
    BlockID closeFrontOfQuietTurns(const OpenLoop&);
    void noteBlockOfFrontOfQuietTurns(uint32_t indexOfLoop, BlockID);
    bool belongsInFrontOfQuietTurns(uint32_t indexOfLoop, const Draft&, const Effects&) const;
    BlockID rowInFrontOfLoopFor(uint32_t indexOfLoop, const Draft&, const Effects&);
    bool branchInFrontOfQuietTurns(uint32_t indexOfLoop, Reference test, bool passesWithYes);
    void openLoop(OpenLoop&&);
    void layChecksOfParametersOfHead(BlockID head, uint32_t numberOfParametersOfInput);
    void askParametersOfHeadsForWhatIsShown();
    void dropChecksProvedAfterLoops();
    TypeID typeClaimedOfHeadParameter(ValueID, bool mayBeEmptyWord);
    void closeNewestLoop();
    bool leaveBlockAnotherWay(const BodyTerminator&);
    NEVER_INLINE bool enterIterationAtHint();
    void splitHeaderTestedAtEnd(BlockID inputHead);
    Reference boundOfEndLoopTest(BlockID inputHead, const EndLoopTest&);
    bool branchOnEndTest(const BodyTerminator&);
    bool branchOnCounterRelation(const BodyTerminator&);
    bool m_normalizesLoopTests { false };
    std::optional<uint32_t> onlyTargetWhereAllComeToOnePlace(const BodyTerminator&) const;
    bool branchOnTestBelowNegations(const BodyTerminator&);
    bool branchOnHoistedComparisons(const BodyTerminator&);
    bool branchOnSwitchedStringLength(const BodyTerminator&);
    std::optional<uint32_t> m_edgeOfCodeBeingLed;
    Reference m_negatedTest;
    bool handlerInForceIgnoresWhatIsThrown() const;
    bool jumpToHandlerInPlaceOfThrow(const BodyTerminator&);
    void walkOperationsOf(BlockID inputBlock);
    bool enterBehindHeadOfHandlerBlock(BlockID inputBlock, std::span<const uint32_t> edges);
    void noteRecordBuiltForPolls(ValueID, const Draft&, const Effects&);
    void buildPollInFrontOfLoop(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
    void settleFateOfPoll(const OpenLoop&);
    void noteEdgeCameRound(BlockID head);
    ValueID m_pollInFrontOfLoopBeingEntered;
    bool m_makesLetterOfStep { false };
    template<typename Functor> void forEachOpenLoopAround(uint32_t numberOfLoops, size_t firstFrame, const Functor&) const;
    uint32_t numberOfOpenLoopsAround(size_t frameIndex, BlockID inputBlock) const;
    void leaveBlockOpenInFrontOfLoop(BlockID inputHead, BlockID head, std::span<const Reference> entering, const Effects& effectsOfLoop);
    void endBlocksInFrontOfLoops(size_t frameIndex, BlockID next);
    bool isInvariantInLoop(uint32_t indexOfLoop, Reference) const;
    std::optional<uint32_t> findLoopToBuildInFrontOf(const Draft&, const Effects&, TypeID proven) const;
    bool excludesInitialPrimitive(const Draft&, TypeID proven) const;
    bool isArrayOrInitialPrimitive(const Draft&, TypeID proven) const;
    Reference tryHoistModuleVariableRead(const Draft&, uint32_t outermostLoopThatMayBeLeft);
    Reference tryHoistCapturedVariableRead(const Draft&, const Effects&, TypeID proven);
    std::optional<uint32_t> findLoopThatReaderVouchesFor(const Draft&, uint32_t outermostLoopThatMayBeLeft) const;
    struct HoistPoint {
        size_t frameIndex;
        BlockID inputBlock;
        BlockID block;
    };
    HoistPoint currentHoistPoint() const;
    struct ArrivalCondition {
        uint32_t loop;
        LoopEntryCondition entry;
    };
    bool findFirstArrivalConditions(uint32_t loopIndex, Vector<ArrivalCondition, 4>&) const;
    bool findFirstArrivalConditionsOf(uint32_t innerLoop, uint32_t loopIndex, const HoistPoint&, Vector<ArrivalCondition, 4>&) const;
    bool isOnMainPath(uint32_t loopIndex, const HoistPoint&) const;
    bool isAfterFewBranches(uint32_t loopIndex, const HoistPoint&) const;
    HoistPoint hoistPointInFrontOf(uint32_t loopIndex) const;
    enum class UnrolledIterations : uint8_t { AsPassed, AsWritten };
    bool executesOnEveryIteration(uint32_t loopIndex, const HoistPoint&, UnrolledIterations = UnrolledIterations::AsPassed) const;
    bool isGuaranteedToExecute(uint32_t loopIndex, const HoistPoint&) const;
    bool precedesReturnsOfInlinedCallees(uint32_t loopIndex, const HoistPoint&) const;
    bool headerMayExitBefore(uint32_t loopIndex, const HoistPoint&) const;
    struct InvariantGuard {
        ValueID test;
        bool expectedResult { false };
    };
    struct PendingGuards {
        Vector<InvariantGuard, 4> guards;
        EdgeID lastEdge;
    };
    bool hoistInvariantGuards(uint32_t loopIndex, IterationPurity, const PendingGuards&);
    void scanForImplicitExits(uint32_t loopIndex, PendingGuards&);
    bool collectGuardExits(uint32_t loopIndex, PendingGuards&) const;
    const OpenLoop* openLoopOfHeader(BlockID head) const;
    void appendCarriedResults(BlockID head, Vector<Reference, 8>& arguments);
    Reference absentResult(Representation);
    uint32_t numberOfCarriedResults(uint32_t summaryIndex);
    void strikeCarriedElement(BlockID head, CarriedResult&, Representation);
    void noteCarriedElements(BlockID head);
    Reference absentCarriedResult(const CarriedResult&);
    std::optional<uint32_t> findCarriedField(Reference object, PropertyKeyID, FieldReference, VariableID staticField);
    Reference strikeCarriedField(uint32_t slot);
    Reference enterWithCarriedField(uint32_t slot, Reference object, LayoutSetID receivers, Reference first);
    Reference unboxNumberBeforeLoop(Reference first, Representation);
    bool isSlotTestedBeforeLoop(uint32_t indexOfLoop, Reference object, PropertyKeyID) const;
    Reference fieldCarriedAcrossBackEdge(const CarriedResult&, const OpenLoop::CarriedField&);
    std::optional<uint32_t> openLoopOfElement(ValueID access) const;
    bool isCounterOfIteration(const OpenLoop&, uint32_t position) const;
    bool passesNextCounter(const OpenLoop&, uint32_t position, std::span<const Reference> arguments) const;
    ValueID findSameInFrontOfLoop(uint32_t indexOfLoop, const Draft&) const;
    Reference appendConversionAmongOpenLoops(Opcode, Representation to, Reference operand, TypeID proven, Origin);
    void noteBuiltInFrontOfLoop(uint32_t indexOfLoop, ValueID);
    void endBlocksInFrontOfLoopsFrom(size_t numberOfLoopsKept);

    bool tryVersionLoop(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
    bool buildLoopForEachMember(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
    bool isTooLargeToVersion(const BodyIndex&, LoopID);
    bool mayBuildLoopSeveralTimes(BlockID inputHead);
    std::optional<Vector<ValueID, 8>> findValuesOfLoopReadBehindIt(BlockID inputHead);
    bool handlersOutsideLoopReadItsValues(BlockID inputHead);
    void noteValuesThatHandlersOutsideLoopRead(BlockID inputHead);
    void walkCleanupsBesideCopyOfLoop(BlockID inputHead);
    void forgetValuesOfLoop(BlockID inputHead);
    struct SharedGenericVersion {
        size_t frameIndex { 0 };
        BlockID inputHead;
        BlockID start;
        BlockID entry;
        bool isAboutToBeTaken { false };
        bool isPlainWalk { false };
        bool otherHoldersAreAskedForLists { false };
        bool boundIsAskedForText { false };
        bool isEnteredFromIteration { false };
    };
    Vector<SharedGenericVersion, 2> m_sharedGenericVersions;
    Reference m_listAskedForStorageOfNumbersInVain;
    bool m_walksNumericListCopy { false };
    std::pair<uint32_t, BlockID> m_loopOfOtherKind;
    Reference m_receiverOfLargerMethod;
    ArrayStorage m_requestedLiteralStorage { ArrayStorage::Other };
    bool m_walksCleanupBesideCopyOfLoop { false };
    struct FrontOfLoopLeftOpen {
        BlockID block;
        BlockID head;
        std::shared_ptr<const ArgumentsOfEdge> entering;
        Origin origin { };
        ValuesBuiltInFront builtInFront;
    };
    FrontOfLoopLeftOpen m_frontOfLoopLeftOpen;
    struct FrontSharedWithTypedBuild {
        size_t frameIndex { 0 };
        BlockID inputHead;
        ValuesBuiltInFront builtInFront;
    };
    Vector<FrontSharedWithTypedBuild, 1> m_frontsSharedWithTypedBuilds;
    bool m_buildsTestsBesidePlainWalk { false };
    ValueID findSameInFrontOfPlainWalk(const Draft&) const;
    bool m_leavesFrontOfLoopOpenBehindWalk { false };
    bool m_genericVersionWouldSpeculate { false };
    struct ReportOfTypedBuild {
        Effects effects { Effects::none() };
        bool genericVersionSpeculates { false };
        bool startOfOtherCopyIsRare { true };
    };
    std::optional<ReportOfTypedBuild> m_reportOfTypedBuildMadeLast;
    bool m_walkOfLoopWasKept { false };
    BlockID m_headOfLoopKeptLast;
    size_t m_frameOfLoopKeptLast { 0 };
    void endFrontOfLoopLeftOpen();
    Vector<uint32_t, 8> nameValuesOnEdgesThatLeavePlainWalk(BlockID inputHead, std::span<const ValueID> readBehindLoop);
    bool writeOutTurnsOfLoop(BlockID inputHead, std::span<const Reference> entering);
    bool writeOutTurnsOfForIn(BlockID inputHead, std::span<const Reference> entering);
    bool unrollOverListedKeys(BlockID inputHead, std::span<const Reference> entering);
    bool unrollUnderOrders(BlockID inputHead, std::span<const Reference> entering, UnrolledForInIterations&, ValueID open, ValueID objectInBody, Reference object, Origin, const Invocable<bool(uint32_t)> auto& mayWriteOut);
    bool bindListedKey(const BodyValue&);
    bool writeOutTurnsOfForOf(BlockID inputHead, std::span<const Reference> entering);
    bool unrollOwnKeysLoop(BlockID inputHead, std::span<const Reference> entering, UnrolledForInIterations&, const ForOfByText&);
    void beginTurnsWrittenOut(BlockID inputHead);
    void walkTurnsWrittenOut(BlockID inputHead, std::span<const Reference> entering, uint32_t numberOfWalks, UnrolledForInIterations*);
    void leadIntoLoopAsWrittenBehindTurns(UnrolledForInIterations&, BlockID inputHead, std::span<const Reference> arriving, Reference position);
    void standAtHeadOfTurnOfForIn(BlockID inputHead);
    std::span<const uint32_t> numbersOfHandlersThatRead(Frame&);
    std::span<const BlockID> handlerBlocksOf(Frame&);
    uint32_t numberOfHandlersThatRead(Frame&, ValueID);
    struct HoistedSiteRead {
        ValueID read;
        Reference value;
    };
    struct SavedModuleVariable {
        VariableID variable;
        Reference value;
        uint16_t initialPrimitive { 0 };
    };
    struct VersionedLoop {
        size_t frameIndex { 0 };
        BlockID inputHead;
        BlockID entry;
        Vector<Reference, 8> valuesAtHeader { };
        BlockID genericStart { };
        Vector<GuessCountedOnByLooks, 4> guessesCountedOn { };
        bool canUseIterationGuards { false };
        bool hasCallInIteration { false };
        bool hasUninlinedCall { false };
        bool isEntered { false };
        uint32_t restartPoint { 0 };
        Reference effectsCheckBeginning { };
        Vector<std::pair<ValueID, Reference>, 2> elementsReadAhead { };
        Vector<std::pair<uint32_t, TypeID>, 2> backEdgeChecks { };
        ValueID relationThatAsksLesserBound { };
        Reference lesserBound { };
        Reference boundBesideLesserBound { };
        Reference viewWhoseLengthIsBound { };
        VariableID variableThatIsBound { };
        Reference readOfVariableThatIsBound { };
        ValueID boundElementRead { };
        Reference boundElement { };
        Reference boundElementView { };
        Reference boundElementKey { };
        struct CounterBelowRoomOfViews {
            ValueID counter;
            ValueID start;
            ValueID stepNonNegativeCheck;
            ValueID isBelowBound;
            Vector<ValueID, 2> fromZeroUp;
            uint32_t position { 0 };
        };
        CounterBelowRoomOfViews counterBelowRoom { };
        struct SecondCounterGuard {
            Reference view;
            Reference length;
            uint32_t position { 0 };
            Range steps;
            Reference least;
            Reference most;
            ValueID isFromZeroUp;
            ValueID isBelow;
        };
        Vector<SecondCounterGuard, 1> secondCounterGuards { };
        ValueID secondCounterTest { };
        bool secondCounterTestResult { true };
        struct SteppedKeyLimit {
            Reference list;
            Reference length;
            uint32_t steppedPosition { 0 };
            uint32_t counterPosition { 0 };
            bool counterRises { false };
            ValueID limitCheck;
            SteppedWithCounter beside { };
            ValueID headerTest { };
            bool headerTestResult { true };
        };
        Vector<SteppedKeyLimit, 2> steppedKeyLimits { };
        bool headAsksLesserBound { false };
        BlockID headThatAsksLesserBound { };
        Vector<FieldOfScope, 1> variableBackEdgeChecks { };
        bool turnsThatRunOtherCodeComeRound { false };
        bool isLeftAtOtherCodeOnUsualWay { false };
        BlockID lastBackEdgeDominator { };
        bool mayRestartUnderHandler { false };
        bool restartsAtCaughtThrows { false };
        Vector<SavedModuleVariable, 1> moduleVariablesAtHeader { };
        Vector<std::pair<uint32_t, BlockID>, 4> undoBlocks { };
        BlockID startOfCopyWithoutSomeIterationGuesses { };
        bool guessOfSomeTurnsIsAskedInFront { false };
        bool questionOfEveryTurnIsAskedInFront { false };
        bool isLeftAtAccessOnUsualWay { false };
        bool genericVersionSpeculates { false };
        bool frontAsksWhatNothingSpeaksFor { false };
        Reference arrayGuardedAtHeader { };
        ArrayStorage storageGuardedAtHeader { ArrayStorage::Other };
        bool isArrayGuardedAtHeaderWritable { false };
        std::span<const UnswitchCandidate> unswitchCandidates { };
        bool isUnswitched { false };
        Vector<HoistedSiteRead, 1> hoistedSiteReads { };
    };
    Vector<VersionedLoop, 2> m_versionedLoops;
    struct LoopReentry {
        size_t frameIndex { 0 };
        BlockID inputHead;
        BlockID caughtThrowEntry;
    };
    Vector<LoopReentry, 1> m_loopReentries;
    Vector<std::pair<uint32_t, BlockID>, 2> m_loopsLeftAtCaughtThrows;
    Vector<std::pair<uint32_t, BlockID>, 2> m_loopsWithSideExits;
    Vector<std::pair<uint32_t, BlockID>, 2> m_loopsWithoutQuietIterations;
    bool m_takesSideExit { false };
    mutable HashMap<const BodyIndex*, BlockID> m_lastReturnDominators;
    bool isOnIterationSidePath();
    void noteEffectsInIteration();
    void noteExitDuringLookup();
    bool tryTakeSideExit();
    bool exitsIterationAtEffects(const Draft&, const Effects&);
    const LoopReentry* findReentry(size_t frameIndex, BlockID inputHead) const;
    bool passesOnEveryIteration(const VersionedLoop&, UnrolledIterations = UnrolledIterations::AsPassed) const;
    void saveModuleVariablesAtHeader(BlockID inputHead);
    bool isUsedAfterFirstStore(VariableID) const;
    Reference narrowToFilled(Reference held, uint16_t empty, BlockID& behind);
    bool askLesserBoundAt(const BodyValue& relation);
    bool tryGuardVariableStep(const BodyValue& step);
    void claimThatCounterNeverLiesBelowItsStart();
    bool counterComesRoundNotBelowItself(BlockID head, std::span<const Reference> arguments);
    bool loweredCountersComeRound(BlockID head, std::span<const Reference> arguments);
    bool raisedCountersComeRound(BlockID head, std::span<const Reference> arguments);
    ValueID claimCounterLowerBound(ValueID counter, ValueID bound);
    NEVER_INLINE bool secondCountersComeRound(BlockID head, std::span<const Reference> arguments);
    Vector<VersionedLoop::SecondCounterGuard, 1> guardSecondCounters(const AccessesNearCounter&, std::span<const Reference> entering, BlockID& genericStart, Origin, NOESCAPE const ScopedLambda<Reference(ValueID)>& builtInFront);
    void claimSecondCounters(size_t tried, ValueID test, bool isOnTakenEdge);
    NEVER_INLINE TypeID typeOfSecondCounter(const Draft&);
    NEVER_INLINE bool narrowSecondCounters(EdgeID&, BlockID from, BlockID to, uint32_t targetIndex, Vector<Reference, 8>& arguments);
    std::optional<Range> stepsOfHeadParameter(BlockID inputHead, uint32_t position);
    NEVER_INLINE std::optional<Range> expectedStepsOfSum(BlockID inputHead, uint32_t position, const Operation& test, uint32_t operandIndex, const SecondCounter&);
    UncheckedKeyHashMap<uint64_t, Range, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_stepsOfHeadParameters;
    Reference buildSecondCounter(const SecondCounter&, Reference name, ValueID test, uint32_t operandIndex, bool isOnTakenEdge, EdgeID&, BlockID to);
    bool handTurnOverWhereLesserBoundIsReached(const BodyTerminator&);
    uint32_t m_restartPointCount { 0 };
    BlockID m_restartablePreviewHeader;
    Vector<ValueID, 16>* m_expectationsRememberedForLook { nullptr };
    StaticFieldSeenInVariable* m_staticFieldSeenInVariableByArm { nullptr };
    ReadArm* m_readArm { nullptr };
    InBoundsPath* m_inBoundsPath { nullptr };
    BlockCheckedForArithmetic m_blockCheckedForArithmetic;
    LoopsStoringByName m_loopCheckedForStores;
    const ScopedLambda<TypeID(ValueID)>* m_typesOfInputsInLook { nullptr };
    mutable bool m_lookHasAskedAboutUnbuiltValue { false };
    std::pair<size_t, LoopID> m_loopThatItsTryHasWalked;
    bool m_looksTakeFormsBehindLooksAsBuilt { false };
    bool m_purchaseAsksWhetherTurnsCanBeQuiet { false };
    void markIterationRestartPoint(BlockID inputHead, const Effects& ofTurns);
    void proveComparedNumber(const BodyValue& operation);
    void proveWhatLooksCountOnAt(const BodyValue&);
    uint32_t m_numberOfStretchesBehindBytesOfMembers { 0 };
    PlacesSeenToStandInStretches m_placesSeenToStandInStretches;
    bool areIterationsRestartable(BlockID inputHead, const LoopPreview&);
    const VersionedLoop* findRestartedLoop() const;
    std::optional<uint32_t> placeOfRestartedLoop(const VersionedLoop&) const;
    std::optional<uint32_t> specializedOpenLoop(IterationPurity = IterationPurity::EffectFree) const;
    ValueID findReadAskedAtHeadOfLoop(BlockID inputHead, std::span<const std::pair<Reference, TypeID>> tested);
    std::optional<HoistedArrayGuard> findListThatHeadReadsUnderKeyFromInFront(BlockID inputHead, std::span<const HoistedArrayGuard>);
    bool tryGuardObjectAtHeader(const BodyValue& read);
    void readElementsAhead(BlockID inputBlock);
    struct IterationsSteppedAtHead {
        Vector<std::pair<uint32_t, IndexedIteration>, 2> asTheyEntered;
    };
    bool canVersionLoopWithIterations(BlockID inputHead) const;
    bool iterationsSteppedAtHandOverPositionsAlone(BlockID inputHead) const;
    IterationsSteppedAtHead takeIterationsSteppedAt(BlockID inputHead) const;
    void putBackIterationsSteppedAt(const IterationsSteppedAtHead&);
    void walkCopyOfLoop(BlockID inputHead, std::span<const Reference> entering);
    bool walkSingleIteration(BlockID inputHead, std::span<const Reference> entering, BlockID reentry);
    bool exitsLoopBeingVersioned(size_t frameIndex, BlockID to) const;
    void takeNamesOntoEdge(OpenEdge&, size_t frameIndex);
    void takeNamesOfWay(BlockID inputBlock, NamesOfWay&&);
    void forgetNamesOfWay(Frame&);
    bool isFrameRecursive(const Frame&) const;
    bool isNamedByWaysInto(const Frame&, ValueID, BlockID inputBlock) const;
    Reference nameBroughtByWay(const Frame&, ValueID) const;
    ALWAYS_INLINE Reference referenceBoundIn(const Frame& frame, ValueID input) const
    {
        Reference bound = frame.references[input.index()];
        if (!bound && frame.duplicatedValues) [[unlikely]]
            return nameBroughtByWay(frame, input);
        return bound;
    }
    bool walkOperationOfGeneratorInLoop(const BodyValue&);
    bool takeGeneratorForConsumer(const BodyValue& call);
    bool findReadsAfterRegion(std::span<const BlockID> region, ValueID firstValue, Vector<ValueID, 4>& readAfterRegion);
    bool loopHoldsGenerators(const BodyIndex&, LoopID);
    bool openGeneratorTaken(const BodyValue& open);
    bool closeGeneratorTaken(const BodyValue& close);
    bool spreadGeneratorTaken(const BodyValue& spread);
    bool spreadGeneratorTakenAt(size_t placeOfTaken, const BodyValue& spread);
    bool patternTakesGeneratorAt(size_t placeOfTaken, const BodyValue& open);
    bool walkYieldOfGeneratorInConsumer(const BodyValue& yield);
    bool walkInitialYieldInConsumer(const BodyValue& initialYield);
    bool leaveHandlerOfGenerator(size_t frameOfGenerator);
    void walkYieldOfGeneratorInPattern(size_t place, const BodyValue& yield, Reference yielded, Origin);
    void walkYieldOfGeneratorInPatternWithRest(size_t place, const BodyValue& yield, Reference yielded, Origin);
    void appendYielded(Reference list, Reference yielded, Origin);
    bool stepsStandApart(const StepsInRow&) const;
    std::optional<uint32_t> numberOfStepsTaken(Reference count) const;
    void chooseInlineFrameOfStep(bool isPastInitialYield = false);
    void nameCallUpToInitialYield(const GeneratorTakenByConsumer&);
    bool walkStepsInRow(size_t placeOfTaken, const BodyValue& firstStep);
    void walkYieldOfGeneratorInRow(size_t place, const BodyValue& yield, Reference yielded, Origin);
    Reference walkToNextStepInRow(size_t place, uint32_t step, Reference yielded);
    Reference walkBetweenStepsInRow(std::span<const ValueID> steps, uint32_t step, Reference value, bool isDone);
    Reference receiverOf(const Frame&) const;
    void finishStepsInRow(const GeneratorWalkedInConsumer&, uint32_t step, Reference returned, Vector<Frame::ReturnOfCopy, 2>& ways, Vector<Vector<Reference, 4>, 2>& carriedOnWays);
    size_t placeOfGeneratorOnTop() const;
    ValueID onlyUserOfEntryParameter(FunctionID generatorFunction, uint32_t entryParameter);
    ValueID onlyTakerOfEntryParameter(FunctionID, uint32_t entryParameter);
    bool onlyStepsEntryParameter(FunctionID, uint32_t entryParameter);
    bool passesGeneratorForIteration(const BodyIndex& caller, ValueID generatorCall, FunctionID callerID);
    Reference findPassedGenerator(ValueID taker, ValueID iterable, ValueID& creatingCall, uint32_t& frameOfCall);
    bool isPassedGenerator(Reference) const;
    bool isOnlyTestedSinceCall(Reference generator) const;
    bool checkPassedGenerator(Reference generator, Origin);
    bool runsNothingSinceCall(Reference generator) const;
    Reference functionOfGenerator(Reference generator) const;
    Reference makeGeneratorAgain(Reference generator);
    Frame& frameLeftForCall();
    bool readGlobalInCopy(const BodyValue& read);
    bool foldGeneratorRead(const BodyValue& read);
    bool foldGeneratorInstanceOf(const BodyValue& test);
    bool takePassedGeneratorForSteps(const BodyValue& read);
    void takePassedGeneratorForLoop(BlockID inputHead);
    size_t placeOfGeneratorTaken(ValueID taker, ValueID iterable) const;
    bool isBeingWalkedOtherThanAtYield(FunctionID) const;
    bool leavesRegionOfIteration(size_t frameIndex, BlockID to) const;
    bool stepGeneratorTakenOutsideLoops(const BodyValue& step);
    void layFrameOfGenerator(FunctionID, uint32_t callPosition, uint32_t loopDepthAtCall, uint32_t numberOfCarriedValues);
    Vector<Reference, 4> carriedValuesInFrame(size_t frameIndex) const;
    Vector<Reference, 4> valuesThatLoopBringsRound(size_t frameIndex, BlockID inputHead) const;
    void bindValuesThatLoopBringsRound(BlockID inputHead, std::span<const Reference> carried);
    void bindInFrame(size_t frameIndex, ValueID input, Reference);
    bool frameMayMove(size_t from) const;
    void moveFrame(size_t from, size_t to);
    void forgetRegionOfIteration(size_t frameIndex, const GeneratorWalkedInConsumer&);
    void restoreBindingsBeforeLoop(const GeneratorWalkedInConsumer&);
    void walkHeadOfLoopBehindStep(BlockID inputHead, ValueID step, std::span<const Reference> carried, bool isDone, Reference stepValue);
    void walkManualIterationAfterStep(const GeneratorWalkedInConsumer&, std::span<const Reference> carried, bool isDone, Reference stepValue);
    struct EndOfManualIteration {
        Reference generator;
        bool isReached { false };
        BlockID block;
        Reference sent;
        WalkState state;
    };
    EndOfManualIteration walkManualIterationToStep(const GeneratorWalkedInConsumer&);
    bool answerStepOfGeneratorTaken(const BodyValue&);
    bool holdsLoopsThatStepByHand(FunctionID);
    bool loopStepsGeneratorTaken(BlockID inputHead) const;
    Reference resumptionOfGenerator(Reference sent, ResumeMode, Origin);
    void carryValuesRoundLoopOfGenerator(BlockID inputHead, BlockID head, Vector<Reference, 8>& entering, Vector<ValueID, 8>& parameters);
    void appendCarriedValuesThatComeRound(BlockID head, Vector<Reference, 8>& arguments);
    bool takeCarriedValuesOntoHandlerEdge(size_t frameIndex, Vector<std::pair<ValueID, Reference>, 2>& names);
    bool boxValuesThatHandlerOutsideLoopReads(size_t frameIndex, BlockID handler, const OpenLoop* loopLeft, Vector<std::pair<ValueID, Reference>, 2>& names);
    const OpenLoop* loopLeftByHoisting(size_t frameIndex) const;
    Reference valueEnteringLoop(const OpenLoop&, Reference) const;
    void bindParametersOfHeadForHandlerOfStep(size_t frameIndex);
    void nameCarriedValuesOnEdge(OpenEdge&, size_t frameIndex);
    BlockID handlerOfCodeInForce(size_t numberOfFramesUpToHandler) const;
    void noteCarriedValuesAtReturnOfGenerator();
    Vector<Reference, 4> walkBodyOfGeneratorToItsEnd(size_t place, Origin originInFront, Reference* returned = nullptr);
    Vector<Vector<Reference, 4>, 2> walkBodyOfGenerator(size_t place, Origin originInFront, Vector<Frame::ReturnOfCopy, 2>& returns);
    Vector<Reference, 4> joinReturnsOfGenerator(Vector<Frame::ReturnOfCopy, 2>& returns, Vector<Vector<Reference, 4>, 2>& carriedAtReturns, Origin originInFront, Reference* returned);
    bool walkGeneratorInPlaceOfLoop(BlockID inputHead, std::span<const TypeID> typesOfParameters, std::span<const Reference> entering);
    void resumeGeneratorToLeave(size_t place, ResumeMode, bool dropsWhatIsThrown, Origin);
    Vector<GeneratorTakenByConsumer, 1> m_generatorsTakenByConsumers;
    Vector<GeneratorWalkedInConsumer, 1> m_generatorsWalkedInConsumers;
    Vector<CopiedCollection, 1> m_copiedCollections;
    bool m_hasTakenGeneratorsSteppedByHand { false };
    EndOfManualIteration* m_endOfManualIteration { nullptr };
    Vector<size_t, 1> m_resumptionsToLeave;
    uint32_t m_numberOfFramesOverHandlersOfCode { 0 };
    std::unique_ptr<KeptForBuild>& m_keptByGeneratorsInLoops;
    bool isTailDuplicatingBlock(size_t frameIndex) const;
    bool letReturnsOfCopyWalkEndOfBlockApart(ValueID call, std::span<Frame::ReturnOfCopy* const> passing, std::span<Frame::ReturnOfCopy* const> closed);
    std::optional<uint32_t> turnWhoseQuietWaysGoToHeadAround(BlockID inputBlock);
    bool letQuietReturnsOfCopyWalkEndOfBlockApart(ValueID call);
    bool letAnswersOfCopyWalkEndOfBlockApart(ValueID call);
    Vector<Vector<Frame::ReturnOfCopy*, 2>, 4> groupReturnsByShape(Vector<Frame::ReturnOfCopy, 2>&, uint32_t numberOfValuesBuiltBefore, bool& hasMixedReturns);
    bool letShapesOfCopyWalkEndOfBlockApart(ValueID call, uint32_t numberOfShapes);
    void walkEndOfBlockBehindReturnsOfCopy(ValueID call);
    bool goOnBehindReturnsOfCopy(const BodyValue& call, const Function&, const LinkedFunction&, Origin originOfCall, Vector<Frame::ReturnOfCopy, 2>& returns, uint32_t numberOfValuesBuiltBefore, bool returnsKey = false);
    bool walkBlockApart(BlockID inputBlock);
    Reference booleanOfUnit(bool);
    bool inlineCalleeAsLinkOfChain(const BodyValue& call);
    bool inlineCalleeAsLinkOfChainOfItsOpcodes(const BodyValue& call);
    bool isCopiedByLinkOfChain(const BodyValue& call) const { return m_callCopiedByLinkOfChain == call.id() && m_numberOfFramesAtCallCopiedByLinkOfChain == m_frames.size(); }
    ValueID m_callCopiedByLinkOfChain;
    size_t m_numberOfFramesAtCallCopiedByLinkOfChain { 0 };
    bool looksAtLoopsAroundHaveSeen(const Effects&) const;
    void leadEdge(EdgeID, BlockID from, const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    void leadHandlerEdge();
    bool handlerOnlyClosesIterations(Frame&, BodyBlock) const;
    bool isUnderHandlerOtherThanCleanups() const
    {
        if (m_handlerMadeOverCopies || m_numberOfFramesOverHandlersOfCode)
            return isUnderHandler();
        bool standsUnderCleanup = false;
        for (const Frame& frame : m_frames) {
            if (!frame.hasHandler)
                continue;
            if (!frame.handlerOnlyClosesIterations)
                return true;
            standsUnderCleanup = true;
        }
        return !standsUnderCleanup && isUnderHandler();
    }
    bool mayCatchInIteration() const
    {
        if (!isUnderHandler())
            return false;
        if (m_versionedLoops.isEmpty() || !m_versionedLoops.last().mayRestartUnderHandler)
            return true;
        return isUnderHandlerOtherThanCleanups() && !hasOnlyOuterHandlers();
    }
    bool handlerPreventsIterationRestart() const { return mayCatchInIteration() && (m_versionedLoops.isEmpty() || !canExitCurrentHandlers()); }
    ALWAYS_INLINE bool isUnderHandler() const
    {
        if (m_block && m_code.at(m_block).handler)
            return true;
        return m_handlerMadeOverCopies || numberOfFramesUpToHandlerInForce();
    }
    ALWAYS_INLINE bool handlerMadeOverCopiesIsInForce() const { return m_handlerMadeOverCopies && numberOfFramesUpToHandlerInForce() <= m_numberOfFramesBelowHandlerMade; }
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
    bool leadHandlerEdgeBehindStringsOfTexts(size_t frameIndex, BodyTarget, Vector<Reference, 4>& arguments, Vector<TypeID, 4>& types, Vector<std::pair<ValueID, Reference>, 2>& names);
    void bindTextsAtHeadOfHandlerBlock(BlockID inputBlock);
    Reference narrowOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&);
    void narrowTestedConversion(Reference subject, Reference name);
    Reference booleanAskedByBranchOnEdge(BlockID from, uint32_t targetIndex, Reference value);
    void nameBooleansAskedByBranch(BlockID from, const BodyTerminator&, uint32_t targetIndex, BlockID to);
    std::optional<Draft> draftOfNarrowedOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&, Reference holderOfSubject = { }, Reference termOfSubject = { });
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
    void checkThatTagAnswers(Reference tag, Reference asked, Reference answer, Origin);
    void checkThatTagLeadsWhereShapeLeads(const BodyTerminator& overTags, uint32_t targetIndex);
    void askShapeInPlaceOfTagAtBranch(const BodyTerminator&);
    bool isNameOfCellAskedForShape(Reference name, ValueID asked) const;
    std::optional<Draft> draftOfHolderNarrowedByShape(BlockID from, uint32_t targetIndex, Reference holder, Reference test);
    Vector<std::unique_ptr<MembersByTag>> m_listsOfMembersByTag;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_membersByTagOfSets;
    bool isReadOfSlotOffHolder(Reference read, Reference holder, PropertyKeyID) const;
public:
    Reference buildTestOfTagAsTestOfShape(const BodyValue& comparison);
    Reference buildTestOfTruthOfTagAsTestOfShape(const BodyValue& truth);
    bool isAskedByBranchOfItsBlock(const BodyValue& comparison) const;
    Reference tagReadInFrontOfTest(const BodyValue& read);
private:
    void noteNarrowerName(BlockID to, Reference value, Reference name);
    void redefineStepsAfterTest(ValueID test, Reference tested, Reference narrowed, BlockID to, Vector<Reference, 8>& arguments);
    bool endBlockInAnotherWay(const BodyTerminator&);
    bool endBlockWithSwitchOverAlternatives(const BodyTerminator&);
    bool endBlockWithConditionNumber(const BodyTerminator&);
    void endWithoutEdges(TerminatorDraft&);

    void walkReturn(const BodyTerminator&);
    void noteReturnOfCopy(const BodyTerminator&);
    void returnFromUnit(Reference returned);
    void endFreshCellLifetimes(std::span<const Reference> liveValues, Origin, uint32_t numberOfValuesBuiltBefore);
    bool shouldEndLifetimesOnEdges() const;
    bool canReleaseArrays() const;
    bool isReleasableArray(Reference) const;
    Vector<Reference, 2> listReleasableArrays(uint32_t numberOfValuesBuiltBefore) const;
    void releaseDeadArrays(std::span<const Reference> liveValues, Origin, uint32_t numberOfValuesBuiltBefore);
    bool armsBringFreshList(std::span<const JoinArm>, size_t position);
    bool standsForFrameOfGenerator(Reference state) const;
    void endUnreferencedCellLifetimes(Vector<Reference, 16>& named, uint32_t numberOfValuesBuiltBefore);
    void endLifetimesAtJump(const BodyTarget&);
    void endLifetimesAtTailCall(const BodyValue& call);
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
    static std::array<uint8_t, numberOfOpcodes> findStepsOfWalkByOpcode();
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
    Reference narrowestNameOf(Reference) const;
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
    WalkMode m_way { WalkMode::AsBuilt };
    const std::array<uint32_t, numberOfOpcodes>& m_stepsOfBuildByOpcode;
    const std::array<uint8_t, numberOfOpcodes>& m_stepsOfWalkByOpcode;
    Code& m_code;
    NumbersOfWalkStates m_numbersOfStates;
    AnswersOfWalk m_answersOfStates;
    WalkState m_state;

    BlockBeingBuilt m_block;
    Vector<uint32_t>& m_numbersOfFramesOfFunctions;
    bool& m_workerHasBegunUnit;
    const Walker*& m_newestWalkerOfWorker;
    bool m_hasBegunUnit { false };
    Frames m_frames;
    FunctionID m_copyThatMakesNewList;
    bool m_runOverBoxedValuesTakesNewListOfCopyFirst { false };
    ValueID m_listThatRunOverBoxedValuesTakesFirst;
    ValueID m_receiverOfCopyThatMakesNewList;
    size_t m_depthOfCopyThatMakesNewList { 0 };
    bool m_opensGuesses { true };
    bool m_closedArmKeepsSites { false };
    AssignedMemberChoice m_assignedMemberChoice { AssignedMemberChoice::WhenUnconditional };
    size_t m_numberOfFramesAtRowOfShapeTests { 0 };
    ClosedArmOfRowOfShapeTests m_closedArmOfRowOfShapeTests;
    StretchWithArmsAsWritten m_stretchWithArmsAsWritten;
    Vector<StoresIntoRowsOfBlock, 2> m_storesIntoRowsOfBlocks;
    Vector<UsersWithinBlock, 2> m_usersWithinBlocks;
    Vector<UsersOfValuesOfLoop, 2> m_usersOfValuesOfLoops;
    LazyMap<HashMap<const BodyIndex*, BitVector>> m_valuesTakenForKeys;
    LazyMap<HashMap<const BodyIndex*, BitVector>> m_loopsThatReckonWithElements;
    LazyMap<HashMap<const BodyIndex*, Vector<ComputedComparison>>> m_computedComparisons;
    mutable LazyMap<HashMap<const void*, BitVector>> m_numbersInListsOfWrittenObjects;
    LazyMap<HashMap<const BodyIndex*, Vector<uint32_t>>> m_lastStoresByKeyOfLoops;
    LazyMap<HashMap<const BodyIndex*, Vector<ElementRecurrence>>> m_elementRecurrences;
    LazyMap<HashMap<const BodyIndex*, Vector<std::optional<Vector<uint32_t>>>>> m_pushesOntoLiterals;
    LazyMap<HashMap<const BodyIndex*, Vector<std::optional<Vector<MentionInLoop>>>>> m_mentionsBeforeLoops;
    LazyMap<HashMap<const BodyIndex*, Vector<std::optional<Vector<ListAccessesInBlock>>>>> m_listAccessesOfLoops;
    HashMap<const BodyIndex*, bool> m_bodiesPushingOntoFields;
    LazyMap<HashMap<const BodyIndex*, Vector<std::optional<bool>>>> m_outermostPushPopLoops;
    HashMap<std::pair<const BodyIndex*, uint32_t>, bool> m_steppedCounters;
    HashMap<std::pair<const BodyIndex*, uint32_t>, uint8_t> m_unsignedZeroValues;
    LazyMap<HashMap<const BodyIndex*, Vector<std::pair<uint64_t, uint32_t>>>> m_storesByKeyOfBlocks;
    LazyMap<HashMap<const BodyIndex*, Vector<uint64_t>>> m_namesStoredInOutermostLoops;
    LazyMap<HashMap<const BodyIndex*, Vector<uint8_t>>> m_whetherElementsOfLoopsMeetWhatGoesRound;
    LazyMap<HashMap<const BodyIndex*, Vector<std::pair<ValueID, TypeID>>>> m_literalElementTypes;
    LazyMap<HashMap<const BodyIndex*, Vector<ValueID>>> m_literalsPushedOntoByLoops;
    LazyMap<HashMap<const BodyIndex*, BitVector>> m_listsThatCallsAloneTake;
    PrintStream* const m_explainsClosedAccessesTo;
    uint32_t m_speculationCount { 0 };
    uint32_t m_hoistedArrayGuardCount { 0 };
    uint32_t m_hoistedElementCheckCount { 0 };
    uint32_t m_hoistedNumberReadCount { 0 };
    uint32_t m_numberReadNearStoresCount { 0 };
    bool m_typedBuildAskedNoListThatItWasBoughtFor { false };
    uint32_t m_versionedLoopCount { 0 };
    uint32_t m_versionedLoopNestCount { 0 };
    uint32_t m_loopVisitCount { 0 };
    uint32_t m_undoneLoopVisitCount { 0 };
    uint32_t m_abortedLoopVisitCount { 0 };
    uint32_t m_predictedLoopEffectsCount { 0 };
    uint32_t m_refutedLoopEffectsCount { 0 };
    uint32_t m_numberOfHearingsUnderQuietTries { 0 };
    uint32_t m_failedLoopVersioningCount { 0 };
    uint32_t m_expandedObjectAssignCount { 0 };
    KeysStoredInFrontOfRow m_keysStoredInFrontOfLastRow;
    NamesGatheredForRoots m_namesGatheredForRoots;
    ValuesThatEveryWayHandsToParameters m_valuesThatEveryWayHandsToParameters;
    AccessesOfSlotsInLoop m_accessesOfSlotsInLoopListedLast;
    uint32_t m_duplicatedJoinCount { 0 };
    uint32_t m_threadedConditionCount { 0 };
    uint32_t m_fieldCalleeSpeculationCount { 0 };
    uint32_t m_genericInstanceCheckCount { 0 };
    uint32_t m_genericMemberLookupCount { 0 };
    uint32_t m_mergedFreshObjectCount { 0 };
    BitVector m_mergedFreshObjects;
    bool m_hasMadeReleasableArrays { false };
    uint32_t m_iterationRestartCount { 0 };
    uint32_t m_undoneStoreCount { 0 };
    struct UndoableStore {
        uint32_t previous { 0 };
        Reference object;
        Reference held;
        PropertyKeyID key;
        LayoutSetID receivers;
        ECMAMode mode { ECMAMode::Strict };
        Reference position;
        Reference length;
        LayoutID layoutOfHeir;
        uint32_t fieldOfHeir { 0 };
        bool isOfGlobalObject { false };
        VariableID staticField;
        bool isOfScope { false };
        VariableID variable;
        Reference address;
        bool findsAddress { false };
        bool isOfInt32Storage { false };
        bool holdsElementWord { false };
        Reference wasStored;
        uint32_t restartPoint { 0 };
        bool mayHoldNothing { false };
        bool isIntoIterationObject { false };
    };
    void recordPendingUndoableStore(UndoableStore);
    struct MergedUndoableStore {
        UndoableStore store;
        Vector<Reference, 4> heldOnPaths;
        Vector<Reference, 4> storedOnPaths;
        bool hasParameter { false };
    };
    struct MergedUndoableStores {
        uint32_t commonStore { 0 };
        Vector<MergedUndoableStore, 2> stores;
    };
    std::optional<MergedUndoableStores> mergeUndoableStores(std::span<const uint32_t> edges, BlockID inputBlock);
    bool mergeUndoableElementStores(MergedUndoableStores&, std::span<const uint32_t> edges, BlockID inputBlock, std::span<const uint32_t> stores);
    void recordMergedUndoableStores(const MergedUndoableStores&);
    bool isDefinedOnPath(Reference, const WalkState&) const;
    void popVersionedLoop();
    Vector<UndoableStore> m_undoableStores;
    Vector<std::pair<uint32_t, WalkState>> m_undoableStoreStates;
    Reference m_valueBeforePendingStore;
    uint32_t m_pendingUndoableStore { 0 };
    bool m_isUndoingStores { false };
    uint32_t m_emptySlotCaseCount { 0 };
    UncheckedKeyHashMap<uint32_t, Vector<FieldIndex, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fillersOfAllocations;
    uint32_t m_foldedSliceCount { 0 };
    Vector<CountOfUnit, 4>& m_countsOfUnit;
    uint32_t m_numberOfValuesWalked { 0 };
    uint32_t m_numberOfValuesWalkedAndKept { 0 };
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>& m_roundsKept;
    struct DeleterOfKeptForInt32Promises {
        void operator()(KeptForInt32Promises*) const;
    };
    std::unique_ptr<KeptForInt32Promises, DeleterOfKeptForInt32Promises> m_keptForInt32Promises;
    struct ListOfNames {
        Vector<PropertyKeyID> names;
        UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> answers;
        HashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> areAllPlainFields;
        UncheckedKeyHashMap<uint32_t, Vector<LayoutID, 4>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> fewInstances;
    };
    Vector<ListOfNames>& m_listsOfNames;
    HashMap<uint64_t, Vector<uint32_t, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>& m_numbersOfListsOfNames;
    std::array<uint32_t, numberOfEnumerators<ProofSource>> m_numbersOfProofsChecked { };
    uint32_t m_untestableProofCount { 0 };
    uint32_t m_impliedProofCount { 0 };
    uint32_t m_unsatisfiableProofCheckCount { 0 };
    uint32_t m_foldedProofCheckCount { 0 };
    uint32_t m_numberOfOperationsBuiltThatAreSeenIfRunAgain { 0 };
    uint32_t m_numberOfOperationsBuiltThatMayEnterByValue { 0 };
    uint32_t m_numberOfRecordsBuiltToAskGuardsAgain { 0 };
    uint32_t m_skippedFreshCellCount { 0 };
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsInReachOfUnit;
    SurveysOfConstants& m_surveysOfConstants;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>& m_constantsThatListFieldsHeldFilled;
    bool m_callIsAccountedFor { false };
    FunctionID m_calleeAccountedFor;
    bool m_hasFailed { false };
    ReadWhoseGuessIsOpen m_readWhoseGuessIsOpen;
    Reference m_guardedReadArray;
    bool m_isInArrayArms { false };
    MethodChain m_currentMethodChain;

    std::unique_ptr<PositionTypes>& m_positionsOfBuild;
    std::unique_ptr<PositionTypes>& m_positionsOfSlowBody;
    bool m_noTextWasEvaluated { false };
    bool m_comparesElementsOfViews { false };
    bool m_hasInBoundsPaths { false };
    const PositionTypes* m_positions { nullptr };
    std::unique_ptr<PositionTypes>& m_positionsKnowingNothing;
    std::unique_ptr<PositionTypes>& m_positionsKnowingWhatFunctionsHandBack;
    std::unique_ptr<PositionTypes>& m_heldPositions;
    TypeID m_resultOfCallBeingTyped;
    TypeID m_resultOfReadBeingTyped;
    bool m_resultOfReadIsRemembered { false };
    TypeID m_contentsBeingTyped;
    std::unique_ptr<PositionTypes> m_positionsWithoutProofs;
    void forEachValueThatTypeShownLeansOn(ValueID, NOESCAPE const Invocable<void(ValueID)> auto&);
    TypeID typeShownAtOnce(ValueID) const;
    bool entryParameterIsShown(const Block&, ValueID) const;
    bool entryParameterIsCapture(const Block&, ValueID) const;
    TypeID computeTypeShown(ValueID);
    bool m_dropsChecksOfProofs { false };
    struct TypeHeldOfSlots {
        uint64_t receiversAndKey { std::numeric_limits<uint64_t>::max() };
        TypeID type;
    };
    std::array<TypeHeldOfSlots, 16> m_typesHeldOfSlots;
    HashMap<uint32_t, TypeID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typesShown;
    HashMap<uint32_t, TypeID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typesAsked;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_headsOfClosedLoops;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_genericVersionEntries;
    bool m_claimsSecondCounters { false };
    bool m_refusesSecondCounters { false };
    ASCIILiteral m_failure;
    InternalErrorKind m_kindOfFailure { InternalErrorKind::Unknown };
    FunctionID m_functionOfFailure;
    bool m_failureIsSaid { false };
    uint32_t m_snapshotsHeld { 0 };
    Vector<UserDiagnostic> m_reports;
    Origin m_origin { };
    ListPutBackByLog<OpenEdge> m_openEdges;
    uint32_t m_firstFreeOpenEdge { noOpenEdge };
    Vector<PendingBlock>& m_pendingBlocks;
    Vector<Binding>& m_bindings;
    Vector<Binding, 2> m_valuesStoodInFor;
    uint32_t m_numberOfClosedArms { 0 };
    ShortList<TypeID, 16> m_operandTypes;
    Vector<uint32_t, 16> m_sharedConstants;
    bool m_holdsReadsOfVariablesAsConstants { false };
    uint32_t m_numberOfSharedConstants { 0 };
    Vector<std::pair<TypeID, Reference>, 2>& m_unspecifiedConstants;
    void storeValueNoLongerCaptured(const StoreOfCapturedValue&);
    void storeValueBehindItsDefinitionInMaker(ValueID);
    std::span<const StoreOfCapturedValue> m_storesOfCapturedValues;
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
    ListPutBackByLog<IndexedIteration, 4> m_indexedIterations;
    Vector<uint64_t>& m_inputHeadsOfIterationsAddedInTries;
    struct TypeChangedInTry {
        uint32_t record { 0 };
        TypeID before;
        bool isAsked { false };
    };
    Vector<TypeChangedInTry>& m_typesChangedInTries;
    Vector<std::pair<uint32_t, uint32_t>>& m_preheaderDepthUndoLog;
    struct ValuesReadBehindLoopFoundLast {
        const BodyIndex* body { nullptr };
        BlockID inputHead;
        std::optional<Vector<ValueID, 8>> found;
        Vector<ValueID, 2> readByHandlersOutside;
        Vector<BlockID, 2> cleanupsOutside;
    };
    ValuesReadBehindLoopFoundLast m_valuesReadBehindLoopFoundLast;
    Vector<uint32_t>& m_sharedConstantsAddedInTries;
    Vector<uint32_t>& m_copiesOfConstantsAddedInTries;
    void forgetSharedConstantsAddedBehind(size_t numberLogged);
    void forgetCopiesOfConstantsAddedBehind(size_t numberLogged);
    void setPreheaderDepth(BlockID block, uint32_t place)
    {
        if (m_trialVisitDepth) [[unlikely]]
            m_preheaderDepthUndoLog.append({ block.index(), m_preheaderDepths[block.index()] });
        m_preheaderDepths[block.index()] = place;
    }
    void setTypeShown(ValueID record, TypeID type)
    {
        if (m_trialVisitDepth) [[unlikely]]
            m_typesChangedInTries.append({ record.index(), m_typesShown.get(record.index()), false });
        m_typesShown.set(record.index(), type);
    }
    void removeTypeShown(ValueID record)
    {
        if (m_trialVisitDepth) [[unlikely]]
            m_typesChangedInTries.append({ record.index(), m_typesShown.get(record.index()), false });
        m_typesShown.remove(record.index());
    }
    void setTypeAsked(ValueID record, TypeID type)
    {
        if (m_trialVisitDepth) [[unlikely]]
            m_typesChangedInTries.append({ record.index(), m_typesAsked.get(record.index()), true });
        m_typesAsked.set(record.index(), type);
    }
    void removeTypeAsked(ValueID record)
    {
        if (m_trialVisitDepth) [[unlikely]]
            m_typesChangedInTries.append({ record.index(), m_typesAsked.get(record.index()), true });
        m_typesAsked.remove(record.index());
    }
    HashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_iterationsOfHeads;
    HashMap<uint32_t, Vector<FieldOfScope>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fieldsOfScopesOfHeads;
    bool m_unitHasMadeScopes { false };
    uint32_t m_numberOfStoresIntoUnmadeRecords { 0 };
    class ScopesNotedInOrder {
    public:
        bool isEmpty() const { return m_inOrder.isEmpty(); }
        size_t size() const { return m_inOrder.size(); }
        bool contains(uint32_t scope) const { return m_scopes.contains(scope); }
        void add(uint32_t scope)
        {
            if (m_scopes.add(scope).isNewEntry)
                m_inOrder.append(scope);
        }
        void shrink(size_t size)
        {
            while (m_inOrder.size() > size)
                m_scopes.remove(m_inOrder.takeLast());
        }
        void clear()
        {
            m_scopes.clear();
            m_inOrder.shrink(0);
        }

    private:
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_scopes;
        Vector<uint32_t> m_inOrder;
    };
    ScopesNotedInOrder m_scopesThatClosuresHold;
    ScopesNotedInOrder m_scopesWhoseFieldsHaveComeRound;
    Vector<uint32_t>& m_blocksWithFieldsOfScopesInOrder;
    bool m_carriesSlotsOfObjects { false };
    HashMap<uint32_t, std::shared_ptr<const Vector<MergedHeaderObject, 1>>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_mergedHeaderObjects;
    Vector<FieldOfScope>& fieldsOfScopesLaidAt(BlockID);
    HashMap<uint64_t, AccessesOfScopeByFunction, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>& m_accessesOfScopesByFunctions;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_inputHeadsOfIterations;
    static uint64_t inputHeadInFrame(uint32_t inlineFrame, BlockID inputHead) { return static_cast<uint64_t>(inlineFrame) << 32 | inputHead.index(); }
    uint32_t m_numberOfIterationsInFrontOfTheirLoops { 0 };
    std::optional<bool>& m_isArrayIteratorProtocolIntact;
    std::optional<bool>& m_objectPrototypeMayGainName;
    bool m_isLeadingHandlerEdge { false };
    BlockID m_handlerMadeOverCopies;
    size_t m_numberOfFramesBelowHandlerMade { 0 };
    ListChangedOnlyByName<OpenLoop, 4> m_openLoops;
    Vector<uint32_t>& m_preheaderDepths;
    uint32_t m_numberOfLoopsThatMayBeLeft { UINT32_MAX };
    bool m_isBuildingInPlace { false };
    RowFoundUnderKey m_rowOfCopyFoundUnderKey;
    bool m_hasHoistedFields { false };
    uint32_t m_numberOfLoopsAroundEdgeBeingLed { UINT32_MAX };
    uint32_t m_numberOfScopesWhereProofsHoldEverywhere { 0 };
    FormAskedAhead m_formAskedAhead;
    Vector<std::pair<FunctionID, BlockID>, 2> m_loopsWhosePushFillsLiteral;
    std::unique_ptr<KeptForBuild>& m_keptByTypedAccesses;
    std::unique_ptr<KeptForBuild>& m_keptByUnionTableCalls;
    std::unique_ptr<KeptForBuild>& m_keptByCopiesOwnKeys;
    std::unique_ptr<KeptForBuild>& m_keptByPlainInstances;
    std::unique_ptr<KeptForBuild>& m_keptByInstanceOf;
    std::unique_ptr<KeptForBuild>& m_keptByInlining;
    CoroutineStateOfUnit m_coroutineState;
    LoopsOverGeneratorsOfUnit m_loopsOverGenerators;
    EnvironmentCellsOfUnit m_environmentCellsOfUnit;
    std::unique_ptr<KeptForBuild> m_keptByStrings;
    mutable std::unique_ptr<KeptForBuild> m_keptByScalarReplacement;
    std::unique_ptr<KeptForBuild>& m_keptByPositionsInTexts;
    mutable BodyLiveness m_liveness;
    struct CallsListedOfFunction {
        FunctionID function;
        std::span<const LinkedCall> calls;
        size_t next { 0 };
    };
    mutable CallsListedOfFunction m_callsListedOfFunction;
    using SidesThatArriveAtBlocks = HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    LazyMap<HashMap<std::pair<const BodyIndex*, uint32_t>, SidesThatArriveAtBlocks>> m_sidesOfChoicesThatArriveAtBlocks;
    uint32_t m_operationsCopiedAtWrittenCall { 0 };
    void noteClosuresThatAllocationAloneTakes(const BodyValue& allocation);
    void walkWhatIsDefinedBehindBirth(const BodyValue& allocation);
    Vector<ValueDefinedBehindBirth, 8> m_valuesDefinedBehindBirth;
    Vector<ValueID, 8> m_valuesWrittenBehindBirth;
    LayoutID m_layoutOfBirthThatListsWhatIsBehind;
    Vector<Reference, 8> m_closuresThatAllocationAloneTakes;
    uint32_t m_coldCopyDepth { 0 };
    bool m_checksBitsOfUnknownValues { false };
    bool m_walksCopyWhereHolderIsNoBuiltin { false };
    bool m_walksCopyForWhatIsNoBuiltin { false };
    struct HolderThatIsNoBuiltin {
        std::span<const ValueID> names;
        Reference value;
        size_t frameIndex { 0 };
        ValueID readOfVariableInLoop;
    };
    HolderThatIsNoBuiltin m_holderThatIsNoBuiltin;
    Vector<std::pair<size_t, BlockID>, 2> m_genericVersionsInsideSpecialized;
    Vector<std::pair<size_t, LoopID>, 2> m_reversionableLoops;
    bool m_isWalkingAccess { false };
    bool m_hasInlineLookup { false };
    bool m_operationWritesWhatTurnHasMade { false };
    ListPutBackByLog<StringInBuilder, 2> m_stringsInBuilders;
    BitVector m_parametersThatSomeScanFoundToReadTexts;
    void truncateStringInBuilderIndex(size_t);
    const StringInBuilderIndex& stringInBuilderIndex();
    StringInBuilderIndex m_stringInBuilderIndex;
    bool m_hasURLsThatAreNeverMade { false };
};

struct KeptBetweenUnits {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(KeptBetweenUnits);

    KeptBetweenUnits(const WalkInputs&, TypeArena&);
    ~KeptBetweenUnits();

    WalkerInRoom makeWalker(const WalkInputs&, TypeArena&, Arrivals&);

    const TypeArena& arenaOfPositions;
    std::unique_ptr<PositionTypes> positionsOfSlowBody;
    std::unique_ptr<PositionTypes> positionsKnowingNothing;
    std::unique_ptr<PositionTypes> heldPositions;
    std::remove_reference_t<decltype(Walker::m_keptByGeneratorsInLoops)> keptByGeneratorsInLoops;
    std::remove_reference_t<decltype(Walker::m_keptByTypedAccesses)> keptByTypedAccesses;
    std::remove_reference_t<decltype(Walker::m_keptByUnionTableCalls)> keptByUnionTableCalls;
    std::remove_reference_t<decltype(Walker::m_keptByCopiesOwnKeys)> keptByCopiesOwnKeys;
    std::remove_reference_t<decltype(Walker::m_keptByPlainInstances)> keptByPlainInstances;
    std::remove_reference_t<decltype(Walker::m_keptByInstanceOf)> keptByInstanceOf;
    std::remove_reference_t<decltype(Walker::m_keptByInlining)> keptByInlining;
    std::remove_reference_t<decltype(Walker::m_keptByPositionsInTexts)> keptByPositionsInTexts;
    std::remove_reference_t<decltype(Walker::m_roundsKept)> roundsKept;
    std::remove_reference_t<decltype(Walker::m_listsOfNames)> listsOfNames;
    std::remove_reference_t<decltype(Walker::m_numbersOfListsOfNames)> numbersOfListsOfNames;
    std::remove_reference_t<decltype(Walker::m_surveysOfConstants)> surveysOfConstants;
    std::remove_reference_t<decltype(Walker::m_constantsThatListFieldsHeldFilled)> constantsThatListFieldsHeldFilled;
    std::remove_reference_t<decltype(Walker::m_accessesOfScopesByFunctions)> accessesOfScopesByFunctions;
    std::remove_reference_t<decltype(Walker::m_isArrayIteratorProtocolIntact)> isArrayIteratorProtocolIntact;
    std::remove_reference_t<decltype(Walker::m_objectPrototypeMayGainName)> objectPrototypeMayGainName;
    std::remove_reference_t<decltype(Walker::m_numbersOfFramesOfFunctions)> numbersOfFramesOfFunctions;
    std::remove_reference_t<decltype(Walker::methodsGivenUnderKeys)> methodsGivenUnderKeys;
    std::remove_reference_t<decltype(Walker::receiversMadeInBodies)> receiversMadeInBodies;
    std::remove_reference_t<decltype(Walker::gettersExpectedInFunction)> gettersExpectedInFunction;
    const Walker* newestWalker { nullptr };
    const Mechanisms mechanismsGiven;
    const Mechanisms mechanismsOfBuild;
    Code code;
    std::array<bool, 2> roomOfWalkerIsTaken { };
    std::array<std::unique_ptr<PositionTypes>, 2> positionsOfBuild;
    std::array<std::unique_ptr<PositionTypes>, 2> positionsKnowingWhatFunctionsHandBack;
    size_t roomOf(const Walker& walker) const { return static_cast<const void*>(&walker) == roomsOfWalkers[1].data(); }
    alignas(Walker) std::array<std::array<std::byte, sizeof(Walker)>, 2> roomsOfWalkers;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_pendingBlocks)>> roomOfPendingBlocks;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_bindings)>> roomOfBindings;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_unspecifiedConstants)>> roomOfUnspecifiedConstants;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_blocksWithFieldsOfScopesInOrder)>> roomOfBlocksWithFieldsOfScopesInOrder;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_preheaderDepths)>> roomOfPreheaderDepths;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_countsOfUnit)>> roomOfCountsOfUnit;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_trialVisits)>> roomOfTrialVisits;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_inputBlocksInTries)>> roomOfInputBlocksInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_statesOfHandlersInTries)>> roomOfStatesOfHandlersInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_placesOfOpenLoopsInTries)>> roomOfPlacesOfOpenLoopsInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_inputHeadsOfIterationsAddedInTries)>> roomOfInputHeadsOfIterationsAddedInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_typesChangedInTries)>> roomOfTypesChangedInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_entriesOfCopiesOfLoopsInTries)>> roomOfEntriesOfCopiesOfLoopsInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_preheaderDepthUndoLog)>> roomOfPlacesOfLoopsChangedInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_sharedConstantsAddedInTries)>> roomOfSharedConstantsAddedInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_copiesOfConstantsAddedInTries)>> roomOfCopiesOfConstantsAddedInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_operationsBeingWalkedInFrames)>> roomOfOperationsBeingWalkedInFrames;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_frameLogSizes)>> roomOfFrameLogSizes;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_expectationsInTries)>> roomOfExpectationsInTries;
    RoomOnly<std::remove_reference_t<decltype(Walker::m_headParametersWithSources)>> roomOfHeadParametersWithSources;
    bool workerHasBegunUnit { false };
    const std::array<uint32_t, numberOfOpcodes> stepsOfBuildByOpcode = Walker::findStepsOfBuildByOpcode();
    const std::array<uint8_t, numberOfOpcodes> stepsOfWalkByOpcode = Walker::findStepsOfWalkByOpcode();
};

class Walker::ClosedArm {
    WTF_MAKE_NONCOPYABLE(ClosedArm);
public:
    explicit ClosedArm(Walker&, bool restartsIterationIfPossible = false);
    ~ClosedArm();

private:
    Walker& m_walker;
    size_t m_firstBinding;
    bool m_openedGuesses;
};

void Walker::walkArmAsWritten(const StretchInArms& stretch, ArmsThatMeet& arms, NOESCAPE const Invocable<void()> auto& walk)
{
    ClosedArm closedArm(*this);
    walk();
    if (!isUnreachable() && !m_hasFailed)
        listArm(stretch, arms);
}

class Walker::SpecializedPreheaderScope {
    WTF_MAKE_NONCOPYABLE(SpecializedPreheaderScope);
public:
    SpecializedPreheaderScope(Walker&, bool shouldHoist, IterationPurity = IterationPurity::EffectFree);
    ~SpecializedPreheaderScope();

private:
    Walker& m_walker;
    bool m_isActive;
    BlockID m_savedBlock;
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
    bool countsOnMembersAbovePrimitives { false };
    Vector<TypeID, 8> typesOfParameters;
    Vector<bool, 8> parametersSteppedOnDoubles;
    bool hasLetMentionsLeave { false };
    Vector<Reference, 2> scopesThatTurnsOnlyBorrow;
    Vector<FieldReference, 4> fieldsOfScopesThatTurnsStore;
    Vector<FieldReference, 4> fieldsOfScopesThatCallsInTurnsStore;
    Vector<SlotOfLoop, 2> slots;
    Vector<CapturedCounter, 2> counters;
    TypesOfValuesOfLoop typesOfValues;
    uint32_t firstValue { 0 };
};
std::optional<std::array<BodyEdgeResult, 2>> boundsProvenOnEdge(const BodyIndex&, const BodyTerminator&, uint32_t targetIndex);
ValueID relationThatNarrowsBehind(const Walker&, const BodyTerminator&);
bool relationBoundsNothing(const BodyIndex&, const BodyValue& relation);
LoopPreview makePessimisticPreview(Walker&, BlockID inputHead, std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround);
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
std::optional<Effects> effectsOfTypedFormAhead(Walker&, const BodyValue&, const TypesOfInputs&, FunctionID* halfRunBehindLooks = nullptr, bool sitesMayAnswer = false);
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
void noteObjectPassedTestOfBeingAsMade(Walker&, Reference object, Reference test);
BuiltinID findConstantMemberOnceForBuild(const Walker&, BuiltinID start, PropertyKeyID);
BuiltinID findMemberAskedByItsByte(const Walker&, BuiltinID holder, PropertyKeyID);
BuiltinID findMemberAskedByItsByteOffType(const Walker&, TypeID typeOfHolder, PropertyKeyID);
BuiltinID findMemberThatGuessAsksByItsByte(Walker&, const BodyValue& read, TypeID typeOfHolder);
TypeID typeOfMemberAskedByItsByte(Walker&, const BodyValue& read, TypeID typeOfHolder);
Reference buildTestOfMemberAsBuilt(Walker&, BuiltinID member, Origin);
void noteMemberPassedTestOfBeingAsBuilt(Walker&, BuiltinID member);
Reference buildTestOfHolderWithoutAddedNames(Walker&, BuiltinID holder, Origin);
void noteHolderPassedTestOfHavingNoAddedNames(Walker&, BuiltinID holder);
BuiltinID findConstantMemberReadOff(const Walker&, TypeID object, PropertyKeyID);
BuiltinID findConstantMemberReadOff(Walker&, Reference object, PropertyKeyID);
TypedForm buildReadOfBuiltinMember(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object);
TypedForm buildStoreThroughSetterOfBuiltin(Walker&, const BodyValue& store, TypeID typeOfObject, TypeID typeOfStored, Reference object);
TypeID typeOfFunctionReadOffFunctionPrototype(Walker&, PropertyKeyID);
TypedForm buildReadOffFunctionPrototype(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object);
TypedForm buildObjectPrototypeMemberRead(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object, BuiltinID location);
bool buildOwnPropertyTest(Walker&, const BodyValue& question, Reference object, PropertyKeyID);
bool dispatchOwnPropertyTest(Walker&, const BodyValue& question, Reference object, Reference key);
BuiltinID findMemberAboveHeirsOfBuiltinClass(const Walker&, BuiltinClassID, PropertyKeyID);
TypedForm buildReadOfMemberAboveHeirOfBuiltinClass(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object, BuiltinID location);
bool readsOffEntryParameterThatStaysAsMade(Walker&, const BodyValue& access);
bool someThreadMayGrowSharedMemory(const Walker&);
bool viewMayFollowSharedMemoryThatGrows(const Walker&, Reference view);
enum class WordOfBodyOnAsMade : bool { IsNotTaken, IsTaken };
bool receiverStaysAsMadeByLink(Walker&, const BodyValue& operation, WordOfBodyOnAsMade = WordOfBodyOnAsMade::IsNotTaken);
bool linksWordCanBeHeldAgainst(const Walker&, Reference object);
void holdLinksWordAgainstObjectAsMade(Walker&, Reference object, Origin);
bool readsOffListThatStaysAsMadeByLink(Walker&, const BodyValue& read);
bool readsPlainListByLink(Walker&, const BodyValue& read);
BuiltinClassID classOfIterableThatStaysAsMadeByLink(Walker&, const BodyValue& operation, ValueID iterable);
bool isArrayPrototypeChainSane(const Walker&);
Reference buildReadOfElementOffExpectedList(Walker&, const BodyValue& read);
TypeID typeOfElementInsideViewOfNumbers(Walker&, const BodyValue& read, TypeID typeOfView, TypeID typeOfPosition, bool readIsUpdated = false);
Reference buildReadOfElementOfViewBehindTestOfLength(Walker&, const BodyValue& read, Reference view, Origin, BlockID whereItLiesOutside, bool readIsUpdated = false);
bool isViewOfNumbersReadInLine(Walker&, TypeID typeOfView);
bool loopIndexesViewsByViewElements(Walker&, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeOfValue);
ValueID findObjectCopiedInLoop(Walker&, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeOfValue);
Reference buildIntegerOperator(Walker&, Opcode, Reference left, Reference right, Origin);
Reference buildTruncatedSum(Walker&, Reference sum, Origin);
Reference findElementRead(Walker&, Reference array, Reference position);
void renameElementRead(Walker&, Reference array, Reference position, Reference name);
Reference buildRoomOfViewBesideAddend(Walker&, Reference view, Reference addend, Origin);
Reference buildArrayLength(Walker&, Reference list, Origin);
bool splitNullishOffReceiverOfAccess(Walker&, const BodyValue& access);
void buildFirstReadOfNamedBuiltin(Walker&, const BodyValue& named);
bool firstReadOfNamedBuiltinIsSeen(const Walker&, const BodyValue& named);
bool buildInstanceOfBuiltin(Walker&, const BodyValue& test, const InputsOfTypedOperation&);
enum class TestOfObjectAsMade : uint8_t { AsksWhatWasWrittenAboveToo, AsksObjectAlone };
Reference buildTestOfObjectAsMade(Walker&, Reference object, Origin, TestOfObjectAsMade = TestOfObjectAsMade::AsksWhatWasWrittenAboveToo);
enum class BeliefInMemberAbove : uint8_t { IsConstant, IsAskedByItsByte, RestsOnByteOfClass };
using HoldersBelowMember = Vector<BuiltinID, 2>;
BeliefInMemberAbove beliefInMemberFoundOnObjectAsMade(const Walker&, BuiltinID member, TypeID typeOfObject, PropertyKeyID, HoldersBelowMember* = nullptr);
BuiltinID findMemberOnObjectAsMade(const Walker&, const BodyValue& read, TypeID typeOfObject);
BuiltinID findMemberOnObjectAsMade(const Walker&, TypeID typeOfObject, PropertyKeyID);
TypeID typeFoundOnObjectAsMade(Walker&, const BodyValue& read, TypeID typeOfObject);
TypeID typeFoundAbovePrimitiveBehindWatch(Walker&, const BodyValue& read, TypeID typeOfPrimitive);
TypeID typeFoundBehindLooks(Walker&, const BodyValue& read, TypeID typeOfObject);
TypeID closuresOfOptionalCallee(Walker&, ValueID callee, const BodyValue& call);
bool findsOneMethodThroughLookups(Walker&, const BodyValue& read, TypeID typeOfObject);
bool readsStaticOfClass(Walker&, const BodyValue& read, TypeID typeOfClassObject);
ValueID findForwardedCall(const Walker&, const BodyValue& read);
Reference buildLookAtStructureOfClass(Walker&, Reference value, LayoutID, Origin, bool asksElements = false);
Reference buildNameBehindLookAtStructureOfClass(Walker&, Reference value, Reference look, LayoutID, Origin);
Reference nameBehindLookAtStructureOfClass(const Walker&, Reference object, LayoutID);
LayoutID arraySubclassWithElements(const Walker&, TypeID);
LayoutID viewSubclassWithLength(const Walker&, TypeID);
LayoutID subclassOfArrayName(const Walker&, Reference);
Draft draftSubclassElementsTest(Reference array, LayoutID subclass, Origin);
Reference arrayNameOfInstance(Walker&, Reference instance, Origin);
bool isLengthOfTypedArraySubclass(const Walker&, const BodyValue& read, LayoutID subclass);
void buildTypedArraySubclassLength(Walker&, const BodyValue& read, Reference view, LayoutID subclass);
void checkStructureOfClassThatIsRemembered(Walker&, Reference name, LayoutID, Origin);
Reference buildReadOfFieldOfHeir(Walker&, Reference name, LayoutID, uint32_t field, Origin);
Reference buildStoreIntoFieldOfHeir(Walker&, Reference name, Reference stored, LayoutID, uint32_t field, Origin);
void nameFieldInEffectsOfAccessToHeir(const Program&, Opcode, PropertyKeyID, Effects&);
TypeID numbersExpectedOfFieldOfHeir(Walker&, LayoutID heir, uint32_t field);
bool readOffConstructorFunctionAwaitsGuard(Walker&, const BodyValue& read, TypeID typeOfObject);
bool findsMethodReadOffPrototypeOfClass(Walker&, const BodyValue& readOfPrototype, TypeID typeOfFunctionObject, bool& guardsHold);
bool canGuardMethodOffPrototype(Walker&, const BodyValue& readOfPrototype);
bool findParentNamedByClassRecordForTypedAccess(const Walker&, LayoutID classLayout, LayoutID& parent);
bool hasOnlyNamedInheritedMembers(Walker&, LayoutID classLayout);
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
ValueID findLoweredCounterUnder(Walker&, const BodyIndex&, ValueID value);
const Walker::IndexedIteration* findIndexedArrayIteration(Walker&, const BodyIndex&, const BodyValue& step, bool allowsAnyIterationKind = false);
const Walker::IndexedIteration* findDestructuredEntriesIteration(Walker&, const BodyIndex&, const BodyValue& open);
bool stepsViewByIndexAlone(Walker&, const BodyIndex&, const BodyValue& step);
bool stepsTextByIndexAlone(Walker&, const BodyIndex&, const BodyValue& step);
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
std::optional<HeldRegExp> findHeldRegExp(const Walker&, ValueID operand);
std::optional<HeldRegExp> findRegExpPassed(const Walker&, ValueID operand);
ValueID findWrittenArgument(const BodyValue& call, const Function& callee, uint32_t entryParameter);
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
    DefinesFilledSlot = 1 << 7,
};
bool possibleChangesForbid(OptionSet<LayoutChange> possibleChanges, TypedAccess, OptionSet<CellFact> knownOfCell = { }, bool guardOfReadsHolds = false);
bool testsOnlyFieldOverMethod(Walker&, const BodyValue& access);
bool callOwnOrMethodByArms(Walker&, const BodyValue& read);
MemberFound findMemberAboveObjectsOf(const Program&, const LinkTables&, LayoutID, PropertyKeyID);
std::optional<FindsNothing> findsNothingAboveObjectsOf(const Program&, const LinkTables&, LayoutID, PropertyKeyID);
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
ValueID findFirstFill(TypeArena&, const BodyIndex&, BlockID);
struct FilledSlot {
    FieldReference place;
    LayoutSetID receivers;
};
std::optional<FilledSlot> findFilledSlot(Walker&, TypeID typeOfObject, PropertyKeyID);
std::optional<FilledSlot> findPermanentlyFilledSlot(Walker&, TypeID typeOfObject, PropertyKeyID, bool everyStoreFits = false);
bool storeMaySetMarkOfCell(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID, TypeID typeOfStored);
bool typedStoreAheadMaySetMarkOfCell(Walker&, TypeID typeOfObject, PropertyKeyID, TypeID typeOfStored);
bool buildStoreThatTestsForItself(Walker&, const BodyValue& store, Reference object, TypeID guessed);
Reference readSlotFoundFilled(Walker&, const BodyValue& access, Reference object, bool isStoredInLoop = false);
Reference readMethodAboveProvenObject(Walker&, Reference object, PropertyKeyID, Origin);
FunctionID findMethodAboveProvenObject(Walker&, Reference object, PropertyKeyID);
FunctionID findMethodAboveProvenType(Walker&, TypeID proven, PropertyKeyID);
FunctionID findMethodInSightOfProvenType(Walker&, TypeID proven, PropertyKeyID);
Reference readMethodInSightOfProvenObject(Walker&, Reference object, PropertyKeyID, Origin);
Reference readMethodForCallOnly(Walker&, Reference object, PropertyKeyID, Origin);
Reference readFilledFieldOfProvenObject(Walker&, Reference object, PropertyKeyID, Origin);
Reference readFilledFieldInPreheader(Walker&, const BodyValue& read, Reference object, bool mustHoldArray, bool mayCheckSlot);
Reference findFieldValue(Walker&, Reference object, PropertyKeyID);
void noteFieldProof(Walker&, Reference object, PropertyKeyID, Reference proof);
Reference readFieldArrayOfHolder(Walker&, Reference object, PropertyKeyID, Origin);
bool isFieldOfUnknownObject(Walker&, const BodyValue& read, Reference object);
bool canHoistFieldRead(Walker&, const BodyValue& read, Reference object);
Reference buildHoistedFieldRead(Walker&, const BodyValue& read, Reference object, BlockID missing);
Reference findHoistedFieldInCopy(Walker&, ValueID fieldRead);
bool findsNothingUnderNameOfProvenObject(Walker&, Reference object, PropertyKeyID);
Reference valueHeldOfFilledSlot(Walker&, const BodyValue& access, Reference object);
void noteAnswerOfReadOfSlotThatMayBeEmpty(Walker&, const BodyValue& access, Reference object, Reference answer);
void noteSlotFilledWhereAnswerOfItsReadIsTruthy(Walker&, Reference answer);
TypeID typeFoundInFilledSlot(Walker&, const BodyValue& read, TypeID typeOfObject);
TypeID typeSeenToArriveInFilledSlot(Walker&, const BodyValue& read, TypeID typeOfObject);
TypeID typeTakenBySlotOfStore(Walker&, const BodyValue& store, TypeID typeOfObject);
void noteSlotFoundEmpty(Walker&, const BodyValue& access, Reference object);
bool definePlainFieldInSlot(Walker&, const BodyValue& call, ValueID objectAsWritten, Reference object, PropertyKeyID, Reference stored);
bool emptySlotIsUndefinedBehindLooks(Walker&, const BodyValue& read, Reference object);
FunctionID methodDeclaredAboveObjects(const Walker&, LayoutID, PropertyKeyID);
void lookAtCellInFrontOfStores(Walker&, std::span<const ValueID> stores, Reference object, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses, std::span<const ValueID> accessesThatLookAtSlots = { });
std::optional<uint32_t> findLikelyOrderWordInFrontOfStores(Walker&, ValueID firstOfStretch, std::span<const ValueID> stores, Reference object, TypeID typeOfObject = { });
void testOrderWordInFrontOfStores(Walker&, ValueID firstStore, Reference object, uint32_t orderWord, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses);
bool looksAtSlotOfObjectBeingMade(const Walker&, const BodyValue& access);
bool isBeingMadeByBodyBeingRead(const Walker&, ValueID object);
bool canBePartiallyConstructed(Walker&, ValueID object, LayoutSetID);
void noteThisAtEntry(Walker&, Reference thisValue);

struct ExpectationOfValue {
    TypeID type;
    bool isDeclared { false };
};
ExpectationOfValue findExpectation(Walker&, ValueID input);
bool isUsedAsString(Walker&, ValueID input);
std::span<const uint32_t> slicingCalls(Walker&);
std::span<const uint32_t> additionsToTables(Walker&);
TypeID findPrimitivesExpectedBesideObjects(Walker&, ValueID input);
LayoutID layoutThatNamesFamily(Walker&, TypeID);
bool valueFitsSlot(Walker&, FieldReference, Reference value);
ExpectationOfValue expectationOfAnswerOfCallBeingCopied(Walker&, const BodyValue& call);
bool isEntryParameterUninformative(Walker&, FunctionID callee, uint32_t entryParameter);
uint64_t entryParametersOnlyReadByTwoNamesOrMore(const BodyIndex&);
uint64_t numbersReadAfterWrites(const BodyIndex&);
Walker::UseOfTextAsKey useOfAnswerOfCallBeingCopiedAsKey(Walker&, const BodyValue& call, const BodyIndex& copied);
bool guessAtCalleeAsksForBuiltinWhereItFails(Walker&, const BodyValue&, ValueID operand);
void forEachTypeThatGuessWouldProveAt(Walker&, const BodyValue&, const TypesOfInputs&, NOESCAPE const ScopedLambda<void(ValueID operand, TypeID expected, TypeID proven)>&);
void guardExpectedType(Walker&, ValueID input, TypeID expected, Origin, Walker::GuardedIterations = Walker::GuardedIterations::All);
bool rowIsToldApartFromViewsInTurn(Walker&, const BodyValue& read, TypeID expected);
TypeID typeThatTypeTestAsksForWhereExpected(Walker&, Reference value, TypeID expected);
bool guessesMayTypeOperationInCleanTurn(Walker&, const BodyValue&, const TypesOfInputs&, bool sitesMayAnswer);
FunctionID findExpectedMethod(Walker&, const BodyValue& call);
bool callsReaders(Walker&, BlockID inputHead);
bool patternMayTakeListApartInCleanTurn(Walker&, const BodyValue& open);
bool destructuresReturnedLiteral(const Walker&, const BodyIndex&, ValueID open);
bool valueIsOnlyReckonedWithAsNumber(Walker&, ValueID, const TypesOfInputs&);
bool someReadThatAsksItsSiteIsReckonedWith(Walker&, std::span<const ValueID> reads, const TypesOfInputs&);
bool readThatAsksItsSiteIsReckonedWithAsNumber(Walker&, ValueID read, const TypesOfInputs&);
Reference guardNumericCallResult(Walker&, const BodyValue& call, Reference found, Origin);
bool lookupInMapIsOnlyReckonedWithAsNumber(Walker&, const BodyValue& call, const TypesOfInputs&);
bool siteMayAnswerReadWithoutOtherCode(const Walker&, const BodyValue& read);
bool canTestPresenceBeforeLoop(const Walker&, const BodyValue& question);
bool readsNumberFromSite(Walker&, const BodyValue& read, const TypesOfInputs&);
bool startsFirstStepOfIterator(Walker&, const BodyValue& read);
bool mayReadLengthInIterations(const Walker&, const BodyValue& read);
enum class KeyHolders : uint8_t { Lists, AnyValues };
bool isUsedForKey(Walker&, ValueID, KeyHolders);
bool mayUseGlobalBindingInIterations(const Walker&, const BodyValue& access, TypeID typeOfObject);
struct GlobalAddress {
    Reference address;
    bool isCheckedInPreheader { false };
};
GlobalAddress buildGlobalAddress(Walker&, const BodyValue& access, Origin);
bool readFiledGlobal(Walker&, const BodyValue& read);
void narrowGlobalAccessEffects(const Program&, Opcode, PropertyKeyID, Effects&);
void narrowStaticFieldEffects(Walker&, const Draft&, Effects&);
bool readsStaticMethod(const Walker&, const Draft& read);
std::optional<Effects> effectsOfStaticFieldAccess(Walker&, const BodyValue& access, TypeID holder);
Reference buildLazyBuiltinValue(Walker&, BuiltinID, BuiltinOverloadID, Origin);
LocationMasks readsOfOwnKeyLookup(const Program&);
void narrowKeyedAccessEffects(Walker&, const Draft&, Effects&);
void narrowNamedLookupEffects(const Program&, PropertyKeyID, Effects&);
bool mayUseKeyedAddress(Walker&, const BodyValue& access, TypeID holder, TypeID key);
bool mayDispatchOverNames(Walker&, const BodyValue& access, TypeID holder, TypeID key);
Effects effectsOfDispatchOverNames(Walker&, const BodyValue& access, TypeID holder);
bool sharesKeyWithOtherAccess(Walker&, const BodyValue& access);
bool isKeyedByElement(const BodyIndex&, const BodyValue& access);
Effects effectsOfKeyedAccess(Walker&, const BodyValue& access, TypeID holder);
bool tryHoistKeyVariable(Walker&, const BodyValue& read);
bool tryGuardKeyAsString(Walker&, const BodyValue& test);
bool convertsInvariantKey(Walker&, const BodyValue& conversion);
bool canAccessAtKeyedAddress(Walker&, const BodyValue& access);
bool mayUseNamedAddress(Walker&, const BodyValue& access, TypeID holder);
inline constexpr OpcodeSet opcodesThatTryAccessAtKeyedAddressTakes { Opcode::ElemGet, Opcode::ElemSet, Opcode::PropGet, Opcode::PropSet };
AOT_LINK_OF_WALK(tryAccessAtKeyedAddress, opcodesThatTryAccessAtKeyedAddressTakes)
void readAtAddress(Walker&, const BodyValue& read, Draft&&, bool isInPreheader, TypeID asked, Origin);
TypeID typeRequiredOfRead(Walker&, const BodyValue& read);
bool canLookUpCallee(Walker&, const BodyValue& read);
bool isStaticMethodCallee(Walker&, const BodyValue& read);
TypeID expectedReceiverType(Walker&, const BodyValue& read);
bool tryGuardInvariantBaseAsArray(Walker&, const BodyValue& access);
enum class ReadInArm : uint8_t { No, Read, WalkedStretch };
ReadInArm tryReadInArm(Walker&, const BodyValue& read);
bool tryOpenInBoundsPath(Walker&, const BodyValue& access);
void checkInBoundsOperands(Walker&, const BodyValue& operation);
bool isProvenByViewMirror(Walker&, const BodyValue& access, Reference receiver, TypeID typeOfView);
void askOperandsInArm(Walker&, const BodyValue& operation);
ReadInArm tryStraightArm(Walker&, const BodyValue& operation);
inline constexpr OpcodeSet opcodesThatTryReadAtSiteTakes { Opcode::PropGet, Opcode::ElemGet, Opcode::PropHas };
AOT_LINK_OF_WALK(tryReadAtSite, opcodesThatTryReadAtSiteTakes)
bool shouldOpenGuess(const Walker&, Reference value, TypeID expected, bool isDeclaredInSource, uint32_t numberOfTypedOperations);
void openGuess(Walker&, Reference value, TypeID expected, BlockID inputBlock, uint32_t begin, uint32_t end, TypeID expectedNext = { });
uint32_t limitOfStretchOfArms(Walker&, const BodyBlock&);
bool isSmallLeaf(const Walker&, FunctionID callee, size_t numberOfArguments);
uint32_t findArmStretchEnd(Walker&, const BodyValue& call, uint32_t begin, uint32_t limit, Reference told, const ScopedLambda<bool(const BodyValue&)>& mayTake);

struct SlotThatFindsHolderOfSlotReadInLoop {
    ValueID read;
    TypeID testedOfHolder;
    LayoutSetID layoutsOfHolder;
    FieldReference field;
    bool holderIsProven { false };
    bool holderIsAskedWhetherItIsFilled { false };
};

struct SlotReadInLoopOffHolderInFront {
    ValueID read;
    ValueID holder;
    Reference nameOfHolder;
    bool holderIsReadOffVariableInLoop { false };
    bool holderIsAskedWhetherItIsFilled { false };
    TypeID testedOfHolder;
    LayoutSetID layoutsOfHolder;
    FieldReference field;
    bool holderIsProven { false };
    Vector<SlotThatFindsHolderOfSlotReadInLoop, 1> slotsThatFindHolder { };
};

OtherClassBelowMixin findOtherClassBelowMixin(Walker&, BlockID inputHead);
bool callersComputeNumbers(Walker&, const BodyValue& read);
struct HoistedSpeculation {
    Reference value;
    TypeID tested;
    LayoutSetID layouts;
    Vector<ValueID, 2> names;
    bool nothingWasExpected { false };
    ValueID readOfVariableInLoop;
    std::optional<SlotReadInLoopOffHolderInFront> slotReadInLoop { };
    ValueID readOfRowInLoop;
    bool restIsWalkedAsWithoutGuess { false };
    struct FieldInPreheader {
        Reference holder;
        PropertyKeyID key;
    };
    std::optional<FieldInPreheader> fieldInPreheader { };
    bool isTermOfKey { false };
    bool isOnlyReckonedWith { false };
    ValueID fieldReadInLoop { };
    bool isReceiverOfInnerLoop { false };
    bool isConvertedToInteger { false };
    std::optional<FieldInPreheader> unknownFieldInPreheader { };
};
Vector<HoistedSpeculation, 4> findHoistedSpeculations(Walker&, BlockID inputHead, const LoopPreview&);
std::optional<HoistedSpeculation> guessAtShapeOfHolderReadByName(Walker&, const BodyValue& read);
Reference buildTestOfSlotFilledBehindShape(Walker&, const HoistedSpeculation&, Reference shaped, const BodyValue& read);
void readSlotFilledBehindShape(Walker&, const HoistedSpeculation&, Reference shaped, const BodyValue& read);
bool wordOfFieldThatFindsValueIsDrawnFromUseForGuesses(Walker&, ValueID operand);
bool otherHolderOfLoopMayBeListForGuesses(Walker&, BlockID inputHead, Reference list);
bool headOnlyMeasuresConstantOfFileForGuesses(Walker&, BlockID inputHead, Reference list);
bool readFindsListThatTurnsReckonWithForGuesses(Walker&, ValueID read);
bool turnsOfLoopCallForGuesses(const BodyIndex&, LoopID);
Vector<ValueID, 2> findOtherHoldersWalkedInStepThatMayBeBytesForGuesses(Walker&, BlockID inputHead, Reference holder, bool alsoThoseWithWordOfList = false);
Vector<ValueID, 2> findOtherHoldersExpectedOfClassOfFirstForGuesses(Walker&, BlockID inputHead, Reference holder, TypeID tested, bool alsoThoseWithWordOfList = false);
Vector<ValueID, 2> findTablesThatMayBeViewsForGuesses(Walker&, BlockID inputHead, Reference holder);
Vector<ValueID, 2> findUnknownsComparedWithElements(Walker&, BlockID inputHead, std::span<const ValueID> holders);
TypeID viewsOfNumbersOfEveryKindForGuesses(TypeArena&);
struct HoistedUnknownField {
    ValueID read;
    Reference holder;
    Reference value;
};
Vector<HoistedSpeculation, 4> findMembersToldApartInFrontOfLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const HoistedUnknownField> = { });
Vector<ValueID, 2> findUnknownListFields(Walker&, BlockID inputHead, const LoopPreview&);
bool copyElementsBeforeLoop(Walker&, BlockID inputHead, Vector<Reference, 8>& entering);
bool movesElementsBeforeLoop(Walker&, BlockID inputHead);
Reference hoistUnknownField(Walker&, ValueID read, BlockID fieldIsAbsent, Vector<HoistedUnknownField, 4>&);
bool isFieldReadInLoop(const BodyIndex&, ValueID, uint32_t firstValueOfLoop);
bool readsSameField(const BodyIndex&, ValueID, const BodyValue& field, uint32_t firstValueOfLoop);
Reference findHoistedUnknownField(Walker&, PropertyKeyID, Reference holder);
ValueID findRowThatHeadReadsOffListInFront(Walker&, BlockID inputHead, const LoopPreview&);
Reference readRowInFrontOfLoop(Walker&, const BodyValue& read, BlockID whereRowIsNotFound, Origin);
void noteRowReadInFrontOfLoop(Walker&, const BodyValue& read, Reference row);
bool mayHoistUnknownField(Walker&, ValueID field);
Reference readUnknownFieldAtSite(Walker&, const BodyValue& read);
void noteHoistedUnknownField(Walker&, PropertyKeyID, Reference holder, Reference value);
bool takeHoistedUnknownField(Walker&, const BodyValue& read);
Reference rowFoundUnderKeyInFrontOfLoop(Walker&, const BodyValue& read);
Walker::RowFoundUnderKey readVariablesOfRowInFrontOfLoop(Walker&, const BodyValue& read, BlockID whereRowIsNotFound, Origin);
Reference rowRememberedOfReadBeingWalked(Walker&, const BodyValue& read);
bool readsRowOfCopy(Walker&, const BodyValue& read);
bool holdRowRememberedAgainstReadInTurn(Walker&, const BodyValue& read, Reference row);
bool readRowOfCopyAgainInTurn(Walker&, const BodyValue& read);
static constexpr size_t mostTextsToldApart = 4;
static constexpr size_t mostTextsOfPassedKey = 16;
static constexpr size_t maxDistinguishedCallees = 16;
TypeID textsThatNamesOfHolderSayOfKey(Walker&, const BodyValue& read, bool looksUpCallee);
bool isKeyPassedIntoLoop(Walker&, const BodyValue& read);
bool isElementUsedInArithmetic(Walker&, const BodyValue& read);
bool readsOnlyVariableLength(const Program&, const BodyIndex&, VariableID);
BitVector findHoldersOfElementsThatLoopComparesWithCharactersOrNumbers(const Walker&, const BodyIndex&, LoopID);
bool comparesStepValuesWithCharacters(const Walker&, const BodyIndex&, LoopID, ValueID open);
bool usesStepValuesAsCharacters(Walker&, const BodyIndex&, LoopID, ValueID open);
bool writtenWordOfParameterNamesTextBesideList(Walker&, ValueID operand);
bool readsElementUnderIndexOffOneOf(const BodyIndex&, const BodyValue& read, const BitVector& holders);
TypeID carriedTypeByRows(Walker&, BlockID inputHead, uint32_t position, TypeID start, std::span<const TypeID> entering);
Reference buildTestInFrontOfLoop(Walker&, const HoistedSpeculation&, Origin);
Reference buildProofInFrontOfLoop(Walker&, const HoistedSpeculation&, Reference test, Origin);

Vector<TypeID, 8> typesBehindParameterGuard(Walker&, FunctionID, bool hasGuard);

bool walkEntryUnit(Walker&, CodeUnit);

String findCallThatHandsOverWhatItsUnitHasNotLeft(const WalkInputs&, TypeArena&, const Code&, std::span<const Walker::ArrivalHeld>, const PositionTypes* whatFunctionsHandBack = nullptr);

} } } // namespace JSC::AOT::Typed

#include "AOTWalkCopies.h"

#include "AOTWalkLinksOfChains.h"
#include "AOTWalkForInLinks.h"
#include "AOTWalkHeirsOfBuiltins.h"

#endif // ENABLE(AOT)

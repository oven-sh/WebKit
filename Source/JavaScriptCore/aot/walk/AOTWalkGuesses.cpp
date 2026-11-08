/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkStrings.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include "AOTTypeText.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkCoroutinePositions.h"
#include "AOTWalkCoroutineCalls.h"
#include "AOTArrayStorageForms.h"
#include "AOTSweep.h"
#include "AOTWalkBirths.h"
#include "AOTWalkBlocksThatEveryTurnPasses.h"
#include "AOTWalkBranches.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkTextBuilderRows.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

TypeID topsOfKindsInLoops(TypeArena&, TypeID);
TypeID tryTypeOfFieldRead(const TypingContext&, TypeID receiver, PropertyKeyID);

static constexpr uint32_t minimumOperationsPerGuess = 2;
static constexpr size_t fewestLooksThatOneTestOfMarkStandsFor = 2;
static constexpr uint32_t minimumOperationsPerGuessInLoop = 1;
static constexpr uint32_t minimumOperationsPerGuessOfShape = 1;

static constexpr OpcodeSet constructionsOfParentForGuesses { Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr OpcodeSet callsOfValuesForGuesses = constructionsOfParentForGuesses | OpcodeSet { Opcode::Call, Opcode::Construct, Opcode::CallList };
static constexpr OpcodeSet operationsThatOpenGuesses = opcodesOfFamily(OperationFamily::Access) | opcodesOfFamily(OperationFamily::PrivateAccess)
    | opcodesOfFamily(OperationFamily::Operator) | opcodesOfFamily(OperationFamily::ArrayIntrinsic) | callsOfValuesForGuesses | OpcodeSet { Opcode::RequireObjectCoercible };
static constexpr OpcodeSet readsOfDeclaredPlacesForGuesses {
    Opcode::PropGet, Opcode::PrivateGet, Opcode::ElemGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::Await
};
static constexpr OpcodeSet callsWithExpectedResultsForGuesses { Opcode::Call, Opcode::CallList };
static constexpr OpcodeSet accessesUnderPrivateNamesForGuesses = opcodesOfFamily(OperationFamily::PrivateAccess);
static constexpr OpcodeSet accessesToSlotsForGuesses = OpcodeSet { Opcode::PropGet, Opcode::PropSet, Opcode::PropHas, Opcode::PropDefine } | accessesUnderPrivateNamesForGuesses;
static constexpr OpcodeSet storesIntoSlotsForGuesses { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet readsOutOfSlotsForGuesses { Opcode::PropGet, Opcode::PrivateGet };
static constexpr OpcodeSet operationsWhoseTypedFormsRunNoCodeForGuesses = accessesToSlotsForGuesses | opcodesOfFamily(OperationFamily::Operator) | OpcodeSet { Opcode::RequireObjectCoercible };
static constexpr OpcodeSet operationsThatEndNoProofOfPrimitiveForGuesses = opcodesOfFamily(OperationFamily::Operator) | OpcodeSet { Opcode::PropGet, Opcode::PrivateGet, Opcode::ElemGet, Opcode::RequireObjectCoercible };

namespace {

struct ExpectationForGuesses {
    TypeID type;
    bool isDeclared { false };
    bool isHeld { false };
    bool restsOnNamesUsedAlone { false };
};

struct TestOfGuess {
    TypeID tested;
    LayoutSetID layouts;
    bool isPassedAlready { false };
    TypeID failingSideAsksFor;
    bool failingSideAsksForNothing { false };
    bool asksForElementOfNumbers { false };
    TypeID elementInsideView;
    bool updatesElement { false };
    bool admitsAllThatIsExpected { false };
    bool failingSideIsAsLikely { false };
    bool failingSideKeepsSites { false };
    bool typedRunOpensGuesses { false };
    LayoutID knownByStructure;
};

struct FoundInLongStretchOfGuess {
    using Table = HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    static uint64_t under(ValueID holder, PropertyKeyID key) { return static_cast<uint64_t>(holder.bits()) << 32 | key.bits(); }

    Table slotsStored;
    Table namesStored;
    Table looksAtSlots;
    Table looksAtSlotsOfHoldersInFrontOfStretch;
    Table looksThatAnswerReads;
};

struct LookAtSlotOfGuess {
    ValueID access;
    ValueID holder;
    ValueID holderInFrontOfStretch { };
    bool isCalled { false };
    TypeID calleeSeenToArrive;
};

struct TestOfStoredValueOfGuess {
    ValueID value;
    TypeID tested;
};

struct StretchOfGuess {
    uint32_t end { 0 };
    uint32_t numberOfTypedOperations { 0 };
    uint32_t endOfOperatorsBehind { 0 };
    Vector<LookAtSlotOfGuess, 4> firstAccessesToSlots;
    bool looksWhetherValueIsAsMade { false };
    bool restsOnMoreThanLengthOfView { false };
    Vector<ValueID, 4> storesThatLookAtCell;
    Vector<TestOfStoredValueOfGuess, 4> testsOfStoredValues;
    std::optional<uint32_t> orderWordAskedFor;
    BitVector placesThatRestOnProof;
    BitVector placesThatTypedRunKnowsBetter;
    Vector<BuiltinID, 2> membersAskedByTheirBytes;
    HoldersBelowMember holdersAskedForAddedNames;
    bool someMemberRestsOnByteOfClass { false };
    bool typedRunDecidesBranch { false };
};

struct FirstOperationOfGuessAsItStands {
    const uint32_t* words { nullptr };
    TypedForm form { TypedForm::None };
    Walker::FormAskedAhead askedAhead;
    TypeID askedOfStoredValue;
};

struct BranchOfGuessAskedAbout {
    bool isAsked { false };
    uint32_t end { 0 };
    Reference value;
    bool isTaken { false };
};

} // anonymous namespace

static ALWAYS_INLINE bool saysNothingForGuesses(const TypeArena& types, TypeID type)
{
    return types.saysNothing(type);
}

static TypeID partThatOneTestTellsForGuesses(TypeArena& types, TypeID expected)
{
    Type type = types.at(expected);
    return type.hasObjects() && (type.primitives & ~Primitive::UndefinedOrNull) ? types.withOnlyObjects(expected) : expected;
}

static TypeID provenTypeOfInputForGuesses(const Walker& walker, ValueID input)
{
    Reference built = input ? walker.referenceOf(input) : Reference();
    if (!built && input && walker.typesOfInputsInLook()) {
        walker.setLookHasAskedAboutUnbuiltValue(true);
        if (TypeID inLook = (*walker.typesOfInputsInLook())(input))
            return inLook;
    }
    return built ? walker.typeOf(built) : TypeID(TypeTable::anything);
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeID Walker::closuresThatCopiedCallHandsByItsText(uint32_t entryParameter) const
{
    if (m_frames.size() < 2 || !m_frames.last().callCopied)
        return { };
    if (m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) { return walked.frameOfTurn == m_frames.size() - 1; }))
        return { };
    const BodyIndex& index = *m_frames[m_frames.size() - 2].index;
    if (m_frames.last().callCopied.index() >= index.numberOfValues() || index.isParameter(m_frames.last().callCopied))
        return { };
    BodyValue call = index.value(m_frames.last().callCopied);
    const Function& callee = program().at(m_frames.last().function);
    uint32_t first = callee.firstParameterIndex();
    if (call.opcode() != Opcode::Call || entryParameter < first || entryParameter - first >= callee.numberOfPositionalParameters() || entryParameter - first >= call.argumentsOperands().size())
        return { };
    ValueID handed = index.valueBehindCasts(call.argumentsOperands()[entryParameter - first]);
    if (!handed || index.isParameter(handed))
        return { };
    if (index.opcodeOf(handed) == Opcode::NewClosure)
        return types().forFunction(index.value(handed).as<Opcode::NewClosure>().function());
    if (index.opcodeOf(handed) != Opcode::ElemGet)
        return { };
    ValueID list = index.valueBehindCasts(index.value(handed).objectOperand());
    if (!list || index.isParameter(list) || index.opcodeOf(list) != Opcode::NewArray || !index.value(list).as<Opcode::NewArray>().holes().empty())
        return { };
    TypeID closures;
    for (ValueID element : index.value(list).as<Opcode::NewArray>().elementsOperands()) {
        ValueID written = index.valueBehindCasts(element);
        if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::NewClosure)
            return { };
        TypeID closure = types().forFunction(index.value(written).as<Opcode::NewClosure>().function());
        closures = closures ? types().unionOf(closures, closure) : closure;
    }
    return closures;
}

static TypeID typeHeldOfEntryParameterForGuesses(const Walker&, uint32_t entryParameter);
struct ParametersReadInLoopsForGuesses;
static bool loopsOfFunctionTellListFromBytesForGuesses(Walker&, ValueID parameter);

static ExpectationForGuesses classesSeenInPlaceOfInferredOnesForGuesses(const Walker& walker, uint32_t entryParameter, ExpectationForGuesses inferred)
{
    TypeArena& types = walker.types();
    const Function& function = walker.program().at(walker.currentFunction());
    if (function.has(FunctionFlags::IsChecked) || function.entryParameterKind(entryParameter) != EntryParameter::Parameter || saysNothingForGuesses(types, inferred.type) || !types.at(inferred.type).hasObjects())
        return inferred;
    TypeID held = typeHeldOfEntryParameterForGuesses(walker, entryParameter);
    if (saysNothingForGuesses(types, held) || !types.at(held).hasObjects())
        return inferred;
    TypeID come = types.withOnlyObjects(held);
    TypeID stated = types.withOnlyObjects(inferred.type);
    if (!types.at(come).hasOnlyClassObjects() || !types.at(stated).hasOnlyClassObjects() || types.overlap(come, stated))
        return inferred;
    if (loopsOfFunctionTellListFromBytesForGuesses(const_cast<Walker&>(walker), ValueID(entryParameter)))
        return inferred;
    return { types.withPrimitives(come, static_cast<uint16_t>(types.at(inferred.type).primitives & Primitive::UndefinedOrNull)), true, true };
}

static ExpectationForGuesses expectationOfEntryParameterForGuesses(const Walker& walker, uint32_t entryParameter)
{
    const LinkTables& link = walker.facts().linked();
    FunctionID function = walker.currentFunction();
    if (function.index() >= link.functions.size() || walker.copyIsWalkedWhereItsGuardFailed())
        return { };
    std::span<const LinkedParameter> parameters = link.parametersOf(function);
    if (entryParameter >= parameters.size())
        return { };
    const Function& record = walker.program().at(function);
    if (record.takesThisFromConstruction() && record.constructedClass && record.thisParameterIndex() == entryParameter && walker.plan().isInUse(record.constructedClass)) {
        TypeID expected = typeOfObjectsMadeBy(walker, function);
        for (LayoutID below : walker.program().layoutsExpectedBelow(record.constructedClass)) {
            if (walker.plan().isInUse(below))
                expected = walker.types().unionOf(expected, walker.types().forLayout(below));
        }
        return { expected, true };
    }
    if (TypeID handed = walker.closuresThatCopiedCallHandsByItsText(entryParameter); handed && (saysNothingForGuesses(walker.types(), parameters[entryParameter].written) || walker.types().isSubset(handed, parameters[entryParameter].written)))
        return { handed, true };
    return classesSeenInPlaceOfInferredOnesForGuesses(walker, entryParameter, { parameters[entryParameter].writtenOrSeenToArrive(), !!parameters[entryParameter].written });
}

static ExpectationForGuesses expectationOfConstructionForGuesses(const Walker& walker, const BodyValue& construction)
{
    if (construction.as<Opcode::Construct>().newTargetOperand())
        return { };
    TypeID callee = provenTypeOfInputForGuesses(walker, construction.calleeOperand());
    FunctionID constructor = singleFunctionOf(walker.types(), walker.types().at(callee));
    LayoutID constructed = constructor ? walker.program().at(constructor).constructedClass : LayoutID();
    if (!constructed || !walker.plan().isInUse(constructed))
        return { };
    return { typeOfObjectsMadeBy(walker, constructor), true };
}

static ExpectationForGuesses expectationOfCallForGuesses(Walker& walker, const BodyValue& call)
{
    const Program& program = walker.program();
    const DeclaredType* writtenResult = nullptr;
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id())) {
        const Function& function = program.at(callee);
        if (!function.isCoroutine() && function.results.size() == 1)
            writtenResult = &function.results[0];
    }
    bool resultIsAnnotated = writtenResult && writtenResult->isGuarded();

    if (ExpectedResultID id = call.expectedResult()) {
        const ExpectedResult& expected = program.at(id);
        if (!expected.flags.contains(ExpectedResultFlags::IsRowsOfQuery))
            return { expected.type, resultIsAnnotated };
        if (TypeID promise = typeOfPromiseExpectedToGive(walker, expected.type))
            return { promise, resultIsAnnotated };
    }
    if (resultIsAnnotated)
        return { writtenResult->expected().predicted(), true };
    return { };
}

static ExpectationForGuesses expectationOfDirectCallForGuesses(const Walker& walker, const BodyValue& call)
{
    FunctionID callee = call.as<Opcode::CallDirect>().function();
    const Function& function = walker.program().at(callee);
    if (function.isCoroutine())
        return { };
    if (function.results.size() == 1 && function.results[0].isGuarded() && !saysNothingForGuesses(walker.types(), function.results[0].written()))
        return { function.results[0].expected().predicted(), true };
    const LinkTables& held = walker.facts().held();
    return { callee.index() < held.functions.size() ? held.function(callee).result : TypeID(), true, true };
}

static constexpr OpcodeSet operationsWithoutTypeByRowForGuesses = opcodesWhere([](Opcode opcode) {
    return resultRuleOf(opcode) == ResultRule::None || opcode == Opcode::ResultAt;
});

static NEVER_INLINE TypeID typeByRowForGuesses(const TypingContext& typing, const BodyValue& operation, NOESCAPE const ScopedLambda<TypeID(ValueID operand, uint32_t operandIndex)>& typeOfOperand, std::optional<Draft>& draftOfRow)
{
    Opcode opcode = operation.opcode();
    if (opcode == Opcode::NarrowByTest)
        return typeOfOperand(operation.operand(OperandIndex::NarrowByTest::left), OperandIndex::NarrowByTest::left);
    if (opcodesThatStateTypes.contains(opcode))
        return typeOfOperand(operation.operand(0), 0);
    if (operationsWithoutTypeByRowForGuesses.contains(opcode))
        return { };

    if (!draftOfRow) {
        draftOfRow.emplace(opcode);
        draftOfRow->flags = static_cast<uint8_t>(operation.flags());
        copyFields(operation, *draftOfRow, [](ValueID operand) {
            return Reference::value(operand);
        });
    }
    if (opcodesThatMayHaveSeveralResults.contains(opcode) && numberOfResultsOf(typing.program, *draftOfRow))
        return { };

    size_t numberOfOperands = numberOfNamedOperandsOf(opcode) + operation.variadicOperands().size();
    std::array<TypeID, 8> typesOfFewOperands;
    Vector<TypeID> typesOfManyOperands;
    std::span<TypeID> operandTypes;
    if (numberOfOperands <= typesOfFewOperands.size()) [[likely]]
        operandTypes = std::span(typesOfFewOperands).first(numberOfOperands);
    else {
        typesOfManyOperands.fill(TypeID(), numberOfOperands);
        operandTypes = typesOfManyOperands.mutableSpan();
    }
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        operandTypes[operandIndex] = typeOfOperand(operand, operandIndex);
    });
    return resultTypeOf(typing, *draftOfRow, OperandTypes { operandTypes });
}

static TypeID typeByRowForGuesses(const TypingContext& typing, const BodyValue& operation, NOESCAPE const ScopedLambda<TypeID(ValueID operand, uint32_t operandIndex)>& typeOfOperand)
{
    std::optional<Draft> draftOfRow;
    return typeByRowForGuesses(typing, operation, typeOfOperand, draftOfRow);
}

static std::optional<uint8_t> placeOfReceiverForGuesses(Opcode opcode)
{
    if (opcode == Opcode::Await)
        return OperandIndex::Await::value;
    return receiverOf(opcode);
}

static ALWAYS_INLINE TypeID typeOfReadForGuesses(Walker& walker, const TypingContext& typing, const BodyValue& read, TypeID receiver)
{
    if (read.opcode() == Opcode::PropGet && !read.isFreeName()) {
        TypeID object = receiver ? receiver : provenTypeOfInputForGuesses(walker, read.objectOperand());
        return isNeverType(object) ? object : tryTypeOfFieldRead(typing, object, read.key());
    }
    std::optional<uint8_t> placeOfReceiver = placeOfReceiverForGuesses(read.opcode());
    return typeByRowForGuesses(typing, read, [&](ValueID operand, uint32_t operandIndex) {
        bool isReceiver = placeOfReceiver && *placeOfReceiver == operandIndex;
        return receiver && isReceiver ? receiver : provenTypeOfInputForGuesses(walker, operand);
    });
}

static ALWAYS_INLINE TypeID typeOfReadAtLevelExpectedForGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    return typeOfReadForGuesses(walker, { walker.program(), walker.types(), walker.positionsKnowingNothing(), TypeLevel::Expected }, read, receiver);
}

static ALWAYS_INLINE bool readsFieldWithoutWrittenTypeForGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    if (read.opcode() != Opcode::PropGet || read.isFreeName() || !receiver)
        return false;
    TypeArena& types = walker.types();
    auto hasFieldWithoutWrittenType = [&](LayoutID member) {
        std::optional<FieldReference> field = findField(walker.program(), member, read.key());
        if (!field || walker.program().at(field->layout).kind == LayoutKind::Container)
            return false;
        const DeclaredType& declared = walker.program().at(field->layout).fields[field->field].declared;
        return declared.isExpected() && declared.writtenFlags().contains(PositionFlags::IsFromCompiler);
    };
    Type objects = types.at(receiver);
    if (liesInThread(read.key()) || !objects.hasOnlyLayouts() || holdsOneMember(objects.layouts))
        return std::ranges::any_of(membersOf(types, objects), hasFieldWithoutWrittenType);
    auto askHolders = [&] {
        Vector<LayoutID, 8> holders;
        walker.facts().layoutsByNames().appendHoldersAmong(read.key(), types.listOf(objects.layouts), holders);
        return std::ranges::any_of(holders, hasFieldWithoutWrittenType);
    };
    if (types.keeps() != ArenaKeeps::UntilReset)
        return askHolders();
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::ReadFindsFieldWithoutWrittenType);
    TypeArena::SetOfRound round = types.setOfRound(objects.layouts);
    if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, read.key().bits()))
        return (*kept)[0];
    bool answer = askHolders();
    types.rememberRound(number, 1, 0, round, read.key().bits(), { answer });
    return answer;
}

static TypeID typeOfReadByWhatIsHeldForGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    TypeArena& types = walker.types();
    TypingContext byWhatIsHeld { walker.program(), types, walker.heldPositions(), TypeLevel::Proven };
    TypeID ofAll = typeOfReadForGuesses(walker, byWhatIsHeld, read, receiver);
    if (!receiver) {
        TypingContext byWhatIsProven { walker.program(), types, walker.positions(), TypeLevel::Proven };
        return ofAll && !types.at(ofAll).classes && typeOfReadForGuesses(walker, byWhatIsProven, read, receiver) == ofAll ? TypeID() : ofAll;
    }
    if (read.opcode() != Opcode::PropGet || read.isFreeName())
        return ofAll;
    Type objects = types.at(types.withoutNullish(receiver));
    static constexpr size_t mostReceiversAskedOneByOne = 4;
    if (!objects.isObject() || !objects.hasOnlyLayouts() || objects.hasOnlyClosures() || holdsOneMember(objects.layouts) || types.at(objects.layouts).size() > mostReceiversAskedOneByOne)
        return ofAll;
    bool someIsInstance = false;
    bool someIsRecord = false;
    for (LayoutID member : membersOf(types, objects.layouts))
        (walker.program().at(member).kind == LayoutKind::Instance ? someIsInstance : someIsRecord) = true;
    bool instancesSpeakFirst = someIsInstance && someIsRecord;
    if (!instancesSpeakFirst && !saysNothingForGuesses(types, ofAll))
        return ofAll;
    TypeID said;
    TypeID saidByInstances;
    for (LayoutID member : membersOf(types, objects.layouts)) {
        TypeID ofMember = tryTypeOfFieldRead(byWhatIsHeld, types.forLayout(member), read.key());
        if (!ofMember || isNeverType(ofMember) || saysNothingForGuesses(types, ofMember))
            continue;
        said = said ? types.unionOf(said, ofMember) : ofMember;
        if (walker.program().at(member).kind == LayoutKind::Instance)
            saidByInstances = saidByInstances ? types.unionOf(saidByInstances, ofMember) : ofMember;
    }
    if (saidByInstances)
        return saidByInstances;
    return said && saysNothingForGuesses(types, ofAll) ? said : ofAll;
}

static bool holdsOnlyCreatedObjectsForGuesses(const Walker& walker, TypeID held)
{
    TypeArena& types = walker.types();
    Type type = types.at(held);
    if (!type.hasOnlyLayouts())
        return false;
    auto askEachLayout = [&] {
        return std::ranges::all_of(membersOf(types, type), [&](LayoutID layout) {
            return walker.program().at(layout).has(LayoutFlags::IsOfCreatedObject);
        });
    };
    if (!Walker::keepsRoundsOver(types, type.layouts))
        return askEachLayout();
    if (std::optional<uint32_t> kept = walker.roundKept(Walker::RoundKeptByWalker::AreAllOfCreatedObjects, type.layouts, 0))
        return *kept;
    bool areAllCreated = askEachLayout();
    const_cast<Walker&>(walker).keepRound(Walker::RoundKeptByWalker::AreAllOfCreatedObjects, type.layouts, 0, areAllCreated);
    return areAllCreated;
}

static TypeID typeHeldOfEntryParameterForGuesses(const Walker& walker, uint32_t entryParameter)
{
    const LinkTables& held = walker.facts().held();
    FunctionID function = walker.currentFunction();
    if (function.index() >= held.functions.size() || walker.copyIsWalkedWhereItsGuardFailed())
        return { };
    std::span<const LinkedParameter> parameters = held.parametersOf(function);
    if (entryParameter >= parameters.size())
        return { };
    const LinkedParameter& parameter = parameters[entryParameter];
    bool isThisOfMemberAboveCreatedObjects = parameter.writtenOrSeenToArrive() && walker.program().at(function).thisParameterIndex() == entryParameter && holdsOnlyCreatedObjectsForGuesses(walker, parameter.writtenOrSeenToArrive());
    return isThisOfMemberAboveCreatedObjects ? parameter.writtenOrSeenToArrive() : parameter.proven;
}

static TypeID typeHeldOfResultOfCallForGuesses(const Walker& walker, const BodyValue& call)
{
    const LinkTables& held = walker.facts().held();
    FunctionID callee = held.calleeAt(walker.currentFunction(), call.id());
    if (callee && callee.index() < held.functions.size() && walker.program().at(callee).isCoroutine())
        return typeHeldOfResultOfCoroutine(walker, callee);
    if (!callee || callee.index() >= held.functions.size())
        return { };
    return held.function(callee).result;
}

static TypeID typeOfReadOffBuiltinForGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    TypingContext byRulesAlone { walker.program(), walker.types(), walker.positionsKnowingNothing(), TypeLevel::Proven };
    return typeOfReadForGuesses(walker, byRulesAlone, read, receiver);
}

static ValueID iterableThatStepReadsForGuesses(const BodyIndex& index, const BodyValue& result)
{
    auto read = result.as<Opcode::ResultAt>();
    ValueID step = read.resultsOperand();
    if (read.index() != static_cast<uint32_t>(IteratorResumeResult::Value) || index.isParameter(step) || index.opcodeOf(step) != Opcode::IteratorResume)
        return { };
    ValueID open = index.value(step).as<Opcode::IteratorResume>().iteratorOperand();
    if (index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen || index.value(open).as<Opcode::IteratorOpen>().iteratorKind() != IteratorKind::Sync)
        return { };
    return index.value(open).as<Opcode::IteratorOpen>().iterableOperand();
}

namespace {

static bool isEntryParameterProvenToBeWholeNumberForGuesses(const Walker& walker, const BodyIndex& index, ValueID value)
{
    if (!index.isParameter(value) || index.blockOf(value) != entryBlock)
        return false;
    const LinkTables& link = walker.facts().linked();
    FunctionID function = walker.currentFunction();
    if (function.index() >= link.functions.size())
        return false;
    std::span<const LinkedParameter> parameters = link.parametersOf(function);
    if (value.index() >= parameters.size() || !parameters[value.index()].proven)
        return false;
    Type proven = walker.types().at(parameters[value.index()].proven);
    return proven.hasOnlyPrimitives(Primitive::NumberWithoutFraction) && (proven.primitives & Primitive::Integer);
}

static bool keyOfReadIsIndexByItsTextForGuesses(const Walker& walker, const BodyIndex& index, const BodyValue& access)
{
    ValueID key = index.valueBehindCasts(access.propertyOperand());
    if (index.isParameter(key))
        return index.isLoopHead(index.blockOf(key)) || isEntryParameterProvenToBeWholeNumberForGuesses(walker, index, key);
    Opcode maker = index.opcodeOf(key);
    return maker == Opcode::Integer || operatorsThatMakeNumbers.contains(maker) || allBitwiseOperators.contains(maker);
}

struct KeptWithBodyForGuesses final : KeptWithBodyIndex {
    Vector<uint32_t> firstIncomingEdges;
    Vector<BodyIncomingEdge> incomingEdges;
    Vector<uint32_t> rootsOfValuesWithMarks;
    Vector<uint32_t> rootsAskedForNames;
    Vector<PropertyKeyID> namesUsedOffValues;
    Vector<uint64_t> joinsThatHandNamesBack;
    ALWAYS_INLINE std::span<const PropertyKeyID> ownNamesOf(uint32_t root) const
    {
        auto asked = std::ranges::equal_range(rootsAskedForNames, root);
        return namesUsedOffValues.span().subspan(asked.begin() - rootsAskedForNames.begin(), asked.size());
    }
    auto joinsThatHandNamesBackTo(uint32_t root) const
    {
        return std::ranges::equal_range(joinsThatHandNamesBack, static_cast<uint64_t>(root), { }, [](uint64_t listed) { return listed >> 32; });
    }
    Vector<uint32_t> groupsOfRootsThatHandNamesOn;
    Vector<uint32_t> firstMembersOfGroupsThatHandNamesOn;
    Vector<uint32_t> membersOfGroupsThatHandNamesOn;
    std::span<const uint32_t> membersOfGroupThatHandsNamesOn(uint32_t group) const
    {
        return membersOfGroupsThatHandNamesOn.span().subspan(firstMembersOfGroupsThatHandNamesOn[group], firstMembersOfGroupsThatHandNamesOn[group + 1] - firstMembersOfGroupsThatHandNamesOn[group]);
    }
    Vector<uint32_t> readersOfCharactersNotOnlyCalled;
    Vector<uint32_t> rootsHandedFunctionsWrittenInPlace;
    Vector<uint64_t> castsOfRoots;
    Vector<uint64_t> joinsThatRootsArriveAt;
    static constexpr uint32_t markOfRootThatTextSpeaksOf = 1u << 31;
    uint32_t rootOf(size_t value) const { return rootsOfValuesWithMarks[value] & ~markOfRootThatTextSpeaksOf; }
    bool textSpeaksOfRootOf(size_t value) const { return rootsOfValuesWithMarks[rootOf(value)] & markOfRootThatTextSpeaksOf; }
    Vector<uint64_t> viewsOfRoots;
    Vector<uint32_t> rootsWhoseElementsAreAskedInLoops;
    Vector<uint32_t> rootsWhoseElementsUnderIndexesAreAskedInLoops;
    Vector<uint32_t> rootsWhoseElementsMeetTextsInLoops;
    Vector<uint32_t> rootsReckonedWithAtWrittenPlacesInLoops;
    Vector<uint64_t> rowsReckonedWithAtWrittenPlacesInLoops;
    std::optional<uint64_t> holderAndKeyOfRow(const BodyIndex& index, uint32_t root) const
    {
        if (index.isParameter(ValueID(root)) || index.opcodeOf(ValueID(root)) != Opcode::ElemGet)
            return std::nullopt;
        BodyValue read = index.value(ValueID(root));
        return static_cast<uint64_t>(rootOf(read.operand(0).index())) << 32 | rootOf(read.propertyOperand().index());
    }
    Vector<uint32_t> rootsFromInFrontThatLoopsReckonWith;
    Vector<uint32_t> rootsMadeInTurnsThatLoopsReckonWith;
    Vector<uint32_t> rootsThatOperatorsOutsideLoopsReckonWith;
    Vector<uint32_t> rootsFromInFrontThatLoopsMakeWholeNumbersOf;
    Vector<uint64_t> layoutsPutIntoListsMadeHere;
    Vector<uint64_t> literalsMadeHere;
    Vector<uint32_t> loopsThatReckonWithWhatTheyRead;
    Vector<uint64_t> placesOfReads;
    Vector<uint32_t> readsOfTexts;
    Vector<uint32_t> placesWhoseLengthIsRead;
    Vector<uint32_t> placesWhoseElementsAreAskedInLoops;
    Vector<uint32_t> placesThatAreNoLists;
    Vector<uint64_t> readsUnderIndexesOfPlacesInLoops;
    Vector<uint64_t> blocksThatBranchOnTruth;
    Vector<std::pair<BlockID, ValueID>> blocksBehindComparisonsOfNewTarget;
    Vector<uint32_t> placesOfNearestBlocksAboveThatAskTheSame;
    Vector<std::pair<uint32_t, uint64_t>> storesOfOutermostLoops;
    Vector<std::pair<uint32_t, uint8_t>> numbersStoredUnderNames;
    Lock lockOfLists;
    std::atomic<bool> hasNumbersStoredUnderNames { false };
    std::atomic<bool> hasIncomingEdges { false };
    std::atomic<bool> hasStoresOfOutermostLoops { false };
    std::atomic<bool> hasBlocksThatBranchOnTruth { false };
    std::atomic<bool> hasBlocksBehindComparisonsOfNewTarget { false };
    std::atomic<bool> hasNamesUsedOffValues { false };
    std::atomic<bool> hasEntryParametersOnlyReadByNames { false };
    uint64_t entryParametersOnlyReadByTwoNamesOrMore { 0 };
    std::atomic<bool> isAskedForReadersOfCharacters { false };
    std::atomic<bool> hasRootsHandedFunctionsWrittenInPlace { false };
    bool callsReaderOfCharacters { false };
    bool callsReaderOfCharactersInLoop { false };
};

} // anonymous namespace

static std::span<const uint64_t> readsUnderIndexesOfPlaceInLoopsForGuesses(const KeptWithBodyForGuesses& kept, uint32_t place)
{
    auto reads = std::ranges::equal_range(kept.readsUnderIndexesOfPlacesInLoops, place, { }, [](uint64_t read) { return static_cast<uint32_t>(read >> 32); });
    return { reads.begin(), reads.end() };
}

static void listIncomingEdgesForGuesses(const BodyIndex& index, KeptWithBodyForGuesses& kept)
{
    uint32_t numberOfBlocks = index.numberOfBlocks();
    auto forEachEdge = [&](auto&& functor) {
        for (uint32_t from = 0; from < numberOfBlocks; ++from) {
            if (!index.isReachable(BlockID(from)))
                continue;
            uint32_t targetIndex = 0;
            for (BodyTarget target : index.terminator(BlockID(from)).targets())
                functor(BodyIncomingEdge { BlockID(from), targetIndex++, target });
        }
    };
    kept.firstIncomingEdges.fill(0, numberOfBlocks + 1);
    forEachEdge([&](const BodyIncomingEdge& edge) {
        ++kept.firstIncomingEdges[edge.target.block().index() + 1];
    });
    for (uint32_t next = 1; next <= numberOfBlocks; ++next)
        kept.firstIncomingEdges[next] += kept.firstIncomingEdges[next - 1];
    kept.incomingEdges.resize(kept.firstIncomingEdges.last());
    forEachEdge([&](const BodyIncomingEdge& edge) {
        kept.incomingEdges[kept.firstIncomingEdges[edge.target.block().index()]++] = edge;
    });
    for (uint32_t next = numberOfBlocks; next; --next)
        kept.firstIncomingEdges[next] = kept.firstIncomingEdges[next - 1];
    kept.firstIncomingEdges[0] = 0;
}

static bool showsValueUnderAnotherViewForGuesses(const Program& program, const BodyValue& operation)
{
    switch (operation.opcode()) {
    case Opcode::NarrowByField:
    case Opcode::NarrowByTest:
        return true;
    case Opcode::Convert: {
        TypeID written = operation.as<Opcode::Convert>().declared().written();
        return written && !holdsOnlyPrimitives(written) && program.types.at(written).unnamedLayouts;
    }
    default:
        return false;
    }
}

static bool namesReaderOfCharactersForGuesses(const Program& program, PropertyKeyID key)
{
    return key == program.keyOf(BuiltinNames::charCodeAt) || key == program.keyOf(BuiltinNames::charAt) || key == program.keyOf(BuiltinNames::codePointAt);
}

static void groupRootsThatHandNamesOnForGuesses(KeptWithBodyForGuesses& kept, uint32_t numberOfValues)
{
    constexpr uint32_t none = UINT32_MAX;
    struct Step {
        uint32_t root;
        uint32_t nextJoin;
        uint32_t endOfJoins;
    };
    const Vector<uint64_t>& joins = kept.joinsThatHandNamesBack;
    Vector<uint32_t>& groups = kept.groupsOfRootsThatHandNamesOn;
    Vector<uint32_t>& firstMembers = kept.firstMembersOfGroupsThatHandNamesOn;
    Vector<uint32_t>& members = kept.membersOfGroupsThatHandNamesOn;
    groups.fill(none, numberOfValues);
    Vector<uint32_t> placeWhereFound;
    placeWhereFound.fill(none, numberOfValues);
    Vector<uint32_t> earliestInReach;
    earliestInReach.fill(0, numberOfValues);
    Vector<uint32_t> withoutGroup;
    Vector<Step> steps;
    uint32_t numberFound = 0;
    auto enter = [&](uint32_t root) {
        placeWhereFound[root] = earliestInReach[root] = numberFound++;
        withoutGroup.append(root);
        auto handingBack = kept.joinsThatHandNamesBackTo(root);
        steps.append({ root, static_cast<uint32_t>(handingBack.begin() - joins.begin()), static_cast<uint32_t>(handingBack.end() - joins.begin()) });
    };
    for (uint64_t first : joins) {
        if (placeWhereFound[first >> 32] != none)
            continue;
        enter(static_cast<uint32_t>(first >> 32));
        while (!steps.isEmpty()) {
            if (Step& step = steps.last(); step.nextJoin < step.endOfJoins) {
                uint32_t join = static_cast<uint32_t>(joins[step.nextJoin++]);
                if (placeWhereFound[join] == none)
                    enter(join);
                else if (groups[join] == none)
                    earliestInReach[step.root] = std::min(earliestInReach[step.root], placeWhereFound[join]);
                continue;
            }
            uint32_t root = steps.takeLast().root;
            if (!steps.isEmpty())
                earliestInReach[steps.last().root] = std::min(earliestInReach[steps.last().root], earliestInReach[root]);
            if (earliestInReach[root] != placeWhereFound[root])
                continue;
            uint32_t group = firstMembers.size();
            firstMembers.append(members.size());
            for (uint32_t member = none; member != root;) {
                member = withoutGroup.takeLast();
                groups[member] = group;
                members.append(member);
            }
        }
    }
    uint32_t numberOfGroups = firstMembers.size();
    firstMembers.append(members.size());
    Vector<uint32_t> standsFor;
    standsFor.grow(numberOfGroups);
    for (uint32_t group = 0; group < numberOfGroups; ++group) {
        uint32_t onlyGroupBehind = none;
        bool standsForItself = false;
        for (uint32_t member : kept.membersOfGroupThatHandsNamesOn(group)) {
            standsForItself |= !kept.ownNamesOf(member).empty();
            for (uint64_t join : kept.joinsThatHandNamesBackTo(member)) {
                if (groups[static_cast<uint32_t>(join)] == group)
                    continue;
                uint32_t behind = standsFor[groups[static_cast<uint32_t>(join)]];
                standsForItself |= onlyGroupBehind != none && onlyGroupBehind != behind;
                onlyGroupBehind = behind;
            }
        }
        standsFor[group] = standsForItself || onlyGroupBehind == none ? group : onlyGroupBehind;
    }
    for (uint32_t member : members)
        groups[member] = standsFor[groups[member]];
}

static void listNamesUsedOffValuesForGuesses(Walker& walker, const BodyIndex& index, KeptWithBodyForGuesses& kept)
{
    static constexpr OpcodeSet operationsThatAskForNamesOrCast { Opcode::PropGet, Opcode::PropSet, Opcode::Call, Opcode::Convert };
    if (!index.holdsAnyOf(operationsThatAskForNamesOrCast | operatorsThatMakeNumbers | allBitwiseOperators))
        return;
    uint32_t numberOfValues = index.numberOfValues();
    Vector<uint32_t> roots;
    roots.grow(numberOfValues);
    std::iota(roots.begin(), roots.end(), 0u);
    using ListInTheMaking = Vector<uint64_t, 32>;
    ListInTheMaking uses;
    ListInTheMaking arrivals;
    ListInTheMaking casts;
    ListInTheMaking views;
    ListInTheMaking usesOfPlaces;
    const std::array<PropertyKeyID, 3> readersOfCharacters { walker.program().keyOf(BuiltinNames::charCodeAt), walker.program().keyOf(BuiltinNames::charAt), walker.program().keyOf(BuiltinNames::codePointAt) };
    auto namesReaderOfCharactersByKey = [&](PropertyKeyID key) {
        return key == readersOfCharacters[0] || key == readersOfCharacters[1] || key == readersOfCharacters[2];
    };
    static constexpr OpcodeSet operationsThatSpeakWhereNoLoopStands = inputOperationsThatHandOn | operatorsThatMakeNumbers | allBitwiseOperators | OpcodeSet { Opcode::PropGet, Opcode::PropSet, Opcode::Call, Opcode::Convert, Opcode::Add };
    static constexpr OpcodeSet readsOfPlaces { Opcode::PropGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::PrivateGet };
    const bool hasLoops = index.hasLoops();
    bool asksElementOfReadInLoop = false;
    static constexpr OpcodeSet operatorsThatTakeNumbers = operatorsThatMakeNumbers | allBitwiseOperators;
    Vector<uint64_t, 8> readsAtWrittenPlacesInLoops;
    Vector<uint32_t, 8> readsInLoops;
    Vector<uint32_t, 8> resultsFollowed;
    BitVector resultsWhoseEndIsKnown;
    BitVector resultsThatEndAsWholeNumbers;
    bool namesReaderOfCharacters = false;
    auto readInLoopBehind = [&](ValueID value) -> std::optional<uint32_t> {
        ValueID root = value ? ValueID(roots[value.index()]) : ValueID();
        if (!root || index.isParameter(root) || index.opcodeOf(root) != Opcode::ElemGet || !index.loopOf(index.blockOf(root)))
            return std::nullopt;
        return roots[index.value(root).operand(0).index()];
    };
    auto ask = [&](ValueID object) ALWAYS_INLINE_LAMBDA {
        if (!hasLoops)
            return;
        if (std::optional<uint32_t> holder = readInLoopBehind(object)) {
            kept.rootsWhoseElementsAreAskedInLoops.append(*holder);
            if (keyOfReadIsIndexByItsTextForGuesses(walker, index, index.value(ValueID(roots[object.index()]))))
                kept.rootsWhoseElementsUnderIndexesAreAskedInLoops.append(*holder);
            asksElementOfReadInLoop |= !index.isParameter(ValueID(*holder)) && readsOfPlaces.contains(index.opcodeOf(ValueID(*holder)));
        }
    };
    static constexpr OpcodeSet makersOfLists { Opcode::NewArray, Opcode::Data };
    auto putIntoList = [&](ValueID list, ValueID element) {
        ValueID made = list ? ValueID(roots[list.index()]) : ValueID();
        if (!made || index.isParameter(made) || !makersOfLists.contains(index.opcodeOf(made)))
            return;
        ValueID object = element ? ValueID(roots[element.index()]) : ValueID();
        LayoutID layout;
        if (object && !index.isParameter(object) && index.opcodeOf(object) == Opcode::NewObject)
            layout = index.value(object).layout();
        else if (const auto* record = object && !index.isParameter(object) && index.opcodeOf(object) == Opcode::Data ? walker.program().at(index.value(object).as<Opcode::Data>().data()).root.tryAs<DataNode::Object>() : nullptr)
            layout = record->layout;
        kept.layoutsPutIntoListsMadeHere.append(static_cast<uint64_t>(made.index()) << 32 | (layout ? layout.index() : UINT32_MAX));
    };
    auto noteRecordsOfConstant = [&](ValueID constant) {
        const DataNode& root = walker.program().at(index.value(constant).as<Opcode::Data>().data()).root;
        auto note = [&](const DataNode::Object& record) {
            if (record.layout)
                kept.literalsMadeHere.append(static_cast<uint64_t>(record.layout.index()) << 32 | constant.index());
        };
        if (const auto* record = root.tryAs<DataNode::Object>())
            note(*record);
        const auto* list = root.tryAs<DataNode::Array>();
        for (const DataNode& element : list ? list->elements : decltype(list->elements)()) {
            const auto* record = element.tryAs<DataNode::Object>();
            kept.layoutsPutIntoListsMadeHere.append(static_cast<uint64_t>(constant.index()) << 32 | (record && record->layout ? record->layout.index() : UINT32_MAX));
            if (record)
                note(*record);
        }
    };
    auto use = [&](ValueID object, PropertyKeyID name) {
        if (object && name) {
            uses.append(static_cast<uint64_t>(roots[object.index()]) << 32 | name.index());
            namesReaderOfCharacters |= hasLoops && namesReaderOfCharactersByKey(name);
        }
        ask(object);
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        if (std::optional<BodyIncomingEdge> edge = number && block.numberOfParameters() ? index.onlyIncomingEdge(BlockID(number)) : std::nullopt) {
            for (uint32_t proofIndex = static_cast<uint32_t>(edge->target.proofs().size()); proofIndex--;) {
                std::optional<BodyEdgeResult> result = index.edgeResult(edge->from, edge->targetIndex, edge->target, proofIndex);
                if (!result || !result->parameter)
                    continue;
                roots[result->parameter.index()] = result->subject && result->subject < result->parameter ? roots[result->subject.index()] : result->parameter.index();
            }
        }
        uint32_t firstValue = block.firstValue().index();
        for (uint32_t place = 0; place < block.numberOfValues(); ++place) {
            Opcode opcode = index.opcodeOf(ValueID(firstValue + place));
            if (!hasLoops && !operationsThatSpeakWhereNoLoopStands.contains(opcode))
                continue;
            BodyValue operation = index.value(ValueID(firstValue + place));
            switch (opcode) {
            case Opcode::PropGet:
                if (auto read = operation.as<Opcode::PropGet>(); !read.isFreeName()) {
                    use(read.objectOperand(), read.key());
                    if (hasLoops && index.loopOf(BlockID(number)))
                        readsInLoops.append(operation.id().index());
                    if (namesReaderOfCharactersByKey(read.key())) {
                        ValueID user = index.onlyUsingValue(operation.id());
                        if (!user || index.opcodeOf(user) != Opcode::Call || index.value(user).calleeOperand() != operation.id())
                            kept.readersOfCharactersNotOnlyCalled.append(read.key().index());
                    }
                }
                break;
            case Opcode::PropSet:
                if (auto store = operation.as<Opcode::PropSet>(); !store.isFreeName()) {
                    use(store.objectOperand(), store.key());
                    if (namesReaderOfCharactersByKey(store.key()))
                        kept.readersOfCharactersNotOnlyCalled.append(store.key().index());
                }
                break;
            case Opcode::Call:
                use(operation.as<Opcode::Call>().thisValueOperand(), operation.as<Opcode::Call>().calleeKey());
                ask(operation.as<Opcode::Call>().calleeOperand());
                if (ValueID receiver = operation.as<Opcode::Call>().thisValueOperand(); receiver && hasLoops && index.loopOf(BlockID(number)) && namesReaderOfCharactersByKey(operation.as<Opcode::Call>().calleeKey()) && std::ranges::binary_search(readsInLoops, roots[receiver.index()]))
                    readsInLoops.append(operation.id().index());
                if (hasLoops && operation.as<Opcode::Call>().calleeKey() == walker.program().keyOf(BuiltinNames::push)) {
                    for (ValueID pushed : operation.argumentsOperands())
                        putIntoList(operation.as<Opcode::Call>().thisValueOperand(), pushed);
                }
                break;
            case Opcode::ElemSet:
                if (hasLoops)
                    putIntoList(operation.operand(0), operation.as<Opcode::ElemSet>().valueOperand());
                break;
            case Opcode::NewObject:
                if (hasLoops)
                    kept.literalsMadeHere.append(static_cast<uint64_t>(operation.layout().index()) << 32 | operation.id().index());
                break;
            case Opcode::Data:
                if (hasLoops)
                    noteRecordsOfConstant(operation.id());
                break;
            case Opcode::NewArray:
                if (hasLoops) {
                    operation.forEachOperand([&](ValueID element, uint32_t) {
                        putIntoList(operation.id(), element);
                    });
                }
                break;
            case Opcode::ElemGet:
                ask(operation.operand(0));
                if (hasLoops && index.loopOf(BlockID(number)))
                    readsInLoops.append(operation.id().index());
                if (ValueID holder(roots[operation.operand(0).index()]); hasLoops && index.loopOf(BlockID(number)) && keyOfReadIsIndexByItsTextForGuesses(walker, index, operation))
                    asksElementOfReadInLoop |= !index.isParameter(holder) && readsOfPlaces.contains(index.opcodeOf(holder));
                if (ValueID key = operation.propertyOperand(); hasLoops && key && (index.isParameter(key) ? isEntryParameterProvenToBeWholeNumberForGuesses(walker, index, key) : index.opcodeOf(key) == Opcode::Integer) && index.loopOf(BlockID(number)))
                    readsAtWrittenPlacesInLoops.append(static_cast<uint64_t>(operation.id().index()) << 32 | roots[operation.operand(0).index()]);
                break;
            case Opcode::StrictEq:
            case Opcode::Eq:
                for (uint32_t side = 0; side < 2; ++side) {
                    ValueID other = operation.operand(1 - side);
                    std::optional<uint32_t> holder = !index.isParameter(other) && index.opcodeOf(other) == Opcode::String ? readInLoopBehind(operation.operand(side)) : std::nullopt;
                    if (holder)
                        kept.rootsWhoseElementsMeetTextsInLoops.append(*holder);
                }
                break;
            default: {
                if (LoopID loopOfOperator = hasLoops && operatorsThatTakeNumbers.contains(opcode) ? index.loopOf(BlockID(number)) : LoopID()) {
                    bool endsAsWholeNumber = allBitwiseOperators.contains(opcode);
                    resultsFollowed.shrink(0);
                    for (ValueID made = operation.id(); !endsAsWholeNumber && made;) {
                        if (resultsWhoseEndIsKnown.get(made.index())) {
                            endsAsWholeNumber = resultsThatEndAsWholeNumbers.get(made.index());
                            break;
                        }
                        resultsFollowed.append(made.index());
                        ValueID taker = index.onlyUsingValue(made);
                        if (!taker)
                            break;
                        Opcode opcodeOfTaker = index.opcodeOf(taker);
                        endsAsWholeNumber = allBitwiseOperators.contains(opcodeOfTaker) || ((opcodeOfTaker == Opcode::ElemGet || opcodeOfTaker == Opcode::ElemSet) && index.value(taker).propertyOperand() == made);
                        made = operatorsThatMakeNumbers.contains(opcodeOfTaker) || opcodeOfTaker == Opcode::Add ? taker : ValueID();
                    }
                    for (uint32_t followed : resultsFollowed) {
                        resultsWhoseEndIsKnown.set(followed);
                        if (endsAsWholeNumber)
                            resultsThatEndAsWholeNumbers.set(followed);
                    }
                    operation.forEachOperand([&](ValueID operand, uint32_t) {
                        if (!operand)
                            return;
                        bool isMadeInTurn = index.loopOf(index.blockOf(ValueID(roots[operand.index()]))) == loopOfOperator;
                        if (isMadeInTurn && operatorKindOf(opcode) == OperatorKind::NumberConversion)
                            return;
                        (endsAsWholeNumber ? kept.rootsFromInFrontThatLoopsMakeWholeNumbersOf : isMadeInTurn ? kept.rootsMadeInTurnsThatLoopsReckonWith : kept.rootsFromInFrontThatLoopsReckonWith).append(roots[operand.index()]);
                    });
                }
                bool standsInLoop = hasLoops && index.loopOf(BlockID(number));
                if (!standsInLoop && operatorsThatTakeNumbers.contains(opcode) && operatorKindOf(opcode) != OperatorKind::NumberConversion && opcode != Opcode::Mod && opcode != Opcode::Pow) {
                    operation.forEachOperand([&](ValueID operand, uint32_t) {
                        if (operand)
                            kept.rootsThatOperatorsOutsideLoopsReckonWith.append(roots[operand.index()]);
                    });
                }
                if (opcode == Opcode::Add) {
                    for (uint32_t side = 0; side < 2; ++side) {
                        ValueID other = operation.operand(1 - side);
                        if (!index.isParameter(other) && operatorsThatTakeNumbers.contains(index.opcodeOf(other)))
                            (standsInLoop ? kept.rootsFromInFrontThatLoopsReckonWith : kept.rootsThatOperatorsOutsideLoopsReckonWith).append(roots[operation.operand(side).index()]);
                    }
                }
                if (opcode == Opcode::ResultAt && hasLoops && index.loopOf(BlockID(number)) && operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value) && !index.isParameter(operation.operand(0)) && index.opcodeOf(operation.operand(0)) == Opcode::IteratorResume)
                    readsInLoops.append(operation.id().index());
                if (!readsInLoops.isEmpty() && (operatorsThatTakeNumbers.contains(opcode) || opcode == Opcode::Add)) {
                    operation.forEachOperand([&](ValueID operand, uint32_t) {
                        if (operand && std::ranges::binary_search(readsInLoops, roots[operand.index()]))
                            kept.loopsThatReckonWithWhatTheyRead.append(index.loopOf(index.blockOf(ValueID(roots[operand.index()]))).index());
                    });
                }
                auto isSumTakenForNumber = [&] {
                    ValueID user = opcode == Opcode::Add ? index.onlyUsingValue(operation.id()) : ValueID();
                    if (!user || index.isParameter(user) || index.opcodeOf(user) != Opcode::Convert)
                        return false;
                    TypeID written = index.value(user).as<Opcode::Convert>().declared().written();
                    return written && walker.types().at(written).isNumber();
                };
                if (!readsAtWrittenPlacesInLoops.isEmpty() && (operatorsThatTakeNumbers.contains(opcode) || isSumTakenForNumber())) {
                    operation.forEachOperand([&](ValueID operand, uint32_t) {
                        if (!operand)
                            return;
                        auto read = std::ranges::lower_bound(readsAtWrittenPlacesInLoops, static_cast<uint64_t>(roots[operand.index()]) << 32);
                        if (read != readsAtWrittenPlacesInLoops.end() && *read >> 32 == roots[operand.index()])
                            kept.rootsReckonedWithAtWrittenPlacesInLoops.append(static_cast<uint32_t>(*read));
                    });
                }
                bool handsOn = inputOperationsThatHandOn.contains(opcode);
                ValueID handedOn = handsOn || opcode == Opcode::Convert ? operation.operand(0) : ValueID();
                if (!handedOn || handedOn >= operation.id())
                    break;
                if (opcode == Opcode::Convert)
                    casts.append(static_cast<uint64_t>(roots[handedOn.index()]) << 32 | operation.id().index());
                if (handsOn && !showsValueUnderAnotherViewForGuesses(walker.program(), operation))
                    roots[operation.id().index()] = roots[handedOn.index()];
                else if (handsOn)
                    views.append(static_cast<uint64_t>(roots[handedOn.index()]) << 32 | operation.id().index());
                break;
            }
            }
        }
        for (BodyTarget target : block.terminator().targets()) {
            std::span<const ValueID> arguments = target.arguments();
            if (arguments.empty())
                continue;
            BodyBlock taking = index.block(target.block());
            for (uint32_t position = 0; position < arguments.size() && position < taking.numberOfParameters(); ++position) {
                if (arguments[position])
                    arrivals.append(static_cast<uint64_t>(roots[arguments[position].index()]) << 32 | taking.parameter(position).index());
            }
        }
    }
    auto sortWithoutRepeats = [](auto& list) {
        std::ranges::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
    };
    std::ranges::sort(kept.loopsThatReckonWithWhatTheyRead);
    bool someRootIsReckonedWith = !kept.rootsFromInFrontThatLoopsReckonWith.isEmpty() || !kept.rootsMadeInTurnsThatLoopsReckonWith.isEmpty() || !kept.rootsFromInFrontThatLoopsMakeWholeNumbersOf.isEmpty() || !kept.rootsReckonedWithAtWrittenPlacesInLoops.isEmpty() || !kept.rootsThatOperatorsOutsideLoopsReckonWith.isEmpty();
    if (uses.isEmpty() && casts.isEmpty() && !someRootIsReckonedWith)
        return;
    if (hasLoops && (asksElementOfReadInLoop || (namesReaderOfCharacters && !walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked)))) {
        Vector<uint32_t> places(numberOfValues, [](size_t number) { return static_cast<uint32_t>(number); });
        UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> firstReadsOfPlaces;
        auto placeOf = [&](ValueID value) {
            return places[roots[value.index()]];
        };
        auto noteReadOfPlace = [&](ValueID read, uint64_t key) {
            uint32_t first = firstReadsOfPlaces.add(key, read.index()).iterator->value;
            if (first == read.index())
                return;
            places[read.index()] = first;
            kept.placesOfReads.append(static_cast<uint64_t>(read.index()) << 32 | first);
            kept.placesOfReads.append(static_cast<uint64_t>(first) << 32 | first);
        };
        auto askPlace = [&](ValueID object) {
            if (std::optional<uint32_t> holder = readInLoopBehind(object))
                kept.placesWhoseElementsAreAskedInLoops.append(places[*holder]);
        };
        auto usePlace = [&](ValueID object, PropertyKeyID name) {
            if (object && name)
                usesOfPlaces.append(static_cast<uint64_t>(placeOf(object)) << 32 | name.index());
            askPlace(object);
        };
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            if (!index.isReachable(BlockID(number)))
                continue;
            BodyBlock block = index.block(BlockID(number));
            uint32_t firstValue = block.firstValue().index();
            for (uint32_t place = 0; place < block.numberOfValues(); ++place) {
                BodyValue operation = index.value(ValueID(firstValue + place));
                switch (operation.opcode()) {
                case Opcode::PropGet:
                    if (auto read = operation.as<Opcode::PropGet>(); !read.isFreeName()) {
                        usePlace(read.objectOperand(), read.key());
                        uint32_t placeOfObject = placeOf(read.objectOperand());
                        noteReadOfPlace(operation.id(), static_cast<uint64_t>(placeOfObject) << 32 | read.key().index());
                        if (walker.program().nameClassOf(read.key()) == NameClass::Length)
                            kept.placesWhoseLengthIsRead.append(placeOfObject);
                    }
                    break;
                case Opcode::PropSet:
                    if (auto store = operation.as<Opcode::PropSet>(); !store.isFreeName())
                        usePlace(store.objectOperand(), store.key());
                    break;
                case Opcode::GetModuleVariable:
                    noteReadOfPlace(operation.id(), uint64_t { 1 } << 63 | operation.variable().index());
                    break;
                case Opcode::GetEnvironmentField:
                    noteReadOfPlace(operation.id(), uint64_t { 1 } << 62 | static_cast<uint64_t>(operation.layout().index()) << 24 | operation.field());
                    break;
                case Opcode::PrivateGet:
                    noteReadOfPlace(operation.id(), uint64_t { 3 } << 62 | static_cast<uint64_t>(placeOf(operation.operand(0))) << 32 | (operation.layout().index() << 8 ^ operation.field()));
                    break;
                case Opcode::Call:
                    usePlace(operation.as<Opcode::Call>().thisValueOperand(), operation.as<Opcode::Call>().calleeKey());
                    askPlace(operation.as<Opcode::Call>().calleeOperand());
                    break;
                case Opcode::ElemGet:
                    askPlace(operation.operand(0));
                    if (index.loopOf(BlockID(number)) && keyOfReadIsIndexByItsTextForGuesses(walker, index, operation))
                        kept.readsUnderIndexesOfPlacesInLoops.append(static_cast<uint64_t>(placeOf(operation.operand(0))) << 32 | operation.id().index());
                    break;
                default:
                    break;
                }
            }
        }
        for (uint32_t holder : kept.rootsWhoseElementsMeetTextsInLoops) {
            kept.readsUnderIndexesOfPlacesInLoops.removeAllMatching([&](uint64_t read) {
                return read >> 32 == places[holder];
            });
        }
        std::ranges::sort(kept.readsUnderIndexesOfPlacesInLoops);
    }
    sortWithoutRepeats(uses);
    sortWithoutRepeats(kept.layoutsPutIntoListsMadeHere);
    sortWithoutRepeats(kept.literalsMadeHere);
    std::ranges::sort(kept.rootsWhoseElementsAreAskedInLoops);
    std::ranges::sort(kept.rootsWhoseElementsUnderIndexesAreAskedInLoops);
    std::ranges::sort(kept.rootsWhoseElementsMeetTextsInLoops);
    std::ranges::sort(kept.rootsReckonedWithAtWrittenPlacesInLoops);
    std::ranges::sort(kept.rootsFromInFrontThatLoopsReckonWith);
    std::ranges::sort(kept.rootsMadeInTurnsThatLoopsReckonWith);
    std::ranges::sort(kept.rootsThatOperatorsOutsideLoopsReckonWith);
    std::ranges::sort(kept.rootsFromInFrontThatLoopsMakeWholeNumbersOf);
    if (!kept.placesOfReads.isEmpty() && !walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked)) {
        Vector<uint32_t> placesOfReaders;
        Vector<uint32_t> placesOfOtherNames;
        for (uint64_t used : usesOfPlaces) {
            PropertyKeyID name(static_cast<uint32_t>(used));
            if (walker.program().nameClassOf(name) == NameClass::Length)
                continue;
            uint32_t place = static_cast<uint32_t>(used >> 32);
            BuiltinName builtin = liesInThread(name) ? BuiltinName() : walker.program().builtinNameOf(name);
            if (!builtin || !std::ranges::contains(walker.program().builtins().classesWithInstanceMember(builtin), BuiltinClassID(BuiltinClasses::String)))
                placesOfOtherNames.append(place);
            else if (namesReaderOfCharactersByKey(name) && !kept.readersOfCharactersNotOnlyCalled.contains(name.index()))
                placesOfReaders.append(place);
        }
        std::ranges::sort(placesOfReaders);
        std::ranges::sort(placesOfOtherNames);
        if (!placesOfReaders.isEmpty()) {
            for (uint64_t pair : kept.placesOfReads) {
                uint32_t place = static_cast<uint32_t>(pair);
                if (std::ranges::binary_search(placesOfReaders, place) && !std::ranges::binary_search(placesOfOtherNames, place))
                    kept.readsOfTexts.append(static_cast<uint32_t>(pair >> 32));
            }
            std::ranges::sort(kept.readsOfTexts);
        }
    }
    std::ranges::sort(kept.placesWhoseElementsAreAskedInLoops);
    if (kept.placesOfReads.isEmpty() || (kept.placesWhoseElementsAreAskedInLoops.isEmpty() && kept.readsUnderIndexesOfPlacesInLoops.isEmpty())) {
        kept.placesOfReads = { };
        kept.placesWhoseLengthIsRead = { };
        kept.placesWhoseElementsAreAskedInLoops = { };
    }
    for (uint64_t used : usesOfPlaces) {
        PropertyKeyID name(static_cast<uint32_t>(used));
        if ((!std::ranges::binary_search(kept.placesWhoseElementsAreAskedInLoops, static_cast<uint32_t>(used >> 32)) && readsUnderIndexesOfPlaceInLoopsForGuesses(kept, static_cast<uint32_t>(used >> 32)).empty()) || walker.program().nameClassOf(name) == NameClass::Length)
            continue;
        BuiltinName builtin = liesInThread(name) ? BuiltinName() : walker.program().builtinNameOf(name);
        if (!builtin || !std::ranges::contains(walker.program().builtins().classesWithInstanceMember(builtin), BuiltinClassID(BuiltinClasses::Array)))
            kept.placesThatAreNoLists.append(static_cast<uint32_t>(used >> 32));
    }
    std::ranges::sort(kept.placesOfReads);
    std::ranges::sort(kept.placesWhoseLengthIsRead);
    std::ranges::sort(kept.placesThatAreNoLists);
    auto rootThatTakes = [&](uint64_t arrival) {
        return static_cast<uint64_t>(roots[static_cast<uint32_t>(arrival)]) << 32;
    };
    bool someNameIsHandedBack = std::ranges::any_of(arrivals, [&](uint64_t arrival) {
        uint64_t taking = rootThatTakes(arrival);
        auto use = std::ranges::lower_bound(uses, taking);
        return taking >> 32 != arrival >> 32 && use != uses.end() && *use >> 32 == taking >> 32;
    });
    if (!casts.isEmpty()) {
        sortWithoutRepeats(arrivals);
        kept.joinsThatRootsArriveAt = Vector<uint64_t>(arrivals.span());
        ASSERT(std::ranges::all_of(kept.joinsThatRootsArriveAt, [&](uint64_t arrival) { return index.isParameter(ValueID(static_cast<uint32_t>(arrival))); }));
    }
    bool listsAreGatheredWhenAsked = false;
    if (someNameIsHandedBack) {
        for (uint64_t& arrival : arrivals)
            arrival = rootThatTakes(arrival) | arrival >> 32;
        sortWithoutRepeats(arrivals);
        ListInTheMaking written(uses);
        std::ranges::sort(written, { }, [](uint64_t use) {
            return use << 32 | use >> 32;
        });
        ListInTheMaking reached;
        Vector<uint32_t> nameThatReachedLast;
        nameThatReachedLast.fill(UINT32_MAX, numberOfValues);
        Vector<uint32_t> firstArrivalAt;
        firstArrivalAt.fill(0, numberOfValues + 1);
        for (uint64_t arrival : arrivals)
            ++firstArrivalAt[(arrival >> 32) + 1];
        for (uint32_t root = 0; root < numberOfValues; ++root)
            firstArrivalAt[root + 1] += firstArrivalAt[root];
        Vector<uint32_t> endOfNamesOf;
        endOfNamesOf.fill(0, numberOfValues + 1);
        size_t next = 0;
        for (size_t place = 0; place < written.size() && !listsAreGatheredWhenAsked;) {
            uint32_t name = static_cast<uint32_t>(written[place]);
            auto reach = [&](uint32_t root) {
                if (std::exchange(nameThatReachedLast[root], name) != name) {
                    reached.append(static_cast<uint64_t>(root) << 32 | name);
                    ++endOfNamesOf[root + 1];
                }
            };
            for (; place < written.size() && static_cast<uint32_t>(written[place]) == name; ++place)
                reach(static_cast<uint32_t>(written[place] >> 32));
            for (; next < reached.size(); ++next) {
                uint32_t taking = static_cast<uint32_t>(reached[next] >> 32);
                for (uint32_t arrival = firstArrivalAt[taking]; arrival < firstArrivalAt[taking + 1]; ++arrival)
                    reach(static_cast<uint32_t>(arrivals[arrival]));
            }
            listsAreGatheredWhenAsked = reached.size() > numberOfValues;
        }
        if (!listsAreGatheredWhenAsked) {
            for (uint32_t root = 0; root < numberOfValues; ++root)
                endOfNamesOf[root + 1] += endOfNamesOf[root];
            kept.rootsAskedForNames.grow(reached.size());
            kept.namesUsedOffValues.grow(reached.size());
            for (uint64_t used : reached) {
                uint32_t place = endOfNamesOf[used >> 32]++;
                kept.rootsAskedForNames[place] = static_cast<uint32_t>(used >> 32);
                kept.namesUsedOffValues[place] = PropertyKeyID(static_cast<uint32_t>(used));
            }
        }
    }
    if (!someNameIsHandedBack || listsAreGatheredWhenAsked) {
        kept.rootsAskedForNames.reserveInitialCapacity(uses.size());
        kept.namesUsedOffValues.reserveInitialCapacity(uses.size());
        for (uint64_t used : uses) {
            kept.rootsAskedForNames.append(static_cast<uint32_t>(used >> 32));
            kept.namesUsedOffValues.append(PropertyKeyID(static_cast<uint32_t>(used)));
        }
    }
    Vector<uint32_t> rootsThatNamesAreHandedBackTo;
    if (listsAreGatheredWhenAsked) {
        BitVector isReachedByNames(numberOfValues);
        Vector<uint32_t> reached;
        for (uint32_t root : kept.rootsAskedForNames) {
            if (!isReachedByNames.set(root))
                reached.append(root);
        }
        size_t numberOfRootsWithOwnNames = reached.size();
        for (size_t next = 0; next < reached.size(); ++next) {
            for (uint64_t arrival : std::ranges::equal_range(arrivals, static_cast<uint64_t>(reached[next]), { }, [](uint64_t listed) { return listed >> 32; })) {
                if (!isReachedByNames.set(static_cast<uint32_t>(arrival)))
                    reached.append(static_cast<uint32_t>(arrival));
            }
        }
        rootsThatNamesAreHandedBackTo.append(reached.span().subspan(numberOfRootsWithOwnNames));
        for (uint64_t arrival : arrivals) {
            if (arrival >> 32 != static_cast<uint32_t>(arrival) && isReachedByNames.get(arrival >> 32))
                kept.joinsThatHandNamesBack.append(arrival << 32 | arrival >> 32);
        }
        std::ranges::sort(kept.joinsThatHandNamesBack);
        groupRootsThatHandNamesOnForGuesses(kept, numberOfValues);
    }
    std::ranges::sort(casts);
    kept.castsOfRoots = Vector<uint64_t>(casts.span());
    std::ranges::sort(views);
    kept.viewsOfRoots = Vector<uint64_t>(views.span());
    kept.rootsOfValuesWithMarks = WTF::move(roots);
    for (uint32_t root : kept.rootsReckonedWithAtWrittenPlacesInLoops) {
        if (std::optional<uint64_t> row = kept.holderAndKeyOfRow(index, root))
            kept.rowsReckonedWithAtWrittenPlacesInLoops.append(*row);
    }
    std::ranges::sort(kept.rowsReckonedWithAtWrittenPlacesInLoops);
    auto mark = [&](uint32_t root) {
        kept.rootsOfValuesWithMarks[root] |= KeptWithBodyForGuesses::markOfRootThatTextSpeaksOf;
    };
    for (const Vector<uint32_t>* list : { &kept.rootsAskedForNames, &kept.readsOfTexts, &kept.rootsFromInFrontThatLoopsMakeWholeNumbersOf, &kept.rootsFromInFrontThatLoopsReckonWith, &kept.rootsMadeInTurnsThatLoopsReckonWith }) {
        for (uint32_t root : *list)
            mark(root);
    }
    for (uint32_t root : rootsThatNamesAreHandedBackTo)
        mark(root);
    for (uint64_t cast : kept.castsOfRoots)
        mark(static_cast<uint32_t>(cast >> 32));
    for (uint64_t arrival : kept.joinsThatRootsArriveAt) {
        auto castsOfJoin = std::ranges::equal_range(kept.castsOfRoots, static_cast<uint64_t>(kept.rootOf(static_cast<uint32_t>(arrival))), { }, [](uint64_t listed) { return listed >> 32; });
        if (!castsOfJoin.empty())
            mark(static_cast<uint32_t>(arrival >> 32));
    }
}

static KeptWithBodyForGuesses& keptWithBodyForGuesses(const BodyIndex& index)
{
    const KeptWithBodyIndex* kept = index.keptByWalk();
    if (!kept)
        kept = &index.keepForWalk(makeUniqueWithoutFastMallocCheck<KeptWithBodyForGuesses>());
    return const_cast<KeptWithBodyForGuesses&>(static_cast<const KeptWithBodyForGuesses&>(*kept));
}

template<typename Functor>
static void makeListKeptWithBodyForGuesses(KeptWithBodyForGuesses& kept, std::atomic<bool>& isMade, const Functor& make)
{
    if (isMade.load(std::memory_order_acquire)) [[likely]]
        return;
    Locker locker { kept.lockOfLists };
    if (isMade.load(std::memory_order_relaxed))
        return;
    make();
    isMade.store(true, std::memory_order_release);
}

TypeID numbersExpectedOfFieldOfHeir(Walker& walker, LayoutID heir, uint32_t field)
{
    enum class StoredUnderName : uint8_t { Int32, Number, Other };
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    const Field& record = program.at(heir).fields[field];
    TypeID written = record.declared.written();
    bool isWrittenAsNumber = written && !saysNothingForGuesses(types, written);
    if (isWrittenAsNumber && !types.at(written).isNumber())
        return { };
    FunctionID constructor = program.at(heir).constructor;
    const BodyIndex* bodyOfConstructor = constructor ? walker.facts().bodies().find(constructor) : nullptr;
    if (!record.key || !bodyOfConstructor)
        return isWrittenAsNumber ? TypeID(TypeTable::number) : TypeID();

    const BodyIndex& body = *bodyOfConstructor;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(body);
    makeListKeptWithBodyForGuesses(kept, kept.hasNumbersStoredUnderNames, [&] {
        for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
            if (!body.isReachable(BlockID(number)))
                continue;
            for (BodyValue store : body.block(BlockID(number)).values()) {
                if (!storesIntoSlotsForGuesses.contains(store.opcode()) || (store.opcode() == Opcode::PropSet && store.isFreeName()) || !store.key())
                    continue;
                StoredUnderName kind = StoredUnderName::Other;
                if (ValueID stored = body.valueBehindCasts(store.valueOperand()); stored && !body.isParameter(stored)) {
                    Opcode made = body.opcodeOf(stored);
                    if (made == Opcode::Integer) {
                        int64_t integer = body.value(stored).as<Opcode::Integer>().integer();
                        kind = integer >= INT32_MIN && integer <= INT32_MAX ? StoredUnderName::Int32 : StoredUnderName::Number;
                    } else if (made == Opcode::Double || operatorsThatMakeNumbers.contains(made))
                        kind = StoredUnderName::Number;
                }
                kept.numbersStoredUnderNames.append({ store.key().bits(), static_cast<uint8_t>(kind) });
            }
        }
        std::ranges::sort(kept.numbersStoredUnderNames);
    });
    auto stores = std::ranges::equal_range(kept.numbersStoredUnderNames, record.key.bits(), { }, &std::pair<uint32_t, uint8_t>::first);
    StoredUnderName widest = stores.empty() ? StoredUnderName::Other : static_cast<StoredUnderName>(stores.back().second);
    if (widest == StoredUnderName::Other)
        return isWrittenAsNumber ? TypeID(TypeTable::number) : TypeID();
    const LinkTables& linked = walker.facts().linked();
    bool isStoredFractionsElsewhere = linked.keysStoredWhatIsNoInt32.get(record.key.index()) || linked.fieldsStoredWhatIsNoInt32.names(program, heir, record.key);
    return widest == StoredUnderName::Int32 && !isStoredFractionsElsewhere ? TypeID(TypeTable::int32) : TypeID(TypeTable::number);
}

std::span<const BodyIncomingEdge> Walker::incomingEdgesOf(const BodyIndex& index, BlockID block)
{
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasIncomingEdges, [&] {
        listIncomingEdgesForGuesses(index, kept);
    });
    uint32_t first = kept.firstIncomingEdges[block.index()];
    return kept.incomingEdges.span().subspan(first, kept.firstIncomingEdges[block.index() + 1] - first);
}

Walker::RememberedExpectation& Walker::rememberedExpectationOf(ValueID value)
{
    Frame& frame = m_frames.last();
    if (frame.expectations.isEmpty()) [[unlikely]]
        frame.expectations.fill({ }, frame.index->numberOfValues());
    if (m_numberOfMarksOfTries) [[unlikely]]
        m_expectationsInTries.append(ExpectationInTry { static_cast<uint32_t>(m_frames.size() - 1), value, frame.expectations[value.index()] });
    return frame.expectations[value.index()];
}

Reference Walker::drawWhetherGuessFails(Origin origin)
{
    if (!m_inputs.options.failsGuesses || isUnreachable()) [[likely]]
        return { };
    origin.isTailCall = false;
    Reference fails = build(Draft(Opcode::GuessFailsUnasked, origin));
    if (fails)
        count(StatisticID::PlacesWhereGuessesFailUnasked);
    return fails;
}

void Walker::letGuessFailUnasked(BlockID& whereGuessFails, Origin origin)
{
    Reference fails = drawWhetherGuessFails(origin);
    if (!fails)
        return;
    BlockID whereNothingFails = addBlock();
    if (!whereGuessFails)
        whereGuessFails = addBlock(BlockFrequency::Rare);
    branch(fails, whereGuessFails, whereNothingFails);
    continueIn(whereNothingFails);
}

bool Walker::loopReckonsWithWhatItReads(BlockID inputHead)
{
    const BodyIndex& index = *m_frames.last().index;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(*this, index, kept);
    });
    return std::ranges::binary_search(kept.loopsThatReckonWithWhatTheyRead, index.loopOf(inputHead).index());
}

static NEVER_INLINE std::span<const PropertyKeyID> gatherNamesOfRootAndOfJoinsItArrivesAtForGuesses(Walker& walker, const KeptWithBodyForGuesses& kept, uint32_t root)
{
    auto& gathered = walker.namesGatheredForRoots();
    uint32_t asked = kept.groupsOfRootsThatHandNamesOn[root];
    if (auto found = gathered.find({ &kept, asked }); found != gathered.end())
        return found->value.span();
    Vector<uint32_t, 16> waiting { asked };
    Vector<uint32_t, 8> groupsBehind;
    while (!waiting.isEmpty()) {
        uint32_t group = waiting.last();
        if (gathered.contains({ &kept, group })) {
            waiting.removeLast();
            continue;
        }
        std::span<const uint32_t> members = kept.membersOfGroupThatHandsNamesOn(group);
        groupsBehind.shrink(0);
        for (uint32_t member : members) {
            for (uint64_t join : kept.joinsThatHandNamesBackTo(member)) {
                if (uint32_t behind = kept.groupsOfRootsThatHandNamesOn[static_cast<uint32_t>(join)]; behind != group)
                    groupsBehind.append(behind);
            }
        }
        std::ranges::sort(groupsBehind);
        groupsBehind.shrink(std::ranges::unique(groupsBehind).begin() - groupsBehind.begin());
        size_t numberThatWaited = waiting.size();
        for (uint32_t behind : groupsBehind) {
            if (!gathered.contains({ &kept, behind }))
                waiting.append(behind);
        }
        if (waiting.size() != numberThatWaited)
            continue;
        waiting.removeLast();
        Vector<PropertyKeyID> names;
        for (uint32_t member : members)
            names.append(kept.ownNamesOf(member));
        if (members.size() == 1 && groupsBehind.size() == 1) {
            std::span<const PropertyKeyID> handedBack = gathered.find({ &kept, groupsBehind[0] })->value.span();
            Vector<PropertyKeyID> both;
            both.grow(names.size() + handedBack.size());
            both.shrink(std::ranges::set_union(names, handedBack, both.begin(), { }, &PropertyKeyID::index, &PropertyKeyID::index).out - both.begin());
            names = WTF::move(both);
        } else {
            for (uint32_t behind : groupsBehind)
                names.append(gathered.find({ &kept, behind })->value.span());
            std::ranges::sort(names, { }, &PropertyKeyID::index);
            names.shrink(std::ranges::unique(names).begin() - names.begin());
        }
        gathered.add({ &kept, group }, WTF::move(names));
    }
    return gathered.find({ &kept, asked })->value.span();
}

static ALWAYS_INLINE std::span<const PropertyKeyID> namesOfRootAndOfJoinsItArrivesAtForGuesses(Walker& walker, const KeptWithBodyForGuesses& kept, uint32_t root)
{
    if (kept.joinsThatHandNamesBack.isEmpty() || kept.joinsThatHandNamesBackTo(root).empty()) [[likely]]
        return kept.ownNamesOf(root);
    return gatherNamesOfRootAndOfJoinsItArrivesAtForGuesses(walker, kept, root);
}

std::span<const PropertyKeyID> Walker::namesUsedOff(ValueID value)
{
    const BodyIndex& index = *m_frames.last().index;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(*this, index, kept);
    });
    if (kept.rootsOfValuesWithMarks.isEmpty())
        return { };
    return namesOfRootAndOfJoinsItArrivesAtForGuesses(*this, kept, kept.rootOf(value.index()));
}

bool Walker::appendNamesUsedOffViewsOf(ValueID value, Vector<PropertyKeyID, 16>& names)
{
    const BodyIndex& index = *m_frames.last().index;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(*this, index, kept);
    });
    if (kept.rootsOfValuesWithMarks.isEmpty() || kept.viewsOfRoots.isEmpty())
        return false;
    size_t sizeBefore = names.size();
    Vector<uint32_t, 8> roots { kept.rootOf(value.index()) };
    for (size_t next = 0; next < roots.size(); ++next) {
        auto views = std::ranges::equal_range(kept.viewsOfRoots, static_cast<uint64_t>(roots[next]), { }, [](uint64_t view) { return view >> 32; });
        for (uint64_t view : views) {
            uint32_t rootOfView = kept.rootOf(static_cast<uint32_t>(view));
            roots.append(rootOfView);
            names.append(namesOfRootAndOfJoinsItArrivesAtForGuesses(*this, kept, rootOfView));
        }
    }
    return names.size() != sizeBefore;
}

static bool textOfBodyIsSilentOnValueForGuesses(Walker& walker, const BodyIndex& index, ValueID value)
{
    if (index.hasLoops()) {
        std::optional<BodyUser> firstUser = index.firstUser(value);
        if (firstUser && firstUser->kind == BodyUseKind::Operand && index.opcodeOf(firstUser->value) == Opcode::ElemGet && index.value(firstUser->value).operand(0) == value)
            return false;
    }
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(walker, index, kept);
    });
    if (kept.rootsOfValuesWithMarks.isEmpty())
        return true;
    return !kept.textSpeaksOfRootOf(value.index()) && !(walker.mayBeginTurnAgainInClosedCopy() && std::ranges::binary_search(kept.rootsThatOperatorsOutsideLoopsReckonWith, kept.rootOf(value.index())));
}

std::span<const uint64_t> Walker::castsOf(ValueID value)
{
    const BodyIndex& index = *m_frames.last().index;
    if (!index.opcodes().contains(Opcode::Convert))
        return { };
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(*this, index, kept);
    });
    if (kept.rootsOfValuesWithMarks.isEmpty())
        return { };
    auto listedUnder = [](const Vector<uint64_t>& list, uint32_t root) {
        auto found = std::ranges::equal_range(list, static_cast<uint64_t>(root), { }, [](uint64_t listed) { return listed >> 32; });
        return list.span().subspan(found.begin() - list.begin(), found.size());
    };
    std::span<const uint64_t> own = listedUnder(kept.castsOfRoots, kept.rootOf(value.index()));
    if (!own.empty())
        return own;
    for (uint64_t arrival : listedUnder(kept.joinsThatRootsArriveAt, kept.rootOf(value.index()))) {
        if (std::span<const uint64_t> ofJoin = listedUnder(kept.castsOfRoots, kept.rootOf(static_cast<uint32_t>(arrival))); !ofJoin.empty())
            return ofJoin.first(1);
    }
    return { };
}

static constexpr uint32_t fewestLayoutsOfSetWhoseRoundsWalkerKeeps = 8;

bool Walker::keepsRoundsOver(const TypeArena& types, LayoutSetID set)
{
    return set && homeOf(set) == IDHome::Program && types.at(set).size() >= fewestLayoutsOfSetWhoseRoundsWalkerKeeps;
}

static uint64_t placeOfRoundKeptByWalker(Walker::RoundKeptByWalker asker, LayoutSetID set, uint32_t second)
{
    RELEASE_ASSERT(second < 1u << 29);
    return static_cast<uint64_t>(asker) << 61 | static_cast<uint64_t>(second) << 32 | set.bits();
}

std::optional<uint32_t> Walker::roundKept(RoundKeptByWalker asker, LayoutSetID set, uint32_t second) const
{
    auto found = m_roundsKept.find(placeOfRoundKeptByWalker(asker, set, second));
    return found == m_roundsKept.end() ? std::nullopt : std::optional<uint32_t>(found->value);
}

void Walker::keepRound(RoundKeptByWalker asker, LayoutSetID set, uint32_t second, uint32_t answer)
{
    m_roundsKept.set(placeOfRoundKeptByWalker(asker, set, second), answer);
}

uint32_t Walker::numberOfListOfNames(std::span<const PropertyKeyID> names)
{
    uint64_t hash = names.size();
    for (PropertyKeyID name : names)
        hash = (hash ^ name.bits()) * 0x9e3779b97f4a7c15ull;
    Vector<uint32_t, 1>& numbers = m_numbersOfListsOfNames.add(hash >> 1, Vector<uint32_t, 1>()).iterator->value;
    for (uint32_t number : numbers) {
        if (std::ranges::equal(m_listsOfNames[number].names, names))
            return number;
    }
    numbers.append(static_cast<uint32_t>(m_listsOfNames.size()));
    m_listsOfNames.append(ListOfNames { Vector<PropertyKeyID>(names), { }, { } });
    return numbers.last();
}

std::optional<uint32_t> Walker::numberOfLongListOfNamesUsedOff(ValueID value, std::span<const PropertyKeyID> names)
{
    static constexpr size_t namesGoneThroughAtEveryAsk = 16;
    if (names.size() <= namesGoneThroughAtEveryAsk) [[likely]]
        return std::nullopt;
    Frame& frame = m_frames.last();
    const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(*frame.index);
    auto added = frame.numbersOfLongListsOfNamesOfRoots.add(kept.rootOf(value.index()), 0);
    if (added.isNewEntry)
        added.iterator->value = numberOfListOfNames(names);
    return added.iterator->value;
}

bool Walker::everyNameOfLongListIsPlainFieldOf(uint32_t numberOfList, LayoutID layout, NOESCAPE const ScopedLambda<bool()>& ask)
{
    uint64_t asker = static_cast<uint64_t>(unit().function.index() << 4 | static_cast<uint32_t>(unit().role)) << 32 | layout.bits();
    if (auto kept = m_listsOfNames[numberOfList].areAllPlainFields.find(asker); kept != m_listsOfNames[numberOfList].areAllPlainFields.end())
        return kept->value;
    bool answer = ask();
    m_listsOfNames[numberOfList].areAllPlainFields.add(asker, answer);
    return answer;
}

LayoutID Walker::onlyLayoutInSightWithAllNames(std::span<const PropertyKeyID> names, bool recordsAnswer, LayoutID beside, std::optional<uint32_t> numberOfLongList)
{
    const LayoutsByNames& layouts = facts().layoutsByNames();
    ModuleID own = program().at(currentFunction()).module;
    if (!own)
        return { };
    bool looksBeyondOwnFile = isInLoop();
    uint32_t asker = own.index() * 4 + looksBeyondOwnFile * 2 + recordsAnswer;
    if (beside)
        numberOfLongList = std::nullopt;
    if (numberOfLongList) [[unlikely]] {
        for (auto [kept, answer] : m_listsOfNames[*numberOfLongList].answers) {
            if (kept == asker)
                return LayoutID::fromBits(answer);
        }
    }
    auto keepUnderNumberOfLongList = [&](LayoutID only) {
        if (numberOfLongList) [[unlikely]]
            m_listsOfNames[*numberOfLongList].answers.append({ asker, only.bits() });
        return only;
    };
    std::span<const uint32_t> holders = layouts.holdersOfRarestOf(names);
    if (holders.empty())
        return keepUnderNumberOfLongList({ });
    uint32_t ownAlone[] = { own.index() };

    auto givesWayTo = [&](LayoutID above, LayoutID below) {
        auto parentIsValue = [&](LayoutID layout) {
            return !program().at(layout).base && program().at(layout).has(LayoutFlags::HasHeritage);
        };
        bool passesParentThatIsValue = parentIsValue(above);
        size_t stepsLeft = program().layouts.size();
        for (LayoutID current = below; current && stepsLeft--;) {
            if (current == above)
                return passesParentThatIsValue && current != below;
            passesParentThatIsValue |= parentIsValue(current);
            LayoutID parent;
            if (!findParentNamedByClassRecordForTypedAccess(*this, current, parent))
                return false;
            current = parent;
        }
        return false;
    };
    auto findIn = [&](std::span<const uint32_t> modules, size_t written) -> std::optional<LayoutID> {
        LayoutID found;
        bool areSeveral = false;
        auto take = [&](LayoutID layout) {
            if (layout == beside)
                return;
            if (!found || givesWayTo(found, layout))
                found = layout;
            else if (found != layout && !givesWayTo(layout, found))
                areSeveral = true;
        };
        if (holders.size() <= written) {
            bool asksForWriterFirst = modules.size() == 1;
            for (uint32_t layout : holders) {
                bool answers = asksForWriterFirst ? layouts.isWrittenInOneOf(modules, layout) && layouts.hasAll(layout, names) : layouts.hasAll(layout, names) && layouts.isWrittenInOneOf(modules, layout);
                if (answers)
                    take(LayoutID(layout));
                if (areSeveral)
                    return LayoutID();
            }
        } else {
            for (uint32_t module : modules) {
                for (uint32_t layout : layouts.layoutsWrittenIn(ModuleID(module))) {
                    if (found != LayoutID(layout) && layouts.hasAll(layout, names))
                        take(LayoutID(layout));
                    if (areSeveral)
                        return LayoutID();
                }
            }
        }
        return found ? std::optional(found) : std::nullopt;
    };
    size_t written = looksBeyondOwnFile ? layouts.numberWrittenInSightOf(own) : layouts.layoutsWrittenIn(own).size();
    auto find = [&] {
        std::optional<LayoutID> found = findIn(ownAlone, layouts.layoutsWrittenIn(own).size());
        if (!found && looksBeyondOwnFile)
            found = findIn(layouts.modulesSeenBy(own), written);
        return found && *found && (recordsAnswer || layouts.isInstance(*found)) ? *found : LayoutID();
    };

    static constexpr size_t layoutsGoneThroughAtEveryAsk = 16;
    if (numberOfLongList || beside || std::min(holders.size(), written) <= layoutsGoneThroughAtEveryAsk)
        return keepUnderNumberOfLongList(find());
    ListOfNames& list = m_listsOfNames[numberOfListOfNames(names)];
    for (auto [kept, answer] : list.answers) {
        if (kept == asker)
            return LayoutID::fromBits(answer);
    }
    LayoutID only = find();
    list.answers.append({ asker, only.bits() });
    return only;
}

static ALWAYS_INLINE PropertyKeyID findNameUsedOffValueForGuesses(const Program& program, const BodyIndex& index, ValueID value)
{
    while (true) {
        std::optional<BodyUser> user = index.firstUser(value);
        if (!user || user->kind != BodyUseKind::Operand)
            return { };
        BodyValue operation = index.value(user->value);
        PropertyKeyID name;
        switch (Opcode opcode = operation.opcode()) {
        case Opcode::PropGet:
            if (auto read = operation.as<Opcode::PropGet>(); !read.isFreeName() && read.objectOperand() == value)
                name = read.key();
            break;
        case Opcode::PropSet:
            if (auto store = operation.as<Opcode::PropSet>(); !store.isFreeName() && store.objectOperand() == value)
                name = store.key();
            break;
        case Opcode::Call:
            if (operation.as<Opcode::Call>().thisValueOperand() == value)
                name = operation.as<Opcode::Call>().calleeKey();
            break;
        default:
            if (!inputOperationsThatHandOn.contains(opcode) || operation.operand(0) != value || value >= operation.id() || showsValueUnderAnotherViewForGuesses(program, operation))
                return { };
            value = operation.id();
            continue;
        }
        return name && index.isReachable(index.blockOf(operation.id())) ? name : PropertyKeyID();
    }
}

static TypeID layoutsThatStayWithCompiledCodeAnewForGuesses(Walker& walker, TypeID expected)
{
    TypeArena& types = walker.types();
    if (!types.at(expected).hasObjects())
        return { };
    Type objects = types.at(types.withOnlyObjects(expected));
    if (!objects.hasOnlyLayouts() || objects.hasOnlyClosures())
        return { };
    LayoutSetID staying = types.layoutSets().intersectionOf(objects.layouts, walker.facts().tables().layoutsThatStayWithCompiledCode);
    return staying ? types.forLayoutSet(staying) : TypeID();
}

static TypeID layoutsThatStayWithCompiledCodeForGuesses(Walker& walker, TypeID expected)
{
    constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::LayoutsThatStayWithCompiledCode);
    TypeArena& types = walker.types();
    if (types.keeps() != ArenaKeeps::UntilReset)
        return layoutsThatStayWithCompiledCodeAnewForGuesses(walker, expected);
    if (std::optional<uint32_t> known = types.rememberedAnswer(number, expected.bits(), 0))
        return TypeID::fromBits(*known);
    TypeID staying = layoutsThatStayWithCompiledCodeAnewForGuesses(walker, expected);
    types.rememberAnswer(number, expected.bits(), 0, staying.bits());
    return staying;
}

static TypeID layoutsPutIntoListMadeHereThatHandsOutForGuesses(Walker& walker, ValueID value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    while (value && (index.isParameter(value) || index.opcodeOf(value) == Opcode::Convert))
        value = index.isParameter(value) ? index.valueNarrowedInto(value) : index.value(value).operand(0);
    if (!value)
        return { };
    BodyValue operation = index.value(value);
    ValueID list = operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().thisValueOperand() : operation.opcode() == Opcode::ElemGet ? operation.objectOperand() : ValueID();
    if (!list)
        return { };
    walker.namesUsedOff(value);
    const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    if (kept.rootsOfValuesWithMarks.isEmpty())
        return { };
    TypeArena& types = walker.types();
    TypeID layouts;
    for (uint64_t listed : std::ranges::equal_range(kept.layoutsPutIntoListsMadeHere, static_cast<uint64_t>(kept.rootOf(list.index())), { }, [](uint64_t listed) { return listed >> 32; })) {
        if (static_cast<uint32_t>(listed) == UINT32_MAX)
            continue;
        TypeID layout = types.forLayout(LayoutID(static_cast<uint32_t>(listed)));
        layouts = layouts ? types.unionOf(layouts, layout) : layout;
    }
    return layouts;
}

static TypeID withoutLayoutsThatStayWithCompiledCodeForGuesses(Walker& walker, ValueID expecting, ValueID value, TypeID expected, NOESCAPE const Invocable<TypeID()> auto& seenToArrive)
{
    static constexpr UnknownSeeds seedsOfUnseenCode { UnknownSeed::UndescribedNativeResult, UnknownSeed::ContentsOfEscapedObject, UnknownSeed::UnseenCode, UnknownSeed::DataFromOutside };
    static constexpr UnknownSeeds seedsAdmittedBesideThem { UnknownSeed::UndescribedNativeResult, UnknownSeed::ContentsOfEscapedObject, UnknownSeed::UnseenCode, UnknownSeed::DataFromOutside, UnknownSeed::ResultOfUnknownCallee };
    if (!expected || holdsOnlyPrimitives(expected))
        return expected;
    TypeID staying = layoutsThatStayWithCompiledCodeForGuesses(walker, expected);
    if (!staying)
        return expected;
    TypeArena& types = walker.types();
    TypeID proven = provenTypeOfInputForGuesses(walker, value);
    if (!proven || !types.at(proven).hasUnboundedUnknown() || !types.at(proven).seeds.containsAny(seedsOfUnseenCode) || !types.at(proven).seeds.containsOnly(seedsAdmittedBesideThem))
        return expected;
    TypeID shapedLike = staying;
    if (TypeID arrives = seenToArrive(); arrives && types.overlap(arrives, staying))
        shapedLike = types.differenceOf(staying, arrives);
    if (TypeID put = shapedLike ? layoutsPutIntoListMadeHereThatHandsOutForGuesses(walker, value) : TypeID(); put && types.overlap(put, shapedLike))
        shapedLike = types.differenceOf(shapedLike, put);
    if (!shapedLike || !types.at(shapedLike).hasObjects())
        return expected;
    walker.count(StatisticID::LayoutsNotTestedForAtWhatComesOutOfUnseenCode);
    walker.rememberShapeOf(expecting, shapedLike);
    expected = types.differenceOf(expected, shapedLike);
    return types.at(expected).hasObjects() ? expected : TypeID();
}

static LayoutID layoutThatAnswersWrittenNamesBesideForGuesses(Walker& walker, ValueID input, LayoutID only)
{
    if (!only)
        return { };
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    const Layout& record = walker.program().at(only);
    Vector<PropertyKeyID, 8> written;
    for (PropertyKeyID name : names) {
        std::optional<uint32_t> field = record.findField(name);
        if (!field || !record.fields[*field].flags.contains(FieldFlags::IsNeverWritten))
            written.append(name);
    }
    if (written.isEmpty() || written.size() == names.size())
        return { };
    LayoutID other = walker.onlyLayoutInSightWithAllNames(written.span(), true, only);
    return other && walker.program().at(other).kind == record.kind ? other : LayoutID();
}

static ExpectationForGuesses expectationByNamesUsedForGuesses(Walker& walker, ValueID input, PropertyKeyID firstUsed)
{
    const LayoutsByNames& layouts = walker.facts().layoutsByNames();
    bool isInLoop = walker.isInLoop();
    if (firstUsed && !(isInLoop ? layouts.isNameOfSomeLayout(firstUsed) : layouts.isNameOfSomeInstance(firstUsed)))
        return { };
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    std::optional<uint32_t> numberOfLongList = walker.numberOfLongListOfNamesUsedOff(input, names);
    LayoutID only = walker.onlyLayoutInSightWithAllNames(names, isInLoop && names.size() > 1, { }, numberOfLongList);
    if (Vector<PropertyKeyID, 16> withViews; !only && !names.empty() && walker.appendNamesUsedOffViewsOf(input, withViews)) {
        withViews.append(names);
        only = walker.onlyLayoutInSightWithAllNames(withViews.span(), false);
    }
    auto everyNameIsPlainFieldOf = [&](LayoutID layout) {
        const Layout& record = walker.program().at(layout);
        const SlotPlan& slots = walker.plan().slots();
        return std::ranges::all_of(names, [&](PropertyKeyID name) {
            std::optional<uint32_t> field = record.findField(name);
            if (!field || record.fields[*field].flags.contains(FieldFlags::IsMethod))
                return false;
            std::optional<FieldIndex> placed = slots.findField(walker.program(), layout, name);
            if (!placed)
                return false;
            const PlacedSlot& slot = slots.slot(layout, *placed);
            TypeID held = slot.typeThatStoresAreHeldTo();
            return slot.representation != SlotRepresentation::DeferredClosure && !slot.deferredFunction && walker.withoutFunctionsThatMayNotBeMentioned(held) == held;
        });
    };
    auto askWhetherEveryNameIsPlainField = [&] {
        return everyNameIsPlainFieldOf(only);
    };
    if (only && walker.program().at(only).kind == LayoutKind::Record && !(numberOfLongList ? walker.everyNameOfLongListIsPlainFieldOf(*numberOfLongList, only, scopedLambda<bool()>(askWhetherEveryNameIsPlainField)) : everyNameIsPlainFieldOf(only)))
        return { };
    bool everyNameIsAnsweredByGetterBeingWalked = only && !names.empty() && std::ranges::all_of(names, [&](PropertyKeyID name) {
        MemberFound found = findMemberAboveObjectsOf(walker.program(), walker.facts().linked(), only, name);
        return found.kind == MemberFound::Kind::Accessor && found.getter == walker.currentFunction();
    });
    if (everyNameIsAnsweredByGetterBeingWalked)
        return { };
    if (only && !withoutLayoutsThatStayWithCompiledCodeForGuesses(walker, input, input, walker.types().forLayout(only), [] { return TypeID(); }))
        return { };
    return only ? ExpectationForGuesses { walker.types().forLayout(only), false, false, true } : ExpectationForGuesses();
}

static ExpectationForGuesses expectationByNameThatOneBuiltinClassAnswersForGuesses(Walker& walker, ValueID input)
{
    if (!walker.isInLoop())
        return { };
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const LayoutsByNames& layouts = walker.facts().layoutsByNames();
    auto classesThatAnswer = [&](PropertyKeyID key) {
        BuiltinName name = liesInThread(key) ? BuiltinName() : program.builtinNameOf(key);
        return name ? table.classesWithInstanceMember(name) : std::span<const BuiltinClassID>();
    };
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    BuiltinClassID only;
    for (PropertyKeyID key : names) {
        std::span<const BuiltinClassID> classes = classesThatAnswer(key);
        if (classes.size() != 1 || layouts.isNameOfSomeLayout(key))
            continue;
        if (only && only != classes[0])
            return { };
        only = classes[0];
    }
    if (!only || only == BuiltinClasses::String || only == BuiltinClasses::Object || only == BuiltinClasses::Function)
        return { };
    bool answersEveryName = std::ranges::all_of(names, [&](PropertyKeyID key) {
        return std::ranges::any_of(classesThatAnswer(key), [&](BuiltinClassID holder) {
            return table.isSameOrSubclassOf(only, holder);
        });
    });
    return answersEveryName ? ExpectationForGuesses { walker.types().builtinClass(only), false } : ExpectationForGuesses();
}

static bool isDeclaredFunctionNeverStoredAnew(const Program& program, const BodyIndex& index, ValueID read)
{
    if (index.opcodeOf(read) != Opcode::GetModuleVariable)
        return false;
    const Variable& variable = program.at(index.value(read).variable());
    return variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable);
}

static ExpectationForGuesses expectationOfListHandedFunctionWrittenInPlaceForGuesses(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.opcodes().contains(Opcode::Call) || !(index.opcodes().contains(Opcode::NewClosure) || index.opcodes().contains(Opcode::GetModuleVariable)))
        return { };
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const LayoutsByNames& layouts = walker.facts().layoutsByNames();
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    bool listsAloneAnswerEveryName = !names.empty() && std::ranges::all_of(names, [&](PropertyKeyID key) {
        BuiltinName name = liesInThread(key) ? BuiltinName() : program.builtinNameOf(key);
        return name && !layouts.isNameOfSomeLayout(key) && std::ranges::any_of(table.classesWithInstanceMember(name), [&](BuiltinClassID holder) {
            return table.isSameOrSubclassOf(BuiltinClasses::Array, holder);
        });
    });
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    if (!listsAloneAnswerEveryName || kept.rootsOfValuesWithMarks.isEmpty())
        return { };
    makeListKeptWithBodyForGuesses(kept, kept.hasRootsHandedFunctionsWrittenInPlace, [&] {
        for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
            ValueID value(number);
            if (index.isParameter(value) || index.opcodeOf(value) != Opcode::Call)
                continue;
            auto call = index.value(value).as<Opcode::Call>();
            ValueID handed = call.thisValueOperand() && !call.argumentsOperands().empty() ? call.argumentsOperands()[0] : ValueID();
            if (!handed || index.isParameter(handed))
                continue;
            bool isWrittenInPlace = index.opcodeOf(handed) == Opcode::NewClosure && index.onlyUsingValue(handed) == value;
            if (isWrittenInPlace || isDeclaredFunctionNeverStoredAnew(program, index, handed))
                kept.rootsHandedFunctionsWrittenInPlace.append(kept.rootOf(call.thisValueOperand().index()));
        }
        std::ranges::sort(kept.rootsHandedFunctionsWrittenInPlace);
    });
    if (!std::ranges::binary_search(kept.rootsHandedFunctionsWrittenInPlace, kept.rootOf(input.index())))
        return { };
    return { walker.types().builtinClass(BuiltinClasses::Array), false };
}

static bool turnsCallReaderOfCharactersForGuesses(const Walker& walker, const BodyIndex& index)
{
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.isAskedForReadersOfCharacters, [&] {
        if (!index.opcodes().contains(Opcode::Call))
            return;
        for (uint32_t number = 0; number < index.numberOfValues() && !kept.callsReaderOfCharactersInLoop; ++number) {
            ValueID value(number);
            if (index.isParameter(value) || index.opcodeOf(value) != Opcode::Call || !namesReaderOfCharactersForGuesses(walker.program(), index.value(value).as<Opcode::Call>().calleeKey()))
                continue;
            kept.callsReaderOfCharacters = true;
            kept.callsReaderOfCharactersInLoop = index.loopDepthOf(index.blockOf(value));
        }
    });
    return kept.callsReaderOfCharactersInLoop || (kept.callsReaderOfCharacters && walker.isInLoop());
}

bool isAskedForNameThatNoBuiltinAnswers(Walker& walker, ValueID value)
{
    const Program& program = walker.program();
    return std::ranges::any_of(walker.namesUsedOff(value), [&](PropertyKeyID key) {
        if (liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text)
            return false;
        BuiltinName name = program.builtinNameOf(key);
        return !name || program.builtins().classesWithInstanceMember(name).empty();
    });
}

static ExpectationForGuesses expectationByNamesOfTextsForGuesses(Walker& walker, ValueID input, PropertyKeyID firstUsed)
{
    const Program& program = walker.program();
    if (!firstUsed || !turnsCallReaderOfCharactersForGuesses(walker, walker.currentBodyIndex()))
        return { };
    auto classesThatAnswer = [&](PropertyKeyID key) {
        BuiltinName name = liesInThread(key) ? BuiltinName() : program.builtinNameOf(key);
        return name ? program.builtins().classesWithInstanceMember(name) : std::span<const BuiltinClassID>();
    };
    auto textsAnswer = [&](PropertyKeyID key) {
        return program.nameClassOf(key) == NameClass::Length || std::ranges::contains(classesThatAnswer(key), BuiltinClassID(BuiltinClasses::String));
    };
    if (!textsAnswer(firstUsed))
        return { };
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    if (!std::ranges::all_of(names, textsAnswer))
        return { };
    const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(walker.currentBodyIndex());
    const BuiltinTable& table = program.builtins();
    bool someCallReadsCharacter = std::ranges::any_of(names, [&](PropertyKeyID key) {
        if (classesThatAnswer(key).size() != 1 || kept.readersOfCharactersNotOnlyCalled.contains(key.index()))
            return false;
        BuiltinMemberLookup found = table.findMember(table.canonicalID(Builtins::StringPrototype), program.builtinNameOf(key));
        return found.outcome == BuiltinMemberLookup::Outcome::Found && std::ranges::any_of(table.overloadsOf(table.canonicalID(found.member)), [](const BuiltinOverload& overload) {
            return overload.code.implementation.inlineBuiltin && readsCharacterOfText(*overload.code.implementation.inlineBuiltin);
        });
    });
    return someCallReadsCharacter ? ExpectationForGuesses { TypeTable::string, false } : ExpectationForGuesses();
}

static ExpectationForGuesses expectationOfPlaceThatLoopsReadByKeyForGuesses(Walker& walker, ValueID input)
{
    const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(walker.currentBodyIndex());
    if (kept.rootsOfValuesWithMarks.isEmpty())
        return { };
    uint32_t root = kept.rootOf(input.index());
    auto read = std::ranges::lower_bound(kept.placesOfReads, static_cast<uint64_t>(root) << 32);
    if (read == kept.placesOfReads.end() || *read >> 32 != root)
        return { };
    uint32_t place = static_cast<uint32_t>(*read);
    if (!std::ranges::binary_search(kept.placesWhoseLengthIsRead, place) || std::ranges::binary_search(kept.placesThatAreNoLists, place))
        return { };
    bool elementsAreAsked = std::ranges::binary_search(kept.placesWhoseElementsAreAskedInLoops, place);
    if (!elementsAreAsked) {
        std::span<const uint64_t> reads = readsUnderIndexesOfPlaceInLoopsForGuesses(kept, place);
        bool someElementIsReckonedWith = std::ranges::any_of(reads, [&](uint64_t read) {
            return loopReckonsWithElementRead(walker, walker.currentBodyIndex().value(ValueID(static_cast<uint32_t>(read))));
        });
        if (reads.empty() || someElementIsReckonedWith)
            return { };
    }
    walker.count(StatisticID::ListsExpectedOfPlaces);
    return { walker.types().builtinClass(BuiltinClasses::Array), false, false, !elementsAreAsked };
}

static bool otherNamesAreReadOffPlaceOfForGuesses(const KeptWithBodyForGuesses& kept, ValueID input)
{
    if (kept.rootsOfValuesWithMarks.isEmpty())
        return false;
    uint32_t root = kept.rootOf(input.index());
    auto read = std::ranges::lower_bound(kept.placesOfReads, static_cast<uint64_t>(root) << 32);
    return read != kept.placesOfReads.end() && *read >> 32 == root && std::ranges::binary_search(kept.placesThatAreNoLists, static_cast<uint32_t>(*read));
}

static ExpectationForGuesses expectationOfWhatLoopReadsByKeyForGuesses(Walker& walker, ValueID input)
{
    if (!walker.currentBodyIndex().hasLoops())
        return { };
    const Program& program = walker.program();
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    if (const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(walker.currentBodyIndex()); names.empty() && !program.at(walker.currentFunction()).has(FunctionFlags::IsChecked) && !kept.rootsOfValuesWithMarks.isEmpty() && std::ranges::binary_search(kept.rootsWhoseElementsUnderIndexesAreAskedInLoops, kept.rootOf(input.index()))) {
        if (!otherNamesAreReadOffPlaceOfForGuesses(kept, input)) {
            walker.count(StatisticID::ListsExpectedOfWhatLoopsAskElementsUnderIndexesOf);
            return { walker.types().builtinClass(BuiltinClasses::Array), false };
        }
    }
    if (std::ranges::none_of(names, [&](PropertyKeyID name) { return program.nameClassOf(name) == NameClass::Length; }))
        return expectationOfPlaceThatLoopsReadByKeyForGuesses(walker, input);
    const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(walker.currentBodyIndex());
    if (kept.rootsOfValuesWithMarks.isEmpty())
        return { };
    uint32_t root = kept.rootOf(input.index());
    if (std::ranges::binary_search(kept.rootsWhoseElementsAreAskedInLoops, root))
        return { walker.types().builtinClass(BuiltinClasses::Array), false };
    if (!std::ranges::binary_search(kept.rootsWhoseElementsMeetTextsInLoops, root))
        return expectationOfPlaceThatLoopsReadByKeyForGuesses(walker, input);
    bool textsAnswerEveryName = std::ranges::all_of(names, [&](PropertyKeyID key) {
        if (program.nameClassOf(key) == NameClass::Length)
            return true;
        BuiltinName name = liesInThread(key) ? BuiltinName() : program.builtinNameOf(key);
        return name && std::ranges::contains(program.builtins().classesWithInstanceMember(name), BuiltinClassID(BuiltinClasses::String));
    });
    return textsAnswerEveryName ? ExpectationForGuesses { TypeTable::string, false } : ExpectationForGuesses();
}

static ALWAYS_INLINE TypeID narrowedByNamesUsedForGuesses(Walker& walker, ValueID input, TypeID expected)
{
    TypeArena& types = walker.types();
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || holdsOneMember(type.layouts))
        return expected;
    std::span<const LayoutID> layouts = types.listOf(type.layouts);
    if (layouts.size() < 2)
        return expected;
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    if (names.empty())
        return expected;
    auto narrowedTo = [&](LayoutID only) {
        return only ? types.forLayout(only) : expected;
    };
    bool isKept = Walker::keepsRoundsOver(types, type.layouts);
    uint32_t numberOfNames = isKept ? walker.numberOfListOfNames(names) : 0;
    if (isKept) {
        if (std::optional<uint32_t> kept = walker.roundKept(Walker::RoundKeptByWalker::OnlyLayoutWithAllNames, type.layouts, numberOfNames))
            return narrowedTo(LayoutID::fromBits(*kept));
    }
    auto answer = [&](LayoutID only) {
        if (isKept)
            walker.keepRound(Walker::RoundKeptByWalker::OnlyLayoutWithAllNames, type.layouts, numberOfNames, only.bits());
        return narrowedTo(only);
    };
    const LayoutsByNames& index = walker.facts().layoutsByNames();
    for (PropertyKeyID name : names) {
        LayoutID holder = index.findOnlyHolderAmong(name, layouts);
        if (!holder)
            continue;
        if (!index.hasAll(holder, names))
            return answer({ });
        bool someOtherIsInUse = std::ranges::any_of(layouts, [&](LayoutID layout) {
            return layout != holder && index.isInUse(layout);
        });
        return answer(someOtherIsInUse ? holder : LayoutID());
    }
    LayoutID only;
    if (std::span<const uint32_t> holders = index.holdersOfRarestOf(names); holders.size() < layouts.size()) {
        for (uint32_t holder : holders) {
            if (!index.hasAll(LayoutID(holder), names) || !std::ranges::binary_search(layouts, holder, { }, [](LayoutID layout) { return layout.index(); }))
                continue;
            if (only)
                return answer({ });
            only = LayoutID(holder);
        }
        bool someOtherIsInUse = only && std::ranges::any_of(layouts, [&](LayoutID layout) {
            return layout != only && index.isInUse(layout);
        });
        return answer(someOtherIsInUse ? only : LayoutID());
    }
    bool someLayoutLacksName = false;
    for (LayoutID layout : layouts) {
        if (!index.isInUse(layout))
            continue;
        if (!index.hasAll(layout, names)) {
            someLayoutLacksName = true;
            continue;
        }
        if (only)
            return answer({ });
        only = layout;
    }
    return answer(someLayoutLacksName ? only : LayoutID());
}

static TypeID withoutHeirsWhoseClassMayBeEvaluatedAgainForGuesses(Walker& walker, ValueID input, TypeID expected)
{
    const Program& program = walker.program();
    const BodyIndex& index = walker.currentBodyIndex();
    const Function& function = program.at(walker.currentFunction());
    if (!function.takesThisFromConstruction() || !function.constructedClass || !index.isParameter(input) || index.blockOf(input) != entryBlock || function.thisParameterIndex() != std::optional<unsigned>(input.index()))
        return expected;
    TypeArena& types = walker.types();
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || holdsOneMember(type.layouts))
        return expected;
    const Facts& facts = walker.facts();
    if (!facts.isEvaluatedAtMostOnce(declaredLayoutOf(program, function.constructedClass)))
        return expected;
    Vector<LayoutID, 8> evaluatedOnce;
    bool someHeirIsLeftOut = false;
    for (LayoutID layout : types.listOf(type.layouts)) {
        if (facts.isEvaluatedAtMostOnce(declaredLayoutOf(program, layout)))
            evaluatedOnce.append(layout);
        else
            someHeirIsLeftOut = true;
    }
    return someHeirIsLeftOut && !evaluatedOnce.isEmpty() ? types.forLayouts(evaluatedOnce.span()) : expected;
}

static bool holdsLayoutsBesideOtherObjectsForGuesses(const Type& type)
{
    return type.layouts && !type.hasOnlyLayouts() && !type.hasUnboundedUnknown() && !(type.primitives & ~Primitive::UndefinedOrNull);
}

static TypeID narrowedByNameAskedForGuesses(Walker& walker, TypeID expected, PropertyKeyID name)
{
    TypeArena& types = walker.types();
    if (!expected || !name)
        return expected;
    Type type = types.at(expected);
    if (holdsLayoutsBesideOtherObjectsForGuesses(type)) {
        LayoutID only = liesInThread(name) ? LayoutID() : walker.facts().layoutsByNames().findOnlyHolderAmong(name, types.listOf(type.layouts));
        return only ? types.forLayout(only) : expected;
    }
    if (liesInThread(name) || !type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures())
        return expected;
    std::span<const LayoutID> layouts = types.listOf(type.layouts);
    if (layouts.size() < 2)
        return expected;
    auto narrowedTo = [&](LayoutID only) {
        return only ? types.forLayout(only) : expected;
    };
    bool isKept = Walker::keepsRoundsOver(types, type.layouts);
    if (isKept) {
        if (std::optional<uint32_t> kept = walker.roundKept(Walker::RoundKeptByWalker::OnlyLayoutWithName, type.layouts, name.index()))
            return narrowedTo(LayoutID::fromBits(*kept));
    }
    const LayoutsByNames& index = walker.facts().layoutsByNames();
    LayoutID only = index.findOnlyHolderAmong(name, layouts);
    if (only && std::ranges::none_of(layouts, [&](LayoutID layout) { return layout != only && index.isInUse(layout); }))
        only = { };
    if (isKept)
        walker.keepRound(Walker::RoundKeptByWalker::OnlyLayoutWithName, type.layouts, name.index(), only.bits());
    return narrowedTo(only);
}

static ExpectationForGuesses findExpectationForGuesses(Walker&, ValueID input);

bool isExpectedToBeArgumentsObject(Walker& walker, ValueID value)
{
    TypeArena& types = walker.types();
    TypeID argumentsObjects = types.builtinClass(BuiltinClasses::Arguments);
    auto namesThemAlone = [&](TypeID type) {
        return type && !isNeverType(type) && types.at(type).isJavaScriptValue() && types.at(type).hasObjects() && types.withOnlyObjects(type) == argumentsObjects;
    };
    Reference given = walker.referenceOf(value);
    if (given && namesThemAlone(walker.typeOf(given)))
        return true;
    Walker::RememberedExpectation remembered = walker.expectationRememberedOf(value);
    return remembered.isKnown && namesThemAlone(remembered.type);
}

static TypeID typeSeenInStaticFieldForGuesses(Walker& walker, TypeID receiver, PropertyKeyID name)
{
    TypeArena& types = walker.types();
    if (!receiver || !name || liesInThread(name) || !walker.facts().linked().keysOfStaticFields.get(name.index()))
        return { };
    Type type = types.at(receiver);
    FunctionID constructor = type.isObject() && !type.primitives ? singleFunctionOf(types, type) : FunctionID();
    LayoutID classLayout = constructor ? classOfClassObject(walker.program(), constructor) : LayoutID();
    std::optional<uint32_t> field = classLayout ? walker.program().at(classLayout).findStaticField(name) : std::nullopt;
    if (!field)
        return { };
    TypeID seen = walker.facts().held().variable(walker.program().at(classLayout).staticFields[*field].variable).proven;
    return seen ? types.withoutPrimitives(seen, Primitive::Absent) : TypeID();
}

static bool admitsNoNumberOrBooleanForGuesses(const TypeArena& types, TypeID expected)
{
    return expected && !(types.at(expected).primitives & ~(cellPrimitives | Primitive::UndefinedOrNull));
}

static ExpectationForGuesses findWhatIsProvenOrExpectedForGuesses(Walker& walker, ValueID input, ValueID user)
{
    ExpectationForGuesses proven { provenTypeOfInputForGuesses(walker, input), true };
    if (input >= user)
        return saysNothingForGuesses(walker.types(), proven.type) ? ExpectationForGuesses() : proven;
    if (saysNothingForGuesses(walker.types(), proven.type))
        return findExpectationForGuesses(walker, input);

    TypeArena& types = walker.types();
    Type provenType = types.at(types.withoutNullish(proven.type));
    const ClassPart* provenClass = provenType.isObject() ? singleBuiltinClassOf(types, provenType) : nullptr;
    if (!provenClass || provenClass->hasWrittenContents())
        return proven;
    ExpectationForGuesses expected = findExpectationForGuesses(walker, input);
    if (!expected.type)
        return proven;
    TypeID expectedObject = types.withoutNullish(expected.type);
    Type expectedType = types.at(expectedObject);
    const ClassPart* expectedClass = expectedType.isObject() ? singleBuiltinClassOf(types, expectedType) : nullptr;
    if (!expectedClass || expectedClass->builtinClass != provenClass->builtinClass || !expectedClass->hasWrittenContents())
        return proven;
    return { expectedObject, expected.isDeclared, expected.isHeld };
}

static bool testedLayoutsAnswerNoNameUsedForGuesses(Walker&, ValueID, TypeID);

static ALWAYS_INLINE TypeID layoutsThatValueIsShapedLikeForGuesses(Walker& walker, ValueID value)
{
    if (!walker.someValueOfFrameHasShape())
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    for (ValueID named = value; named; named = index.isParameter(named) ? index.valueNarrowedInto(named) : index.opcodeOf(named) == Opcode::Convert ? index.value(named).operand(0) : ValueID()) {
        if (TypeID shape = walker.shapeRememberedOf(named))
            return shape;
    }
    return { };
}

static TypeID kindThatFieldsOfLayoutsHoldForGuesses(Walker& walker, const BodyValue& read, TypeID layouts)
{
    TypeArena& types = walker.types();
    TypeID written = typeOfReadAtLevelExpectedForGuesses(walker, read, layouts);
    if (!saysNothingForGuesses(types, written))
        return written;
    TypeID proven;
    for (LayoutID member : membersOf(types, types.at(layouts))) {
        std::optional<FieldReference> field = findField(walker.program(), member, read.key());
        TypeID ofMember = field ? walker.facts().linked().provenOfField(*field) : TypeID();
        if (!ofMember)
            return { };
        proven = proven ? types.unionOf(proven, ofMember) : ofMember;
    }
    return proven;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool isShapedLikeLayoutsThatItCanNeverBe(Walker& walker, ValueID value)
{
    return !!layoutsThatValueIsShapedLikeForGuesses(walker, value);
}

static ExpectationForGuesses findExpectationOfWhatArrivesForGuesses(Walker& walker, ValueID parameter)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    BlockID block = index.blockOf(parameter);
    ExpectationForGuesses joined { { }, true };
    bool somethingComesRound = false;
    bool nothingIsKnownOfSomeWay = false;
    TypeID shapeOfSomeWay;
    if (!index.isEnteredByHandlerEdge(block)) {
        uint32_t position = parameter.index() - index.block(block).parameter(0).index();
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID argument = position < arguments.size() ? arguments[position] : ValueID();
            if (argument && argument >= parameter) {
                somethingComesRound = true;
                continue;
            }
            ExpectationForGuesses arriving = argument ? findWhatIsProvenOrExpectedForGuesses(walker, argument, parameter) : ExpectationForGuesses();
            joined.isDeclared &= arriving.type && arriving.isDeclared;
            joined.isHeld |= arriving.isHeld;
            nothingIsKnownOfSomeWay |= !arriving.type;
            if (arriving.type)
                joined.type = joined.type ? joinOf(types, walker.facts().families(), joined.type, arriving.type) : arriving.type;
            else if (TypeID shape = argument ? layoutsThatValueIsShapedLikeForGuesses(walker, argument) : TypeID())
                shapeOfSomeWay = shapeOfSomeWay ? types.unionOf(shapeOfSomeWay, shape) : shape;
        }
    }
    if (shapeOfSomeWay) {
        walker.rememberShapeOf(parameter, shapeOfSomeWay);
        if (!joined.type || testedLayoutsAnswerNoNameUsedForGuesses(walker, parameter, joined.type))
            return { };
    }
    if (nothingIsKnownOfSomeWay && joined.type && types.at(joined.type).hasObjects() && types.at(joined.type).hasOnlyLayouts() && !types.at(joined.type).hasOnlyClosures()) {
        if (std::span<const LayoutID> arriving = types.listOf(types.at(joined.type).layouts); arriving.size() == 1) {
            if (LayoutID other = layoutThatAnswersWrittenNamesBesideForGuesses(walker, parameter, arriving[0])) {
                walker.count(StatisticID::LayoutsExpectedBesideOneWhoseOtherNamesAreNeverWritten);
                joined.type = types.unionOf(joined.type, types.forLayout(other));
                joined.isDeclared = true;
            }
        }
    }
    if (somethingComesRound && joined.type && types.at(joined.type).isJavaScriptValue())
        joined.type = types.unionOf(topsOfKindsInLoops(types, types.withoutObjects(joined.type)), types.withOnlyObjects(joined.type));
    if (nothingIsKnownOfSomeWay && joined.type && types.at(joined.type).isJavaScriptValue() && (types.at(joined.type).primitives & Primitive::Boolean))
        joined.type = types.unionOf(joined.type, TypeTable::boolean);
    if (saysNothingForGuesses(types, joined.type) || (nothingIsKnownOfSomeWay && types.at(joined.type).isUndefinedOrNull()))
        joined = { };
    return joined;
}

static bool testedLayoutsAnswerNoNameUsedForGuesses(Walker& walker, ValueID value, TypeID tested)
{
    TypeArena& types = walker.types();
    Type type = types.at(tested);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures())
        return false;
    std::span<const PropertyKeyID> names = walker.namesUsedOff(walker.currentBodyIndex().valueBehindCasts(value));
    if (names.empty())
        return false;
    const LayoutsByNames& byNames = walker.facts().layoutsByNames();
    return std::ranges::none_of(types.listOf(type.layouts), [&](LayoutID layout) {
        return byNames.hasAnyOf(layout, names);
    });
}

static bool nothingIsKnownOfSomeWayIntoJoinForGuesses(Walker& walker, ValueID value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID parameter = index.valueBehindCasts(value);
    if (!index.isParameter(parameter) || index.blockOf(parameter) == entryBlock || index.valueNarrowedInto(parameter))
        return false;
    BlockID block = index.blockOf(parameter);
    if (index.isEnteredByHandlerEdge(block) || !saysNothingForGuesses(walker.types(), walker.asWalked(index.value(parameter)).parameterType()))
        return false;
    uint32_t position = parameter.index() - index.block(block).parameter(0).index();
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID argument = position < arguments.size() ? arguments[position] : ValueID();
        if (argument && argument >= parameter)
            continue;
        if (!argument || !findWhatIsProvenOrExpectedForGuesses(walker, argument, parameter).type)
            return true;
    }
    return false;
}

static ALWAYS_INLINE bool isParameterThatFrontEndGuessedByNamesAloneForGuesses(Walker& walker, ValueID input)
{
    ValueID behind = walker.currentBodyIndex().valueBehindCasts(input);
    const Function& function = walker.program().at(walker.currentFunction());
    const DeclaredType* declared = behind && behind.index() < function.numberOfEntryParameters() ? function.declaredEntryParameter(behind.index()) : nullptr;
    if (!declared || !declared->isExpected() || !declared->written() || function.thisParameterIndex() == std::optional<unsigned>(behind.index()))
        return false;
    Type written = walker.types().at(declared->written());
    return written.hasObjects() && (written.primitives & Primitive::Number) && (written.primitives & Primitive::AnyString) && (written.primitives & Primitive::BigInt);
}

static ExpectationForGuesses asWrittenWhereFamilyLacksNamesForGuesses(Walker& walker, ValueID input, ExpectationForGuesses expectation)
{
    if (!expectation.type || !expectation.isDeclared || expectation.isHeld)
        return expectation;
    const Function& function = walker.program().at(walker.currentFunction());
    const DeclaredType* declared = input.index() < function.numberOfEntryParameters() ? function.declaredEntryParameter(input.index()) : nullptr;
    if (!declared || declared->isEstablished())
        return expectation;
    TypeArena& types = walker.types();
    TypeID written = declared->written();
    if (!written || written == expectation.type || isNeverType(written) || holdsOnlyPrimitives(written) || holdsOnlyPrimitives(expectation.type))
        return expectation;
    Type family = types.at(expectation.type);
    Type set = types.at(written);
    if (!family.hasObjects() || !family.hasOnlyLayouts() || family.hasOnlyClosures() || !set.hasObjects() || !set.hasOnlyLayouts() || set.hasOnlyClosures())
        return expectation;
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    if (names.empty())
        return expectation;
    const LayoutsByNames& byNames = walker.facts().layoutsByNames();
    for (LayoutID layout : types.listOf(family.layouts)) {
        if (byNames.isInUse(layout) && !byNames.hasAll(layout, names)) {
            expectation.type = written;
            return expectation;
        }
    }
    return expectation;
}

static ExpectationForGuesses unlessFamilyMissesWhoAnswersFromAboveForGuesses(Walker& walker, ValueID input, ExpectationForGuesses expectation)
{
    if (!expectation.type || !expectation.isDeclared || expectation.isHeld)
        return expectation;
    const Function& function = walker.program().at(walker.currentFunction());
    const DeclaredType* declared = input.index() < function.numberOfEntryParameters() ? function.declaredEntryParameter(input.index()) : nullptr;
    if (!declared || !declared->isExpected() || function.thisParameterIndex() == std::optional<unsigned>(input.index()))
        return expectation;
    TypeArena& types = walker.types();
    Type family = types.at(expectation.type);
    if (!family.hasObjects() || !family.hasOnlyLayouts() || family.hasOnlyClosures())
        return expectation;
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    if (names.empty())
        return expectation;
    const LayoutsByNames& byNames = walker.facts().layoutsByNames();
    std::span<const LayoutID> members = types.listOf(family.layouts);
    for (uint32_t holder : byNames.holdersOfRarestOf(names)) {
        LayoutID layout(holder);
        if (!byNames.isInstance(layout) || !byNames.hasAll(holder, names) || std::ranges::contains(members, layout))
            continue;
        const Layout& record = walker.program().at(layout);
        bool answersFieldOfFamilyFromAbove = std::ranges::any_of(names, [&](PropertyKeyID name) {
            return !record.findField(name) && std::ranges::any_of(members, [&](LayoutID member) {
                const Layout& ofFamily = walker.program().at(member);
                std::optional<uint32_t> field = ofFamily.findField(name);
                return field && !ofFamily.fields[*field].flags.contains(FieldFlags::IsMethod);
            });
        });
        if (answersFieldOfFamilyFromAbove)
            return { };
    }
    return expectation;
}

static bool comesFromWhereIsArraySaysYesForGuesses(const BodyIndex& index, const BodyIncomingEdge& arriving)
{
    BlockID from = arriving.from;
    uint32_t targetIndex = arriving.targetIndex;
    if (index.terminator(from).kind() == TerminatorKind::Jump) {
        std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(from);
        if (!edge)
            return false;
        from = edge->from;
        targetIndex = edge->targetIndex;
    }
    BodyTerminator end = index.terminator(from);
    if (targetIndex || end.kind() != TerminatorKind::Branch)
        return false;
    ValueID asked = end.operand(0);
    if (!index.isParameter(asked) && index.opcodeOf(asked) == Opcode::ToBoolean)
        asked = index.value(asked).operand(0);
    asked = index.valueBehindCasts(asked);
    if (index.isParameter(asked) || index.opcodeOf(asked) != Opcode::Call)
        return false;
    ValueID callee = index.value(asked).calleeOperand();
    return callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::Builtin && index.value(callee).as<Opcode::Builtin>().builtin() == Builtins::ArrayIsArray;
}

static std::optional<BodyEdgeResult> findProofOfTypeTestForGuesses(const BodyIndex& index, ValueID parameter)
{
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(index.blockOf(parameter));
    if (!edge)
        return std::nullopt;
    uint32_t numberOfProofs = static_cast<uint32_t>(edge->target.proofs().size());
    for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
        std::optional<BodyEdgeResult> result = index.edgeResult(edge->from, edge->targetIndex, edge->target, proofIndex);
        if (!result || result->parameter != parameter)
            continue;
        if (!result->test || index.isParameter(result->test) || index.opcodeOf(result->test) != Opcode::TypeTest)
            return std::nullopt;
        return result;
    }
    return std::nullopt;
}

static ExpectationForGuesses findWholeExpectationForGuesses(Walker&, ValueID input);

static ExpectationForGuesses findWholeExpectationOfParameterForGuesses(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    BlockID block = index.blockOf(input);
    if (block == entryBlock) {
        ExpectationForGuesses expectation = expectationOfEntryParameterForGuesses(walker, input.index());
        TypeID held = typeHeldOfEntryParameterForGuesses(walker, input.index());
        if (saysNothingForGuesses(types, expectation.type))
            expectation = { held, true, true };
        else if (!saysNothingForGuesses(types, held) && (types.at(held).primitives & ~types.at(expectation.type).primitives))
            expectation.type = types.unionOf(expectation.type, held);
        return expectation;
    }
    if (index.numberOfIncomingEdges(block) < 2 || index.isEnteredByHandlerEdge(block) || index.isLoopHead(block))
        return findExpectationForGuesses(walker, input);
    uint32_t position = input.index() - index.block(block).parameter(0).index();
    ExpectationForGuesses joined;
    for (const BodyIncomingEdge& arriving : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = arriving.target.arguments();
        ExpectationForGuesses whole = position < arguments.size() && arguments[position] < input ? findWholeExpectationForGuesses(walker, arguments[position]) : ExpectationForGuesses();
        if (saysNothingForGuesses(types, whole.type))
            return { };
        if (comesFromWhereIsArraySaysYesForGuesses(index, arriving) && !types.overlap(whole.type, types.builtinClass(BuiltinClasses::Array)))
            continue;
        if (joined.type)
            whole.type = types.unionOf(joined.type, whole.type);
        joined = whole;
    }
    return joined;
}

static ExpectationForGuesses findWholeExpectationForGuesses(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    auto keep = [&](ValueID parameter, const ExpectationForGuesses& whole) {
        walker.keepWholeExpectationOf(parameter, { whole.type, whole.isDeclared, whole.isHeld, true });
    };
    Vector<BodyEdgeResult, 8> proofs;
    ExpectationForGuesses whole;
    for (ValueID current = input;;) {
        if (!index.isParameter(current)) {
            BodyValue definition = index.value(current);
            if (definition.opcode() == Opcode::NarrowByTest) {
                current = definition.as<Opcode::NarrowByTest>().leftOperand();
                continue;
            }
            whole = findExpectationForGuesses(walker, current);
            break;
        }
        if (std::optional<Walker::RememberedExpectation> kept = walker.wholeExpectationKeptOf(current)) {
            whole = { kept->type, kept->isDeclared, kept->isHeld };
            break;
        }
        std::optional<BodyEdgeResult> proof = findProofOfTypeTestForGuesses(index, current);
        if (!proof) {
            whole = findWholeExpectationOfParameterForGuesses(walker, current);
            keep(current, whole);
            break;
        }
        proofs.append(*proof);
        current = proof->subject;
    }
    TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };
    for (const BodyEdgeResult& proof : proofs | std::views::reverse) {
        TypeID tested = index.value(proof.test).as<Opcode::TypeTest>().testedType();
        if (saysNothingForGuesses(types, whole.type))
            whole = { };
        else if (!holdsOnlyPrimitives(tested) || !types.at(tested).isUndefinedOrNull())
            whole.type = narrowByTest(expecting, whole.type, tested, proof.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
        keep(proof.parameter, whole);
    }
    return whole;
}

static TypeID withoutRecordsOutOfSightForGuesses(Walker& walker, TypeID expected, bool wasSeenToArrive)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::RecordsThatFileWrites);
    enum : uint32_t { AllStay, NoneStays, SomeStay };
    TypeArena& types = walker.types();
    if (!expected)
        return expected;
    Type type = types.at(expected);
    const Function& function = walker.program().at(walker.currentFunction());
    ModuleID own = function.module;
    if (!own || function.has(FunctionFlags::IsChecked) || !type.isJavaScriptValue() || !type.hasObjects() || types.listOf(type.layouts).empty() || type.hasOnlyClosures())
        return expected;
    auto askEachLayout = [&]() -> TypeArena::WordsOfRound {
        const LayoutsByNames& byNames = walker.facts().layoutsByNames();
        std::span<const LayoutID> layouts = types.listOf(type.layouts);
        Vector<LayoutID, 8> staying;
        for (LayoutID layout : layouts) {
            if (walker.program().at(layout).kind != LayoutKind::Record || byNames.isWrittenInSightOf(own, layout.index()))
                staying.append(layout);
        }
        if (staying.size() == layouts.size())
            return { 0, AllStay };
        if (staying.isEmpty())
            return { 0, NoneStays };
        return { types.forLayouts(staying.span()).bits(), SomeStay };
    };
    TypeArena::WordsOfRound answer;
    if (types.keeps() != ArenaKeeps::UntilReset) {
        bool isKept = Walker::keepsRoundsOver(types, type.layouts);
        std::optional<uint32_t> kept = isKept ? walker.roundKept(Walker::RoundKeptByWalker::RecordsThatFileWrites, type.layouts, own.index()) : std::nullopt;
        if (kept)
            answer = { *kept >> 2, *kept & 3 };
        else {
            answer = askEachLayout();
            if (isKept && answer[0] < 1u << 30)
                walker.keepRound(Walker::RoundKeptByWalker::RecordsThatFileWrites, type.layouts, own.index(), answer[0] << 2 | answer[1]);
        }
    } else {
        TypeArena::SetOfRound round = types.setOfRound(type.layouts);
        if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 2, round, own.index()))
            answer = *kept;
        else {
            answer = askEachLayout();
            types.rememberRound(number, 2, answer[1] == SomeStay ? 1 : 0, round, own.index(), answer);
        }
    }
    if (answer[1] == AllStay || (answer[1] == NoneStays && wasSeenToArrive))
        return expected;
    if (type.hasOnlyLayouts())
        return answer[1] == NoneStays ? TypeID() : types.unionOf(types.withoutObjects(expected), TypeID::fromBits(answer[0]));
    TypeID withoutLayouts = types.differenceOf(expected, types.forLayoutSet(type.layouts));
    return answer[1] == NoneStays ? withoutLayouts : types.unionOf(withoutLayouts, TypeID::fromBits(answer[0]));
}

static ExpectationForGuesses expectationOfTextInPlaceForGuesses(Walker& walker, ValueID input)
{
    if (walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) || !walker.currentBodyIndex().hasLoops() || !turnsCallReaderOfCharactersForGuesses(walker, walker.currentBodyIndex()))
        return { };
    walker.namesUsedOff(input);
    const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(walker.currentBodyIndex());
    if (kept.rootsOfValuesWithMarks.isEmpty() || !std::ranges::binary_search(kept.readsOfTexts, kept.rootOf(input.index())))
        return { };
    const Program& program = walker.program();
    for (PropertyKeyID reader : { program.keyOf(BuiltinNames::charCodeAt), program.keyOf(BuiltinNames::charAt), program.keyOf(BuiltinNames::codePointAt) }) {
        std::array<PropertyKeyID, 2> names { program.keyOf(BuiltinNames::length), reader };
        if (walker.onlyLayoutInSightWithAllNames(std::span<const PropertyKeyID>(names), true))
            return { };
    }
    walker.count(StatisticID::TextsExpectedByNamesOfPlaces);
    return { TypeTable::string, false };
}

static TypeID typeThatLiteralsMadeHereWriteIntoFieldForGuesses(Walker& walker, ValueID read, PropertyKeyID key, TypeID receiver)
{
    static constexpr OpcodeSet operatorsWhoseAnswersAreExpectedToBeNumbers = operatorsThatMakeNumbers | allBitwiseOperators;
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    walker.namesUsedOff(read);
    const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };
    TypeID objects = types.withoutNullish(receiver);
    if (kept.rootsOfValuesWithMarks.isEmpty() || kept.literalsMadeHere.isEmpty() || !objects || isNeverType(objects) || !types.at(objects).hasOnlyLayouts())
        return { };
    TypeID written;
    for (LayoutID member : membersOf(types, types.at(objects))) {
        std::optional<uint32_t> field = walker.program().at(member).findField(key);
        auto literals = std::ranges::equal_range(kept.literalsMadeHere, static_cast<uint64_t>(member.index()), { }, [](uint64_t listed) { return listed >> 32; });
        if (!field || literals.empty())
            return { };
        for (uint64_t listed : literals) {
            BodyValue literal = index.value(ValueID(static_cast<uint32_t>(listed)));
            if (literal.opcode() == Opcode::Data) {
                if (literal.id() >= read)
                    return { };
                bool everyRecordSays = true;
                auto add = [&](const DataNode& node) {
                    const auto* record = node.tryAs<DataNode::Object>();
                    if (!record || record->layout != member)
                        return;
                    const DataMember* held = std::ranges::find(record->members, key, &DataMember::key);
                    TypeID stored = held != record->members.end() ? typeOfData(expecting, held->value) : TypeID();
                    everyRecordSays &= !saysNothingForGuesses(types, stored);
                    if (!everyRecordSays)
                        return;
                    if (types.isSubset(stored, TypeTable::number))
                        stored = TypeTable::number;
                    written = written ? types.unionOf(written, stored) : stored;
                };
                const DataNode& root = walker.program().at(literal.as<Opcode::Data>().data()).root;
                add(root);
                const auto* list = root.tryAs<DataNode::Array>();
                for (const DataNode& element : list ? list->elements : decltype(list->elements)())
                    add(element);
                if (!everyRecordSays)
                    return { };
                continue;
            }
            auto fields = literal.as<Opcode::NewObject>().fields();
            std::span<const ValueID> values = literal.variadicOperands();
            size_t place = std::ranges::find(fields, *field) - fields.begin();
            if (literal.id() >= read || place >= values.size() || fields.size() != values.size() || !values[place])
                return { };
            TypeID stored = provenTypeOfInputForGuesses(walker, values[place]);
            if (saysNothingForGuesses(types, stored)) {
                ValueID root(kept.rootOf(values[place].index()));
                stored = !index.isParameter(root) && operatorsWhoseAnswersAreExpectedToBeNumbers.contains(index.opcodeOf(root)) ? TypeID(TypeTable::number) : findExpectationForGuesses(walker, values[place]).type;
            }
            if (saysNothingForGuesses(types, stored))
                return { };
            if (types.isSubset(stored, TypeTable::number))
                stored = TypeTable::number;
            written = written ? types.unionOf(written, stored) : stored;
        }
    }
    return written;
}

static TypeID typeExpectedOfSlotHandedIntoCopyForGuesses(Walker& walker, const TypingContext& expecting, ValueID parameter)
{
    Reference handed = walker.referenceOf(parameter);
    if (!handed || !handed.isValue())
        return { };
    const auto& read = walker.code().at(handed.id());
    if (read.opcode != Opcode::GetSlot)
        return { };
    TypeID holder = walker.typeOf(Reference::value(read.objectOperand()));
    return holder && walker.types().at(holder).hasOnlyLayouts() ? tryTypeOfFieldRead(expecting, holder, read.key()) : TypeID();
}

static std::optional<TypeID> recordsThatOwnTypeOfParameterNamesForGuesses(Walker& walker, uint32_t entryParameter)
{
    const Function& function = walker.program().at(walker.currentFunction());
    const DeclaredType* declared = !function.has(FunctionFlags::IsChecked) && function.module && entryParameter < function.numberOfEntryParameters() ? function.declaredEntryParameter(entryParameter) : nullptr;
    if (!declared || function.parameters.isEmpty() || declared < function.parameters.begin() || declared >= function.parameters.end())
        return std::nullopt;
    auto rows = walker.facts().layoutsByNames().recordsOfDeclaredParameter(function.module, function.position, static_cast<uint32_t>(declared - function.parameters.begin()));
    if (rows.empty())
        return std::nullopt;
    TypeID named;
    for (const LayoutsByNames::RecordOfDeclaredParameter& row : rows) {
        if (row.record)
            named = walker.types().unionOf(named ? named : TypeTable::nullish, walker.types().forLayout(row.record));
    }
    return named;
}

static bool isOpenedByIterationAloneForGuesses(const BodyIndex& index, ValueID input)
{
    ValueID user = index.onlyUsingValue(input);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    return user && index.opcodeOf(user) == Opcode::IteratorOpen;
}

static TypeID classThatEveryFieldNamesForGuesses(Walker& walker, const TypingContext& expecting, const BodyValue& read, TypeID receiver)
{
    TypeArena& types = walker.types();
    TypeID present = types.withoutNullish(receiver);
    LayoutID record = isNeverType(present) ? LayoutID() : singleLayoutOf(types, types.at(present));
    TypeID key = provenTypeOfInputForGuesses(walker, read.operand(OperandIndex::ElemGet::property));
    if (!record || (key && types.at(key).isNumber()) || walker.program().at(record).kind != LayoutKind::Record)
        return { };
    TypeID held;
    for (const Field& field : walker.program().at(record).fields) {
        TypeID named = field.form.isEmpty() && !field.flags.contains(FieldFlags::IsMethod) ? expecting.typeIDOf(field.declared) : TypeID();
        if (saysNothingForGuesses(types, named))
            return { };
        named = types.withoutNullish(named);
        held = held ? types.unionOf(held, named) : named;
        if (isNeverType(held) || !singleBuiltinClassOf(types, types.at(held)))
            return { };
    }
    return held;
}

static bool ownLiteralMeetsWhatArrivesForGuesses(Walker&, ValueID asked, TypeID expected);

static bool staysTheSameThroughTurnsOfTypedCopyForGuesses(Walker& walker, ValueID input)
{
    Reference built = walker.referenceOf(input);
    if (!built || !built.isValue())
        return false;
    if (walker.standsInFrontOfTypedCopy(built, Walker::TurnOfTypedCopy::HasRunNoOtherCode))
        return true;
    const Code& code = walker.code();
    ValueID read = built.id();
    while (opcodesThatGiveSameValue.contains(code.at(read).opcode) || code.at(read).opcode == Opcode::CheckInitialized)
        read = code.at(read).operand(0);
    return code.at(read).opcode == Opcode::GetModuleVariable && walker.program().at(code.at(read).variable()).flags.contains(VariableFlags::IsImmutable);
}

static bool isListThatFrontEndGuessedByNamesAloneForGuesses(Walker& walker, ValueID list)
{
    ValueID behind = walker.currentBodyIndex().valueBehindCasts(list);
    const Function& function = walker.program().at(walker.currentFunction());
    const DeclaredType* declared = behind && behind.index() < function.numberOfEntryParameters() ? function.declaredEntryParameter(behind.index()) : nullptr;
    if (!declared || !declared->isExpected() || !declared->written())
        return false;
    TypeArena& types = walker.types();
    TypeID lists = types.withoutNullish(declared->written());
    if (isNeverType(lists) || !types.isSubset(lists, types.builtinClass(BuiltinClasses::Array)))
        return false;
    TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };
    Type element = types.at(typeOfContentsRead(expecting, lists, ContentsSlot::Element));
    return element.hasObjects() && (element.primitives & Primitive::Number) && (element.primitives & Primitive::AnyString) && (element.primitives & Primitive::BigInt);
}

static TypeID typeOfElementWithoutLayoutsThatStayWithCompiledCodeForGuesses(Walker& walker, ValueID list, ValueID element, ValueID readThatIsHeld, TypeID expected)
{
    if (!expected || !isListThatFrontEndGuessedByNamesAloneForGuesses(walker, list))
        return expected;
    TypeID left = withoutLayoutsThatStayWithCompiledCodeForGuesses(walker, element, element, expected, [&] { return walker.facts().held().resultOfReadByKey(walker.currentFunction(), readThatIsHeld); });
    if (left != expected)
        walker.count(StatisticID::ElementsOfListsGuessedByNamesNotAskedForLayoutsThatStayWithCompiledCode);
    return left;
}

static ExpectationForGuesses findExpectationAnewForGuesses(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };

    BodyValue definition = walker.asWalked(index.value(input));
    Opcode opcode = definition.opcode();
    ExpectationForGuesses expectation;
    ValueID behind;
    bool isNamedByOwnType = false;
    if (definition.isParameter()) {
        if (std::optional<TypeID> named = index.blockOf(input) == entryBlock ? recordsThatOwnTypeOfParameterNamesForGuesses(walker, input.index()) : std::nullopt) {
            expectation = { *named, true };
            isNamedByOwnType = true;
        } else if (index.blockOf(input) == entryBlock) {
            expectation = expectationOfEntryParameterForGuesses(walker, input.index());
            if (isParameterThatFrontEndGuessedByNamesAloneForGuesses(walker, input))
                expectation.type = withoutLayoutsThatStayWithCompiledCodeForGuesses(walker, input, input, expectation.type, [&] { return typeHeldOfEntryParameterForGuesses(walker, input.index()); });
            expectation = asWrittenWhereFamilyLacksNamesForGuesses(walker, input, expectation);
            expectation = unlessFamilyMissesWhoAnswersFromAboveForGuesses(walker, input, expectation);
            if (walker.depthOfCopies() && saysNothingForGuesses(types, expectation.type))
                expectation = { typeExpectedOfSlotHandedIntoCopyForGuesses(walker, expecting, input), false };
            if (saysNothingForGuesses(types, expectation.type))
                expectation = { typeHeldOfEntryParameterForGuesses(walker, input.index()), true, true };
        } else if (ValueID tested = index.valueNarrowedInto(input))
            behind = tested;
        else {
            expectation = { definition.parameterType(), true };
            if (saysNothingForGuesses(types, expectation.type))
                expectation = findExpectationOfWhatArrivesForGuesses(walker, input);
        }
    } else if (opcode == Opcode::NarrowByTest)
        expectation = findWholeExpectationForGuesses(walker, input);
    else if (opcode == Opcode::Convert) {
        DeclaredType declared = definition.as<Opcode::Convert>().declared();
        expectation = { targetOfConvert(expecting, declared), !declared.isExpected() };
        if (declared.isExpected() && !saysNothingForGuesses(types, expectation.type) && types.at(expectation.type).hasObjects() && !walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked)) {
            ExpectationForGuesses ofValue = findExpectationForGuesses(walker, definition.operand(0));
            expectation.type = withoutLayoutsThatStayWithCompiledCodeForGuesses(walker, input, definition.operand(0), expectation.type, [&] { return ofValue.type; });
            if (!expectation.type)
                return ofValue;
            if (!saysNothingForGuesses(types, ofValue.type) && types.at(ofValue.type).hasObjects()) {
                TypeID come = types.withOnlyObjects(ofValue.type);
                TypeID stated = types.withOnlyObjects(expectation.type);
                if (types.at(come).hasOnlyClassObjects() && types.at(stated).hasOnlyClassObjects() && !types.overlap(come, stated))
                    expectation = { types.withPrimitives(come, static_cast<uint16_t>(types.at(expectation.type).primitives & Primitive::UndefinedOrNull)), ofValue.isDeclared, ofValue.isHeld };
            }
        }
    } else if (opcode == Opcode::Check || opcode == Opcode::Expect)
        expectation = { definition.expectedType().predicted(), true };
    else if (opcode == Opcode::CatchValue)
        expectation = { walker.facts().held().caughtAt(walker.currentFunction(), index.blockOf(input)), true, true };
    else if (opcode == Opcode::Construct) {
        expectation = expectationOfConstructionForGuesses(walker, definition);
        if (saysNothingForGuesses(types, expectation.type))
            expectation = { typeExpectedOfResultOfCall(walker, definition), false };
    } else if (callsWithExpectedResultsForGuesses.contains(opcode)) {
        expectation = expectationOfCallForGuesses(walker, definition);
        if (saysNothingForGuesses(types, expectation.type))
            expectation = { typeHeldOfResultOfCallForGuesses(walker, definition), true, true };
        if (saysNothingForGuesses(types, expectation.type))
            expectation = { typeThatMapIsExpectedToGive(walker, definition), true, true };
        if (saysNothingForGuesses(types, expectation.type))
            expectation = { typeExpectedOfResultOfCall(walker, definition), false };
    } else if (opcode == Opcode::CallDirect)
        expectation = expectationOfDirectCallForGuesses(walker, definition);
    else if (readsOfDeclaredPlacesForGuesses.contains(opcode)) {
        if (std::optional<uint8_t> receiver = placeOfReceiverForGuesses(opcode)) {
            ExpectationForGuesses ofReceiver = findWhatIsProvenOrExpectedForGuesses(walker, definition.operand(*receiver), input);
            if (ofReceiver.type)
                expectation = { typeOfReadAtLevelExpectedForGuesses(walker, definition, ofReceiver.type), ofReceiver.isDeclared && !readsFieldWithoutWrittenTypeForGuesses(walker, definition, ofReceiver.type), ofReceiver.isHeld };
            if (TypeID shape = opcode == Opcode::PropGet && !definition.isFreeName() && saysNothingForGuesses(types, expectation.type) ? layoutsThatValueIsShapedLikeForGuesses(walker, definition.operand(*receiver)) : TypeID()) {
                TypeID given = kindThatFieldsOfLayoutsHoldForGuesses(walker, definition, shape);
                if (given && holdsOnlyPrimitives(given) && !saysNothingForGuesses(types, given))
                    expectation = { topsOfKindsInLoops(types, given), false };
            }
            if (opcode == Opcode::PropGet && !definition.isFreeName() && ofReceiver.type && walker.isInLoop() && saysNothingForGuesses(types, expectation.type))
                expectation.type = typeThatLiteralsMadeHereWriteIntoFieldForGuesses(walker, input, definition.key(), ofReceiver.type);
            if (opcode == Opcode::ElemGet && walker.isInLoop() && saysNothingForGuesses(types, expectation.type)) {
                walker.namesUsedOff(input);
                const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
                std::span<const uint64_t> puts = kept.rootsOfValuesWithMarks.isEmpty() ? std::span<const uint64_t>() : kept.layoutsPutIntoListsMadeHere.span();
                uint64_t list = puts.empty() ? 0 : kept.rootOf(definition.operand(*receiver).index());
                auto put = std::ranges::equal_range(puts, list, { }, [](uint64_t listed) { return listed >> 32; });
                if (!put.empty() && static_cast<uint32_t>(put.back()) != UINT32_MAX) {
                    TypeID elements;
                    for (uint64_t listed : put)
                        elements = elements ? types.unionOf(elements, types.forLayout(LayoutID(static_cast<uint32_t>(listed)))) : types.forLayout(LayoutID(static_cast<uint32_t>(listed)));
                    expectation = { elements, false };
                }
            }
            if (opcode == Opcode::PropGet && !definition.isFreeName() && ofReceiver.type && saysNothingForGuesses(types, expectation.type)) {
                if (TypeID seen = typeSeenInStaticFieldForGuesses(walker, ofReceiver.type, definition.key()))
                    expectation = { seen, false, true };
            }
            if (ofReceiver.type && saysNothingForGuesses(types, expectation.type))
                expectation.type = typeOfReadOffBuiltinForGuesses(walker, definition, ofReceiver.type);
            if (opcode == Opcode::ElemGet && ofReceiver.type && saysNothingForGuesses(types, expectation.type) && isOpenedByIterationAloneForGuesses(index, input)) {
                if (TypeID named = classThatEveryFieldNamesForGuesses(walker, expecting, definition, ofReceiver.type))
                    expectation = { named, false };
            }
            if (opcode == Opcode::ElemGet && ofReceiver.type && saysNothingForGuesses(types, expectation.type) && types.isSubset(types.withoutNullish(ofReceiver.type), types.builtinClass(BuiltinClasses::Array))) {
                TypeID key = provenTypeOfInputForGuesses(walker, definition.operand(OperandIndex::ElemGet::property));
                if (!key || !types.at(key).isAnyString())
                    expectation.type = typeOfContentsRead(expecting, types.withoutNullish(ofReceiver.type), ContentsSlot::Element);
            }
            if (LayoutID heir = opcode == Opcode::PropGet && !definition.isFreeName() && ofReceiver.type && saysNothingForGuesses(types, expectation.type) ? layoutKnownByStructureOfClass(walker, types.withoutNullish(ofReceiver.type)) : LayoutID()) {
                if (std::optional<uint32_t> field = walker.program().at(heir).findField(definition.key()))
                    expectation = { numbersExpectedOfFieldOfHeir(walker, heir, *field), ofReceiver.isDeclared, true };
            }
            if (ofReceiver.type && saysNothingForGuesses(types, expectation.type))
                expectation = { typeOfReadByWhatIsHeldForGuesses(walker, definition, ofReceiver.type), ofReceiver.isDeclared, true };
        } else {
            expectation = { typeOfReadAtLevelExpectedForGuesses(walker, definition, { }), true };
            if (saysNothingForGuesses(types, expectation.type))
                expectation = { typeOfReadByWhatIsHeldForGuesses(walker, definition, { }), true, true };
            else if (!walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) && !types.at(expectation.type).hasObjects() && index.onlyUsingValue(input)) {
                TypeID held = typeOfReadByWhatIsHeldForGuesses(walker, definition, { });
                Type objects = !saysNothingForGuesses(types, held) && types.at(held).hasObjects() ? types.at(types.withOnlyObjects(held)) : Type();
                bool someObjectAnswersNames = objects.isObject() && objects.hasOnlyLayouts() && !objects.hasOnlyClosures() && !walker.namesUsedOff(input).empty() && std::ranges::any_of(membersOf(types, objects), [&](LayoutID member) {
                    return walker.facts().layoutsByNames().isInUse(member) && walker.facts().layoutsByNames().hasAll(member.index(), walker.namesUsedOff(input));
                });
                if (someObjectAnswersNames)
                    expectation = { types.withOnlyObjects(held), true, true };
            }
        }
        if (opcode == Opcode::ElemGet)
            expectation.type = typeOfElementWithoutLayoutsThatStayWithCompiledCodeForGuesses(walker, definition.operand(OperandIndex::ElemGet::object), input, input, expectation.type);
        if (opcode == Opcode::ElemGet && saysNothingForGuesses(types, expectation.type))
            expectation = { walker.facts().held().resultOfReadByKey(walker.currentFunction(), input), true, true };
    } else if (opcode == Opcode::ResultAt) {
        if (ValueID iterable = iterableThatStepReadsForGuesses(index, definition)) {
            ExpectationForGuesses ofIterable = findWhatIsProvenOrExpectedForGuesses(walker, iterable, input);
            if (ofIterable.type) {
                TypeID iterated = types.withoutNullish(ofIterable.type);
                TypeID pairs = typeOfPairsThatStepsHandOut(walker, iterated);
                if (types.at(iterated).isAnyString())
                    expectation = { TypeTable::string, ofIterable.isDeclared, ofIterable.isHeld };
                else
                    expectation = { pairs ? pairs : typeOfContentsRead(expecting, iterated, ContentsSlot::Element), ofIterable.isDeclared, ofIterable.isHeld };
                expectation.type = typeOfElementWithoutLayoutsThatStayWithCompiledCodeForGuesses(walker, iterable, input, definition.operand(0), expectation.type);
            }
            if (saysNothingForGuesses(types, expectation.type))
                expectation = { walker.facts().held().resultOfReadByKey(walker.currentFunction(), definition.operand(0)), true, true };
        }
    }

    bool isSeen = expectation.isHeld || (definition.isParameter() && index.blockOf(input) == entryBlock && !expectation.isDeclared);
    if (TypeID proven = isSeen && !saysNothingForGuesses(types, expectation.type) && !types.at(expectation.type).hasObjects() ? provenTypeOfInputForGuesses(walker, input) : TypeID(); proven && types.isSubset(proven, types.withoutNullish(expectation.type)))
        expectation = { };
    bool isHeldInVariable = expectation.isHeld && readsOfDeclaredPlacesForGuesses.contains(opcode) && !placeOfReceiverForGuesses(opcode);
    bool hasEveryPrimitive = expectation.type && (types.at(expectation.type).primitives & Primitive::All) == Primitive::All;
    bool isViewThatTestOfProgramNarrowed = !definition.isParameter() && opcode == Opcode::NarrowByTest;
    if (isSeen && expectation.type && (hasEveryPrimitive || (!isViewThatTestOfProgramNarrowed && types.at(expectation.type).hasObjects() && !admitsNoNumberOrBooleanForGuesses(types, expectation.type)))) {
        Type objects = types.at(types.withOnlyObjects(expectation.type));
        ValueID user = index.onlyUsingValue(input);
        bool isCalled = user && !index.isParameter(user) && index.opcodeOf(user) == Opcode::Call && index.value(user).calleeOperand() == input;
        std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
        bool isThisOfFunction = definition.isParameter() && index.blockOf(input) == entryBlock && walker.program().at(walker.currentFunction()).thisParameterIndex() == std::optional<unsigned>(input.index());
        bool someObjectOfProgramAnswersNames = objects.hasOnlyLayouts() && !objects.hasOnlyClosures() && (names.empty() ? isThisOfFunction : std::ranges::any_of(membersOf(types, objects), [&](LayoutID member) {
            return walker.facts().layoutsByNames().isInUse(member) && walker.facts().layoutsByNames().hasAll(member.index(), names);
        }));
        const ClassPart* onlyClass = objects.isObject() ? singleBuiltinClassOf(types, objects) : nullptr;
        bool someNameIsOfBuiltinClassAlone = onlyClass && std::ranges::any_of(names, [&](PropertyKeyID key) {
            BuiltinName name = liesInThread(key) ? BuiltinName() : walker.program().builtinNameOf(key);
            if (!name)
                return false;
            auto classes = walker.program().builtins().classesWithInstanceMember(name);
            return classes.size() == 1 && classes[0] == onlyClass->builtinClass;
        });
        if (!someObjectOfProgramAnswersNames && definition.isParameter() && index.blockOf(input) == entryBlock) {
            TypeID held = typeHeldOfEntryParameterForGuesses(walker, input.index());
            someObjectOfProgramAnswersNames = !saysNothingForGuesses(types, held) && !types.at(held).primitives && types.at(held).hasOnlyLayouts() && !types.at(held).hasOnlyClosures()
                && std::ranges::any_of(types.listOf(types.at(held).layouts), [&](LayoutID member) {
                    return std::ranges::any_of(names, [&](const PropertyKeyID& name) { return walker.facts().layoutsByNames().hasAll(member, singleElementSpan(name)); });
                });
        }
        if (!isCalled && !someObjectOfProgramAnswersNames && !someNameIsOfBuiltinClassAlone && (hasEveryPrimitive || (names.empty() && !isOpenedByIterationAloneForGuesses(index, input)))) {
            if (!hasEveryPrimitive) {
                if (ExpectationForGuesses byKey = expectationOfWhatLoopReadsByKeyForGuesses(walker, input); byKey.type)
                    return byKey;
            }
            expectation = { };
            isHeldInVariable = false;
        }
    }
    if (!isNamedByOwnType && !isHeldInVariable && !saysNothingForGuesses(types, expectation.type))
        expectation.type = withoutRecordsOutOfSightForGuesses(walker, partThatOneTestTellsForGuesses(types, expectation.type), isSeen);
    if (!saysNothingForGuesses(types, expectation.type)) {
        TypeID told = partThatOneTestTellsForGuesses(types, expectation.type);
        TypeID narrowed = narrowedByNamesUsedForGuesses(walker, input, told);
        if (narrowed != told && ownLiteralMeetsWhatArrivesForGuesses(walker, input, told))
            narrowed = told;
        expectation.type = withoutHeirsWhoseClassMayBeEvaluatedAgainForGuesses(walker, input, narrowed);
        if (expectation.type && types.at(expectation.type).isUndefinedOrNull()) {
            if (ExpectationForGuesses byNames = expectationByNamesUsedForGuesses(walker, input, findNameUsedOffValueForGuesses(walker.program(), index, input)); byNames.type)
                return byNames;
        }
        if (expectation.isHeld && expectation.type && (types.at(expectation.type).isUndefinedOrNull() || types.at(types.withoutNullish(expectation.type)).hasOnlyClosures())) {
            if (ExpectationForGuesses byKey = expectationOfWhatLoopReadsByKeyForGuesses(walker, input); byKey.type)
                return byKey;
        }
        if (expectation.isHeld && expectation.type && holdsLayoutsBesideOtherObjectsForGuesses(types.at(expectation.type))) {
            if (ExpectationForGuesses byKey = expectationOfWhatLoopReadsByKeyForGuesses(walker, input); byKey.type && types.isSubset(byKey.type, expectation.type))
                return byKey;
        }
        if (!expectation.isHeld || !types.at(expectation.type).isUndefinedOrNull())
            return expectation;
    }
    if (!behind && !definition.isParameter() && inputOperationsThatHandOn.contains(opcode))
        behind = definition.operand(0);
    if (!behind && !definition.isParameter() && numberOfNamedOperandsOf(opcode) == 1 && !hasVariadicOperand(opcode) && definition.operandUses(0).containsOnly({ OperandUse::Returns }))
        behind = definition.operand(0);
    if (behind && behind < input) {
        ExpectationForGuesses ofValueBehind = findExpectationForGuesses(walker, behind);
        if (!saysNothingForGuesses(types, ofValueBehind.type))
            return ofValueBehind;
    }
    if (TypeID ofAnswer = walker.typeExpectedOfAnswerOfCopy(); ofAnswer && types.at(ofAnswer).hasObjects() && admitsNoNumberOrBooleanForGuesses(types, ofAnswer) && walker.copyHandsBack(input)) {
        ExpectationForGuesses stated { ofAnswer, walker.answerOfCopyIsDeclared() };
        stated.type = narrowedByNamesUsedForGuesses(walker, input, partThatOneTestTellsForGuesses(types, stated.type));
        return stated;
    }
    if (textOfBodyIsSilentOnValueForGuesses(walker, index, input) && !findNameUsedOffValueForGuesses(walker.program(), index, input))
        return { };
    for (uint64_t cast : walker.castsOf(input)) {
        DeclaredType declared = index.value(ValueID(static_cast<uint32_t>(cast))).as<Opcode::Convert>().declared();
        ExpectationForGuesses stated { targetOfConvert(expecting, declared), !declared.isExpected() };
        if (saysNothingForGuesses(types, stated.type) || !types.at(stated.type).hasObjects() || !admitsNoNumberOrBooleanForGuesses(types, stated.type))
            continue;
        stated.type = narrowedByNamesUsedForGuesses(walker, input, partThatOneTestTellsForGuesses(types, stated.type));
        return stated;
    }
    PropertyKeyID firstUsed = findNameUsedOffValueForGuesses(walker.program(), index, input);
    if (ExpectationForGuesses ofTextInPlace = expectationOfTextInPlaceForGuesses(walker, input); ofTextInPlace.type)
        return ofTextInPlace;
    if (ExpectationForGuesses byNames = expectationByNamesUsedForGuesses(walker, input, firstUsed); byNames.type)
        return byNames;
    if (ExpectationForGuesses byName = firstUsed ? expectationByNameThatOneBuiltinClassAnswersForGuesses(walker, input) : ExpectationForGuesses(); byName.type)
        return byName;
    if (ExpectationForGuesses ofList = firstUsed ? expectationOfListHandedFunctionWrittenInPlaceForGuesses(walker, input) : ExpectationForGuesses(); ofList.type)
        return ofList;
    std::optional<BodyUser> firstUser = firstUsed || !index.hasLoops() ? std::nullopt : index.firstUser(input);
    bool isReadByKeyFirst = firstUser && firstUser->kind == BodyUseKind::Operand && index.opcodeOf(firstUser->value) == Opcode::ElemGet && index.value(firstUser->value).operand(0) == input;
    auto saysNothingBeyondNumberOrBigInteger = [&](TypeID proven) {
        if (saysNothingForGuesses(types, proven))
            return true;
        const Type& known = types.at(proven);
        return !known.hasObjects() && (known.primitives & Primitive::BigInt) && (known.primitives & Primitive::Number) && !(known.primitives & ~(Primitive::Number | Primitive::BigInt));
    };
    bool standsInTypedCopyOfLoopOfCaller = walker.mayBeginTurnAgainInClosedCopy() && !walker.isInLoopOfBodyAtWork();
    if (!firstUsed && !isReadByKeyFirst && (index.hasLoops() || standsInTypedCopyOfLoopOfCaller) && saysNothingBeyondNumberOrBigInteger(provenTypeOfInputForGuesses(walker, input))) {
        walker.namesUsedOff(input);
        const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
        ValueID root = kept.rootsOfValuesWithMarks.isEmpty() ? ValueID() : ValueID(kept.rootOf(input.index()));
        TypeID writtenOfRoot = root && index.isParameter(root) && index.blockOf(root) == entryBlock ? expectationOfEntryParameterForGuesses(walker, root.index()).type : TypeID();
        if (root && (!writtenOfRoot || types.overlap(writtenOfRoot, TypeTable::int32)) && std::ranges::binary_search(kept.rootsFromInFrontThatLoopsMakeWholeNumbersOf, root.index()))
            return { TypeTable::int32, false };
        if (root && (!writtenOfRoot || types.overlap(writtenOfRoot, TypeTable::number)) && std::ranges::binary_search(kept.rootsFromInFrontThatLoopsReckonWith, root.index()))
            return { TypeTable::number, false };
        if (root && !walker.mayBeginTurnAgainInClosedCopy() && std::ranges::binary_search(kept.rootsMadeInTurnsThatLoopsReckonWith, root.index()))
            return { TypeTable::number, false };
        if (root && standsInTypedCopyOfLoopOfCaller && !walker.leavesOutQuestionThatEveryTurnWouldAsk() && staysTheSameThroughTurnsOfTypedCopyForGuesses(walker, input) && (!writtenOfRoot || types.overlap(writtenOfRoot, TypeTable::number)) && std::ranges::binary_search(kept.rootsThatOperatorsOutsideLoopsReckonWith, root.index()))
            return { TypeTable::number, false };
    }
    if (ExpectationForGuesses ofText = expectationByNamesOfTextsForGuesses(walker, input, firstUsed); ofText.type || (!firstUsed && !isReadByKeyFirst))
        return ofText;
    ExpectationForGuesses byKey = expectationOfWhatLoopReadsByKeyForGuesses(walker, input);
    if (!byKey.type && index.hasLoops()) {
        walker.namesUsedOff(input);
        const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
        auto isRowThatIsReckonedWith = [&] {
            std::optional<uint64_t> row = kept.rowsReckonedWithAtWrittenPlacesInLoops.isEmpty() ? std::nullopt : kept.holderAndKeyOfRow(index, kept.rootOf(input.index()));
            return row && std::ranges::binary_search(kept.rowsReckonedWithAtWrittenPlacesInLoops, *row);
        };
        if (!kept.rootsOfValuesWithMarks.isEmpty() && (std::ranges::binary_search(kept.rootsReckonedWithAtWrittenPlacesInLoops, kept.rootOf(input.index())) || isRowThatIsReckonedWith())) {
            std::array<TypeID, 1> numbers { TypeTable::number };
            return { types.builtinClass(BuiltinClasses::Array, numbers), false };
        }
    }
    if (byKey.type && !firstUsed)
        walker.count(StatisticID::BasesAskedThatAnElementReadsFirst);
    return byKey;
}

ExpectationOfValue expectationOfAnswerOfCallBeingCopied(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return { };
    ExpectationForGuesses expected = findExpectationForGuesses(walker, call.id());
    if (saysNothingForGuesses(walker.types(), expected.type))
        return { };
    return { expected.type, expected.isDeclared };
}

static ExpectationForGuesses expectationTakenInPlaceOfWhatIsFoundForGuesses(Walker& walker, ValueID input, const ExpectationForGuesses& found)
{
    if (walker.ignoresExpectedTypes())
        return { };
    uint32_t function = walker.currentFunction().index();
    uint32_t drawn = walker.drawFor(DrawnFor::ExpectedType, function, input.index()) % 4;
    if (!drawn)
        return { };
    if (drawn != 1 || !input.index())
        return found;
    ValueID other(walker.drawFor(DrawnFor::OtherExpectedType, function, input.index()) % input.index());
    ExpectationForGuesses ofOther = findExpectationForGuesses(walker, other);
    return ofOther.type ? ExpectationForGuesses { ofOther.type, false, false } : found;
}

static bool constructorIsExpectedToBeHandedItsOwnClassUnseenForGuesses(Walker& walker, ValueID input, TypeID expected)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!expected || !index.isParameter(input) || index.blockOf(input) != entryBlock)
        return false;
    FunctionID function = walker.currentFunction();
    const Function& record = walker.program().at(function);
    if (!record.constructedClass || record.thisParameterIndex() == std::optional<unsigned>(input.index()))
        return false;
    TypeArena& types = walker.types();
    TypeID objects = types.withOnlyObjects(expected);
    Type expectedObjects = types.at(objects);
    if (!expectedObjects.hasObjects() || !expectedObjects.hasOnlyLayouts() || expectedObjects.hasOnlyClosures() || !types.isSubset(types.forLayout(record.constructedClass), objects))
        return false;
    bool everyOtherLayoutIsBornNowhere = std::ranges::all_of(membersOf(types, expectedObjects), [&](LayoutID member) {
        return member == record.constructedClass || !walker.plan().isInUse(member);
    });
    if (!everyOtherLayoutIsBornNowhere)
        return false;
    const LinkTables& linked = walker.facts().linked();
    if (function.index() >= linked.functions.size() || (!linked.functions[function.index()].everyCallerIsKnown && walker.isInLoop()))
        return false;
    std::span<const LinkedParameter> parameters = linked.parametersOf(function);
    if (input.index() >= parameters.size())
        return false;
    TypeID seen = parameters[input.index()].seenToArrive;
    return !seen || types.saysNothing(seen) || !types.overlap(seen, objects);
}

static NEVER_INLINE ExpectationForGuesses lessWhatInstanceOfDeniedForGuesses(Walker& walker, ValueID input, ExpectationForGuesses expectation)
{
    if (!expectation.type || !walker.currentBodyIndex().opcodes().contains(Opcode::InstanceOf))
        return expectation;
    Reference value = walker.referenceOf(input);
    std::optional<uint64_t> denied = value && value.isValue() ? findKnown(walker, KnownNote::ClassesThatInstanceOfDenied, value) : std::nullopt;
    if (!denied)
        return expectation;
    TypeArena& types = walker.types();
    TypeID left = types.differenceOf(expectation.type, types.forLayoutSet(LayoutSetID::fromBits(static_cast<uint32_t>(*denied))));
    if (left == expectation.type)
        return expectation;
    if (isNeverType(left) || holdsOnlyPrimitives(left))
        return { };
    expectation.type = left;
    return expectation;
}

static NEVER_INLINE ExpectationForGuesses findAndRememberExpectationForGuesses(Walker& walker, ValueID input)
{
    Walker::RememberedExpectation& remembered = walker.rememberedExpectationOf(input);
    remembered.isKnown = true;
    walker.forgetShapeOf(input);
    ExpectationForGuesses expectation = findExpectationAnewForGuesses(walker, input);
    if (constructorIsExpectedToBeHandedItsOwnClassUnseenForGuesses(walker, input, expectation.type))
        expectation = { };
    refineExpectationOfValue(walker, input, expectation.type, expectation.isDeclared, expectation.restsOnNamesUsedAlone);
    if (walker.ignoresExpectedTypes() || walker.shufflesExpectedTypes()) [[unlikely]] {
        TypeID found = expectation.type;
        expectation = expectationTakenInPlaceOfWhatIsFoundForGuesses(walker, input, expectation);
        if (expectation.type != found)
            walker.count(StatisticID::ExpectedTypesTakenOtherwise);
    }
    if (expectation.type && !holdsOnlyPrimitives(expectation.type) && walker.types().recordOf(expectation.type).traitsOfNamed.contains(ObjectTrait::IsClosure)) {
        TypeArena& types = walker.types();
        uint32_t asker = walker.unit().function.index() << 4 | static_cast<uint32_t>(walker.unit().role);
        bool isRemembered = types.keeps() == ArenaKeeps::UntilReset;
        std::optional<uint32_t> known = isRemembered ? types.rememberedAnswer(static_cast<uint8_t>(AnswerKeptInArena::UnitMayMentionFunctions), expectation.type.bits(), asker) : std::nullopt;
        bool mayBeTestedFor = known.value_or(true);
        if (!known) {
            forEachFunctionOf(types, types.at(expectation.type), scopedLambda<void(FunctionID)>([&](FunctionID named) {
                mayBeTestedFor = mayBeTestedFor && walker.facts().linked().mayBeMentionedBy(walker.unit(), named);
            }));
            if (isRemembered)
                types.rememberAnswer(static_cast<uint8_t>(AnswerKeptInArena::UnitMayMentionFunctions), expectation.type.bits(), asker, mayBeTestedFor);
        }
        if (!mayBeTestedFor)
            expectation = { };
    }
    remembered = { expectation.type, expectation.isDeclared, expectation.isHeld, true, expectation.restsOnNamesUsedAlone };
    if (Vector<ValueID, 16>* rememberedForLook = walker.expectationsRememberedForLook())
        rememberedForLook->append(input);
    return lessWhatInstanceOfDeniedForGuesses(walker, input, expectation);
}

static ExpectationForGuesses findExpectationAsRememberedForGuesses(Walker& walker, ValueID input)
{
    Walker::RememberedExpectation remembered = walker.expectationRememberedOf(input);
    if (!remembered.isKnown)
        return findAndRememberExpectationForGuesses(walker, input);
    ExpectationForGuesses expectation { remembered.type, remembered.isDeclared, remembered.isHeld, remembered.restsOnNamesUsedAlone };
    if (!expectation.type || !walker.currentBodyIndex().opcodes().contains(Opcode::InstanceOf))
        return expectation;
    return lessWhatInstanceOfDeniedForGuesses(walker, input, expectation);
}

static ALWAYS_INLINE ExpectationForGuesses unlessEveryTurnWouldAskItForGuesses(Walker& walker, ValueID input, ExpectationForGuesses expectation)
{
    if (walker.walksCopyWithoutQuestionsOfEveryTurn() && expectation.type && walker.types().at(expectation.type).isNumber() && walker.leavesOutQuestionThatEveryTurnWouldAsk() && staysTheSameThroughTurnsOfTypedCopyForGuesses(walker, input)) [[unlikely]]
        return { };
    return expectation;
}

static NEVER_INLINE ExpectationForGuesses orWhatIsExpectedOfArgumentOfCopyForGuesses(Walker& walker, ValueID input, ExpectationForGuesses expectation)
{
    const Walker::ExpectationOfArgument* ofArgument = saysNothingForGuesses(walker.types(), expectation.type) && walker.currentBodyIndex().isParameter(input) && walker.currentBodyIndex().blockOf(input) == entryBlock && walker.mayLeaveCopyForCall() ? walker.expectedOfArgumentOfCopy(input.index()) : nullptr;
    if (ofArgument)
        return { ofArgument->type, ofArgument->isDeclared };
    return unlessEveryTurnWouldAskItForGuesses(walker, input, expectation);
}

static ExpectationForGuesses findExpectationForGuesses(Walker& walker, ValueID input)
{
    ExpectationForGuesses expectation = findExpectationAsRememberedForGuesses(walker, input);
    if (walker.someCopyExpectsSomethingOfArguments()) [[unlikely]]
        return orWhatIsExpectedOfArgumentOfCopyForGuesses(walker, input, expectation);
    return unlessEveryTurnWouldAskItForGuesses(walker, input, expectation);
}

static bool isEntryParameterExpectedToBeObjectForGuesses(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    ValueID behind = index.valueBehindCasts(input);
    if (!behind || !index.isParameter(behind) || index.blockOf(behind) != entryBlock)
        return false;
    TypeID expected = expectationOfEntryParameterForGuesses(walker, behind.index()).type;
    TypeID present = expected ? types.withoutNullish(expected) : TypeID();
    return present && !isNeverType(present) && types.at(present).isObject();
}

void sayWhatIsExpectedOfComparedOperands(Walker& walker, const BodyValue& equality, Draft& draft)
{
    TypeArena& types = walker.types();
    bool someIsNumber = false;
    bool someIsOther = false;
    bool leftIsObject = false;
    bool rightIsObject = false;
    equality.forEachOperand([&](ValueID operand, uint32_t index) {
        TypeID type = provenTypeOfInputForGuesses(walker, operand);
        if (saysNothingForGuesses(types, type))
            type = findExpectationForGuesses(walker, operand).type;
        TypeID present = type ? types.withoutNullish(type) : TypeID();
        if (!present || isNeverType(present) || saysNothingForGuesses(types, present)) {
            if (isEntryParameterExpectedToBeObjectForGuesses(walker, operand))
                (index == OperandIndex::StrictEq::left ? leftIsObject : rightIsObject) = true;
            return;
        }
        (types.at(present).isNumber() ? someIsNumber : someIsOther) = true;
        if (types.at(present).isObject())
            (index == OperandIndex::StrictEq::left ? leftIsObject : rightIsObject) = true;
    });
    draft.setExpectsInt32s(someIsNumber && !someIsOther);
    draft.setExpectsObjectOnLeft(leftIsObject);
    draft.setExpectsObjectOnRight(rightIsObject && !leftIsObject);
}

LayoutID layoutThatNamesFamily(Walker& walker, TypeID family)
{
    TypeArena& types = walker.types();
    TypeID present = family ? types.withoutNullish(family) : TypeID();
    if (!present || isNeverType(present) || holdsOnlyPrimitives(present))
        return { };
    Type type = types.at(present);
    if (!type.hasOnlyLayouts())
        return { };
    if (holdsOneMember(type.layouts))
        return LayoutID(positionOf(type.layouts));
    const Program& program = walker.program();
    LayoutSets& sets = types.layoutSets();
    LayoutID widest;
    uint32_t most = 0;
    for (LayoutID member : membersOf(types, type.layouts)) {
        uint32_t passing = program.hasDeclaredMembers(member) ? sets.sizeOf(program.declaredMembersOf(member)) : 0;
        if (passing > most) {
            widest = member;
            most = passing;
        }
    }
    return widest && sets.isSubset(type.layouts, program.declaredMembersOf(widest)) ? widest : LayoutID();
}

uint64_t entryParametersOnlyReadByTwoNamesOrMore(const BodyIndex& body)
{
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(body);
    makeListKeptWithBodyForGuesses(kept, kept.hasEntryParametersOnlyReadByNames, [&] {
        uint32_t numberOfParameters = std::min<uint32_t>(body.block(entryBlock).numberOfParameters(), 64);
        uint64_t usedTwiceOrMore = 0;
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter)
            usedTwiceOrMore |= static_cast<uint64_t>(body.useCount(ValueID(parameter)) >= 2) << parameter;
        if (!usedTwiceOrMore)
            return;
        std::array<uint32_t, 64> reads { };
        for (uint32_t number = numberOfParameters; number < body.numberOfValues(); ++number) {
            ValueID id(number);
            if (body.isParameter(id) || body.opcodeOf(id) != Opcode::PropGet)
                continue;
            if (ValueID object = body.value(id).objectOperand(); object && object.index() < numberOfParameters)
                ++reads[object.index()];
        }
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
            if (reads[parameter] >= 2 && reads[parameter] < BodyIndex::maxCountedUses && reads[parameter] == body.useCount(ValueID(parameter)))
                kept.entryParametersOnlyReadByTwoNamesOrMore |= uint64_t { 1 } << parameter;
        }
    });
    return kept.entryParametersOnlyReadByTwoNamesOrMore;
}

bool recordOfEntryParameterSaysNothing(Walker& walker, FunctionID callee, uint32_t entryParameter)
{
    const LinkTables& link = walker.facts().linked();
    if (callee.index() >= link.functions.size())
        return true;
    std::span<const LinkedParameter> parameters = link.parametersOf(callee);
    return entryParameter >= parameters.size() || saysNothingForGuesses(walker.types(), parameters[entryParameter].writtenOrSeenToArrive());
}

ExpectationOfValue findExpectation(Walker& walker, ValueID input)
{
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, input);
    return { expectation.type, expectation.isDeclared };
}

static ALWAYS_INLINE FunctionID onlyFunctionBesideBuiltinsAndInstancesForGuesses(const TypeArena& types, const Type& type)
{
    if (!type.hasObjects() || !type.isJavaScriptValue() || !type.objects || type.anyObject || type.anyFunction || type.unnamedClasses || type.hasOnlyClosures())
        return { };
    const ObjectTable& objects = types.objects();
    FunctionID only;
    bool namesNothingElse = TypeReader(types).at(type.objects).allOf([&](ObjectID object) {
        FunctionID function = objects.closureFunction(object);
        if (!function)
            return objects.builtinOf(object) || objects.kindOf(object) == ObjectKind::Instance;
        if (only && only != function)
            return false;
        only = function;
        return true;
    });
    return namesNothingElse ? only : FunctionID();
}

static ExpectationForGuesses expectationOfOperandForGuesses(Walker& walker, const BodyValue& operation, ValueID operand)
{
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, operand);
    if (!allNumericOperators.contains(operation.opcode())) {
        if (expectation.type == TypeTable::number && !expectation.isDeclared && !expectation.isHeld && walker.mayBeginTurnAgainInClosedCopy() && !walker.isInLoopOfBodyAtWork()) {
            const KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(walker.currentBodyIndex());
            if (!kept.rootsOfValuesWithMarks.isEmpty() && std::ranges::binary_search(kept.rootsThatOperatorsOutsideLoopsReckonWith, kept.rootOf(operand.index())))
                return { };
        }
        return expectation;
    }
    TypeArena& types = walker.types();
    if (expectation.type && !expectation.isHeld)
        return expectation;
    TypeID proven = provenTypeOfInputForGuesses(walker, operand);
    if (expectation.type && types.at(types.intersectionOf(proven, expectation.type)).isNumber())
        return expectation;
    Type held = types.at(proven);
    bool holdsBigIntsBesideNumbers = held.hasOnlyPrimitives(Primitive::Number | Primitive::BigInt) && (held.primitives & Primitive::BigInt);
    bool holdsOtherPrimitivesInLoop = walker.isInLoop() && !walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) && held.hasOnlyPrimitives(Primitive::NumberLike | Primitive::AnyString) && !held.isNumber();
    if (!(held.primitives & Primitive::Number) || (!holdsBigIntsBesideNumbers && !holdsOtherPrimitivesInLoop))
        return expectation;
    bool standsBesideNumber = false;
    operation.forEachOperand([&](ValueID other, uint32_t) {
        Type beside = types.at(provenTypeOfInputForGuesses(walker, other));
        standsBesideNumber |= other != operand && (beside.isNumber() || (!holdsBigIntsBesideNumbers && !beside.isNever() && beside.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) && (beside.primitives & Primitive::Number)));
    });
    return standsBesideNumber ? ExpectationForGuesses { TypeTable::number, false } : expectation;
}

static bool isContainerKeptInConstantOfItsOwnFile(const Program& program, const BodyIndex& index, ValueID receiver, LayoutID layout)
{
    ValueID read = index.valueBehindCasts(receiver);
    while (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = index.valueBehindCasts(index.value(read).operand(0));
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
        return false;
    const Variable& variable = program.at(index.value(read).variable());
    return program.at(layout).kind == LayoutKind::Container && program.at(layout).module == variable.module && variable.flags.contains(VariableFlags::IsImmutable);
}

static ALWAYS_INLINE FunctionID functionWrittenForFieldThatCalleeIsReadOutOf(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call)
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID called = index.valueBehindCasts(call.calleeOperand());
    if (!called || index.isParameter(called) || index.opcodeOf(called) != Opcode::PropGet || index.value(called).isFreeName())
        return { };
    BodyValue read = index.value(called);
    if (!walker.plan().someFieldUnderNameIsWrittenAsOneFunction(keyOfAccessToSlot(walker.program(), read)))
        return { };
    TypeArena& types = walker.types();
    ExpectationForGuesses ofReceiver = findExpectationForGuesses(walker, read.objectOperand());
    if (!ofReceiver.isDeclared)
        return { };
    TypeID objects = ofReceiver.type ? types.withoutNullish(ofReceiver.type) : TypeID();
    if (!objects || isNeverType(objects) || !types.at(objects).hasOnlyLayouts() || !holdsOneMember(types.at(objects).layouts))
        return { };
    if (ofReceiver.isHeld && !isContainerKeptInConstantOfItsOwnFile(walker.program(), index, read.objectOperand(), membersOf(types, types.at(objects))[0]))
        return { };
    std::optional<FieldReference> field = findField(walker.program(), membersOf(types, types.at(objects))[0], keyOfAccessToSlot(walker.program(), read));
    if (!field)
        return { };
    const DeclaredType& declared = walker.program().at(field->layout).fields[field->field].declared;
    FunctionID written = declared.isExpected() && declared.written() ? singleFunctionOf(types, types.at(declared.written())) : FunctionID();
    return written && walker.facts().linked().mayBeMentionedBy(walker.unit(), written) ? written : FunctionID();
}

static FunctionID functionThatVariableReadIsBornWithForGuesses(const Walker& walker, ValueID callee)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(callee);
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
        return { };
    const Variable& variable = walker.program().at(index.value(read).variable());
    const LinkTables& linked = walker.facts().linked();
    if (variable.initialization != VariableInitialization::Function || (!linked.function(variable.function).isInQueueFromStart && !linked.mayBeMentionedBy(walker.unit(), variable.function)))
        return { };
    return variable.function;
}

static ALWAYS_INLINE FunctionID functionThatVariableReadTakesOutOfRequiredFileForGuesses(const Walker& walker, ValueID callee)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(callee);
    if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = index.valueBehindCasts(index.value(read).operand(0));
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
        return { };
    const LinkTables& linked = walker.facts().linked();
    FunctionID taken = linked.functionTakenOutOfRequiredFileBy(index.value(read).variable());
    return taken && linked.mayBeMentionedBy(walker.unit(), taken) ? taken : FunctionID();
}

static ALWAYS_INLINE FunctionID functionLaidUnderNameThatCalleeIsReadUnderForGuesses(const Walker& walker, ValueID callee)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(callee);
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    ValueID holder = index.valueBehindCasts(index.value(read).as<Opcode::PropGet>().objectOperand());
    if (holder && !index.isParameter(holder) && index.opcodeOf(holder) == Opcode::CheckInitialized)
        holder = index.valueBehindCasts(index.value(holder).operand(0));
    if (!holder || index.isParameter(holder) || index.opcodeOf(holder) != Opcode::GetModuleVariable)
        return { };
    const LinkTables& linked = walker.facts().linked();
    FunctionID laid = linked.functionLaidUnderNameOf(index.value(holder).variable(), index.value(read).as<Opcode::PropGet>().key());
    return laid && linked.mayBeMentionedBy(walker.unit(), laid) ? laid : FunctionID();
}

static bool builtinClassGivesNameCalledOnUnknownReceiverForGuesses(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call)
        return false;
    auto operation = call.as<Opcode::Call>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(operation.calleeOperand());
    if (!read || !operation.thisValueOperand() || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return false;
    if (index.valueBehindCasts(index.value(read).objectOperand()) != index.valueBehindCasts(operation.thisValueOperand()))
        return false;
    Reference receiver = walker.referenceOf(operation.thisValueOperand());
    if (!receiver || !receiver.isValue() || !saysNothingForGuesses(walker.types(), walker.typeOf(receiver)))
        return false;
    if (TypeID expected = findExpectationForGuesses(walker, operation.thisValueOperand()).type; expected && !saysNothingForGuesses(walker.types(), expected))
        return false;
    return !!findMethodThatSomeClassGivesUnderKey(walker, walker.typeOf(receiver), index.value(read).key(), static_cast<unsigned>(operation.argumentsOperands().size()));
}

static ExpectationForGuesses expectationOfCalleeForGuesses(Walker& walker, const BodyValue& call, uint32_t operandIndex)
{
    if (!walker.has(Mechanism::DirectCalls) || !call.operandUses(operandIndex).contains(OperandUse::Calls))
        return { };
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        return { walker.types().forFunction(callee), false };
    if (walker.isInLoop() && callIsExpectedToEnterTargetOfBoundFunction(walker, call))
        return { walker.types().builtinClass(BuiltinClasses::BoundFunction), false, true };
    if (constructionsOfParentForGuesses.contains(call.opcode())) {
        const Layout& below = walker.program().at(call.layout());
        bool isSeenToArrive = !below.base && below.has(LayoutFlags::HasHeritage);
        LayoutID above = isSeenToArrive ? walker.facts().classOf(call.layout()).prototypeBase : below.base;
        FunctionID constructor = above ? walker.program().at(above).constructor : FunctionID();
        if (!constructor)
            return { };
        TypeID returned = walker.plan().typeHeldOfResult(walker.program(), walker.facts().linked(), constructor);
        if (isClassConstructorOf(walker.program().at(constructor).kind) && (!returned || !walker.types().isSubset(returned, TypeTable::object)))
            return { };
        return { walker.types().forFunction(constructor), !isSeenToArrive, isSeenToArrive };
    }
    if (FunctionID callee = functionThatVariableReadIsBornWithForGuesses(walker, call.operand(operandIndex)))
        return { walker.types().forFunction(callee), true };
    if (FunctionID callee = functionThatVariableReadTakesOutOfRequiredFileForGuesses(walker, call.operand(operandIndex)))
        return { walker.types().forFunction(callee), true };
    if (FunctionID callee = functionLaidUnderNameThatCalleeIsReadUnderForGuesses(walker, call.operand(operandIndex)))
        return { walker.types().forFunction(callee), true };

    if (Reference value = walker.referenceOf(call.operand(operandIndex)); value && value.isValue()) {
        if (FunctionID callee = onlyFunctionBesideBuiltinsAndInstancesForGuesses(walker.types(), walker.types().at(walker.typeOf(value))); callee && walker.facts().linked().mayBeMentionedBy(walker.unit(), callee))
            return { walker.types().forFunction(callee), false };
    }

    if (const LinkedCallThatFindsBuiltin* found = walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), call.id())) {
        Reference value = walker.referenceOf(call.operand(operandIndex));
        if (value && value.isValue() && walker.code().at(value.id()).opcode != Opcode::PropGet && walker.has(Mechanism::BuiltinCalls)) {
            if (found->callee && walker.facts().linked().mayBeMentionedBy(walker.unit(), found->callee))
                return { walker.types().forFunction(found->callee), false, true };
            if (isTestableBuiltin(walker.program().builtins(), found->builtin))
                return { walker.types().forBuiltin(found->builtin), false, true };
        }
    }

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID called = index.valueBehindCasts(call.operand(operandIndex));
    bool isFreeName = called && !index.isParameter(called) && index.opcodeOf(called) == Opcode::PropGet && index.value(called).isFreeName() && call.opcode() == Opcode::Call;
    if (FunctionID written = functionWrittenForFieldThatCalleeIsReadOutOf(walker, call))
        return { walker.types().forFunction(written), true };
    if (FunctionID callee = walker.facts().held().calleeAt(walker.currentFunction(), call.id()); callee && !(builtinClassGivesNameCalledOnUnknownReceiverForGuesses(walker, call)))
        return { walker.types().forFunction(callee), true, true };
    if (!walker.has(Mechanism::BuiltinCalls) || !called || (!index.isParameter(called) && index.opcodeOf(called) == Opcode::PropGet && !isFreeName))
        return { };
    ExpectationForGuesses written = findExpectationForGuesses(walker, call.operand(operandIndex));
    if (!written.type)
        return { };
    TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    Type type = types.at(types.withoutNullish(written.type));
    BuiltinID builtin = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID();
    if (!builtin || !isTestableBuiltin(table, table.canonicalID(builtin)))
        return { };
    if (isFreeName) {
        BuiltinID function = table.canonicalID(builtin);
        bool codesAddresses = function == Builtins::DecodeURI || function == Builtins::DecodeURIComponent || function == Builtins::EncodeURI || function == Builtins::EncodeURIComponent;
        bool hasCodeInLine = std::ranges::any_of(table.overloadsOf(function), [](const BuiltinOverload& row) {
            const BuiltinImplementation& implementation = row.code.implementation;
            return row.mode == BuiltinCallMode::Call && implementation.kind == BuiltinImplementationKind::Inline && implementation.inlineBuiltin && hasEmitter(*implementation.inlineBuiltin);
        });
        std::span<const ValueID> arguments = call.argumentsOperands();
        bool givesNumberBack = function == Builtins::ParseFloat && !arguments.empty() && arguments[0] && types.at(provenTypeOfInputForGuesses(walker, arguments[0])).isNumber();
        if (!codesAddresses && !hasCodeInLine && !givesNumberBack)
            return { };
        return { types.forBuiltin(function), true, written.isHeld };
    }
    return { types.forBuiltin(table.canonicalID(builtin)), written.isDeclared, written.isHeld };
}

static ExpectationForGuesses expectationOfHolderOfPrivateNameForGuesses(Walker& walker, const BodyValue& access, const ExpectationForGuesses& written)
{
    TypeArena& types = walker.types();
    Vector<LayoutID, 8> layouts { access.layout() };
    layouts.append(walker.program().layoutsExpectedBelow(access.layout()));
    TypeID holders = types.forLayouts(layouts.span());
    if (written.type && written.isDeclared && types.isSubset(types.withoutNullish(written.type), holders))
        return written;
    return { holders, true };
}

static ALWAYS_INLINE bool standsBehindTestOfGuess(const Code& code, Reference value, const TestOfGuess& chosen)
{
    const Operation& narrowed = code.at(value.id());
    if (narrowed.opcode != Opcode::Narrowed || !narrowed.testOperand())
        return false;
    const Operation& test = code.at(narrowed.testOperand());
    if (chosen.knownByStructure)
        return test.opcode == Opcode::HasStructureOfClass && test.layout() == chosen.knownByStructure;
    if (chosen.layouts)
        return test.opcode == Opcode::ShapeTest && test.layouts() == chosen.layouts;
    return test.opcode == Opcode::TypeTest && test.testedType() == chosen.tested;
}

enum class ShapesOfLayoutsForGuesses : uint8_t { SomeLayoutInUseHasNone, NoLayoutHasAny, Testable };
static ShapesOfLayoutsForGuesses askEachLayoutForShapesForGuesses(const Walker&, LayoutSetID);

static ShapesOfLayoutsForGuesses shapesOfLayoutsForGuesses(const Walker& walker, LayoutSetID layouts)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::ShapesOfLayouts);
    TypeArena& types = walker.types();
    if (holdsOneMember(layouts))
        return askEachLayoutForShapesForGuesses(walker, layouts);
    if (types.keeps() != ArenaKeeps::UntilReset) {
        if (!Walker::keepsRoundsOver(types, layouts))
            return askEachLayoutForShapesForGuesses(walker, layouts);
        if (std::optional<uint32_t> kept = walker.roundKept(Walker::RoundKeptByWalker::ShapesOfLayouts, layouts, 0))
            return static_cast<ShapesOfLayoutsForGuesses>(*kept);
        ShapesOfLayoutsForGuesses shapes = askEachLayoutForShapesForGuesses(walker, layouts);
        const_cast<Walker&>(walker).keepRound(Walker::RoundKeptByWalker::ShapesOfLayouts, layouts, 0, static_cast<uint32_t>(shapes));
        return shapes;
    }
    TypeArena::SetOfRound round = types.setOfRound(layouts);
    if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, 0))
        return static_cast<ShapesOfLayoutsForGuesses>((*kept)[0]);
    ShapesOfLayoutsForGuesses shapes = askEachLayoutForShapesForGuesses(walker, layouts);
    types.rememberRound(number, 1, 0, round, 0, { static_cast<uint32_t>(shapes) });
    return shapes;
}

static ShapesOfLayoutsForGuesses askEachLayoutForShapesForGuesses(const Walker& walker, LayoutSetID layouts)
{
    const Plan& plan = walker.plan();
    bool someShapePasses = false;
    for (LayoutID layout : membersOf(walker.types(), layouts)) {
        bool hasShape = !plan.shapes().shapesOf(layout).isEmpty();
        if (!hasShape && plan.isInUse(layout))
            return ShapesOfLayoutsForGuesses::SomeLayoutInUseHasNone;
        someShapePasses |= hasShape;
    }
    return someShapePasses ? ShapesOfLayoutsForGuesses::Testable : ShapesOfLayoutsForGuesses::NoLayoutHasAny;
}

static TypeID findLayoutsWithObjectsAboveForGuesses(Walker& walker, LayoutSetID withoutObjects)
{
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    Vector<LayoutID, 8> level;
    for (LayoutID layout : membersOf(types, withoutObjects))
        level.append(layout);
    Vector<LayoutID, 16> seen(level);
    while (!level.isEmpty()) {
        Vector<LayoutID, 8> above;
        auto climbTo = [&](LayoutID layout) {
            if (!layout || seen.contains(layout))
                return;
            seen.append(layout);
            above.append(layout);
        };
        for (LayoutID layout : level) {
            for (LayoutID supertype : program.at(layout).supertypes)
                climbTo(supertype);
            climbTo(program.at(layout).base);
        }

        Vector<LayoutID, 16> writtenAsThose;
        for (LayoutID layout : above) {
            writtenAsThose.append(layout);
            if (!program.hasDeclaredMembers(layout))
                continue;
            for (LayoutID member : membersOf(types, program.declaredMembersOf(layout)))
                writtenAsThose.append(member);
        }
        if (!writtenAsThose.isEmpty()) {
            TypeID found = widenedToFamily(types, walker.facts().families(), types.forLayouts(writtenAsThose.span()));
            Type type = types.at(found);
            if (type.isObject() && type.hasOnlyLayouts() && shapesOfLayoutsForGuesses(walker, type.layouts) != ShapesOfLayoutsForGuesses::NoLayoutHasAny)
                return found;
        }
        level = WTF::move(above);
    }
    return { };
}

static TypeID partOfWrittenSetThatFactoriesFill(Walker& walker, TypeID expected, LayoutSetID set)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::PartOfWrittenSetThatFactoriesFill);
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    if (program.taggedFamilies.isEmpty())
        return expected;
    bool keepsRound = types.keeps() == ArenaKeeps::UntilReset;
    TypeArena::SetOfRound round;
    TypeID filled;
    bool isKnown = false;
    if (keepsRound) {
        round = types.setOfRound(set);
        if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, 0)) {
            filled = TypeID::fromBits((*kept)[0]);
            isKnown = true;
        }
    }
    if (!isKnown) {
        const KeyOrders& orders = walker.plan().keyOrders();
        Vector<LayoutID, 8> members;
        bool everyLayoutIsMember = true;
        for (LayoutID layout : membersOf(types, set)) {
            LayoutID maker = writtenBaseOf(program.at(layout));
            everyLayoutIsMember &= maker && !program.at(layout).constructor && std::ranges::any_of(program.taggedFamilies, [&](const TaggedFamily& family) {
                return family.maker == maker;
            });
            bool someFactoryFills = false;
            for (uint32_t order = 0; order < orders.numberOfOrdersOf(layout) && !someFactoryFills; ++order)
                someFactoryFills = std::ranges::find(orders.sourcesOfFields(layout, order), KeyOrderSource::StoresInSequence) != orders.sourcesOfFields(layout, order).end();
            if (someFactoryFills)
                members.append(layout);
        }
        if (everyLayoutIsMember && !members.isEmpty())
            filled = types.forLayouts(members.span());
        if (keepsRound)
            types.rememberRound(number, 1, filled ? 1 : 0, round, 0, { filled.bits() });
    }
    if (!filled)
        return expected;
    uint16_t primitives = types.at(expected).primitives;
    return primitives ? types.unionOf(typeOfPrimitives(primitives), filled) : filled;
}

static TypeID widenedForTestOfGuess(Walker& walker, TypeID expected)
{
    TypeArena& types = walker.types();
    TypeID widened = widenedToFamily(types, walker.facts().families(), expected, SetsThatNoFamilyHolds::BecomeAnyObject);
    if (widened == expected || !types.at(expected).hasObjects())
        return widened;
    Type objects = types.at(types.withOnlyObjects(expected));
    if (objects.hasOnlyLayouts() && !objects.hasOnlyClosures() && mayAccessThroughSetOfLayouts(walker, objects.layouts))
        return partOfWrittenSetThatFactoriesFill(walker, expected, objects.layouts);
    return widened;
}

static std::optional<TestOfGuess> chooseTestOfTypeAnewForGuesses(Walker& walker, TypeID expected)
{
    TypeArena& types = walker.types();
    TypeID tested = widenedForTestOfGuess(walker, expected);
    if (TypeID neitherUndefinedNorNull = types.withoutNullish(tested); !isNeverType(neitherUndefinedNorNull))
        tested = neitherUndefinedNorNull;
    tested = types.testablePartOf(types.withLiteralStringsWidened(tested));
    if (Type bounded = types.at(tested); (bounded.primitives & Primitive::Integer) && bounded.integers.isSubsetOf(Range::int32()))
        tested = types.withIntegers(tested, Range::int32());

    Type type = types.at(tested);
    if (type.isNever() || !type.isJavaScriptValue() || type.hasUnboundedUnknown() || (type.hasObjects() && type.primitives))
        return std::nullopt;
    if (type.hasOnlyClosures() && !singleFunctionOf(types, type))
        return std::nullopt;

    TestOfGuess chosen { tested, { } };
    if (type.hasOnlyLayouts() && !type.hasOnlyClosures()) {
        ShapesOfLayoutsForGuesses shapes = shapesOfLayoutsForGuesses(walker, type.layouts);
        if (shapes == ShapesOfLayoutsForGuesses::NoLayoutHasAny) {
            TypeID above = findLayoutsWithObjectsAboveForGuesses(walker, type.layouts);
            if (!above)
                return std::nullopt;
            type = types.at(above);
            shapes = shapesOfLayoutsForGuesses(walker, type.layouts);
        }
        if (shapes != ShapesOfLayoutsForGuesses::Testable)
            return std::nullopt;
        chosen = { types.forLayoutSet(type.layouts), type.layouts };
    }
    return chosen;
}

static std::optional<TestOfGuess> chooseTestOfTypeForGuesses(Walker& walker, TypeID expected)
{
    constexpr uint8_t numberOfType = static_cast<uint8_t>(AnswerKeptInArena::TestOfGuess);
    constexpr uint8_t numberOfLayouts = numberOfType + 1;
    TypeArena& types = walker.types();
    if (types.keeps() != ArenaKeeps::UntilReset)
        return chooseTestOfTypeAnewForGuesses(walker, expected);
    std::optional<uint32_t> tested = types.rememberedAnswer(numberOfType, expected.bits(), 0);
    std::optional<uint32_t> layouts = tested ? types.rememberedAnswer(numberOfLayouts, expected.bits(), 0) : std::nullopt;
    if (layouts)
        return TypeID::fromBits(*tested) ? std::optional(TestOfGuess { TypeID::fromBits(*tested), LayoutSetID::fromBits(*layouts) }) : std::nullopt;
    std::optional<TestOfGuess> chosen = chooseTestOfTypeAnewForGuesses(walker, expected);
    types.rememberAnswer(numberOfType, expected.bits(), 0, (chosen ? chosen->tested : TypeID()).bits());
    types.rememberAnswer(numberOfLayouts, expected.bits(), 0, (chosen ? chosen->layouts : LayoutSetID()).bits());
    return chosen;
}

static std::optional<TestOfGuess> chooseTestOfGuess(Walker& walker, Reference value, TypeID expected)
{
    if (!value || !value.isValue() || !expected)
        return std::nullopt;

    TypeArena& types = walker.types();
    std::optional<TestOfGuess> ofType = chooseTestOfTypeForGuesses(walker, expected);
    if (!ofType)
        return std::nullopt;
    TestOfGuess chosen = *ofType;
    if (walker.isHolderThatIsNoBuiltin(value) && types.saysNothing(walker.typeOf(value)) && types.at(chosen.tested).isObject() && types.at(chosen.tested).hasOnlyClassObjects())
        return std::nullopt;

    bool namesClosures = !holdsOnlyPrimitives(chosen.tested) && types.recordOf(chosen.tested).traitsOfNamed.contains(ObjectTrait::IsClosure);
    if (namesClosures && walker.withoutFunctionsThatMayNotBeMentioned(chosen.tested) != chosen.tested)
        return std::nullopt;
    TypeID proven = walker.typeOf(value);
    if (types.isSubset(proven, chosen.tested) || !types.overlap(proven, chosen.tested) || standsBehindTestOfGuess(walker.code(), value, chosen))
        return std::nullopt;
    return chosen;
}

static ALWAYS_INLINE std::optional<TestOfGuess> chooseLookAtStructureOfClassForGuesses(Walker& walker, Reference value, TypeID expected)
{
    if (!value || !value.isValue() || !expected)
        return std::nullopt;
    TypeArena& types = walker.types();
    LayoutID heir = layoutKnownByStructureOfClass(walker, types.withoutNullish(expected));
    if (!heir)
        return std::nullopt;
    TestOfGuess chosen { types.forLayout(heir), { } };
    chosen.knownByStructure = heir;
    TypeID proven = walker.typeOf(value);
    if (types.isSubset(proven, chosen.tested) || !types.overlap(proven, chosen.tested) || standsBehindTestOfGuess(walker.code(), value, chosen))
        return std::nullopt;
    ValueID root = rootOfCellInSweptCode(walker.code(), value.id());
    if (walker.state().isFresh(walker.code().isConstant(root) ? Reference::constant(root) : Reference::value(root)))
        return std::nullopt;
    return chosen;
}

static ALWAYS_INLINE TypeID partWhoseClassesHaveGuardsForGuesses(Walker& walker, TypeID expected)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::PartWhoseClassesHaveGuards);
    TypeArena& types = walker.types();
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || holdsOneMember(type.layouts))
        return expected;
    auto askEachLayout = [&]() -> TypeID {
        const Program& program = walker.program();
        Vector<LayoutID, 16> left;
        bool someIsTakenOut = false;
        for (LayoutID layout : membersOf(types, type.layouts)) {
            if (program.at(layout).kind == LayoutKind::Instance && !walker.facts().isEvaluatedAtMostOnce(declaredLayoutOf(program, layout)))
                someIsTakenOut = true;
            else
                left.append(layout);
        }
        return someIsTakenOut && !left.isEmpty() ? types.forLayouts(left.span()) : TypeID();
    };
    TypeID part;
    if (types.keeps() != ArenaKeeps::UntilReset)
        part = askEachLayout();
    else {
        TypeArena::SetOfRound round = types.setOfRound(type.layouts);
        if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, 0))
            part = TypeID::fromBits((*kept)[0]);
        else {
            part = askEachLayout();
            types.rememberRound(number, 1, part ? 1 : 0, round, 0, { part.bits() });
        }
    }
    return part ? part : expected;
}

static ALWAYS_INLINE TypeID partThatOwnAllocationMakesForGuesses(Walker& walker, const BodyIndex& index, ValueID operand, BlockID block, TypeID expected)
{
    const Function& own = walker.program().at(walker.currentFunction());
    LayoutID ownClass = own.constructedClass;
    if (!expected || !ownClass || !own.has(FunctionFlags::HasThis) || !own.has(FunctionFlags::HasNewTarget) || own.kind == FunctionKind::DerivedConstructor)
        return expected;
    BodyBlock entry = index.block(entryBlock);
    if (entry.indexOfParameter(index.valueBehindCasts(operand)) != own.numberOfEntryParametersBefore(EntryParameter::This))
        return expected;
    TypeArena& types = walker.types();
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || holdsOneMember(type.layouts) || !std::ranges::contains(membersOf(types, type.layouts), ownClass))
        return expected;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasBlocksBehindComparisonsOfNewTarget, [&] {
        ValueID newTarget = entry.parameter(own.numberOfEntryParametersBefore(EntryParameter::NewTarget));
        for (BlockID behind : index.blocksInDominatorOrder()) {
            std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(behind);
            BodyTerminator end = edge ? index.terminator(edge->from) : BodyTerminator();
            if (!edge || edge->targetIndex || end.kind() != TerminatorKind::Branch)
                continue;
            ValueID question = end.as<TerminatorKind::Branch>().conditionOperand();
            if (!question || index.isParameter(question) || index.opcodeOf(question) != Opcode::StrictEq)
                continue;
            ValueID left = index.valueBehindCasts(index.value(question).operand(OperandIndex::StrictEq::left));
            ValueID right = index.valueBehindCasts(index.value(question).operand(OperandIndex::StrictEq::right));
            if (left == newTarget || right == newTarget)
                kept.blocksBehindComparisonsOfNewTarget.append({ behind, left == newTarget ? right : left });
        }
    });
    for (auto [behind, compared] : kept.blocksBehindComparisonsOfNewTarget) {
        if (!index.dominates(behind, block))
            continue;
        Reference closure = walker.referenceOf(compared);
        TypeID proven = closure ? walker.typeOf(closure) : TypeID();
        Type ofClosure = proven ? types.at(proven) : Type();
        if (proven && ofClosure.isObject() && ofClosure.isJavaScriptValue() && singleFunctionOf(types, ofClosure) == walker.currentFunction())
            return types.withPrimitives(types.forLayout(ownClass), type.primitives);
    }
    return expected;
}

static TypeID partThatHasNameAskedForGuesses(Walker& walker, TypeID expected, PropertyKeyID name)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::PartThatHasNameAsked);
    TypeArena& types = walker.types();
    if (!expected || !name || liesInThread(name))
        return expected;
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || holdsOneMember(type.layouts))
        return expected;
    auto askEachLayout = [&]() -> TypeID {
        const LayoutsByNames& index = walker.facts().layoutsByNames();
        std::span<const LayoutID> layouts = types.listOf(type.layouts);
        Vector<LayoutID, 16> holders;
        index.appendHoldersAmong(name, layouts, holders);
        const Program& program = walker.program();
        const SlotPlan& slots = walker.plan().slots();
        if (holders.size() > 1 && !slots.fieldAccess(program, name, holders.span(), false).hasLocation) {
            auto formInCell = [&](LayoutID holder) -> std::optional<SlotRepresentation> {
                std::optional<FieldIndex> field = slots.holdsCompiledObjects(holder) ? slots.findField(program, holder, name) : std::nullopt;
                if (!field || slots.slot(holder, *field).storage != SlotStorage::Inline)
                    return std::nullopt;
                return slots.slot(holder, *field).representation;
            };
            Vector<std::pair<SlotRepresentation, uint32_t>, 4> numbersOfHolders;
            for (LayoutID holder : holders) {
                std::optional<SlotRepresentation> form = formInCell(holder);
                if (!form)
                    continue;
                size_t place = numbersOfHolders.findIf([&](const auto& counted) { return counted.first == *form; });
                if (place == notFound)
                    numbersOfHolders.append({ *form, 1 });
                else
                    ++numbersOfHolders[place].second;
            }
            std::optional<SlotRepresentation> formOfMost;
            uint32_t most = 0;
            for (auto [form, numberOfHolders] : numbersOfHolders) {
                if (numberOfHolders > most) {
                    most = numberOfHolders;
                    formOfMost = form;
                }
            }
            holders.removeAllMatching([&](LayoutID holder) {
                return !formOfMost || formInCell(holder) != formOfMost;
            });
        }
        if (holders.isEmpty())
            return { };
        size_t numberInUse = 0;
        for (LayoutID layout : layouts) {
            if (index.isInUse(layout) && ++numberInUse > holders.size())
                break;
        }
        return holders.size() < numberInUse ? types.forLayouts(holders.span()) : TypeID();
    };
    TypeID part;
    if (types.keeps() != ArenaKeeps::UntilReset)
        part = askEachLayout();
    else {
        TypeArena::SetOfRound round = types.setOfRound(type.layouts);
        if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, name.bits()))
            part = TypeID::fromBits((*kept)[0]);
        else {
            part = askEachLayout();
            types.rememberRound(number, 1, part ? 1 : 0, round, name.bits(), { part.bits() });
        }
    }
    return part ? part : expected;
}

static Reference buildTestOfGuess(Walker& walker, Reference value, const TestOfGuess& chosen, Origin origin, bool refusesMarkedObjects = false)
{
    if (chosen.knownByStructure)
        return buildLookAtStructureOfClass(walker, value, chosen.knownByStructure, origin);
    if (chosen.layouts) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(value);
        test.setLayouts(chosen.layouts);
        test.setRefusesMarkedObjects(refusesMarkedObjects);
        return walker.build(WTF::move(test));
    }
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(chosen.tested);
    return walker.build(WTF::move(test));
}

static Reference buildProofOfGuess(Walker& walker, Reference value, Reference test, const TestOfGuess& chosen, Origin origin)
{
    if (chosen.knownByStructure)
        return buildNameBehindLookAtStructureOfClass(walker, value, test, chosen.knownByStructure, origin);
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(test);
    narrowed.setOperandIndex(chosen.layouts ? OperandIndex::ShapeTest::object : OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    Reference proven = walker.build(WTF::move(narrowed));
    if (proven && chosen.layouts)
        walker.state().noteShape(proven, walker.typeOf(proven));
    return proven;
}

bool askWhetherHolderOfStoreInClosedArmOfLoopIsListOfItsOpcodes(Walker& walker, const BodyValue& store)
{
    if (!walker.isInLoop() || walker.unit().role != CodeRole::Body || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::LoopInvariants) || walker.code().at(walker.currentBlock()).frequency == BlockFrequency::Rare)
        return false;
    TypeArena& types = walker.types();
    Reference holder = walker.referenceOf(store.objectOperand());
    Reference key = walker.referenceOf(store.propertyOperand());
    if (!holder || !holder.isValue() || !key || !types.at(walker.typeOf(key)).isInteger() || !saysNothingForGuesses(types, walker.typeOf(holder)))
        return false;
    const BodyIndex& body = walker.currentBodyIndex();
    ValueID named = body.valueBehindCasts(store.objectOperand());
    LoopID loop = body.loopOf(body.blockOf(store.id()));
    bool holderComesFromInFront = named && loop && !body.isInLoop(body.blockOf(named), loop);
    if (walker.opensGuesses() && !holderComesFromInFront)
        return false;
    Walker::ReadWhoseGuessIsOpen opened { walker.depthOfCopies(), store.id() };
    if (walker.readWhoseGuessIsOpen() == opened)
        return false;
    SetForScope isOpen(walker.readWhoseGuessIsOpen(), opened);

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(store.id()));
    uint32_t place = store.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(store);
    origin.isTailCall = false;
    TypeID anyList = types.builtinClass(BuiltinClasses::Array);
    TypeID expected = findExpectationForGuesses(walker, store.objectOperand()).type;
    TypeID listExpected = expected ? types.withOnlyObjects(types.withoutNullish(expected)) : TypeID();
    TestOfGuess chosen { listExpected && !isNeverType(listExpected) && types.isSubset(listExpected, anyList) ? listExpected : anyList, { } };
    Reference isList = buildTestOfGuess(walker, holder, chosen, origin);
    if (!isList || !isList.isValue() || walker.isUnreachable())
        return false;
    BlockID typed = walker.addBlock();
    BlockID asWritten = walker.addBlock(holderComesFromInFront ? BlockFrequency::Normal : BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.branch(isList, typed, asWritten);

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Walker::ArmsThatMeet arms;
    walker.continueIn(asWritten);
    walker.walkStretch(block.id(), place, place + 1);
    walker.listArm(stretch, arms);
    if (walker.hasFailed())
        return true;

    walker.continueIn(typed);
    {
        Reference list = buildProofOfGuess(walker, holder, isList, chosen, origin);
        Walker::StandIn listInPlaceOfHolder(walker, store.objectOperand(), list ? list : holder);
        if (!buildTypedOperation(walker, store))
            walker.copy(store);
    }
    walker.count(StatisticID::StoresInLoopsThatAskWhetherTheirHolderIsAList);
    walker.listArm(stretch, arms);
    if (!walker.hasFailed())
        walker.letArmsMeet(stretch, arms, meeting);
    return true;
}

static TypeID typeAskedOfStoredValueForGuesses(Walker& walker, const BodyValue& store, TypeID typeOfObject, TypeID typeOfValue)
{
    if (!storesIntoSlotsForGuesses.contains(store.opcode()) || !typeOfObject || !typeOfValue)
        return { };
    TypeArena& types = walker.types();
    TypeID taken = typeTakenBySlotOfStore(walker, store, types.withoutNullish(typeOfObject));
    if (!taken || types.isSubset(typeOfValue, taken) || !types.overlap(typeOfValue, taken))
        return { };
    return taken;
}

static bool isOnlyHandedOnForGuesses(const BodyValue& user, uint32_t operandIndex)
{
    return user.opcode() != Opcode::RequireObjectCoercible && user.operandUses(operandIndex).containsOnly({ OperandUse::Retains, OperandUse::Returns });
}

template<typename User>
static bool namesByDefinitionForGuesses(const User& user, ValueID value)
{
    bool namesByDefinition = false;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        namesByDefinition |= operand == value && mustBeDefinition(user.operandKind(operandIndex));
    });
    return namesByDefinition;
}

static bool isWrittenListThatPatternTakesApartForGuesses(const BodyIndex& index, ValueID value, const BodyValue& user)
{
    return user.opcode() == Opcode::IteratorOpen && user.as<Opcode::IteratorOpen>().isDestructuring() && index.opcodeOf(value) == Opcode::NewArray && index.hasOneUse(value);
}

static bool namesSomeOperandByDefinitionForGuesses(Opcode opcode)
{
    static const OpcodeSet operationsThatNameOperandsByDefinition = opcodesWhere([](Opcode opcode) {
        uint32_t numberOfKinds = operandInfosByOpcode[static_cast<unsigned>(opcode)].size;
        for (uint32_t operandIndex = 0; operandIndex < numberOfKinds; ++operandIndex) {
            if (mustBeDefinition(operandKindOf(opcode, operandIndex)))
                return true;
        }
        return false;
    });
    return operationsThatNameOperandsByDefinition.contains(opcode);
}

static bool mustBeNamedByDefinitionForGuesses(const BodyIndex& index, ValueID value, const std::optional<BodyUser>& user)
{
    if (!user)
        return false;
    switch (user->kind) {
    case BodyUseKind::Operand:
        return (namesSomeOperandByDefinitionForGuesses(index.opcodeOf(user->value)) && namesByDefinitionForGuesses(index.value(user->value), value)) || isWrittenListThatPatternTakesApartForGuesses(index, value, index.value(user->value));
    case BodyUseKind::TerminatorOperand:
        return namesByDefinitionForGuesses(index.terminator(user->block), value);
    case BodyUseKind::BlockSubject:
        return true;
    case BodyUseKind::EdgeArgument:
    case BodyUseKind::EdgeProof:
    case BodyUseKind::HandlerArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static NEVER_INLINE bool isReturnedAtOnceByEndOfBlockForGuesses(const BodyIndex& index, ValueID call, const BodyUser& user)
{
    BodyBlock block = index.block(user.block);
    BodyTerminator end = block.terminator();
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call && block.holds(call);
}

static ALWAYS_INLINE bool isReturnedAtOnceForGuesses(const BodyIndex& index, ValueID call, const std::optional<BodyUser>& user)
{
    return user && user->kind == BodyUseKind::TerminatorOperand && isReturnedAtOnceByEndOfBlockForGuesses(index, call, *user);
}

static bool isReturnedAtOnceForGuesses(const BodyIndex& index, ValueID call)
{
    return isReturnedAtOnceForGuesses(index, call, index.firstUser(call));
}

static constexpr OpcodeSet operationsThatMaySuspendForGuesses = opcodesWhere([](Opcode opcode) {
    return maySuspend(opcode);
});

static bool endsEveryStretchOfGuess(Opcode opcode)
{
    if (operationsThatMaySuspendForGuesses.contains(opcode))
        return true;
    return opcodesWithoutEmitter().contains(opcode) && !inputOperationsThatHandOn.contains(opcode);
}

static bool standsInNoStretchOfGuessByItsUsers(const BodyIndex& index, ValueID operation, const std::optional<BodyUser>& firstUser)
{
    if (isReturnedAtOnceForGuesses(index, operation, firstUser))
        return false;
    return mustBeNamedByDefinitionForGuesses(index, operation, firstUser);
}

static bool standsInNoStretchOfGuess(const BodyIndex& index, const BodyValue& operation)
{
    return endsEveryStretchOfGuess(operation.opcode()) || standsInNoStretchOfGuessByItsUsers(index, operation.id(), index.firstUser(operation.id()));
}

static bool mayEndProofOfGuess(Opcode opcode)
{
    static const OpcodeSet operationsThatMayEndProof = opcodesWhere([](Opcode opcode) {
        const Effects& bound = declaredBoundInTable(opcode);
        return !bound.alwaysCompletes() || bound.writes.contains(AbstractHeap::ShapeWords) || operandsMayRunCode(opcode);
    });
    return operationsThatMayEndProof.contains(opcode);
}

static uint32_t placeTakenByEndOfBlockForGuesses(const BodyIndex& index, const BodyBlock& block)
{
    uint32_t place = block.numberOfValues();
    auto keepBehindStretches = [&](ValueID value) {
        if (value && value >= block.firstValue() && block.holds(value))
            place = std::min(place, value.index() - block.firstValue().index());
    };

    BodyTerminator end = block.terminator();
    if (end.isConditionalBranch()) {
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            if (!result.parameter || index.useCount(result.parameter))
                keepBehindStretches(result.test);
        });
    }
    return place;
}

static uint32_t limitOfStretchOfGuess(Walker& walker, const BodyBlock& block)
{
    Walker::PlaceTakenByEndOfBlock& askedLast = walker.placeTakenByEndOfBlockAskedLast();
    if (askedLast.block != block.id())
        askedLast = { block.id(), placeTakenByEndOfBlockForGuesses(walker.currentBodyIndex(), block) };
    uint32_t limit = std::min(askedLast.place, walker.endOfStretchBeingWalked());
    BodyTerminator end = block.terminator();
    ValueID condition = end.isConditionalBranch() ? end.operand(0) : ValueID();
    if (condition && condition >= block.firstValue() && block.holds(condition) && condition.index() - block.firstValue().index() < limit && relationThatNarrowsBehind(walker, end))
        limit = condition.index() - block.firstValue().index();
    return limit;
}

uint32_t limitOfStretchOfArms(Walker& walker, const BodyBlock& block)
{
    return limitOfStretchOfGuess(walker, block);
}

static TypeID typeThatGuessWouldProveForGuesses(Walker& walker, ValueID input)
{
    Reference built = walker.referenceOf(input);
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, input);
    if (!shouldOpenGuess(walker, built, expectation.type, expectation.isDeclared, UINT32_MAX))
        return { };
    std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, built, expectation.type);
    return chosen ? walker.types().intersectionOf(walker.typeOf(built), chosen->tested) : TypeID();
}

static bool allocationFillsAllItsSlotsForGuesses(Walker& walker, const BodyValue& operation, const TypesOfInputs& types)
{
    auto allocation = operation.as<Opcode::NewObject>();
    std::span<const ValueID> values = operation.variadicOperands();
    if (!walker.plan().layout(allocation.layout()).holdsCompiledObjects() || allocation.fields().size() != values.size())
        return false;
    for (size_t index = 0; index < values.size(); ++index) {
        TypeID held = walker.plan().slot(allocation.layout(), allocation.fields()[index]).typeInLayoutRecord();
        if (held && !walker.types().isSubset(types(values[index]), held))
            return false;
    }
    return true;
}

static TypedForm typedFormForGuesses(Walker& walker, const BodyValue& operation, const TypesOfInputs& types, std::optional<BuiltinCallAskedAbout> callOfBuiltin = std::nullopt)
{
    if (operation.opcode() == Opcode::NewObject)
        return allocationFillsAllItsSlotsForGuesses(walker, operation, types) ? TypedForm::Built : TypedForm::None;
    if (!callsOfValuesForGuesses.contains(operation.opcode()))
        return typedFormOf(walker, operation, types);
    if (!walker.has(Mechanism::DirectCalls))
        return TypedForm::None;
    if (walker.has(Mechanism::BuiltinCalls) && (callOfBuiltin ? *callOfBuiltin : wouldCallBuiltin(walker, operation, types)) != BuiltinCallAskedAbout::None)
        return TypedForm::Built;
    FunctionID callee;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operation.operandUses(operandIndex).contains(OperandUse::Calls))
            callee = findOnlyCallee(walker, operation, types(operand));
    });
    if (callee && operation.opcode() == Opcode::SuperConstructList && !listOfSuperCallCanBeHandedOver(walker, operation, callee))
        return TypedForm::None;
    return callee ? TypedForm::Built : TypedForm::None;
}

static ValueID onlyUserBehindWhatWireExpectsForGuesses(const BodyIndex& index, ValueID value)
{
    ValueID user = index.onlyUsingValue(value);
    bool isExpectation = user && index.opcodeOf(user) == Opcode::Convert && index.value(user).as<Opcode::Convert>().declared().isExpected();
    return isExpectation ? index.onlyUsingValue(user) : user;
}

static bool isReadOfMethodForItsCallForGuesses(const BodyIndex& index, const BodyValue& read)
{
    if (read.opcode() != Opcode::PropGet)
        return false;
    ValueID user = onlyUserBehindWhatWireExpectsForGuesses(index, read.id());
    return user && callsOfValuesForGuesses.contains(index.opcodeOf(user)) && !constructionsOfParentForGuesses.contains(index.opcodeOf(user)) && index.valueBehindCasts(index.value(user).calleeOperand()) == read.id();
}

static bool readsFunctionWrittenForFieldApartFromItsCallForGuesses(Walker& walker, const BodyBlock& block, const BodyValue& readOfMethodForItsCall)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID call = onlyUserBehindWhatWireExpectsForGuesses(index, readOfMethodForItsCall.id());
    return !block.holds(call) && functionWrittenForFieldThatCalleeIsReadOutOf(walker, index.value(call));
}

static bool callOfMethodIsHandedClosureMadeForItForGuesses(const BodyIndex& index, const BodyValue& read)
{
    if (!isReadOfMethodForItsCallForGuesses(index, read))
        return false;
    BodyValue call = index.value(onlyUserBehindWhatWireExpectsForGuesses(index, read.id()));
    if (call.opcode() != Opcode::Call)
        return false;
    for (ValueID argument : call.as<Opcode::Call>().argumentsOperands()) {
        if (argument && !index.isParameter(argument) && index.opcodeOf(argument) == Opcode::NewClosure && index.onlyUsingValue(argument) == call.id())
            return true;
    }
    return false;
}

static uint32_t mostOperationsThatStretchCountsForGuesses(Walker& walker, Reference value, bool isPassedAlready, const BodyBlock& block, uint32_t begin, uint32_t limit, uint32_t enough)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t first = block.firstValue().index() + begin;
    BitVector mayRestOnProof;
    uint32_t most = 0;
    ValueID callAwaited;
    for (uint32_t place = begin; place < limit && most < enough; ++place) {
        BodyValue operation = walker.asWalked(index.value(ValueID(first + place - begin)));
        Opcode opcode = operation.opcode();
        bool isSpreadOfListAsMade = opcode == Opcode::AppendSpread && spreadAppendsElementsOfListAsMade(walker, operation.as<Opcode::AppendSpread>().iterableOperand());
        if (!isSpreadOfListAsMade && endsEveryStretchOfGuess(opcode))
            break;
        bool takesWhatMayRestOnProof = false;
        if (opcode != Opcode::ReleaseHint) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                takesWhatMayRestOnProof |= operand.index() >= first ? mayRestOnProof.get(operand.index() - first) : walker.referenceOf(operand) == value;
            });
        }
        if (!takesWhatMayRestOnProof) {
            bool standsInFrontOfCallAwaited = callAwaited && operation.id() < callAwaited && !isReadOfMethodForItsCallForGuesses(index, operation);
            if (mayEndProofOfGuess(opcode) && !operationsWhoseTypedFormsRunNoCodeForGuesses.contains(opcode) && !isSpreadOfListAsMade && !standsInFrontOfCallAwaited)
                break;
            continue;
        }
        mayRestOnProof.set(place - begin);
        bool readsMethodForItsCall = opcode == Opcode::PropGet && isReadOfMethodForItsCallForGuesses(index, operation);
        if (readsMethodForItsCall) {
            ValueID call = onlyUserBehindWhatWireExpectsForGuesses(index, operation.id());
            if (block.holds(call) && call.index() - block.firstValue().index() < limit && (!callAwaited || callAwaited < call))
                callAwaited = call;
        }
        if (opcode != Opcode::RequireObjectCoercible && (!readsMethodForItsCall || readsFunctionWrittenForFieldApartFromItsCallForGuesses(walker, block, operation)))
            most += opcode == Opcode::Construct || isPassedAlready ? 2 : 1;
    }
    return most;
}

static StretchOfGuess findStretchOfGuess(Walker& walker, Reference value, const TestOfGuess& chosen, const BodyBlock& block, uint32_t begin, uint32_t limit, const FirstOperationOfGuessAsItStands* firstAsItStands = nullptr)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypingContext typing { walker.program(), types, walker.positions(), TypeLevel::Proven };
    TypeID typeOfValue = walker.typeOf(value);
    TypeID typeBehindTest = types.intersectionOf(typeOfValue, chosen.tested);
    uint32_t first = block.firstValue().index() + begin;

    ValueID nameOfValueFoundLast;
    ValueID otherInputFoundLast;
    TypeID typeOfOtherInputFoundLast;
    auto namesValueOfGuess = [&](ValueID operand) {
        if (operand == nameOfValueFoundLast)
            return true;
        if (operand == otherInputFoundLast)
            return false;
        if (walker.referenceOf(operand) == value) {
            nameOfValueFoundLast = operand;
            return true;
        }
        otherInputFoundLast = operand;
        typeOfOtherInputFoundLast = { };
        return false;
    };
    auto provenTypeOfInput = [&](ValueID operand) -> TypeID {
        if (namesValueOfGuess(operand))
            return typeOfValue;
        if (!typeOfOtherInputFoundLast)
            typeOfOtherInputFoundLast = provenTypeOfInputForGuesses(walker, operand);
        return typeOfOtherInputFoundLast;
    };

    Vector<std::pair<ValueID, TypeID>, 2> typesOfGuessesInside;
    BodyValue firstAsWritten = index.value(ValueID(first));
    firstAsWritten.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (isOnlyHandedOnForGuesses(firstAsWritten, operandIndex) || namesValueOfGuess(operand) || !saysNothingForGuesses(types, provenTypeOfInput(operand)))
            return;
        if (TypeID guessed = typeThatGuessWouldProveForGuesses(walker, operand))
            typesOfGuessesInside.append({ operand, guessed });
    });

    ValueID askedByBranch = block.terminator().kind() == TerminatorKind::Branch ? block.terminator().operand(0) : ValueID();
    if (askedByBranch && !index.isParameter(askedByBranch) && index.opcodeOf(askedByBranch) == Opcode::ToBoolean)
        askedByBranch = index.valueBehindCasts(index.value(askedByBranch).operand(0));

    StretchOfGuess stretch { begin, 0, { } };
    Vector<TestOfStoredValueOfGuess, 4>& testsOfStoredValues = stretch.testsOfStoredValues;
    Vector<TypeID, 32> typesInTypedRun;
    Vector<TypeID, 32> typesInClosedArm;
    BitVector restsOnProof;
    BitVector& knownBetter = stretch.placesThatTypedRunKnowsBetter;
    auto typeAmong = [&](const Vector<TypeID, 32>& typesOfStretch, ValueID operand) -> TypeID {
        TypeID type = typesOfStretch[operand.index() - first];
        return type ? type : TypeID(TypeTable::anything);
    };
    auto typeInTypedRun = [&](ValueID operand, uint32_t) -> TypeID {
        if (operand.index() >= first)
            return typeAmong(typesInTypedRun, operand);
        if (namesValueOfGuess(operand))
            return typeBehindTest;
        for (auto [guessedOperand, guessed] : typesOfGuessesInside) {
            if (guessedOperand == operand)
                return guessed;
        }
        TypeID proven = provenTypeOfInput(operand);
        for (const TestOfStoredValueOfGuess& test : testsOfStoredValues) {
            if (test.value == operand)
                proven = types.intersectionOf(proven, test.tested);
        }
        return proven;
    };
    auto typeInClosedArm = [&](ValueID operand, uint32_t) -> TypeID {
        return operand.index() >= first ? typeAmong(typesInClosedArm, operand) : provenTypeOfInput(operand);
    };
    auto typeOfInputInTypedRun = [&](ValueID operand) {
        return typeInTypedRun(operand, 0);
    };
    auto typeOfInputInClosedArm = [&](ValueID operand) {
        return typeInClosedArm(operand, 0);
    };
    TypesOfInputs inputsOfTypedRun(typeOfInputInTypedRun);
    TypesOfInputs inputsOfClosedArm(typeOfInputInClosedArm);


    auto isStoredAtOnceIntoValueOfGuess = [&](ValueID read) {
        ValueID user = index.onlyUsingValue(read);
        if (!user || !block.holds(user) || index.opcodeOf(user) != Opcode::PropSet)
            return false;
        BodyValue store = index.value(user);
        return store.valueOperand() == read && namesValueOfGuess(store.objectOperand());
    };
    bool slotsAreAsFound = true;
    Vector<std::pair<ValueID, PropertyKeyID>, 8> slotsStoredInStretch;
    Vector<std::pair<ValueID, ValueID>, 8> readsAnsweredByLooks;
    using Found = FoundInLongStretchOfGuess;
    std::unique_ptr<Found> tables;
    auto goByTables = [&] {
        tables = makeUniqueWithoutFastMallocCheck<Found>();
        for (auto [holder, key] : slotsStoredInStretch) {
            tables->slotsStored.add(Found::under(holder, key), 0);
            tables->namesStored.add(key.bits(), 0);
        }
        for (auto [read, look] : readsAnsweredByLooks)
            tables->looksThatAnswerReads.set(read.bits(), look.bits());
        for (uint32_t place = 0; place < stretch.firstAccessesToSlots.size(); ++place) {
            const LookAtSlotOfGuess& look = stretch.firstAccessesToSlots[place];
            PropertyKeyID key = keyOfAccessToSlot(walker.program(), walker.asWalked(index.value(look.access)));
            if (look.holderInFrontOfStretch)
                tables->looksAtSlotsOfHoldersInFrontOfStretch.add(Found::under(look.holderInFrontOfStretch, key), place);
            else
                tables->looksAtSlots.add(Found::under(look.holder, key), place);
        }
    };
    auto goByTablesWhereMuchIsFound = [&] {
        if (slotsStoredInStretch.size() + readsAnsweredByLooks.size() + stretch.firstAccessesToSlots.size() > 192) [[unlikely]]
            goByTables();
    };
    ValueID callAwaited;
    ValueID callOfMethodFound;
    for (uint32_t place = begin; place < limit; ++place) {
        BodyValue operation = walker.asWalked(index.value(ValueID(first + place - begin)));
        Opcode opcode = operation.opcode();
        bool isSpreadOfListAsMade = opcode == Opcode::AppendSpread && spreadAppendsElementsOfListAsMade(walker, operation.as<Opcode::AppendSpread>().iterableOperand());
        if (!isSpreadOfListAsMade && endsEveryStretchOfGuess(opcode))
            break;
        std::optional<BodyUser> firstUser = index.firstUser(operation.id());
        if (standsInNoStretchOfGuessByItsUsers(index, operation.id(), firstUser))
            break;

        auto restsOnProofOfGuess = [&](ValueID operand) {
            return operand.index() >= first ? restsOnProof.get(operand.index() - first) : namesValueOfGuess(operand);
        };
        bool takesWhatRestsOnProof = false;
        if (opcode != Opcode::ReleaseHint) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                takesWhatRestsOnProof |= restsOnProofOfGuess(operand);
            });
        }
        bool leans = false;
        bool looksAtWhatNothingIsProvenOf = false;
        bool storesNumberOfTypedRunIntoViewInLoop = false;
        if (takesWhatRestsOnProof && opcode == Opcode::ElemSet && walker.isInLoop() && restsOnProofOfGuess(operation.valueOperand()) && operation.valueOperand() != operation.objectOperand() && operation.valueOperand() != operation.propertyOperand()) {
            TypedArrayType kind = typedArrayTypeOf({ walker.program(), types }, typeInTypedRun(operation.objectOperand(), 0));
            storesNumberOfTypedRunIntoViewInLoop = isTypedView(kind) && hasInlineElementAccess(kind) && types.at(typeInTypedRun(operation.valueOperand(), 0)).isNumber() && !types.at(typeInClosedArm(operation.valueOperand(), 0)).isNumber();
        }
        bool mayCallBuiltin = takesWhatRestsOnProof && callsOfValuesForGuesses.contains(opcode) && walker.has(Mechanism::DirectCalls) && walker.has(Mechanism::BuiltinCalls);
        bool codeOfRowStandsInLine = false;
        BuiltinCallAskedAbout callOfBuiltinInTypedRun = mayCallBuiltin ? wouldCallBuiltin(walker, operation, inputsOfTypedRun, &codeOfRowStandsInLine) : BuiltinCallAskedAbout::None;
        if (takesWhatRestsOnProof) {
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (isOnlyHandedOnForGuesses(operation, operandIndex) && callOfBuiltinInTypedRun != BuiltinCallAskedAbout::ByOneRow && !(storesNumberOfTypedRunIntoViewInLoop && operand == operation.valueOperand()))
                    return;
                if (restsOnProofOfGuess(operand))
                    leans = true;
                else if (opcode != Opcode::StrictEq && saysNothingForGuesses(types, typeInTypedRun(operand, operandIndex)))
                    looksAtWhatNothingIsProvenOf = true;
            });
        }
        if (place > begin && looksAtWhatNothingIsProvenOf)
            leans = false;
        bool handsWhatRestsOnProofToCopy = false;
        if (takesWhatRestsOnProof && place > begin && opcode == Opcode::Call && callOfBuiltinInTypedRun == BuiltinCallAskedAbout::None && walker.has(Mechanism::Inlining) && walker.opensGuesses() && !restsOnProofOfGuess(operation.calleeOperand())) {
            bool someArgumentRestsOnProof = std::ranges::any_of(operation.variadicOperands(), restsOnProofOfGuess);
            FunctionID callee = someArgumentRestsOnProof ? findOnlyCallee(walker, operation, typeInTypedRun(operation.calleeOperand(), 0)) : FunctionID();
            if (callee) {
                Draft site(opcode);
                site.flags = static_cast<uint8_t>(operation.flags());
                copyFields(operation, site, [](ValueID operand) {
                    return Reference::value(operand);
                });
                handsWhatRestsOnProofToCopy = mayCopyCallee(walker, site, callee, PlaceOfCall { walker.currentFunction(), walker.isInLoop(), &index, operation.id() });
            }
        }
        leans |= handsWhatRestsOnProofToCopy;
        bool mayEndProof = mayEndProofOfGuess(opcode);
        bool isTypedEitherWay = !leans && (mayEndProof || operationsThatJoinTexts.contains(opcode)) && operationsWhoseTypedFormsRunNoCodeForGuesses.contains(opcode) && typedFormForGuesses(walker, operation, inputsOfClosedArm) == TypedForm::Built;
        bool endsNoProofOfPrimitive = callAwaited && operation.id() < callAwaited && operationsThatEndNoProofOfPrimitiveForGuesses.contains(opcode) && !types.at(chosen.tested).hasObjects();
        bool appendsElementsOfList = !leans && isSpreadOfListAsMade && callAwaited && operation.id() < callAwaited;
        bool readsOffHolderInFrontOfStretch = !leans && !isTypedEitherWay && place > begin && slotsAreAsFound && opcode == Opcode::PropGet && operation.objectOperand().index() < first
            && !namesValueOfGuess(operation.objectOperand()) && walker.referenceOf(operation.objectOperand()).isValue() && isStoredAtOnceIntoValueOfGuess(operation.id())
            && !types.at(provenTypeOfInput(operation.objectOperand())).mayBeUndefinedOrNull()
            && !(tables ? tables->namesStored.contains(keyOfAccessToSlot(walker.program(), operation).bits()) : slotsStoredInStretch.containsIf([&](const auto& stored) { return stored.second == keyOfAccessToSlot(walker.program(), operation); }))
            && typedFormForGuesses(walker, operation, inputsOfClosedArm) == TypedForm::BuiltBehindFilledSlot;
        isTypedEitherWay |= readsOffHolderInFrontOfStretch;
        bool isReadOfMethod = opcode == Opcode::PropGet && (leans || callOfMethodFound) && isReadOfMethodForItsCallForGuesses(index, operation);
        bool standsInFrontOfCallOfMethodFound = callOfMethodFound && operation.id() < callOfMethodFound && !isReadOfMethod;
        if (!leans && !isTypedEitherWay && !endsNoProofOfPrimitive && !appendsElementsOfList && !standsInFrontOfCallOfMethodFound && mayEndProof)
            break;
        bool readsMethodForItsCall = leans && isReadOfMethod;
        if (readsMethodForItsCall)
            callAwaited = onlyUserBehindWhatWireExpectsForGuesses(index, operation.id());

        bool isAskedAboutAsItStands = firstAsItStands && place == begin && typeBehindTest == typeOfValue && typesOfGuessesInside.isEmpty() && firstAsItStands->words == operation.words();

        size_t numberOfTestsOfStoredValuesInFront = testsOfStoredValues.size();
        if (leans && storesIntoSlotsForGuesses.contains(opcode) && operation.valueOperand().index() < first && walker.referenceOf(operation.valueOperand()).isValue()) {
            if (TypeID asked = isAskedAboutAsItStands ? firstAsItStands->askedOfStoredValue : typeAskedOfStoredValueForGuesses(walker, operation, typeInTypedRun(operation.objectOperand(), 0), typeInTypedRun(operation.valueOperand(), 0)))
                testsOfStoredValues.append({ operation.valueOperand(), asked });
        }

        bool counts = false;
        bool countsForTwo = false;
        bool looksAtCellOfProvenRecord = false;
        bool goesWithWhatItTakes = false;
        bool runsNoCode = false;
        TypeID foundOnObjectAsMade;
        TypeID foundBehindLooks;
        TypeID foundInFilledSlot;
        TypeID givenByTypedForm;
        if (leans) {
            walker.takeFormAskedAhead();
            bool formIsAskedAsItStands = isAskedAboutAsItStands && (testsOfStoredValues.isEmpty() ? !firstAsItStands->askedOfStoredValue : !namesValueOfGuess(operation.valueOperand()));
            TypedForm form = formIsAskedAsItStands ? firstAsItStands->form : typedFormForGuesses(walker, operation, inputsOfTypedRun, mayCallBuiltin ? std::optional(callOfBuiltinInTypedRun) : std::nullopt);
            Walker::FormAskedAhead askedAhead = formIsAskedAsItStands ? firstAsItStands->askedAhead : walker.takeFormAskedAhead();
            if (askedAhead.level == KnowledgeLevel::Expected && !walker.isInLoop() && (opcode != Opcode::ElemGet || place == begin))
                form = TypedForm::None;
            if (askedAhead.opcode == Opcode::IsSlotFilled && place > begin)
                form = TypedForm::None;
            if (form == TypedForm::Built && opcode == Opcode::PropGet && askedAhead.level == KnowledgeLevel::Proven)
                givenByTypedForm = widestPrimitiveResultOf(askedAhead.opcode);
            runsNoCode = form != TypedForm::BuiltAsCall && operationsWhoseTypedFormsRunNoCodeForGuesses.contains(opcode);
            if (chosen.asksForElementOfNumbers && place == begin) {
                form = TypedForm::None;
                counts = true;
                runsNoCode = true;
                givenByTypedForm = TypeTable::number;
            }
            switch (form) {
            case TypedForm::None:
                break;
            case TypedForm::Built:
            case TypedForm::BuiltAsCall: {
                std::optional<BuiltinCallAskedAbout> callOfBuiltinInClosedArm = mayCallBuiltin ? std::optional(wouldCallBuiltin(walker, operation, inputsOfClosedArm)) : std::nullopt;
                counts = place == begin || typedFormForGuesses(walker, operation, inputsOfClosedArm, callOfBuiltinInClosedArm) == TypedForm::None;
                counts |= chosen.asksForElementOfNumbers && opcode == Opcode::ElemSet && restsOnProofOfGuess(operation.valueOperand()) && types.at(typeInTypedRun(operation.valueOperand(), 0)).isNumber();
                counts |= chosen.asksForElementOfNumbers && operatorsThatMakeNumbers.contains(opcode);
                counts |= callOfBuiltinInClosedArm && callOfBuiltinInTypedRun > *callOfBuiltinInClosedArm;
                goesWithWhatItTakes = !counts && form == TypedForm::Built && ((familyOf(opcode) == OperationFamily::Operator && opcode != Opcode::ToString) || storesNumberOfTypedRunIntoViewInLoop);
                countsForTwo |= counts && codeOfRowStandsInLine && callOfBuiltinInClosedArm == BuiltinCallAskedAbout::None && !types.at(chosen.tested).hasObjects();
                if (opcode == Opcode::Construct && callOfBuiltinInTypedRun == BuiltinCallAskedAbout::None) {
                    FunctionID constructor = findOnlyCallee(walker, operation, typeInTypedRun(operation.calleeOperand(), 0));
                    countsForTwo |= constructor && makesThisOfConstruction(walker, constructor);
                }
                countsForTwo |= counts && constructionsOfParentForGuesses.contains(opcode);
                if (counts && opcode == Opcode::CallList && callOfBuiltinInTypedRun == BuiltinCallAskedAbout::None) {
                    FunctionID entered = findOnlyCallee(walker, operation, typeInTypedRun(operation.calleeOperand(), 0));
                    countsForTwo |= entered && listSpreadBehindArgumentsIsRestListOf(walker, operation, entered);
                }
                break;
            }
            case TypedForm::BuiltBehindFilledSlot:
            case TypedForm::BuiltBehindEmptySlot:
            case TypedForm::BuiltBehindLookAtCell:
            case TypedForm::BuiltBehindFilledSlotAndLookAtCell: {
                ValueID object = operation.objectOperand();
                while (object.index() >= first && inputOperationsThatHandOn.contains(index.opcodeOf(object)))
                    object = index.value(object).operand(0);
                ValueID holder;
                if (tables) [[unlikely]] {
                    if (auto found = tables->looksThatAnswerReads.find(object.bits()); found != tables->looksThatAnswerReads.end())
                        holder = ValueID::fromBits(found->value);
                }
                for (auto [read, look] : readsAnsweredByLooks) {
                    if (read == object)
                        holder = look;
                }
                bool isAtHand = object.index() < first ? namesValueOfGuess(object) : holder && form == TypedForm::BuiltBehindFilledSlot;
                counts = slotsAreAsFound && isAtHand;
                PropertyKeyID key = keyOfAccessToSlot(walker.program(), operation);
                bool isStoredInStretch;
                size_t placeOfLook;
                if (tables) [[unlikely]] {
                    isStoredInStretch = tables->slotsStored.contains(Found::under(holder, key));
                    auto found = tables->looksAtSlots.find(Found::under(holder, key));
                    placeOfLook = found == tables->looksAtSlots.end() ? notFound : found->value;
                } else {
                    isStoredInStretch = slotsStoredInStretch.contains(std::pair { holder, key });
                    placeOfLook = stretch.firstAccessesToSlots.findIf([&](const LookAtSlotOfGuess& earlier) {
                        return !earlier.holderInFrontOfStretch && earlier.holder == holder && keyOfAccessToSlot(walker.program(), walker.asWalked(index.value(earlier.access))) == key;
                    });
                }
                bool isLookedAtAlready = form == TypedForm::BuiltBehindLookAtCell || (form != TypedForm::BuiltBehindEmptySlot && isStoredInStretch) || placeOfLook != notFound;
                if (!isLookedAtAlready && storesIntoSlotsForGuesses.contains(opcode) && looksAtSlotOfObjectBeingMade(walker, operation))
                    counts = false;
                if (counts && !isLookedAtAlready) {
                    if (tables) [[unlikely]]
                        tables->looksAtSlots.add(Found::under(holder, key), stretch.firstAccessesToSlots.size());
                    stretch.firstAccessesToSlots.append({ operation.id(), holder });
                    if (!tables)
                        goByTablesWhereMuchIsFound();
                }
                if (counts && (form == TypedForm::BuiltBehindLookAtCell || form == TypedForm::BuiltBehindFilledSlotAndLookAtCell))
                    stretch.storesThatLookAtCell.append(operation.id());
                if (counts && form == TypedForm::BuiltBehindLookAtCell)
                    foundBehindLooks = typeFoundBehindLooks(walker, operation, typeBehindTest);
                looksAtCellOfProvenRecord = counts && form == TypedForm::BuiltBehindLookAtCell && chosen.isPassedAlready && opcode == Opcode::PropGet && types.at(typeBehindTest).hasOnlyLayouts();
                if (foundBehindLooks && isStoredInStretch && !types.at(foundBehindLooks).isObject()) {
                    foundBehindLooks = TypeTable::anything;
                    for (uint32_t earlier = place; earlier-- > begin;) {
                        BodyValue store = walker.asWalked(index.value(ValueID(first + earlier - begin)));
                        if (store.opcode() != Opcode::PropSet || store.key() != key)
                            continue;
                        if (TypeID stored = store.objectOperand().index() < first && namesValueOfGuess(store.objectOperand()) ? typeInTypedRun(store.valueOperand(), 0) : TypeID())
                            foundBehindLooks = stored;
                        break;
                    }
                }
                if (counts && storesIntoSlotsForGuesses.contains(opcode)) {
                    if (tables) [[unlikely]] {
                        tables->slotsStored.add(Found::under(holder, key), 0);
                        tables->namesStored.add(key.bits(), 0);
                    } else
                        slotsStoredInStretch.append({ holder, key });
                }
                if (counts && form == TypedForm::BuiltBehindFilledSlot && readsOutOfSlotsForGuesses.contains(opcode) && !isStoredInStretch) {
                    ValueID look = placeOfLook != notFound ? stretch.firstAccessesToSlots[placeOfLook].access : operation.id();
                    if (tables) [[unlikely]]
                        tables->looksThatAnswerReads.set(operation.id().bits(), look.bits());
                    else {
                        readsAnsweredByLooks.append({ operation.id(), look });
                        goByTablesWhereMuchIsFound();
                    }
                    foundInFilledSlot = typeFoundInFilledSlot(walker, operation, types.withoutNullish(typeInTypedRun(operation.objectOperand(), 0)));
                    if (chosen.isPassedAlready && saysNothingForGuesses(types, foundInFilledSlot) && readsMethodForItsCall) {
                        TypeID seenToArrive = typeSeenToArriveInFilledSlot(walker, operation, types.withoutNullish(typeInTypedRun(operation.objectOperand(), 0)));
                        TypeID function = seenToArrive ? types.withOnlyObjects(seenToArrive) : TypeID();
                        FunctionID seen = function && !isNeverType(function) && types.at(function).isObject() ? singleFunctionOf(types, types.at(function)) : FunctionID();
                        if (seen && walker.facts().linked().mayBeMentionedBy(walker.unit(), seen)) {
                            foundInFilledSlot = function;
                            stretch.firstAccessesToSlots[placeOfLook != notFound ? placeOfLook : stretch.firstAccessesToSlots.size() - 1].calleeSeenToArrive = function;
                        }
                    }
                    TypeID functions = foundInFilledSlot ? types.withoutNullish(foundInFilledSlot) : TypeID();
                    if (functions && functions != foundInFilledSlot && !isNeverType(functions) && types.at(functions).hasOnlyClosures() && readsMethodForItsCall) {
                        foundInFilledSlot = functions;
                        stretch.firstAccessesToSlots[placeOfLook != notFound ? placeOfLook : stretch.firstAccessesToSlots.size() - 1].isCalled = true;
                    }
                }
                break;
            }
            case TypedForm::BuiltOnObjectAsMade: {
                ValueID object = operation.objectOperand();
                while (object.index() >= first && inputOperationsThatHandOn.contains(index.opcodeOf(object)))
                    object = index.value(object).operand(0);
                counts = slotsAreAsFound && object.index() < first && namesValueOfGuess(object);
                if (counts) {
                    stretch.looksWhetherValueIsAsMade = true;
                    stretch.restsOnMoreThanLengthOfView |= opcode != Opcode::PropGet || operation.key() != walker.program().keyOf(BuiltinNames::length) || !lengthOfViewTakesInstancesBelow(walker, types.withoutNullish(typeBehindTest));
                    foundOnObjectAsMade = typeFoundOnObjectAsMade(walker, operation, typeBehindTest);
                    if (walker.program().admitsReplacedBuiltins) {
                        BuiltinID member = findMemberOnObjectAsMade(walker, operation, typeBehindTest);
                        HoldersBelowMember below;
                        BeliefInMemberAbove belief = member ? beliefInMemberFoundOnObjectAsMade(walker, member, typeBehindTest, operation.key(), &below) : BeliefInMemberAbove::RestsOnByteOfClass;
                        stretch.someMemberRestsOnByteOfClass |= belief == BeliefInMemberAbove::RestsOnByteOfClass;
                        if (belief == BeliefInMemberAbove::IsAskedByItsByte)
                            stretch.membersAskedByTheirBytes.appendIfNotContains(member);
                        for (BuiltinID holder : below)
                            stretch.holdersAskedForAddedNames.appendIfNotContains(holder);
                    }
                    countsForTwo = chosen.isPassedAlready && readOffObjectAsMadeCallsGetter(walker, operation, typeBehindTest);
                }
                break;
            }
            }
        }
        counts |= handsWhatRestsOnProofToCopy;
        if (storesIntoSlotsForGuesses.contains(opcode)) {
            PropertyKeyID key = keyOfAccessToSlot(walker.program(), operation);
            if (tables) [[unlikely]] {
                tables->slotsStored.add(Found::under(operation.objectOperand(), key), 0);
                tables->namesStored.add(key.bits(), 0);
            } else {
                slotsStoredInStretch.append({ operation.objectOperand(), key });
                goByTablesWhereMuchIsFound();
            }
        }
        if (readsOffHolderInFrontOfStretch) {
            PropertyKeyID key = keyOfAccessToSlot(walker.program(), operation);
            bool isLookedAtAlready = tables ? tables->looksAtSlotsOfHoldersInFrontOfStretch.contains(Found::under(operation.objectOperand(), key)) : stretch.firstAccessesToSlots.containsIf([&](const LookAtSlotOfGuess& earlier) {
                return earlier.holderInFrontOfStretch == operation.objectOperand() && keyOfAccessToSlot(walker.program(), walker.asWalked(index.value(earlier.access))) == key;
            });
            if (!isLookedAtAlready && tables) [[unlikely]]
                tables->looksAtSlotsOfHoldersInFrontOfStretch.add(Found::under(operation.objectOperand(), key), stretch.firstAccessesToSlots.size());
            if (!isLookedAtAlready) {
                stretch.firstAccessesToSlots.append({ operation.id(), { }, operation.objectOperand() });
                if (!tables)
                    goByTablesWhereMuchIsFound();
            }
            foundInFilledSlot = typeFoundInFilledSlot(walker, operation, types.withoutNullish(typeInClosedArm(operation.objectOperand(), 0)));
        }
        if (!counts)
            testsOfStoredValues.shrink(numberOfTestsOfStoredValuesInFront);
        if (place == begin && !counts)
            return stretch;
        if (mayEndProof && !(counts && runsNoCode) && !isTypedEitherWay)
            slotsAreAsFound = false;

        bool isReadInStretch = firstUser && (firstUser->kind == BodyUseKind::Operand ? firstUser->value.index() - first < limit - begin : !index.hasOneUse(operation.id()));
        std::optional<Draft> draftOfRow;
        TypeID inClosedArm = isReadInStretch ? typeByRowForGuesses(typing, operation, typeInClosedArm, draftOfRow) : TypeID();
        bool formSaysWhatTypedRunGives = foundOnObjectAsMade || foundBehindLooks || foundInFilledSlot || givenByTypedForm;
        TypeID inTypedRun = isReadInStretch && takesWhatRestsOnProof && !formSaysWhatTypedRunGives ? typeByRowForGuesses(typing, operation, typeInTypedRun, draftOfRow) : inClosedArm;
        if (foundOnObjectAsMade)
            inTypedRun = foundOnObjectAsMade;
        if (foundBehindLooks)
            inTypedRun = foundBehindLooks;
        if (foundInFilledSlot)
            inTypedRun = foundInFilledSlot;
        if (givenByTypedForm)
            inTypedRun = givenByTypedForm;
        typesInClosedArm.append(inClosedArm);
        typesInTypedRun.append(inTypedRun);
        if (inTypedRun && inTypedRun != inClosedArm && !saysNothingForGuesses(types, inTypedRun))
            knownBetter.set(place - begin);
        if (counts && operation.id() == askedByBranch && inTypedRun && !isNeverType(inTypedRun) && (types.isSubset(inTypedRun, TypeTable::nullish) || types.at(inTypedRun).isObject()))
            stretch.typedRunDecidesBranch = true;
        if (isReadInStretch && takesWhatRestsOnProof && !saysNothingForGuesses(types, inTypedRun) && !types.isSubset(inClosedArm, inTypedRun))
            restsOnProof.set(place - begin);
        if (opcode == Opcode::RequireObjectCoercible)
            counts = false;
        if (counts && readsMethodForItsCall && inTypedRun && types.at(inTypedRun).isObject() && (types.at(inTypedRun).hasOnlyClosures() || (walker.mayBeginTurnAgainInClosedCopy() && singleBuiltinOf(types, types.at(inTypedRun))))) {
            ValueID call = onlyUserBehindWhatWireExpectsForGuesses(index, operation.id());
            if (block.holds(call) && call.index() - block.firstValue().index() < limit && (!callOfMethodFound || callOfMethodFound < call))
                callOfMethodFound = call;
        }
        bool readsMethodOutOfSlotOfWhatTypedRunProves = readsMethodForItsCall && place > begin && operation.objectOperand().index() >= first && restsOnProofOfGuess(operation.objectOperand());
        if (counts && (!readsMethodForItsCall || readsMethodOutOfSlotOfWhatTypedRunProves || (chosen.layouts && (walker.isInLoop() || readsFunctionWrittenForFieldApartFromItsCallForGuesses(walker, block, operation))) || (looksAtCellOfProvenRecord && walker.isInLoop()))) {
            stretch.numberOfTypedOperations += countsForTwo ? 2 : 1;
            stretch.end = place + 1;
        }
        if (goesWithWhatItTakes && stretch.numberOfTypedOperations) {
            uint32_t reached = std::max(stretch.end, stretch.endOfOperatorsBehind);
            bool standsRightBehind = true;
            for (uint32_t between = reached; between < place; ++between)
                standsRightBehind &= primitiveConstants.contains(index.opcodeOf(ValueID(first + between - begin)));
            if (standsRightBehind)
                stretch.endOfOperatorsBehind = place + 1;
        }
        if (isTypedEitherWay && stretch.numberOfTypedOperations && operationsThatJoinTexts.contains(opcode)) {
            uint32_t reached = std::max(stretch.end, stretch.endOfOperatorsBehind);
            bool goesOnWithJoinOfStretch = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                if (operand.index() < first || operand.index() - first + begin >= reached || !operationsThatJoinTexts.contains(index.opcodeOf(operand)))
                    return;
                goesOnWithJoinOfStretch |= index.onlyUsingValue(operand) == operation.id() && types.at(typeInClosedArm(operand, 0)).isAnyString();
            });
            for (uint32_t between = reached; goesOnWithJoinOfStretch && between < place; ++between)
                goesOnWithJoinOfStretch = primitiveConstants.contains(index.opcodeOf(ValueID(first + between - begin)));
            if (goesOnWithJoinOfStretch) {
                TypeID joined = inClosedArm ? inClosedArm : typeByRowForGuesses(typing, operation, typeInClosedArm, draftOfRow);
                if (joined && types.at(joined).isAnyString())
                    stretch.endOfOperatorsBehind = place + 1;
            }
        }
    }

    stretch.firstAccessesToSlots.removeAllMatching([&](const LookAtSlotOfGuess& look) {
        return look.holderInFrontOfStretch && look.access.index() - first + begin >= stretch.end;
    });
    if (chosen.asksForElementOfNumbers)
        stretch.placesThatRestOnProof = WTF::move(restsOnProof);
    return stretch;
}

static TypeID viewsThatMayComeWhereUseWroteListForGuesses(Walker&, ValueID holder, TypeID expected);
static TypeID listBesideTextInWrittenWordForGuesses(Walker&, ValueID operand);
static TypeID viewsSeenToArriveWhereUseWroteRowOfNumbersForGuesses(Walker&, ValueID holder);

static TestOfGuess secondTestOfGuess(Walker& walker, const TestOfGuess& first)
{
    TestOfGuess second { first.failingSideAsksFor, { } };
    if (Type asked = walker.types().at(second.tested); asked.isObject() && asked.hasOnlyLayouts() && !asked.hasOnlyClosures()) {
        if (std::optional<TestOfGuess> ofShapes = chooseTestOfTypeForGuesses(walker, second.tested))
            second = *ofShapes;
        second.failingSideKeepsSites = true;
    }
    second.typedRunOpensGuesses = first.failingSideIsAsLikely;
    return second;
}

static constexpr AbstractHeapSet heapsUnderNumberReadOutOfListForGuesses { AbstractHeap::ArrayStorage, AbstractHeap::Elements };
static constexpr AbstractHeapSet heapsUnderNumberReadOutOfViewForGuesses { AbstractHeap::TypedArrayStorage, AbstractHeap::TypedArrayElements };

static void noteWhatReadOfViewGivesForGuesses(Walker& walker, const BodyValue& read, Reference view, Reference given)
{
    if (given && !walker.isUnreachable())
        noteKnown(walker, KnownNote::NumberReadOutOfList, view, walker.referenceOf(read.propertyOperand()), 0, LocationMasks(heapsUnderNumberReadOutOfViewForGuesses), given.bits());
}

static void buildReadOfElementOfNumbersForGuesses(Walker& walker, const BodyValue& read, Reference list, Origin origin, BlockID closedBlock)
{
    Reference position = walker.referenceOf(read.propertyOperand());
    std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position);
    BlockID meetingBlock = walker.addBlock();
    Reference number = walker.addParameter(meetingBlock, TypeTable::number, Representation::Double);
    for (ArrayStorage kind : { ArrayStorage::Int32, ArrayStorage::Double }) {
        BlockID otherKind = kind == ArrayStorage::Double ? closedBlock : walker.addBlock();
        Draft hasKind(Opcode::HasArrayStorage, origin);
        walker.chooseConservativeFields(hasKind);
        hasKind.setArrayOperand(list);
        hasKind.setArrayStorage(kind);
        Reference hasStorage = walker.build(WTF::move(hasKind));
        BlockID passed = walker.addBlock();
        walker.branch(hasStorage, passed, otherKind);
        walker.continueIn(passed);
        Draft isPresent(Opcode::IsElementPresent, origin);
        walker.chooseConservativeFields(isPresent);
        isPresent.setArrayOperand(list);
        isPresent.setPositionOperand(position);
        isPresent.setArrayStorage(kind);
        isPresent.setKeyIsIntegral(true);
        if (inside)
            isPresent.setIsInBounds(*inside);
        Reference test = walker.build(WTF::move(isPresent));
        BlockID found = walker.addBlock();
        walker.branchOnTest(test, found, closedBlock);
        walker.continueIn(found);
        std::optional<PassedTest> present = walker.showElementPresentBehindTest(list, position, walker.findPassedTest(test));
        if (!present && !walker.isUnreachable()) {
            walker.fail("a number is read out of a list behind no passed test of its presence"_s);
            return;
        }

        Draft element(Opcode::GetArrayElement, origin);
        walker.chooseConservativeFields(element);
        element.setArrayOperand(list);
        element.setPositionOperand(position);
        element.setArrayStorage(kind);
        element.setLevel(KnowledgeLevel::Proven);
        element.setKeyIsIntegral(true);
        if (present)
            element.setIsPresent(*present);
        Reference loaded = walker.build(WTF::move(element));
        walker.jumpTo(meetingBlock, singleElementSpan(loaded));
        if (walker.hasFailed())
            return;
        if (otherKind != closedBlock)
            walker.continueIn(otherKind);
    }
    walker.continueIn(meetingBlock);
    if (!walker.isUnreachable())
        noteKnown(walker, KnownNote::NumberReadOutOfList, list, position, 0, LocationMasks(heapsUnderNumberReadOutOfListForGuesses), number.bits());
}

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

static bool isTakenBySwitchAloneForGuesses(Walker& walker, BlockID inputBlock, ValueID subject, uint32_t end)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(inputBlock);
    BodyTerminator terminator = block.terminator();
    if (terminator.kind() != TerminatorKind::Switch || terminator.operand(0) != subject || !terminator.numberOfCases() || block.hasHandler())
        return false;
    for (int64_t written : terminator.cases()) {
        if (written != static_cast<int32_t>(written))
            return false;
    }
    bool isTakenElsewhere = false;
    for (uint32_t place = end; place < block.numberOfValues() && !isTakenElsewhere; ++place) {
        index.value(ValueID(block.firstValue().index() + place)).forEachOperand([&](ValueID operand, uint32_t) {
            isTakenElsewhere |= operand == subject;
        });
    }
    for (BodyTarget target : terminator.targets()) {
        for (ValueID argument : target.arguments())
            isTakenElsewhere |= argument == subject;
        isTakenElsewhere = isTakenElsewhere || walker.isLiveAtHeadOf(subject, target.block());
    }
    index.forEachEdgeResult(inputBlock, [&](const BodyEdgeResult& result) {
        if (result.subject != subject || index.isUnused(result.parameter))
            return;
        std::optional<BodyUser> user = index.onlyUser(result.parameter);
        bool onlyCutsHolder = user && user->kind == BodyUseKind::Operand && index.opcodeOf(user->value) == Opcode::NarrowByField && index.value(user->value).operand(OperandIndex::NarrowByField::value) != result.parameter;
        isTakenElsewhere |= !onlyCutsHolder;
    });
    return !isTakenElsewhere;
}

static Reference buildIntegerThatSwitchTakesForGuesses(Walker& walker, const BodyTerminator& terminator, Reference subject, Origin origin)
{
    if (!subject || !subject.isValue() || walker.isUnreachable())
        return { };
    TypeArena& types = walker.types();
    if (types.at(walker.typeOf(subject)).isInt32())
        return subject;
    if (!types.overlap(walker.typeOf(subject), TypeTable::int32))
        return { };
    int64_t takesNoCase = std::numeric_limits<int32_t>::min();
    bool namesZero = false;
    for (bool isNamed = true; isNamed;) {
        isNamed = false;
        for (int64_t written : terminator.cases()) {
            isNamed |= written == takesNoCase;
            namesZero |= !written;
        }
        takesNoCase += isNamed;
    }
    auto int32Written = [&](int64_t integer) {
        Draft written(Opcode::Integer, origin);
        written.setInteger(integer);
        written.representation = Representation::Int32;
        return walker.constant(WTF::move(written));
    };

    TestOfGuess isInt32 { TypeTable::int32, { } };
    Reference test = buildTestOfGuess(walker, subject, isInt32, origin);
    if (!test)
        return { };
    BlockID holdsInt32 = walker.addBlock(BlockFrequency::Rare);
    BlockID holdsNone = walker.addBlock(BlockFrequency::Rare);
    BlockID met = walker.addBlock(BlockFrequency::Rare);
    Reference integer = walker.addParameter(met, TypeTable::int32, Representation::Int32);
    walker.branch(test, holdsInt32, holdsNone);

    walker.continueIn(holdsInt32);
    if (Reference proven = buildProofOfGuess(walker, subject, test, isInt32, origin))
        walker.jumpTo(met, singleElementSpan(proven));

    walker.continueIn(holdsNone);
    if (namesZero && !walker.isUnreachable()) {
        Draft equalsZero(Opcode::StrictEq, origin);
        walker.chooseConservativeFields(equalsZero);
        equalsZero.setOperand(0, subject);
        equalsZero.setOperand(1, int32Written(0));
        equalsZero.setForm(formAllowedForTypedOperator(walker, equalsZero));
        if (Reference isZero = walker.build(WTF::move(equalsZero))) {
            BlockID holdsZero = walker.addBlock(BlockFrequency::Rare);
            BlockID holdsNoZero = walker.addBlock(BlockFrequency::Rare);
            walker.branch(isZero, holdsZero, holdsNoZero);
            walker.continueIn(holdsZero);
            Reference zero = int32Written(0);
            walker.jumpTo(met, singleElementSpan(zero));
            walker.continueIn(holdsNoZero);
        }
    }
    if (!walker.isUnreachable()) {
        Reference none = int32Written(takesNoCase);
        walker.jumpTo(met, singleElementSpan(none));
    }
    walker.continueIn(met);
    return walker.hasFailed() ? Reference() : integer;
}

static bool armsOfGuessTakeBranch(Walker& walker, BlockID inputBlock, uint32_t begin, uint32_t end, Reference value, const BitVector& placesThatTypedRunKnowsBetter, BranchOfGuessAskedAbout* askedAbout)
{
    if (askedAbout && askedAbout->isAsked && askedAbout->end == end && askedAbout->value == value)
        return askedAbout->isTaken;
    bool isTaken = walker.armsTakeBranchForProofOf(inputBlock, begin, end, value, placesThatTypedRunKnowsBetter);
    if (askedAbout)
        *askedAbout = { true, end, value, isTaken };
    return isTaken;
}

static Origin originInFrontOfOperationForGuesses(Walker& walker, const BodyValue& operation)
{
    bool lacksOperand = false;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        lacksOperand |= !walker.referenceOf(operand);
    });
    if (lacksOperand && operation.opcode() != Opcode::ReleaseHint && !walker.isUnreachable())
        walker.fail("an operation takes a value of the body that nothing was built for"_s);
    Origin origin = walker.originOf(operation);
    origin.isTailCall = false;
    return origin;
}

static bool isKeyOfReadOrStoreInBodyForGuesses(const BodyIndex& index, ValueID parameter)
{
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if ((operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet) && operation.propertyOperand() == parameter)
                return true;
        }
    }
    return false;
}

static bool branchAsksForTruthOfValueForGuesses(const BodyIndex& index, BlockID block, ValueID value)
{
    BodyTerminator terminator = index.terminator(block);
    if (terminator.kind() != TerminatorKind::Branch)
        return false;
    ValueID condition = terminator.as<TerminatorKind::Branch>().conditionOperand();
    while (condition && condition != value && !index.isParameter(condition) && (index.opcodeOf(condition) == Opcode::ToBoolean || index.opcodeOf(condition) == Opcode::LogicalNot || index.opcodeOf(condition) == Opcode::Convert))
        condition = index.value(condition).operand(0);
    return condition == value;
}

static bool firstUserAsksForTruthForGuesses(const BodyIndex& index, ValueID read)
{
    std::optional<BodyUser> user = index.firstUser(read);
    while (user && user->value && index.opcodeOf(user->value) == Opcode::Convert)
        user = index.firstUser(user->value);
    if (!user)
        return false;
    if (!user->value)
        return user->block && index.terminator(user->block).kind() == TerminatorKind::Branch;
    return index.opcodeOf(user->value) == Opcode::ToBoolean || index.opcodeOf(user->value) == Opcode::LogicalNot;
}

static bool asksStructureOrFieldOfHeirForGuesses(Walker& walker, Reference value, const TestOfGuess& chosen, const StretchOfGuess& stretch)
{
    if (chosen.knownByStructure)
        return true;
    if (chosen.isPassedAlready)
        return !stretch.storesThatLookAtCell.isEmpty() && layoutKnownByStructureOfClass(walker, walker.types().withoutNullish(chosen.tested));
    if (!value || !value.isValue())
        return false;
    const Code& code = walker.code();
    ValueID read = value.id();
    while (opcodesThatGiveSameValue.contains(code.at(read).opcode))
        read = code.at(read).operand(0);
    return code.at(read).opcode == Opcode::GetPropertyOfHeir;
}

static void openGuessWithTest(Walker& walker, Reference value, const TestOfGuess& chosen, BlockID inputBlock, uint32_t begin, const StretchOfGuess& stretch, BranchOfGuessAskedAbout* branchAskedAbout = nullptr)
{
    if (chosen.admitsAllThatIsExpected && !chosen.failingSideAsksFor && !chosen.failingSideAsksForNothing)
        walker.noteTestSeenInTries(value, ValueID(walker.currentBodyIndex().block(inputBlock).firstValue().index() + begin), chosen.tested);
    const BodyIndex& index = walker.currentBodyIndex();
    Code& code = walker.code();
    uint32_t first = index.block(inputBlock).firstValue().index() + begin;
    uint32_t end = std::max(stretch.end, stretch.endOfOperatorsBehind);
    uint32_t size = end - begin;
    bool takesInCallReturnedAtOnce = size && isReturnedAtOnceForGuesses(index, ValueID(first + size - 1));
    bool failingWayBeginsTurnAgain = chosen.admitsAllThatIsExpected && !chosen.failingSideAsksFor && !chosen.failingSideAsksForNothing && walker.mayBeginTurnAgainInClosedCopy();
    bool asksInFrontBehindStores = !failingWayBeginsTurnAgain && chosen.admitsAllThatIsExpected && !chosen.failingSideAsksFor && !chosen.failingSideAsksForNothing && walker.standsInTurnOfTypedCopy();
    bool asksKindOfValueAndNothingElse = !chosen.layouts && walker.testOfTypeLooksAtNoShape(chosen.tested) && !chosen.knownByStructure && !chosen.isPassedAlready && stretch.firstAccessesToSlots.isEmpty() && !stretch.looksWhetherValueIsAsMade && stretch.storesThatLookAtCell.isEmpty() && stretch.testsOfStoredValues.isEmpty() && !stretch.orderWordAskedFor && stretch.membersAskedByTheirBytes.isEmpty() && !stretch.someMemberRestsOnByteOfClass;
    Walker::TurnOfTypedCopy turnThatAsksInFront = !asksInFrontBehindStores ? Walker::TurnOfTypedCopy::HasDoneNothing : asksKindOfValueAndNothingElse ? Walker::TurnOfTypedCopy::HasRunNoOtherCodeThoughCopiesBegan : Walker::TurnOfTypedCopy::HasRunNoOtherCode;
    Reference valueTested = chosen.isPassedAlready ? Reference() : value;
    bool takesInBranch = !failingWayBeginsTurnAgain && (armsOfGuessTakeBranch(walker, inputBlock, begin, end, valueTested, stretch.placesThatTypedRunKnowsBetter, branchAskedAbout) || (stretch.typedRunDecidesBranch && walker.armsMayTakeBranchOf(inputBlock, end, valueTested)));
    bool failingWayLeavesCopy = !failingWayBeginsTurnAgain && !(takesInBranch && walker.isInLoop()) && chosen.admitsAllThatIsExpected && !chosen.failingSideAsksFor && !chosen.failingSideAsksForNothing && !chosen.failingSideIsAsLikely && walker.mayLeaveCopyForCall();
    BlockID closedArmOfRow = failingWayBeginsTurnAgain || failingWayLeavesCopy ? BlockID() : walker.closedArmOfRowOfShapeTestsAt(ValueID(first));
    failingWayLeavesCopy |= !!closedArmOfRow;
    takesInBranch &= !failingWayLeavesCopy;

    Vector<ValueID, 4> names;
    Vector<uint32_t, 32> mentions;
    mentions.fill(0, size);
    for (uint32_t place = 0; place < size; ++place) {
        index.value(ValueID(first + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand.index() >= first)
                ++mentions[operand.index() - first];
            else if (walker.referenceOf(operand) == value && !names.contains(operand))
                names.append(operand);
        });
    }
    Vector<ValueID, 8> valuesMentionedLater;
    for (uint32_t place = 0; place < size; ++place) {
        uint32_t uses = index.useCount(ValueID(first + place));
        if (uses == BodyIndex::maxCountedUses || uses > mentions[place])
            valuesMentionedLater.append(ValueID(first + place));
    }

    ValueID subjectOfSwitch;
    if (ValueID subject = index.terminator(inputBlock).kind() == TerminatorKind::Switch ? index.terminator(inputBlock).operand(0) : ValueID(); !takesInBranch && subject && valuesMentionedLater.contains(subject) && walker.has(Mechanism::UnboxedNumbers)) {
        bool isLoadedByLook = stretch.firstAccessesToSlots.containsIf([&](const LookAtSlotOfGuess& look) {
            return look.access == subject && !look.holder && !look.holderInFrontOfStretch;
        });
        TypeID found = isLoadedByLook ? typeFoundInFilledSlot(walker, walker.asWalked(index.value(subject)), walker.types().withoutNullish(walker.types().intersectionOf(walker.typeOf(value), chosen.tested))) : TypeID();
        if (found && !isNeverType(found) && walker.types().at(found).isInt32() && isTakenBySwitchAloneForGuesses(walker, inputBlock, subject, end))
            subjectOfSwitch = subject;
    }

    Origin origin = originInFrontOfOperationForGuesses(walker, index.value(ValueID(first)));
    BlockID closedBlock;
    if (failingWayLeavesCopy)
        closedBlock = closedArmOfRow ? closedArmOfRow : walker.blockWhereCopyLeavesForCall();
    Reference lastTest;
    unsigned numberOfTests = 0;
    bool lastTestIsAskedInFront = false;
    bool closedArmIsWalkedHereForWhatMayBeExpected = !failingWayBeginsTurnAgain && asksStructureOrFieldOfHeirForGuesses(walker, value, chosen, stretch);
    auto readsNameThatNoTestedLayoutAnswers = [&] {
        BodyValue read = index.value(ValueID(first));
        if (read.opcode() != Opcode::PropGet || !chosen.layouts)
            return false;
        PropertyKeyID name = read.key();
        return std::ranges::none_of(walker.types().listOf(chosen.layouts), [&](LayoutID member) {
            return walker.plan().slots().findField(walker.program(), member, name) || walker.facts().layoutsByNames().hasAll(member, singleElementSpan(name));
        });
    };
    auto testsForRecordWhereNothingIsKnown = [&] {
        if (!chosen.layouts || walker.types().at(walker.types().withoutNullish(walker.typeOf(value))).hasOnlyLayouts())
            return false;
        return std::ranges::any_of(walker.types().listOf(chosen.layouts), [&](LayoutID member) {
            return walker.program().at(member).kind == LayoutKind::Record;
        });
    };
    auto goOnWhereTestPasses = [&](Reference test, bool passesWithYes = true) {
        lastTest = passesWithYes ? test : Reference();
        ++numberOfTests;
        lastTestIsAskedInFront = (failingWayBeginsTurnAgain || asksInFrontBehindStores) && walker.branchInFrontOfTypedCopy(test, passesWithYes, turnThatAsksInFront, Walker::TurnsThatAsk::SomeBesideCopyWithoutGuess);
        if (lastTestIsAskedInFront) {
            if (asksInFrontBehindStores && !walker.standsInFrontOfTypedCopy(test, Walker::TurnOfTypedCopy::HasRunNoOtherCode))
                walker.count(StatisticID::GuessesInCopiesAskedInFrontBehindStoresOfTurns);
            return;
        }
        BlockID passed = walker.addBlock();
        if (!closedBlock) {
            closedBlock = chosen.failingSideIsAsLikely || closedArmIsWalkedHereForWhatMayBeExpected || (chosen.failingSideAsksForNothing && !walker.isInLoop()) ? walker.addBlock() : walker.addBlock(BlockFrequency::Rare);
            bool isRecordOfWhichNothingIsKnown = !chosen.failingSideKeepsSites && walker.code().at(closedBlock).frequency == BlockFrequency::Rare && walker.isInLoop() && testsForRecordWhereNothingIsKnown();
            if (isRecordOfWhichNothingIsKnown)
                walker.count(StatisticID::FailingSidesOfGuessesAtRecordsThatKeepSites);
            if (chosen.failingSideKeepsSites || isRecordOfWhichNothingIsKnown || (walker.isInLoop() && readsNameThatNoTestedLayoutAnswers()))
                walker.code().markRareBlockThatKeepsSites(closedBlock);
        }
        walker.branch(test, passesWithYes ? passed : closedBlock, passesWithYes ? closedBlock : passed);
        walker.continueIn(passed);
    };

    auto buildTestWhereItMayStand = [&](Reference tested, const TestOfGuess& asked, bool refusesMarkedObjects = false) {
        Walker::InFrontOfTypedCopy inFront(walker, (failingWayBeginsTurnAgain || asksInFrontBehindStores) && walker.standsInFrontOfTypedCopy(tested, turnThatAsksInFront) && (!asked.knownByStructure || walker.turnsOfTypedCopyWriteNothingAboveValues()), turnThatAsksInFront);
        return buildTestOfGuess(walker, tested, asked, origin, refusesMarkedObjects);
    };
    auto buildProofWhereTestWasAsked = [&](Reference tested, Reference test, const TestOfGuess& asked) {
        Walker::InFrontOfTypedCopy inFront(walker, lastTestIsAskedInFront, turnThatAsksInFront);
        return buildProofOfGuess(walker, tested, test, asked, origin);
    };

    Reference proven = value;
    if (!chosen.isPassedAlready) {
        Type known = walker.types().at(walker.typeOf(value));
        bool isBeingMade = std::ranges::any_of(names, [&](ValueID name) {
            return isBeingMadeByBodyBeingRead(walker, name);
        });
        bool refusesMarkedObjects = chosen.layouts && !isBeingMade && (!known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow));
        uint32_t firstNewValue = walker.code().numberOfValues();
        BlockID blockOfTest = walker.currentBlock();
        Reference test = buildTestWhereItMayStand(value, chosen, refusesMarkedObjects);
        if (!test)
            return;
        bool isAskedHere = test.isValue() && test.id().index() >= firstNewValue && walker.code().blockOf(test.id()) == blockOfTest;
        goOnWhereTestPasses(test);
        proven = buildProofWhereTestWasAsked(value, test, chosen);
        if (refusesMarkedObjects && isAskedHere && proven && !walker.isUnreachable())
            walker.state().noteFactsOfCell(proven, FactOfCell::BearsNoMark);
        if (std::optional<uint32_t> orderWord = proven && proven != value && !walker.isUnreachable() ? walker.state().orderWordOf(value) : std::nullopt)
            walker.state().noteOrderWord(proven, *orderWord);
        if ((takesInBranch || failingWayBeginsTurnAgain || failingWayLeavesCopy) && proven && proven.isValue() && value.isValue() && !walker.isUnreachable()) {
            Reference last = value;
            while (Reference checked = walker.state().checkedNameOf(last))
                last = checked;
            if (last != proven)
                walker.state().noteCheckedName(last, proven);
        }
    }
    if (Reference fails = walker.drawWhetherGuessFails(origin))
        goOnWhereTestPasses(fails, false);
    if (stretch.orderWordAskedFor && proven) {
        testOrderWordInFrontOfStores(walker, ValueID(first), proven, *stretch.orderWordAskedFor, scopedLambda<void(Reference)>([&](Reference test) {
            goOnWhereTestPasses(test);
        }));
    } else if (!stretch.storesThatLookAtCell.isEmpty() && proven) {
        Vector<ValueID, 4> accessesThatLookAtSlotsOfValue;
        for (const LookAtSlotOfGuess& look : stretch.firstAccessesToSlots) {
            if (!look.holder && !look.holderInFrontOfStretch)
                accessesThatLookAtSlotsOfValue.append(look.access);
        }
        lookAtCellInFrontOfStores(walker, stretch.storesThatLookAtCell.span(), proven, scopedLambda<void(Reference)>([&](Reference test) {
            goOnWhereTestPasses(test);
        }), accessesThatLookAtSlotsOfValue.span());
    }
    for (const TestOfStoredValueOfGuess& asked : stretch.testsOfStoredValues) {
        Reference stored = walker.isUnreachable() ? Reference() : walker.referenceOf(asked.value);
        if (!stored || !stored.isValue() || walker.types().isSubset(walker.typeOf(stored), asked.tested))
            continue;
        TestOfGuess fitsSlots { asked.tested, { } };
        Reference test = buildTestWhereItMayStand(stored, fitsSlots);
        if (!test)
            break;
        goOnWhereTestPasses(test);
        if (Reference fitting = buildProofWhereTestWasAsked(stored, test, fitsSlots))
            walker.state().noteCheckedName(stored, fitting);
    }
    if (takesInBranch && !chosen.isPassedAlready && !walker.isInLoop() && inputBlock == entryBlock && walker.currentFunction() == walker.unit().function && value.isValue()) {
        BodyBlock entry = index.block(entryBlock);
        for (uint32_t position = 0; position < entry.numberOfParameters(); ++position) {
            ValueID parameter = entry.parameter(position);
            Reference arrives = walker.isUnreachable() ? Reference() : walker.referenceOf(parameter);
            if (!arrives || !arrives.isValue() || arrives == value || !saysNothingForGuesses(walker.types(), walker.typeOf(arrives)))
                continue;
            ExpectationForGuesses expectation = findExpectationForGuesses(walker, parameter);
            if (!expectation.isDeclared || !expectation.type || !walker.types().at(expectation.type).isNumber() || !isKeyOfReadOrStoreInBodyForGuesses(index, parameter))
                continue;
            TestOfGuess isNumber { TypeTable::number, { } };
            Reference test = buildTestWhereItMayStand(arrives, isNumber);
            if (!test)
                break;
            goOnWhereTestPasses(test);
            if (Reference number = buildProofWhereTestWasAsked(arrives, test, isNumber))
                walker.state().noteCheckedName(arrives, number);
        }
    }
    auto askWhetherLoadedValueIsThere = [&](Reference loaded) -> std::optional<Reference> {
        while (Reference checked = loaded ? walker.state().checkedNameOf(loaded) : Reference())
            loaded = checked;
        TypeID held = loaded ? walker.typeOf(loaded) : TypeID();
        TypeID rest = held ? walker.types().withoutNullish(held) : TypeID();
        if (!rest || rest == held || !loaded.isValue())
            return loaded;
        TestOfGuess isNeitherUndefinedNorNull { rest, { } };
        Reference test = buildTestWhereItMayStand(loaded, isNeitherUndefinedNorNull);
        if (!test)
            return std::nullopt;
        goOnWhereTestPasses(test);
        Reference rested = buildProofWhereTestWasAsked(loaded, test, isNeitherUndefinedNorNull);
        if (rested)
            walker.state().noteCheckedName(loaded, rested);
        return rested;
    };
    auto askWhetherLoadedValueIsClosureOf = [&](Reference loaded, TypeID function) {
        while (Reference checked = loaded ? walker.state().checkedNameOf(loaded) : Reference())
            loaded = checked;
        if (!loaded || !loaded.isValue())
            return false;
        if (walker.types().isSubset(walker.typeOf(loaded), function))
            return true;
        TestOfGuess isClosureOfFunction { function, { } };
        Reference test = buildTestWhereItMayStand(loaded, isClosureOfFunction);
        if (!test)
            return false;
        goOnWhereTestPasses(test);
        if (Reference closure = buildProofWhereTestWasAsked(loaded, test, isClosureOfFunction))
            walker.state().noteCheckedName(loaded, closure);
        return true;
    };
    Vector<std::pair<ValueID, Reference>, 4> loadedByLooks;
    Vector<ValueID, 2> holdersAskedForTheirMark;
    BlockID blockOfEmptySlot;
    Reference holderOfEmptySlot;
    ValueID readOfEmptySlot;
    for (const LookAtSlotOfGuess& look : stretch.firstAccessesToSlots) {
        Reference holder = proven;
        if (look.holderInFrontOfStretch) {
            holder = walker.referenceOf(look.holderInFrontOfStretch);
            while (Reference checked = holder ? walker.state().checkedNameOf(holder) : Reference())
                holder = checked;
            auto isLeftToCell = [&](const LookAtSlotOfGuess& other) {
                return other.holderInFrontOfStretch == look.holderInFrontOfStretch && !valueHeldOfFilledSlot(walker, walker.asWalked(index.value(other.access)), holder);
            };
            bool mayAskMark = holder && holder.isValue() && !walker.isUnreachable() && !holdersAskedForTheirMark.contains(look.holderInFrontOfStretch)
                && !walker.state().factsOfCell(holder).contains(FactOfCell::BearsNoMark) && isLeftToCell(look);
            if (mayAskMark && static_cast<size_t>(std::ranges::count_if(stretch.firstAccessesToSlots, isLeftToCell)) >= fewestLooksThatOneTestOfMarkStandsFor
                && markOfCellAnswersForRead(walker, walker.asWalked(index.value(look.access)), walker.typeOf(holder))) {
                holdersAskedForTheirMark.append(look.holderInFrontOfStretch);
                std::optional<TestOfGuess> ofHolder = chooseTestOfTypeForGuesses(walker, walker.typeOf(holder));
                uint32_t firstNewValue = walker.code().numberOfValues();
                BlockID blockOfTest = walker.currentBlock();
                Reference test = ofHolder && ofHolder->layouts ? buildTestOfGuess(walker, holder, *ofHolder, origin, true) : Reference();
                bool isAskedHere = test && test.isValue() && test.id().index() >= firstNewValue && walker.code().blockOf(test.id()) == blockOfTest
                    && walker.code().at(test.id()).opcode == Opcode::ShapeTest && walker.code().at(test.id()).refusesMarkedObjects();
                if (isAskedHere) {
                    goOnWhereTestPasses(test);
                    if (!lastTestIsAskedInFront && !walker.isUnreachable())
                        walker.state().noteFactsOfCell(holder, FactOfCell::BearsNoMark);
                }
            }
        } else if (look.holder) {
            size_t place = loadedByLooks.findIf([&](const auto& loaded) {
                return loaded.first == look.holder;
            });
            holder = place != notFound ? loadedByLooks[place].second : Reference();
            std::optional<Reference> rested = askWhetherLoadedValueIsThere(holder);
            if (!rested)
                break;
            holder = *rested;
        }
        bool asksForFilledSlot = index.opcodeOf(look.access) != Opcode::PrivateDefine;
        Reference loaded = holder && asksForFilledSlot ? valueHeldOfFilledSlot(walker, walker.asWalked(index.value(look.access)), holder) : Reference();
        bool markAnswers = !loaded && numberOfTests && holder && walker.state().factsOfCell(holder).contains(FactOfCell::BearsNoMark) && markOfCellAnswersForRead(walker, walker.asWalked(index.value(look.access)), walker.typeOf(holder));
        Reference test = holder && !loaded && !markAnswers ? buildTestOfSlot(walker, walker.asWalked(index.value(look.access)), holder) : Reference();
        if (!test && !loaded && !markAnswers)
            break;
        bool isCaseInTypedCopy = test && asksForFilledSlot && failingWayBeginsTurnAgain && &look == &stretch.firstAccessesToSlots.last() && !blockOfEmptySlot && !walker.standsInFrontOfTypedCopy(test, turnThatAsksInFront) && firstUserAsksForTruthForGuesses(index, look.access);
        bool branchAsksWhatIsRead = isCaseInTypedCopy && branchAsksForTruthOfValueForGuesses(index, inputBlock, look.access);
        bool armsTakeBranchInTypedCopy = branchAsksWhatIsRead && walker.armsMayTakeBranchOf(inputBlock, end, valueTested) && walker.armsMayEndWithBranchBehind(index.value(ValueID(first + size - 1)));
        isCaseInTypedCopy &= armsTakeBranchInTypedCopy || !branchAsksWhatIsRead;
        if (test)
        if (asksForFilledSlot && (failingWayBeginsTurnAgain ? isCaseInTypedCopy : takesInBranch || !walker.isInLoop()) && &look == &stretch.firstAccessesToSlots.last() && !blockOfEmptySlot && emptySlotIsUndefinedBehindLooks(walker, walker.asWalked(index.value(look.access)), holder)) {
            lastTest = { };
            ++numberOfTests;
            lastTestIsAskedInFront = false;
            BlockID passed = walker.addBlock();
            BlockID empty = walker.addBlock();
            walker.branch(test, passed, empty);
            walker.continueIn(empty);
            walker.letGuessFailUnasked(closedBlock, origin);
            ValueID reads[] = { look.access };
            lookAtCellInFrontOfStores(walker, reads, holder, scopedLambda<void(Reference)>([&](Reference look) {
                BlockID next = walker.addBlock();
                if (!closedBlock)
                    closedBlock = walker.addBlock(BlockFrequency::Rare);
                walker.branch(look, next, closedBlock);
                walker.continueIn(next);
            }));
            if (walker.hasFailed())
                return;
            if (!walker.isUnreachable()) {
                noteSlotFoundEmpty(walker, walker.asWalked(index.value(look.access)), holder);
                blockOfEmptySlot = walker.currentBlock();
                takesInBranch |= armsTakeBranchInTypedCopy;
                holderOfEmptySlot = holder;
                readOfEmptySlot = look.access;
                walker.noteEmptySlotTakenAsCase();
            }
            walker.continueIn(passed);
        } else
            goOnWhereTestPasses(test, asksForFilledSlot);
        if (asksForFilledSlot) {
            if (!loaded) {
                Walker::InFrontOfTypedCopy inFront(walker, test && lastTestIsAskedInFront, turnThatAsksInFront);
                if (markAnswers)
                    walker.checkFactsRememberedOfCell(holder, origin, FactOfCell::BearsNoMark);
                loaded = readSlotFoundFilled(walker, walker.asWalked(index.value(look.access)), holder);
            }
            loadedByLooks.append({ look.access, loaded });
            if (look.calleeSeenToArrive && !askWhetherLoadedValueIsClosureOf(loadedByLooks.last().second, look.calleeSeenToArrive))
                break;
            if (look.isCalled && !askWhetherLoadedValueIsThere(loadedByLooks.last().second))
                break;
        } else
            noteSlotFoundEmpty(walker, index.value(look.access), holder);
    }
    bool isPrimitive = proven && !walker.types().at(walker.typeOf(proven)).hasObjects();
    Reference isAsMade;
    bool takesInstancesBelow = false;
    bool asksMembersByTheirBytes = walker.program().admitsReplacedBuiltins && !stretch.someMemberRestsOnByteOfClass;
    if (stretch.looksWhetherValueIsAsMade && proven && !walker.isUnreachable()) {
        Walker::InFrontOfTypedCopy inFront(walker, isPrimitive && failingWayBeginsTurnAgain && walker.turnsOfTypedCopyWriteNothingAboveValues());
        takesInstancesBelow = walker.isInLoop() && !stretch.restsOnMoreThanLengthOfView && proven.isValue() && lengthOfViewTakesInstancesBelow(walker, walker.code().at(proven.id()).type);
        isAsMade = takesInstancesBelow ? buildTestOfViewOrInstanceBelow(walker, proven, origin) : buildTestOfObjectAsMade(walker, proven, origin, asksMembersByTheirBytes ? TestOfObjectAsMade::AsksObjectAlone : TestOfObjectAsMade::AsksWhatWasWrittenAboveToo);
    }
    if (isAsMade) {
        goOnWhereTestPasses(isAsMade);
        if (takesInstancesBelow) {
            noteThatLengthOfViewIsFoundAbove(walker, proven);
            walker.count(StatisticID::GuessesAtLengthsThatTakeInstancesBelow);
        } else if (!isPrimitive)
            noteListAsMade(walker, proven);
        if (!asksMembersByTheirBytes)
            noteObjectPassedTestOfBeingAsMade(walker, proven, isAsMade);
    }
    for (BuiltinID member : stretch.membersAskedByTheirBytes) {
        Reference test;
        if (!walker.isUnreachable() && !walker.hasFailed()) {
            Walker::InFrontOfTypedCopy inFront(walker, failingWayBeginsTurnAgain && walker.turnsOfTypedCopyWriteNothingAboveValues());
            test = buildTestOfMemberAsBuilt(walker, member, origin);
        }
        if (!test)
            break;
        goOnWhereTestPasses(test);
        noteMemberPassedTestOfBeingAsBuilt(walker, member);
    }
    for (BuiltinID holder : stretch.holdersAskedForAddedNames) {
        Reference test;
        if (!walker.isUnreachable() && !walker.hasFailed()) {
            Walker::InFrontOfTypedCopy inFront(walker, failingWayBeginsTurnAgain && walker.turnsOfTypedCopyWriteNothingAboveValues());
            test = buildTestOfHolderWithoutAddedNames(walker, holder, origin);
        }
        if (!test)
            break;
        goOnWhereTestPasses(test);
        noteHolderPassedTestOfHavingNoAddedNames(walker, holder);
    }
    if (walker.hasFailed())
        return;
    if (chosen.asksForElementOfNumbers && proven && !walker.isUnreachable()) {
        if (!closedBlock)
            closedBlock = walker.addBlock(BlockFrequency::Rare);
        lastTest = { };
        numberOfTests += 2;
        if (chosen.elementInsideView)
            noteWhatReadOfViewGivesForGuesses(walker, walker.asWalked(index.value(ValueID(first))), proven, buildReadOfElementOfViewBehindTestOfLength(walker, walker.asWalked(index.value(ValueID(first))), proven, origin, closedBlock, chosen.updatesElement));
        else
            buildReadOfElementOfNumbersForGuesses(walker, walker.asWalked(index.value(ValueID(first))), proven, origin, closedBlock);
        if (walker.hasFailed())
            return;
    }
    if (!closedBlock && !((failingWayBeginsTurnAgain || asksInFrontBehindStores) && numberOfTests))
        return walker.fail("a guess is opened without a test"_s);
    BlockID typedBlock = walker.isUnreachable() ? BlockID() : walker.currentBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.noteGuessOpened();

    Vector<Walker::ArmThatMeets, 2> arms;
    auto endArm = [&](bool isClosedArm) {
        if (takesInBranch)
            walker.endArmWithBranchOf(inputBlock);
        if (walker.isUnreachable())
            return;
        if (isClosedArm && subjectOfSwitch) {
            if (Reference integer = buildIntegerThatSwitchTakesForGuesses(walker, index.terminator(inputBlock), walker.referenceOf(subjectOfSwitch), origin))
                walker.bind(subjectOfSwitch, integer);
            if (walker.isUnreachable())
                return;
        }
        if (walker.standsInTryOfLoopOfBodyAtWork()) {
            size_t nextMentionedLater = 0;
            for (uint32_t place = 0; place < size; ++place) {
                ValueID sum(first + place);
                bool isMentionedLater = nextMentionedLater < valuesMentionedLater.size() && valuesMentionedLater[nextMentionedLater] == sum;
                nextMentionedLater += isMentionedLater;
                if (index.opcodeOf(sum) == Opcode::Add && !isMentionedLater)
                    walker.noteTypeOfSumLeftInArm(sum);
            }
        }
        Walker::ArmThatMeets arm { walker.currentBlock(), { } };
        for (ValueID mentioned : valuesMentionedLater) {
            Reference built = walker.referenceOf(mentioned);
            if (!built || !built.isRecord())
                return walker.fail("an arm of a guess has no value for what is read behind it"_s);
            arm.values.append(built);
        }
        arms.append(WTF::move(arm));
    };

    if (closedBlock && !failingWayLeavesCopy) {
        walker.continueIn(closedBlock);
        Walker::ClosedArm closedArm(walker, failingWayBeginsTurnAgain);
        SetForScope waysOutKeepSites(walker.closedArmKeepsSites(), walker.isInLoop() && walker.code().at(closedBlock).keepsSitesThoughRare);
        if (numberOfTests == 1 && !chosen.isPassedAlready && !chosen.knownByStructure)
            bindNamesBehindFailedTest(walker, value, lastTest, chosen.layouts ? OperandIndex::ShapeTest::object : OperandIndex::TypeTest::value, names.span(), origin);
        Reference refused = (chosen.failingSideAsksForNothing || chosen.failingSideAsksFor) && !names.isEmpty() && !walker.isUnreachable() ? walker.referenceOf(names[0]) : Reference();
        if (chosen.failingSideAsksFor && refused && refused.isValue() && walker.types().overlap(walker.typeOf(refused), chosen.failingSideAsksFor) && !walker.types().isSubset(walker.typeOf(refused), chosen.failingSideAsksFor))
            openGuessWithTest(walker, refused, secondTestOfGuess(walker, chosen), inputBlock, begin, StretchOfGuess { end, 0, { } });
        else
        if (refused && refused.isValue() && walker.types().overlap(walker.typeOf(refused), TypeTable::nullish) && !walker.types().isSubset(walker.typeOf(refused), TypeTable::nullish))
            openGuessWithTest(walker, refused, TestOfGuess { TypeTable::nullish, { } }, inputBlock, begin, StretchOfGuess { end, 0, { } });
        else
            walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        endArm(true);
    }
    if (walker.hasFailed())
        return;

    if (blockOfEmptySlot) {
        walker.continueIn(blockOfEmptySlot);
        for (uint32_t place = 0; place < size; ++place)
            walker.rememberedExpectationOf(ValueID(first + place)) = { };
        Vector<Reference, 4> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, proven);
        }
        walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        endArm(true);
        for (size_t index = 0; index < names.size(); ++index)
            walker.bind(names[index], boundBefore[index]);
        if (walker.hasFailed())
            return;
    }

    if (typedBlock) {
        walker.continueIn(typedBlock);
        std::optional<Walker::OpenArm> openArm;
        if (chosen.typedRunOpensGuesses)
            openArm.emplace(walker);
        if (chosen.failingSideIsAsLikely || blockOfEmptySlot) {
            for (uint32_t place = 0; place < size; ++place)
                walker.rememberedExpectationOf(ValueID(first + place)) = { };
        }
        Vector<Reference, 4> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, proven);
        }
        std::optional<uint32_t> likelyOrderWord;
        if (!stretch.orderWordAskedFor && !stretch.storesThatLookAtCell.isEmpty() && proven)
            likelyOrderWord = findLikelyOrderWordInFrontOfStores(walker, ValueID(first), stretch.storesThatLookAtCell.span(), proven);
        if (likelyOrderWord) {
            StretchOfGuess inner { end, 0, { } };
            inner.orderWordAskedFor = likelyOrderWord;
            openGuessWithTest(walker, proven, TestOfGuess { walker.typeOf(proven), { }, true }, inputBlock, begin, inner);
        } else
            walker.walkStretch(inputBlock, begin, end);
        if (takesInCallReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        for (ValueID mentioned : valuesMentionedLater) {
            Reference brought = takesInBranch && !walker.isUnreachable() ? walker.referenceOf(mentioned) : Reference();
            TypeID broughtByTypedRun = brought && brought.isValue() ? code.at(brought.id()).type : TypeID();
            if (broughtByTypedRun && !saysNothingForGuesses(walker.types(), broughtByTypedRun) && !findExpectationForGuesses(walker, mentioned).type)
                walker.rememberedExpectationOf(mentioned) = { partThatOneTestTellsForGuesses(walker.types(), broughtByTypedRun), false, false, true };
        }
        endArm(false);
        for (size_t index = 0; index < names.size(); ++index)
            walker.bind(names[index], boundBefore[index]);
    }
    if (walker.hasFailed())
        return;

    size_t nextMentionedLater = 0;
    for (uint32_t place = 0; place < size; ++place) {
        bool isMentionedLater = nextMentionedLater < valuesMentionedLater.size() && valuesMentionedLater[nextMentionedLater] == ValueID(first + place);
        nextMentionedLater += isMentionedLater;
        if (arms.isEmpty() || !isMentionedLater)
            walker.bind(ValueID(first + place), Reference());
    }

    Vector<Reference, 8> met(valuesMentionedLater.size());
    walker.letArmsMeet(arms.span(), meetingBlock, met.mutableSpan());
    for (size_t position = 0; position < met.size() && !arms.isEmpty() && !walker.hasFailed(); ++position) {
        if (met[position] != arms.first().values[position]) {
            TypeID broughtByTypedRun = code.at(arms.last().values[position].id()).type;
            if (saysNothingForGuesses(walker.types(), code.at(met[position].id()).type) && !saysNothingForGuesses(walker.types(), broughtByTypedRun) && !findExpectationForGuesses(walker, valuesMentionedLater[position]).type)
                walker.rememberedExpectationOf(valuesMentionedLater[position]) = { partThatOneTestTellsForGuesses(walker.types(), broughtByTypedRun), false, false, true };
        }
        walker.bind(valuesMentionedLater[position], met[position]);
    }
    bool slotIsAsReadAtMeeting = !!readOfEmptySlot;
    for (uint32_t place = readOfEmptySlot ? readOfEmptySlot.index() + 1 : first + size; place < first + size; ++place)
        slotIsAsReadAtMeeting &= declaredBoundInTable(index.opcodeOf(ValueID(place))).mayBeRunAgainUnseen() && !operandsMayRunCode(index.opcodeOf(ValueID(place)));
    if (blockOfEmptySlot && slotIsAsReadAtMeeting && arms.size() == 2 && (!closedBlock || failingWayLeavesCopy || failingWayBeginsTurnAgain) && index.opcodeOf(readOfEmptySlot) == Opcode::PropGet && !walker.hasFailed() && !walker.isUnreachable())
        noteAnswerOfReadOfSlotThatMayBeEmpty(walker, walker.asWalked(index.value(readOfEmptySlot)), holderOfEmptySlot, walker.referenceOf(readOfEmptySlot));
}

static bool oneOperationPaysForGuess(const Walker& walker, TypeID expected, Opcode leaning, bool readsMethodForItsCall)
{
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };
    static constexpr OpcodeSet accesses = opcodesOfFamily(OperationFamily::Access);
    bool asksForTruth = leaning == Opcode::ToBoolean;
    if (!asksForTruth && (readsMethodForItsCall || !accesses.contains(leaning) || accessesByKey.contains(leaning)))
        return false;
    Type present = walker.types().at(walker.types().withoutNullish(expected));
    return asksForTruth ? present.isObject() || present.isNumber() : present.isAnyString();
}

static bool isListHandedToUnitForGuesses(const Walker& walker, const BodyIndex& index, ValueID operand, const ExpectationForGuesses& expectation)
{
    if (!expectation.isDeclared || !index.isParameter(operand) || index.blockOf(operand) != entryBlock || walker.currentFunction() != walker.unit().function)
        return false;
    if (!walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) && walker.facts().summaries().at(walker.currentFunction()).has(SummaryTrait::HasLoops))
        return false;
    Type present = walker.types().at(walker.types().withoutNullish(expectation.type));
    const ClassPart* onlyClass = present.isObject() ? singleBuiltinClassOf(walker.types(), present) : nullptr;
    return onlyClass && onlyClass->builtinClass == BuiltinClasses::Array;
}

static uint32_t leastOperationsThatOpenGuessOutsideLoops(const Walker& walker, TypeID expected, bool isDeclaredInSource, std::optional<Opcode> leaning, bool leaningReadsMethodForItsCall)
{
    if (Walker::ChoiceOfCost drawn = walker.drawChoice(DrawnFor::GuessOutsideLoops, walker.currentFunction().index(), walker.originOfOperation().position); drawn != Walker::ChoiceOfCost::AsItPays)
        return drawn == Walker::ChoiceOfCost::Yes;
    Type type = walker.types().at(expected);
    if (type.hasObjects() && !(type.primitives & ~Primitive::UndefinedOrNull) && type.hasOnlyLayouts() && !type.hasOnlyClosures())
        return minimumOperationsPerGuessOfShape;
    if (isDeclaredInSource && type.isObject() && singleBuiltinOf(walker.types(), type))
        return minimumOperationsPerGuessOfShape;
    if (!isDeclaredInSource)
        return 0;
    return leaning && oneOperationPaysForGuess(walker, expected, *leaning, leaningReadsMethodForItsCall) ? 1 : minimumOperationsPerGuess;
}

static uint32_t leastOperationsThatOpenGuess(const Walker& walker, Reference value, TypeID expected, bool isDeclaredInSource, std::optional<Opcode> leaning = std::nullopt, bool leaningReadsMethodForItsCall = false)
{
    if (!walker.has(Mechanism::Guesses) || !value || !value.isValue() || !expected)
        return 0;
    if (walker.isInLoop())
        return minimumOperationsPerGuessInLoop;
    FunctionID function = walker.unit().function;
    if (walker.program().at(function).isModuleBody())
        return 0;
    return leastOperationsThatOpenGuessOutsideLoops(walker, expected, isDeclaredInSource, leaning, leaningReadsMethodForItsCall);
}

static std::optional<uint64_t> placeOfVariableForGuesses(const BodyValue& operation)
{
    switch (operation.opcode()) {
    case Opcode::GetModuleVariable:
    case Opcode::PutModuleVariable:
        return operation.variable().index();
    case Opcode::GetEnvironmentField:
    case Opcode::PutEnvironmentField:
        return uint64_t { 1 } << 63 | static_cast<uint64_t>(operation.layout().index()) << 32 | operation.field();
    default:
        return std::nullopt;
    }
}

bool outermostLoopStoresWhatIsRead(const BodyIndex& index, LoopID outermost, const BodyValue& read)
{
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasStoresOfOutermostLoops, [&] {
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            LoopID loop = index.isReachable(BlockID(number)) ? index.loopOf(BlockID(number)) : LoopID();
            if (!loop)
                continue;
            loop = index.loop(loop).outermost;
            for (BodyValue operation : index.block(BlockID(number)).values()) {
                if (operation.opcode() == Opcode::PutModuleVariable || operation.opcode() == Opcode::PutEnvironmentField)
                    kept.storesOfOutermostLoops.append(std::pair { loop.index(), *placeOfVariableForGuesses(operation) });
            }
        }
        std::ranges::sort(kept.storesOfOutermostLoops);
        kept.storesOfOutermostLoops.shrink(std::ranges::unique(kept.storesOfOutermostLoops).begin() - kept.storesOfOutermostLoops.begin());
    });
    return std::ranges::binary_search(kept.storesOfOutermostLoops, std::pair { outermost.index(), *placeOfVariableForGuesses(read) });
}

BlockID nearestBlockAboveThatBranchesOnTruthOf(const BodyIndex& index, ValueID asked, BlockID below)
{
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasBlocksThatBranchOnTruth, [&] {
        for (BlockID block : index.blocksInDominatorOrder()) {
            BodyTerminator end = index.terminator(block);
            ValueID question = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
            if (question && !index.isParameter(question) && index.opcodeOf(question) == Opcode::ToBoolean && index.value(question).operand(OperandIndex::ToBoolean::value))
                kept.blocksThatBranchOnTruth.append(static_cast<uint64_t>(index.valueBehindCasts(index.value(question).operand(OperandIndex::ToBoolean::value)).index()) << 32 | index.dominatorNumber(block));
        }
        std::ranges::sort(kept.blocksThatBranchOnTruth);
        std::span<const uint64_t> listed = kept.blocksThatBranchOnTruth.span();
        kept.placesOfNearestBlocksAboveThatAskTheSame.fill(UINT32_MAX, listed.size());
        Vector<uint32_t, 16> placesAbove;
        for (uint32_t place = 0; place < listed.size(); ++place) {
            if (place && listed[place - 1] >> 32 != listed[place] >> 32)
                placesAbove.shrink(0);
            BlockID block = index.blocksInDominatorOrder()[static_cast<uint32_t>(listed[place])];
            while (!placesAbove.isEmpty() && !index.dominates(index.blocksInDominatorOrder()[static_cast<uint32_t>(listed[placesAbove.last()])], block))
                placesAbove.removeLast();
            if (!placesAbove.isEmpty())
                kept.placesOfNearestBlocksAboveThatAskTheSame[place] = placesAbove.last();
            placesAbove.append(place);
        }
    });
    uint64_t asker = static_cast<uint64_t>(asked.index()) << 32 | index.dominatorNumber(below);
    std::span<const uint64_t> listed = kept.blocksThatBranchOnTruth.span();
    auto found = std::ranges::lower_bound(listed, asker);
    if (found == listed.end() || *found != asker)
        return { };
    uint32_t above = kept.placesOfNearestBlocksAboveThatAskTheSame[found - listed.begin()];
    return above == UINT32_MAX ? BlockID() : index.blocksInDominatorOrder()[static_cast<uint32_t>(listed[above])];
}

void sayWhatIsExpectedOfValueAskedForTruth(Walker& walker, const BodyValue& question, Draft& draft)
{
    if (!walker.takeQuestionOfTruthLeftToItsOwnTests(question.id()))
        return;
    TypeArena& types = walker.types();
    ValueID asked = question.operand(0);
    if (!saysNothingForGuesses(types, provenTypeOfInputForGuesses(walker, asked)))
        return;
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, asked);
    TypeID present = expectation.type ? types.withoutNullish(expectation.type) : TypeID();
    bool isBoolean = present && types.at(present).hasOnlyPrimitives(Primitive::Boolean);
    if (!present || isNeverType(present) || (!types.at(present).isObject() && !types.at(present).isNumber() && !isBoolean))
        return;
    if (leastOperationsThatOpenGuess(walker, walker.referenceOf(asked), expectation.type, expectation.isDeclared, Opcode::ToBoolean) != 1)
        return;
    draft.setExpectsObjectOrNothing(types.at(present).isObject());
    draft.setExpectsInt32OrNothing(types.at(present).isNumber());
    draft.setExpectsBoolean(isBoolean);
}

bool shouldOpenGuess(const Walker& walker, Reference value, TypeID expected, bool isDeclaredInSource, uint32_t numberOfTypedOperations)
{
    uint32_t least = leastOperationsThatOpenGuess(walker, value, expected, isDeclaredInSource);
    return least && numberOfTypedOperations >= least;
}

void openGuess(Walker& walker, Reference value, TypeID expected, BlockID inputBlock, uint32_t begin, uint32_t end)
{
    if (std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, value, expected))
        openGuessWithTest(walker, value, *chosen, inputBlock, begin, StretchOfGuess { end, 0, { } });
    else
        walker.walkStretch(inputBlock, begin, end);
}

static ALWAYS_INLINE bool testOfGuessStaysOperationOfItsOwn(const Walker& walker, Reference value)
{
    if (!walker.isInLoop())
        return false;
    std::optional<uint32_t> innermost = walker.innermostOpenLoop();
    return !innermost || walker.standsInFrontOfOpenLoop(*innermost, value) || walker.mayBeginTurnAgainInClosedCopy();
}

static ALWAYS_INLINE bool buildLoneReadOfGuess(Walker& walker, const BodyValue& operation, Reference value, const TestOfGuess& chosen, BlockID inputBlock, uint32_t begin, const StretchOfGuess& stretch, BranchOfGuessAskedAbout& branchAskedAbout)
{
    walker.noteTestSeenInTries(value, operation.id(), chosen.tested);
    Walker::HeardAsArm heardAsArm(walker);
    if (operation.opcode() != Opcode::PropGet || stretch.end != begin + 1 || (!chosen.layouts && !chosen.isPassedAlready) || testOfGuessStaysOperationOfItsOwn(walker, value))
        return false;
    if (armsOfGuessTakeBranch(walker, inputBlock, begin, stretch.end, chosen.isPassedAlready ? Reference() : value, stretch.placesThatTypedRunKnowsBetter, &branchAskedAbout))
        return false;
    if (walker.has(Mechanism::UnboxedNumbers) && isTakenBySwitchAloneForGuesses(walker, inputBlock, operation.id(), stretch.end)) {
        TypeID found = typeFoundInFilledSlot(walker, operation, walker.types().withoutNullish(walker.types().intersectionOf(walker.typeOf(value), chosen.tested)));
        if (found && !isNeverType(found) && walker.types().at(found).isInt32())
            return false;
    }
    bool looksAtSlot = stretch.firstAccessesToSlots.size() == 1 && stretch.firstAccessesToSlots[0].access == operation.id() && !stretch.firstAccessesToSlots[0].holder && stretch.storesThatLookAtCell.isEmpty();
    bool looksAtCell = stretch.firstAccessesToSlots.isEmpty() && stretch.storesThatLookAtCell.size() == 1 && stretch.storesThatLookAtCell[0] == operation.id();
    if ((!looksAtSlot && !looksAtCell) || stretch.looksWhetherValueIsAsMade || !stretch.testsOfStoredValues.isEmpty())
        return false;
    TypeID guessed = walker.types().intersectionOf(walker.typeOf(value), chosen.tested);
    branchAskedAbout.isAsked = false;
    Reference read = looksAtSlot ? buildReadThatTestsShapeAndSlot(walker, operation, value, guessed) : buildReadThatTestsShapeAndCell(walker, operation, value, guessed);
    if (!read)
        return false;
    walker.bind(operation.id(), read);
    walker.noteGuessOpened();
    return true;
}

static ALWAYS_INLINE bool buildLoneStoreOfGuess(Walker& walker, const BodyValue& operation, Reference value, const TestOfGuess& chosen, BlockID inputBlock, uint32_t begin, const StretchOfGuess& stretch, BranchOfGuessAskedAbout& branchAskedAbout)
{
    walker.noteTestSeenInTries(value, operation.id(), chosen.tested);
    Walker::HeardAsArm heardAsArm(walker);
    if (operation.opcode() != Opcode::PropSet || stretch.end != begin + 1 || (!chosen.layouts && !chosen.isPassedAlready) || testOfGuessStaysOperationOfItsOwn(walker, value))
        return false;
    if (!chosen.isPassedAlready && armsOfGuessTakeBranch(walker, inputBlock, begin, stretch.end, value, stretch.placesThatTypedRunKnowsBetter, &branchAskedAbout))
        return false;
    bool looksAtOwnSlotAlone = stretch.firstAccessesToSlots.isEmpty() || (stretch.firstAccessesToSlots.size() == 1 && stretch.firstAccessesToSlots[0].access == operation.id() && !stretch.firstAccessesToSlots[0].holder);
    bool looksAtCellForItselfAlone = stretch.storesThatLookAtCell.isEmpty() || (stretch.storesThatLookAtCell.size() == 1 && stretch.storesThatLookAtCell[0] == operation.id());
    bool testsOwnValueAlone = stretch.testsOfStoredValues.isEmpty() || (stretch.testsOfStoredValues.size() == 1 && stretch.testsOfStoredValues[0].value == operation.valueOperand());
    if (!looksAtOwnSlotAlone || !looksAtCellForItselfAlone || !testsOwnValueAlone || stretch.looksWhetherValueIsAsMade || stretch.orderWordAskedFor)
        return false;
    branchAskedAbout.isAsked = false;
    if (!buildStoreThatTestsForItself(walker, operation, value, walker.types().intersectionOf(walker.typeOf(value), chosen.tested)))
        return false;
    walker.noteGuessOpened();
    return true;
}

static bool buildLoneReadOfElementOfGuess(Walker& walker, const BodyValue& operation, ValueID operand, Reference value, const ExpectationForGuesses& expectation)
{
    if (walker.isInLoop() || operation.opcode() != Opcode::ElemGet || operand != operation.objectOperand() || !expectation.type || !expectation.isDeclared || !walker.has(Mechanism::TypedOperations))
        return false;
    TypeArena& types = walker.types();
    Type expected = types.at(types.withoutNullish(expectation.type));
    const ClassPart* onlyClass = expected.isObject() ? singleBuiltinClassOf(types, expected) : nullptr;
    if (!onlyClass || onlyClass->builtinClass != BuiltinClasses::Array)
        return false;
    if (!(types.at(provenTypeOfInputForGuesses(walker, operation.propertyOperand())).primitives & Primitive::Integer))
        return false;
    if (TypeID expectedOfKey = findExpectationForGuesses(walker, operation.propertyOperand()).type; expectedOfKey && !(types.at(expectedOfKey).primitives & Primitive::Integer))
        return false;
    if (!chooseTestOfGuess(walker, value, expectation.type))
        return false;
    walker.bind(operation.id(), buildReadOfElementOffExpectedList(walker, operation));
    walker.noteGuessOpened();
    return true;
}

static bool openGuessAtCalleeOfReturnedCall(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyTerminator end = index.terminator(index.blockOf(call.id()));
    if (end.kind() != TerminatorKind::ReturnCall || end.operand(0) != call.id() || walker.isUnreachable())
        return false;
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.calleeOperand());
    ExpectationForGuesses expectation = expectationOfCalleeForGuesses(walker, call, OperandIndex::Call::callee);
    if (!expectation.type || !types.at(expectation.type).isObject() || !singleBuiltinOf(types, types.at(expectation.type)))
        return false;
    if (!shouldOpenGuess(walker, callee, expectation.type, expectation.isDeclared, minimumOperationsPerGuessOfShape))
        return false;
    std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, callee, expectation.type);
    if (!chosen)
        return false;
    TypeID typeBehindTest = types.intersectionOf(walker.typeOf(callee), chosen->tested);
    auto typeOfInput = [&](ValueID input) {
        return walker.referenceOf(input) == callee ? typeBehindTest : provenTypeOfInputForGuesses(walker, input);
    };
    if (wouldCallBuiltin(walker, call, TypesOfInputs(typeOfInput)) == BuiltinCallAskedAbout::None)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    Reference test = buildTestOfGuess(walker, callee, *chosen, originInFrontOfCall);
    if (!test)
        return true;
    walker.noteGuessOpened();
    BlockID typedRun = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    walker.branch(test, typedRun, asWritten);

    walker.continueIn(asWritten);
    bool returnsAtOnce = walker.unitReturnsAtOnce(written);
    walker.chooseConservativeFields(written);
    Reference result = walker.build(WTF::move(written));
    BlockID behindCall;
    Reference resultBehindCall;
    if (returnsAtOnce)
        walker.returnAtOnce(result);
    else {
        behindCall = walker.addBlock();
        resultBehindCall = walker.addParameter(behindCall, { }, Representation::JSValue);
        if (result)
            walker.jumpTo(behindCall, singleElementSpan(result));
    }

    walker.continueIn(typedRun);
    Reference asBound = walker.referenceBoundTo(call.calleeOperand());
    walker.bind(call.calleeOperand(), buildProofOfGuess(walker, callee, test, *chosen, originInFrontOfCall));
    walker.bind(call.id(), { });
    if (!callBuiltin(walker, call))
        walker.copy(call);
    walker.bind(call.calleeOperand(), asBound);
    if (!behindCall)
        return true;
    Reference resultOfTypedRun = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    if (resultOfTypedRun)
        walker.jumpTo(behindCall, singleElementSpan(resultOfTypedRun));
    else
        walker.continueIn(behindCall);
    if (!walker.isUnreachable())
        walker.bind(call.id(), resultBehindCall);
    return true;
}

static ValueID findReadOfVariableThatMeetsMockForGuesses(Walker& walker, const BodyIndex& index, ValueID parameter)
{
    BlockID block = index.blockOf(parameter);
    if (index.isEnteredByHandlerEdge(block))
        return { };
    uint32_t position = parameter.index() - index.block(block).parameter(0).index();
    ValueID read;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        if (position >= arguments.size())
            return { };
        ValueID source = arguments[position];
        while (!index.isParameter(source) && inputOperationsThatHandOn.contains(index.opcodeOf(source)))
            source = index.value(source).operand(0);
        if (index.isParameter(source))
            return { };
        if (index.opcodeOf(source) == Opcode::PropGet) {
            ValueID subject = index.value(source).operand(0);
            if (index.isParameter(subject) || index.opcodeOf(subject) != Opcode::MockSeenBy)
                return { };
            continue;
        }
        if (index.opcodeOf(source) != Opcode::GetModuleVariable || read)
            return { };
        read = source;
    }
    return read;
}

bool callFunctionGivenToVariableOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::DirectCalls) || call.isDoneInPlace())
        return false;
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee || !callee.isValue())
        return false;
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    Type proven = types.at(walker.typeOf(callee));
    TypeID tested;
    TypeID testedOnFailingSide;
    if (proven.isObject() && proven.hasOnlyClosures())
        return false;
    {
        ValueID source = call.calleeOperand();
        while (!index.isParameter(source) && inputOperationsThatHandOn.contains(index.opcodeOf(source)))
            source = index.value(source).operand(0);
        bool meetsMock = index.isParameter(source);
        if (meetsMock)
            source = findReadOfVariableThatMeetsMockForGuesses(walker, index, source);
        if (!source || index.opcodeOf(source) != Opcode::GetModuleVariable)
            return false;
        VariableID variable = index.value(source).as<Opcode::GetModuleVariable>().variable();
        const FactTables::FunctionGivenToVariable* given = walker.facts().findFunctionGivenTo(variable);
        if (!given || (walker.isInLoop() && !meetsMock && walker.program().at(variable).flags.contains(VariableFlags::IsReplaceable)))
            return false;
        tested = types.forFunction(given->atStart ? given->atStart : given->function);
        if (given->atStart)
            testedOnFailingSide = walker.withoutFunctionsThatMayNotBeMentioned(types.forFunction(given->function)) == types.forFunction(given->function) ? types.forFunction(given->function) : TypeID();
    }
    FunctionID function = walker.unit().function;
    if (walker.program().at(function).isModuleBody())
        return false;
    std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, callee, tested);
    if (!chosen)
        return false;
    chosen->failingSideAsksFor = testedOnFailingSide;

    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(call.id()))
        block = index.block(index.blockOf(call.id()));
    uint32_t begin = call.id().index() - block.firstValue().index();
    if (begin >= limitOfStretchOfGuess(walker, block) || standsInNoStretchOfGuess(index, call))
        return false;
    openGuessWithTest(walker, callee, *chosen, block.id(), begin, StretchOfGuess { begin + 1, 1, { } });
    return true;
}

static ALWAYS_INLINE bool nothingIsExpectedOfOperandsForGuesses(Walker& walker, const BodyValue& operation, ExpectationForGuesses& firstExpected, uint32_t& indexOfFirstExpected)
{
    bool isCall = callsOfValuesForGuesses.contains(operation.opcode());
    bool nothingIsExpected = true;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!nothingIsExpected || isOnlyHandedOnForGuesses(operation, operandIndex))
            return;
        Reference value = walker.referenceOf(operand);
        if (!value || !value.isValue())
            return;
        ExpectationForGuesses expectation = isCall ? expectationOfCalleeForGuesses(walker, operation, operandIndex) : expectationOfOperandForGuesses(walker, operation, operand);
        if (!expectation.type)
            return;
        nothingIsExpected = false;
        firstExpected = expectation;
        indexOfFirstExpected = operandIndex;
    });
    return nothingIsExpected;
}

static bool noGuessIsOpenedWhereWalkStandsForGuesses(const Walker& walker, bool isInLoop)
{
    if (walker.isNestedTooDeeply()) [[unlikely]]
        return true;
    if (isInLoop)
        return false;
    FunctionID function = walker.unit().function;
    return walker.program().at(function).isModuleBody();
}

static ALWAYS_INLINE bool isReadOfVariableForGuesses(const BodyIndex& index, ValueID value)
{
    value = index.valueBehindCasts(value);
    while (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::CheckInitialized)
        value = index.valueBehindCasts(index.value(value).operand(0));
    return value && !index.isParameter(value) && (index.opcodeOf(value) == Opcode::GetEnvironmentField || index.opcodeOf(value) == Opcode::GetModuleVariable);
}

static ALWAYS_INLINE bool testAdmitsAllThatIsExpectedForGuesses(Walker& walker, const BodyValue& operation, ValueID operand, uint32_t operandIndex, TypeID expected, TypeID tested)
{
    static constexpr OpcodeSet accessesThatThrowOffNothing { Opcode::PropGet, Opcode::PropSet, Opcode::ElemGet, Opcode::ElemSet };
    static constexpr OpcodeSet accessesByName { Opcode::PropGet, Opcode::PropSet };
    Opcode opcode = operation.opcode();
    bool throwsOffNothing = accessesThatThrowOffNothing.contains(opcode)
        ? operand == operation.objectOperand() && !(accessesByName.contains(opcode) && operation.isFreeName())
        : callsOfValuesForGuesses.contains(opcode) && operation.operandUses(operandIndex).contains(OperandUse::Calls);
    TypeArena& types = walker.types();
    static constexpr OpcodeSet operatorsOnNumbers = allRelations | allBitwiseOperators | operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative });
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet };
    bool isKey = accessesByKey.contains(opcode) && operand == operation.propertyOperand();
    bool takesNumbersAlone = (isKey || operatorsOnNumbers.contains(opcode)) && types.at(types.withoutNullish(expected)).hasOnlyPrimitives(Primitive::Number | Primitive::Absent);
    return types.isSubset(throwsOffNothing || takesNumbersAlone ? types.withoutNullish(expected) : expected, tested);
}

static bool splitUndefinedOffKeyOfUpdateForGuesses(Walker& walker, const BodyValue& read, uint32_t behindUpdate)
{
    Reference position = walker.referenceOf(read.propertyOperand());
    if (!position || !position.isValue())
        return false;
    Type key = walker.types().at(walker.typeOf(position));
    if (!key.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) || !(key.primitives & Primitive::Number) || !(key.primitives & Primitive::Undefined))
        return false;
    BodyBlock block = walker.currentBodyIndex().block(walker.inputBlockBeingWalked());
    TestOfNumber asked { read.propertyOperand(), walker.types().withoutNullish(walker.typeOf(position)) };
    return walkStretchBehindTestsOfNumbers(walker, std::span<const TestOfNumber>(&asked, 1), block.id(), read.id().index() - block.firstValue().index(), behindUpdate, BlockFrequency::Rare);
}

bool openGuessAtElementOfNumbersOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !walker.has(Mechanism::Folding))
        return false;
    Reference list = walker.referenceOf(read.objectOperand());
    Reference position = walker.referenceOf(read.propertyOperand());
    if (!list || !list.isValue() || !position)
        return false;
    TypeID typeOfList = walker.typeOf(list);
    std::optional<uint32_t> behindUpdate = !walker.isInLoop() && isViewOfNumbersReadInLine(walker, typeOfList) ? findPlaceBehindUpdateOfElement(walker, read) : std::nullopt;
    if (behindUpdate && leastOperationsThatOpenGuess(walker, list, typeOfList, true, read.opcode()) && splitUndefinedOffKeyOfUpdateForGuesses(walker, read, *behindUpdate))
        return true;
    bool turnMayBeginAgain = walker.isInLoop() && walker.turnOfTypedCopyHasDoneNothing();
    TypeID elementInsideView = turnMayBeginAgain || behindUpdate ? typeOfElementInsideViewOfNumbers(walker, read, walker.types().withoutNullish(typeOfList), walker.typeOf(position), !!behindUpdate) : TypeID();
    if (elementInsideView && walker.types().at(typeOfList).mayBeUndefinedOrNull()) {
        if (splitNullishOffReceiverOfAccess(walker, read))
            return true;
        list = walker.referenceOf(read.objectOperand());
        typeOfList = walker.typeOf(list);
        if (!list.isValue() || walker.types().at(typeOfList).mayBeUndefinedOrNull())
            return false;
    }
    if (elementInsideView && (positionIsKnownToLieInsideList(walker, list, position) || walker.findWhatHoldsPositionBelowRoomOfView(list, position)))
        return false;
    if (!elementInsideView && !readTakesIndexOfProvenList(walker.types(), typeOfList, walker.typeOf(position)))
        return false;
    if (!elementInsideView && !isDeclaredToHoldNumbersAlone(walker.program(), walker.types(), typeOfList)) {
        TypeID expected = findExpectationForGuesses(walker, read.objectOperand()).type;
        if (!expected || !isDeclaredToHoldNumbersAlone(walker.program(), walker.types(), walker.types().withoutNullish(expected)))
            return false;
    }
    if (findKnown(walker, KnownNote::NumberReadOutOfList, list, position))
        return false;
    if (!elementInsideView && findListTestedInFrontOfLoop(walker, read))
        return false;
    if (TypeID proven = elementInsideView ? TypeID() : walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id()); proven && !saysNothingForGuesses(walker.types(), proven))
        return false;
    ValueID cell = rootOfCellInSweptCode(walker.code(), list.id());
    if (!walker.code().isConstant(cell) && walker.state().isFresh(Reference::value(cell)))
        return false;

    uint32_t least = leastOperationsThatOpenGuess(walker, list, typeOfList, true, read.opcode());
    if (!least)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(read.id()))
        block = index.block(index.blockOf(read.id()));
    uint32_t begin = read.id().index() - block.firstValue().index();
    uint32_t limit = limitOfStretchOfGuess(walker, block);
    if (begin >= limit)
        return false;
    if (behindUpdate && *behindUpdate > limit)
        return false;
    TestOfGuess chosen { typeOfList, { }, true };
    chosen.asksForElementOfNumbers = true;
    chosen.elementInsideView = elementInsideView;
    chosen.admitsAllThatIsExpected = !!elementInsideView && !behindUpdate;
    chosen.updatesElement = !!elementInsideView && behindUpdate;
    StretchOfGuess stretch = elementInsideView ? StretchOfGuess { behindUpdate ? *behindUpdate : begin + 1, least, { } } : findStretchOfGuess(walker, list, chosen, block, begin, limit);
    if (stretch.numberOfTypedOperations < least)
        return false;
    uint32_t first = block.firstValue().index();
    for (uint32_t place = begin + 1; place < stretch.end; ++place) {
        BodyValue operation = index.value(ValueID(first + place));
        if (!operationsThatJoinTexts.contains(operation.opcode()))
            continue;
        bool joinsNumbersAlone = true;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            if (operand.index() >= first + begin) {
                Opcode maker = index.opcodeOf(operand);
                joinsNumbersAlone &= operand == read.id() || operatorsThatMakeNumbers.contains(maker) || maker == Opcode::Integer || maker == Opcode::Double || inputOperationsThatOnlyHandOn.contains(maker);
                return;
            }
            TypeID proven = provenTypeOfInputForGuesses(walker, operand);
            joinsNumbersAlone &= proven && walker.types().at(proven).isNumber();
        });
        if (!joinsNumbersAlone)
            return false;
    }
    for (uint32_t place = begin; place < stretch.end && !elementInsideView; ++place) {
        ValueID made(first + place);
        if (place > begin && !stretch.placesThatRestOnProof.get(place - begin))
            continue;
        uint32_t mentionsInStretch = 0;
        for (uint32_t later = place + 1; later < stretch.end; ++later) {
            index.value(ValueID(first + later)).forEachOperand([&](ValueID operand, uint32_t) {
                mentionsInStretch += operand == made;
            });
        }
        uint32_t uses = index.useCount(made);
        if (uses == BodyIndex::maxCountedUses || uses > mentionsInStretch)
            return false;
    }
    Walker::ReadWhoseGuessIsOpen opened { walker.depthOfCopies(), read.id() };
    if (walker.readWhoseGuessIsOpen() == opened) {
        walker.fail("the stretch of a guess at an element opens the same guess at the same read again"_s, InternalErrorKind::GuessOpensAgain);
        return true;
    }
    SetForScope isOpen(walker.readWhoseGuessIsOpen(), opened);
    openGuessWithTest(walker, list, chosen, block.id(), begin, stretch);
    if (elementInsideView && !walker.isUnreachable())
        endKnown(walker, KnownNote::NumberReadOutOfList, list, walker.referenceOf(read.propertyOperand()));
    return true;
}

static uint32_t endOfWhatTakesResultForGuesses(Walker& walker, const BodyBlock& block, uint32_t place, uint32_t limit)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t first = block.firstValue().index();
    BitVector takesResult;
    takesResult.set(place);
    uint32_t end = place + 1;
    for (uint32_t next = place + 1; next < limit; ++next) {
        BodyValue operation = walker.asWalked(index.value(ValueID(first + next)));
        if (standsInNoStretchOfGuess(index, operation))
            break;
        bool takes = false;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            takes |= operand.index() >= first + place && takesResult.get(operand.index() - first);
        });
        if (takes) {
            takesResult.set(next);
            end = next + 1;
        }
    }
    return end;
}

static ALWAYS_INLINE bool takeApartCallThatFindsBuiltinForGuesses(Walker& walker, const BodyValue& read)
{
    if (!walker.facts().held().callersThatFindBuiltins.get(walker.currentFunction().index()) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::DirectCalls))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!isReadOfMethodForItsCallForGuesses(index, read))
        return false;
    ValueID call = onlyUserBehindWhatWireExpectsForGuesses(index, read.id());
    if (!walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), call))
        return false;
    Reference receiver = walker.referenceOf(read.objectOperand());
    if (!receiver || !receiver.isValue() || !leastOperationsThatOpenGuess(walker, receiver, walker.typeOf(receiver), false))
        return false;
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t begin = read.id().index() - block.firstValue().index();
    uint32_t limit = limitOfStretchOfGuess(walker, block);
    if (!block.holds(call) || call.index() - block.firstValue().index() >= limit)
        return false;
    TypedForm form = typedFormOf(walker, read, [&](ValueID input) {
        return provenTypeOfInputForGuesses(walker, input);
    });
    if (form != TypedForm::BuiltBehindFilledSlot)
        return false;
    StretchOfGuess stretch { endOfWhatTakesResultForGuesses(walker, block, call.index() - block.firstValue().index(), limit), 1, { } };
    stretch.firstAccessesToSlots.append({ read.id(), { } });
    openGuessWithTest(walker, receiver, TestOfGuess { walker.typeOf(receiver), { }, true }, block.id(), begin, stretch);
    return true;
}

static ALWAYS_INLINE bool askGuardOfNameReadOffConstructorFunction(Walker& walker, const BodyValue& read)
{
    Reference function = walker.has(Mechanism::TypedOperations) && !read.isFreeName() ? walker.referenceOf(read.objectOperand()) : Reference();
    if (!function || !function.isRecord() || !readOffConstructorFunctionAwaitsGuard(walker, read, walker.typeOf(function)))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    ValueID call = onlyUserBehindWhatWireExpectsForGuesses(index, read.id());
    uint32_t first = block.firstValue().index();
    uint32_t begin = read.id().index() - first;
    if (!block.holds(call) || call.index() - first >= limitOfStretchOfGuess(walker, block))
        return false;
    for (uint32_t place = begin; first + place <= call.index(); ++place) {
        if (standsInNoStretchOfGuess(index, walker.asWalked(index.value(ValueID(first + place)))))
            return false;
    }
    StretchOfGuess stretch { call.index() + 1 - first, 1, { } };
    stretch.storesThatLookAtCell.append(read.id());
    TestOfGuess passedAlready { walker.typeOf(function), { }, true };
    passedAlready.admitsAllThatIsExpected = true;
    openGuessWithTest(walker, function, passedAlready, block.id(), begin, stretch);
    return true;
}

static ALWAYS_INLINE bool askGuardsOfMethodReadOffPrototypeOfClass(Walker& walker, const BodyValue& read)
{
    if (!walker.has(Mechanism::TypedOperations) || read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::prototype))
        return false;
    Reference function = walker.referenceOf(read.objectOperand());
    if (!function || !function.isRecord())
        return false;
    TypeArena& types = walker.types();
    auto namesOneFunction = [&](TypeID type) {
        return type && types.at(type).isObject() && types.at(type).isJavaScriptValue() && singleFunctionOf(types, types.at(type));
    };
    TypeID typeOfFunctionObject = walker.typeOf(function);
    bool isProven = namesOneFunction(typeOfFunctionObject);
    if (!isProven) {
        TypeID expected = function.isValue() ? findExpectationForGuesses(walker, read.objectOperand()).type : TypeID();
        typeOfFunctionObject = expected ? types.withoutNullish(expected) : TypeID();
        if (!namesOneFunction(typeOfFunctionObject))
            return false;
    }
    bool guardsHold = false;
    if (!findsMethodReadOffPrototypeOfClass(walker, read, typeOfFunctionObject, guardsHold) || (isProven && guardsHold))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    ValueID method = index.onlyUsingValue(read.id());
    ValueID call = index.onlyUsingValue(method);
    if (!call && index.useCount(method) == 2) {
        std::optional<BodyUser> forwarder = index.firstUser(method);
        if (forwarder && forwarder->value && index.opcodeOf(forwarder->value) == Opcode::PropGet)
            call = index.onlyUsingValue(forwarder->value);
    }
    if (!call || index.opcodeOf(call) != Opcode::Call)
        return false;
    bool mentionsMethod = false;
    index.value(call).forEachOperand([&](ValueID operand, uint32_t) {
        mentionsMethod |= operand == method;
    });
    uint32_t first = block.firstValue().index();
    uint32_t begin = read.id().index() - first;
    if (!mentionsMethod || !block.holds(call) || call.index() - first >= limitOfStretchOfGuess(walker, block))
        return false;
    for (uint32_t place = begin; first + place <= call.index(); ++place) {
        if (standsInNoStretchOfGuess(index, walker.asWalked(index.value(ValueID(first + place)))))
            return false;
    }
    StretchOfGuess stretch { call.index() + 1 - first, 1, { } };
    stretch.storesThatLookAtCell.append(read.id());
    TestOfGuess isThatFunctionObject { typeOfFunctionObject, { }, isProven };
    isThatFunctionObject.admitsAllThatIsExpected = true;
    openGuessWithTest(walker, function, isThatFunctionObject, block.id(), begin, stretch);
    return true;
}

static uint32_t limitOfStretchLookedAtAheadForGuesses(const BodyIndex& index, const BodyBlock& block)
{
    uint32_t limit = placeTakenByEndOfBlockForGuesses(index, block);
    BodyTerminator end = block.terminator();
    ValueID condition = end.isConditionalBranch() ? end.operand(0) : ValueID();
    if (condition && condition >= block.firstValue() && block.holds(condition) && allRelations.contains(index.opcodeOf(condition)))
        limit = std::min(limit, condition.index() - block.firstValue().index());
    return limit;
}

namespace {

struct ChainOfMembersAskedByTheirBytes {
    Vector<BuiltinID, 2> members;
    BlockID block;
    uint32_t begin { 0 };
    uint32_t placeOfTaker { 0 };
    uint32_t limit { 0 };
};

} // anonymous namespace

static std::optional<ChainOfMembersAskedByTheirBytes> findChainOfMembersAskedByTheirBytes(Walker& walker, const BodyValue& read, TypeID typeOfHolder)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses))
        return std::nullopt;
    BuiltinID member = findMemberAskedByItsByteOffType(walker, typeOfHolder, read.key());
    if (!member)
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    if (!walker.isInLoop() && !index.loopOf(block.id()))
        return std::nullopt;
    uint32_t first = block.firstValue().index();
    ChainOfMembersAskedByTheirBytes chain;
    chain.block = block.id();
    chain.begin = read.id().index() - first;
    chain.limit = walker.inputBlockBeingWalked() == block.id() ? limitOfStretchOfGuess(walker, block) : limitOfStretchLookedAtAheadForGuesses(index, block);
    if (chain.begin >= chain.limit)
        return std::nullopt;
    chain.members.append(member);
    for (ValueID last = read.id();;) {
        std::optional<BodyUser> user = index.firstUser(last);
        while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(index.opcodeOf(user->value))) {
            last = user->value;
            user = index.firstUser(last);
        }
        if (!user || user->kind != BodyUseKind::Operand || !block.holds(user->value) || user->value.index() - first >= chain.limit)
            return std::nullopt;
        BodyValue next = index.value(user->value);
        BuiltinID below = next.opcode() == Opcode::PropGet && !next.isFreeName() && next.objectOperand() == last ? findMemberAskedByItsByte(walker, member, next.key()) : BuiltinID();
        if (!below) {
            chain.placeOfTaker = next.id().index() - first;
            break;
        }
        chain.members.append(below);
        member = below;
        last = next.id();
    }
    Walker::PlacesSeenToStandInStretches& seen = walker.placesSeenToStandInStretches();
    if (seen.index == &index && seen.block == block.id() && seen.from <= chain.begin && chain.placeOfTaker <= seen.to)
        return chain;
    for (uint32_t place = chain.begin; place <= chain.placeOfTaker; ++place) {
        if (standsInNoStretchOfGuess(index, index.value(ValueID(first + place))))
            return std::nullopt;
    }
    seen = { &index, block.id(), chain.begin, chain.placeOfTaker };
    return chain;
}

static StretchOfGuess stretchBehindBytesOfMembers(Walker& walker, const ChainOfMembersAskedByTheirBytes& chain)
{
    static constexpr OpcodeSet storesByNameOrKey { Opcode::PropSet, Opcode::ElemSet };
    StretchOfGuess stretch { chain.placeOfTaker + 1, 1, { } };
    stretch.membersAskedByTheirBytes = chain.members;
    if (walker.mayBeginTurnAgainInClosedCopy() || walker.numberOfStretchesBehindBytesOfMembers())
        return stretch;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(chain.block);
    uint32_t first = block.firstValue().index();
    BitVector takesResult;
    takesResult.set(chain.begin);
    BitVector isAsked;
    for (BuiltinID member : chain.members)
        isAsked.set(member.index());
    Vector<BuiltinID, 32> memberReadAt;
    memberReadAt.append(chain.members.first());
    auto memberNamedBy = [&](ValueID value) -> BuiltinID {
        value = index.valueBehindCasts(value);
        if (index.isParameter(value))
            return { };
        if (index.opcodeOf(value) == Opcode::Builtin)
            return index.value(value).builtin();
        return value.index() >= first + chain.begin && value.index() - first - chain.begin < memberReadAt.size() ? memberReadAt[value.index() - first - chain.begin] : BuiltinID();
    };
    Vector<std::pair<uint32_t, BuiltinID>, 8> readBesideChain;
    bool otherCodeMayHaveRun = false;
    uint32_t end = chain.begin + 1;
    for (uint32_t next = chain.begin + 1; next < chain.limit; ++next) {
        BodyValue operation = walker.asWalked(index.value(ValueID(first + next)));
        if (standsInNoStretchOfGuess(index, operation))
            break;
        BuiltinID holder = operation.opcode() == Opcode::PropGet ? memberNamedBy(operation.objectOperand()) : BuiltinID();
        BuiltinID member = holder ? findMemberAskedByItsByte(walker, holder, operation.key()) : BuiltinID();
        if (member && otherCodeMayHaveRun && next > chain.placeOfTaker)
            break;
        memberReadAt.append(member);
        if (member && !otherCodeMayHaveRun && !isAsked.get(member.index()))
            readBesideChain.append({ next, member });
        bool takes = false;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            takes |= operand.index() >= first + chain.begin && takesResult.get(operand.index() - first);
        });
        if (takes) {
            takesResult.set(next);
            end = next + 1;
        }
        otherCodeMayHaveRun |= storesByNameOrKey.contains(operation.opcode()) || (callsOfValuesForGuesses.contains(operation.opcode()) && !memberNamedBy(operation.operand(0)));
    }
    stretch.end = std::max(end, chain.placeOfTaker + 1);
    for (auto [place, member] : readBesideChain) {
        if (place < stretch.end && !isAsked.set(member.index()))
            stretch.membersAskedByTheirBytes.append(member);
    }
    return stretch;
}

BuiltinID findMemberThatGuessAsksByItsByte(Walker& walker, const BodyValue& read, TypeID typeOfHolder)
{
    if (!walker.program().admitsReplacedBuiltins || read.opcode() != Opcode::PropGet)
        return { };
    std::optional<ChainOfMembersAskedByTheirBytes> chain = findChainOfMembersAskedByTheirBytes(walker, read, typeOfHolder);
    return chain ? chain->members.first() : BuiltinID();
}

static bool askBytesOfMembersReadOffBuiltins(Walker& walker, const BodyValue& read, Reference holder)
{
    if (walker.numberOfStretchesBehindBytesOfMembers() > 1 && !walker.mayBeginTurnAgainInClosedCopy())
        return false;
    std::optional<ChainOfMembersAskedByTheirBytes> chain = findChainOfMembersAskedByTheirBytes(walker, read, walker.typeOf(holder));
    if (!chain)
        return false;
    StretchOfGuess stretch = stretchBehindBytesOfMembers(walker, *chain);
    walker.count(StatisticID::MembersOfBuiltinsAskedByTheirBytes, stretch.membersAskedByTheirBytes.size());
    TestOfGuess passedAlready { walker.typeOf(holder), { }, true };
    passedAlready.admitsAllThatIsExpected = true;
    walker.noteMemberAskedByItsByteInTries(read, walker.typeOf(holder));
    ++walker.numberOfStretchesBehindBytesOfMembers();
    openGuessWithTest(walker, holder, passedAlready, chain->block, chain->begin, stretch);
    --walker.numberOfStretchesBehindBytesOfMembers();
    return true;
}

static ALWAYS_INLINE bool nameMayOpenGuessOfKeyOrdersForGuesses(const Walker& walker, const BodyValue& access)
{
    if (access.isFreeName())
        return false;
    PropertyKeyID name = access.key();
    if (!name || liesInThread(name))
        return false;
    const LinkTables& linked = walker.facts().linked();
    return linked.keysOfAccessorsExpectedInFields.get(name.index()) || linked.keysOfStaticFields.get(name.index());
}

static constexpr size_t mostCalleesToldApartAtCall = 4;
static constexpr uint32_t mostOperationsWalkedBehindCallForEachCallee = 8;
static constexpr OpcodeSet callsToldApartByCalleeForGuesses { Opcode::Call, Opcode::Construct };
static constexpr size_t atNoFrameOfCopies = std::numeric_limits<size_t>::max();

TypeID fewClosuresWhoseCopiesPayInLoop(Walker& walker, TypeID held, size_t numberOfArguments)
{
    if (!held || holdsOnlyPrimitives(held))
        return { };
    TypeArena& types = walker.types();
    TypeID closures = types.withOnlyObjects(held);
    Type named = types.at(closures);
    if (!named.isObject() || !named.isJavaScriptValue() || !named.hasOnlyClosures() || named.hasUnknown() || named.namesMergedObjects() || walker.withoutFunctionsThatMayNotBeMentioned(closures) != closures)
        return { };
    size_t numberOfFunctions = 0;
    bool everyCopyPays = true;
    forEachFunctionOf(types, named, scopedLambda<void(FunctionID)>([&](FunctionID function) {
        ++numberOfFunctions;
        everyCopyPays = everyCopyPays && copyOfCalleeCalledInLoopPays(walker, function, numberOfArguments);
    }));
    return everyCopyPays && numberOfFunctions >= 2 && numberOfFunctions <= mostCalleesToldApartAtCall ? closures : TypeID();
}

static TypeID fewClosuresThatCalleeIsHeldToBeForGuesses(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), call.id()))
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    for (ValueID called = call.calleeOperand();;) {
        called = index.valueBehindCasts(called);
        if (!called)
            return { };
        if (loop && !index.isInLoop(index.blockOf(called), loop))
            break;
        if (index.isParameter(called))
            return { };
        BodyValue read = index.value(called);
        if (read.opcode() == Opcode::GetModuleVariable)
            break;
        if (read.opcode() == Opcode::CheckInitialized)
            called = read.operand(0);
        else if (read.opcode() == Opcode::PropGet && !read.isFreeName())
            called = read.objectOperand();
        else
            return { };
    }
    ExpectationForGuesses expected = findExpectationForGuesses(walker, call.calleeOperand());
    return expected.isHeld ? fewClosuresWhoseCopiesPayInLoop(walker, expected.type, call.argumentsOperands().size()) : TypeID();
}

static bool callEachFunctionOfProvenSetForGuesses(Walker& walker, const BodyValue& call)
{
    if (walker.standsInArmOfRowOfShapeTests() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    if (call.opcode() == Opcode::Call ? call.as<Opcode::Call>().isDoneInPlace() : !!call.as<Opcode::Construct>().newTargetOperand())
        return false;
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee || !callee.isValue())
        return false;
    TypeArena& types = walker.types();
    auto namesClosuresAlone = [](const Type& type) {
        return type.hasObjects() && type.isJavaScriptValue() && type.hasOnlyClosures() && !type.hasUnknown() && !type.namesMergedObjects();
    };
    Type named = types.at(walker.typeOf(callee));
    bool isProven = namesClosuresAlone(named) && named.isObject();
    TypeID held = !isProven && walker.mayBeginTurnAgainInClosedCopy() ? fewClosuresThatCalleeIsHeldToBeForGuesses(walker, call) : TypeID();
    if (held)
        named = types.at(held);
    else if (!isProven) {
        BodyValue read = walker.currentBodyIndex().value(call.calleeOperand());
        if (read.opcode() != Opcode::ElemGet)
            return false;
        TypeID holder = findWhatIsProvenOrExpectedForGuesses(walker, read.objectOperand(), read.id()).type;
        LayoutID record = holder ? singleLayoutOf(types, types.at(types.withoutNullish(holder))) : LayoutID();
        auto closuresInFields = [&](const LinkTables& tables) {
            TypeID found;
            if (!record || walker.program().at(record).kind != LayoutKind::Record)
                return found;
            for (uint32_t field = 0; field < walker.program().at(record).fields.size(); ++field) {
                const LinkedCell* cell = tables.findCellOfField({ record, field });
                if (cell && cell->proven && namesClosuresAlone(types.at(cell->proven)))
                    found = found ? types.unionOf(found, cell->proven) : cell->proven;
            }
            return found;
        };
        TypeID expected = closuresInFields(walker.facts().linked());
        if (!expected) {
            TypeID seen = walker.facts().held().resultOfReadByKey(walker.currentFunction(), read.id());
            expected = seen && namesClosuresAlone(types.at(seen)) ? seen : closuresInFields(walker.facts().held());
        }
        if (!expected)
            return false;
        named = types.at(expected);
    }
    Vector<FunctionID, mostCalleesToldApartAtCall> functions;
    bool areFew = true;
    forEachFunctionOf(types, named, scopedLambda<void(FunctionID)>([&](FunctionID function) {
        if (functions.size() == mostCalleesToldApartAtCall)
            areFew = false;
        else
            functions.append(function);
    }));
    if (!areFew || functions.size() < (isProven ? 2 : 1))
        return false;
    const CallingConventions& conventions = walker.plan().conventions();
    for (FunctionID function : functions) {
        if (!conventions.exists(conventions.closureEntryOf(function)) || findOnlyCallee(walker, call, types.forFunction(function)) != function || !chooseTestOfGuess(walker, callee, types.forFunction(function)))
            return false;
    }

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(call.id()))
        block = index.block(index.blockOf(call.id()));
    uint32_t firstOfBlock = block.firstValue().index();
    uint32_t begin = call.id().index() - firstOfBlock;
    uint32_t limit = limitOfStretchOfGuess(walker, block);
    if (begin >= limit || standsInNoStretchOfGuess(index, call))
        return false;

    auto callsAnotherOfSeveral = [&](const BodyValue& operation) {
        if (!callsToldApartByCalleeForGuesses.contains(operation.opcode()) || !operation.calleeOperand() || operation.calleeOperand() >= call.id())
            return false;
        Reference other = walker.referenceOf(operation.calleeOperand());
        if (!other || other == callee || !other.isValue())
            return false;
        Type type = types.at(walker.typeOf(other));
        return type.isObject() && type.hasOnlyClosures() && !singleFunctionOf(types, type);
    };
    uint32_t end = begin + 1;
    if (!isReturnedAtOnceForGuesses(index, call.id())) {
        BitVector leansOnResult;
        leansOnResult.set(0);
        uint32_t numberOfOperations = 0;
        for (uint32_t place = begin + 1; place < limit && numberOfOperations < mostOperationsWalkedBehindCallForEachCallee; ++place) {
            BodyValue operation = walker.asWalked(index.value(ValueID(firstOfBlock + place)));
            if (standsInNoStretchOfGuess(index, operation) || isReturnedAtOnceForGuesses(index, operation.id()) || callsAnotherOfSeveral(operation))
                break;
            numberOfOperations += !opcodesThatStateTypes.contains(operation.opcode());
            bool leans = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                leans |= operand && operand >= call.id() && leansOnResult.get(operand.index() - call.id().index());
            });
            if (!leans)
                continue;
            leansOnResult.set(place - begin);
            end = place + 1;
        }
    }

    uint32_t first = firstOfBlock + begin;
    uint32_t size = end - begin;
    Vector<ValueID, 4> names;
    for (uint32_t place = 0; place < size; ++place) {
        index.value(ValueID(first + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand && operand.index() < first && walker.referenceOf(operand) == callee && !names.contains(operand))
                names.append(operand);
        });
    }

    Origin origin = originInFrontOfOperationForGuesses(walker, call);
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, end, true);
    Walker::ArmsThatMeet built;
    auto walkArmWith = [&](Reference closure) {
        Vector<Reference, 4> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, closure);
        }
        for (uint32_t place = 0; place < size; ++place) {
            walker.bind(ValueID(first + place), Reference());
            walker.rememberedExpectationOf(ValueID(first + place)) = { };
        }
        walker.walkStretch(block.id(), begin, end);
        walker.listArm(stretch, built);
        for (size_t name = 0; name < names.size(); ++name)
            walker.bind(names[name], boundBefore[name]);
    };

    TestOfGuess askedFirst { types.forFunction(functions[0]), { } };
    Reference test = buildTestOfGuess(walker, callee, askedFirst, origin);
    if (!test || !test.isValue())
        return false;

    SetForScope standsInArm(walker.numberOfFramesAtRowOfShapeTests(), atNoFrameOfCopies);
    Reference left = callee;
    BlockID meeting;
    size_t numberTestedFor = isProven ? functions.size() - 1 : functions.size();
    for (size_t place = 0; place < numberTestedFor && !walker.isUnreachable(); ++place) {
        TestOfGuess asked { types.forFunction(functions[place]), { } };
        if (place)
            test = buildTestOfGuess(walker, left, asked, origin);
        if (!test || !test.isValue())
            break;
        if (!place) {
            meeting = walker.addBlock();
            walker.noteGuessOpened();
            walker.count(call.opcode() == Opcode::Construct ? StatisticID::ConstructionsToldApartByCallee : StatisticID::CallsToldApartByCallee);
        }
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock();
        walker.branch(test, passed, failed);
        walker.continueIn(passed);
        if (Reference closure = buildProofOfGuess(walker, left, test, asked, origin))
            walkArmWith(closure);
        if (walker.hasFailed())
            return true;
        walker.continueIn(failed);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(left);
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(false);
        left = walker.build(WTF::move(narrowed));
        if (!left || walker.hasFailed())
            return true;
    }
    if (held && !walker.isUnreachable())
        walker.beginTurnAgainInClosedCopy();
    else if (!walker.isUnreachable())
        walkArmWith(left);
    if (!walker.hasFailed())
        walker.letArmsMeet(stretch, built, meeting);
    return true;
}

bool rowIsToldApartFromViewsInTurn(Walker& walker, const BodyValue& read, TypeID expected)
{
    if (read.opcode() != Opcode::ElemGet || !expected || !isExpectedToBeListOfNumbers(walker, walker.types().withoutNullish(expected)))
        return false;
    return !!viewsSeenToArriveWhereUseWroteRowOfNumbersForGuesses(walker, read.objectOperand());
}

static bool tellRowsApartWhereTheyMayBeToldApart(Walker&, const BodyValue& read);

bool tellRowsApartByKindOfStorage(Walker& walker, const BodyValue& read)
{
    if (tellRowsApartWhereTheyMayBeToldApart(walker, read))
        return true;
    Reference row = walker.referenceOf(read.objectOperand());
    if (walker.hasFailed() || walker.isUnreachable() || !row || !row.isValue() || hasKind(walker.types(), walker.typeOf(row), OperandKind::Array) || !walker.standsInTurnThatLooksCountOn() || !walker.mayBeginTurnAgainInClosedCopy())
        return false;
    TypeID expected = expectationOfOperandForGuesses(walker, read, read.objectOperand()).type;
    if (rowIsToldApartFromViewsInTurn(walker, read, expected))
        proveExpectedTypeOrBeginTurnAgain(walker, read.objectOperand(), expected, walker.draftOf(read).origin());
    return false;
}

static bool tellRowsApartWhereTheyMayBeToldApart(Walker& walker, const BodyValue& read)
{
    if (walker.standsInArmOfRowOfShapeTests() || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference row = walker.referenceOf(read.objectOperand());
    if (!turnOfTypedCopyMayAskList(walker, row) || walker.standsInFrontOfTypedCopy(row) || findListTestedInFrontOfLoop(walker, row))
        return false;
    TypeArena& types = walker.types();
    bool isProvenList = hasKind(types, walker.typeOf(row), OperandKind::Array);
    TypeID expected = isProvenList ? walker.typeOf(row) : expectationOfOperandForGuesses(walker, read, read.objectOperand()).type;
    expected = expected ? types.withoutNullish(expected) : TypeID();
    if (!expected || !hasKind(types, expected, OperandKind::Array))
        return false;
    std::optional<TestOfGuess> asked = isProvenList ? std::nullopt : chooseTestOfGuess(walker, row, expected);
    if (!isProvenList && !asked)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(read.id()))
        block = index.block(index.blockOf(read.id()));
    uint32_t firstOfBlock = block.firstValue().index();
    uint32_t begin = read.id().index() - firstOfBlock;
    uint32_t limit = limitOfStretchOfGuess(walker, block);
    if (begin >= limit || standsInNoStretchOfGuess(index, read))
        return false;
    ValueID source = index.valueBehindCasts(read.objectOperand());
    auto namesRow = [&](ValueID input) {
        ValueID root = index.valueBehindCasts(input);
        if (root == source)
            return true;
        if (index.isParameter(root) || index.isParameter(source) || index.opcodeOf(root) != Opcode::ElemGet || index.opcodeOf(source) != Opcode::ElemGet)
            return false;
        BodyValue one = index.value(root);
        BodyValue other = index.value(source);
        return index.valueBehindCasts(one.objectOperand()) == index.valueBehindCasts(other.objectOperand()) && index.valueBehindCasts(one.propertyOperand()) == index.valueBehindCasts(other.propertyOperand());
    };
    uint32_t end = begin + 1;
    BitVector leansOnRow;
    leansOnRow.set(0);
    for (uint32_t place = begin + 1; place < limit; ++place) {
        BodyValue operation = walker.asWalked(index.value(ValueID(firstOfBlock + place)));
        if (standsInNoStretchOfGuess(index, operation) || isReturnedAtOnceForGuesses(index, operation.id()))
            break;
        bool leans = operation.opcode() == Opcode::ElemGet && namesRow(operation.objectOperand());
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            leans |= operand && operand >= read.id() && leansOnRow.get(operand.index() - read.id().index());
        });
        if (!leans)
            continue;
        leansOnRow.set(place - begin);
        end = place + 1;
    }

    uint32_t first = firstOfBlock + begin;
    uint32_t size = end - begin;
    Vector<ValueID, 4> names;
    for (uint32_t place = 0; place < size; ++place) {
        index.value(ValueID(first + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand && operand.index() < first && walker.referenceOf(operand) == row && !names.contains(operand))
                names.append(operand);
        });
    }

    Origin origin = originInFrontOfOperationForGuesses(walker, read);
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, end);
    Walker::ArmsThatMeet built;
    BlockID meeting;
    TypeID viewsSeen;
    auto walkArmWith = [&](Reference proven) {
        Vector<Reference, 4> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, proven);
        }
        if (viewsSeen && proven != row && !walker.state().checkedNameOf(row))
            walker.state().noteCheckedName(row, proven);
        for (uint32_t place = 0; place < size; ++place) {
            walker.bind(ValueID(first + place), Reference());
            walker.rememberedExpectationOf(ValueID(first + place)) = { };
        }
        walker.walkStretch(block.id(), begin, end);
        walker.listArm(stretch, built);
        for (size_t name = 0; name < names.size(); ++name)
            walker.bind(names[name], boundBefore[name]);
    };
    Reference list = row;
    BlockID whereRowIsNoList;
    if (!isProvenList) {
        Reference isList = buildTestOfGuess(walker, row, *asked, origin);
        if (!isList || !isList.isValue())
            return false;
        viewsSeen = viewsSeenToArriveWhereUseWroteRowOfNumbersForGuesses(walker, read.objectOperand());
        bool useAloneSaysList = !!viewsSeen;
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(useAloneSaysList ? BlockFrequency::Unlikely : BlockFrequency::Rare);
        walker.branch(isList, passed, failed);
        if (useAloneSaysList)
            whereRowIsNoList = failed;
        else {
            walker.continueIn(failed);
            if (!walker.isUnreachable())
                walker.beginTurnAgainInClosedCopy();
        }
        walker.continueIn(passed);
        list = buildProofOfGuess(walker, row, isList, *asked, origin);
    }
    bool listGoesOn = list && !walker.hasFailed() && goOnWhereListIsAsMadeOrBeginTurnAgain(walker, list, origin);
    if (!listGoesOn && (!whereRowIsNoList || walker.hasFailed()))
        return false;
    walker.noteGuessOpened();
    meeting = walker.addBlock();
    SetForScope standsInArm(walker.numberOfFramesAtRowOfShapeTests(), atNoFrameOfCopies);
    if (listGoesOn) {
        walker.count(StatisticID::ListsAskedInTurnsOfTypedCopies);
        for (ArrayStorage kind : { ArrayStorage::Double, ArrayStorage::Int32, ArrayStorage::Contiguous }) {
            Reference isOfKind = askWhetherStorageOfListIsOfKind(walker, list, kind, origin);
            if (!isOfKind || !isOfKind.isValue())
                break;
            BlockID passed = walker.addBlock();
            BlockID failed = walker.addBlock(kind == ArrayStorage::Contiguous ? BlockFrequency::Rare : BlockFrequency::Normal);
            walker.branch(isOfKind, passed, failed);
            walker.continueIn(passed);
            noteKindOfStorageThatTurnFound(walker, list, kind);
            walkArmWith(list);
            if (walker.hasFailed())
                return true;
            walker.continueIn(failed);
        }
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
    }
    if (whereRowIsNoList && !walker.hasFailed()) {
        walker.continueIn(whereRowIsNoList);
        for (TypeID views : { viewsSeen }) {
            if (walker.isUnreachable())
                break;
            TestOfGuess askedOfView { views, { } };
            Reference isView = buildTestOfGuess(walker, row, askedOfView, origin);
            if (!isView || !isView.isValue())
                break;
            BlockID isOne = walker.addBlock();
            BlockID isNone = walker.addBlock(BlockFrequency::Rare);
            walker.branch(isView, isOne, isNone);
            walker.continueIn(isOne);
            if (Reference view = buildProofOfGuess(walker, row, isView, askedOfView, origin)) {
                walker.count(StatisticID::RowsAskedForViewsInTurnsOfTypedCopies);
                walkArmWith(view);
            }
            if (walker.hasFailed())
                return true;
            walker.continueIn(isNone);
        }
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
    }
    if (!walker.hasFailed())
        walker.letArmsMeet(stretch, built, meeting);
    return true;
}

static uint64_t keyOfRowForGuesses(const BodyIndex& index, ValueID input)
{
    ValueID root = index.valueBehindCasts(input);
    if (index.isParameter(root) || index.opcodeOf(root) != Opcode::ElemGet)
        return 1ull << 63 | root.index();
    BodyValue fetch = index.value(root);
    return static_cast<uint64_t>(index.valueBehindCasts(fetch.objectOperand()).index()) << 32 | index.valueBehindCasts(fetch.propertyOperand()).index();
}

static const Walker::StoresIntoRowsOfBlock& storesIntoRowsOfBlockForGuesses(Walker& walker, const BodyIndex& index, const BodyBlock& block)
{
    Walker::StoresIntoRowsOfBlock& found = walker.storesIntoRowsOfBlockOfFrame();
    if (found.body == &index && found.function == walker.currentFunction() && found.block == block.id())
        return found;
    found.body = &index;
    found.function = walker.currentFunction();
    found.block = block.id();
    found.stores.shrink(0);
    found.placesInNoStretch.shrink(0);
    uint32_t firstOfBlock = block.firstValue().index();
    for (BodyValue operation : block.values()) {
        uint32_t place = operation.id().index() - firstOfBlock;
        if (standsInNoStretchOfGuess(index, operation) || isReturnedAtOnceForGuesses(index, operation.id()))
            found.placesInNoStretch.append(place);
        else if (operation.opcode() == Opcode::ElemSet)
            found.stores.append({ keyOfRowForGuesses(index, operation.objectOperand()), place });
    }
    std::ranges::sort(found.stores);
    return found;
}

static uint32_t endOfStretchThatStoresIntoRowForGuesses(const Walker::StoresIntoRowsOfBlock& found, uint64_t row, uint32_t begin, uint32_t limit)
{
    auto ends = std::ranges::upper_bound(found.placesInNoStretch, begin);
    if (ends != found.placesInNoStretch.end())
        limit = std::min(limit, *ends);
    auto store = std::ranges::lower_bound(found.stores, std::pair { row, begin + 1 });
    auto standsInStretch = [&] {
        return store != found.stores.end() && store->first == row && store->second < limit;
    };
    if (!standsInStretch())
        return begin + 1;
    uint32_t behindFirstStore = store->second + 1;
    ++store;
    return standsInStretch() ? store->second + 1 : behindFirstStore;
}

static bool rowChangesWithTurnsOfInnermostLoopForGuesses(const BodyIndex& index, const BodyValue& read, ValueID row)
{
    LoopID innermost = index.loopOf(index.blockOf(read.id()));
    if (!innermost || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
        return false;
    auto isMadeInLoop = [&](ValueID input) {
        return index.isInLoop(index.blockOf(index.valueBehindCasts(input)), innermost);
    };
    return isMadeInLoop(index.value(row).propertyOperand()) || isMadeInLoop(index.value(row).objectOperand());
}

static Reference askWhetherValueIsListItselfForGuesses(Walker& walker, Reference list, Reference value, Origin origin)
{
    Draft isSame(Opcode::StrictEq, origin);
    walker.chooseConservativeFields(isSame);
    isSame.setLeftOperand(list);
    isSame.setRightOperand(value);
    isSame.setForm(formAllowedForTypedOperator(walker, isSame));
    return walker.build(WTF::move(isSame));
}

static void nameRowOfStretchAgainForGuesses(Walker& walker, const BodyValue& access)
{
    const Walker::StretchWithArmsAsWritten& stretch = walker.stretchWithArmsAsWritten();
    Reference named = walker.referenceOf(access.objectOperand());
    const BodyIndex& index = walker.currentBodyIndex();
    bool runsAsWrittenInstead = walker.stretchMayRunAsWrittenInstead();
    if (!named || !named.isValue() || named == stretch.list || (!runsAsWrittenInstead && (!stretch.turnBeginsAgainInstead || stretch.numberOfFrames != walker.depthOfCopies() + 1 || !walker.standsInTurnThatLooksCountOnAndCanPutBack())) || keyOfRowForGuesses(index, access.objectOperand()) != keyOfRowForGuesses(index, stretch.row))
        return;
    Reference isSame = askWhetherValueIsListItselfForGuesses(walker, stretch.list, named, originInFrontOfOperationForGuesses(walker, access));
    if (!isSame)
        return;
    BlockID passed = walker.addBlock();
    BlockID failed = runsAsWrittenInstead ? stretch.asWritten : walker.addBlock(BlockFrequency::Rare);
    walker.branch(isSame, passed, failed);
    if (!runsAsWrittenInstead) {
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
    }
    walker.continueIn(passed);
    if (walker.isUnreachable() || walker.hasFailed())
        return;
    if (!walker.state().checkedNameOf(named))
        walker.state().noteCheckedName(named, stretch.list);
    walker.bind(access.objectOperand(), stretch.list);
}

bool askRowOnceWhereStretchStoresIntoItOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (walker.standsInStretchWithArmsAsWritten()) {
        nameRowOfStretchAgainForGuesses(walker, read);
        return false;
    }
    if (read.opcode() != Opcode::ElemGet || walker.standsInArmOfRowOfShapeTests() || !walker.isInLoop() || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Folding))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID source = index.valueBehindCasts(read.objectOperand());
    if (!rowChangesWithTurnsOfInnermostLoopForGuesses(index, read, source))
        return false;
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(read.id()))
        block = index.block(index.blockOf(read.id()));
    uint32_t firstOfBlock = block.firstValue().index();
    uint32_t begin = read.id().index() - firstOfBlock;
    uint32_t limit = limitOfStretchOfGuess(walker, block);
    if (begin >= limit || standsInNoStretchOfGuess(index, read))
        return false;
    uint64_t keyOfRow = keyOfRowForGuesses(index, source);
    uint32_t end = endOfStretchThatStoresIntoRowForGuesses(storesIntoRowsOfBlockForGuesses(walker, index, block), keyOfRow, begin, limit);
    if (end == begin + 1)
        return false;

    Reference row = walker.referenceOf(read.objectOperand());
    if (!row || !row.isValue() || findListTestedInFrontOfLoop(walker, row))
        return false;
    TypeArena& types = walker.types();
    bool isProvenList = hasKind(types, walker.typeOf(row), OperandKind::Array);
    TypeID expected = isProvenList ? walker.typeOf(row) : expectationOfOperandForGuesses(walker, read, read.objectOperand()).type;
    expected = expected ? types.withoutNullish(expected) : TypeID();
    if (!expected || !isExpectedToBeListOfNumbers(walker, expected)) {
        TypeID foundByLink = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
        foundByLink = foundByLink ? types.withoutNullish(foundByLink) : TypeID();
        if (!foundByLink || !types.at(foundByLink).isNumber())
            return false;
        TypeID number = TypeTable::number;
        expected = types.builtinClass(BuiltinClasses::Array, singleElementSpan(number));
    }
    std::optional<TestOfGuess> asked = isProvenList ? std::nullopt : chooseTestOfGuess(walker, row, expected);
    if (!isProvenList && !asked)
        return false;

    uint32_t first = firstOfBlock + begin;
    uint32_t size = end - begin;
    Reference holder;
    Vector<ValueID, 4> names;
    Vector<Reference, 2> others;
    for (uint32_t place = 0; place < size; ++place) {
        ValueID walked(first + place);
        if (place && !holder && index.opcodeOf(walked) == Opcode::ElemGet && keyOfRowForGuesses(index, walked) == keyOfRow)
            holder = walker.referenceOf(index.value(source).objectOperand());
        index.value(walked).forEachOperand([&](ValueID operand, uint32_t) {
            if (!operand || operand.index() >= first || names.contains(operand))
                return;
            Reference named = walker.referenceOf(operand);
            if (named != row && (!named || !named.isValue() || keyOfRowForGuesses(index, operand) != keyOfRow))
                return;
            names.append(operand);
            if (named != row && !others.contains(named))
                others.append(named);
        });
    }
    if (holder && (!holder.isValue() || !hasKind(types, walker.typeOf(holder), OperandKind::Array) || findListTestedInFrontOfLoop(walker, holder)))
        holder = { };

    Origin origin = originInFrontOfOperationForGuesses(walker, read);
    Reference isList = isProvenList ? Reference() : buildTestOfGuess(walker, row, *asked, origin);
    if (!isProvenList && (!isList || !isList.isValue()))
        return false;
    bool turnBeginsAgainInstead = walker.mayBeginTurnAgainInClosedCopy() && (walker.standsInTurnThatLooksCountOn() || walker.standsInTurnThatLooksCountOnAndCanPutBack()) && !rowIsToldApartFromViewsInTurn(walker, read, expectationOfOperandForGuesses(walker, read, read.objectOperand()).type);
    BlockID asWritten = walker.addBlock(turnBeginsAgainInstead ? BlockFrequency::Rare : BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    auto goOnOrRunAsWritten = [&](Reference test) {
        if (!test)
            return false;
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, asWritten);
        walker.continueIn(passed);
        return !walker.isUnreachable() && !walker.hasFailed();
    };
    Reference list = row;
    bool isAsked = true;
    BlockID whereRowIsNoList;
    if (!isProvenList) {
        if (!walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked)) {
            whereRowIsNoList = walker.addBlock();
            BlockID passed = walker.addBlock();
            walker.branch(isList, passed, whereRowIsNoList);
            walker.continueIn(passed);
            isAsked = !walker.isUnreachable() && !walker.hasFailed();
        } else
            isAsked = goOnOrRunAsWritten(isList);
        list = isAsked ? buildProofOfGuess(walker, row, isList, *asked, origin) : Reference();
        isAsked = isAsked && list && !walker.hasFailed();
    }
    for (Reference other : others)
        isAsked = isAsked && goOnOrRunAsWritten(askWhetherValueIsListItselfForGuesses(walker, list, other, origin));
    isAsked = isAsked && goOnOrRunAsWritten(askWhetherListIsAsMade(walker, list, origin));
    isAsked = isAsked && goOnOrRunAsWritten(askWhetherStorageOfListIsOfKind(walker, list, ArrayStorage::Double, origin, true));
    if (holder) {
        isAsked = isAsked && goOnOrRunAsWritten(askWhetherListIsAsMade(walker, holder, origin));
        isAsked = isAsked && goOnOrRunAsWritten(askWhetherStorageOfListIsOfKind(walker, holder, ArrayStorage::Contiguous, origin));
    }
    if (walker.hasFailed())
        return true;
    walker.noteGuessOpened();

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, end);
    Walker::ArmsThatMeet built;
    auto forgetStretch = [&] {
        for (uint32_t place = 0; place < size; ++place) {
            walker.bind(ValueID(first + place), Reference());
            walker.rememberedExpectationOf(ValueID(first + place)) = { };
        }
    };
    auto walkArmOf = [&](Reference list, bool isList) {
        Vector<Reference, 4> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, list);
        }
        Walker::StretchWithArmsAsWritten inKind;
        {
            SetForScope inStretch(walker.stretchWithArmsAsWritten(), Walker::StretchWithArmsAsWritten { asWritten, turnBeginsAgainInstead ? walker.numberOfTurnOfTypedCopy() : walker.beginStretchInWhichNothingWasDone(), walker.depthOfCopies() + 1, source, list, first, size, { }, 0, { }, turnBeginsAgainInstead });
            if (isList)
                noteKindOfStorageThatTurnFound(walker, list, ArrayStorage::Double, true);
            if (holder)
                noteKindOfStorageThatTurnFound(walker, holder, ArrayStorage::Contiguous);
            walker.walkStretch(block.id(), begin, end);
            walker.listArm(stretch, built);
            inKind = std::exchange(walker.stretchWithArmsAsWritten(), { });
        }
        if (inKind.restAsWritten && !walker.hasFailed()) {
            for (uint32_t place = 0; place < size; ++place) {
                walker.bind(ValueID(first + place), place < inKind.placeOfRest ? inKind.boundInFrontOfRest[place] : Reference());
                if (place >= inKind.placeOfRest)
                    walker.rememberedExpectationOf(ValueID(first + place)) = { };
            }
            walker.continueIn(inKind.restAsWritten);
            if (!walker.isUnreachable()) {
                Walker::ClosedArm closedArm(walker);
                walker.walkStretch(block.id(), begin + inKind.placeOfRest, end);
                walker.listArm(stretch, built);
            }
        }
        for (size_t name = 0; name < names.size(); ++name)
            walker.bind(names[name], boundBefore[name]);
    };
    if (isAsked) {
        walker.count(StatisticID::RowsAskedOnceWhereStretchesStoreIntoThem);
        if (turnBeginsAgainInstead)
            walker.count(StatisticID::RowsAskedOnceWhereTurnsBeginAgain);
        walkArmOf(list, true);
        if (walker.hasFailed())
            return true;
    } else if (!walker.isUnreachable())
        walker.jumpTo(asWritten);
    Vector<TypeID, 4> kindsOfViewsAsked;
    if (whereRowIsNoList) {
        kindsOfViewsAsked.append(types.builtinClass(BuiltinClasses::Float64Array));
        if (TypeID seen = viewsSeenToArriveWhereUseWroteRowOfNumbersForGuesses(walker, read.objectOperand())) {
            const BuiltinTable& table = walker.program().builtins();
            for (unsigned position = 0; position < table.classCount(); ++position) {
                TypeID kind = types.builtinClass(BuiltinClassID(position));
                if (hasInlineElementAccess(table.typedArrayTypeOf(BuiltinClassID(position))) && !kindsOfViewsAsked.contains(kind) && types.overlap(seen, kind))
                    kindsOfViewsAsked.append(kind);
            }
        }
    }
    for (size_t number = 0; number < kindsOfViewsAsked.size(); ++number) {
        forgetStretch();
        walker.continueIn(whereRowIsNoList);
        TestOfGuess askedOfView { kindsOfViewsAsked[number], { } };
        bool viewIsAsked = !walker.isUnreachable();
        Reference isView = viewIsAsked ? buildTestOfGuess(walker, row, askedOfView, origin) : Reference();
        if (viewIsAsked && isView && number + 1 < kindsOfViewsAsked.size()) {
            whereRowIsNoList = walker.addBlock();
            BlockID passed = walker.addBlock();
            walker.branch(isView, passed, whereRowIsNoList);
            walker.continueIn(passed);
            viewIsAsked = !walker.isUnreachable() && !walker.hasFailed();
        } else {
            viewIsAsked = viewIsAsked && goOnOrRunAsWritten(isView);
            kindsOfViewsAsked.shrink(number + 1);
        }
        Reference view = viewIsAsked ? buildProofOfGuess(walker, row, isView, askedOfView, origin) : Reference();
        viewIsAsked = viewIsAsked && view && !walker.hasFailed();
        for (Reference other : others)
            viewIsAsked = viewIsAsked && goOnOrRunAsWritten(askWhetherValueIsListItselfForGuesses(walker, view, other, origin));
        if (holder) {
            viewIsAsked = viewIsAsked && goOnOrRunAsWritten(askWhetherListIsAsMade(walker, holder, origin));
            viewIsAsked = viewIsAsked && goOnOrRunAsWritten(askWhetherStorageOfListIsOfKind(walker, holder, ArrayStorage::Contiguous, origin));
        }
        if (walker.hasFailed())
            return true;
        if (viewIsAsked) {
            walker.count(StatisticID::RowsThatAreNoListsAskedForViews);
            walkArmOf(view, false);
            if (walker.hasFailed())
                return true;
        } else if (!walker.isUnreachable())
            walker.jumpTo(asWritten);
    }

    forgetStretch();
    walker.continueIn(asWritten);
    if (turnBeginsAgainInstead) {
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
    } else if (!walker.isUnreachable()) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), begin, end);
        walker.listArm(stretch, built);
    }
    if (!walker.hasFailed())
        walker.letArmsMeet(stretch, built, meeting);
    return true;
}

constinit const OpcodeSet opcodesThatOpenGuessAtTakes = operationsThatOpenGuesses;
static_assert(opcodesThatOpenGuessAtTakesInChains == operationsThatOpenGuesses);

static GuessOpened openGuessAtOperationForGuesses(Walker&, const BodyValue&, bool expectsOnlyProvenHolderOfName);

GuessOpened openGuessAtOfItsOpcodes(Walker& walker, const BodyValue& operation)
{
    return openGuessAtOperationForGuesses(walker, operation, false);
}

GuessOpened openGuessAtOnlyProvenHolderOfName(Walker& walker, const BodyValue& read)
{
    Reference object = walker.referenceOf(read.objectOperand());
    TypeID proven = object && object.isValue() ? walker.typeOf(object) : TypeID();
    if (!proven)
        return GuessOpened::None;
    TypeArena& types = walker.types();
    TypeID present = types.withoutNullish(proven);
    Type type = types.at(present);
    if (TypeID objects = walker.isInLoop() ? types.withOnlyObjects(present) : present; objects != present && types.at(objects).hasObjects() && types.at(objects).hasOnlyLayouts() && !types.at(objects).hasOnlyClosures())
        return openGuessAtOperationForGuesses(walker, read, true);
    bool holdsSeveral = holdsLayoutsBesideOtherObjectsForGuesses(type) || (type.hasOnlyLayouts() && !holdsOneMember(type.layouts));
    if (!type.hasObjects() || !holdsSeveral || narrowedByNameAskedForGuesses(walker, present, read.key()) == present)
        return GuessOpened::None;
    return openGuessAtOperationForGuesses(walker, read, true);
}

struct MembersAskedInTurnForGuesses {
    TypeID first;
    TypeID second;
    bool failingSideIsAsLikely { false };
};

static TypeID literalsWithoutNamesAmongForGuesses(Walker& walker, TypeID expected)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::LiteralsWithoutNames);
    TypeArena& types = walker.types();
    Type type = types.at(expected);
    auto askEachLayout = [&]() -> TypeID {
        const LayoutsByNames& byNames = walker.facts().layoutsByNames();
        const LinkTables& linked = walker.facts().linked();
        Vector<LayoutID, 4> literals;
        for (LayoutID layout : types.listOf(type.layouts)) {
            if (walker.program().at(layout).kind == LayoutKind::Record && layout.index() < linked.layouts.size() && linked.layouts[layout.index()].isAllocated && !linked.layouts[layout.index()].isFilledUnderComputedKeys && !byNames.isInUse(layout))
                literals.append(layout);
        }
        return literals.isEmpty() ? TypeID() : types.forLayouts(literals.span());
    };
    if (types.keeps() != ArenaKeeps::UntilReset) {
        if (!Walker::keepsRoundsOver(types, type.layouts))
            return askEachLayout();
        if (std::optional<uint32_t> kept = walker.roundKept(Walker::RoundKeptByWalker::LiteralsWithoutNames, type.layouts, 0))
            return TypeID::fromBits(*kept);
        TypeID literals = askEachLayout();
        walker.keepRound(Walker::RoundKeptByWalker::LiteralsWithoutNames, type.layouts, 0, literals.bits());
        return literals;
    }
    TypeArena::SetOfRound round = types.setOfRound(type.layouts);
    if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, 0))
        return TypeID::fromBits((*kept)[0]);
    TypeID literals = askEachLayout();
    types.rememberRound(number, 1, literals ? 1 : 0, round, 0, { literals.bits() });
    return literals;
}

static MembersAskedInTurnForGuesses membersThatMeetAtJoinForGuesses(Walker& walker, ValueID asked, TypeID expected)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    asked = index.valueBehindCasts(asked);
    while (index.isParameter(asked) && index.blockOf(asked) != entryBlock && index.valueNarrowedInto(asked))
        asked = index.valueBehindCasts(index.valueNarrowedInto(asked));
    if (!index.isParameter(asked) || index.isEnteredByHandlerEdge(index.blockOf(asked)))
        return { };
    auto holdsLayoutsAlone = [&](TypeID held) {
        Type type = types.at(held);
        return type.isObject() && type.hasOnlyLayouts() && !type.hasOnlyClosures();
    };
    if (!holdsLayoutsAlone(expected) || holdsOneMember(types.at(expected).layouts))
        return { };
    auto someMemberAnswersNameUsed = [&](TypeID members) {
        std::span<const PropertyKeyID> names = walker.namesUsedOff(asked);
        const LayoutsByNames& byNames = walker.facts().layoutsByNames();
        return std::ranges::any_of(types.listOf(types.at(members).layouts), [&](LayoutID layout) {
            return std::ranges::any_of(names, [&](const PropertyKeyID& name) {
                return byNames.hasAll(layout, singleElementSpan(name));
            });
        });
    };
    if (index.blockOf(asked) == entryBlock) {
        if (walker.currentFunction().index() >= walker.facts().linked().functions.size())
            return { };
        std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(walker.currentFunction());
        if (asked.index() >= parameters.size() || saysNothingForGuesses(types, parameters[asked.index()].writtenOrSeenToArrive()))
            return { };
        MembersAskedInTurnForGuesses seen;
        seen.second = literalsWithoutNamesAmongForGuesses(walker, expected);
        seen.first = seen.second ? types.differenceOf(expected, seen.second) : TypeID();
        if (!seen.first || isNeverType(seen.first) || !holdsLayoutsAlone(seen.first) || !someMemberAnswersNameUsed(seen.first))
            return { };
        return { seen.first, { }, true };
    }
    BlockID block = index.blockOf(asked);
    uint32_t position = asked.index() - index.block(block).parameter(0).index();
    MembersAskedInTurnForGuesses members;
    TypeID writtenOrSeenOnOtherWays;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID argument = position < arguments.size() ? arguments[position] : ValueID();
        if (!argument || argument >= asked)
            return { };
        ValueID root = index.valueBehindCasts(argument);
        if (!index.isParameter(root) && (index.opcodeOf(root) == Opcode::NewObject || index.opcodeOf(root) == Opcode::Data)) {
            TypeID made = provenTypeOfInputForGuesses(walker, argument);
            if (!made || !holdsLayoutsAlone(made))
                made = findExpectationForGuesses(walker, argument).type;
            if (!made || !holdsLayoutsAlone(made))
                return { };
            members.second = members.second ? types.unionOf(members.second, made) : made;
            continue;
        }
        while (index.isParameter(root) && index.blockOf(root) != entryBlock && index.valueNarrowedInto(root))
            root = index.valueBehindCasts(index.valueNarrowedInto(root));
        TypeID writtenOrSeen;
        if (index.isParameter(root) && index.blockOf(root) == entryBlock && walker.currentFunction().index() < walker.facts().linked().functions.size()) {
            std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(walker.currentFunction());
            if (root.index() < parameters.size() && !saysNothingForGuesses(types, parameters[root.index()].writtenOrSeenToArrive()))
                writtenOrSeen = parameters[root.index()].writtenOrSeenToArrive();
        }
        if (!writtenOrSeen) {
            ExpectationForGuesses found = findExpectationForGuesses(walker, argument);
            if (found.type && (found.isDeclared || found.isHeld))
                writtenOrSeen = found.type;
        }
        if (!writtenOrSeen)
            return { };
        writtenOrSeenOnOtherWays = writtenOrSeenOnOtherWays ? types.unionOf(writtenOrSeenOnOtherWays, writtenOrSeen) : writtenOrSeen;
    }
    if (!members.second || !types.isSubset(members.second, expected))
        return { };
    members.first = types.differenceOf(expected, members.second);
    if (!members.first || !holdsLayoutsAlone(members.first) || !chooseTestOfTypeForGuesses(walker, members.second))
        return { };
    const LinkTables& linked = walker.facts().linked();
    bool othersAreFilledUnderComputedKeys = std::ranges::all_of(types.listOf(types.at(members.first).layouts), [&](LayoutID layout) {
        return layout.index() < linked.layouts.size() && linked.layouts[layout.index()].isFilledUnderComputedKeys;
    });
    if (othersAreFilledUnderComputedKeys || !writtenOrSeenOnOtherWays || !types.isSubset(members.first, writtenOrSeenOnOtherWays))
        return { members.second, { }, true };
    if (!someMemberAnswersNameUsed(members.first))
        return { };
    return members;
}

static bool ownLiteralMeetsWhatArrivesForGuesses(Walker& walker, ValueID asked, TypeID expected)
{
    TypeArena& types = walker.types();
    TypeID objects = expected ? types.withoutNullish(expected) : TypeID();
    return objects && !isNeverType(objects) && !!membersThatMeetAtJoinForGuesses(walker, asked, objects).first;
}

bool askBaseInFrontOfTypedCopyForList(Walker& walker, const BodyValue& access)
{
    bool readsLength = access.opcode() == Opcode::PropGet && !access.isFreeName() && walker.program().nameClassOf(access.key()) == NameClass::Length;
    if ((access.opcode() != Opcode::ElemGet && !readsLength) || !walker.standsInTurnThatLooksCountOn() || !walker.mayBeginTurnAgainInClosedCopy() || !walker.otherCopyOfTypedCopyOpensItsGuesses() || !walker.everyTurnOfTypedCopyThatComesRoundPassesHere())
        return false;
    Reference base = walker.referenceOf(access.objectOperand());
    if (!base || !base.isValue() || !walker.types().saysNothing(walker.typeOf(base)) || !walker.standsInFrontOfTypedCopy(base) || walker.isWhereArmsMetInFrontOfTypedCopy(base) || findExpectationForGuesses(walker, access.objectOperand()).type || otherNamesAreReadOffPlaceOfForGuesses(keptWithBodyForGuesses(walker.currentBodyIndex()), access.objectOperand()))
        return false;
    if (!readsLength && !walker.types().at(provenTypeOfInputForGuesses(walker, access.propertyOperand())).isNumber())
        return false;
    std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, base, walker.types().builtinClass(BuiltinClasses::Array));
    if (!chosen || chosen->isPassedAlready)
        return false;
    chosen->admitsAllThatIsExpected = true;
    BodyBlock block = walker.currentBodyIndex().block(walker.currentBodyIndex().blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    walker.count(StatisticID::BasesInFrontOfTypedCopiesAskedForLists);
    walker.noteThatFrontAsksWhatNothingSpeaksFor();
    openGuessWithTest(walker, base, *chosen, block.id(), place, StretchOfGuess { place + 1, 0, { } });
    return true;
}

static GuessOpened openGuessAtOperationForGuesses(Walker& walker, const BodyValue& operation, bool expectsOnlyProvenHolderOfName)
{
    Opcode opcode = operation.opcode();
    if (!operationsThatOpenGuesses.contains(opcode))
        return GuessOpened::None;
    if (guessThatOperandBesideStringHasText(walker, operation))
        return GuessOpened::WithArms;
    bool isInLoop = walker.isInLoop();
    if (noGuessIsOpenedWhereWalkStandsForGuesses(walker, isInLoop))
        return GuessOpened::None;
    if (opcode == Opcode::Call && (forwardCallBehindTestOfCallee(walker, operation) || callTargetOfBoundFunctionBehindTests(walker, operation) || openGuessAtCalleeOfReturnedCall(walker, operation)))
        return GuessOpened::WithArms;
    if (Reference holder = opcode == Opcode::PropGet && walker.program().admitsReplacedBuiltins ? walker.referenceOf(operation.objectOperand()) : Reference(); holder && findMemberAskedByItsByteOffType(walker, walker.typeOf(holder), operation.key()))
        return askBytesOfMembersReadOffBuiltins(walker, operation, holder) ? GuessOpened::WithArms : GuessOpened::None;
    if (opcodesThatOpenGuessOfKeyOrdersTakes.contains(opcode) && nameMayOpenGuessOfKeyOrdersForGuesses(walker, operation) && openGuessOfKeyOrders(walker, operation))
        return GuessOpened::WithArms;
    if (opcode == Opcode::ElemGet && isInLoop && callGetterExpectedAtRead(walker, operation))
        return GuessOpened::WithArms;
    if (opcode == Opcode::PropGet && (callGetterExpectedAtRead(walker, operation) || (isInLoop && callMethodsBehindTestsOfShape(walker, operation)) || takeApartCallThatFindsBuiltinForGuesses(walker, operation) || askGuardOfNameReadOffConstructorFunction(walker, operation) || askGuardsOfMethodReadOffPrototypeOfClass(walker, operation)))
        return GuessOpened::WithArms;
    if (isInLoop && callsToldApartByCalleeForGuesses.contains(opcode) && callEachFunctionOfProvenSetForGuesses(walker, operation))
        return GuessOpened::WithArms;
    if (callFunctionGivenToVariable(walker, operation))
        return GuessOpened::WithArms;

    bool accessesSlot = accessesToSlotsForGuesses.contains(opcode);
    bool isUnderPrivateName = accessesUnderPrivateNamesForGuesses.contains(opcode);
    ValueID object = accessesSlot ? operation.objectOperand() : ValueID();
    TypeID provenOfObject = accessesSlot && !isUnderPrivateName ? provenTypeOfInputForGuesses(walker, object) : TypeID();
    bool mayHaveTypedForm = accessesSlot && (isUnderPrivateName || !saysNothingForGuesses(walker.types(), provenOfObject));
    ExpectationForGuesses firstExpected;
    uint32_t indexOfFirstExpected = UINT32_MAX;
    if (!mayHaveTypedForm && nothingIsExpectedOfOperandsForGuesses(walker, operation, firstExpected, indexOfFirstExpected))
        return GuessOpened::None;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(operation.id()))
        block = index.block(index.blockOf(operation.id()));
    uint32_t begin = operation.id().index() - block.firstValue().index();
    uint32_t limit = limitOfStretchOfGuess(walker, block);
    if (begin >= limit) {
        if (opcode == Opcode::ToBoolean)
            walker.leaveQuestionOfTruthToItsOwnTests(operation.id());
        return GuessOpened::None;
    }

    TypeID askedOfStoredValue;
    if (mayHaveTypedForm && storesIntoSlotsForGuesses.contains(opcode) && walker.referenceOf(operation.valueOperand()).isValue())
        askedOfStoredValue = typeAskedOfStoredValueForGuesses(walker, operation, provenOfObject, provenTypeOfInputForGuesses(walker, operation.valueOperand()));
    FirstOperationOfGuessAsItStands asItStands { operation.words(), TypedForm::None, { }, askedOfStoredValue };
    if (mayHaveTypedForm) {
        walker.takeFormAskedAhead();
        asItStands.form = typedFormOf(walker, operation, [&](ValueID input) {
            TypeID proven = provenOfObject && input == object ? provenOfObject : provenTypeOfInputForGuesses(walker, input);
            return askedOfStoredValue && input == operation.valueOperand() ? walker.types().intersectionOf(proven, askedOfStoredValue) : proven;
        });
        asItStands.askedAhead = walker.takeFormAskedAhead();
    }
    TypedForm formAsItStands = asItStands.form;
    bool lacksOnlyFilledSlot = formAsItStands == TypedForm::BuiltBehindFilledSlot || formAsItStands == TypedForm::BuiltBehindEmptySlot || formAsItStands == TypedForm::BuiltOnObjectAsMade
        || formAsItStands == TypedForm::BuiltBehindLookAtCell || formAsItStands == TypedForm::BuiltBehindFilledSlotAndLookAtCell || (askedOfStoredValue && formAsItStands == TypedForm::Built);

    bool isCall = callsOfValuesForGuesses.contains(opcode);
    bool readsMethodForItsCall = isReadOfMethodForItsCallForGuesses(index, operation);
    GuessOpened opened = GuessOpened::None;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (opened != GuessOpened::None || isOnlyHandedOnForGuesses(operation, operandIndex))
            return;
        Reference value = walker.referenceOf(operand);
        bool isWrittenPrimitive = value && value.isConstant() && operand == object && formAsItStands == TypedForm::BuiltOnObjectAsMade && !walker.types().at(walker.typeOf(value)).hasObjects();
        if (!value || (!value.isValue() && !isWrittenPrimitive))
            return;

        ExpectationForGuesses expectation;
        std::optional<TestOfGuess> chosen;
        bool otherObjectsAnswerNameToo = false;
        if (lacksOnlyFilledSlot) {
            if (operand != object)
                return;
            expectation = { walker.typeOf(value), true };
            chosen = TestOfGuess { expectation.type, { }, true };
        } else {
            if (operandIndex == indexOfFirstExpected)
                expectation = firstExpected;
            else {
                expectation = isCall
                    ? expectationOfCalleeForGuesses(walker, operation, operandIndex)
                    : expectationOfOperandForGuesses(walker, operation, operand);
            }
            if (expectation.type && walker.standsInTurnThatLooksCountOn() && !isCall) {
                ExpectationOfNumber found { expectation.type, expectation.isDeclared, expectation.isHeld };
                expectation.type = (isReadOfVariableForGuesses(index, operand) ? numbersExpectedOf(walker, operand, found) : numbersExpectedOfSlotThatTurnsStep(walker, operand, found)).type;
            }
            if (operand == object && expectsOnlyProvenHolderOfName && (saysNothingForGuesses(walker.types(), expectation.type) || !walker.types().at(expectation.type).hasObjects()))
                expectation = { narrowedByNameAskedForGuesses(walker, walker.types().withOnlyObjects(walker.types().withoutNullish(provenOfObject)), operation.key()), true };
            if (operand == object)
                expectation.type = partThatOwnAllocationMakesForGuesses(walker, index, operand, index.blockOf(operation.id()), expectation.type);
            if (isUnderPrivateName && operand == object)
                expectation = expectationOfHolderOfPrivateNameForGuesses(walker, operation, expectation);
            else if (operand == object && expectation.type && holdsLayoutsBesideOtherObjectsForGuesses(walker.types().at(expectation.type))) {
                TypeID narrowed = narrowedByNameAskedForGuesses(walker, expectation.type, operation.key());
                otherObjectsAnswerNameToo = narrowed != expectation.type && walker.program().builtinNameOf(operation.key());
                expectation.type = narrowed;
            }
            else if (operand == object && !index.isParameter(operand) && index.opcodeOf(operand) == Opcode::NarrowByField) {
                TypeID narrowed = narrowedByNameAskedForGuesses(walker, expectation.type, operation.key());
                bool isOneLayout = narrowed && walker.types().at(narrowed).hasOnlyLayouts() && holdsOneMember(walker.types().at(narrowed).layouts);
                if (isOneLayout)
                    expectation.type = narrowed;
                else
                    expectation = findExpectationForGuesses(walker, index.value(operand).as<Opcode::NarrowByField>().valueOperand());
            }
        }
        uint32_t least = 0;
        if (walker.has(Mechanism::Guesses) && expectation.type)
            least = isInLoop ? minimumOperationsPerGuessInLoop : leastOperationsThatOpenGuessOutsideLoops(walker, expectation.type, expectation.isDeclared, opcode, readsMethodForItsCall);
        bool testsForFunctionWrittenForField = isCall && least && expectation.isDeclared && !expectation.isHeld && functionWrittenForFieldThatCalleeIsReadOutOf(walker, operation);
        if (testsForFunctionWrittenForField)
            least = 1;
        if (least > 1 && formAsItStands == TypedForm::BuiltOnObjectAsMade && readsMethodForItsCall) {
            Type receiver = walker.types().at(walker.types().withoutNullish(expectation.type));
            const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), receiver);
            if (!receiver.hasObjects() || (onlyClass && onlyClass->builtinClass == BuiltinClasses::Array))
                least = 1;
        }
        bool callIsLoopOverTable = false;
        bool isOnlyExpected = expectation.type && !expectation.isDeclared && !expectation.isHeld;
        if (readsMethodForItsCall && (expectation.isDeclared || isOnlyExpected) && walker.has(Mechanism::Guesses) && walker.has(Mechanism::Inlining) && callOfMethodIsHandedClosureMadeForItForGuesses(index, operation)) {
            const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), walker.types().at(walker.types().withoutNullish(expectation.type)));
            ValueID call = index.onlyUsingValue(operation.id());
            uint32_t firstValue = block.firstValue().index();
            bool stretchReachesCall = block.holds(call) && call.index() - firstValue < limit;
            for (uint32_t place = begin; stretchReachesCall && firstValue + place <= call.index(); ++place)
                stretchReachesCall = !standsInNoStretchOfGuess(index, walker.asWalked(index.value(ValueID(firstValue + place))));
            bool isList = onlyClass && onlyClass->builtinClass == BuiltinClasses::Array;
            bool isDeclaredTable = expectation.isDeclared && onlyClass && (onlyClass->builtinClass == BuiltinClasses::Map || onlyClass->builtinClass == BuiltinClasses::Set || (isList && formAsItStands == TypedForm::BuiltOnObjectAsMade && isReadOutOfVariableOfFileThatStays(walker, operation.objectOperand())));
            callIsLoopOverTable = stretchReachesCall && (isDeclaredTable || (isOnlyExpected && isList));
        }
        if (callIsLoopOverTable)
            least = 1;
        if (!least)
            return;
        auto buildAsOneOperationOrLeaveClosed = [&] {
            if (buildLoneReadOfElementOfGuess(walker, operation, operand, value, expectation))
                opened = GuessOpened::AsOneOperation;
        };
        if (least > 2 * (limit - begin))
            return buildAsOneOperationOrLeaveClosed();
        bool proofOutlivesStretch = readsMethodForItsCall && isInLoop && operand == object && (walker.mayBeginTurnAgainInClosedCopy() || walker.standsInFrontOfTypedCopy(value, Walker::TurnOfTypedCopy::HasRunNoOtherCode));
        bool readsOffListHandedToUnit = least == minimumOperationsPerGuess && !isInLoop && opcode == Opcode::PropGet && operand == object && !readsMethodForItsCall && isListHandedToUnitForGuesses(walker, index, operand, expectation);
        if (!proofOutlivesStretch && !readsOffListHandedToUnit && (least > 1 || readsMethodForItsCall) && mostOperationsThatStretchCountsForGuesses(walker, value, lacksOnlyFilledSlot, block, begin, limit, least) < least)
            return buildAsOneOperationOrLeaveClosed();
        if (!chosen)
            chosen = chooseTestOfGuess(walker, value, expectation.type);
        TypeID membersLeftToFailingSide;
        if (operand == object && !isUnderPrivateName && !lacksOnlyFilledSlot) {
            MembersAskedInTurnForGuesses members = membersThatMeetAtJoinForGuesses(walker, operand, walker.types().withoutNullish(expectation.type));
            if (std::optional<TestOfGuess> ofWhatArrives = members.first ? chooseTestOfGuess(walker, value, members.first) : std::nullopt) {
                chosen = ofWhatArrives;
                membersLeftToFailingSide = members.second;
                chosen->failingSideIsAsLikely |= members.failingSideIsAsLikely;
            }
        }
        std::optional<Walker::WeighsWhatIsWritten> weighsWhatIsWritten;
        weighsWhatIsWritten.emplace(walker, expectation.isDeclared && !expectation.isHeld && !lacksOnlyFilledSlot && !isInLoop && !walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked));
        StretchOfGuess stretch = chosen ? findStretchOfGuess(walker, value, *chosen, block, begin, limit, lacksOnlyFilledSlot ? &asItStands : nullptr) : StretchOfGuess();
        if (std::optional<TestOfGuess> look = (opcode == Opcode::PropGet || opcode == Opcode::PropSet) && operand == object && !lacksOnlyFilledSlot ? chooseLookAtStructureOfClassForGuesses(walker, value, expectation.type) : std::nullopt) {
            StretchOfGuess stretchBehindLook = findStretchOfGuess(walker, value, *look, block, begin, limit);
            if (stretchBehindLook.numberOfTypedOperations && stretchBehindLook.numberOfTypedOperations >= stretch.numberOfTypedOperations) {
                chosen = look;
                stretch = WTF::move(stretchBehindLook);
            }
        }
        if (TypeID part = lacksOnlyFilledSlot || !chosen ? expectation.type : partWhoseClassesHaveGuardsForGuesses(walker, expectation.type); part != expectation.type) {
            if (std::optional<TestOfGuess> testOfPart = chooseTestOfGuess(walker, value, part)) {
                StretchOfGuess stretchOfPart = findStretchOfGuess(walker, value, *testOfPart, block, begin, limit);
                if (stretchOfPart.numberOfTypedOperations > stretch.numberOfTypedOperations) {
                    chosen = testOfPart;
                    stretch = WTF::move(stretchOfPart);
                }
            }
        }
        bool asksForNameOfSomeMembers = ((expectation.isDeclared && !expectation.isHeld) || isInLoop) && !lacksOnlyFilledSlot && stretch.numberOfTypedOperations < least && !isUnderPrivateName && operand == object;
        if (asksForNameOfSomeMembers) {
            UnknownSeeds seeds = walker.types().at(walker.typeOf(value)).seeds;
            asksForNameOfSomeMembers = seeds.isEmpty() || !seeds.containsOnly({ UnknownSeed::ResultOfUnknownCallee, UnknownSeed::UndescribedNativeResult });
        }
        if (TypeID part = asksForNameOfSomeMembers ? partThatHasNameAskedForGuesses(walker, expectation.type, operation.key()) : expectation.type; part != expectation.type) {
            if (std::optional<TestOfGuess> testOfPart = chooseTestOfGuess(walker, value, part)) {
                StretchOfGuess stretchOfPart = findStretchOfGuess(walker, value, *testOfPart, block, begin, limit);
                if (stretchOfPart.numberOfTypedOperations > stretch.numberOfTypedOperations) {
                    chosen = testOfPart;
                    stretch = WTF::move(stretchOfPart);
                }
            }
        }
        weighsWhatIsWritten.reset();
        if (!chosen)
            return;
        if (stretch.end > begin) {
            uint32_t firstValue = block.firstValue().index();
            if (ValueID test = findTestOfEndThatTakesJoinApart(walker, index.value(ValueID(firstValue + stretch.end - 1)), value); test && test.index() < firstValue + limit)
                stretch.end = test.index() + 1 - firstValue;
        }
        if (callIsLoopOverTable && stretch.numberOfTypedOperations < least) {
            stretch.end = index.onlyUsingValue(operation.id()).index() + 1 - block.firstValue().index();
            stretch.numberOfTypedOperations = least;
        }
        if (proofOutlivesStretch && stretch.numberOfTypedOperations < least && (stretch.looksWhetherValueIsAsMade || !stretch.firstAccessesToSlots.isEmpty() || !stretch.storesThatLookAtCell.isEmpty()) && testAdmitsAllThatIsExpectedForGuesses(walker, operation, operand, operandIndex, expectation.type, chosen->tested)) {
            stretch.end = std::max(stretch.end, begin + 1);
            stretch.numberOfTypedOperations = least;
        }
        BranchOfGuessAskedAbout branchAskedAbout;
        bool opensForWhatStandsBehindBranch = readsOffListHandedToUnit && stretch.numberOfTypedOperations && stretch.numberOfTypedOperations < least && armsOfGuessTakeBranch(walker, block.id(), begin, stretch.end, value, stretch.placesThatTypedRunKnowsBetter, &branchAskedAbout);
        if (opensForWhatStandsBehindBranch)
            stretch.numberOfTypedOperations = least;
        if (stretch.numberOfTypedOperations < least || (chosen->layouts && walker.copyIsWalkedWhereItsGuardFailed() && isParameterThatFrontEndGuessedByNamesAloneForGuesses(walker, operand)))
            return buildAsOneOperationOrLeaveClosed();
        chosen->admitsAllThatIsExpected = !otherObjectsAnswerNameToo && testAdmitsAllThatIsExpectedForGuesses(walker, operation, operand, operandIndex, expectation.type, chosen->tested);
        chosen->failingSideKeepsSites = isInLoop && expectation.restsOnNamesUsedAlone;
        chosen->failingSideIsAsLikely |= otherObjectsAnswerNameToo || (testedLayoutsAnswerNoNameUsedForGuesses(walker, operand, chosen->tested) && nothingIsKnownOfSomeWayIntoJoinForGuesses(walker, operand));
        if (membersLeftToFailingSide)
            chosen->failingSideAsksFor = membersLeftToFailingSide;
        if (const LinkedCallThatFindsBuiltin* found = callsOfValuesForGuesses.contains(operation.opcode()) ? walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), operation.id()) : nullptr) {
            if (found->callee && chosen->tested == walker.types().forFunction(found->callee) && operand == operation.calleeOperand() && isTestableBuiltin(walker.program().builtins(), found->builtin)) {
                chosen->failingSideAsksFor = walker.types().forBuiltin(found->builtin);
                chosen->failingSideIsAsLikely = true;
            }
            stretch.end = std::max(stretch.end, endOfWhatTakesResultForGuesses(walker, block, begin, limit));
        }
        if (isInLoop && operand == ((opcode == Opcode::ElemGet || opcode == Opcode::ElemSet) ? operation.objectOperand() : object) && !chosen->failingSideAsksFor && !chosen->isPassedAlready && !chosen->layouts && hasKind(walker.types(), chosen->tested, OperandKind::Array)) {
            if (TypeID views = walker.mayLeaveCopyForCall() ? TypeID() : viewsThatMayComeWhereUseWroteListForGuesses(walker, operand, expectation.type)) {
                chosen->failingSideAsksFor = views;
                chosen->failingSideIsAsLikely = true;
                walker.count(StatisticID::GuessesAtListsWhoseFailingSideAsksForViews);
            }
        }
        if (isInLoop && allBitwiseOperators.contains(opcode) && !chosen->failingSideAsksFor && !chosen->isPassedAlready && !chosen->layouts && walker.types().at(chosen->tested).isInteger()
            && walker.mayLeaveCopyForCall() && !walker.mayBeginTurnAgainInClosedCopy() && !walker.standsInTurnOfTypedCopy()) {
            chosen->failingSideAsksFor = TypeTable::number;
            walker.count(StatisticID::GuessesAtInt32sOfOperatorsOnBitsWhoseFailingSideAsksForNumbers);
        }
        if (isInLoop && opcode == Opcode::PropGet && operand == object && !chosen->failingSideAsksFor && !chosen->isPassedAlready && !chosen->layouts && hasKind(walker.types(), chosen->tested, OperandKind::Array)
            && !walker.mayBeginTurnAgainInClosedCopy() && !walker.mayLeaveCopyForCall() && listBesideTextInWrittenWordForGuesses(walker, operand))
            chosen->failingSideAsksFor = TypeTable::string;
        if (opcode == Opcode::ToBoolean && !chosen->isPassedAlready && !chosen->layouts && stretch.end == begin + 1 && walker.isInLoop() && walker.types().at(chosen->tested).hasOnlyPrimitives(Primitive::Boolean)
            && index.hasOneUse(operation.id()) && walker.isAskedByBranchOfItsBlock(operation) && !testOfGuessStaysOperationOfItsOwn(walker, value)) {
            walker.leaveQuestionOfTruthToItsOwnTests(operation.id());
            return;
        }
        if (opcode == Opcode::ToBoolean && !chosen->isPassedAlready && stretch.end == begin + 1 && walker.types().at(chosen->tested).isObject()) {
            if (chosen->layouts && !armsOfGuessTakeBranch(walker, block.id(), begin, stretch.end, value, stretch.placesThatTypedRunKnowsBetter, &branchAskedAbout) && !standsBehindTestOfGuess(walker.code(), value, TestOfGuess { TypeTable::object, { } })) {
                walker.leaveQuestionOfTruthToItsOwnTests(operation.id());
                return;
            }
            chosen->failingSideAsksForNothing = true;
        }
        if (!chosen->failingSideAsksFor && !(chosen->admitsAllThatIsExpected && (walker.mayBeginTurnAgainInClosedCopy() || walker.mayLeaveCopyForCall())) && (buildLoneReadOfGuess(walker, operation, value, *chosen, block.id(), begin, stretch, branchAskedAbout) || buildLoneStoreOfGuess(walker, operation, value, *chosen, block.id(), begin, stretch, branchAskedAbout))) {
            opened = GuessOpened::AsOneOperation;
            return;
        }
        if (testsForFunctionWrittenForField)
            walker.noteCalleeTestedForFunctionWrittenForItsField();
        if (opensForWhatStandsBehindBranch)
            walker.count(StatisticID::ListsHandedInAskedOnceForWhatStandsBehindBranch);
        openGuessWithTest(walker, value, *chosen, block.id(), begin, stretch, &branchAskedAbout);
        opened = GuessOpened::WithArms;
    });
    return opened;
}

void forEachTypeThatGuessWouldProveAt(Walker& walker, const BodyValue& operation, const TypesOfInputs& typesOfInputs, NOESCAPE const ScopedLambda<void(ValueID operand, TypeID expected, TypeID proven)>& functor)
{
    if (!operationsThatOpenGuesses.contains(operation.opcode()) || accessesUnderPrivateNamesForGuesses.contains(operation.opcode()))
        return;
    TypeArena& types = walker.types();
    Vector<ValueID, 16> remembered;
    bool forgetsItself = !walker.expectationsRememberedForLook();
    if (forgetsItself)
        walker.setExpectationsRememberedForLook(&remembered);
    walker.setTypesOfInputsInLook(&typesOfInputs);
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (isOnlyHandedOnForGuesses(operation, operandIndex))
            return;
        Vector<ValueID, 16>& toForget = *walker.expectationsRememberedForLook();
        size_t numberToForget = toForget.size();
        walker.setLookHasAskedAboutUnbuiltValue(false);
        ExpectationForGuesses expectation = callsOfValuesForGuesses.contains(operation.opcode()) ? expectationOfCalleeForGuesses(walker, operation, operandIndex) : findExpectationForGuesses(walker, operand);
        if (!walker.lookHasAskedAboutUnbuiltValue())
            toForget.shrink(numberToForget);
        if (!expectation.type)
            return;
        if (isReadOfVariableForGuesses(walker.currentBodyIndex(), operand))
            expectation.type = numbersExpectedOf(walker, operand, { expectation.type, expectation.isDeclared, expectation.isHeld }).type;
        else
            expectation.type = numbersExpectedOfSlotThatTurnsStep(walker, operand, { expectation.type, expectation.isDeclared, expectation.isHeld }).type;
        std::optional<TestOfGuess> chosen = chooseTestOfTypeForGuesses(walker, expectation.type);
        bool isAccessByName = (operation.opcode() == Opcode::PropGet || operation.opcode() == Opcode::PropSet) && !operation.isFreeName() && operand == operation.objectOperand();
        if (LayoutID heir = isAccessByName ? layoutKnownByStructureOfClass(walker, types.withoutNullish(expectation.type)) : LayoutID()) {
            chosen = TestOfGuess { types.forLayout(heir), { } };
            chosen->knownByStructure = heir;
        }
        if (!chosen || chosen->failingSideAsksFor || !testAdmitsAllThatIsExpectedForGuesses(walker, operation, operand, operandIndex, expectation.type, chosen->tested) || walker.withoutFunctionsThatMayNotBeMentioned(chosen->tested) != chosen->tested)
            return;
        TypeID held = typesOfInputs(operand);
        if (!held || types.isSubset(held, chosen->tested) || !types.overlap(held, chosen->tested))
            return;
        if (allBitwiseOperators.contains(operation.opcode()) && types.at(held).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Boolean)) {
            const BodyIndex& index = walker.currentBodyIndex();
            LoopID loop = index.loopOf(index.blockOf(operation.id()));
            if (loop && !index.isInLoop(index.blockOf(index.valueBehindCasts(operand)), loop))
                return;
        }
        functor(operand, expectation.type, types.intersectionOf(held, chosen->tested));
    });
    walker.setTypesOfInputsInLook(nullptr);
    if (!forgetsItself)
        return;
    walker.setExpectationsRememberedForLook(nullptr);
    for (ValueID value : remembered)
        walker.rememberedExpectationOf(value) = { };
}

bool guessAtCalleeAsksForBuiltinWhereItFails(Walker& walker, const BodyValue& operation, ValueID operand)
{
    if (!callsOfValuesForGuesses.contains(operation.opcode()) || constructionsOfParentForGuesses.contains(operation.opcode()) || operand != operation.calleeOperand())
        return false;
    const LinkedCallThatFindsBuiltin* found = walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), operation.id());
    return found && found->callee && isTestableBuiltin(walker.program().builtins(), found->builtin);
}

TypeID typeThatTypeTestAsksForWhereExpected(Walker& walker, Reference value, TypeID expected)
{
    std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, value, expected);
    return chosen && !chosen->layouts ? chosen->tested : TypeID();
}

void proveExpectedTypeOrBeginTurnAgain(Walker& walker, ValueID input, TypeID expected, Origin origin, Walker::TurnsThatAsk turnsThatAsk)
{
    Reference value = walker.referenceOf(input);
    std::optional<TestOfGuess> chosen = chooseLookAtStructureOfClassForGuesses(walker, value, expected);
    if (!chosen)
        chosen = chooseTestOfGuess(walker, value, expected);
    if (!chosen)
        return;
    origin.isTailCall = false;
    Type known = walker.types().at(walker.typeOf(value));
    bool refusesMarkedObjects = chosen->layouts && !isBeingMadeByBodyBeingRead(walker, input) && (!known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow));
    uint32_t firstNewValue = walker.code().numberOfValues();
    BlockID blockOfTest = walker.currentBlock();
    Reference test;
    {
        Walker::InFrontOfTypedCopy inFront(walker, walker.standsInFrontOfTypedCopy(value) && (!chosen->knownByStructure || walker.turnsOfTypedCopyWriteNothingAboveValues()));
        test = buildTestOfGuess(walker, value, *chosen, origin, refusesMarkedObjects);
    }
    if (!test)
        return;
    bool isAskedHere = test.isValue() && test.id().index() >= firstNewValue && walker.code().blockOf(test.id()) == blockOfTest;
    bool isAskedInFront = walker.branchInFrontOfTypedCopy(test, true, Walker::TurnOfTypedCopy::HasDoneNothing, turnsThatAsk);
    if (!isAskedInFront) {
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        walker.letGuessFailUnasked(failed, origin);
        walker.branch(test, passed, failed);
        walker.continueIn(failed);
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
        walker.continueIn(passed);
    }
    walker.noteGuessOpened();
    Reference proven;
    {
        Walker::InFrontOfTypedCopy inFront(walker, isAskedInFront);
        proven = buildProofOfGuess(walker, value, test, *chosen, origin);
    }
    if (!proven || !proven.isValue() || walker.isUnreachable())
        return;
    if (refusesMarkedObjects && isAskedHere)
        walker.state().noteFactsOfCell(proven, FactOfCell::BearsNoMark);
    Reference last = value;
    while (Reference checked = walker.state().checkedNameOf(last))
        last = checked;
    if (last != proven)
        walker.state().noteCheckedName(last, proven);
}

static bool isFoundFromInFrontOfLoopOfReadForGuesses(const BodyIndex& index, ValueID read, ValueID operand)
{
    LoopID loop = index.loopOf(index.blockOf(read));
    if (!loop)
        return false;
    uint32_t firstValueOfLoop = index.block(index.loop(loop).head).firstParameter().index();
    ValueID source = index.valueBehindCasts(operand);
    for (unsigned depth = 2; source && depth--;) {
        if (source.index() < firstValueOfLoop)
            return true;
        if (index.isParameter(source))
            return false;
        BodyValue definition = index.value(source);
        if (definition.opcode() == Opcode::GetModuleVariable)
            return true;
        if (definition.opcode() != Opcode::CheckInitialized)
            return false;
        source = index.valueBehindCasts(definition.operand(0));
    }
    return false;
}

TypeID textsThatNamesOfHolderSayOfKey(Walker& walker, const BodyValue& read)
{
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const BodyIndex& index = walker.currentBodyIndex();
    if (!isFoundFromInFrontOfLoopOfReadForGuesses(index, read.id(), read.propertyOperand()))
        return { };
    TypeID holder = findWhatIsProvenOrExpectedForGuesses(walker, read.objectOperand(), read.id()).type;
    LayoutID layout = holder ? singleLayoutOf(types, types.at(types.withoutNullish(holder))) : LayoutID();
    if (!layout)
        return { };
    ValueID user = index.onlyUsingValue(read.id());
    bool isCalled = user && callsOfValuesForGuesses.contains(index.opcodeOf(user));
    auto holdsClosuresAlone = [&](uint32_t field) {
        for (const LinkTables* tables : { &walker.facts().linked(), &walker.facts().held() }) {
            const LinkedCell* cell = tables->findCellOfField({ layout, field });
            if (!cell || !cell->proven)
                continue;
            Type held = types.at(cell->proven);
            if (held.hasObjects() && held.isJavaScriptValue() && held.hasOnlyClosures() && !held.hasUnknown() && !held.namesMergedObjects())
                return true;
        }
        return false;
    };
    TypeID texts;
    std::span<const Field> fields = program.at(layout).fields;
    for (uint32_t field = 0; field < fields.size(); ++field) {
        PropertyKeyID name = fields[field].key;
        if (!name || program.at(name).kind != PropertyKeyKind::Text || !program.at(name).text)
            return { };
        if (isCalled && !holdsClosuresAlone(field))
            continue;
        TypeID text = types.stringLiteral(program.at(name).text);
        texts = texts ? types.unionOf(texts, text) : text;
    }
    return texts;
}

bool siteMayAnswerReadWithoutOtherCode(const Walker& walker, const BodyValue& read)
{
    if (read.opcode() == Opcode::ElemGet) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID user = index.onlyUsingValue(read.id());
        return (walker.textNamesHolderThatIsNoBuiltin(read.objectOperand()) || isFoundFromInFrontOfLoopOfReadForGuesses(index, read.id(), read.propertyOperand())) && isFoundFromInFrontOfLoopOfReadForGuesses(index, read.id(), read.objectOperand()) && (!user || !callsOfValuesForGuesses.contains(index.opcodeOf(user)));
    }
    if (read.opcode() != Opcode::PropGet || read.isFreeName())
        return false;
    const Program& program = walker.program();
    PropertyKeyID key = read.key();
    if (!key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text || (program.nameClassOf(key) != NameClass::Ordinary && !(program.nameClassOf(key) == NameClass::Length && walker.textNamesHolderThatIsNoBuiltin(read.objectOperand()))))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(read.id());
    return !user || !callsOfValuesForGuesses.contains(index.opcodeOf(user));
}

static bool everyWayIntoJoinWritesNumberForGuesses(Walker& walker, ValueID joined)
{
    const BodyIndex& index = walker.currentBodyIndex();
    joined = index.valueBehindCasts(joined);
    if (!index.isParameter(joined) || index.blockOf(joined) == entryBlock || index.isEnteredByHandlerEdge(index.blockOf(joined)))
        return false;
    BlockID block = index.blockOf(joined);
    uint32_t position = joined.index() - index.block(block).parameter(0).index();
    bool someWayLeadsIn = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID argument = position < arguments.size() ? index.valueBehindCasts(arguments[position]) : ValueID();
        if (!argument || index.isParameter(argument) || (index.opcodeOf(argument) != Opcode::Integer && index.opcodeOf(argument) != Opcode::Double))
            return false;
        someWayLeadsIn = true;
    }
    return someWayLeadsIn;
}

static bool givesNoNumberByShapeOfItsReceiverForGuesses(Walker& walker, ValueID value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet || index.value(value).isFreeName() || !layoutsThatValueIsShapedLikeForGuesses(walker, index.value(value).objectOperand()))
        return false;
    TypeID expected = findExpectationForGuesses(walker, value).type;
    return expected && !saysNothingForGuesses(walker.types(), expected) && isNeverType(walker.types().intersectionOf(expected, TypeTable::number));
}

static ValueID parameterWhereValueMeetsWrittenNumberForGuesses(Walker&, const BodyIndex&, ValueID, BodyUser);

static bool isOnlyReckonedWithAsNumber(Walker& walker, ValueID value, const TypesOfInputs& types)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(value);
    if (!user) {
        std::optional<BodyUser> first = index.firstUser(value);
        ValueID met = first && first->kind == BodyUseKind::EdgeArgument ? parameterWhereValueMeetsWrittenNumberForGuesses(walker, index, value, *first) : ValueID();
        return met && !givesNoNumberByShapeOfItsReceiverForGuesses(walker, value) && isOnlyReckonedWithAsNumber(walker, met, types);
    }
    BodyValue operation = index.value(user);
    if (operatorsThatMakeNumbers.contains(operation.opcode()) || allBitwiseOperators.contains(operation.opcode()))
        return true;
    if (!operationsThatJoinTexts.contains(operation.opcode()) && !allRelations.contains(operation.opcode()))
        return false;
    if (givesNoNumberByShapeOfItsReceiverForGuesses(walker, value))
        return false;
    TypeID other = types(operation.operand(operation.operand(0) == value ? 1 : 0));
    if (!other)
        return everyWayIntoJoinWritesNumberForGuesses(walker, operation.operand(operation.operand(0) == value ? 1 : 0));
    return !isNeverType(walker.types().intersectionOf(other, TypeTable::number));
}

bool valueIsOnlyReckonedWithAsNumber(Walker& walker, ValueID value, const TypesOfInputs& types)
{
    return isOnlyReckonedWithAsNumber(walker, value, types);
}

static bool isOnlyReadAsListForGuesses(Walker& walker, ValueID value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(value);
    if (!user)
        return false;
    BodyValue operation = index.value(user);
    if (operation.opcode() == Opcode::ElemGet)
        return operation.objectOperand() == value;
    return operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operation.objectOperand() == value && operation.key() == walker.program().keyOf(BuiltinNames::length);
}

static bool isFoundFromInFrontOfItsLoopForGuesses(const BodyIndex& index, ValueID read)
{
    LoopID loop = index.loopOf(index.blockOf(read));
    if (!loop)
        return false;
    uint32_t firstValueOfLoop = index.block(index.loop(loop).head).firstParameter().index();
    ValueID holder = index.value(read).objectOperand();
    for (unsigned depth = 4; depth--;) {
        holder = index.valueBehindCasts(holder);
        if (holder.index() < firstValueOfLoop)
            return true;
        if (index.isParameter(holder))
            return false;
        BodyValue definition = index.value(holder);
        if (definition.opcode() == Opcode::GetModuleVariable || definition.opcode() == Opcode::Builtin)
            return true;
        if (definition.opcode() == Opcode::CheckInitialized)
            holder = definition.operand(0);
        else if (definition.opcode() == Opcode::PropGet && !definition.isFreeName())
            holder = definition.objectOperand();
        else
            return false;
    }
    return false;
}

bool readThatAsksItsSiteIsReckonedWithAsNumber(Walker& walker, ValueID read, const TypesOfInputs& types)
{
    return read && siteMayAnswerReadWithoutOtherCode(walker, walker.currentBodyIndex().value(read)) && isOnlyReckonedWithAsNumber(walker, read, types);
}

bool someReadThatAsksItsSiteIsReckonedWith(Walker& walker, std::span<const ValueID> reads, const TypesOfInputs& types)
{
    const BodyIndex& index = walker.currentBodyIndex();
    return std::ranges::any_of(reads, [&](ValueID read) {
        return read && siteMayAnswerReadWithoutOtherCode(walker, index.value(read)) && (isOnlyReckonedWithAsNumber(walker, read, types) || isFoundFromInFrontOfItsLoopForGuesses(index, read));
    });
}

bool readAtSiteOrBeginTurnAgainOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::TypedOperations) || !siteMayAnswerReadWithoutOtherCode(walker, read))
        return false;
    if (!walker.standsInTurnThatLooksCountOn() || !walker.mayBeginTurnAgainInClosedCopy())
        return false;
    Reference object = walker.referenceOf(read.objectOperand());
    if (!object || !object.isRecord())
        return false;
    if (walker.someCallStaysInTurnsOfTypedCopy() && !walker.readsAskSitesThoughCallStays()) {
        walker.noteReadKeptFromItsSiteBecauseCallStays();
        return false;
    }
    bool isByKey = read.opcode() == Opcode::ElemGet;
    Reference key = isByKey ? walker.referenceOf(read.propertyOperand()) : Reference();
    bool isByIndex = isByKey && key && walker.textNamesHolderThatIsNoBuiltin(read.objectOperand()) && walker.types().at(walker.typeOf(key)).isInteger();
    isByKey &= !isByIndex;
    if (isByKey && (!key || !key.isValue() || !(walker.types().at(walker.typeOf(key)).primitives & Primitive::String) || !walker.types().saysNothing(walker.typeOf(object))))
        return false;
    bool isFoundUnderKeyInFront = isByKey && walker.standsInFrontOfTypedCopy(object, Walker::TurnOfTypedCopy::HasRunNoOtherCode) && walker.standsInFrontOfTypedCopy(key, Walker::TurnOfTypedCopy::HasRunNoOtherCode) && walker.everyTurnOfTypedCopyFindsTheSameUnderEveryName();
    if (isByKey && !isFoundUnderKeyInFront && !walker.buildsWhereItStands())
        return false;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;

    const BodyIndex& index = walker.currentBodyIndex();
    auto typeAsBuilt = [&](ValueID input) {
        if (Reference built = walker.referenceOf(input))
            return walker.typeOf(built);
        ValueID later = index.valueBehindCasts(input);
        ValueID user = index.onlyUsingValue(read.id());
        if (!index.isParameter(later) && user && index.blockOf(user) == index.blockOf(read.id()))
            return widestTypeOfOperation(walker, index.value(later));
        return !index.isParameter(later) && siteMayAnswerReadWithoutOtherCode(walker, index.value(later)) ? TypeID(TypeTable::anything) : TypeID();
    };
    bool asksForNumber = isOnlyReckonedWithAsNumber(walker, read.id(), TypesOfInputs(typeAsBuilt));
    bool asksForList = !asksForNumber && isOnlyReadAsListForGuesses(walker, read.id());
    bool goesOnWhereItHolds = asksForNumber || asksForList;
    if (object.isConstant() && !goesOnWhereItHolds && !(walker.types().at(walker.typeOf(object)).isObject() && walker.types().at(walker.typeOf(object)).hasOnlyClosures()))
        return false;
    if (!isByKey && !isByIndex && !walker.standsInFrontOfTypedCopy(object, Walker::TurnOfTypedCopy::HasRunNoOtherCode)) {
        const Code& code = walker.code();
        ValueID holder = object.id();
        while (!code.isConstant(holder)) {
            const Operation& record = code.at(holder);
            if (record.opcode != Opcode::Narrowed || record.narrowsHolderOfOperand() || record.narrowsOtherResultOfOperand())
                break;
            ValueID narrowed = record.operand(0);
            if (code.isConstant(narrowed) || !walker.types().at(code.at(narrowed).type).isJavaScriptValue() || (walker.types().at(code.at(narrowed).type).primitives & (Primitive::Absent | Primitive::Unspecified)))
                break;
            holder = narrowed;
        }
        if (holder != object.id() && walker.standsInFrontOfTypedCopy(Reference::value(holder), Walker::TurnOfTypedCopy::HasRunNoOtherCode))
            object = Reference::value(holder);
    }
    bool isReadInFront = isByIndex ? false : isByKey ? isFoundUnderKeyInFront : (walker.standsInFrontOfTypedCopy(object, Walker::TurnOfTypedCopy::HasRunNoOtherCode) && walker.everyTurnOfTypedCopyFindsTheSameUnderName(read.key()));
    Reference found;
    Reference test;
    {
        Walker::InFrontOfTypedCopy inFront(walker, isReadInFront, Walker::TurnOfTypedCopy::HasRunNoOtherCode);
        Draft lookup(isByIndex ? Opcode::GetElementInStorageOrAbsent : isByKey ? Opcode::GetOwnValueUnderKeyOrAbsent : Opcode::GetValueAtSiteOrAbsent, origin);
        lookup.setObjectOperand(object);
        if (isByIndex) {
            lookup.setPositionOperand(key);
            walker.count(StatisticID::ElementsReadInPlainStorageOfWhatIsNoList);
        } else if (isByKey)
            lookup.setPropertyOperand(key);
        else
            lookup.setKey(read.key());
        lookup.setValueType(TypeTable::anythingOrAbsent);
        found = walker.build(WTF::move(lookup));
        if (!found || walker.hasFailed())
            return true;
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(found);
        question.setTestedType(asksForNumber ? TypeID(TypeTable::number) : asksForList ? walker.types().builtinClass(BuiltinClasses::Array) : TypeArena::fromPrimitives(Primitive::Absent));
        test = walker.build(WTF::move(question));
        if (!test || walker.hasFailed())
            return true;
    }

    walker.noteGuessOpened();
    if (isReadInFront)
        walker.count(isByKey ? StatisticID::ReadsUnderKeysFromInFrontBuiltInFrontOfLoops : StatisticID::ReadsThatAskTheirSitesBuiltInFrontOfLoops);
    if (walker.someCallStaysInTurnsOfTypedCopy())
        walker.count(StatisticID::ReadsThatAskTheirSitesInTurnsThatCall);
    bool isAskedInFront = isReadInFront && walker.branchInFrontOfTypedCopy(test, goesOnWhereItHolds, Walker::TurnOfTypedCopy::HasRunNoOtherCode);
    if (!isAskedInFront) {
        BlockID leaves = walker.addBlock(BlockFrequency::Rare);
        BlockID goesOn = walker.addBlock();
        walker.branch(test, goesOnWhereItHolds ? goesOn : leaves, goesOnWhereItHolds ? leaves : goesOn);
        walker.continueIn(leaves);
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
        walker.continueIn(goesOn);
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }
    Walker::InFrontOfTypedCopy inFront(walker, isAskedInFront, Walker::TurnOfTypedCopy::HasRunNoOtherCode);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(found);
    proof.setTestOperand(test);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(goesOnWhereItHolds);
    walker.bind(read.id(), walker.build(WTF::move(proof)));
    return true;
}

Reference askWhatCallHandsBackForNumberOrBeginTurnAgain(Walker& walker, const BodyValue& call, Reference found, Origin origin)
{
    if (!found || !found.isValue() || walker.isUnreachable() || walker.hasFailed())
        return found;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::TypedOperations) || !walker.types().saysNothing(walker.typeOf(found)))
        return found;
    if (!walker.standsInTurnThatLooksCountOn() || !walker.mayBeginTurnAgainInClosedCopy() || walker.someCallStaysInTurnsOfTypedCopy())
        return found;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(call.id());
    if (!user || index.blockOf(user) != index.blockOf(call.id()))
        return found;
    auto typeAsBuilt = [&](ValueID input) {
        if (Reference built = walker.referenceOf(input))
            return walker.typeOf(built);
        ValueID later = index.valueBehindCasts(input);
        return index.isParameter(later) ? TypeID() : widestTypeOfOperation(walker, index.value(later));
    };
    if (!isOnlyReckonedWithAsNumber(walker, call.id(), TypesOfInputs(typeAsBuilt)))
        return found;

    origin.isTailCall = false;
    bool standsInFront = walker.standsInFrontOfTypedCopy(found, Walker::TurnOfTypedCopy::HasRunNoOtherCode);
    Reference test;
    {
        Walker::InFrontOfTypedCopy inFront(walker, standsInFront, Walker::TurnOfTypedCopy::HasRunNoOtherCode);
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(found);
        question.setTestedType(TypeTable::number);
        test = walker.build(WTF::move(question));
        if (!test || !test.isValue() || walker.hasFailed())
            return found;
    }

    walker.noteGuessOpened();
    walker.count(StatisticID::AnswersOfLookupsAskedForNumbers);
    bool isAskedInFront = standsInFront && walker.branchInFrontOfTypedCopy(test, true, Walker::TurnOfTypedCopy::HasRunNoOtherCode);
    if (!isAskedInFront) {
        BlockID leaves = walker.addBlock(BlockFrequency::Rare);
        BlockID goesOn = walker.addBlock();
        walker.branch(test, goesOn, leaves);
        walker.continueIn(leaves);
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
        walker.continueIn(goesOn);
        if (walker.isUnreachable() || walker.hasFailed())
            return found;
    }
    Walker::InFrontOfTypedCopy inFront(walker, isAskedInFront, Walker::TurnOfTypedCopy::HasRunNoOtherCode);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(found);
    proof.setTestOperand(test);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    return walker.build(WTF::move(proof));
}

bool guessesMayTypeOperationInCleanTurn(Walker& walker, const BodyValue& operation, const TypesOfInputs& typesOfInputs, bool sitesMayAnswer)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Opcode opcode = operation.opcode();
    if (callsOfValuesForGuesses.contains(opcode)) {
        if (walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) || walker.facts().held().calleeAt(walker.currentFunction(), operation.id()))
            return true;
        static_assert(OperandIndex::SuperConstruct::parent == OperandIndex::Call::callee && OperandIndex::SuperConstructList::parent == OperandIndex::Call::callee);
        ValueID whatIsCalled = constructionsOfParentForGuesses.contains(opcode) ? operation.parentOperand() : operation.calleeOperand();
        ValueID called = index.valueBehindCasts(whatIsCalled);
        if (called && !index.isParameter(called) && index.opcodeOf(called) == Opcode::PropGet && !index.value(called).isFreeName())
            return true;
        TypeID callee = typesOfInputs(whatIsCalled);
        return (callee && !saysNothingForGuesses(walker.types(), callee)) || !!expectationOfCalleeForGuesses(walker, operation, OperandIndex::Call::callee).type || callIsExpectedToEnterTargetOfBoundFunction(walker, operation) || !!fewClosuresThatCalleeIsHeldToBeForGuesses(walker, operation);
    }
    static constexpr OpcodeSet accesses = opcodesOfFamily(OperationFamily::Access);
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    bool isAccess = accesses.contains(opcode);
    if (!isAccess && !operators.contains(opcode))
        return effectRuleOf(opcode) == EffectRule::Call;
    Vector<std::pair<ValueID, TypeID>, 2> proven;
    forEachTypeThatGuessWouldProveAt(walker, operation, typesOfInputs, scopedLambda<void(ValueID, TypeID, TypeID)>([&](ValueID operand, TypeID, TypeID left) {
        proven.append({ operand, left });
    }));
    auto typeBehindTests = [&](ValueID input) {
        for (auto [operand, left] : proven) {
            if (operand == input)
                return left;
        }
        return typesOfInputs(input);
    };
    if (!isAccess)
        return typedFormOf(walker, operation, TypesOfInputs(typeBehindTests)) != TypedForm::None;
    walker.setLooksTakeFormsBehindLooksAsBuilt(true);
    FunctionID halfRunBehindLooks;
    std::optional<Effects> effects = effectsOfTypedFormAhead(walker, operation, TypesOfInputs(typeBehindTests), &halfRunBehindLooks, sitesMayAnswer);
    walker.setLooksTakeFormsBehindLooksAsBuilt(false);
    return halfRunBehindLooks || (effects && !effects->mayRunOtherCode());
}

static LayoutID classNamedByOperandForGuesses(Walker& walker, ValueID operand)
{
    Reference constructor = walker.referenceOf(operand);
    if (!constructor)
        return { };
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    Type typeOfConstructor = types.at(walker.typeOf(constructor));
    FunctionID function = typeOfConstructor.isObject() && typeOfConstructor.isJavaScriptValue() ? singleFunctionOf(types, typeOfConstructor) : FunctionID();
    if (!function)
        return { };
    LayoutID classLayout = classOfClassObject(program, function);
    if (!classLayout)
        classLayout = classOfConstructorFunction(program, function);
    if (!classLayout || program.at(classLayout).kind != LayoutKind::Instance || !walker.facts().isEvaluatedAtMostOnce(classLayout))
        return { };
    return classLayout;
}

static bool guardsEndWhereClassIsEvaluatedAgain(const Program& program, LayoutID classLayout)
{
    const Layout& layout = program.at(classLayout);
    return layout.constructor && isClassConstructorOf(program.at(layout.constructor).kind);
}

static bool listConditionsOfPlainInstanceForGuesses(const Walker& walker, LayoutID layout, Vector<MemberConditionRecord, 16>& conditions, LayoutID classAsked = { })
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    const Facts& facts = walker.facts();
    const LinkTables& link = facts.linked();
    Vector<PropertyKeyID, 16> names;
    for (const Field& field : program.at(layout).fields) {
        if (field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods }))
            continue;
        if (!field.key || !plan.slots().findField(program, layout, field.key))
            return false;
        names.append(field.key);
    }
    names.appendVector(plan.slots().layout(layout).appendedKeys);
    bool someSlotTakesAnyAccessor = false;
    names.removeAllMatching([&](PropertyKeyID name) {
        std::optional<FieldIndex> field = plan.slots().findField(program, layout, name);
        if (!field || !plan.slot(layout, *field).holdsAccessor)
            return false;
        someSlotTakesAnyAccessor |= !takesAccessorsOfLibraryAlone(program, plan.slots(), layout, name);
        return true;
    });
    if (someSlotTakesAnyAccessor)
        return false;

    auto isOfLibrary = [&](FunctionID function) {
        return function && isLibraryCode(program, program.at(function));
    };
    LayoutID classLayout = declaredLayoutOf(program, layout);
    size_t numberOfClassesLeft = program.layouts.size();
    bool standsBelowClassAsked = !!classAsked;
    for (LayoutID current = classLayout; current;) {
        if (current == classAsked)
            standsBelowClassAsked = false;
        if (!numberOfClassesLeft-- || program.at(current).kind != LayoutKind::Instance || !plan.hasClassRecord(current) || (!facts.isEvaluatedAtMostOnce(current) && !guardsEndWhereClassIsEvaluatedAgain(program, current)))
            return false;
        if (facts.hasMemberUnder(current, ClassSide::Prototype, program.keyOf(BuiltinNames::__proto__)) || !link.membersUnder(current, ClassSide::Prototype, program.keyOf(BuiltinNames::__proto__)).empty())
            return false;
        for (uint32_t place : facts.placesOfMembersUnder(current, ClassSide::Prototype, { })) {
            if (!isOfLibrary(facts.membersOf(current)[place].function))
                return false;
        }
        for (uint32_t place : link.membersUnder(current, ClassSide::Prototype, { })) {
            const LinkedMember& member = link.members[place];
            if (!isOfLibrary(member.everyWriterIsKnown ? member.function : member.written))
                return false;
        }
        bool accessorIsOfProgram = false;
        bool accessorLiesOverFieldOfClassAsked = false;
        names.removeAllMatching([&](PropertyKeyID name) {
            bool isAccessor = false;
            for (uint32_t place : facts.placesOfMembersUnder(current, ClassSide::Prototype, name)) {
                if (facts.membersOf(current)[place].kind == MethodKind::Method)
                    continue;
                isAccessor = true;
                accessorIsOfProgram |= !isOfLibrary(facts.membersOf(current)[place].function);
            }
            for (uint32_t place : link.membersUnder(current, ClassSide::Prototype, name)) {
                if (link.members[place].kind == MethodKind::Method)
                    continue;
                isAccessor = true;
                accessorIsOfProgram |= !link.members[place].everyWriterIsKnown || !isOfLibrary(link.members[place].function);
            }
            accessorLiesOverFieldOfClassAsked |= isAccessor && standsBelowClassAsked && plan.slots().findField(program, classAsked, name);
            return isAccessor;
        });
        if (accessorIsOfProgram || accessorLiesOverFieldOfClassAsked)
            return false;
        for (const LinkedNameOnPrototype& laid : std::ranges::equal_range(link.namesOnPrototypes, current, { }, &LinkedNameOnPrototype::layout)) {
            if (laid.mayHoldAccessor && laid.key && names.contains(laid.key))
                return false;
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return false;
        current = parent;
    }
    if (names.isEmpty())
        conditions.append({ MemberConditionKind::PrototypeLinkUnchanged, classLayout, ClassSide::Prototype, { } });
    for (PropertyKeyID name : names)
        conditions.append({ MemberConditionKind::MemberPassedByStores, classLayout, ClassSide::Prototype, name });
    conditions.append({ MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, walker.program().keyOfWordOnWhatSeesAccesses() });
    return true;
}

static ValueID findQuestionAboutDeclaredMemberThatDecidesBranch(const BodyIndex& index, const BodyTerminator& terminator)
{
    if (terminator.kind() != TerminatorKind::Branch || terminator.numberOfTargets() != 2)
        return { };
    ValueID condition = terminator.as<TerminatorKind::Branch>().conditionOperand();
    while (condition && !index.isParameter(condition) && (index.opcodeOf(condition) == Opcode::ToBoolean || index.opcodeOf(condition) == Opcode::Convert))
        condition = index.value(condition).operand(0);
    return condition && !index.isParameter(condition) && index.opcodeOf(condition) == Opcode::FindsDeclaredMember ? condition : ValueID();
}

static void listGuardsOfQuestionsInRowBehind(Walker& walker, const BodyValue& first, TypeID objects, TypeID declaring, LayoutID declaredAtOrAbove, Vector<MemberConditionRecord, 8>& guards)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID block = index.blockOf(first.id());
    if (findQuestionAboutDeclaredMemberThatDecidesBranch(index, index.terminator(block)) != first.id())
        return;
    auto targetOf = [&](BlockID from, uint32_t targetIndex) {
        BodyTargets::Iterator target = index.terminator(from).targets().begin();
        if (targetIndex)
            ++target;
        return *target;
    };
    auto nameInFrontOfTests = [&](ValueID value) {
        while (true) {
            value = index.valueBehindCasts(value);
            ValueID narrowed = index.valueNarrowedInto(value);
            if (!narrowed)
                return value;
            value = narrowed;
        }
    };
    auto holdsNothingSeenBeside = [&](BlockID holder, ValueID question, bool looksBehindQuestionAlone) {
        bool isLookedAt = !looksBehindQuestionAlone;
        for (BodyValue operation : index.block(holder).values()) {
            if (operation.id() == question)
                isLookedAt = true;
            else if (isLookedAt && !isUnseenWhenUnused(walker, operation))
                return false;
        }
        return true;
    };
    auto nameOneClass = [&](ValueID one, ValueID other) {
        if (one == other)
            return true;
        if (!one || !other || index.isParameter(one) || index.isParameter(other))
            return false;
        return index.opcodeOf(one) == Opcode::GetModuleVariable && index.opcodeOf(other) == Opcode::GetModuleVariable && index.value(one).variable() == index.value(other).variable();
    };
    if (!holdsNothingSeenBeside(block, first.id(), true))
        return;
    WayBehindTerminator wayOfNo = wayBehindTarget(walker, block, 1, targetOf(block, 1));
    ValueID object = nameInFrontOfTests(first.objectOperand());
    TypeArena& types = walker.types();
    while (true) {
        BlockID next = targetOf(block, 0).block();
        if (index.numberOfIncomingEdges(next) != 1 || index.isEnteredByHandlerEdge(next))
            return;
        BodyTerminator end = index.terminator(next);
        ValueID question = findQuestionAboutDeclaredMemberThatDecidesBranch(index, end);
        bool handsAnswerOn = !question && end.kind() == TerminatorKind::Jump;
        if (handsAnswerOn) {
            for (ValueID argument : end.next().arguments()) {
                ValueID brought = index.valueBehindCasts(argument);
                if (!index.isParameter(brought) && index.opcodeOf(brought) == Opcode::FindsDeclaredMember)
                    question = brought;
            }
        }
        if (!question || index.blockOf(question) != next || !holdsNothingSeenBeside(next, question, false))
            return;
        BodyValue following = index.value(question);
        if (nameInFrontOfTests(following.objectOperand()) != object || !nameOneClass(following.declaredByOperand(), first.declaredByOperand()))
            return;
        WayBehindTerminator wayOfNextNo = handsAnswerOn ? wayBehindTarget(walker, next, 0, end.next()) : wayBehindTarget(walker, next, 1, targetOf(next, 1));
        if (!comeToSamePlace(index, wayOfNo, wayOfNextNo, first.id(), question))
            return;
        if (objectsAboveWhichRecordsDeclareMember(walker, objects, following.key(), declaredAtOrAbove) != declaring)
            return;
        for (LayoutID member : membersOf(types, types.at(declaring).layouts)) {
            MemberConditionRecord guard { MemberConditionKind::MemberUnchanged, declaredLayoutOf(walker.program(), member), ClassSide::Prototype, following.key() };
            if (walker.plan().isInUse(member) && !guards.contains(guard) && !walker.state().guardOfReadsHolds(guard.layout, guard.key))
                guards.append(guard);
        }
        if (handsAnswerOn)
            return;
        block = next;
    }
}

bool askGuardsAboutDeclaredMember(Walker& walker, const BodyValue& operation)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    LayoutID declaredAtOrAbove = operation.declaredByOperand() ? classNamedByOperandForGuesses(walker, operation.declaredByOperand()) : LayoutID();
    if (operation.declaredByOperand() && !declaredAtOrAbove)
        return false;
    ValueID input = operation.objectOperand();
    Reference value = walker.referenceOf(input);
    if (!value || !value.isValue())
        return false;
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(value);
    TypeID expected = saysNothingForGuesses(types, proven) ? findExpectationForGuesses(walker, input).type : proven;
    if (saysNothingForGuesses(types, expected))
        return false;
    TypeID objects = types.withOnlyObjects(expected);
    Type type = types.at(objects);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures())
        return false;
    TypeID declaring = objectsAboveWhichRecordsDeclareMember(walker, objects, operation.key(), declaredAtOrAbove);
    if (!declaring)
        return false;
    LayoutSetID receivers = types.at(declaring).layouts;
    if (shapesOfLayoutsForGuesses(walker, receivers) != ShapesOfLayoutsForGuesses::Testable)
        return false;
    Vector<MemberConditionRecord, 8> guards;
    for (LayoutID member : membersOf(types, receivers)) {
        MemberConditionRecord guard { MemberConditionKind::MemberUnchanged, declaredLayoutOf(walker.program(), member), ClassSide::Prototype, operation.key() };
        if (walker.plan().isInUse(member) && !guards.contains(guard))
            guards.append(guard);
    }
    Origin origin = walker.originOf(operation);
    bool cellIsKnownToStandBelowDeclaringClass = types.isSubset(proven, declaring) && factsOfCellAsPlannerTakesThem(walker, value).contains(FactOfCell::HasLayoutPrototype);
    bool everyGuardIsKnownToHold = !guards.isEmpty() && std::ranges::all_of(guards, [&](const MemberConditionRecord& guard) {
        return walker.takesGuardsAndFactsToHold() || walker.state().guardOfReadsHolds(guard.layout, guard.key);
    });
    FunctionID declared = walker.checksWalk() ? findOnlyMethodThatRecordsDeclareAbove(walker, declaring, operation.key()) : FunctionID();
    if (cellIsKnownToStandBelowDeclaringClass && everyGuardIsKnownToHold && walker.has(Mechanism::Folding)) {
        walker.checkFactsRememberedOfCell(value, origin, FactOfCell::HasLayoutPrototype);
        checkGuardsThatAreRemembered(walker, guards.span(), origin);
        if (declared)
            walker.askObjectAgainForFunctionThatNameFinds(value, operation.key(), declared, origin);
        Draft yes(Opcode::Boolean);
        yes.setBoolean(true);
        walker.bind(operation.id(), walker.constant(WTF::move(yes)));
        return true;
    }
    bool cellHasAnswered = guards.size() == 1 && cellIsKnownToStandBelowDeclaringClass;
    if (walker.has(Mechanism::Folding))
        listGuardsOfQuestionsInRowBehind(walker, operation, objects, declaring, declaredAtOrAbove, guards);
    Draft question(cellHasAnswered ? Opcode::AreMembersUnchanged : Opcode::FindsDeclaredMember, origin);
    if (cellHasAnswered)
        walker.checkFactsRememberedOfCell(value, origin, FactOfCell::HasLayoutPrototype);
    else {
        walker.chooseConservativeFields(question);
        question.setObjectOperand(value);
        question.setKey(operation.key());
        question.setReceivers(receivers);
        if (declared)
            question.setMemberDeclared(declared);
    }
    question.setConditions(guards.span());
    Reference asked = walker.build(WTF::move(question));
    walker.bind(operation.id(), asked);
    if (cellHasAnswered && declared && asked && asked.isValue())
        walker.askObjectAgainForFunctionThatNameFinds(value, operation.key(), declared, origin, asked);
    if (asked && asked.isValue())
        noteKnown(walker, KnownNote::AnswerOfQuestionStillStands, value, asked, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards }), 1, true);
    walker.noteGuessOpened();
    return true;
}

TypeID objectsExpectedOfOperandOfQuestion(Walker& walker, ValueID input)
{
    Reference value = walker.referenceOf(input);
    if (!value || !value.isValue())
        return { };
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(value);
    TypeID expected = saysNothingForGuesses(types, proven) ? findExpectationForGuesses(walker, input).type : proven;
    if (saysNothingForGuesses(types, expected))
        return { };
    TypeID objects = types.withOnlyObjects(expected);
    Type type = types.at(objects);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || shapesOfLayoutsForGuesses(walker, type.layouts) != ShapesOfLayoutsForGuesses::Testable)
        return { };
    return objects;
}

bool askCellWhetherItIsPlainInstance(Walker& walker, const BodyValue& operation)
{
    if (!walker.has(Mechanism::TypedOperations) || !operation.constructorOperand())
        return false;
    walker.noteQuestionForPlainInstance(false);
    const Program& program = walker.program();
    Reference value = walker.referenceOf(operation.objectOperand());
    LayoutID classLayout = classNamedByOperandForGuesses(walker, operation.constructorOperand());
    if (!value || !value.isValue() || !classLayout || (objectPrototypeMayGainName(program) && !byteTellsOfNamesAddedToObjectPrototype(program)))
        return false;
    Vector<MemberConditionRecord, 16> conditions;
    if (!findConditionsOfOrdinaryInstanceOf(walker, walker.typeOf(walker.referenceOf(operation.constructorOperand())), conditions) || !listConditionsOfPlainInstanceForGuesses(walker, classLayout, conditions))
        return false;
    Vector<LayoutID, 16> taken { classLayout };
    if (operation.opcode() == Opcode::IsPlainHeirOf) {
        for (LayoutID layout : program.layoutsExpectedBelow(classLayout)) {
            if (program.at(layout).kind == LayoutKind::Instance && !program.at(layout).isDerived() && walker.plan().isInUse(layout) && listConditionsOfPlainInstanceForGuesses(walker, layout, conditions, classLayout))
                taken.append(layout);
        }
    }
    TypeArena& types = walker.types();
    LayoutSetID receivers = types.at(types.forLayouts(taken.span())).layouts;
    if (shapesOfLayoutsForGuesses(walker, receivers) != ShapesOfLayoutsForGuesses::Testable)
        return false;
    if (taken.size() > 1 && program.at(classLayout).has(LayoutFlags::IsConstructorFunction) && !walker.state().guardsHaveBegun(classLayout)) {
        Draft begin(Opcode::BeginMemberGuards, walker.originOf(operation));
        begin.setLayout(classLayout);
        walker.build(WTF::move(begin));
    }
    walker.noteQuestionForPlainInstance(true);
    Draft question(Opcode::IsPlainInstanceOf, walker.originOf(operation));
    walker.chooseConservativeFields(question);
    question.setObjectOperand(value);
    question.setReceivers(receivers);
    question.setConditions(conditions.span());
    if (taken.size() > 1)
        question.setClassAsked(classLayout);
    Reference asked = walker.build(WTF::move(question));
    walker.bind(operation.id(), asked);
    if (asked && asked.isValue())
        noteKnown(walker, KnownNote::AnswerOfQuestionStillStands, Reference::value(walker.code().at(asked.id()).objectOperand()), asked, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards }), 1, true);
    walker.noteGuessOpened();
    return true;
}

void beginGuardsOfClassesAtEndOfModule(Walker& walker, Origin origin)
{
    const Program& program = walker.program();
    const Function& body = program.at(walker.currentFunction());
    if (body.kind != FunctionKind::Module || !isLibraryCode(program, body) || walker.isUnreachable() || !walker.has(Mechanism::TypedOperations))
        return;
    origin.isTailCall = false;
    const BodyIndex& index = walker.currentBodyIndex();
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue named : index.block(BlockID(number)).values()) {
            FunctionID function;
            if (named.opcode() == Opcode::NewClosure)
                function = named.as<Opcode::NewClosure>().function();
            else if (named.opcode() == Opcode::GetModuleVariable) {
                const Variable& variable = program.at(named.as<Opcode::GetModuleVariable>().variable());
                if (variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable) && variable.module == body.module)
                    function = variable.function;
            }
            LayoutID classLayout = function ? classOfConstructorFunction(program, function) : LayoutID();
            if (!classLayout || !walker.plan().hasClassRecord(classLayout) || !walker.facts().isEvaluatedAtMostOnce(classLayout) || walker.state().guardsHaveBegun(classLayout))
                continue;
            Draft begin(Opcode::BeginMemberGuards, origin);
            begin.setLayout(classLayout);
            begin.setEndsTopLevelOfModule(true);
            walker.build(WTF::move(begin));
        }
    }
}

bool askGuardsWhetherPrototypeChainIsPlain(Walker& walker, const BodyValue& operation)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    walker.noteQuestionForPlainInstance(false);
    const Program& program = walker.program();
    bool asksByte = objectPrototypeMayGainName(program);
    if (asksByte && !byteTellsOfNamesAddedToObjectPrototype(program))
        return false;
    Vector<LayoutID, 4> classes;
    Vector<MemberConditionRecord, 8> conditions;
    for (ValueID constructor : { operation.constructorOperand(), operation.secondOperand(), operation.thirdOperand(), operation.fourthOperand() }) {
        if (!constructor)
            continue;
        LayoutID classLayout = classNamedByOperandForGuesses(walker, constructor);
        Vector<MemberConditionRecord, 16> ofInstanceOf;
        if (!classLayout || !walker.plan().hasClassRecord(classLayout) || !findConditionsOfOrdinaryInstanceOf(walker, walker.typeOf(walker.referenceOf(constructor)), ofInstanceOf))
            return false;
        for (const MemberConditionRecord& condition : ofInstanceOf) {
            if (condition.key == program.keyOf(BuiltinNames::prototype))
                conditions.append(condition);
        }
        conditions.append({ MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, program.keyOfWordOnWhatSeesAccesses() });
        classes.append(classLayout);
    }
    Origin origin = walker.originOf(operation);
    for (LayoutID classLayout : classes) {
        if (!program.at(classLayout).has(LayoutFlags::IsConstructorFunction) || walker.state().guardsHaveBegun(classLayout))
            continue;
        Draft begin(Opcode::BeginMemberGuards, origin);
        begin.setLayout(classLayout);
        walker.build(WTF::move(begin));
    }
    walker.noteQuestionForPlainInstance(true);
    Reference answered;
    if (walker.takesGuardsAndFactsToHold()) {
        Draft yes(Opcode::Boolean);
        yes.setBoolean(true);
        answered = walker.constant(WTF::move(yes));
    } else {
        Draft question(asksByte ? Opcode::IsPrototypeChainPlain : Opcode::AreMembersUnchanged, origin);
        if (asksByte)
            question.setConstructorOperand(walker.referenceOf(operation.constructorOperand()));
        question.setConditions(conditions.span());
        answered = walker.build(WTF::move(question));
    }
    walker.bind(operation.id(), answered);
    if (walker.checksWalk()) {
        for (ValueID constructor : { operation.constructorOperand(), operation.secondOperand(), operation.thirdOperand(), operation.fourthOperand() }) {
            if (!constructor)
                continue;
            Reference classObject = walker.referenceOf(constructor);
            walker.askClassAgainForChainAbovePrototype(classObject, singleFunctionOf(walker.types(), walker.types().at(walker.typeOf(classObject))), origin, walker.takesGuardsAndFactsToHold() ? Reference() : answered);
        }
    }
    walker.noteGuessOpened();
    return true;
}

static LayoutID layoutOfOnlyLiteralThatFunctionReturnsForGuesses(const Walker& walker, FunctionID maker, bool& isBornWithoutPrototype)
{
    const BodyIndex* index = walker.facts().bodies().find(maker);
    if (!index)
        return { };
    const Program& program = walker.program();
    ValueID literal;
    LayoutID layout;
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        for (BodyValue made : index->block(BlockID(number)).values()) {
            DataID data = made.opcode() == Opcode::Data ? made.as<Opcode::Data>().data() : DataID();
            const auto* written = data ? program.at(data).root.tryAs<DataNode::Object>() : nullptr;
            if (made.opcode() != Opcode::NewObject && !written)
                continue;
            if (literal)
                return { };
            literal = made.id();
            layout = written ? written->layout : made.as<Opcode::NewObject>().layout();
        }
    }
    if (!literal)
        return { };
    auto isLiteral = [&](ValueID value) {
        value = index->valueBehindCasts(value);
        if (!index->isParameter(value) && index->opcodeOf(value) == Opcode::FinishObject)
            value = index->valueBehindCasts(index->value(value).operand(0));
        return value == literal;
    };
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        for (BodyValue laid : index->block(BlockID(number)).values()) {
            if (laid.opcode() != Opcode::SetLiteralPrototype)
                continue;
            ValueID above = index->valueBehindCasts(laid.as<Opcode::SetLiteralPrototype>().prototypeOperand());
            if (!isLiteral(laid.as<Opcode::SetLiteralPrototype>().objectOperand()) || index->isParameter(above) || index->opcodeOf(above) != Opcode::Null)
                return { };
            isBornWithoutPrototype = true;
        }
        BodyTerminator end = index->terminator(BlockID(number));
        if (end.kind() == TerminatorKind::ReturnCall)
            return { };
        if (end.kind() == TerminatorKind::Return && (!end.operand(0) || !isLiteral(end.operand(0))))
            return { };
    }
    return layout;
}

bool askCellWhetherItIsAsBorn(Walker& walker, const BodyValue& operation)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    walker.noteQuestionForPlainInstance(false);
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    TypeArena& types = walker.types();
    Reference value = walker.referenceOf(operation.objectOperand());
    Reference maker = walker.referenceOf(operation.makerOperand());
    if (!value || !value.isValue() || !maker || (objectPrototypeMayGainName(program) && !byteTellsOfNamesAddedToObjectPrototype(program)))
        return false;
    Type typeOfMaker = types.at(walker.typeOf(maker));
    FunctionID function = typeOfMaker.isObject() && typeOfMaker.isJavaScriptValue() ? singleFunctionOf(types, typeOfMaker) : FunctionID();
    bool isBornWithoutPrototype = false;
    LayoutID layout = function ? layoutOfOnlyLiteralThatFunctionReturnsForGuesses(walker, function, isBornWithoutPrototype) : LayoutID();
    if (!layout || program.at(layout).kind != LayoutKind::Record || !plan.isInUse(layout))
        return false;
    for (const Field& field : program.at(layout).fields) {
        std::optional<FieldIndex> slot = field.key ? plan.slots().findField(program, layout, field.key) : std::nullopt;
        if (!slot || plan.slot(layout, *slot).holdsAccessor)
            return false;
    }
    LayoutSetID receivers = types.at(types.forLayout(layout)).layouts;
    if (shapesOfLayoutsForGuesses(walker, receivers) != ShapesOfLayoutsForGuesses::Testable)
        return false;
    walker.noteQuestionForPlainInstance(true);
    Draft question(Opcode::IsPlainInstanceOf, walker.originOf(operation));
    walker.chooseConservativeFields(question);
    question.setObjectOperand(value);
    question.setReceivers(receivers);
    question.setIsAskedWhetherAsBorn(true);
    question.setIsBornWithoutPrototype(isBornWithoutPrototype);
    question.setMayHoldAddedNames(operation.opcode() == Opcode::IsAsBornExceptForAddedNames);
    Reference asked = walker.build(WTF::move(question));
    walker.bind(operation.id(), asked);
    if (asked && asked.isValue())
        noteKnown(walker, KnownNote::AnswerOfQuestionStillStands, Reference::value(walker.code().at(asked.id()).objectOperand()), asked, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks }), 1, true);
    walker.noteGuessOpened();
    return true;
}

static constexpr uint32_t closedOperationsOfQuestionAboutOwnProperty = 2;

LayoutSetID receiversWhoseCellsAnswerForOwnProperty(Walker& walker, ValueID input, PropertyKeyID key)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return { };
    Reference value = walker.referenceOf(input);
    if (!value || !value.isValue())
        return { };
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(value);
    ExpectationForGuesses expectation = saysNothingForGuesses(types, proven) ? findExpectationForGuesses(walker, input) : ExpectationForGuesses { proven, true };
    if (saysNothingForGuesses(types, expectation.type))
        return { };
    TypeID objects = types.withOnlyObjects(expectation.type);
    Type type = types.at(objects);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || shapesOfLayoutsForGuesses(walker, type.layouts) != ShapesOfLayoutsForGuesses::Testable)
        return { };
    if (!shouldOpenGuess(walker, value, objects, expectation.isDeclared, closedOperationsOfQuestionAboutOwnProperty) || !cellAnswersForOwnProperty(walker, objects, key))
        return { };
    return type.layouts;
}

bool openGuessAtAllocation(Walker& walker, const BodyValue& operation)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(operation.id()));
    uint32_t begin = operation.id().index() - block.firstValue().index();
    auto allocation = operation.as<Opcode::NewObject>();
    std::span<const ValueID> values = operation.variadicOperands();
    if (begin >= limitOfStretchOfGuess(walker, block) || standsInNoStretchOfGuess(index, operation))
        return false;
    if (!walker.plan().layout(allocation.layout()).holdsCompiledObjects() || allocation.fields().size() != values.size())
        return false;

    TypeArena& types = walker.types();
    StretchOfGuess stretch { begin + 1, 0, { } };
    for (size_t place = 0; place < values.size(); ++place) {
        TypeID held = walker.plan().slot(allocation.layout(), allocation.fields()[place]).typeInLayoutRecord();
        Reference value = walker.referenceOf(values[place]);
        TypeID proven = value ? walker.typeOf(value) : TypeID();
        if (!held || !proven || types.isSubset(proven, held))
            continue;
        if (!value.isValue() || !types.overlap(proven, held) || standsBehindTestOfGuess(walker.code(), value, TestOfGuess { held, { } }))
            return false;
        if (!stretch.numberOfTypedOperations)
            stretch.numberOfTypedOperations = static_cast<uint32_t>(values.size() - place);
        stretch.testsOfStoredValues.append({ values[place], held });
    }
    if (stretch.testsOfStoredValues.isEmpty())
        return false;

    TestOfStoredValueOfGuess first = stretch.testsOfStoredValues[0];
    stretch.testsOfStoredValues.removeAt(0);
    Reference value = walker.referenceOf(first.value);
    if (!shouldOpenGuess(walker, value, first.tested, true, std::max(stretch.numberOfTypedOperations, minimumOperationsPerGuess)))
        return false;
    openGuessWithTest(walker, value, TestOfGuess { first.tested, { } }, block.id(), begin, stretch);
    return true;
}

static ValueID valueBehindCastsOfLoopForGuesses(const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    while (value.index() >= firstValueOfLoop && !index.isParameter(value) && inputOperationsThatHandOn.contains(index.opcodeOf(value)) && index.value(value).operand(0))
        value = index.value(value).operand(0);
    return value;
}

static Reference holderThatEntersLoopForGuesses(Walker& walker, const BodyIndex& index, const BodyValue& access, uint32_t firstValueOfLoop)
{
    ValueID inFront = valueBehindCastsOfLoopForGuesses(index, access.objectOperand(), firstValueOfLoop);
    if (!inFront)
        return { };
    return inFront.index() < firstValueOfLoop ? walker.referenceOf(inFront) : nameOfListThatEntersLoopAround(walker, index, access);
}

static bool mayRunCodeAsOperatorForGuesses(Walker& walker, const BodyValue& operation, const TypesOfInputs& types)
{
    if (effectRuleOf(operation.opcode()) != EffectRule::Operator)
        return false;
    Vector<TypeID, 4> operandTypes;
    operandTypes.fill(TypeID(), numberOfNamedOperandsOf(operation.opcode()));
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex < operandTypes.size())
            operandTypes[operandIndex] = types(operand);
    });
    TypeID result = typeByRowForGuesses(walker.typing(), operation, [&](ValueID operand, uint32_t) {
        return types(operand);
    });
    return effectsOfOperator(walker.typing(), operation.opcode(), operandTypes.span(), result).mayRunOtherCode();
}

class TextOfLoopForGuesses {
public:
    TextOfLoopForGuesses(const BodyIndex& index, LoopID loop, uint32_t firstValueOfLoop)
        : m_index(index)
        , m_loop(loop)
        , m_firstValueOfLoop(firstValueOfLoop)
    {
    }

    bool multipliesElementsOf(ValueID holder);
    bool holdsElementsAgainstElements(ValueID holder);
    bool comparesElementsWithCharacters(const Program&, ValueID holder);
    bool comparesElementsWithNumbers(const Program&, ValueID holder);
    const BitVector& holdersOfElementsComparedWithCharacters() const { return m_meetCharacters; }
    const BitVector& holdersOfElementsComparedWithNumbers() const { return m_meetNumbers; }
    bool switchesOnElementsOf(ValueID holder) const { return m_areSwitchedOn.get(holder.index()); }
    bool turnsCall();
    bool turnsStoreIntoWhatMayBeList(Walker&);

private:
    const BodyIndex& m_index;
    LoopID m_loop;
    uint32_t m_firstValueOfLoop;
    bool m_isRead { false };
    BitVector m_giveFactors;
    BitVector m_areHeldAgainstElements;
    bool m_comparisonsAreRead { false };
    BitVector m_meetCharacters;
    BitVector m_meetNumbers;
    BitVector m_areSwitchedOn;
    std::optional<bool> m_turnsCall;
    std::optional<bool> m_turnsStoreIntoWhatMayBeList;
};

struct HoldersInLoopThatAreListsForGuesses {
    TextOfLoopForGuesses text;
    bool areFound { false };
    Vector<Reference, 2> holders;
};

static TypeID listWhereOtherHolderInLoopIsOneForGuesses(Walker&, LoopID, uint32_t firstValueOfLoop, HoldersInLoopThatAreListsForGuesses&, const BodyValue& access, Reference value);

static bool isConvertedByViewOfNumbersForGuesses(Walker& walker, const BodyValue& operation, ValueID operand, const TypesOfInputs& types)
{
    if (operation.opcode() != Opcode::ElemSet || operand != operation.valueOperand() || operand == operation.objectOperand() || operand == operation.propertyOperand())
        return false;
    TypeID holder = types(operation.objectOperand());
    TypedArrayType kind = typedArrayTypeOf({ walker.program(), walker.types() }, holder);
    return (isTypedView(kind) && hasInlineElementAccess(kind)) || kindsOfViewsOfNumbersIn(walker.program(), walker.types(), holder);
}

Vector<GuessInFrontOfLoop, 4> findGuessesInFrontOfLoop(Walker& walker, BlockID inputHead, const LoopPreview& preview)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();

    auto typeInLook = [&](ValueID input) -> TypeID {
        if (input.index() < firstValueOfLoop)
            return provenTypeOfInputForGuesses(walker, input);
        size_t place = input.index() - preview.firstValue;
        TypeID type = input.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
        return type ? type : TypeID(TypeTable::anything);
    };
    TypesOfInputs typesOfLook(typeInLook);

    Vector<GuessInFrontOfLoop, 4> guesses;
    Vector<ValueID, 16> mentioned;
    auto mention = [&](ValueID operand) {
        if (operand && operand.index() < firstValueOfLoop)
            mentioned.append(operand);
    };
    std::optional<bool> someTurnAccessesElements;
    auto turnsAccessElements = [&] {
        if (!someTurnAccessesElements) {
            someTurnAccessesElements = std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
                return std::ranges::any_of(index.block(member).values(), [](BodyValue operation) {
                    return operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet;
                });
            });
        }
        return *someTurnAccessesElements;
    };
    BlocksThatEveryTurnPasses passedByEveryTurn(index, loop);
    Vector<TypeID, 2> provenOfHoldersThatEveryTurnReads;
    HoldersInLoopThatAreListsForGuesses holdersThatAreLists { TextOfLoopForGuesses(index, loop, firstValueOfLoop) };
    auto isHeldAgainstElement = [&](const BodyValue& read) {
        ValueID user = index.onlyUsingValue(read.id());
        while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
            user = index.onlyUsingValue(user);
        if (!user || !allEqualities.contains(index.opcodeOf(user)))
            return false;
        bool meetsElementsAlone = true;
        index.value(user).forEachOperand([&](ValueID operand, uint32_t) {
            ValueID element = index.valueBehindCasts(operand);
            meetsElementsAlone &= !index.isParameter(element) && index.opcodeOf(element) == Opcode::ElemGet;
        });
        return meetsElementsAlone;
    };
    for (bool asksBesideWhatEveryTurnReads : { false, true }) {
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        bool isPassedByEveryTurn = passedByEveryTurn.contains(member);
        if (asksBesideWhatEveryTurnReads && (isPassedByEveryTurn || (guesses.isEmpty() && provenOfHoldersThatEveryTurnReads.isEmpty()) || index.loopOf(member) != loop))
            continue;
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mention(operand);
            });
            if (isPassedByEveryTurn == asksBesideWhatEveryTurnReads || !operationsThatOpenGuesses.contains(operation.opcode()) || isReadOfMethodForItsCallForGuesses(index, operation))
                continue;
            if (asksBesideWhatEveryTurnReads && operation.opcode() != Opcode::ElemGet)
                continue;
            if (operation.opcode() == Opcode::ElemGet && walker.standsInCopyOfLoopBuiltForEachMember() && rowFoundUnderKeyInFrontOfLoop(walker, operation))
                continue;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                ValueID inFront = valueBehindCastsOfLoopForGuesses(index, operand, firstValueOfLoop);
                ValueID readOfVariable = inFront.index() >= firstValueOfLoop && (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet) && operand == operation.objectOperand() && walker.otherHoldersAreAskedForListsInCopyOf(inputHead) ? readOfVariableThatFindsReceiverInLoop(index, operation, firstValueOfLoop) : ValueID();
                if (!asksBesideWhatEveryTurnReads && operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operand == operation.objectOperand() && walker.program().nameClassOf(operation.key()) == NameClass::Length && walker.otherHoldersAreAskedForListsInCopyOf(inputHead)) {
                    Reference measured = nameOfListThatEntersLoopAround(walker, index, operation);
                    if (measured && measured.isValue() && types.at(walker.typeOf(measured)).isObject())
                        provenOfHoldersThatEveryTurnReads.append(walker.typeOf(measured));
                }
                if ((inFront.index() >= firstValueOfLoop && !readOfVariable) || (asksBesideWhatEveryTurnReads && operand != operation.objectOperand()))
                    return;
                if (!asksBesideWhatEveryTurnReads && !readOfVariable && operation.opcode() == Opcode::ElemGet && operand == operation.objectOperand() && walker.standsInCopyOfLoopBuiltForEachMember()) {
                    Reference holder = walker.referenceOf(inFront);
                    if (holder && holder.isValue() && types.at(walker.typeOf(holder)).isObject())
                        provenOfHoldersThatEveryTurnReads.append(walker.typeOf(holder));
                }
                bool isConvertedByView = isConvertedByViewOfNumbersForGuesses(walker, operation, operand, typesOfLook);
                if (isOnlyHandedOnForGuesses(operation, operandIndex) && !isConvertedByView)
                    return;
                Reference value = readOfVariable ? nameOfListThatEntersLoopAround(walker, index, operation) : walker.referenceOf(inFront);
                if (!asksBesideWhatEveryTurnReads && readOfVariable && operation.opcode() == Opcode::ElemGet && walker.standsInCopyOfLoopBuiltForEachMember() && value && value.isValue() && types.at(walker.typeOf(value)).isObject())
                    provenOfHoldersThatEveryTurnReads.append(walker.typeOf(value));
                bool isListed = guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
                    return guess.value == value;
                });
                if (!value || !value.isValue() || isListed)
                    return;

                ExpectationForGuesses expectation = callsOfValuesForGuesses.contains(operation.opcode())
                    ? expectationOfCalleeForGuesses(walker, operation, operandIndex)
                    : findExpectationForGuesses(walker, operand);
                if (!expectation.type && isConvertedByView)
                    expectation.type = TypeTable::number;
                if (!expectation.type && allRelations.contains(operation.opcode()) && operandIndex < 2 && turnsAccessElements()) {
                    TypeID other = typeInLook(operation.operand(1 - operandIndex));
                    if (other && types.at(other).isNumber())
                        expectation.type = TypeTable::number;
                }
                bool isOnlyMeasured = false;
                if (!expectation.type && operation.opcode() == Opcode::PropGet && !operation.isFreeName() && operand == operation.objectOperand() && member == inputHead
                    && walker.program().nameClassOf(operation.key()) == NameClass::Length && saysNothingForGuesses(types, walker.typeOf(value)) && turnsAccessElements()) {
                    bool boundsParameterOfHead = std::ranges::any_of(block.values(), [&](BodyValue relation) {
                        if (!allRelations.contains(relation.opcode()))
                            return false;
                        for (uint32_t side = 0; side < 2; ++side) {
                            ValueID held = index.valueBehindCasts(relation.operand(1 - side));
                            if (index.valueBehindCasts(relation.operand(side)) == operation.id() && index.isParameter(held) && index.blockOf(held) == inputHead)
                                return true;
                        }
                        return false;
                    });
                    bool nothingButLengthIsNamed = std::ranges::all_of(walker.namesUsedOff(index.valueBehindCasts(operand)), [&](PropertyKeyID key) {
                        return walker.program().nameClassOf(key) == NameClass::Length;
                    });
                    ValueID holder = index.valueBehindCasts(operand);
                    bool loopOnlyMeasuresIt = std::ranges::all_of(index.membersOf(loop), [&](BlockID inLoop) {
                        return std::ranges::all_of(index.block(inLoop).values(), [&](BodyValue other) {
                            bool namesHolder = false;
                            other.forEachOperand([&](ValueID named, uint32_t) {
                                namesHolder |= named && index.valueBehindCasts(named) == holder;
                            });
                            return !namesHolder || inputOperationsThatOnlyHandOn.contains(other.opcode()) || (other.opcode() == Opcode::PropGet && !other.isFreeName() && walker.program().nameClassOf(other.key()) == NameClass::Length);
                        });
                    });
                    isOnlyMeasured = boundsParameterOfHead && nothingButLengthIsNamed && loopOnlyMeasuresIt;
                    if (isOnlyMeasured)
                        expectation.type = types.builtinClass(BuiltinClasses::Array);
                }
                bool isAskedBesideList = false;
                if (!expectation.type && walker.otherHoldersAreAskedForListsInCopyOf(inputHead) && (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet) && operand == operation.objectOperand() && saysNothingForGuesses(types, walker.typeOf(value))) {
                    expectation.type = listWhereOtherHolderInLoopIsOneForGuesses(walker, loop, firstValueOfLoop, holdersThatAreLists, operation, value);
                    isAskedBesideList = !!expectation.type;
                }
                if (readOfVariable && !isAskedBesideList)
                    return;
                std::optional<TestOfGuess> chosen = expectation.type ? chooseTestOfGuess(walker, value, expectation.type) : std::nullopt;
                if (!chosen || (allEqualities.contains(operation.opcode()) && !expectation.isDeclared))
                    return;
                if (asksBesideWhatEveryTurnReads && !guesses.containsIf([&](const GuessInFrontOfLoop& guess) { return guess.tested == chosen->tested; }) && !((isHeldAgainstElement(operation) || (isAskedBesideList && loopReckonsWithElementRead(walker, operation))) && provenOfHoldersThatEveryTurnReads.containsIf([&](TypeID proven) { return types.isSubset(proven, chosen->tested); })))
                    return;
                bool rulesOutNullish = accessesToSlotsForGuesses.contains(operation.opcode()) && operand == operation.objectOperand() && types.withoutNullish(walker.typeOf(value)) != walker.typeOf(value);
                TypeID typeBehindTest = types.intersectionOf(walker.typeOf(value), chosen->tested);
                auto typeWithProof = [&](ValueID input) -> TypeID {
                    ValueID behindCasts = valueBehindCastsOfLoopForGuesses(index, input, firstValueOfLoop);
                    if (behindCasts.index() >= firstValueOfLoop)
                        return readOfVariable && behindCasts == inFront ? typeBehindTest : typeInLook(input);
                    Reference inFrontOfLoop = walker.referenceOf(behindCasts);
                    if (inFrontOfLoop == value)
                        return typeBehindTest;
                    for (const GuessInFrontOfLoop& guess : guesses) {
                        if (guess.value == inFrontOfLoop)
                            return types.intersectionOf(walker.typeOf(inFrontOfLoop), guess.tested);
                    }
                    return typeInLook(input);
                };
                TypesOfInputs typesWithProof(typeWithProof);
                if (typedFormForGuesses(walker, operation, typesWithProof) == TypedForm::None)
                    return;
                bool isTypedWithoutProof = typedFormForGuesses(walker, operation, typesOfLook) != TypedForm::None;
                if (isTypedWithoutProof && !rulesOutNullish && !isConvertedByView && (!mayRunCodeAsOperatorForGuesses(walker, operation, typesOfLook) || mayRunCodeAsOperatorForGuesses(walker, operation, typesWithProof)))
                    return;
                if (isAskedBesideList)
                    walker.count(StatisticID::ListsAskedForBesideListsInFrontOfLoops);
                if (isOnlyMeasured)
                    walker.count(StatisticID::HoldersOnlyMeasuredByHeadsGuessedToBeLists);
                guesses.append(GuessInFrontOfLoop { value, chosen->tested, chosen->layouts, { } });
                guesses.last().readOfVariableInLoop = readOfVariable;
            });
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, uint32_t) {
            mention(operand);
        });
        for (BodyTarget target : end.targets()) {
            for (ValueID argument : target.arguments())
                mention(argument);
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                mention(argument);
        }
    }
    }
    if (guesses.isEmpty())
        return guesses;
    Vector<std::pair<uint32_t, uint32_t>, 4> guessesByValue;
    for (uint32_t number = 0; number < guesses.size(); ++number)
        guessesByValue.append({ guesses[number].value.bits(), number });
    std::ranges::sort(guessesByValue);
    for (ValueID name : mentioned) {
        uint32_t value = walker.referenceOf(name).bits();
        auto found = std::ranges::lower_bound(guessesByValue, std::pair<uint32_t, uint32_t> { value, 0 });
        if (found == guessesByValue.end() || found->first != value)
            continue;
        Vector<ValueID, 2>& names = guesses[found->second].names;
        if (!names.contains(name))
            names.append(name);
    }
    return guesses;
}

static constexpr size_t mostMembersToldApartInFrontOfLoop = 4;
static constexpr OpcodeSet accessesToldApartInFrontOfLoop { Opcode::PropGet, Opcode::PropSet, Opcode::ElemGet, Opcode::ElemSet, Opcode::Call };

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> membersOfSetForGuesses(Walker& walker, TypeID expected)
{
    TypeArena& types = walker.types();
    TypeID objects = types.withOnlyObjects(expected);
    Type type = types.at(objects);
    if (!type.hasObjects() || type.hasUnboundedUnknown())
        return { };
    if (type.hasOnlyClosures()) {
        Vector<FunctionID, mostMembersToldApartInFrontOfLoop> functions;
        bool areFew = true;
        forEachFunctionOf(types, type, scopedLambda<void(FunctionID)>([&](FunctionID function) {
            if (functions.size() == mostMembersToldApartInFrontOfLoop)
                areFew = false;
            else
                functions.append(function);
        }));
        if (!areFew || functions.size() < 2)
            return { };
        Vector<TypeID, mostMembersToldApartInFrontOfLoop> closures;
        for (FunctionID function : functions)
            closures.append(types.forFunction(function));
        return closures;
    }
    std::span<const ClassPart> classes = types.at(type.classes);
    std::span<const LayoutID> layouts = types.listOf(type.layouts);
    if (classes.size() + layouts.size() < 2 || classes.size() + layouts.size() > mostMembersToldApartInFrontOfLoop)
        return { };
    Vector<TypeID, mostMembersToldApartInFrontOfLoop> members;
    for (size_t place = 0; place < classes.size(); ++place)
        members.append(types.intersectionOf(objects, types.builtinClass(types.at(type.classes)[place].builtinClass)));
    for (size_t place = 0; place < layouts.size(); ++place)
        members.append(types.intersectionOf(objects, types.forLayout(types.listOf(type.layouts)[place])));
    for (size_t place = 0; place < members.size(); ++place) {
        if (!members[place] || isNeverType(members[place]))
            return { };
        for (size_t other = 0; other < place; ++other) {
            if (types.overlap(members[place], members[other]))
                return { };
        }
    }
    return members;
}

static ValueID valueBehindCastsAndTestsForGuesses(const BodyIndex& index, ValueID value)
{
    ValueID behind = index.valueBehindCasts(value);
    while (ValueID tested = index.valueNarrowedInto(behind))
        behind = index.valueBehindCasts(tested);
    return behind;
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> listBytesAndOtherViewsForGuesses(TypeArena& types, TypeID list, bool fractionsAreKept, bool turnsCall)
{
    TypeID otherViews = types.builtinClass(BuiltinClasses::Int8Array);
    for (BuiltinClassID builtinClass : { BuiltinClasses::Uint8ClampedArray, BuiltinClasses::Int16Array, BuiltinClasses::Uint16Array, BuiltinClasses::Int32Array, BuiltinClasses::Uint32Array })
        otherViews = types.unionOf(otherViews, types.builtinClass(builtinClass));
    TypeID doubles = types.builtinClass(BuiltinClasses::Float64Array);
    TypeID floats = types.builtinClass(BuiltinClasses::Float32Array);
    Vector<TypeID, mostMembersToldApartInFrontOfLoop> members { list };
    if (!turnsCall)
        members.append(types.builtinClass(BuiltinClasses::Array));
    if (turnsCall) {
        members.append(types.builtinClass(BuiltinClasses::Buffer));
        members.append(types.builtinClass(BuiltinClasses::Uint8Array));
    } else
        members.append(types.unionOf(types.builtinClass(BuiltinClasses::Buffer), types.builtinClass(BuiltinClasses::Uint8Array)));
    if (fractionsAreKept) {
        members.append(doubles);
        members.append(floats);
        members.append(otherViews);
    } else
        members.append(types.unionOf(otherViews, types.unionOf(doubles, floats)));
    return members;
}

static bool listsAndViewsAnswerNameForGuesses(const Program& program, PropertyKeyID key)
{
    BuiltinName name = liesInThread(key) ? BuiltinName() : program.builtinNameOf(key);
    if (!name)
        return false;
    const BuiltinTable& table = program.builtins();
    auto isAnsweredBy = [&](BuiltinClassID builtinClass) {
        return std::ranges::any_of(table.classesWithInstanceMember(name), [&](BuiltinClassID holder) {
            return table.isSameOrSubclassOf(builtinClass, holder);
        });
    };
    return isAnsweredBy(BuiltinClasses::Array) && isAnsweredBy(BuiltinClasses::Uint8Array);
}

static bool answersUnderNameAreDroppedForGuesses(const BodyIndex& index, ValueID holder, PropertyKeyID key)
{
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (operation.opcode() != Opcode::PropGet)
                continue;
            auto read = operation.as<Opcode::PropGet>();
            if (read.isFreeName() || read.key() != key || valueBehindCastsAndTestsForGuesses(index, read.objectOperand()) != holder)
                continue;
            ValueID call = index.onlyUsingValue(operation.id());
            if (!call || index.opcodeOf(call) != Opcode::Call || index.value(call).calleeOperand() != operation.id() || !index.isUnused(call))
                return false;
        }
    }
    return true;
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> listAndBytesThatMayComeForGuesses(Walker& walker, ValueID operand, TextOfLoopForGuesses* text = nullptr, ValueID holder = { })
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    FunctionID current = walker.currentFunction();
    const Function& function = program.at(current);
    const LinkTables& link = walker.facts().linked();
    if (!operand || !index.hasLoops() || index.hasIrreducibleFlow() || current.index() >= link.functions.size())
        return { };
    ValueID parameter = valueBehindCastsAndTestsForGuesses(index, operand);
    std::span<const LinkedParameter> linked = link.parametersOf(current);
    if (!index.isParameter(parameter) || index.blockOf(parameter) != entryBlock || parameter.index() >= linked.size() || function.entryParameterKind(parameter.index()) != EntryParameter::Parameter)
        return { };
    const DeclaredType* declared = function.has(FunctionFlags::IsChecked) ? function.declaredEntryParameter(parameter.index()) : nullptr;
    if (function.has(FunctionFlags::IsChecked) && (!declared || !declared->isExpected()))
        return { };
    if (saysNothingForGuesses(types, linked[parameter.index()].written))
        return { };
    TypeID list = types.withOnlyObjects(linked[parameter.index()].written);
    if (!isDeclaredToHoldNumbersAlone(program, types, list))
        return { };
    bool mayBeText = !!(types.at(linked[parameter.index()].written).primitives & Primitive::AnyString);
    bool someNameIsOfListsAndViews = false;
    bool noNameIsOfListsAlone = std::ranges::all_of(walker.namesUsedOff(parameter), [&](PropertyKeyID key) {
        if (program.nameClassOf(key) == NameClass::Length)
            return true;
        bool isOfListsAndViews = !mayBeText && listsAndViewsAnswerNameForGuesses(program, key) && answersUnderNameAreDroppedForGuesses(index, parameter, key);
        someNameIsOfListsAndViews |= isOfListsAndViews;
        return isOfListsAndViews;
    });
    if (!noNameIsOfListsAlone)
        return { };
    if (someNameIsOfListsAndViews)
        walker.count(StatisticID::HoldersAskedForBytesPastNamesOfListsAndViews);
    return listBytesAndOtherViewsForGuesses(types, list, !text || text->multipliesElementsOf(holder), !text || text->turnsCall());
}

static TypeID viewsThatMayComeWhereUseWroteListForGuesses(Walker& walker, ValueID holder, TypeID expected)
{
    if (!listAndBytesThatMayComeForGuesses(walker, holder).isEmpty())
        return walker.types().unionOf(walker.types().builtinClass(BuiltinClasses::Buffer), walker.types().builtinClass(BuiltinClasses::Uint8Array));
    if (!expected || !isExpectedToBeListOfNumbers(walker, walker.types().withoutNullish(expected)))
        return { };
    return viewsSeenToArriveWhereUseWroteRowOfNumbersForGuesses(walker, holder);
}

static TypeID viewsSeenToArriveWhereUseWroteRowOfNumbersForGuesses(Walker& walker, ValueID holder)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID row = valueBehindCastsAndTestsForGuesses(index, holder);
    if (walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) || !row || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet)
        return { };
    TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    TypeID arrives = walker.facts().held().resultOfReadByKey(walker.currentFunction(), row);
    if (!arrives)
        return { };
    TypeID views;
    for (unsigned position = 0; position < table.classCount(); ++position) {
        BuiltinClassID builtinClass(position);
        TypedArrayType kind = table.typedArrayTypeOf(builtinClass);
        if (kind == NotTypedArray || kind == TypeDataView || isBigInt(kind) || !types.overlap(arrives, types.builtinClass(builtinClass)))
            continue;
        views = views ? types.unionOf(views, types.builtinClass(builtinClass)) : types.builtinClass(builtinClass);
    }
    return views;
}

static bool onlyAsksWhatItTakesForGuesses(const BodyIndex& index, const BodyValue& operation)
{
    static constexpr OpcodeSet questions { Opcode::ToBoolean, Opcode::TypeTest, Opcode::TypeOf, Opcode::StrictEq, Opcode::Eq, Opcode::InstanceOf };
    if (questions.contains(operation.opcode()))
        return true;
    ValueID callee = operation.opcode() == Opcode::Call ? operation.calleeOperand() : ValueID();
    return callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::Builtin && index.value(callee).as<Opcode::Builtin>().builtin() == Builtins::ArrayIsArray;
}

struct ParametersReadInLoopsForGuesses {
    bool leaveFrontStanding(ValueID parameter) const
    {
        return haveElementsReadInLoops.get(parameter.index()) && !areReadInLoopWithoutFront.get(parameter.index()) && !giveLoopsWhatIsReadOutside.get(parameter.index());
    }

    BitVector haveElementsReadInLoops;
    BitVector areReadInLoopWithoutFront;
    BitVector giveLoopsWhatIsReadOutside;
    BitVector haveElementsComparedWithCharacters;
    BitVector haveElementsHeldAgainstElements;
};

static bool isTextOfOneUnitForGuesses(const Program& program, StringID written)
{
    Vector<char16_t> buffer;
    return viewOfProgramString(program.string(written), buffer).length() == 1;
}

static ValueID openOfStepThatHandsOutForGuesses(const BodyIndex& index, ValueID handedOut)
{
    if (index.isParameter(handedOut) || index.opcodeOf(handedOut) != Opcode::ResultAt)
        return { };
    auto result = index.value(handedOut).as<Opcode::ResultAt>();
    ValueID step = result.resultsOperand();
    if (result.index() != static_cast<uint32_t>(IteratorResumeResult::Value) || index.isParameter(step) || index.opcodeOf(step) != Opcode::IteratorResume)
        return { };
    ValueID open = index.value(step).as<Opcode::IteratorResume>().iteratorOperand();
    return !index.isParameter(open) && index.opcodeOf(open) == Opcode::IteratorOpen ? open : ValueID();
}

static ValueID holderOfElementComparedWithCharacterForGuesses(const Program& program, const BodyIndex& index, const BodyValue& operation)
{
    if (operation.opcode() == Opcode::Call) {
        auto call = operation.as<Opcode::Call>();
        ValueID callee = call.calleeOperand();
        if (index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin || index.value(callee).as<Opcode::Builtin>().builtin() != Builtins::Number || call.argumentsOperands().size() != 1)
            return { };
        ValueID handed = valueBehindCastsAndTestsForGuesses(index, call.argumentsOperands()[0]);
        return !index.isParameter(handed) && index.opcodeOf(handed) == Opcode::ElemGet ? valueBehindCastsAndTestsForGuesses(index, index.value(handed).objectOperand()) : ValueID();
    }
    if (!allEqualities.contains(operation.opcode()) && !allRelations.contains(operation.opcode()))
        return { };
    ValueID holder;
    bool comparesWithCharacter = false;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        ValueID compared = valueBehindCastsAndTestsForGuesses(index, operand);
        if (index.isParameter(compared))
            return;
        if (index.opcodeOf(compared) == Opcode::ElemGet)
            holder = valueBehindCastsAndTestsForGuesses(index, index.value(compared).objectOperand());
        else if (ValueID open = openOfStepThatHandsOutForGuesses(index, compared))
            holder = open;
        else if (index.opcodeOf(compared) == Opcode::String)
            comparesWithCharacter |= isTextOfOneUnitForGuesses(program, index.value(compared).as<Opcode::String>().string());
    });
    return comparesWithCharacter ? holder : ValueID();
}

static bool isNumberByWhatMakesItForGuesses(const BodyIndex& index, ValueID value)
{
    Vector<ValueID, 4> parts { value };
    BitVector seen;
    while (!parts.isEmpty()) {
        ValueID made = valueBehindCastsAndTestsForGuesses(index, parts.takeLast());
        if (index.isParameter(made))
            return false;
        if (seen.set(made.index()))
            continue;
        Opcode opcode = index.opcodeOf(made);
        if (opcode == Opcode::Integer || opcode == Opcode::Double || operatorsThatMakeNumbers.contains(opcode))
            continue;
        if (!allSums.contains(opcode))
            return false;
        index.value(made).forEachOperand([&](ValueID operand, uint32_t) {
            parts.append(operand);
        });
    }
    return true;
}

static ValueID holderOfElementComparedWithNumberForGuesses(const BodyIndex& index, const BodyValue& operation)
{
    if (!allEqualities.contains(operation.opcode()) && !allRelations.contains(operation.opcode()))
        return { };
    ValueID holder;
    bool comparesWithNumber = false;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        ValueID compared = valueBehindCastsAndTestsForGuesses(index, operand);
        if (index.isParameter(compared))
            return;
        if (index.opcodeOf(compared) == Opcode::ElemGet)
            holder = valueBehindCastsAndTestsForGuesses(index, index.value(compared).objectOperand());
        else
            comparesWithNumber |= isNumberByWhatMakesItForGuesses(index, compared);
    });
    return comparesWithNumber ? holder : ValueID();
}

static ParametersReadInLoopsForGuesses findParametersReadInLoopsForGuesses(const Program& program, const BodyIndex& index)
{
    static constexpr OpcodeSet handsNumbersOn = allNumericOperators | opcodesThatStateTypes;
    static constexpr OpcodeSet reckonsWithWhatItTakes = allNumericOperators | allRelations;
    static constexpr uint32_t nothing = std::numeric_limits<uint32_t>::max();
    ParametersReadInLoopsForGuesses found;
    Vector<uint32_t> parameterReadOutside;
    parameterReadOutside.fill(nothing, index.numberOfValues());
    Vector<uint32_t> parameterBehindCall;
    parameterBehindCall.fill(nothing, index.numberOfValues());
    auto at = [&](const Vector<uint32_t>& parameters, ValueID value) {
        return value && value.index() < parameters.size() ? parameters[value.index()] : nothing;
    };
    auto entryParameterBehind = [&](ValueID value) {
        ValueID behind = value ? valueBehindCastsAndTestsForGuesses(index, value) : ValueID();
        return behind && index.isParameter(behind) && index.blockOf(behind) == entryBlock ? behind.index() : nothing;
    };
    auto givesLoops = [&](uint32_t parameter) {
        if (parameter != nothing)
            found.giveLoopsWhatIsReadOutside.set(parameter);
    };
    auto join = [&](uint32_t& into, uint32_t other) {
        if (other == nothing || into == other)
            return;
        if (into != nothing) {
            givesLoops(into);
            givesLoops(other);
        }
        into = other;
    };
    BitVector loopsThatReckonWithElements = Walker::findLoopsThatReckonWithElements(index);
    auto hasNoFront = [&](LoopID loop) {
        if (Walker::isTooLargeToBeBuiltTwice(index, loop, loopsThatReckonWithElements) || index.isEnteredByHandlerEdge(index.loop(loop).head))
            return true;
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values()) {
                if (operation.opcode() == Opcode::IteratorResume && index.loopOf(member) == loop)
                    return true;
            }
        }
        return false;
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        LoopID loop = index.loopOf(BlockID(number));
        std::optional<bool> loopHasNoFront;
        for (BodyValue operation : block.values()) {
            uint32_t read = nothing;
            uint32_t behindCall = nothing;
            Vector<uint32_t, 2> taken;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                join(read, at(parameterReadOutside, operand));
                join(behindCall, at(parameterBehindCall, operand));
                if (uint32_t parameter = entryParameterBehind(operand); parameter != nothing && !taken.contains(parameter))
                    taken.append(parameter);
            });
            Opcode opcode = operation.opcode();
            if (loop) {
                givesLoops(read);
                if (reckonsWithWhatItTakes.contains(opcode))
                    givesLoops(behindCall);
                if (uint32_t compared = entryParameterBehind(holderOfElementComparedWithCharacterForGuesses(program, index, operation)); compared != nothing)
                    found.haveElementsComparedWithCharacters.set(compared);
                if (allEqualities.contains(opcode)) {
                    Vector<uint32_t, 2> holders;
                    bool meetsElementsAlone = true;
                    operation.forEachOperand([&](ValueID operand, uint32_t) {
                        ValueID element = valueBehindCastsAndTestsForGuesses(index, operand);
                        if (index.isParameter(element) || index.opcodeOf(element) != Opcode::ElemGet) {
                            meetsElementsAlone = false;
                            return;
                        }
                        if (uint32_t holder = entryParameterBehind(index.value(element).objectOperand()); holder != nothing)
                            holders.append(holder);
                    });
                    if (meetsElementsAlone) {
                        for (uint32_t holder : holders)
                            found.haveElementsHeldAgainstElements.set(holder);
                    }
                }
                uint32_t holder = opcode == Opcode::ElemGet || opcode == Opcode::ElemSet ? entryParameterBehind(operation.objectOperand()) : nothing;
                if (holder == nothing)
                    continue;
                if (!loopHasNoFront)
                    loopHasNoFront = hasNoFront(loop);
                (*loopHasNoFront ? found.areReadInLoopWithoutFront : found.haveElementsReadInLoops).set(holder);
                continue;
            }
            bool isRead = opcode == Opcode::ElemGet || opcode == Opcode::PropGet;
            if (!isRead && !onlyAsksWhatItTakesForGuesses(index, operation) && !opcodesThatStateTypes.contains(opcode) && opcode != Opcode::ReleaseHint) {
                for (uint32_t parameter : taken)
                    givesLoops(parameter);
            }
            if (isRead && !taken.isEmpty()) {
                for (uint32_t parameter : taken)
                    join(parameterReadOutside[operation.id().index()], parameter);
            } else if (!isRead && read != nothing && handsNumbersOn.contains(opcode))
                parameterReadOutside[operation.id().index()] = read;
            else if (opcode == Opcode::Call && read != nothing)
                parameterBehindCall[operation.id().index()] = read;
            else if (behindCall != nothing && handsNumbersOn.contains(opcode))
                parameterBehindCall[operation.id().index()] = behindCall;
        }
        BodyTerminator end = block.terminator();
        if (loop) {
            end.forEachOperand([&](ValueID operand, uint32_t) {
                givesLoops(at(parameterReadOutside, operand));
            });
        }
        for (BodyTarget target : end.targets()) {
            std::span<const ValueID> arguments = target.arguments();
            BodyBlock taking = index.block(target.block());
            for (uint32_t position = 0; position < arguments.size() && position < taking.numberOfParameters(); ++position) {
                uint32_t read = at(parameterReadOutside, arguments[position]);
                uint32_t behindCall = at(parameterBehindCall, arguments[position]);
                if (index.loopOf(target.block())) {
                    givesLoops(read);
                    givesLoops(behindCall);
                    continue;
                }
                if (read != nothing)
                    join(parameterReadOutside[taking.parameter(position).index()], read);
                else if (behindCall != nothing)
                    join(parameterBehindCall[taking.parameter(position).index()], behindCall);
            }
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                givesLoops(at(parameterReadOutside, argument));
        }
    }
    return found;
}

static bool loopsTellListFromBytesForGuesses(Walker& walker, ValueID parameter, std::optional<ParametersReadInLoopsForGuesses>& readInLoops)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::LoopInvariants) || !walker.has(Mechanism::Guesses) || index.holdsParametersReadByReleaseHintsAlone())
        return false;
    if (listAndBytesThatMayComeForGuesses(walker, parameter).isEmpty())
        return false;
    if (!readInLoops)
        readInLoops = findParametersReadInLoopsForGuesses(walker.program(), index);
    return readInLoops->leaveFrontStanding(parameter);
}

static bool wordOfEntryParameterIsListOfNoContentsDrawnFromUseForGuesses(Walker&, ValueID operand);
static std::optional<Opcode> operatorThatElementGoesThroughForGuesses(Walker&, const BodyValue& access);

static bool loopsTellListOfNoContentsFromBytesForGuesses(Walker& walker, ValueID parameter, std::optional<ParametersReadInLoopsForGuesses>& readInLoops)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::LoopInvariants) || !walker.has(Mechanism::Guesses) || index.holdsParametersReadByReleaseHintsAlone() || !index.hasLoops() || index.hasIrreducibleFlow())
        return false;
    if (!wordOfEntryParameterIsListOfNoContentsDrawnFromUseForGuesses(walker, parameter))
        return false;
    bool nothingButLengthIsNamed = std::ranges::all_of(walker.namesUsedOff(parameter), [&](PropertyKeyID key) {
        return walker.program().nameClassOf(key) == NameClass::Length;
    });
    if (!nothingButLengthIsNamed)
        return false;
    if (!readInLoops)
        readInLoops = findParametersReadInLoopsForGuesses(walker.program(), index);
    if (!readInLoops->leaveFrontStanding(parameter))
        return false;
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        for (BodyValue operation : index.block(BlockID(block)).values()) {
            if (operation.opcode() == Opcode::ElemGet && valueBehindCastsAndTestsForGuesses(index, operation.objectOperand()) == parameter && !operatorThatElementGoesThroughForGuesses(walker, operation))
                return false;
        }
    }
    return true;
}

static bool loopsOfFunctionTellListFromBytesForGuesses(Walker& walker, ValueID parameter)
{
    std::optional<ParametersReadInLoopsForGuesses> readInLoops;
    return loopsTellListFromBytesForGuesses(walker, parameter, readInLoops) || loopsTellListOfNoContentsFromBytesForGuesses(walker, parameter, readInLoops);
}

static TypeID wordExpectedAtBindingForGuesses(const BodyIndex& index, ValueID operand)
{
    for (ValueID value = operand; value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::Convert; value = index.value(value).operand(0)) {
        DeclaredType declared = index.value(value).as<Opcode::Convert>().declared();
        if (declared.isExpected())
            return declared.written();
    }
    return { };
}

static TypeID listBesideTextInWrittenWordForGuesses(Walker& walker, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    FunctionID current = walker.currentFunction();
    const Function& function = program.at(current);
    const LinkTables& link = walker.facts().linked();
    if (!operand || !index.hasLoops() || index.hasIrreducibleFlow() || current.index() >= link.functions.size())
        return { };
    ValueID parameter = valueBehindCastsAndTestsForGuesses(index, operand);
    std::span<const LinkedParameter> linked = link.parametersOf(current);
    bool isEntryParameter = index.isParameter(parameter) && index.blockOf(parameter) == entryBlock && parameter.index() < linked.size() && function.entryParameterKind(parameter.index()) == EntryParameter::Parameter;
    if (!isEntryParameter && (index.isParameter(parameter) || function.has(FunctionFlags::IsChecked)))
        return { };
    TypeID expected = isEntryParameter ? linked[parameter.index()].written : wordExpectedAtBindingForGuesses(index, operand);
    if (!expected || saysNothingForGuesses(types, expected) || !(types.at(expected).primitives & Primitive::AnyString) || types.at(expected).primitives & ~(Primitive::AnyString | Primitive::UndefinedOrNull) || !types.at(expected).hasObjects())
        return { };
    TypeID list = types.withOnlyObjects(expected);
    const ClassPart* part = singleBuiltinClassOf(types, types.at(list));
    if (!part || part->builtinClass != BuiltinClasses::Array)
        return { };
    bool nothingButLengthIsNamed = std::ranges::all_of(walker.namesUsedOff(parameter), [&](PropertyKeyID key) {
        return program.nameClassOf(key) == NameClass::Length;
    });
    return nothingButLengthIsNamed ? list : TypeID();
}

bool TextOfLoopForGuesses::comparesElementsWithCharacters(const Program& program, ValueID holder)
{
    if (!std::exchange(m_comparisonsAreRead, true)) {
        for (BlockID member : m_index.membersOf(m_loop)) {
            for (BodyValue operation : m_index.block(member).values()) {
                if (ValueID compared = holderOfElementComparedWithCharacterForGuesses(program, m_index, operation))
                    m_meetCharacters.set(compared.index());
                if (ValueID compared = holderOfElementComparedWithNumberForGuesses(m_index, operation))
                    m_meetNumbers.set(compared.index());
            }
            BodyTerminator end = m_index.block(member).terminator();
            bool isOverCharacters = end.kind() == TerminatorKind::SwitchString && std::ranges::all_of(end.strings(), [&](StringID string) { return isTextOfOneUnitForGuesses(program, string); });
            if (!isOverCharacters && end.kind() != TerminatorKind::Switch)
                continue;
            ValueID subject = valueBehindCastsAndTestsForGuesses(m_index, end.subjectOperand());
            if (!m_index.isParameter(subject) && m_index.opcodeOf(subject) == Opcode::ElemGet) {
                ValueID holderOfSubject = valueBehindCastsAndTestsForGuesses(m_index, m_index.value(subject).objectOperand());
                (isOverCharacters ? m_meetCharacters : m_meetNumbers).set(holderOfSubject.index());
                m_areSwitchedOn.set(holderOfSubject.index());
            }
            else if (ValueID open = isOverCharacters ? openOfStepThatHandsOutForGuesses(m_index, subject) : ValueID())
                m_meetCharacters.set(open.index());
        }
    }
    return m_meetCharacters.get(holder.index());
}

bool TextOfLoopForGuesses::comparesElementsWithNumbers(const Program& program, ValueID holder)
{
    comparesElementsWithCharacters(program, holder);
    return m_meetNumbers.get(holder.index());
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> textAndListThatMayComeForGuesses(Walker& walker, ValueID operand, TextOfLoopForGuesses& text)
{
    TypeID list = listBesideTextInWrittenWordForGuesses(walker, operand);
    if (!list || !text.comparesElementsWithCharacters(walker.program(), valueBehindCastsAndTestsForGuesses(walker.currentBodyIndex(), operand)))
        return { };
    return { TypeID(TypeTable::string), list };
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> textAndListWhoseElementsMeetCharactersForGuesses(Walker& walker, ValueID operand, TextOfLoopForGuesses& text)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!operand || index.hasIrreducibleFlow())
        return { };
    ValueID holder = valueBehindCastsAndTestsForGuesses(index, operand);
    if (!text.comparesElementsWithCharacters(walker.program(), holder))
        return { };
    bool nothingButLengthIsNamed = std::ranges::all_of(walker.namesUsedOff(holder), [&](PropertyKeyID key) {
        return walker.program().nameClassOf(key) == NameClass::Length;
    });
    if (!nothingButLengthIsNamed)
        return { };
    return { TypeID(TypeTable::string), walker.types().builtinClass(BuiltinClasses::Array) };
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> listAndViewsWhoseElementsMeetNumbersForGuesses(Walker& walker, ValueID operand, TextOfLoopForGuesses& text)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!operand || index.hasIrreducibleFlow())
        return { };
    ValueID holder = valueBehindCastsAndTestsForGuesses(index, operand);
    if (!text.comparesElementsWithNumbers(walker.program(), holder) || text.turnsCall())
        return { };
    bool nothingButLengthIsNamed = std::ranges::all_of(walker.namesUsedOff(holder), [&](PropertyKeyID key) {
        return walker.program().nameClassOf(key) == NameClass::Length;
    });
    if (!nothingButLengthIsNamed)
        return { };
    TypeID bytes = walker.types().unionOf(walker.types().builtinClass(BuiltinClasses::Buffer), walker.types().builtinClass(BuiltinClasses::Uint8Array));
    if (text.switchesOnElementsOf(holder))
        return { walker.types().builtinClass(BuiltinClasses::Array), bytes };
    TypeID number = TypeTable::number;
    return { walker.types().builtinClass(BuiltinClasses::Array, singleElementSpan(number)), walker.types().builtinClass(BuiltinClasses::Array), bytes };
}

static bool loopsTellTextFromListForGuesses(Walker& walker, ValueID parameter, std::optional<ParametersReadInLoopsForGuesses>& readInLoops)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::LoopInvariants) || !walker.has(Mechanism::Guesses) || index.holdsParametersReadByReleaseHintsAlone())
        return false;
    if (!listBesideTextInWrittenWordForGuesses(walker, parameter))
        return false;
    if (!readInLoops)
        readInLoops = findParametersReadInLoopsForGuesses(walker.program(), index);
    return (readInLoops->haveElementsComparedWithCharacters.get(parameter.index()) || readInLoops->haveElementsHeldAgainstElements.get(parameter.index())) && readInLoops->leaveFrontStanding(parameter);
}

static bool keySpeaksForIndexForGuesses(const BodyIndex& index, const BodyValue& access)
{
    ValueID key = index.valueBehindCasts(access.propertyOperand());
    if (index.isParameter(key))
        return index.isLoopHead(index.blockOf(key));
    Opcode maker = index.opcodeOf(key);
    return maker == Opcode::Integer || maker == Opcode::Add || operatorsThatMakeNumbers.contains(maker) || allBitwiseOperators.contains(maker);
}

static constexpr uint16_t sumSpeaksForNumberForGuesses = 1 << 14;
static constexpr uint16_t sumSpeaksForNoNumberForGuesses = 1 << 15;
static constexpr uint16_t operatorFoundBehindValueForGuesses = sumSpeaksForNumberForGuesses - 1;
static_assert(numberOfOpcodes + 2 <= operatorFoundBehindValueForGuesses);

static std::optional<Opcode> operatorThatValueGoesThroughForGuesses(Walker&, const BodyIndex&, ValueID, Vector<uint16_t>* found);

static Vector<FunctionID, 4> helpersThatCallMayReachForGuesses(Walker& walker, const BodyValue& call)
{
    Vector<FunctionID, 4> helpers;
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id())) {
        helpers.append(callee);
        return helpers;
    }
    TypeArena& types = walker.types();
    TypeID expected = findExpectationForGuesses(walker, call.calleeOperand()).type;
    TypeID closures = expected && !holdsOnlyPrimitives(expected) ? types.withOnlyObjects(expected) : TypeID();
    if (!closures || !types.at(closures).isObject() || !types.at(closures).hasOnlyClosures() || types.at(closures).hasUnknown() || types.at(closures).namesMergedObjects())
        return helpers;
    forEachFunctionOf(types, types.at(closures), scopedLambda<void(FunctionID)>([&](FunctionID function) {
        helpers.append(function);
    }));
    return helpers;
}

static std::optional<Opcode> operatorThatArgumentGoesThroughInHelperForGuesses(Walker&, FunctionID callee, const BodyValue& call, ValueID argument);

static std::optional<Opcode> operatorThatArgumentGoesThroughInCalleeForGuesses(Walker& walker, const BodyValue& call, ValueID argument)
{
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    if (std::ranges::find(arguments, argument) == arguments.end())
        return std::nullopt;
    for (FunctionID helper : helpersThatCallMayReachForGuesses(walker, call)) {
        if (std::optional<Opcode> found = operatorThatArgumentGoesThroughInHelperForGuesses(walker, helper, call, argument))
            return found;
    }
    return std::nullopt;
}

static std::optional<Opcode> operatorThatArgumentGoesThroughInHelperForGuesses(Walker& walker, FunctionID callee, const BodyValue& call, ValueID argument)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return std::nullopt;
    const Function& function = walker.program().at(callee);
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    size_t position = std::ranges::find(arguments, argument) - arguments.begin();
    BodyBlock entry = body->block(entryBlock);
    if (position >= arguments.size() || position >= function.numberOfPositionalParameters() || function.firstParameterIndex() + position >= entry.numberOfParameters())
        return std::nullopt;
    return operatorThatValueGoesThroughForGuesses(walker, *body, entry.parameter(function.firstParameterIndex() + position), nullptr);
}

static std::optional<Opcode> operatorThatElementGoesThroughForGuesses(Walker& walker, const BodyValue& access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if ((access.opcode() == Opcode::ElemGet || access.opcode() == Opcode::ElemSet) && !keySpeaksForIndexForGuesses(index, access))
        return std::nullopt;
    auto isElementUnderIndex = [&](ValueID value) {
        return !index.isParameter(value) && index.opcodeOf(value) == Opcode::ElemGet && keySpeaksForIndexForGuesses(index, index.value(value));
    };
    if (access.opcode() == Opcode::ElemSet) {
        ValueID stored = index.valueBehindCasts(access.valueOperand());
        if (index.isParameter(stored))
            return std::nullopt;
        auto operatorThatMakesNumber = [&](const BodyIndex& text, ValueID made) -> std::optional<Opcode> {
            auto addsNumbers = [&](ValueID whole) {
                BitVector sumsAsked;
                Vector<ValueID, 16> sumsToAsk { whole };
                bool everySideIsNumber = true;
                while (everySideIsNumber && !sumsToAsk.isEmpty()) {
                    text.value(sumsToAsk.takeLast()).forEachOperand([&](ValueID operand, uint32_t) {
                        ValueID side = text.valueBehindCasts(operand);
                        if (!everySideIsNumber || (&text == &index && isElementUnderIndex(side)))
                            return;
                        Opcode makerOfSide = text.isParameter(side) ? Opcode::Call : text.opcodeOf(side);
                        if (makerOfSide == Opcode::Add) {
                            if (!sumsAsked.set(side.index()))
                                sumsToAsk.append(side);
                            return;
                        }
                        everySideIsNumber = operatorsThatMakeNumbers.contains(makerOfSide) || allBitwiseOperators.contains(makerOfSide) || makerOfSide == Opcode::Integer || makerOfSide == Opcode::Double;
                    });
                }
                return everySideIsNumber;
            };
            Opcode maker = text.opcodeOf(made);
            return operatorsThatMakeNumbers.contains(maker) || (maker == Opcode::Add && addsNumbers(made)) ? std::optional(maker) : std::nullopt;
        };
        if (index.opcodeOf(stored) != Opcode::Call)
            return operatorThatMakesNumber(index, stored);
        for (FunctionID helper : helpersThatCallMayReachForGuesses(walker, index.value(stored))) {
            const BodyIndex* body = walker.facts().bodies().find(helper);
            if (!body || body->hasLoops() || body->numberOfBlocks() != 1 || walker.program().at(helper).isCoroutine() || !copyOfCalleeCalledInLoopPays(walker, helper, index.value(stored).as<Opcode::Call>().argumentsOperands().size()))
                continue;
            BodyTerminator end = body->terminator(entryBlock);
            ValueID answer = end.kind() == TerminatorKind::Return && end.operand(0) ? body->valueBehindCasts(end.operand(0)) : ValueID();
            if (std::optional<Opcode> found = answer && !body->isParameter(answer) ? operatorThatMakesNumber(*body, answer) : std::nullopt)
                return found;
        }
        return std::nullopt;
    }
    if (access.opcode() != Opcode::ElemGet)
        return std::nullopt;
    Vector<uint16_t>& found = walker.operatorsFoundBehindValuesForGuesses();
    if (found.isEmpty())
        found.fill(0, index.numberOfValues());
    return operatorThatValueGoesThroughForGuesses(walker, index, access.id(), &found);
}

static ValueID parameterWhereValueMeetsWrittenNumberForGuesses(Walker& walker, const BodyIndex& index, ValueID value, BodyUser user)
{
    if (user.kind == BodyUseKind::Operand) {
        Opcode opcode = index.opcodeOf(user.value);
        bool asksWhetherValueIsThere = opcode == Opcode::ToBoolean || (opcode == Opcode::TypeTest && walker.types().isSubset(index.value(user.value).as<Opcode::TypeTest>().testedType(), TypeTable::nullish));
        std::optional<BodyUser> asker = asksWhetherValueIsThere ? index.onlyUser(user.value) : std::nullopt;
        if (!asker || asker->kind != BodyUseKind::TerminatorOperand)
            return { };
        ValueID named;
        bool isNamedOnce = true;
        index.forEachEdgeResult(asker->block, [&](const BodyEdgeResult& result) {
            if (result.subject != value || !result.parameter || index.isUnused(result.parameter))
                return;
            isNamedOnce &= !named;
            named = result.parameter;
        });
        std::optional<BodyUser> userOfName = named && isNamedOnce ? index.onlyUser(named) : std::nullopt;
        if (!userOfName)
            return { };
        value = named;
        user = *userOfName;
    }
    if (user.kind != BodyUseKind::EdgeArgument || !index.hasOneUse(value) || (index.terminator(user.block).kind() != TerminatorKind::Jump && !index.terminator(user.block).isConditionalBranch()))
        return { };
    BodyTarget handsOn;
    for (BodyTarget target : index.terminator(user.block).targets()) {
        if (std::ranges::find(target.arguments(), value) != target.arguments().end())
            handsOn = target;
    }
    if (!handsOn)
        return { };
    BlockID join = handsOn.block();
    size_t position = std::ranges::find(handsOn.arguments(), value) - handsOn.arguments().begin();
    if (index.numberOfIncomingEdges(join) != 2 || index.isLoopHead(join) || index.isEnteredByHandlerEdge(join) || position >= index.block(join).numberOfParameters())
        return { };
    BodyTerminator fork = index.terminator(index.immediateDominator(join));
    if (!fork.isConditionalBranch())
        return { };
    ValueID other;
    bool comesFromFork = false;
    for (BodyTarget way : fork.targets()) {
        if (way.block() == user.block || way.arguments().data() == handsOn.arguments().data()) {
            comesFromFork = true;
            continue;
        }
        BodyTarget intoJoin = way;
        if (way.block() != join) {
            if (index.terminator(way.block()).kind() != TerminatorKind::Jump)
                return { };
            intoJoin = *index.terminator(way.block()).targets().begin();
        }
        if (intoJoin.block() != join || position >= intoJoin.arguments().size())
            return { };
        other = index.valueBehindCasts(intoJoin.arguments()[position]);
    }
    if (!comesFromFork || !other || index.isParameter(other) || (index.opcodeOf(other) != Opcode::Integer && index.opcodeOf(other) != Opcode::Double))
        return { };
    return index.block(join).parameter(static_cast<uint32_t>(position));
}

static std::optional<Opcode> operatorThatValueGoesThroughForGuesses(Walker& walker, const BodyIndex& index, ValueID value, Vector<uint16_t>* found)
{
    auto isElementUnderIndex = [&](ValueID element) {
        return !index.isParameter(element) && index.opcodeOf(element) == Opcode::ElemGet && keySpeaksForIndexForGuesses(index, index.value(element));
    };
    Vector<ValueID, 8> passed;
    auto answer = [&](std::optional<Opcode> opcode) {
        for (ValueID behind : passed) {
            if (found)
                (*found)[behind.index()] |= opcode ? static_cast<uint16_t>(*opcode) + 2 : 1;
        }
        return opcode;
    };
    for (;;) {
        if (uint16_t known = found ? (*found)[value.index()] & operatorFoundBehindValueForGuesses : 0)
            return answer(known == 1 ? std::nullopt : std::optional(static_cast<Opcode>(known - 2)));
        passed.append(value);
        ValueID used = value;
        std::optional<BodyUser> firstUser = index.firstUser(value);
        if (ValueID met = firstUser ? parameterWhereValueMeetsWrittenNumberForGuesses(walker, index, value, *firstUser) : ValueID()) {
            value = met;
            continue;
        }
        value = firstUser && firstUser->kind == BodyUseKind::Operand ? firstUser->value : ValueID();
        if (!value)
            return answer(std::nullopt);
        Opcode opcode = index.opcodeOf(value);
        if (operatorsThatMakeNumbers.contains(opcode))
            return answer(opcode);
        if (allRelations.contains(opcode)) {
            bool meetsWrittenText = false;
            index.value(value).forEachOperand([&](ValueID operand, uint32_t) {
                ValueID other = index.valueBehindCasts(operand);
                meetsWrittenText |= !index.isParameter(other) && index.opcodeOf(other) == Opcode::String;
            });
            return answer(meetsWrittenText ? std::nullopt : std::optional(opcode));
        }
        if (opcode == Opcode::Call) {
            ValueID receiver = index.value(value).as<Opcode::Call>().thisValueOperand();
            bool isFunctionOfMath = receiver && used != index.value(value).calleeOperand() && !index.isParameter(receiver) && index.opcodeOf(receiver) == Opcode::Builtin && index.value(receiver).as<Opcode::Builtin>().builtin() == Builtins::Math;
            return answer(isFunctionOfMath ? std::optional(opcode) : found ? operatorThatArgumentGoesThroughInCalleeForGuesses(walker, index.value(value), used) : std::nullopt);
        }
        if (opcode != Opcode::Add && !opcodesThatStateTypes.contains(opcode))
            return answer(std::nullopt);
        if (opcode == Opcode::Add) {
            bool isAddedToNumber = false;
            index.value(value).forEachOperand([&](ValueID operand, uint32_t) {
                if (operand == used)
                    return;
                ValueID other = index.valueBehindCasts(operand);
                auto speaksForNumber = [&](ValueID added) {
                    return index.isParameter(added) || operatorsThatMakeNumbers.contains(index.opcodeOf(added)) || index.opcodeOf(added) == Opcode::Integer || index.opcodeOf(added) == Opcode::Double || isElementUnderIndex(added);
                };
                auto known = [&] {
                    return found ? (*found)[other.index()] & (sumSpeaksForNumberForGuesses | sumSpeaksForNoNumberForGuesses) : 0;
                };
                Vector<ValueID, 8> sums;
                while (!known() && !speaksForNumber(other) && index.opcodeOf(other) == Opcode::Add && speaksForNumber(index.valueBehindCasts(index.value(other).operand(1)))) {
                    sums.append(other);
                    other = index.valueBehindCasts(index.value(other).operand(0));
                }
                bool speaks = known() ? known() == sumSpeaksForNumberForGuesses : speaksForNumber(other);
                for (ValueID sum : sums) {
                    if (found)
                        (*found)[sum.index()] |= speaks ? sumSpeaksForNumberForGuesses : sumSpeaksForNoNumberForGuesses;
                }
                isAddedToNumber |= speaks;
            });
            if (isAddedToNumber)
                return answer(opcode);
        }
    }
}

bool turnsOfLoopCallForGuesses(const BodyIndex& index, LoopID loop)
{
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            if (!callsOfValuesForGuesses.contains(operation.opcode()))
                return false;
            ValueID callee = operation.opcode() == Opcode::Call ? operation.calleeOperand() : ValueID();
            return !callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin;
        });
    });
}

bool TextOfLoopForGuesses::turnsCall()
{
    if (!m_turnsCall)
        m_turnsCall = turnsOfLoopCallForGuesses(m_index, m_loop);
    return *m_turnsCall;
}

bool TextOfLoopForGuesses::turnsStoreIntoWhatMayBeList(Walker& walker)
{
    if (!m_turnsStoreIntoWhatMayBeList) {
        m_turnsStoreIntoWhatMayBeList = std::ranges::any_of(m_index.membersOf(m_loop), [&](BlockID member) {
            return std::ranges::any_of(m_index.block(member).values(), [&](BodyValue operation) {
                if (operation.opcode() != Opcode::ElemSet)
                    return false;
                ValueID holder = valueBehindCastsOfLoopForGuesses(m_index, operation.objectOperand(), m_firstValueOfLoop);
                return holder.index() >= m_firstValueOfLoop || !isViewOfNumbersReadInLine(walker, provenTypeOfInputForGuesses(walker, holder));
            });
        });
    }
    return *m_turnsStoreIntoWhatMayBeList;
}

bool TextOfLoopForGuesses::multipliesElementsOf(ValueID holder)
{
    static constexpr OpcodeSet multiplicativeOperators = operatorsOfKinds({ OperatorKind::Multiplicative });
    if (!std::exchange(m_isRead, true)) {
        auto noteHolderIn = [&](BitVector& holders, const BodyValue& access) {
            ValueID read = valueBehindCastsOfLoopForGuesses(m_index, access.objectOperand(), m_firstValueOfLoop);
            if (read)
                holders.set(read.index());
        };
        auto noteHolderOf = [&](const BodyValue& access) {
            noteHolderIn(m_giveFactors, access);
        };
        auto isMadeBy = [&](ValueID value, const OpcodeSet& opcodes) {
            return !m_index.isParameter(value) && opcodes.contains(m_index.opcodeOf(value));
        };
        auto noteElementsHeldAgainstElementsBy = [&](const BodyValue& comparison) {
            if (!allEqualities.contains(comparison.opcode()) && !allRelations.contains(comparison.opcode()))
                return;
            bool meetsElementsAlone = true;
            comparison.forEachOperand([&](ValueID operand, uint32_t) {
                ValueID element = m_index.valueBehindCasts(operand);
                meetsElementsAlone &= isMadeBy(element, OpcodeSet { Opcode::ElemGet }) && keySpeaksForIndexForGuesses(m_index, m_index.value(element));
            });
            if (!meetsElementsAlone)
                return;
            comparison.forEachOperand([&](ValueID operand, uint32_t) {
                noteHolderIn(m_areHeldAgainstElements, m_index.value(m_index.valueBehindCasts(operand)));
            });
        };
        for (BlockID member : m_index.membersOf(m_loop)) {
            for (BodyValue operation : m_index.block(member).values()) {
                if (operation.opcode() == Opcode::ElemSet && isMadeBy(m_index.valueBehindCasts(operation.valueOperand()), multiplicativeOperators))
                    noteHolderOf(operation);
                noteElementsHeldAgainstElementsBy(operation);
                if (!multiplicativeOperators.contains(operation.opcode()))
                    continue;
                ValueID onlyUser = m_index.onlyUsingValue(operation.id());
                if (onlyUser && allBitwiseOperators.contains(m_index.opcodeOf(onlyUser)))
                    continue;
                bool hasWholeFactor = false;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    hasWholeFactor |= operation.opcode() != Opcode::Div && isMadeBy(m_index.valueBehindCasts(operand), OpcodeSet { Opcode::Integer });
                });
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    ValueID factor = m_index.valueBehindCasts(operand);
                    if (isMadeBy(factor, OpcodeSet { Opcode::ElemGet }) && (!hasWholeFactor || (m_index.hasOneUse(operand) && m_index.hasOneUse(factor))))
                        noteHolderOf(m_index.value(factor));
                });
            }
        }
    }
    return m_giveFactors.get(holder.index());
}

bool TextOfLoopForGuesses::holdsElementsAgainstElements(ValueID holder)
{
    multipliesElementsOf(holder);
    return m_areHeldAgainstElements.get(holder.index());
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> listAndBytesWhereNothingIsExpectedForGuesses(Walker& walker, const BodyValue& access, ValueID operand, TextOfLoopForGuesses& text, ValueID holder, TypeID storedUnderInteger = { })
{
    const BodyIndex& index = walker.currentBodyIndex();
    bool goesThroughOperator = !!operatorThatElementGoesThroughForGuesses(walker, access);
    bool isHeldAgainstElement = !goesThroughOperator && access.opcode() == Opcode::ElemGet && keySpeaksForIndexForGuesses(index, access) && text.holdsElementsAgainstElements(holder) && !text.turnsStoreIntoWhatMayBeList(walker);
    goesThroughOperator |= isHeldAgainstElement;
    if (index.hasIrreducibleFlow() || (!goesThroughOperator && !storedUnderInteger))
        return { };
    ValueID named = index.valueBehindCasts(operand);
    bool nothingButLengthIsNamed = std::ranges::all_of(walker.namesUsedOff(named), [&](PropertyKeyID key) {
        return walker.program().nameClassOf(key) == NameClass::Length;
    });
    if (!nothingButLengthIsNamed)
        return { };
    TypeArena& types = walker.types();
    TypeID number = TypeTable::number;
    if (!goesThroughOperator) {
        Vector<TypeID, mostMembersToldApartInFrontOfLoop> parts = listBytesAndOtherViewsForGuesses(types, types.builtinClass(BuiltinClasses::Array), !types.at(storedUnderInteger).isInteger(), false);
        parts.removeAt(0);
        return parts;
    }
    Vector<TypeID, mostMembersToldApartInFrontOfLoop> parts = listBytesAndOtherViewsForGuesses(types, types.builtinClass(BuiltinClasses::Array, singleElementSpan(number)), text.multipliesElementsOf(holder), text.turnsCall());
    if (isHeldAgainstElement)
        parts.append(TypeID(TypeTable::string));
    return parts;
}

static bool listOfVariableIsExpectedByUseForGuesses(const Program& program, const BodyValue& read)
{
    if (read.opcode() == Opcode::GetModuleVariable)
        return program.at(read.variable()).declared.isExpected();
    const Layout& layout = program.at(read.layout());
    return read.field() < layout.fields.size() && layout.fields[read.field()].declared.isExpected();
}

static TypeID listWhereOtherHolderInLoopIsOneForGuesses(Walker& walker, LoopID loop, uint32_t firstValueOfLoop, HoldersInLoopThatAreListsForGuesses& found, const BodyValue& access, Reference value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    auto membersFor = [&](const BodyValue& read) {
        return listAndBytesWhereNothingIsExpectedForGuesses(walker, read, read.objectOperand(), found.text, valueBehindCastsOfLoopForGuesses(index, read.objectOperand(), firstValueOfLoop));
    };
    auto own = membersFor(access);
    if (own.isEmpty())
        return { };
    for (BlockID member : index.membersOf(loop)) {
        if (found.areFound || found.holders.size() == 2)
            break;
        for (BodyValue other : index.block(member).values()) {
            if (found.holders.size() == 2)
                break;
            if (other.opcode() != Opcode::ElemGet && other.opcode() != Opcode::ElemSet)
                continue;
            Reference holder = holderThatEntersLoopForGuesses(walker, index, other, firstValueOfLoop);
            if (!holder || !holder.isValue() || found.holders.contains(holder))
                continue;
            Type proven = types.at(walker.typeOf(holder));
            const ClassPart* provenClass = proven.isObject() ? singleBuiltinClassOf(types, proven) : nullptr;
            bool isList = provenClass && provenClass->builtinClass == BuiltinClasses::Array;
            bool isView = !isList && proven.isObject() && proven.hasOnlyClassObjects() && kindsOfViewsOfNumbersIn(walker.program(), types, walker.typeOf(holder));
            if ((isList || isView) && !membersFor(other).isEmpty())
                found.holders.append(holder);
        }
    }
    found.areFound = true;
    return found.holders.containsIf([&](Reference holder) { return holder != value; }) ? own[0] : TypeID();
}

static bool keyComesOfCounterAloneForGuesses(const BodyIndex& index, BlockID inputHead, ValueID key)
{
    for (unsigned depth = 0; depth < 4; ++depth) {
        key = index.valueBehindCasts(key);
        if (index.isParameter(key))
            return index.blockOf(key) == inputHead;
        BodyValue made = index.value(key);
        if (!operatorsThatMakeNumbers.contains(made.opcode()) && made.opcode() != Opcode::Add)
            return false;
        ValueID next;
        unsigned numberThatAreNoConstants = 0;
        made.forEachOperand([&](ValueID operand, uint32_t) {
            ValueID behind = index.valueBehindCasts(operand);
            if (!index.isParameter(behind) && (index.opcodeOf(behind) == Opcode::Integer || index.opcodeOf(behind) == Opcode::Double))
                return;
            next = operand;
            ++numberThatAreNoConstants;
        });
        if (numberThatAreNoConstants != 1)
            return false;
        key = next;
    }
    return false;
}

Vector<ValueID, 2> findOtherHoldersWalkedInStepThatMayBeBytesForGuesses(Walker& walker, BlockID inputHead, Reference holder)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    BlocksThatEveryTurnPasses passedByEveryTurn(index, loop);
    TextOfLoopForGuesses text(index, loop, firstValueOfLoop);
    Vector<ValueID, 2> found;
    Vector<ValueID, 2> storedInto;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue access : index.block(member).values()) {
            if (access.opcode() != Opcode::ElemSet)
                continue;
            if (ValueID holderOfStore = valueBehindCastsOfLoopForGuesses(index, access.objectOperand(), firstValueOfLoop); !storedInto.contains(holderOfStore))
                storedInto.append(holderOfStore);
        }
    }
    for (BlockID member : index.membersOf(loop)) {
        if (!passedByEveryTurn.contains(member))
            continue;
        for (BodyValue access : index.block(member).values()) {
            if (access.opcode() != Opcode::ElemGet)
                continue;
            ValueID inFront = valueBehindCastsOfLoopForGuesses(index, access.objectOperand(), firstValueOfLoop);
            if (!inFront || inFront.index() >= firstValueOfLoop || found.contains(inFront) || storedInto.contains(inFront))
                continue;
            Reference other = walker.referenceOf(inFront);
            if (!other || !other.isValue() || other == holder || !saysNothingForGuesses(walker.types(), walker.typeOf(other)))
                continue;
            if ((!operatorThatElementGoesThroughForGuesses(walker, access) && !(keySpeaksForIndexForGuesses(index, access) && text.holdsElementsAgainstElements(inFront) && !text.turnsStoreIntoWhatMayBeList(walker))) || !keyComesOfCounterAloneForGuesses(index, inputHead, access.propertyOperand()))
                continue;
            TypeID word = findExpectationForGuesses(walker, inFront).type;
            bool isListOfAnything = word && walker.types().isSubset(walker.types().withOnlyObjects(word), walker.types().builtinClass(BuiltinClasses::Array));
            if (word && !isListOfAnything && listAndBytesThatMayComeForGuesses(walker, inFront).isEmpty())
                continue;
            found.append(inFront);
        }
    }
    return found;
}

Vector<ValueID, 2> findOtherHoldersExpectedOfClassOfFirstForGuesses(Walker& walker, BlockID inputHead, Reference holder, TypeID tested)
{
    Vector<ValueID, 2> found = findOtherHoldersWalkedInStepThatMayBeBytesForGuesses(walker, inputHead, holder);
    if (tested != TypeID(TypeTable::string))
        return found;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue comparison : index.block(member).values()) {
            if (!allEqualities.contains(comparison.opcode()))
                continue;
            Vector<ValueID, 2> holders;
            bool meetsElementsAlone = true;
            comparison.forEachOperand([&](ValueID operand, uint32_t) {
                ValueID element = index.valueBehindCasts(operand);
                if (index.isParameter(element) || index.opcodeOf(element) != Opcode::ElemGet || !keySpeaksForIndexForGuesses(index, index.value(element))) {
                    meetsElementsAlone = false;
                    return;
                }
                holders.append(valueBehindCastsOfLoopForGuesses(index, index.value(element).objectOperand(), firstValueOfLoop));
            });
            if (!meetsElementsAlone || holders.size() != 2)
                continue;
            for (unsigned place = 0; place < 2; ++place) {
                ValueID first = holders[place];
                ValueID other = holders[1 - place];
                if (!first || !other || first.index() >= firstValueOfLoop || other.index() >= firstValueOfLoop || found.contains(other))
                    continue;
                Reference one = walker.referenceOf(first);
                if (!one || (one != holder && !(one.isValue() && walker.types().isSubset(walker.typeOf(one), TypeID(TypeTable::string)))))
                    continue;
                Reference built = walker.referenceOf(other);
                if (!built || !built.isValue() || built == holder || !saysNothingForGuesses(walker.types(), walker.typeOf(built)) || !listBesideTextInWrittenWordForGuesses(walker, other))
                    continue;
                found.append(other);
            }
        }
    }
    return found;
}

Vector<ValueID, 2> findTablesThatMayBeViewsForGuesses(Walker& walker, BlockID inputHead, Reference holder)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    TextOfLoopForGuesses text(index, loop, firstValueOfLoop);
    Vector<ValueID, 2> found;
    if (text.turnsCall())
        return found;
    Vector<ValueID, 2> storedInto;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue access : index.block(member).values()) {
            if (access.opcode() != Opcode::ElemSet)
                continue;
            if (ValueID holderOfStore = valueBehindCastsOfLoopForGuesses(index, access.objectOperand(), firstValueOfLoop); !storedInto.contains(holderOfStore))
                storedInto.append(holderOfStore);
        }
    }
    BlocksThatEveryTurnPasses passedByEveryTurn(index, loop);
    for (BlockID member : index.membersOf(loop)) {
        if (!passedByEveryTurn.contains(member))
            continue;
        for (BodyValue access : index.block(member).values()) {
            if (access.opcode() != Opcode::ElemGet)
                continue;
            ValueID inFront = valueBehindCastsOfLoopForGuesses(index, access.objectOperand(), firstValueOfLoop);
            if (!inFront || inFront.index() >= firstValueOfLoop || found.contains(inFront) || storedInto.contains(inFront))
                continue;
            Reference table = walker.referenceOf(inFront);
            if (!table || !table.isValue() || table == holder || !saysNothingForGuesses(walker.types(), walker.typeOf(table)))
                continue;
            if (!operatorThatElementGoesThroughForGuesses(walker, access) || keyComesOfCounterAloneForGuesses(index, inputHead, access.propertyOperand()))
                continue;
            if (findExpectationForGuesses(walker, inFront).type && listAndBytesThatMayComeForGuesses(walker, inFront).isEmpty())
                continue;
            found.append(inFront);
        }
    }
    return found;
}

TypeID viewsOfNumbersOfEveryKindForGuesses(TypeArena& types)
{
    return types.unionOf(types.builtinClass(BuiltinClasses::Uint8Array), listBytesAndOtherViewsForGuesses(types, types.builtinClass(BuiltinClasses::Array), false, false).last());
}

bool readFindsListThatTurnsReckonWithForGuesses(Walker& walker, ValueID read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(read);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    return user && index.opcodeOf(user) == Opcode::ElemGet && index.valueBehindCasts(index.value(user).objectOperand()) == read && !!operatorThatElementGoesThroughForGuesses(walker, index.value(user));
}

bool otherHolderOfLoopMayBeListForGuesses(Walker& walker, BlockID inputHead, Reference list)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    TextOfLoopForGuesses text(index, loop, firstValueOfLoop);
    BlocksThatEveryTurnPasses passedByEveryTurn(index, loop);
    for (BlockID member : index.membersOf(loop)) {
        bool isPassedByEveryTurn = passedByEveryTurn.contains(member);
        if (!isPassedByEveryTurn && index.loopOf(member) != loop)
            continue;
        for (BodyValue other : index.block(member).values()) {
            if (!isPassedByEveryTurn && (other.opcode() != Opcode::ElemGet || !loopReckonsWithElementRead(walker, other)))
                continue;
            if (other.opcode() == Opcode::Call && copyOfCalleeReadsUnderKeyWhatNobodyKnows(walker, other, firstValueOfLoop))
                return true;
            if (other.opcode() != Opcode::ElemGet && other.opcode() != Opcode::ElemSet)
                continue;
            ValueID inFront = valueBehindCastsOfLoopForGuesses(index, other.objectOperand(), firstValueOfLoop);
            Reference holder = holderThatEntersLoopForGuesses(walker, index, other, firstValueOfLoop);
            if (!holder || !holder.isValue() || holder == list || !saysNothingForGuesses(walker.types(), walker.typeOf(holder)) || findExpectationForGuesses(walker, other.objectOperand()).type)
                continue;
            if (!listAndBytesWhereNothingIsExpectedForGuesses(walker, other, other.objectOperand(), text, inFront).isEmpty())
                return true;
        }
    }
    return false;
}

static bool wordOfWhatNameFindsOnUnknownObjectIsDrawnFromUseForGuesses(Walker& walker, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, operand);
    if (!expectation.type || expectation.isDeclared || expectation.isHeld)
        return false;
    ValueID found = index.valueBehindCasts(operand);
    if (!found || index.isParameter(found) || index.opcodeOf(found) != Opcode::PropGet || index.value(found).isFreeName())
        return false;
    TypeID receiver = findExpectationForGuesses(walker, index.value(found).objectOperand()).type;
    TypeID objects = receiver ? types.withoutNullish(receiver) : TypeID();
    return !objects || isNeverType(objects) || !types.at(objects).layouts;
}

bool wordOfFieldThatFindsValueIsDrawnFromUseForGuesses(Walker& walker, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    ValueID found = index.valueBehindCasts(operand);
    if (!found || index.isParameter(found) || index.opcodeOf(found) != Opcode::PropGet || index.value(found).isFreeName())
        return false;
    BodyValue read = index.value(found);
    TypeID receiver = findExpectationForGuesses(walker, read.objectOperand()).type;
    TypeID objects = receiver ? types.withoutNullish(receiver) : TypeID();
    if (!objects || isNeverType(objects) || !types.at(objects).hasOnlyLayouts())
        return false;
    return std::ranges::all_of(membersOf(types, types.at(objects)), [&](LayoutID layout) {
        const Layout& holder = walker.program().at(layout);
        std::optional<uint32_t> field = holder.findField(read.key());
        return field && holder.fields[*field].declared.isExpected();
    });
}

static std::optional<SlotReadInLoopOffHolderInFront> findSlotThatFindsReceiverInLoopForGuesses(Walker& walker, const BodyValue& access, uint32_t firstValueOfLoop)
{
    ValueID receiver = access.opcode() == Opcode::Call ? access.as<Opcode::Call>().calleeOperand() : access.objectOperand();
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    auto isReadByNameInLoop = [&](ValueID value) {
        if (!value || value.index() < firstValueOfLoop || index.isParameter(value))
            return false;
        return index.opcodeOf(value) == Opcode::PrivateGet || (index.opcodeOf(value) == Opcode::PropGet && !index.value(value).isFreeName());
    };
    ValueID found = index.valueBehindCasts(receiver);
    if (!isReadByNameInLoop(found))
        return std::nullopt;
    Vector<ValueID, 2> reads { found };
    while (isReadByNameInLoop(index.valueBehindCasts(index.value(reads.last()).objectOperand())))
        reads.append(index.valueBehindCasts(index.value(reads.last()).objectOperand()));
    ValueID holder = index.value(reads.last()).objectOperand();
    bool holderIsReadOffVariableInLoop = holder.index() >= firstValueOfLoop;
    Reference name = holderIsReadOffVariableInLoop ? nameOfListThatEntersLoopAround(walker, index, index.value(reads.last())) : walker.referenceOf(holder);
    if (!name || !name.isValue())
        return std::nullopt;
    auto liesInOneSlotOfAll = [&](TypeID objects, PropertyKeyID key) {
        if (walker.plan().slots().someSlotUnderKeyHoldsMark(key))
            return false;
        FieldAccess access = walker.plan().fieldAccess(walker.program(), key, types.at(objects).layouts, membersOf(types, types.at(objects)), false);
        return access.hasLocation && !access.findsOffsetByShape && !access.mayBeFilledWithoutStorage && access.coverage == FieldAccess::Coverage::EveryMember && access.slot.hasStorage() && !access.slot.holdsAccessor;
    };

    SlotReadInLoopOffHolderInFront slot;
    slot.read = found;
    slot.holder = holder;
    slot.nameOfHolder = name;
    slot.holderIsReadOffVariableInLoop = holderIsReadOffVariableInLoop;
    TypeID foundInSlotAround;
    for (size_t place = reads.size(); place--;) {
        BodyValue read = index.value(reads[place]);
        bool isOutermost = place + 1 == reads.size();
        TypeID known = isOutermost ? walker.typeOf(name) : foundInSlotAround;
        bool isProven = known && types.at(known).isObject() && types.at(known).hasOnlyLayouts();
        TypeID objects = known;
        TypeID tested = known;
        LayoutSetID layouts = isProven ? types.at(known).layouts : LayoutSetID();
        TypeID whereFilled = !isProven && isOutermost && holderIsReadOffVariableInLoop && known ? types.withoutPrimitives(known, Primitive::Absent | Primitive::Undefined) : TypeID();
        bool isAskedWhetherItIsFilled = whereFilled && types.at(whereFilled).isObject() && types.at(whereFilled).hasOnlyLayouts();
        if (isAskedWhetherItIsFilled) {
            std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, name, whereFilled);
            if (!chosen || chosen->isPassedAlready || !chosen->layouts || chosen->tested != whereFilled)
                return std::nullopt;
            isProven = true;
            objects = whereFilled;
            tested = chosen->tested;
            layouts = chosen->layouts;
        }
        if (!isProven) {
            if (isOutermost && (holderIsReadOffVariableInLoop || !saysNothingForGuesses(types, known)))
                return std::nullopt;
            TypeID expected = findExpectationForGuesses(walker, isOutermost ? holder : reads[place + 1]).type;
            objects = expected ? types.withoutNullish(expected) : TypeID();
            if (!objects || isNeverType(objects) || !types.at(objects).hasOnlyLayouts() || !holdsOneMember(types.at(objects).layouts))
                return std::nullopt;
            std::optional<TestOfGuess> chosen = isOutermost ? chooseTestOfGuess(walker, name, objects) : chooseTestOfTypeForGuesses(walker, objects);
            if (!chosen || chosen->isPassedAlready)
                return std::nullopt;
            tested = chosen->tested;
            layouts = chosen->layouts;
        }
        PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
        std::optional<FieldReference> field = findField(walker.program(), membersOf(types, types.at(objects))[0], key);
        if (!field || !liesInOneSlotOfAll(objects, key))
            return std::nullopt;
        if (place) {
            slot.slotsThatFindHolder.append(SlotThatFindsHolderOfSlotReadInLoop { reads[place], tested, layouts, *field, isProven, isAskedWhetherItIsFilled });
            foundInSlotAround = isProven ? typeFoundInFilledSlot(walker, read, objects) : TypeID();
            continue;
        }
        if (isProven) {
            for (LayoutID layout : membersOf(types, types.at(objects))) {
                std::optional<FieldReference> own = findField(walker.program(), layout, key);
                if (!own || !saysNothingForGuesses(types, walker.program().at(own->layout).fields[own->field].declared.expected().predicted()))
                    return std::nullopt;
            }
        } else if (!wordOfFieldThatFindsValueIsDrawnFromUseForGuesses(walker, found))
            return std::nullopt;
        slot.testedOfHolder = tested;
        slot.layoutsOfHolder = layouts;
        slot.field = *field;
        slot.holderIsProven = isProven;
        slot.holderIsAskedWhetherItIsFilled = isAskedWhetherItIsFilled;
    }
    return slot;
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> listAndEveryViewWhereUseAloneSaysListForGuesses(Walker& walker, const BodyValue& access, ValueID operand, TextOfLoopForGuesses& text)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, operand);
    if (index.hasIrreducibleFlow() || !expectation.type || expectation.isDeclared || expectation.isHeld || text.turnsStoreIntoWhatMayBeList(walker))
        return { };
    TypeID list = types.withOnlyObjects(expectation.type);
    if (!isDeclaredToHoldNumbersAlone(walker.program(), types, list) || !operatorThatElementGoesThroughForGuesses(walker, access))
        return { };
    bool nothingButLengthIsNamed = std::ranges::all_of(walker.namesUsedOff(index.valueBehindCasts(operand)), [&](PropertyKeyID key) {
        return walker.program().nameClassOf(key) == NameClass::Length;
    });
    if (!nothingButLengthIsNamed)
        return { };
    TypeID otherViews = types.builtinClass(BuiltinClasses::Int8Array);
    for (BuiltinClassID builtinClass : { BuiltinClasses::Uint8ClampedArray, BuiltinClasses::Int16Array, BuiltinClasses::Uint16Array, BuiltinClasses::Int32Array, BuiltinClasses::Uint32Array, BuiltinClasses::Float32Array, BuiltinClasses::Float64Array })
        otherViews = types.unionOf(otherViews, types.builtinClass(builtinClass));
    if (text.turnsCall())
        return { list, types.builtinClass(BuiltinClasses::Buffer), types.builtinClass(BuiltinClasses::Uint8Array), otherViews };
    return { list, types.unionOf(types.builtinClass(BuiltinClasses::Buffer), types.builtinClass(BuiltinClasses::Uint8Array)), otherViews };
}

static bool wordOfEntryParameterIsListOfNoContentsDrawnFromUseForGuesses(Walker& walker, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    FunctionID current = walker.currentFunction();
    const Function& function = walker.program().at(current);
    const LinkTables& link = walker.facts().linked();
    if (function.has(FunctionFlags::IsChecked) || current.index() >= link.functions.size())
        return false;
    ValueID parameter = valueBehindCastsAndTestsForGuesses(index, operand);
    std::span<const LinkedParameter> linked = link.parametersOf(current);
    if (!index.isParameter(parameter) || index.blockOf(parameter) != entryBlock || parameter.index() >= linked.size() || function.entryParameterKind(parameter.index()) != EntryParameter::Parameter)
        return false;
    TypeID written = linked[parameter.index()].written;
    if (saysNothingForGuesses(types, written) || (types.at(written).primitives & ~(Primitive::AnyString | Primitive::UndefinedOrNull)))
        return false;
    return types.withOnlyObjects(written) == types.builtinClass(BuiltinClasses::Array);
}

static bool elementIsComparedOrStoredByKeyForGuesses(const BodyIndex& index, const BodyValue& read)
{
    ValueID value = read.id();
    for (;;) {
        ValueID used = value;
        value = index.onlyUsingValue(value);
        if (!value)
            return false;
        Opcode opcode = index.opcodeOf(value);
        if (allEqualities.contains(opcode))
            return true;
        if (opcode == Opcode::ElemSet)
            return index.value(value).valueOperand() == used;
        if (!opcodesThatStateTypes.contains(opcode))
            return false;
    }
}

static Vector<TypeID, mostMembersToldApartInFrontOfLoop> listWhereCounterRunsUpToLengthForGuesses(Walker& walker, BlockID inputHead, const BodyValue& access, ValueID operand, const TypesOfInputs& typesOfLook)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (access.opcode() != Opcode::ElemGet || index.hasIrreducibleFlow())
        return { };
    ValueID holder = index.valueBehindCasts(operand);
    ValueID counter = index.valueBehindCasts(access.propertyOperand());
    if (!index.isParameter(counter) || index.blockOf(counter) != inputHead)
        return { };
    LoopID loop = index.loopOf(inputHead);
    bool holdsNoOtherLoop = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
        return index.loopOf(member) == loop;
    });
    if (!holdsNoOtherLoop)
        return { };
    bool turnsCallBuiltinsAlone = std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue operation) {
            return !callsOfValuesForGuesses.contains(operation.opcode()) || wouldCallBuiltin(walker, operation, typesOfLook) != BuiltinCallAskedAbout::None;
        });
    });
    if (!turnsCallBuiltinsAlone)
        return { };
    std::span<const PropertyKeyID> names = walker.namesUsedOff(holder);
    bool lengthAloneIsNamed = !names.empty() && std::ranges::all_of(names, [&](PropertyKeyID key) {
        return walker.program().nameClassOf(key) == NameClass::Length;
    });
    if (!lengthAloneIsNamed)
        return { };
    bool counterIsHeldAgainstLength = false;
    index.terminator(inputHead).forEachOperand([&](ValueID condition, uint32_t) {
        ValueID relation = index.valueBehindCasts(condition);
        if (index.isParameter(relation) || !allRelations.contains(index.opcodeOf(relation)))
            return;
        bool takesCounter = false;
        bool takesLength = false;
        index.value(relation).forEachOperand([&](ValueID side, uint32_t) {
            ValueID compared = index.valueBehindCasts(side);
            takesCounter |= compared == counter;
            takesLength |= !index.isParameter(compared) && index.opcodeOf(compared) == Opcode::PropGet && index.valueBehindCasts(index.value(compared).objectOperand()) == holder;
        });
        counterIsHeldAgainstLength |= takesCounter && takesLength;
    });
    if (!counterIsHeldAgainstLength)
        return { };
    TypeArena& types = walker.types();
    TypeID list = types.builtinClass(BuiltinClasses::Array);
    if (!elementIsComparedOrStoredByKeyForGuesses(index, access))
        return { list };
    TypeID number = TypeTable::number;
    return { types.builtinClass(BuiltinClasses::Array, singleElementSpan(number)), list };
}

static ValueID readOfVariableOfFileInLoopForGuesses(const BodyIndex& index, ValueID operand, uint32_t firstValueOfLoop)
{
    ValueID read = valueBehindCastsOfLoopForGuesses(index, operand, firstValueOfLoop);
    if (read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = valueBehindCastsOfLoopForGuesses(index, index.value(read).operand(0), firstValueOfLoop);
    return read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable ? read : ValueID();
}

static VariableID variableOfFileReadInLoopForGuesses(const BodyIndex& index, ValueID operand, uint32_t firstValueOfLoop)
{
    ValueID read = readOfVariableOfFileInLoopForGuesses(index, operand, firstValueOfLoop);
    return read ? index.value(read).variable() : VariableID();
}

static bool readsSameRowForGuesses(const BodyIndex& index, ValueID value, const BodyValue& row, uint32_t firstValueOfLoop)
{
    if (!value || value.index() < firstValueOfLoop || index.isParameter(value) || index.opcodeOf(value) != Opcode::ElemGet)
        return false;
    BodyValue read = index.value(value);
    auto isSame = [&](ValueID one, ValueID other) {
        if (valueBehindCastsOfLoopForGuesses(index, one, firstValueOfLoop) == valueBehindCastsOfLoopForGuesses(index, other, firstValueOfLoop))
            return true;
        VariableID variable = variableOfFileReadInLoopForGuesses(index, one, firstValueOfLoop);
        return variable && variable == variableOfFileReadInLoopForGuesses(index, other, firstValueOfLoop);
    };
    return isSame(read.objectOperand(), row.objectOperand()) && isSame(read.propertyOperand(), row.propertyOperand());
}

static Reference valueOfVariableWhereWalkStandsForGuesses(Walker& walker, const BodyValue& lookup)
{
    if (Reference kept = valueOfStableBinding(walker, lookup))
        return kept;
    Reference held = walker.state().variable(lookup.variable());
    return held && held.isValue() && !(walker.types().at(walker.typeOf(held)).primitives & Primitive::Absent) ? held : Reference();
}

static Reference operandOfRowInLookForGuesses(Walker& walker, const BodyValue& read, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Walker::RowFoundUnderKey& row = walker.rowOfCopyFoundUnderKey();
    LoopID loop = row.variableOfHolder || row.variableOfKey ? index.loopOf(index.blockOf(read.id())) : LoopID();
    VariableID variable = loop ? variableOfFileReadInLoopForGuesses(index, operand, index.block(index.loop(loop).head).firstParameter().index()) : VariableID();
    if (variable && variable == row.variableOfHolder)
        return row.holder;
    if (variable && variable == row.variableOfKey)
        return row.key;
    return walker.referenceOf(operand);
}

static bool rowMayBeFoundUnderKeyForGuesses(Walker& walker, Reference holder, Reference key)
{
    return walker.has(Mechanism::Guesses) && walker.opensGuesses() && key.isValue() && walker.types().saysNothing(walker.typeOf(holder)) && walker.types().saysNothing(walker.typeOf(key));
}

ValueID findRowThatHeadReadsOffListInFront(Walker& walker, BlockID inputHead, const LoopPreview& preview)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    BodyBlock head = index.block(inputHead);
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = head.firstParameter().index();
    if (index.hasIrreducibleFlow() || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Folding))
        return { };
    auto nothingIsSeenOf = [&](ValueID read) {
        size_t place = read.index() - preview.firstValue;
        TypeID seen = read.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
        return !seen || saysNothingForGuesses(types, seen);
    };
    auto staysThroughLoop = [&](VariableID variable) {
        return !walker.plan().deferredDefinitions().findDeferred(variable) && std::ranges::none_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) { return operation.opcode() == Opcode::PutModuleVariable && operation.variable() == variable; });
        });
    };
    for (BodyValue row : head.values()) {
        if (row.opcode() == Opcode::LoopHint || row.opcode() == Opcode::GetModuleVariable || (row.opcode() == Opcode::CheckInitialized && variableOfFileReadInLoopForGuesses(index, row.id(), firstValueOfLoop)))
            continue;
        if (row.opcode() != Opcode::ElemGet)
            return { };
        VariableID variableOfHolder = variableOfFileReadInLoopForGuesses(index, row.objectOperand(), firstValueOfLoop);
        VariableID variableOfKey = variableOfFileReadInLoopForGuesses(index, row.propertyOperand(), firstValueOfLoop);
        if ((!variableOfHolder && row.objectOperand().index() >= firstValueOfLoop) || (!variableOfKey && row.propertyOperand().index() >= firstValueOfLoop))
            return { };
        if ((variableOfHolder && !staysThroughLoop(variableOfHolder)) || (variableOfKey && !staysThroughLoop(variableOfKey)))
            return { };
        auto whereWalkStands = [&](ValueID operand, VariableID variable) {
            return variable ? valueOfVariableWhereWalkStandsForGuesses(walker, index.value(readOfVariableOfFileInLoopForGuesses(index, operand, firstValueOfLoop))) : walker.referenceOf(operand);
        };
        Reference list = whereWalkStands(row.objectOperand(), variableOfHolder);
        Reference position = whereWalkStands(row.propertyOperand(), variableOfKey);
        bool variablesAreYetToBeRead = (variableOfHolder && !list) || (variableOfKey && !position);
        if (variablesAreYetToBeRead) {
            auto saysNothing = [&](ValueID operand, Reference known) {
                ValueID read = readOfVariableOfFileInLoopForGuesses(index, operand, firstValueOfLoop);
                TypeID type = known ? walker.typeOf(known) : read ? widestTypeOfOperation(walker, index.value(read)) : TypeID();
                return type && types.saysNothing(types.withoutPrimitives(type, Primitive::Absent));
            };
            if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !saysNothing(row.objectOperand(), list) || !saysNothing(row.propertyOperand(), position))
                return { };
        } else if (!list || !list.isValue() || !position || (!readTakesIndexOfProvenList(types, walker.typeOf(list), walker.typeOf(position)) && !rowMayBeFoundUnderKeyForGuesses(walker, list, position)))
            return { };
        bool takesIndexOfProvenList = !variablesAreYetToBeRead && readTakesIndexOfProvenList(types, walker.typeOf(list), walker.typeOf(position));
        static constexpr OpcodeSet storesThatEndWhatIsRememberedUnderKey { Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemDelete, Opcode::PropDefine, Opcode::PropDelete };
        auto isNumberWhereRowHoldsNumbers = [&](auto& self, ValueID input, unsigned depth) -> bool {
            ValueID value = valueBehindCastsOfLoopForGuesses(index, input, firstValueOfLoop);
            if (value.index() < firstValueOfLoop || index.isParameter(value)) {
                TypeID proven = value.index() < firstValueOfLoop ? provenTypeOfInputForGuesses(walker, value) : TypeID();
                return proven && types.at(proven).isNumber();
            }
            BodyValue maker = index.value(value);
            if (maker.opcode() == Opcode::ElemGet)
                return readsSameRowForGuesses(index, valueBehindCastsOfLoopForGuesses(index, maker.objectOperand(), firstValueOfLoop), row, firstValueOfLoop);
            if (maker.opcode() == Opcode::Integer || maker.opcode() == Opcode::Double)
                return true;
            static constexpr OpcodeSet makeNumbersOfNumbers { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
            return depth && makeNumbersOfNumbers.contains(maker.opcode()) && self(self, maker.operand(0), depth - 1) && self(self, maker.operand(1), depth - 1);
        };
        auto endsWhatIsRemembered = [&](BodyValue operation) {
            if (!storesThatEndWhatIsRememberedUnderKey.contains(operation.opcode()))
                return false;
            return operation.opcode() != Opcode::ElemSet || !readsSameRowForGuesses(index, valueBehindCastsOfLoopForGuesses(index, operation.objectOperand(), firstValueOfLoop), row, firstValueOfLoop) || !isNumberWhereRowHoldsNumbers(isNumberWhereRowHoldsNumbers, operation.valueOperand(), 3);
        };
        if (!takesIndexOfProvenList && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) { return std::ranges::any_of(index.block(member).values(), endsWhatIsRemembered); }))
            return { };
        TextOfLoopForGuesses text(index, loop, firstValueOfLoop);
        for (BlockID member : index.membersOf(loop)) {
            if (index.loopOf(member) != loop)
                continue;
            for (BodyValue operation : index.block(member).values()) {
                if (operation.opcode() != Opcode::ElemGet)
                    continue;
                ValueID holder = valueBehindCastsOfLoopForGuesses(index, operation.objectOperand(), firstValueOfLoop);
                if (!readsSameRowForGuesses(index, holder, row, firstValueOfLoop))
                    continue;
                if (!nothingIsSeenOf(holder))
                    continue;
                bool membersCome = findExpectationForGuesses(walker, operation.objectOperand()).type && takesIndexOfProvenList
                    ? !listAndEveryViewWhereUseAloneSaysListForGuesses(walker, operation, operation.objectOperand(), text).isEmpty()
                    : !listAndBytesWhereNothingIsExpectedForGuesses(walker, operation, operation.objectOperand(), text, holder).isEmpty();
                if (membersCome)
                    return row.id();
            }
        }
        return { };
    }
    return { };
}

static Reference askWhetherOwnValueUnderKeyIsFoundForGuesses(Walker& walker, Reference holder, Reference key, Reference& found, Origin origin)
{
    Draft lookup(Opcode::GetOwnValueUnderKeyOrAbsent, origin);
    lookup.setObjectOperand(holder);
    lookup.setPropertyOperand(key);
    lookup.setValueType(TypeTable::anythingOrAbsent);
    found = walker.build(WTF::move(lookup));
    if (!found || walker.hasFailed())
        return { };
    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(found);
    question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
    return walker.build(WTF::move(question));
}

static Reference nameOwnValueFoundUnderKeyForGuesses(Walker& walker, Reference found, Reference isAbsent, Origin origin)
{
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(found);
    proof.setTestOperand(isAbsent);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(false);
    return walker.build(WTF::move(proof));
}

bool readRowOfCopyAgainInTurn(Walker& walker, const BodyValue& read)
{
    constexpr auto turn = Walker::TurnOfTypedCopy::HasRunNoOtherCode;
    const Walker::RowFoundUnderKey& row = walker.rowOfCopyFoundUnderKey();
    if (!row.row || walker.referenceOf(read.objectOperand()) != row.holder || walker.referenceOf(read.propertyOperand()) != row.key || !walker.mayBeginTurnAgainInClosedCopy())
        return false;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    BlockID liesThereNoMore;
    auto goOnWhere = [&](Reference test, bool passesWithYes, bool mayBeAskedInFront) {
        if (mayBeAskedInFront && walker.branchInFrontOfTypedCopy(test, passesWithYes, turn))
            return true;
        if (!liesThereNoMore)
            liesThereNoMore = walker.addBlock(BlockFrequency::Rare);
        BlockID goesOn = walker.addBlock();
        walker.branch(test, passesWithYes ? goesOn : liesThereNoMore, passesWithYes ? liesThereNoMore : goesOn);
        walker.continueIn(goesOn);
        return false;
    };
    bool standsInFront = walker.standsInFrontOfTypedCopy(row.holder, turn) && walker.standsInFrontOfTypedCopy(row.key, turn) && walker.standsInFrontOfTypedCopy(row.row, turn) && walker.everyTurnOfTypedCopyFindsTheSameUnderEveryName();
    if (!standsInFront)
        return false;
    Reference found;
    Reference isAbsent;
    {
        Walker::InFrontOfTypedCopy inFront(walker, standsInFront, turn);
        isAbsent = askWhetherOwnValueUnderKeyIsFoundForGuesses(walker, row.holder, row.key, found, origin);
    }
    if (!isAbsent || walker.hasFailed())
        return true;
    walker.count(StatisticID::RowsFoundUnderKeysReadAgainInTurns);
    standsInFront = goOnWhere(isAbsent, false, standsInFront);
    Reference isSame;
    if (!walker.isUnreachable() && !walker.hasFailed()) {
        Walker::InFrontOfTypedCopy inFront(walker, standsInFront, turn);
        isSame = askWhetherValueIsListItselfForGuesses(walker, row.row, nameOwnValueFoundUnderKeyForGuesses(walker, found, isAbsent, origin), origin);
    }
    if (isSame && !walker.hasFailed())
        goOnWhere(isSame, true, standsInFront);
    if (walker.hasFailed())
        return true;
    if (liesThereNoMore) {
        BlockID goesOn = walker.currentBlock();
        bool goesOnIsReached = !walker.isUnreachable();
        walker.continueIn(liesThereNoMore);
        if (!walker.isUnreachable())
            walker.beginTurnAgainInClosedCopy();
        if (!goesOnIsReached)
            return true;
        walker.continueIn(goesOn);
    }
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    noteRowReadInFrontOfLoop(walker, read, row.row);
    walker.bind(read.id(), row.row);
    return true;
}

Walker::RowFoundUnderKey readVariablesOfRowInFrontOfLoop(Walker& walker, const BodyValue& read, BlockID whereRowIsNotFound, Origin origin)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t firstValueOfLoop = index.block(index.loop(index.loopOf(index.blockOf(read.id()))).head).firstParameter().index();
    origin.isTailCall = false;
    auto readInFront = [&](ValueID operand) -> Reference {
        ValueID written = readOfVariableOfFileInLoopForGuesses(index, operand, firstValueOfLoop);
        if (!written)
            return walker.referenceOf(operand);
        if (walker.isUnreachable() || walker.hasFailed())
            return { };
        if (Reference held = valueOfVariableWhereWalkStandsForGuesses(walker, index.value(written)))
            return held;
        VariableID variable = index.value(written).variable();
        Draft lookup(Opcode::GetModuleVariable, origin);
        lookup.setVariable(variable);
        Reference held = walker.build(WTF::move(lookup));
        if (!held || !held.isValue())
            return { };
        if (walker.types().at(walker.typeOf(held)).primitives & Primitive::Absent) {
            Draft question(Opcode::TypeTest, origin);
            question.setValueOperand(held);
            question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
            Reference isEmpty = walker.build(WTF::move(question));
            if (!isEmpty || !isEmpty.isValue())
                return { };
            BlockID filled = walker.addBlock();
            walker.branch(isEmpty, whereRowIsNotFound, filled);
            walker.continueIn(filled);
            if (walker.isUnreachable() || walker.hasFailed())
                return { };
            held = nameOwnValueFoundUnderKeyForGuesses(walker, held, isEmpty, origin);
            if (!held)
                return { };
        }
        if (walker.program().at(variable).initialization != VariableInitialization::Uninitialized || leavesDeadZoneForGood(walker.program(), variable))
            walker.state().noteEndOfDeadZone(variable);
        walker.state().noteVariable(variable, held);
        noteValueOfStableBinding(walker, variable, held);
        return held;
    };
    Walker::RowFoundUnderKey operands;
    operands.holder = readInFront(read.objectOperand());
    operands.key = readInFront(read.propertyOperand());
    operands.variableOfHolder = variableOfFileReadInLoopForGuesses(index, read.objectOperand(), firstValueOfLoop);
    operands.variableOfKey = variableOfFileReadInLoopForGuesses(index, read.propertyOperand(), firstValueOfLoop);
    return operands;
}

Reference readRowInFrontOfLoop(Walker& walker, const BodyValue& read, BlockID whereRowIsNotFound, Origin origin)
{
    Reference list = operandOfRowInLookForGuesses(walker, read, read.objectOperand());
    Reference position = operandOfRowInLookForGuesses(walker, read, read.propertyOperand());
    if (!readTakesIndexOfProvenList(walker.types(), walker.typeOf(list), walker.typeOf(position))) {
        origin.isTailCall = false;
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(position);
        question.setTestedType(TypeTable::string);
        Reference isText = walker.build(WTF::move(question));
        if (!isText || walker.hasFailed())
            return { };
        BlockID whereKeyIsText = walker.addBlock();
        walker.branch(isText, whereKeyIsText, whereRowIsNotFound);
        walker.continueIn(whereKeyIsText);
        if (walker.isUnreachable() || walker.hasFailed())
            return { };
        Reference found;
        Reference isAbsent = askWhetherOwnValueUnderKeyIsFoundForGuesses(walker, list, position, found, origin);
        if (!isAbsent || walker.hasFailed())
            return { };
        BlockID whereRowIsFound = walker.addBlock();
        walker.branch(isAbsent, whereRowIsNotFound, whereRowIsFound);
        walker.continueIn(whereRowIsFound);
        if (walker.isUnreachable() || walker.hasFailed())
            return { };
        walker.count(StatisticID::RowsFoundUnderKeysInFrontOfLoops);
        return nameOwnValueFoundUnderKeyForGuesses(walker, found, isAbsent, origin);
    }
    std::optional<PassedTest> inside = walker.findWhatHoldsPositionInsideList(list, position);
    Draft hasKind(Opcode::HasArrayStorage, origin);
    walker.chooseConservativeFields(hasKind);
    hasKind.setArrayOperand(list);
    hasKind.setArrayStorage(ArrayStorage::Contiguous);
    Reference hasStorage = walker.build(WTF::move(hasKind));
    BlockID passed = walker.addBlock();
    walker.branch(hasStorage, passed, whereRowIsNotFound);
    walker.continueIn(passed);
    Draft isPresent(Opcode::IsElementPresent, origin);
    walker.chooseConservativeFields(isPresent);
    isPresent.setArrayOperand(list);
    isPresent.setPositionOperand(position);
    isPresent.setArrayStorage(ArrayStorage::Contiguous);
    isPresent.setKeyIsIntegral(true);
    if (inside)
        isPresent.setIsInBounds(*inside);
    Reference test = walker.build(WTF::move(isPresent));
    BlockID found = walker.addBlock();
    walker.branchOnTest(test, found, whereRowIsNotFound);
    walker.continueIn(found);
    std::optional<PassedTest> present = walker.showElementPresentBehindTest(list, position, walker.findPassedTest(test));
    if (!present || walker.isUnreachable() || walker.hasFailed())
        return { };
    Draft element(Opcode::GetArrayElement, origin);
    walker.chooseConservativeFields(element);
    element.setArrayOperand(list);
    element.setPositionOperand(position);
    element.setArrayStorage(ArrayStorage::Contiguous);
    element.setLevel(KnowledgeLevel::Proven);
    element.setKeyIsIntegral(true);
    element.setIsPresent(*present);
    return walker.build(WTF::move(element));
}

void noteRowReadInFrontOfLoop(Walker& walker, const BodyValue& read, Reference row)
{
    Reference holder = operandOfRowInLookForGuesses(walker, read, read.objectOperand());
    Reference key = operandOfRowInLookForGuesses(walker, read, read.propertyOperand());
    bool isReadOffList = readTakesIndexOfProvenList(walker.types(), walker.typeOf(holder), walker.typeOf(key));
    noteKnown(walker, KnownNote::NumberReadOutOfList, holder, key, 0, isReadOffList ? LocationMasks(heapsUnderNumberReadOutOfListForGuesses) : declaredBoundInTable(Opcode::GetOwnValueUnderKeyOrAbsent).reads, row.bits());
}

Reference rowRememberedOfReadBeingWalked(Walker& walker, const BodyValue& read)
{
    if (walker.state().answers().isEmpty())
        return { };
    auto nameOf = [&](ValueID operand) -> Reference {
        if (walker.namesOperandsOfRowsByWhatTheirVariablesHoldNow()) [[unlikely]] {
            const BodyIndex& index = walker.currentBodyIndex();
            LoopID loop = index.loopOf(index.blockOf(read.id()));
            if (ValueID lookup = loop ? readOfVariableOfFileInLoopForGuesses(index, operand, index.block(index.loop(loop).head).firstParameter().index()) : ValueID())
                return valueOfVariableWhereWalkStandsForGuesses(walker, index.value(lookup));
        }
        return walker.referenceOf(operand);
    };
    Reference holder = nameOf(read.objectOperand());
    Reference key = nameOf(read.propertyOperand());
    if (!holder || !holder.isValue() || !key || !key.isValue() || readTakesIndexOfProvenList(walker.types(), walker.typeOf(holder), walker.typeOf(key)))
        return { };
    std::optional<uint64_t> row = findKnown(walker, KnownNote::NumberReadOutOfList, holder, key);
    return row ? Reference::fromBits(static_cast<uint32_t>(*row)) : Reference();
}

bool holdRowRememberedAgainstReadInTurn(Walker& walker, const BodyValue& read, Reference row)
{
    const Walker::RowFoundUnderKey& ofCopy = walker.rowOfCopyFoundUnderKey();
    if (!walker.checksProofs() || !ofCopy.row || row != ofCopy.row)
        return false;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Draft lookup(Opcode::GetOwnValueUnderKeyOrAbsent, origin);
    lookup.setObjectOperand(walker.referenceOf(read.objectOperand()));
    lookup.setPropertyOperand(walker.referenceOf(read.propertyOperand()));
    lookup.setValueType(TypeTable::anythingOrAbsent);
    Reference found = walker.buildTestOfCheck(WTF::move(lookup));
    Reference isSame = found ? askWhetherValueIsListItselfForGuesses(walker, row, found, origin) : Reference();
    if (!isSame || !isSame.isValue() || walker.isUnreachable() || walker.hasFailed())
        return false;
    walker.buildCheckOfProof(isSame, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
    return true;
}

Reference rowFoundUnderKeyInFrontOfLoop(Walker& walker, const BodyValue& read)
{
    if (walker.state().answers().isEmpty())
        return { };
    Reference holder = operandOfRowInLookForGuesses(walker, read, read.objectOperand());
    Reference key = operandOfRowInLookForGuesses(walker, read, read.propertyOperand());
    if (!holder || !holder.isValue() || !key || !key.isValue() || readTakesIndexOfProvenList(walker.types(), walker.typeOf(holder), walker.typeOf(key)))
        return { };
    std::optional<uint64_t> row = findKnown(walker, KnownNote::NumberReadOutOfList, holder, key);
    return row ? Reference::fromBits(static_cast<uint32_t>(*row)) : Reference();
}

Vector<GuessInFrontOfLoop, 4> findMembersToldApartInFrontOfLoop(Walker& walker, BlockID inputHead, const LoopPreview& preview)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    ValueID rowOfHead;
    for (BodyValue operation : index.block(inputHead).values()) {
        if (operation.opcode() == Opcode::ElemGet && walker.isMadeInFrontOfLoop(operation.id()))
            rowOfHead = operation.id();
        if (operation.opcode() != Opcode::LoopHint && operation.opcode() != Opcode::GetModuleVariable && operation.opcode() != Opcode::CheckInitialized)
            break;
    }

    auto typeInLook = [&](ValueID input) -> TypeID {
        if (input.index() < firstValueOfLoop)
            return provenTypeOfInputForGuesses(walker, input);
        size_t place = input.index() - preview.firstValue;
        TypeID type = input.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
        return type ? type : TypeID(TypeTable::anything);
    };
    TypesOfInputs typesOfLook(typeInLook);

    Vector<GuessInFrontOfLoop, 4> members;
    TextOfLoopForGuesses text(index, loop, firstValueOfLoop);
    auto turnsCallWhatNobodyKnows = [&] {
        return text.turnsCall() && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue call) {
                if (!callsOfValuesForGuesses.contains(call.opcode()))
                    return false;
                ValueID callee = call.opcode() == Opcode::Call ? call.calleeOperand() : ValueID();
                if (callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::Builtin)
                    return false;
                return !callee || !findOnlyCallee(walker, call, typeInLook(callee));
            });
        });
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!accessesToldApartInFrontOfLoop.contains(operation.opcode()))
                continue;
            ValueID operand = operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().calleeOperand() : operation.objectOperand();
            ValueID inFront = operand ? valueBehindCastsOfLoopForGuesses(index, operand, firstValueOfLoop) : ValueID();
            if (!inFront)
                continue;
            ValueID readOfVariable = inFront.index() >= firstValueOfLoop && (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet) && index.loopOf(member) == loop && types.at(typeInLook(index.valueBehindCasts(operation.propertyOperand()))).isNumber() && (operatorThatElementGoesThroughForGuesses(walker, operation) || (keySpeaksForIndexForGuesses(index, operation) && (text.comparesElementsWithCharacters(walker.program(), valueBehindCastsAndTestsForGuesses(index, operation.objectOperand())) || text.comparesElementsWithNumbers(walker.program(), valueBehindCastsAndTestsForGuesses(index, operation.objectOperand()))))) ? readOfVariableThatFindsReceiverInLoop(index, operation, firstValueOfLoop) : ValueID();
            std::optional<SlotReadInLoopOffHolderInFront> readOfSlot = !readOfVariable && inFront.index() >= firstValueOfLoop && operation.opcode() == Opcode::ElemGet && index.loopOf(member) == loop && types.at(typeInLook(index.valueBehindCasts(operation.propertyOperand()))).isNumber() && keySpeaksForIndexForGuesses(index, operation) ? findSlotThatFindsReceiverInLoopForGuesses(walker, operation, firstValueOfLoop) : std::nullopt;
            bool calleeIsReadOutOfSlotInLoop = false;
            if (!readOfVariable && !readOfSlot && inFront.index() >= firstValueOfLoop && operation.opcode() == Opcode::Call && index.loopOf(member) == loop) {
                if (std::optional<SlotReadInLoopOffHolderInFront> slot = findSlotThatFindsReceiverInLoopForGuesses(walker, operation, firstValueOfLoop); slot && findExpectationForGuesses(walker, operand).isHeld) {
                    readOfSlot = WTF::move(slot);
                    calleeIsReadOutOfSlotInLoop = true;
                }
            }
            if (!calleeIsReadOutOfSlotInLoop && readOfSlot && readOfSlot->holderIsProven && !operatorThatElementGoesThroughForGuesses(walker, operation) && !text.comparesElementsWithNumbers(walker.program(), valueBehindCastsAndTestsForGuesses(index, operation.objectOperand())))
                readOfSlot = std::nullopt;
            bool readsOffRow = !readOfVariable && !readOfSlot && rowOfHead && operation.opcode() == Opcode::ElemGet && index.loopOf(member) == loop && readsSameRowForGuesses(index, inFront, index.value(rowOfHead), firstValueOfLoop);
            if (inFront.index() >= firstValueOfLoop && !readOfVariable && !readOfSlot && !readsOffRow)
                continue;
            if ((operation.opcode() == Opcode::PropGet || operation.opcode() == Opcode::PropSet) && operation.isFreeName())
                continue;
            Reference value = readOfVariable ? nameOfListThatEntersLoopAround(walker, index, operation) : readsOffRow ? walker.nameOfOperatorMadeInFrontOfLoop(rowOfHead) : readOfSlot ? readOfSlot->nameOfHolder : walker.referenceOf(inFront);
            if (!value || !value.isValue() || (!(readOfSlot && readOfSlot->holderIsProven) && typedFormForGuesses(walker, operation, typesOfLook) != TypedForm::None))
                continue;
            TypeID proven = readOfSlot ? TypeID(TypeTable::anything) : walker.typeOf(value);
            TypeID expected = saysNothingForGuesses(types, proven) ? findExpectationForGuesses(walker, operand).type : proven;
            if (readOfVariable && saysNothingForGuesses(types, proven) && findExpectationForGuesses(walker, operand).isHeld)
                expected = { };
            if (readOfSlot && readOfSlot->holderIsProven && !calleeIsReadOutOfSlotInLoop)
                expected = { };
            if (readsOffRow && !readTakesIndexOfProvenList(types, walker.typeOf(operandOfRowInLookForGuesses(walker, index.value(rowOfHead), index.value(rowOfHead).objectOperand())), walker.typeOf(operandOfRowInLookForGuesses(walker, index.value(rowOfHead), index.value(rowOfHead).propertyOperand()))))
                expected = { };
            if (!expected && !saysNothingForGuesses(types, proven))
                continue;
            Vector<TypeID, mostMembersToldApartInFrontOfLoop> parts = expected ? membersOfSetForGuesses(walker, expected) : listAndBytesWhereNothingIsExpectedForGuesses(walker, operation, operand, text, inFront, operation.opcode() == Opcode::ElemSet && types.at(typeInLook(operation.propertyOperand())).isInteger() && !turnsCallWhatNobodyKnows() ? typeInLook(operation.valueOperand()) : TypeID());
            if (parts.isEmpty() && expected && saysNothingForGuesses(types, proven) && types.withOnlyObjects(expected) == types.builtinClass(BuiltinClasses::TypedArray)) {
                bool turnsCall = text.turnsCall();
                parts = listBytesAndOtherViewsForGuesses(types, types.builtinClass(BuiltinClasses::Array), text.multipliesElementsOf(inFront), turnsCall);
                parts.removeAt(0, turnsCall ? 1 : 2);
            }
            if (parts.isEmpty() && operation.opcode() != Opcode::Call && saysNothingForGuesses(types, proven))
                parts = listAndBytesThatMayComeForGuesses(walker, operand, &text, inFront);
            auto isJoinedToText = [&] {
                ValueID user = operation.opcode() == Opcode::ElemGet ? index.onlyUsingValue(operation.id()) : ValueID();
                while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
                    user = index.onlyUsingValue(user);
                if (!user || index.opcodeOf(user) != Opcode::Add)
                    return false;
                bool meetsText = false;
                index.value(user).forEachOperand([&](ValueID other, uint32_t) {
                    Type type = types.at(typeInLook(other));
                    meetsText |= type.primitives && !(type.primitives & ~Primitive::AnyString) && !type.hasObjects();
                });
                return meetsText;
            };
            if (parts.isEmpty() && !readOfVariable && expected && saysNothingForGuesses(types, proven) && wordOfEntryParameterIsListOfNoContentsDrawnFromUseForGuesses(walker, operand) && !isJoinedToText())
                parts = listAndBytesWhereNothingIsExpectedForGuesses(walker, operation, operand, text, inFront);
            if (parts.isEmpty() && !readOfVariable && expected && saysNothingForGuesses(types, proven) && !(types.at(expected).primitives & ~(Primitive::AnyString | Primitive::UndefinedOrNull)) && types.withOnlyObjects(expected) == types.builtinClass(BuiltinClasses::Array) && wordOfFieldThatFindsValueIsDrawnFromUseForGuesses(walker, operand) && !isJoinedToText())
                parts = listAndBytesWhereNothingIsExpectedForGuesses(walker, operation, operand, text, inFront);
            if (parts.isEmpty() && operation.opcode() != Opcode::Call && saysNothingForGuesses(types, proven))
                parts = textAndListThatMayComeForGuesses(walker, operand, text);
            if (parts.size() < 2 && operation.opcode() == Opcode::ElemGet && saysNothingForGuesses(types, proven))
                parts = textAndListWhoseElementsMeetCharactersForGuesses(walker, operand, text);
            if (readOfVariable && parts.size() < 2 && expected && saysNothingForGuesses(types, proven) && !findExpectationForGuesses(walker, operand).isHeld && listOfVariableIsExpectedByUseForGuesses(walker.program(), index.value(readOfVariable)) && isDeclaredToHoldNumbersAlone(walker.program(), types, types.withOnlyObjects(expected)) && operatorThatElementGoesThroughForGuesses(walker, operation))
                parts = listBytesAndOtherViewsForGuesses(types, types.withOnlyObjects(expected), text.multipliesElementsOf(inFront), text.turnsCall());
            if (!readOfVariable && parts.size() < 2 && expected && saysNothingForGuesses(types, proven) && operation.opcode() == Opcode::ElemSet && types.at(typeInLook(operation.propertyOperand())).isInteger() && !turnsCallWhatNobodyKnows()
                && wordOfWhatNameFindsOnUnknownObjectIsDrawnFromUseForGuesses(walker, operand) && types.isSubset(types.withOnlyObjects(expected), types.builtinClass(BuiltinClasses::Array))) {
                parts = listBytesAndOtherViewsForGuesses(types, types.withOnlyObjects(expected), !types.at(typeInLook(operation.valueOperand())).isInteger(), false);
                parts.removeAt(0);
            }
            if (!readOfVariable && parts.size() < 2 && expected && saysNothingForGuesses(types, proven) && !findExpectationForGuesses(walker, operand).isHeld && wordOfFieldThatFindsValueIsDrawnFromUseForGuesses(walker, operand) && isDeclaredToHoldNumbersAlone(walker.program(), types, types.withOnlyObjects(expected)) && operation.opcode() == Opcode::ElemGet && keySpeaksForIndexForGuesses(index, operation))
                parts = listBytesAndOtherViewsForGuesses(types, types.withOnlyObjects(expected), text.multipliesElementsOf(inFront), text.turnsCall());
            if (!readOfVariable && parts.isEmpty() && operation.opcode() != Opcode::Call && saysNothingForGuesses(types, proven))
                parts = listAndEveryViewWhereUseAloneSaysListForGuesses(walker, operation, operand, text);
            if (!readOfVariable && parts.isEmpty() && !expected && saysNothingForGuesses(types, proven))
                parts = listWhereCounterRunsUpToLengthForGuesses(walker, inputHead, operation, operand, typesOfLook);
            if (readOfVariable && parts.size() < 2 && expected && saysNothingForGuesses(types, proven) && !findExpectationForGuesses(walker, operand).isHeld && listOfVariableIsExpectedByUseForGuesses(walker.program(), index.value(readOfVariable)) && isViewOfNumbersReadInLine(walker, types.withOnlyObjects(expected))) {
                TypeID number = TypeTable::number;
                parts = { types.withOnlyObjects(expected), types.builtinClass(BuiltinClasses::Array, singleElementSpan(number)) };
            }
            if (parts.size() < 2 && operation.opcode() == Opcode::ElemGet && saysNothingForGuesses(types, proven) && (!expected || (types.withOnlyObjects(expected) == types.builtinClass(BuiltinClasses::Array) && !findExpectationForGuesses(walker, operand).isHeld))) {
                if (auto members = listAndViewsWhoseElementsMeetNumbersForGuesses(walker, operand, text); !members.isEmpty())
                    parts = WTF::move(members);
            }
            if (size_t joint = readOfVariable || readOfSlot || readsOffRow ? parts.findIf([&](TypeID part) { return !!classOfViewBesideInstancesBelowIt(walker, part); }) : notFound; joint != notFound) {
                parts[joint] = types.builtinClass(BuiltinClasses::Buffer);
                parts.insert(joint + 1, types.builtinClass(BuiltinClasses::Uint8Array));
            }
            bool textStandsAloneInFrontOfRest = false;
            if (operation.opcode() == Opcode::ElemGet && !readOfVariable && !readOfSlot && !readsOffRow && saysNothingForGuesses(types, proven) && !parts.contains(TypeID(TypeTable::string)) && text.holdsElementsAgainstElements(inFront)) {
                if (TypeID list = index.loopOf(member) == loop || index.isParameter(valueBehindCastsAndTestsForGuesses(index, operand)) ? listBesideTextInWrittenWordForGuesses(walker, operand) : TypeID()) {
                    textStandsAloneInFrontOfRest = parts.isEmpty() || (parts.size() == 1 && types.isSubset(parts[0], list) && !isDeclaredToHoldNumbersAlone(walker.program(), types, parts[0]));
                    if (textStandsAloneInFrontOfRest)
                        parts.shrink(0);
                    parts.insert(0, TypeID(TypeTable::string));
                    walker.count(StatisticID::TextsOrListsAskedForWhereElementsAreHeldAgainstElements);
                }
            }
            if (calleeIsReadOutOfSlotInLoop && std::ranges::any_of(parts, [&](TypeID part) { return !copyOfCalleeCalledInLoopPays(walker, singleFunctionOf(types, types.at(part)), operation.as<Opcode::Call>().argumentsOperands().size()); }))
                parts.shrink(0);
            size_t numberOfMembersBefore = members.size();
            for (TypeID part : parts) {
                if (operation.opcode() == Opcode::Call && !copyOfCalleeCalledInLoopPays(walker, singleFunctionOf(types, types.at(part)), operation.as<Opcode::Call>().argumentsOperands().size()))
                    continue;
                std::optional<TestOfGuess> chosen = readOfSlot ? chooseTestOfTypeForGuesses(walker, part) : chooseTestOfGuess(walker, value, part);
                TypeID typeBehindTest = chosen ? types.intersectionOf(proven, chosen->tested) : TypeID();
                if (BuiltinClassID above = chosen ? classOfViewBesideInstancesBelowIt(walker, chosen->tested) : BuiltinClassID())
                    typeBehindTest = types.builtinClass(above);
                auto typeWithProof = [&](ValueID input) -> TypeID {
                    ValueID behindCasts = valueBehindCastsOfLoopForGuesses(index, input, firstValueOfLoop);
                    if ((readOfVariable || readOfSlot) && behindCasts == inFront)
                        return typeBehindTest;
                    if (readsOffRow && readsSameRowForGuesses(index, behindCasts, index.value(rowOfHead), firstValueOfLoop))
                        return typeBehindTest;
                    return behindCasts.index() < firstValueOfLoop && walker.referenceOf(behindCasts) == value ? typeBehindTest : typeInLook(input);
                };
                TypesOfInputs typesWithProof(typeWithProof);
                if (chosen && typedFormForGuesses(walker, operation, typesWithProof) != TypedForm::None)
                    members.append(GuessInFrontOfLoop { value, chosen->tested, chosen->layouts, { }, !expected && !(readOfSlot && readOfSlot->holderIsProven), readOfVariable, readOfSlot, readsOffRow ? rowOfHead : ValueID(), textStandsAloneInFrontOfRest });
            }
            if (calleeIsReadOutOfSlotInLoop && members.size() - numberOfMembersBefore != parts.size())
                members.shrink(numberOfMembersBefore);
            if (members.size() >= 2 || (members.size() == 1 && (!expected || textStandsAloneInFrontOfRest)))
                break;
            members.shrink(0);
        }
        if (!members.isEmpty())
            break;
    }
    if (members.isEmpty() || members[0].slotReadInLoop)
        return members;

    Vector<ValueID, 2> names;
    auto mention = [&](ValueID operand) {
        if (operand && operand.index() < firstValueOfLoop && !names.contains(operand) && walker.referenceOf(operand) == members[0].value)
            names.append(operand);
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mention(operand);
            });
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, uint32_t) {
            mention(operand);
        });
        for (BodyTarget target : end.targets()) {
            for (ValueID argument : target.arguments())
                mention(argument);
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                mention(argument);
        }
    }
    for (GuessInFrontOfLoop& member : members)
        member.names = names;
    return members;
}

static std::optional<FieldReference> fieldReadBehindShapeForGuesses(Walker& walker, TypeID tested, const BodyValue& read)
{
    TypeArena& types = walker.types();
    if (!types.at(tested).hasOnlyLayouts() || !holdsOneMember(types.at(tested).layouts))
        return std::nullopt;
    std::optional<FieldReference> field = findField(walker.program(), membersOf(types, types.at(tested))[0], keyOfAccessToSlot(walker.program(), read));
    if (!field)
        return std::nullopt;
    FieldFacts facts = walker.facts().field(*field);
    const PlacedSlot& slot = walker.plan().slot(field->layout, field->field);
    if (!slot.hasStorage() || slot.holdsAccessor || slot.deferredFunction || slot.representation == SlotRepresentation::DeferredClosure)
        return std::nullopt;
    if (fieldAt(walker.program(), *field).form.contains(FieldForm::IsAccessor) || facts.formOfValue.contains(FieldForm::IsAccessor) || facts.facts.contains(FieldFact::IsAccessor))
        return std::nullopt;
    return field;
}

std::optional<GuessInFrontOfLoop> guessAtShapeOfHolderReadByName(Walker& walker, const BodyValue& read)
{
    Reference holder = walker.referenceOf(read.objectOperand());
    TypeID expected = holder && holder.isValue() ? findExpectation(walker, read.objectOperand()).type : TypeID();
    std::optional<TestOfGuess> test = expected ? chooseTestOfGuess(walker, holder, walker.types().withoutNullish(expected)) : std::nullopt;
    if (!test || !test->layouts || test->isPassedAlready || !fieldReadBehindShapeForGuesses(walker, test->tested, read))
        return std::nullopt;
    return GuessInFrontOfLoop { holder, test->tested, test->layouts, { } };
}

Reference buildTestOfSlotFilledBehindShape(Walker& walker, const GuessInFrontOfLoop& guess, Reference shaped, const BodyValue& read)
{
    Draft isFilled(Opcode::IsSlotFilled, walker.originOf(read));
    walker.chooseConservativeFields(isFilled);
    isFilled.setObjectOperand(shaped);
    isFilled.setKey(keyOfAccessToSlot(walker.program(), read));
    isFilled.setReceivers(guess.layouts);
    isFilled.setLevel(KnowledgeLevel::Proven);
    return walker.build(WTF::move(isFilled));
}

void readSlotFilledBehindShape(Walker& walker, const GuessInFrontOfLoop& guess, Reference shaped, const BodyValue& read)
{
    std::optional<FieldReference> field = fieldReadBehindShapeForGuesses(walker, guess.tested, read);
    if (!field)
        return;
    Draft filled(Opcode::GetSlot, walker.originOf(read));
    walker.chooseConservativeFields(filled);
    filled.setObjectOperand(shaped);
    filled.setKey(keyOfAccessToSlot(walker.program(), read));
    filled.setReceivers(guess.layouts);
    filled.setLevel(KnowledgeLevel::Proven);
    if (Reference value = walker.build(WTF::move(filled)))
        walker.state().noteSlot(shaped, *field, value);
}

Reference buildTestInFrontOfLoop(Walker& walker, const GuessInFrontOfLoop& guess, Origin origin)
{
    return buildTestOfGuess(walker, guess.value, TestOfGuess { guess.tested, guess.layouts }, origin);
}

Reference buildProofInFrontOfLoop(Walker& walker, const GuessInFrontOfLoop& guess, Reference test, Origin origin)
{
    return buildProofOfGuess(walker, guess.value, test, TestOfGuess { guess.tested, guess.layouts }, origin);
}

OperatorForm formAllowedForTypedOperator(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);

namespace {

struct ParameterOfGuard {
    uint32_t entryParameter { 0 };
    TypeID tested;
    TypeID inFrontOfGuard;
    TypeID behindGuard;
    bool isLeanedOn { false };
    TypeID testedWithPrimitives;
    uint16_t primitivesProvenToArriveBesideObjects { 0 };
    bool isListWrittenFromUse { false };
    bool isToldApartByBody { false };
    uint16_t nullishAskedFor { 0 };
    bool isAskedOfBodyForNullish { false };
    bool wordIsWritten { false };
};

struct WorkerOfParameterGuards {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WorkerOfParameterGuards);

    WorkerOfParameterGuards(const WalkInputs& inputs, TypeTable& table)
        : types(table, ArenaKeeps::InProgram)
        , kept(inputs, types)
    {
    }

    TypeArena types;
    KeptBetweenUnits kept;
    WalkerInRoom walker;
};

} // anonymous namespace

static bool mayHaveParameterGuard(const Walker& walker, FunctionID id)
{
    const Function& function = walker.program().at(id);
    if (!walker.plan().conventions().of(id).has(CodeRole::Body) || function.isCoroutine() || function.results.size() != 1)
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return true;
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool describesBagOfOptionsForGuesses(Walker& walker, TypeID expected)
{
    TypeArena& types = walker.types();
    LayoutSetID layouts = expected && !holdsOnlyPrimitives(expected) ? types.at(expected).layouts : LayoutSetID();
    if (!Walker::keepsRoundsOver(types, layouts))
        return describesBagOfOptions(types, expected);
    if (std::optional<uint32_t> kept = walker.roundKept(Walker::RoundKeptByWalker::DescribesBagOfOptions, layouts, 0))
        return *kept;
    bool describesBag = describesBagOfOptions(types, expected);
    walker.keepRound(Walker::RoundKeptByWalker::DescribesBagOfOptions, layouts, 0, describesBag);
    return describesBag;
}

static TypeID typeTestedByParameterGuard(Walker& walker, TypeID expected, TypeID inFrontOfGuard)
{
    TypeArena& types = walker.types();
    if (saysNothingForGuesses(types, expected) || describesBagOfOptionsForGuesses(walker, expected) || !types.at(inFrontOfGuard).isJavaScriptValue())
        return { };
    TypeID tested = types.testablePartOf(types.withLiteralStringsWidened(widenedForTestOfGuess(walker, expected)));
    Type type = types.at(tested);
    if (type.isNever() || !type.isJavaScriptValue() || type.hasUnboundedUnknown() || !changesCodeWhereProven(types, tested))
        return { };

    if (type.hasObjects() && type.hasOnlyLayouts() && !type.hasOnlyClosures()) {
        ShapesOfLayoutsForGuesses shapes = shapesOfLayoutsForGuesses(walker, type.layouts);
        if (shapes == ShapesOfLayoutsForGuesses::NoLayoutHasAny) {
            TypeID above = findLayoutsWithObjectsAboveForGuesses(walker, type.layouts);
            if (!above)
                return { };
            shapes = shapesOfLayoutsForGuesses(walker, types.at(above).layouts);
            tested = types.withPrimitives(above, type.primitives);
        }
        if (shapes != ShapesOfLayoutsForGuesses::Testable)
            return { };
    }
    if (types.isSubset(inFrontOfGuard, tested) || !types.overlap(inFrontOfGuard, tested))
        return { };
    return types.isSubset(types.intersectionOf(inFrontOfGuard, tested), tested) ? tested : TypeID();
}

static constexpr uint32_t mostOperationsOfBodyForOneThatLeans = 4;
static constexpr uint32_t roundsOfLoopForParameterGuards = 8;

static uint32_t weightOutsideLoopsForParameterGuards(Opcode opcode)
{
    return opcode == Opcode::ElemGet || opcode == Opcode::ElemSet ? 2 : 1;
}

static bool everyRowConvertsArgumentToNumberForGuesses(const Program& program, BuiltinID builtin, size_t argument)
{
    const BuiltinTable& table = program.builtins();
    bool someRowDoes = false;
    for (const BuiltinOverload& row : table.overloadsOf(table.canonicalID(builtin))) {
        if (row.mode != BuiltinCallMode::Call)
            continue;
        bool converts = false;
        for (const BuiltinParameter& parameter : row.parameters) {
            if (!parameter.operand.coversArgument(static_cast<unsigned>(argument)))
                continue;
            converts = parameter.conversion == BuiltinConversion::ToNumber || parameter.conversion == BuiltinConversion::ToInt32 || parameter.conversion == BuiltinConversion::ToUint32 || parameter.conversion == BuiltinConversion::ToIntegerOrInfinity;
        }
        if (!converts)
            return false;
        someRowDoes = true;
    }
    return someRowDoes;
}

template<typename Functor>
static bool builtinCalledInLoopConvertsArgumentToNumberForGuesses(const Program& program, const BodyIndex& index, BlockID block, const BodyValue& operation, const Functor& isAskedAbout)
{
    if (operation.opcode() != Opcode::Call || !index.loopDepthOf(block))
        return false;
    ValueID callee = operation.as<Opcode::Call>().calleeOperand();
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin)
        return false;
    BuiltinID builtin = index.value(callee).as<Opcode::Builtin>().builtin();
    std::span<const ValueID> arguments = operation.as<Opcode::Call>().argumentsOperands();
    for (size_t place = 0; place < arguments.size(); ++place) {
        if (arguments[place] && isAskedAbout(arguments[place]) && everyRowConvertsArgumentToNumberForGuesses(program, builtin, place))
            return true;
    }
    return false;
}

static uint32_t findParametersThatBodyLeansOn(Walker& walker, std::span<ParameterOfGuard> parameters, std::span<const TypeID> typesInFrontOfGuard)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypingContext typing { walker.program(), types, walker.positions(), TypeLevel::Proven };

    Vector<uint16_t, 256> namedParameters;
    namedParameters.fill(0, index.numberOfValues());
    for (size_t place = 0; place < parameters.size(); ++place)
        namedParameters[parameters[place].entryParameter] = static_cast<uint16_t>(place + 1);

    auto typeOfOtherInput = [&](ValueID input) -> TypeID {
        if (input.index() < typesInFrontOfGuard.size())
            return typesInFrontOfGuard[input.index()];
        if (!index.isParameter(input) && constantsInFormOfUses.contains(index.opcodeOf(input))) {
            TypeID constant = typeByRowForGuesses(typing, index.value(input), [](ValueID, uint32_t) {
                return TypeID();
            });
            return constant ? constant : TypeID(TypeTable::anything);
        }
        ExpectationForGuesses expectation = findExpectationForGuesses(walker, input);
        if (!expectation.isDeclared || saysNothingForGuesses(types, expectation.type))
            return TypeTable::anything;
        TypeID guessed = types.testablePartOf(types.withLiteralStringsWidened(types.withoutNullish(widenedForTestOfGuess(walker, expectation.type))));
        return saysNothingForGuesses(types, guessed) ? TypeID(TypeTable::anything) : guessed;
    };
    auto typeBehindGuard = [&](ValueID input) -> TypeID {
        uint16_t named = namedParameters[input.index()];
        return named ? parameters[named - 1].behindGuard : typeOfOtherInput(input);
    };
    auto typeInFrontOfGuard = [&](ValueID input) -> TypeID {
        uint16_t named = namedParameters[input.index()];
        return named ? parameters[named - 1].inFrontOfGuard : typeOfOtherInput(input);
    };
    struct NarrowedName {
        TypeID behindGuard;
        TypeID inFrontOfGuard;
    };
    UncheckedKeyHashMap<uint32_t, NarrowedName, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> narrowedNames;
    BitVector blocksThatNoRunEnters;
    std::array<BitVector, 2> branchesWithEdgeThatNoRunTakes;
    BitVector iterationsThatLean;
    auto findNarrowedName = [&](ValueID input) -> const NarrowedName* {
        if (narrowedNames.isEmpty())
            return nullptr;
        auto found = narrowedNames.find(input.index());
        return found == narrowedNames.end() ? nullptr : &found->value;
    };
    auto typeOfNameBehindGuard = [&](ValueID input) -> TypeID {
        const NarrowedName* narrowed = findNarrowedName(input);
        return narrowed ? narrowed->behindGuard : typeBehindGuard(input);
    };
    auto typeOfNameInFrontOfGuard = [&](ValueID input) -> TypeID {
        const NarrowedName* narrowed = findNarrowedName(input);
        return narrowed ? narrowed->inFrontOfGuard : typeInFrontOfGuard(input);
    };
    TypesOfInputs inputsBehindGuard(typeOfNameBehindGuard);
    TypesOfInputs inputsInFrontOfGuard(typeOfNameInFrontOfGuard);

    uint32_t numberThatLean = 0;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        if (blocksThatNoRunEnters.get(number))
            continue;
        if (!narrowedNames.isEmpty() && index.numberOfIncomingEdges(block.id()) > 1 && !index.isLoopHead(block.id()) && !index.isEnteredByHandlerEdge(block.id())) {
            for (uint32_t position = 0; position < block.numberOfParameters(); ++position) {
                NarrowedName met { TypeTable::never, TypeTable::never };
                uint16_t named = 0;
                bool everyWayBringsParameter = true;
                for (const BodyIncomingEdge& arriving : walker.incomingEdgesOf(index, block.id())) {
                    if (blocksThatNoRunEnters.get(arriving.from.index()) || (arriving.targetIndex < 2 && branchesWithEdgeThatNoRunTakes[arriving.targetIndex].get(arriving.from.index())))
                        continue;
                    std::span<const ValueID> arguments = arriving.target.arguments();
                    uint16_t brought = position < arguments.size() && arguments[position] ? namedParameters[arguments[position].index()] : 0;
                    everyWayBringsParameter &= brought && (!named || named == brought);
                    if (!everyWayBringsParameter)
                        break;
                    named = brought;
                    met.behindGuard = types.unionOf(met.behindGuard, typeOfNameBehindGuard(arguments[position]));
                    met.inFrontOfGuard = types.unionOf(met.inFrontOfGuard, typeOfNameInFrontOfGuard(arguments[position]));
                }
                if (everyWayBringsParameter && named) {
                    namedParameters[block.parameter(position).index()] = named;
                    narrowedNames.set(block.parameter(position).index(), met);
                }
            }
        }
        ValueID objectWhoseBirthLeans;
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            bool looksAtParameter = false;
            uint16_t firstNamed = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                uint16_t named = namedParameters[operand.index()];
                if (!named)
                    return;
                if (!operandIndex)
                    firstNamed = named;
                looksAtParameter |= !isOnlyHandedOnForGuesses(operation, operandIndex) || (storesIntoSlotsForGuesses.contains(opcode) && operand == operation.valueOperand()) || opcode == Opcode::NewObject;
            });
            bool convertsParameterToNumberInLoop = builtinCalledInLoopConvertsArgumentToNumberForGuesses(walker.program(), index, block.id(), operation, [&](ValueID argument) {
                return namedParameters[argument.index()] && types.at(typeOfNameBehindGuard(argument)).isNumber() && !types.at(typeOfNameInFrontOfGuard(argument)).isNumber();
            });
            looksAtParameter |= convertsParameterToNumberInLoop;
            if (firstNamed && inputOperationsThatHandOn.contains(opcode)) {
                namedParameters[operation.id().index()] = firstNamed;
                if (const NarrowedName* narrowed = findNarrowedName(operation.operand(opcode == Opcode::NarrowByTest ? OperandIndex::NarrowByTest::left : 0))) {
                    NarrowedName handedOn = *narrowed;
                    narrowedNames.set(operation.id().index(), handedOn);
                }
                continue;
            }
            uint32_t weight = index.loopDepthOf(block.id()) ? roundsOfLoopForParameterGuards : 1;
            if (opcode == Opcode::IteratorOpen && firstNamed && parameters[firstNamed - 1].isToldApartByBody
                && iterationGoesByIndexWithoutTest(walker, typeOfNameBehindGuard(operation.operand(0))) && !iterationGoesByIndexWithoutTest(walker, typeOfNameInFrontOfGuard(operation.operand(0)))) {
                numberThatLean += weight;
                parameters[firstNamed - 1].isLeanedOn = true;
                iterationsThatLean.set(operation.id().index());
                continue;
            }
            if (opcode == Opcode::IteratorResume && iterationsThatLean.get(operation.operand(0).index()))
                numberThatLean += weight;
            if (!looksAtParameter && objectWhoseBirthLeans && storesIntoSlotsForGuesses.contains(opcode) && operation.objectOperand() == objectWhoseBirthLeans && typedFormForGuesses(walker, operation, inputsBehindGuard) != TypedForm::None) {
                numberThatLean += weight;
                continue;
            }
            if (!looksAtParameter || !(operationsThatOpenGuesses.contains(opcode) || opcode == Opcode::NewObject))
                continue;
            if (!convertsParameterToNumberInLoop && typedFormForGuesses(walker, operation, inputsBehindGuard) == TypedForm::None)
                continue;
            if (!convertsParameterToNumberInLoop && typedFormForGuesses(walker, operation, inputsInFrontOfGuard) != TypedForm::None) {
                static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
                if (!operators.contains(opcode))
                    continue;
                OperatorForm formInFrontOfGuard = formAllowedForTypedOperator(walker, opcode, operation, InputsOfTypedOperation(walker, &inputsInFrontOfGuard));
                if (formAllowedForTypedOperator(walker, opcode, operation, InputsOfTypedOperation(walker, &inputsBehindGuard)) == formInFrontOfGuard)
                    continue;
                if (!mayCallRuntime(opcode, formInFrontOfGuard))
                    continue;
            }
            if (allEqualities.contains(opcode)) {
                bool someWordIsWritten = false;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    uint16_t named = namedParameters[operand.index()];
                    someWordIsWritten |= named && parameters[named - 1].wordIsWritten;
                });
                if (!someWordIsWritten)
                    continue;
            }
            numberThatLean += index.loopDepthOf(block.id()) ? roundsOfLoopForParameterGuards : weightOutsideLoopsForParameterGuards(opcode);
            if (ValueID made = storesIntoSlotsForGuesses.contains(opcode) ? index.valueBehindCasts(operation.objectOperand()) : ValueID(); made && !index.isParameter(made) && index.opcodeOf(made) == Opcode::NewObject)
                objectWhoseBirthLeans = operation.objectOperand();
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                uint16_t named = namedParameters[operand.index()];
                if (named && (!convertsParameterToNumberInLoop || types.at(typeOfNameBehindGuard(operand)).isNumber()))
                    parameters[named - 1].isLeanedOn = true;
            });
        }
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            uint16_t named = result.subject ? namedParameters[result.subject.index()] : 0;
            if (!named || !result.parameter || index.numberOfIncomingEdges(index.blockOf(result.parameter)) != 1)
                return;
            namedParameters[result.parameter.index()] = named;
            if (!parameters[named - 1].isToldApartByBody || !result.test || index.isParameter(result.test) || index.opcodeOf(result.test) != Opcode::TypeTest)
                return;
            TypeID tested = index.value(result.test).as<Opcode::TypeTest>().testedType();
            EdgeSide side = result.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken;
            NarrowedName left { narrowByTest(typing, typeOfNameBehindGuard(result.subject), tested, side), narrowByTest(typing, typeOfNameInFrontOfGuard(result.subject), tested, side) };
            narrowedNames.set(result.parameter.index(), left);
        });
        if (BodyTerminator end = block.terminator(); !narrowedNames.isEmpty() && end.kind() == TerminatorKind::Branch) {
            ValueID asked = end.operand(0);
            if (!index.isParameter(asked) && index.opcodeOf(asked) == Opcode::ToBoolean)
                asked = index.value(asked).operand(0);
            asked = index.valueBehindCasts(asked);
            TypeID answer = index.isParameter(asked) ? TypeID() : typeOfIsArrayAhead(walker, index, index.value(asked), inputsBehindGuard);
            if (answer) {
                uint32_t neverTaken = answer == TypeArena::fromPrimitives(Primitive::False) ? 0 : 1;
                BlockID target = end.targets()[neverTaken].block();
                if (index.numberOfIncomingEdges(target) == 1)
                    blocksThatNoRunEnters.set(target.index());
                else
                    branchesWithEdgeThatNoRunTakes[neverTaken].set(number);
            }
        }
    }
    return numberThatLean;
}

static void findParametersThatBodyTellsApart(Walker& walker, std::span<ParameterOfGuard> parameters)
{
    if (std::ranges::none_of(parameters, [](const ParameterOfGuard& parameter) { return !!parameter.testedWithPrimitives; }))
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (operation.opcode() != Opcode::TypeTest)
                continue;
            ValueID subject = index.valueBehindCasts(operation.operand(0));
            while (ValueID tested = index.valueNarrowedInto(subject))
                subject = index.valueBehindCasts(tested);
            TypeID asked = operation.as<Opcode::TypeTest>().testedType();
            for (ParameterOfGuard& parameter : parameters) {
                if (!parameter.testedWithPrimitives || parameter.isToldApartByBody || subject.index() != parameter.entryParameter)
                    continue;
                TypeID members = types.withoutNullish(parameter.testedWithPrimitives);
                parameter.isToldApartByBody = types.overlap(asked, members) && !types.isSubset(members, asked);
                if (!types.overlap(asked, members))
                    parameter.nullishAskedFor |= types.at(asked).primitives & types.at(parameter.testedWithPrimitives).primitives & Primitive::UndefinedOrNull;
            }
        }
    }
    for (ParameterOfGuard& parameter : parameters) {
        if (!parameter.isToldApartByBody)
            continue;
        parameter.tested = parameter.testedWithPrimitives;
        parameter.behindGuard = types.intersectionOf(parameter.inFrontOfGuard, parameter.tested);
    }
}

static void findNullishThatBodyAsksFor(Walker& walker, std::span<ParameterOfGuard> parameters)
{
    if (std::ranges::none_of(parameters, &ParameterOfGuard::isAskedOfBodyForNullish))
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            uint16_t asked = 0;
            if (operation.opcode() == Opcode::ToBoolean)
                asked = Primitive::UndefinedOrNull;
            else if (operation.opcode() == Opcode::TypeTest) {
                Type tested = types.at(operation.as<Opcode::TypeTest>().testedType());
                if (!tested.hasObjects() && !(tested.primitives & ~Primitive::UndefinedOrNull))
                    asked = tested.primitives;
            }
            if (!asked)
                continue;
            ValueID subject = index.valueBehindCasts(operation.operand(0));
            while (ValueID narrowed = index.valueNarrowedInto(subject))
                subject = index.valueBehindCasts(narrowed);
            for (ParameterOfGuard& parameter : parameters) {
                if (parameter.isAskedOfBodyForNullish && subject.index() == parameter.entryParameter)
                    parameter.nullishAskedFor |= asked;
            }
        }
    }
}

static void letThroughNullishThatBodyAsksFor(TypeArena& types, ParameterOfGuard& parameter)
{
    if (parameter.isToldApartByBody || !parameter.nullishAskedFor)
        return;
    parameter.tested = types.withPrimitives(parameter.tested, parameter.nullishAskedFor);
    parameter.behindGuard = types.intersectionOf(parameter.inFrontOfGuard, parameter.tested);
}

static_assert(opcodesWhere([](Opcode opcode) { return inputOperationsThatHandOn.contains(opcode) && (operationsThatOpenGuesses.contains(opcode) || opcode == Opcode::NewObject) && opcode != Opcode::RequireObjectCoercible; }).isEmpty());

static uint32_t countOperationsThatLookAtValuesNamed(const Program& program, const BodyIndex& index, Vector<bool, 256>& namesParameter)
{
    constexpr uint64_t moreUsesThanBodyHolds = 1ull << 32;
    uint64_t usesStillToMeet = 0;
    auto expectUsesOf = [&](ValueID named) {
        uint32_t uses = index.useCount(named);
        usesStillToMeet += uses == BodyIndex::maxCountedUses ? moreUsesThanBodyHolds : uses;
    };
    auto meetUseOf = [&](ValueID used) {
        if (used && namesParameter[used.index()])
            --usesStillToMeet;
    };
    for (uint32_t parameter = 0; parameter < index.block(entryBlock).numberOfParameters(); ++parameter) {
        if (namesParameter[parameter])
            expectUsesOf(ValueID(parameter));
    }

    uint32_t numberThatLook = 0;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!usesStillToMeet || static_cast<uint64_t>(numberThatLook) * mostOperationsOfBodyForOneThatLeans >= index.numberOfValues())
            return numberThatLook;
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        meetUseOf(block.subject());
        ValueID objectStoredInto;
        for (BodyValue operation : block.values()) {
            if (!usesStillToMeet && !objectStoredInto)
                return numberThatLook;
            Opcode opcode = operation.opcode();
            bool looksAtParameter = false;
            bool firstIsNamed = false;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!namesParameter[operand.index()])
                    return;
                --usesStillToMeet;
                if (!operandIndex)
                    firstIsNamed = true;
                looksAtParameter |= !isOnlyHandedOnForGuesses(operation, operandIndex) || (storesIntoSlotsForGuesses.contains(opcode) && operand == operation.valueOperand()) || opcode == Opcode::NewObject;
            });
            looksAtParameter |= builtinCalledInLoopConvertsArgumentToNumberForGuesses(program, index, block.id(), operation, [&](ValueID argument) {
                return namesParameter[argument.index()];
            });
            if (firstIsNamed && inputOperationsThatHandOn.contains(opcode)) {
                namesParameter[operation.id().index()] = true;
                expectUsesOf(operation.id());
                continue;
            }
            if (looksAtParameter && (operationsThatOpenGuesses.contains(opcode) || opcode == Opcode::NewObject))
                numberThatLook += index.loopDepthOf(block.id()) ? roundsOfLoopForParameterGuards : weightOutsideLoopsForParameterGuards(opcode);
            if (storesIntoSlotsForGuesses.contains(opcode)) {
                if (!looksAtParameter) {
                    if (operation.objectOperand() == objectStoredInto)
                        numberThatLook += index.loopDepthOf(block.id()) ? roundsOfLoopForParameterGuards : 1;
                } else if (ValueID made = index.valueBehindCasts(operation.objectOperand()); made && !index.isParameter(made) && index.opcodeOf(made) == Opcode::NewObject)
                    objectStoredInto = operation.objectOperand();
            }
            if (firstIsNamed && opcode == Opcode::IteratorOpen)
                numberThatLook += 2 * roundsOfLoopForParameterGuards;
        }
        if (!usesStillToMeet)
            return numberThatLook;
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            meetUseOf(operand);
        });
        block.forEachOutgoingEdge([&](BodyTarget target, bool) {
            for (ValueID argument : target.arguments())
                meetUseOf(argument);
        });
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            if (!result.subject || !namesParameter[result.subject.index()])
                return;
            if (result.proof != Proof::None)
                --usesStillToMeet;
            if (result.parameter && index.numberOfIncomingEdges(index.blockOf(result.parameter)) == 1 && !std::exchange(namesParameter[result.parameter.index()], true))
                expectUsesOf(result.parameter);
        });
    }
    return numberThatLook;
}

static uint32_t countOperationsThatLookAtParameters(Walker& walker, std::span<const ParameterOfGuard> parameters)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<bool, 256> namesParameter;
    namesParameter.fill(false, index.numberOfValues());
    for (const ParameterOfGuard& parameter : parameters)
        namesParameter[parameter.entryParameter] = true;
    return countOperationsThatLookAtValuesNamed(walker.program(), index, namesParameter);
}

static uint32_t countOperationsInLoopsOnWhatTextsNamedYield(Walker&, const BodyIndex&, BitVector& namesText);

static uint32_t countOperationsInLoopsOnWhatTextsYield(Walker& walker, std::span<const ParameterOfGuard> parameters, bool ofThoseLeanedOnAlone)
{
    const TypeArena& types = walker.types();
    BitVector namesText;
    for (const ParameterOfGuard& parameter : parameters) {
        if ((parameter.isLeanedOn || !ofThoseLeanedOnAlone) && types.at(parameter.behindGuard).hasOnlyPrimitives(Primitive::AnyString))
            namesText.set(parameter.entryParameter);
    }
    if (namesText.isEmpty())
        return 0;
    return countOperationsInLoopsOnWhatTextsNamedYield(walker, walker.currentBodyIndex(), namesText);
}

static uint32_t countOperationsInLoopsOnWhatTextsNamedYield(Walker& walker, const BodyIndex& index, BitVector& namesText)
{
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    if (!index.hasLoops())
        return 0;
    BitVector isYielded;
    uint32_t numberOnWhatIsYielded = 0;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        if (!isYielded.isEmpty() && block.numberOfParameters()) {
            for (const BodyIncomingEdge& arriving : walker.incomingEdgesOf(index, block.id())) {
                std::span<const ValueID> arguments = arriving.target.arguments();
                for (uint32_t position = 0; position < block.numberOfParameters() && position < arguments.size(); ++position) {
                    if (arguments[position] && isYielded.get(arguments[position].index()))
                        isYielded.set(block.parameter(position).index());
                }
            }
        }
        bool isInLoop = index.loopDepthOf(block.id());
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            bool firstNamesText = false;
            bool someNamesText = false;
            bool firstIsYielded = false;
            bool someIsYielded = false;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (namesText.get(operand.index())) {
                    someNamesText = true;
                    firstNamesText |= !operandIndex;
                }
                if (isYielded.get(operand.index())) {
                    someIsYielded = true;
                    firstIsYielded |= !operandIndex;
                }
            });
            if (inputOperationsThatHandOn.contains(opcode)) {
                if (firstNamesText)
                    namesText.set(operation.id().index());
                else if (firstIsYielded)
                    isYielded.set(operation.id().index());
                continue;
            }
            if (!isInLoop)
                continue;
            if (someNamesText && (opcode == Opcode::PropGet || opcode == Opcode::ElemGet || opcode == Opcode::Call)) {
                isYielded.set(operation.id().index());
                continue;
            }
            if (!someIsYielded)
                continue;
            if (operators.contains(opcode)) {
                isYielded.set(operation.id().index());
                numberOnWhatIsYielded += roundsOfLoopForParameterGuards;
            } else if (opcode == Opcode::ElemSet && isYielded.get(operation.valueOperand().index()))
                numberOnWhatIsYielded += roundsOfLoopForParameterGuards;
        }
    }
    return numberOnWhatIsYielded;
}

static bool holdsRecordsAloneForGuesses(const Walker& walker, TypeID type)
{
    const TypeArena& types = walker.types();
    if (!types.at(type).hasOnlyLayouts())
        return false;
    for (LayoutID member : membersOf(types, types.at(type).layouts)) {
        if (walker.program().at(member).kind != LayoutKind::Record)
            return false;
    }
    return true;
}

Vector<TypeID, 8> typesBehindParameterGuard(Walker& walker, FunctionID id)
{
    if (!walker.has(Mechanism::ParameterGuards) || !mayHaveParameterGuard(walker, id))
        return { };
    const LinkTables& link = walker.facts().linked();
    if (id.index() >= link.functions.size())
        return { };
    const Function& function = walker.program().at(id);
    std::span<const LinkedParameter> linked = link.parametersOf(id);
    uint32_t numberOfEntryParameters = function.numberOfEntryParameters();
    if (linked.size() != numberOfEntryParameters)
        return { };
    uint32_t firstPositionalParameter = function.firstParameterIndex();
    uint32_t endOfPositionalParameters = firstPositionalParameter + function.numberOfPositionalParameters();
    auto isTested = [&](uint32_t index) {
        return function.entryParameterKind(index) == EntryParameter::This || (index >= firstPositionalParameter && index < endOfPositionalParameters);
    };
    bool someParameterIsTested = false;
    for (uint32_t index = 0; index < numberOfEntryParameters && !someParameterIsTested; ++index)
        someParameterIsTested = isTested(index);
    if (!someParameterIsTested)
        return { };
    bool bodyLooksTooSeldom = false;
    if (const BodyIndex* index = walker.drawsNoChoices() ? walker.facts().bodies().find(id) : nullptr; index && index->block(entryBlock).numberOfParameters() == numberOfEntryParameters) {
        bool someIsUsed = false;
        for (uint32_t parameter = 0; parameter < numberOfEntryParameters && !someIsUsed; ++parameter)
            someIsUsed = isTested(parameter) && index->useCount(ValueID(parameter));
        bodyLooksTooSeldom = !someIsUsed;
        if (someIsUsed) {
            Vector<bool, 256> namesParameter;
            namesParameter.fill(false, index->numberOfValues());
            BitVector namesText;
            for (uint32_t parameter = 0; parameter < numberOfEntryParameters; ++parameter) {
                if (!isTested(parameter))
                    continue;
                namesParameter[parameter] = true;
                namesText.set(parameter);
            }
            uint64_t mostThatLook = countOperationsThatLookAtValuesNamed(walker.program(), *index, namesParameter);
            bodyLooksTooSeldom = mostThatLook * mostOperationsOfBodyForOneThatLeans < index->numberOfValues()
                && (mostThatLook + countOperationsInLoopsOnWhatTextsNamedYield(walker, *index, namesText)) * mostOperationsOfBodyForOneThatLeans < index->numberOfValues();
        }
    }
    if (bodyLooksTooSeldom)
        return { };
    if (!walker.standInFrontOfBodyOf(id) || walker.currentBodyIndex().block(entryBlock).numberOfParameters() != numberOfEntryParameters)
        return { };

    TypeArena& types = walker.types();
    Vector<TypeID, 8> typesInFrontOfGuard;
    Vector<ParameterOfGuard, 8> parameters;
    std::optional<ParametersReadInLoopsForGuesses> readInLoops;
    for (uint32_t index = 0; index < numberOfEntryParameters; ++index) {
        typesInFrontOfGuard.append(walker.typeArrivingAtEntryParameter(index));
        TypeID tested = isTested(index) && !singleLayoutOf(types, types.at(typesInFrontOfGuard.last())) ? typeTestedByParameterGuard(walker, findExpectationForGuesses(walker, ValueID(index)).type, typesInFrontOfGuard.last()) : TypeID();
        tested = typeTestedInPlaceOfWrittenType(walker, id, index, tested);
        if (tested && !function.has(FunctionFlags::IsChecked) && !types.at(tested).hasObjects()) {
            TypeID seen = typeHeldOfEntryParameterForGuesses(walker, index);
            if (!saysNothingForGuesses(types, seen) && !types.at(seen).hasObjects() && !types.isSubset(seen, tested))
                tested = typeTestedByParameterGuard(walker, types.unionOf(tested, seen), typesInFrontOfGuard.last());
        }
        bool isAskedOfBodyForNullish = false;
        bool isListWrittenFromUse = false;
        if (tested && !function.has(FunctionFlags::IsChecked) && function.entryParameterKind(index) == EntryParameter::This && linked[index].written && holdsRecordsAloneForGuesses(walker, tested))
            tested = { };
        if (tested && !function.has(FunctionFlags::IsChecked) && function.entryParameterKind(index) == EntryParameter::Parameter && types.at(tested).hasObjects()) {
            if (linked[index].written && holdsRecordsAloneForGuesses(walker, tested))
                tested = { };
            else if (linked[index].written && types.isSubset(types.withOnlyObjects(tested), types.builtinClass(BuiltinClasses::Array)))
                isListWrittenFromUse = true;
            else
                isAskedOfBodyForNullish = !types.at(tested).primitives;
        }
        if (tested && !function.has(FunctionFlags::IsChecked) && function.entryParameterKind(index) == EntryParameter::Parameter && !types.at(tested).hasObjects() && !(types.at(tested).primitives & ~Primitive::UndefinedOrNull))
            tested = { };
        if (tested && function.entryParameterKind(index) == EntryParameter::Parameter && types.at(tested).hasObjects() && types.at(types.withOnlyObjects(tested)).hasOnlyLayouts()) {
            LayoutID made = function.constructedClass ? function.constructedClass : classOfConstructorFunction(walker.program(), id);
            TypeID seen = typeHeldOfEntryParameterForGuesses(walker, index);
            bool namesClassBeingMade = made && std::ranges::contains(membersOf(types, types.at(types.withOnlyObjects(tested))), made) && std::ranges::all_of(membersOf(types, types.at(types.withOnlyObjects(tested))), [&](LayoutID layout) {
                return layout == made || !walker.plan().isInUse(layout);
            });
            if (namesClassBeingMade && (saysNothingForGuesses(types, seen) || !types.overlap(seen, types.withOnlyObjects(tested))))
                tested = { };
        }
        if (tested && (loopsTellListFromBytesForGuesses(walker, ValueID(index), readInLoops) || loopsTellTextFromListForGuesses(walker, ValueID(index), readInLoops)))
            tested = { };
        if (tested && isNeverType(types.intersectionOf(typesInFrontOfGuard.last(), tested)))
            tested = { };
        if (!tested)
            continue;
        parameters.append(ParameterOfGuard { index, tested, typesInFrontOfGuard.last(), types.intersectionOf(typesInFrontOfGuard.last(), tested), false, { }, false });
        parameters.last().isAskedOfBodyForNullish = isAskedOfBodyForNullish;
        parameters.last().isListWrittenFromUse = isListWrittenFromUse;
        parameters.last().wordIsWritten = findExpectationForGuesses(walker, ValueID(index)).isDeclared;
        TypeID whole = types.at(tested).hasObjects() ? findWholeExpectationForGuesses(walker, ValueID(index)).type : TypeID();
        if (!saysNothingForGuesses(types, whole) && partThatOneTestTellsForGuesses(types, whole) != whole)
            parameters.last().testedWithPrimitives = typeTestedByParameterGuard(walker, whole, typesInFrontOfGuard.last());
        TypeID proven = linked[index].proven;
        if (proven && !types.at(proven).hasUnboundedUnknown() && (types.at(proven).primitives & ~Primitive::UndefinedOrNull) && types.at(tested).hasObjects() && !(types.at(tested).primitives & ~Primitive::UndefinedOrNull)) {
            parameters.last().primitivesProvenToArriveBesideObjects = types.at(proven).primitives & ~Primitive::UndefinedOrNull;
            if (!parameters.last().testedWithPrimitives)
                parameters.last().testedWithPrimitives = typeTestedByParameterGuard(walker, proven, typesInFrontOfGuard.last());
        }
    }
    if (std::ranges::any_of(parameters, [](const ParameterOfGuard& parameter) { return parameter.isListWrittenFromUse || parameter.primitivesProvenToArriveBesideObjects; })) {
        Vector<ParameterOfGuard, 8> asBodyTellsApart = parameters;
        findParametersThatBodyTellsApart(walker, asBodyTellsApart.mutableSpan());
        size_t kept = 0;
        for (size_t place = 0; place < parameters.size(); ++place) {
            if ((parameters[place].isListWrittenFromUse || parameters[place].primitivesProvenToArriveBesideObjects) && !asBodyTellsApart[place].isToldApartByBody)
                continue;
            if (parameters[place].primitivesProvenToArriveBesideObjects & ~types.at(asBodyTellsApart[place].tested).primitives)
                continue;
            parameters[kept++] = parameters[place];
        }
        parameters.shrink(kept);
    }
    if (parameters.isEmpty())
        return { };
    Walker::ChoiceOfCost guardIsDrawn = walker.drawChoice(DrawnFor::ParameterGuard, id.index(), 0);
    if (guardIsDrawn == Walker::ChoiceOfCost::No)
        return { };
    if (guardIsDrawn == Walker::ChoiceOfCost::AsItPays) {
        uint64_t numberThatLook = countOperationsThatLookAtParameters(walker, parameters.span());
        if (numberThatLook * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues()
            && (numberThatLook + countOperationsInLoopsOnWhatTextsYield(walker, parameters.span(), false)) * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues())
            return { };
    }
    Vector<ParameterOfGuard, 8> asOneTestTells = parameters;
    findParametersThatBodyTellsApart(walker, parameters.mutableSpan());
    findNullishThatBodyAsksFor(walker, parameters.mutableSpan());
    for (ParameterOfGuard& parameter : parameters)
        letThroughNullishThatBodyAsksFor(types, parameter);
    uint32_t numberThatLean = findParametersThatBodyLeansOn(walker, parameters.mutableSpan(), typesInFrontOfGuard.span());
    size_t numberKept = 0;
    for (size_t place = 0; place < parameters.size(); ++place) {
        if (parameters[place].isListWrittenFromUse && !parameters[place].isLeanedOn)
            continue;
        asOneTestTells[numberKept] = asOneTestTells[place];
        parameters[numberKept++] = parameters[place];
    }
    parameters.shrink(numberKept);
    asOneTestTells.shrink(numberKept);
    if (parameters.isEmpty())
        return { };
    bool someGoBack = false;
    for (size_t place = 0; place < parameters.size(); ++place) {
        if (!parameters[place].isToldApartByBody || parameters[place].isLeanedOn)
            continue;
        uint16_t nullishAskedFor = parameters[place].nullishAskedFor;
        parameters[place] = asOneTestTells[place];
        parameters[place].nullishAskedFor = nullishAskedFor;
        letThroughNullishThatBodyAsksFor(types, parameters[place]);
        someGoBack = true;
    }
    if (someGoBack) {
        for (ParameterOfGuard& parameter : parameters)
            parameter.isLeanedOn = false;
        numberThatLean = findParametersThatBodyLeansOn(walker, parameters.mutableSpan(), typesInFrontOfGuard.span());
    }
    if (guardIsDrawn == Walker::ChoiceOfCost::AsItPays && static_cast<uint64_t>(numberThatLean) * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues()
        && (static_cast<uint64_t>(numberThatLean) + countOperationsInLoopsOnWhatTextsYield(walker, parameters.span(), true)) * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues())
        return { };

    Vector<TypeID, 8> typesBehindGuard;
    for (const ParameterOfGuard& parameter : parameters) {
        if (!parameter.isLeanedOn)
            continue;
        if (typesBehindGuard.isEmpty())
            typesBehindGuard.fill(TypeID(), numberOfEntryParameters);
        typesBehindGuard[parameter.entryParameter] = parameter.tested;
    }
    return typesBehindGuard;
}

Vector<ParameterGuardPlan> planParameterGuards(Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    if (!facts.mechanisms().contains(Mechanism::ParameterGuards))
        return { };
    const ParallelLoops& loops = build.parallelLoops();
    Arrivals nothingIsLeftYet(program, facts, program.types);
    WalkOptions options = walkOptionsOf(build);
    options.mechanisms = facts.mechanisms();
    WalkInputs inputs { program, facts, plan, options, nothingIsLeftYet, &build.regExpSummaries() };

    Vector<std::unique_ptr<WorkerOfParameterGuards>> workers(loops.width());
    ChunksOfIndices division = divideIntoChunks(static_cast<unsigned>(program.functions.size()));
    Vector<Vector<ParameterGuardPlan>> guardsInChunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk, unsigned worker) {
        if (!workers[worker])
            workers[worker] = makeUnique<WorkerOfParameterGuards>(inputs, program.types);
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            if (!workers[worker]->walker || workers[worker]->walker->hasBegunUnit()) {
                WalkerInRoom former = std::exchange(workers[worker]->walker, workers[worker]->kept.makeWalker(inputs, workers[worker]->types, inputs.arrivals));
                if (former)
                    workers[worker]->walker->takeRoomOf(*former);
            }
            Vector<TypeID, 8> typesBehindGuard = typesBehindParameterGuard(*workers[worker]->walker, FunctionID(index));
            if (!typesBehindGuard.isEmpty())
                guardsInChunks[chunk].append(ParameterGuardPlan { FunctionID(index), WTF::move(typesBehindGuard) });
        }
    });
    return joinChunks<ParameterGuardPlan>(loops, WTF::move(guardsInChunks));
}

static ASCIILiteral whereValueComesFromForExplanation(Walker& walker, const BodyIndex& index, ValueID source)
{
    while (!index.isParameter(source) && inputOperationsThatHandOn.contains(index.opcodeOf(source)))
        source = index.value(source).operand(0);
    if (index.isParameter(source))
        return index.blockOf(source) == entryBlock ? (walker.currentFunction() == walker.unit().function ? "an entry parameter"_s : "an entry parameter of a copy in line"_s) : "a join"_s;
    switch (index.opcodeOf(source)) {
    case Opcode::PropGet:
    case Opcode::ElemGet:
    case Opcode::PrivateGet:
        return "a read off an object"_s;
    case Opcode::Call:
    case Opcode::CallDirect:
    case Opcode::Construct:
        return "a call"_s;
    case Opcode::GetEnvironmentField:
    case Opcode::GetCapture:
    case Opcode::GetModuleVariable:
        return "a variable"_s;
    default:
        return "another operation"_s;
    }
}

static TypeID typeThatGuessAtReceiverWouldTestForGuesses(Walker& walker, const BodyValue& access, TypeID expected)
{
    TypeArena& types = walker.types();
    if (LayoutID heir = layoutKnownByStructureOfClass(walker, types.withoutNullish(expected))) {
        TypeID ofHeir = types.forLayout(heir);
        ValueID receiver = access.objectOperand();
        auto inputs = [&walker, receiver, ofHeir](ValueID input) -> TypeID {
            return input == receiver ? ofHeir : provenTypeOfInputForGuesses(walker, input);
        };
        if (typedFormForGuesses(walker, access, TypesOfInputs(inputs)) != TypedForm::None)
            return ofHeir;
    }
    return types.withoutNullish(types.testablePartOf(types.withLiteralStringsWidened(widenedForTestOfGuess(walker, expected))));
}

static void explainClosedCall(Walker& walker, const BodyValue& operation, PrintStream& out)
{
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    Opcode opcode = operation.opcode();
    ValueID callee = constructionsOfParentForGuesses.contains(opcode) ? operation.parentOperand() : operation.calleeOperand();
    ValueID read = callee ? index.valueBehindCasts(callee) : ValueID();
    ValueID receiver = opcode == Opcode::Call ? operation.as<Opcode::Call>().thisValueOperand() : ValueID();
    bool isByName = read && receiver && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName()
        && index.valueBehindCasts(index.value(read).objectOperand()) == index.valueBehindCasts(receiver);
    ValueID subject = isByName ? receiver : callee;
    if (!subject)
        return;
    TypeID proven = provenTypeOfInputForGuesses(walker, subject);
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, subject);

    bool standsApartFromRead = false;
    if (isByName) {
        standsApartFromRead = index.blockOf(read) != index.blockOf(operation.id());
        for (uint32_t between = read.index() + 1; !standsApartFromRead && between < operation.id().index(); ++between) {
            Opcode inBetween = index.opcodeOf(ValueID(between));
            const Effects& bound = declaredBoundInTable(inBetween);
            standsApartFromRead = !opcodesThatStateTypes.contains(inBetween) && !(bound.isUnobservable() && bound.reads.isEmpty());
        }
    }

    ASCIILiteral stage = ""_s;
    ASCIILiteral reason = ""_s;
    TypeID provenObjects = proven ? types.withoutNullish(proven) : TypeID();
    Type provenType = types.at(provenObjects ? provenObjects : TypeID(TypeTable::anything));
    bool objectsAreProven = provenObjects && !provenType.hasUnboundedUnknown() && provenType.isJavaScriptValue() && provenType.hasObjects() && !provenType.primitives;
    ASCIILiteral whereItStands = walker.isInLoop() ? "in a loop"_s : "outside of loops"_s;
    if (walker.unit().role == CodeRole::SlowBody)
        stage = "slow version"_s;
    else if (!walker.opensGuesses())
        stage = "closed arm of a guess"_s;
    else if (isByName) {
        Draft asWritten = walker.draftOf(operation);
        nameMethodOfReceiverAsMade(walker, operation, asWritten);
        BodyValue readOfMethod = index.value(read);
        if (asWritten.methodOfReceiverAsMade()) {
            bool isText = proven && !isNeverType(proven) && types.at(proven).isJavaScriptValue() && types.at(proven).hasOnlyPrimitives(Primitive::AnyString);
            stage = asWritten.methodIsNamedByKeyAlone() ? "A BUILT-IN'S NAME, THE RECEIVER'S CLASS NEITHER PROVEN NOR EXPECTED"_s
                : objectsAreProven || isText ? "PROVEN, A BUILT-IN'S METHOD"_s
                : expectation.isDeclared ? "DECLARED, A BUILT-IN'S METHOD"_s : "SEEN TO ARRIVE, A BUILT-IN'S METHOD"_s;
            reason = standsApartFromRead ? (walker.isInLoop() ? "the method is read in front of operands that may be seen, in a loop: by value, the stub looks at the receiver"_s : "the method is read in front of operands that may be seen, outside of loops: by value, the stub looks at the receiver"_s)
                : walker.isInLoop() ? "one call, in a loop: by name, the stub looks at the receiver"_s : "one call opens no guess outside of loops: by name, the stub looks at the receiver"_s;
        } else if (objectsAreProven) {
            stage = "PROVEN"_s;
            reason = whyAccessByNameHasNoTypedForm(walker, readOfMethod, provenObjects);
        } else if (!expectation.type)
            stage = "NOTHING EXPECTED"_s;
        else if (saysNothingForGuesses(types, expectation.type))
            stage = "EXPECTED SAYS NOTHING"_s;
        else if (describesBagOfOptions(types, expectation.type))
            stage = "EXPECTED IS A BAG OF OPTIONS"_s;
        else {
            TypeID tested = typeThatGuessAtReceiverWouldTestForGuesses(walker, readOfMethod, expectation.type);
            Type type = types.at(tested);
            if (type.isNever() || !type.isJavaScriptValue() || type.hasUnboundedUnknown())
                stage = "EXPECTED HAS NO TEST"_s;
            else {
                ValueID objectOfRead = readOfMethod.objectOperand();
                auto inputs = [&walker, objectOfRead, tested](ValueID input) -> TypeID {
                    return input == objectOfRead ? tested : provenTypeOfInputForGuesses(walker, input);
                };
                if (typedFormForGuesses(walker, readOfMethod, TypesOfInputs(inputs)) == TypedForm::None) {
                    stage = expectation.isDeclared ? "DECLARED, THE READ OF THE METHOD HAS NO TYPED FORM"_s : "SEEN TO ARRIVE, THE READ OF THE METHOD HAS NO TYPED FORM"_s;
                    reason = whyAccessByNameHasNoTypedForm(walker, readOfMethod, tested);
                } else {
                    stage = expectation.isDeclared ? "DECLARED, THE READ OF THE METHOD HAS A TYPED FORM, NOTHING TESTED"_s : "SEEN TO ARRIVE, THE READ OF THE METHOD HAS A TYPED FORM, NOTHING TESTED"_s;
                    reason = whereItStands;
                }
            }
        }
    } else {
        unsigned numberOfFunctions = 0;
        if (objectsAreProven && provenType.hasOnlyClosures()) {
            forEachFunctionOf(types, provenType, scopedLambda<void(FunctionID)>([&](FunctionID) {
                ++numberOfFunctions;
            }));
        }
        reason = whereItStands;
        if (numberOfFunctions == 1)
            stage = "PROVEN, THE CLOSURES OF ONE FUNCTION"_s;
        else if (numberOfFunctions == 2)
            stage = "PROVEN, THE CLOSURES OF TWO FUNCTIONS: NO DISPATCH"_s;
        else if (numberOfFunctions && numberOfFunctions <= 4)
            stage = "PROVEN, THE CLOSURES OF THREE OR FOUR FUNCTIONS: NO DISPATCH"_s;
        else if (numberOfFunctions)
            stage = "PROVEN, THE CLOSURES OF FIVE AND MORE FUNCTIONS"_s;
        else if (walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()))
            stage = "linked() LISTS ONE CALLEE, NOTHING TESTED"_s;
        else if (walker.facts().held().calleeAt(walker.currentFunction(), operation.id()))
            stage = "held() LISTS ONE CALLEE, NOTHING TESTED"_s;
        else if (objectsAreProven && singleBuiltinOf(types, provenType))
            stage = "PROVEN, ONE BUILT-IN: NO ROW TAKES THE OPERANDS"_s;
        else if (!expectation.type || saysNothingForGuesses(types, expectation.type))
            stage = "NOBODY IS LISTED, NOTHING EXPECTED"_s;
        else
            stage = expectation.isDeclared ? "NOBODY IS LISTED, A TYPE IS DECLARED"_s : "NOBODY IS LISTED, A TYPE WAS SEEN TO ARRIVE"_s;
    }

    StringPrintStream line;
    Origin origin = walker.originOfOperation();
    line.print("f", walker.unit().function.index(), ":", walker.unit().role == CodeRole::Body ? "Body" : walker.unit().role == CodeRole::SlowBody ? "SlowBody" : walker.unit().role == CodeRole::CoroutineBody ? "CoroutineBody" : "other");
    line.print("\t", static_cast<uint32_t>(origin.inlineFrame), "\t", origin.position, "\t", nameOf(opcode), isByName ? (standsApartFromRead ? " by value of a method" : " by name") : " by value", "\t", whereValueComesFromForExplanation(walker, index, subject), "\t", stage, "\t", reason, "\t");
    if (proven)
        printType(line, types, proven);
    line.print("\t");
    if (expectation.type)
        printType(line, types, expectation.type);
    line.print("\t");
    Vector<char16_t> buffer;
    if (isByName) {
        if (StringID text = types.at(index.value(read).key()).text)
            line.print(types.viewOf(text, buffer));
    } else if (opcode == Opcode::Call) {
        if (StringID text = operation.as<Opcode::Call>().calleeName())
            line.print(types.viewOf(text, buffer));
    }
    out.println(line.toString());
}

void explainClosedAccess(Walker& walker, const BodyValue& operation, PrintStream& out)
{
    constexpr OpcodeSet accessesByName { Opcode::PropGet, Opcode::PropSet, Opcode::PropDefine };
    constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine };
    Opcode opcode = operation.opcode();
    if (callsOfValuesForGuesses.contains(opcode))
        return explainClosedCall(walker, operation, out);
    bool isByName = accessesByName.contains(opcode);
    if (!isByName && !accessesByKey.contains(opcode))
        return;
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID receiver = operation.objectOperand();
    TypeID proven = provenTypeOfInputForGuesses(walker, receiver);
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, receiver);

    ValueID source = receiver;
    while (!index.isParameter(source) && inputOperationsThatHandOn.contains(index.opcodeOf(source)))
        source = index.value(source).operand(0);
    ASCIILiteral comesFrom = "another operation"_s;
    if (index.isParameter(source))
        comesFrom = index.blockOf(source) == entryBlock ? (walker.currentFunction() == walker.unit().function ? "an entry parameter"_s : "an entry parameter of a copy in line"_s) : "a join"_s;
    else {
        switch (index.opcodeOf(source)) {
        case Opcode::PropGet:
        case Opcode::ElemGet:
        case Opcode::PrivateGet:
            comesFrom = "a read off an object"_s;
            break;
        case Opcode::Call:
        case Opcode::CallDirect:
        case Opcode::Construct:
            comesFrom = "a call"_s;
            break;
        case Opcode::GetEnvironmentField:
        case Opcode::GetCapture:
        case Opcode::GetModuleVariable:
            comesFrom = "a variable"_s;
            break;
        default:
            break;
        }
    }

    ASCIILiteral stage = ""_s;
    ASCIILiteral reason = ""_s;
    TypeID provenObjects = proven ? types.withoutNullish(proven) : TypeID();
    Type provenType = types.at(provenObjects ? provenObjects : TypeID(TypeTable::anything));
    auto typesWithReceiver = [&](TypeID typeOfReceiver) {
        return [&walker, receiver, typeOfReceiver](ValueID input) -> TypeID {
            return input == receiver ? typeOfReceiver : provenTypeOfInputForGuesses(walker, input);
        };
    };
    if (walker.unit().role == CodeRole::SlowBody)
        stage = "slow version"_s;
    else if (!walker.opensGuesses())
        stage = "closed arm of a guess"_s;
    else if (provenObjects && !provenType.hasUnboundedUnknown() && provenType.isJavaScriptValue() && provenType.hasObjects() && !provenType.primitives) {
        stage = "PROVEN"_s;
        if (isByName)
            reason = whyAccessByNameHasNoTypedForm(walker, operation, provenObjects);
    } else if (!expectation.type)
        stage = "NOTHING EXPECTED"_s;
    else if (saysNothingForGuesses(types, expectation.type))
        stage = "EXPECTED SAYS NOTHING"_s;
    else if (describesBagOfOptions(types, expectation.type))
        stage = "EXPECTED IS A BAG OF OPTIONS"_s;
    else {
        TypeID tested = typeThatGuessAtReceiverWouldTestForGuesses(walker, operation, expectation.type);
        Type type = types.at(tested);
        if (type.isNever() || !type.isJavaScriptValue() || type.hasUnboundedUnknown())
            stage = "EXPECTED HAS NO TEST"_s;
        else {
            auto inputs = typesWithReceiver(tested);
            TypedForm form = typedFormForGuesses(walker, operation, TypesOfInputs(inputs));
            if (form == TypedForm::None) {
                stage = expectation.isDeclared ? "DECLARED, NO TYPED FORM"_s : "SEEN TO ARRIVE, NO TYPED FORM"_s;
                if (isByName)
                    reason = whyAccessByNameHasNoTypedForm(walker, operation, tested);
            } else {
                stage = expectation.isDeclared ? "DECLARED, HAS A TYPED FORM, NOTHING TESTED"_s : "SEEN TO ARRIVE, HAS A TYPED FORM, NOTHING TESTED"_s;
                reason = walker.isInLoop() ? "in a loop"_s : "outside of loops"_s;
            }
        }
    }

    StringPrintStream line;
    Origin origin = walker.originOfOperation();
    line.print("f", walker.unit().function.index(), ":", walker.unit().role == CodeRole::Body ? "Body" : walker.unit().role == CodeRole::SlowBody ? "SlowBody" : walker.unit().role == CodeRole::CoroutineBody ? "CoroutineBody" : "other");
    line.print("\t", static_cast<uint32_t>(origin.inlineFrame), "\t", origin.position, "\t", nameOf(opcode), "\t", comesFrom, "\t", stage, "\t", reason, "\t");
    if (proven)
        printType(line, types, proven);
    line.print("\t");
    if (expectation.type)
        printType(line, types, expectation.type);
    line.print("\t");
    if (isByName) {
        Vector<char16_t> buffer;
        if (StringID text = types.at(operation.key()).text)
            line.print(types.viewOf(text, buffer));
    } else if (TypeID key = provenTypeOfInputForGuesses(walker, operation.propertyOperand()))
        printType(line, types, key);
    out.println(line.toString());
}

bool loopReckonsWithElementRead(Walker& walker, const BodyValue& read)
{
    return (read.opcode() == Opcode::ElemGet || read.opcode() == Opcode::ElemSet) && operatorThatElementGoesThroughForGuesses(walker, read);
}

BitVector findHoldersOfElementsThatLoopComparesWithCharactersOrNumbers(const Program& program, const BodyIndex& index, LoopID loop)
{
    TextOfLoopForGuesses text(index, loop, index.block(index.loop(loop).head).firstParameter().index());
    text.comparesElementsWithCharacters(program, index.block(index.loop(loop).head).firstParameter());
    BitVector holders = text.holdersOfElementsComparedWithCharacters();
    holders.merge(text.holdersOfElementsComparedWithNumbers());
    return holders;
}

bool loopComparesWhatStepsHandOutWithCharacters(const Program& program, const BodyIndex& index, LoopID loop, ValueID open)
{
    TextOfLoopForGuesses text(index, loop, index.block(index.loop(loop).head).firstParameter().index());
    return text.comparesElementsWithCharacters(program, open);
}

bool writtenWordOfParameterNamesTextBesideList(Walker& walker, ValueID operand)
{
    return !!listBesideTextInWrittenWordForGuesses(walker, operand);
}

bool readsElementUnderIndexOffOneOf(const BodyIndex& index, const BodyValue& read, const BitVector& holders)
{
    return read.opcode() == Opcode::ElemGet && keySpeaksForIndexForGuesses(index, read) && holders.get(valueBehindCastsAndTestsForGuesses(index, read.objectOperand()).index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

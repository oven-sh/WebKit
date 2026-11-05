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
#include "AOTWalkCoroutinePositions.h"
#include "AOTWalkCoroutineCalls.h"
#include "AOTArrayStorageForms.h"
#include "AOTSweep.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkTextBuilderRows.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

TypeID topsOfKindsInLoops(TypeArena&, TypeID);
TypeID tryTypeOfFieldRead(const TypingContext&, TypeID receiver, PropertyKeyID);

static constexpr uint32_t minimumOperationsPerGuess = 2;
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
};

struct TestOfGuess {
    TypeID tested;
    LayoutSetID layouts;
    bool isPassedAlready { false };
    TypeID failingSideAsksFor;
    bool failingSideAsksForNothing { false };
    bool asksForElementOfNumbers { false };
    TypeID elementInsideView;
    bool admitsAllThatIsExpected { false };
    bool failingSideIsAsLikely { false };
    bool typedRunOpensGuesses { false };
};

struct LookAtSlotOfGuess {
    ValueID access;
    ValueID holder;
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
    Vector<ValueID, 4> storesThatLookAtCell;
    Vector<TestOfStoredValueOfGuess, 4> testsOfStoredValues;
    std::optional<uint32_t> orderWordAskedFor;
    BitVector placesThatRestOnProof;
    BitVector placesThatTypedRunKnowsBetter;
    Vector<BuiltinID, 2> membersAskedByTheirBytes;
    HoldersBelowMember holdersAskedForAddedNames;
    bool someMemberRestsOnByteOfClass { false };
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

static bool saysNothingForGuesses(const TypeArena& types, TypeID type)
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

static ExpectationForGuesses expectationOfEntryParameterForGuesses(const Walker& walker, uint32_t entryParameter)
{
    const LinkTables& link = walker.facts().linked();
    FunctionID function = walker.currentFunction();
    if (function.index() >= link.functions.size())
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
    return { parameters[entryParameter].expected, parameters[entryParameter].expectedIsDeclared };
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

static TypeID typeOfReadForGuesses(Walker& walker, const TypingContext& typing, const BodyValue& read, TypeID receiver)
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

static TypeID typeOfReadAtLevelExpectedForGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    return typeOfReadForGuesses(walker, { walker.program(), walker.types(), walker.positionsKnowingNothing(), TypeLevel::Expected }, read, receiver);
}

static TypeID typeOfReadByWhatIsHeldForGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    TypeArena& types = walker.types();
    TypingContext byWhatIsHeld { walker.program(), types, walker.heldPositions(), TypeLevel::Proven };
    TypeID ofAll = typeOfReadForGuesses(walker, byWhatIsHeld, read, receiver);
    if (!receiver || read.opcode() != Opcode::PropGet || read.isFreeName())
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

static TypeID typeHeldOfEntryParameterForGuesses(const Walker& walker, uint32_t entryParameter)
{
    const LinkTables& held = walker.facts().held();
    FunctionID function = walker.currentFunction();
    if (function.index() >= held.functions.size())
        return { };
    std::span<const LinkedParameter> parameters = held.parametersOf(function);
    if (entryParameter >= parameters.size())
        return { };
    const LinkedParameter& parameter = parameters[entryParameter];
    bool isThisOfMemberAboveCreatedObjects = parameter.expected && walker.program().at(function).thisParameterIndex() == entryParameter && walker.types().at(parameter.expected).hasOnlyLayouts()
        && std::ranges::all_of(membersOf(walker.types(), walker.types().at(parameter.expected)), [&](LayoutID layout) {
            return walker.program().at(layout).has(LayoutFlags::IsOfCreatedObject);
        });
    return isThisOfMemberAboveCreatedObjects ? parameter.expected : parameter.proven;
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

struct KeptWithBodyForGuesses final : KeptWithBodyIndex {
    Vector<uint32_t> firstIncomingEdges;
    Vector<BodyIncomingEdge> incomingEdges;
    Vector<uint32_t> rootsOfValues;
    Vector<uint32_t> rootsAskedForNames;
    Vector<PropertyKeyID> namesUsedOffValues;
    Vector<uint64_t> castsOfRoots;
    Vector<uint64_t> viewsOfRoots;
    Lock lockOfLists;
    std::atomic<bool> hasIncomingEdges { false };
    std::atomic<bool> hasNamesUsedOffValues { false };
};

} // anonymous namespace

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

static void listNamesUsedOffValuesForGuesses(Walker& walker, const BodyIndex& index, KeptWithBodyForGuesses& kept)
{
    static constexpr OpcodeSet operationsThatAskForNames { Opcode::PropGet, Opcode::PropSet, Opcode::Call };
    if (!index.holdsAnyOf(operationsThatAskForNames))
        return;
    uint32_t numberOfValues = index.numberOfValues();
    Vector<uint32_t> roots(numberOfValues, [](size_t number) { return static_cast<uint32_t>(number); });
    Vector<uint64_t> uses;
    Vector<uint64_t> arrivals;
    Vector<uint64_t> casts;
    Vector<uint64_t> views;
    auto use = [&](ValueID object, PropertyKeyID name) {
        if (object && name)
            uses.append(static_cast<uint64_t>(roots[object.index()]) << 32 | name.index());
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
            BodyValue operation = index.value(ValueID(firstValue + place));
            switch (Opcode opcode = operation.opcode()) {
            case Opcode::PropGet:
                if (auto read = operation.as<Opcode::PropGet>(); !read.isFreeName())
                    use(read.objectOperand(), read.key());
                break;
            case Opcode::PropSet:
                if (auto store = operation.as<Opcode::PropSet>(); !store.isFreeName())
                    use(store.objectOperand(), store.key());
                break;
            case Opcode::Call:
                use(operation.as<Opcode::Call>().thisValueOperand(), operation.as<Opcode::Call>().calleeKey());
                break;
            default: {
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
    auto sortWithoutRepeats = [](Vector<uint64_t>& list) {
        std::ranges::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
    };
    if (uses.isEmpty())
        return;
    sortWithoutRepeats(uses);
    for (size_t sizeBefore = 0; !arrivals.isEmpty() && sizeBefore != uses.size();) {
        sizeBefore = uses.size();
        for (uint64_t arrival : arrivals) {
            uint64_t taking = static_cast<uint64_t>(roots[static_cast<uint32_t>(arrival)]) << 32;
            if (taking >> 32 == arrival >> 32)
                continue;
            std::span<const uint64_t> sorted = uses.span().first(sizeBefore);
            size_t first = std::ranges::lower_bound(sorted, taking) - sorted.begin();
            for (size_t place = first; place < sizeBefore && uses[place] >> 32 == taking >> 32; ++place)
                uses.append((arrival >> 32) << 32 | static_cast<uint32_t>(uses[place]));
        }
        if (uses.size() != sizeBefore)
            sortWithoutRepeats(uses);
    }

    kept.rootsAskedForNames.reserveInitialCapacity(uses.size());
    kept.namesUsedOffValues.reserveInitialCapacity(uses.size());
    for (uint64_t used : uses) {
        kept.rootsAskedForNames.append(static_cast<uint32_t>(used >> 32));
        kept.namesUsedOffValues.append(PropertyKeyID(static_cast<uint32_t>(used)));
    }
    std::ranges::sort(casts);
    kept.castsOfRoots = WTF::move(casts);
    std::ranges::sort(views);
    kept.viewsOfRoots = WTF::move(views);
    kept.rootsOfValues = WTF::move(roots);
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

std::span<const PropertyKeyID> Walker::namesUsedOff(ValueID value)
{
    const BodyIndex& index = *m_frames.last().index;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(*this, index, kept);
    });
    if (kept.rootsOfValues.isEmpty())
        return { };
    auto asked = std::ranges::equal_range(kept.rootsAskedForNames, kept.rootsOfValues[value.index()]);
    return kept.namesUsedOffValues.span().subspan(asked.begin() - kept.rootsAskedForNames.begin(), asked.size());
}

bool Walker::appendNamesUsedOffViewsOf(ValueID value, Vector<PropertyKeyID, 16>& names)
{
    const BodyIndex& index = *m_frames.last().index;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(*this, index, kept);
    });
    if (kept.rootsOfValues.isEmpty() || kept.viewsOfRoots.isEmpty())
        return false;
    size_t sizeBefore = names.size();
    Vector<uint32_t, 8> roots { kept.rootsOfValues[value.index()] };
    for (size_t next = 0; next < roots.size(); ++next) {
        auto views = std::ranges::equal_range(kept.viewsOfRoots, static_cast<uint64_t>(roots[next]), { }, [](uint64_t view) { return view >> 32; });
        for (uint64_t view : views) {
            uint32_t rootOfView = kept.rootsOfValues[static_cast<uint32_t>(view)];
            roots.append(rootOfView);
            auto asked = std::ranges::equal_range(kept.rootsAskedForNames, rootOfView);
            names.append(kept.namesUsedOffValues.span().subspan(asked.begin() - kept.rootsAskedForNames.begin(), asked.size()));
        }
    }
    return names.size() != sizeBefore;
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
    if (kept.rootsOfValues.isEmpty())
        return { };
    auto found = std::ranges::equal_range(kept.castsOfRoots, static_cast<uint64_t>(kept.rootsOfValues[value.index()]), { }, [](uint64_t cast) { return cast >> 32; });
    return kept.castsOfRoots.span().subspan(found.begin() - kept.castsOfRoots.begin(), found.size());
}

static constexpr uint32_t fewestLayoutsOfSetWhoseRoundsWalkerKeeps = 8;

bool Walker::keepsRoundsOver(const TypeArena& types, LayoutSetID set)
{
    return set && homeOf(set) == IDHome::Program && types.at(set).size() >= fewestLayoutsOfSetWhoseRoundsWalkerKeeps;
}

static uint64_t placeOfRoundKeptByWalker(Walker::RoundKeptByWalker asker, LayoutSetID set, uint32_t second)
{
    RELEASE_ASSERT(second <= idPositionMask);
    return static_cast<uint64_t>(asker) << 62 | static_cast<uint64_t>(second) << 32 | set.bits();
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
        if (std::ranges::equal(m_listsOfNames[number], names))
            return number;
    }
    numbers.append(static_cast<uint32_t>(m_listsOfNames.size()));
    m_listsOfNames.append(Vector<PropertyKeyID>(names));
    return numbers.last();
}

static PropertyKeyID findNameUsedOffValueForGuesses(const Program& program, const BodyIndex& index, ValueID value)
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

static ExpectationForGuesses expectationByNamesUsedForGuesses(Walker& walker, ValueID input)
{
    const LayoutsByNames& layouts = walker.facts().layoutsByNames();
    bool isInLoop = walker.isInLoop();
    if (PropertyKeyID name = findNameUsedOffValueForGuesses(walker.program(), walker.currentBodyIndex(), input); name && !(isInLoop ? layouts.isNameOfSomeLayout(name) : layouts.isNameOfSomeInstance(name)))
        return { };
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    LayoutID only = layouts.findOnlyLayoutWithAll(names, isInLoop && names.size() > 1);
    if (Vector<PropertyKeyID, 16> withViews; !only && !names.empty() && walker.appendNamesUsedOffViewsOf(input, withViews)) {
        withViews.append(names);
        only = layouts.findOnlyLayoutWithAll(withViews.span(), false);
    }
    if (only && walker.program().at(only).kind == LayoutKind::Record) {
        const Layout& record = walker.program().at(only);
        const SlotPlan& slots = walker.plan().slots();
        bool everyNameIsPlainField = std::ranges::all_of(names, [&](PropertyKeyID name) {
            std::optional<uint32_t> field = record.findField(name);
            if (!field || record.fields[*field].flags.contains(FieldFlags::IsMethod))
                return false;
            std::optional<FieldIndex> placed = slots.findField(walker.program(), only, name);
            if (!placed)
                return false;
            const PlacedSlot& slot = slots.slot(only, *placed);
            TypeID held = slot.typeThatStoresAreHeldTo();
            return slot.representation != SlotRepresentation::DeferredClosure && !slot.deferredFunction && walker.withoutFunctionsThatMayNotBeMentioned(held) == held;
        });
        if (!everyNameIsPlainField)
            return { };
    }
    return only ? ExpectationForGuesses { walker.types().forLayout(only), false } : ExpectationForGuesses();
}

static TypeID narrowedByNamesUsedForGuesses(Walker& walker, ValueID input, TypeID expected)
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
        bool isAnsweredByBuiltin = liesInThread(name) || walker.program().builtinNameOf(name);
        LayoutID only = isAnsweredByBuiltin ? LayoutID() : walker.facts().layoutsByNames().findOnlyHolderAmong(name, types.listOf(type.layouts));
        return only ? types.forLayout(only) : expected;
    }
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures())
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

static ExpectationForGuesses findExpectationOfWhatArrivesForGuesses(Walker& walker, ValueID parameter)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    BlockID block = index.blockOf(parameter);
    ExpectationForGuesses joined { { }, true };
    bool somethingComesRound = false;
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
            if (arriving.type)
                joined.type = joined.type ? joinOf(types, walker.facts().families(), joined.type, arriving.type) : arriving.type;
        }
    }
    if (somethingComesRound && joined.type && types.at(joined.type).isJavaScriptValue())
        joined.type = types.unionOf(topsOfKindsInLoops(types, types.withoutObjects(joined.type)), types.withOnlyObjects(joined.type));
    if (saysNothingForGuesses(types, joined.type))
        joined = { };
    return joined;
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

static ExpectationForGuesses findExpectationAnewForGuesses(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };

    BodyValue definition = walker.asWalked(index.value(input));
    Opcode opcode = definition.opcode();
    ExpectationForGuesses expectation;
    ValueID behind;
    if (definition.isParameter()) {
        if (index.blockOf(input) == entryBlock) {
            expectation = expectationOfEntryParameterForGuesses(walker, input.index());
            expectation = asWrittenWhereFamilyLacksNamesForGuesses(walker, input, expectation);
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
        ExpectationForGuesses ofReceiver { { }, true };
        std::optional<uint8_t> receiver = placeOfReceiverForGuesses(opcode);
        if (receiver)
            ofReceiver = findWhatIsProvenOrExpectedForGuesses(walker, definition.operand(*receiver), input);
        if (!receiver || ofReceiver.type) {
            expectation = { typeOfReadAtLevelExpectedForGuesses(walker, definition, ofReceiver.type), ofReceiver.isDeclared, ofReceiver.isHeld };
            if (saysNothingForGuesses(types, expectation.type))
                expectation = { typeOfReadByWhatIsHeldForGuesses(walker, definition, ofReceiver.type), ofReceiver.isDeclared, true };
        }
        if (opcode == Opcode::ElemGet && saysNothingForGuesses(types, expectation.type))
            expectation = { walker.facts().held().resultOfReadByKey(walker.currentFunction(), input), true, true };
    } else if (opcode == Opcode::ResultAt) {
        if (ValueID iterable = iterableThatStepReadsForGuesses(index, definition)) {
            ExpectationForGuesses ofIterable = findWhatIsProvenOrExpectedForGuesses(walker, iterable, input);
            if (!ofIterable.type)
                ofIterable = { partHeldUnderNameThatLoopWalksUnseen(walker, iterable), false, true };
            if (ofIterable.type) {
                TypeID iterated = types.withoutNullish(ofIterable.type);
                TypeID pairs = typeOfPairsThatStepsHandOut(walker, iterated);
                if (types.at(iterated).isAnyString())
                    expectation = { TypeTable::string, ofIterable.isDeclared, ofIterable.isHeld };
                else
                    expectation = { pairs ? pairs : typeOfContentsRead(expecting, iterated, ContentsSlot::Element), ofIterable.isDeclared, ofIterable.isHeld };
            }
            if (saysNothingForGuesses(types, expectation.type))
                expectation = { walker.facts().held().resultOfReadByKey(walker.currentFunction(), definition.operand(0)), true, true };
        }
    }

    if (!saysNothingForGuesses(types, expectation.type)) {
        expectation.type = narrowedByNamesUsedForGuesses(walker, input, partThatOneTestTellsForGuesses(types, expectation.type));
        if (expectation.type && types.at(expectation.type).isUndefinedOrNull()) {
            if (ExpectationForGuesses byNames = expectationByNamesUsedForGuesses(walker, input); byNames.type)
                return byNames;
        }
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
    for (uint64_t cast : walker.castsOf(input)) {
        DeclaredType declared = index.value(ValueID(static_cast<uint32_t>(cast))).as<Opcode::Convert>().declared();
        ExpectationForGuesses stated { targetOfConvert(expecting, declared), !declared.isExpected() };
        if (saysNothingForGuesses(types, stated.type) || !types.at(stated.type).hasObjects() || !admitsNoNumberOrBooleanForGuesses(types, stated.type))
            continue;
        stated.type = narrowedByNamesUsedForGuesses(walker, input, partThatOneTestTellsForGuesses(types, stated.type));
        return stated;
    }
    return expectationByNamesUsedForGuesses(walker, input);
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

static ExpectationForGuesses findExpectationForGuesses(Walker& walker, ValueID input)
{
    Walker::RememberedExpectation& remembered = walker.rememberedExpectationOf(input);
    if (remembered.isKnown)
        return { remembered.type, remembered.isDeclared, remembered.isHeld };
    ExpectationForGuesses expectation = findExpectationAnewForGuesses(walker, input);
    refineExpectationOfValue(walker, input, expectation.type, expectation.isDeclared);
    if (walker.ignoresExpectedTypes() || walker.shufflesExpectedTypes()) [[unlikely]] {
        TypeID found = expectation.type;
        expectation = expectationTakenInPlaceOfWhatIsFoundForGuesses(walker, input, expectation);
        if (expectation.type != found)
            walker.count(StatisticID::ExpectedTypesTakenOtherwise);
    }
    if (expectation.type && !holdsOnlyPrimitives(expectation.type) && walker.types().recordOf(expectation.type).traitsOfNamed.contains(ObjectTrait::IsClosure)) {
        bool mayBeTestedFor = true;
        forEachFunctionOf(walker.types(), walker.types().at(expectation.type), scopedLambda<void(FunctionID)>([&](FunctionID named) {
            mayBeTestedFor = mayBeTestedFor && walker.facts().linked().mayBeMentionedBy(walker.unit(), named);
        }));
        if (!mayBeTestedFor)
            expectation = { };
    }
    remembered = { expectation.type, expectation.isDeclared, expectation.isHeld, true };
    if (Vector<ValueID, 16>* rememberedForLook = walker.expectationsRememberedForLook())
        rememberedForLook->append(input);
    return expectation;
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

PropertyKeyID nameUnderWhichValueIsRead(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(input);
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    return index.value(read).key();
}

TypeID typeHeldUnderName(Walker& walker, PropertyKeyID name)
{
    constexpr uint8_t question = static_cast<uint8_t>(AnswerKeptInArena::TypeHeldUnderName);
    TypeArena& types = walker.types();
    bool isRemembered = types.keeps() == ArenaKeeps::UntilReset;
    TypeArena::SetOfRound asked { name.bits(), isRemembered && types.table().sharedAnswers() && !liesInThread(name) };
    if (std::optional<TypeArena::WordsOfRound> kept = isRemembered ? types.rememberedRound(question, 1, asked, 0) : std::nullopt)
        return TypeID::fromBits((*kept)[0]);

    Vector<LayoutID, 8> holders;
    walker.facts().layoutsByNames().appendHolders(name, holders);
    TypingContext holding { walker.program(), types, walker.heldPositions(), TypeLevel::Proven };
    TypeID held;
    for (LayoutID holder : holders) {
        TypeID underName = tryTypeOfFieldRead(holding, types.forLayout(holder), name);
        if (underName && !saysNothingForGuesses(types, underName))
            held = held ? types.unionOf(held, underName) : underName;
    }
    if (isRemembered)
        types.rememberRound(question, 1, held ? 1 : 0, asked, 0, { held.bits() });
    return held;
}

ExpectationOfValue findExpectation(Walker& walker, ValueID input)
{
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, input);
    return { expectation.type, expectation.isDeclared };
}

static FunctionID onlyFunctionBesideBuiltinsForGuesses(const TypeArena& types, const Type& type)
{
    if (!type.isObject() || !type.isJavaScriptValue() || !type.hasOnlyNamedObjects() || type.hasOnlyClosures())
        return { };
    const ObjectTable& objects = types.objects();
    FunctionID only;
    bool namesNothingElse = TypeReader(types).at(type.objects).allOf([&](ObjectID object) {
        FunctionID function = objects.closureFunction(object);
        if (!function)
            return !!objects.builtinOf(object);
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
    if (expectation.type || !allNumericOperators.contains(operation.opcode()))
        return expectation;
    TypeArena& types = walker.types();
    Type held = types.at(provenTypeOfInputForGuesses(walker, operand));
    if (!held.hasOnlyPrimitives(Primitive::Number | Primitive::BigInt) || !(held.primitives & Primitive::Number) || !(held.primitives & Primitive::BigInt))
        return expectation;
    bool standsBesideNumber = false;
    operation.forEachOperand([&](ValueID other, uint32_t) {
        standsBesideNumber |= other != operand && types.at(provenTypeOfInputForGuesses(walker, other)).isNumber();
    });
    return standsBesideNumber ? ExpectationForGuesses { TypeTable::number, false } : expectation;
}

static ExpectationForGuesses expectationOfCalleeForGuesses(Walker& walker, const BodyValue& call, uint32_t operandIndex)
{
    if (!walker.has(Mechanism::DirectCalls) || !call.operandUses(operandIndex).contains(OperandUse::Calls))
        return { };
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        return { walker.types().forFunction(callee), false };
    if (constructionsOfParentForGuesses.contains(call.opcode())) {
        LayoutID above = walker.program().at(call.layout()).base;
        FunctionID constructor = above ? walker.program().at(above).constructor : FunctionID();
        if (!constructor)
            return { };
        TypeID returned = walker.plan().typeHeldOfResult(walker.program(), walker.facts().linked(), constructor);
        if (isClassConstructorOf(walker.program().at(constructor).kind) && (!returned || !walker.types().isSubset(returned, TypeTable::object)))
            return { };
        return { walker.types().forFunction(constructor), true };
    }
    if (FunctionID callee = walker.facts().held().calleeAt(walker.currentFunction(), call.id()))
        return { walker.types().forFunction(callee), true, true };

    if (Reference value = walker.referenceOf(call.operand(operandIndex)); value && value.isValue()) {
        if (FunctionID callee = onlyFunctionBesideBuiltinsForGuesses(walker.types(), walker.types().at(walker.typeOf(value))); callee && walker.facts().linked().mayBeMentionedBy(walker.unit(), callee))
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
        if (!codesAddresses && !hasCodeInLine)
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

static bool standsBehindTestOfGuess(const Code& code, Reference value, const TestOfGuess& chosen)
{
    const Operation& narrowed = code.at(value.id());
    if (narrowed.opcode != Opcode::Narrowed || !narrowed.testOperand())
        return false;
    const Operation& test = code.at(narrowed.testOperand());
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

    bool namesClosures = !holdsOnlyPrimitives(chosen.tested) && types.recordOf(chosen.tested).traitsOfNamed.contains(ObjectTrait::IsClosure);
    if (namesClosures && walker.withoutFunctionsThatMayNotBeMentioned(chosen.tested) != chosen.tested)
        return std::nullopt;
    TypeID proven = walker.typeOf(value);
    if (types.isSubset(proven, chosen.tested) || !types.overlap(proven, chosen.tested) || standsBehindTestOfGuess(walker.code(), value, chosen))
        return std::nullopt;
    return chosen;
}

static TypeID partWhoseClassesHaveGuardsForGuesses(Walker& walker, TypeID expected)
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
        size_t numberInUse = std::ranges::count_if(layouts, [&](LayoutID layout) {
            return index.isInUse(layout);
        });
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

static bool mustBeNamedByDefinitionForGuesses(const BodyIndex& index, ValueID value, const std::optional<BodyUser>& user)
{
    if (!user)
        return false;
    switch (user->kind) {
    case BodyUseKind::Operand:
        return namesByDefinitionForGuesses(index.value(user->value), value) || isWrittenListThatPatternTakesApartForGuesses(index, value, index.value(user->value));
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

static bool isReturnedAtOnceForGuesses(const BodyIndex& index, ValueID call, const std::optional<BodyUser>& user)
{
    if (!user || user->kind != BodyUseKind::TerminatorOperand)
        return false;
    BodyBlock block = index.block(user->block);
    BodyTerminator end = block.terminator();
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call && block.holds(call);
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

static bool isReadOfMethodForItsCallForGuesses(const BodyIndex& index, const BodyValue& read)
{
    if (read.opcode() != Opcode::PropGet)
        return false;
    ValueID user = index.onlyUsingValue(read.id());
    return user && callsOfValuesForGuesses.contains(index.opcodeOf(user)) && !constructionsOfParentForGuesses.contains(index.opcodeOf(user)) && index.value(user).calleeOperand() == read.id();
}

static bool callOfMethodIsHandedClosureMadeForItForGuesses(const BodyIndex& index, const BodyValue& read)
{
    if (!isReadOfMethodForItsCallForGuesses(index, read))
        return false;
    BodyValue call = index.value(index.onlyUsingValue(read.id()));
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
        if (isReadOfMethodForItsCallForGuesses(index, operation)) {
            ValueID call = index.onlyUsingValue(operation.id());
            if (block.holds(call) && call.index() - block.firstValue().index() < limit && (!callAwaited || callAwaited < call))
                callAwaited = call;
        }
        if (opcode != Opcode::RequireObjectCoercible && !isReadOfMethodForItsCallForGuesses(index, operation))
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


    bool slotsAreAsFound = true;
    Vector<std::pair<ValueID, PropertyKeyID>, 8> slotsStoredInStretch;
    Vector<std::pair<ValueID, ValueID>, 8> readsAnsweredByLooks;
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
        bool mayCallBuiltin = takesWhatRestsOnProof && callsOfValuesForGuesses.contains(opcode) && walker.has(Mechanism::DirectCalls) && walker.has(Mechanism::BuiltinCalls);
        bool codeOfRowStandsInLine = false;
        BuiltinCallAskedAbout callOfBuiltinInTypedRun = mayCallBuiltin ? wouldCallBuiltin(walker, operation, inputsOfTypedRun, &codeOfRowStandsInLine) : BuiltinCallAskedAbout::None;
        if (takesWhatRestsOnProof) {
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (isOnlyHandedOnForGuesses(operation, operandIndex) && callOfBuiltinInTypedRun != BuiltinCallAskedAbout::ByOneRow)
                    return;
                if (restsOnProofOfGuess(operand))
                    leans = true;
                else if (saysNothingForGuesses(types, typeInTypedRun(operand, operandIndex)))
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
        bool isTypedEitherWay = !leans && operationsWhoseTypedFormsRunNoCodeForGuesses.contains(opcode) && typedFormForGuesses(walker, operation, inputsOfClosedArm) == TypedForm::Built;
        bool endsNoProofOfPrimitive = callAwaited && operation.id() < callAwaited && operationsThatEndNoProofOfPrimitiveForGuesses.contains(opcode) && !types.at(chosen.tested).hasObjects();
        bool appendsElementsOfList = !leans && isSpreadOfListAsMade && callAwaited && operation.id() < callAwaited;
        bool standsInFrontOfCallOfMethodFound = callOfMethodFound && operation.id() < callOfMethodFound && !isReadOfMethodForItsCallForGuesses(index, operation);
        if (!leans && !isTypedEitherWay && !endsNoProofOfPrimitive && !appendsElementsOfList && !standsInFrontOfCallOfMethodFound && mayEndProofOfGuess(opcode))
            break;
        bool readsMethodForItsCall = leans && isReadOfMethodForItsCallForGuesses(index, operation);
        if (readsMethodForItsCall)
            callAwaited = index.onlyUsingValue(operation.id());

        bool isAskedAboutAsItStands = firstAsItStands && place == begin && typeBehindTest == typeOfValue && typesOfGuessesInside.isEmpty() && firstAsItStands->words == operation.words();

        size_t numberOfTestsOfStoredValuesInFront = testsOfStoredValues.size();
        if (leans && storesIntoSlotsForGuesses.contains(opcode) && operation.valueOperand().index() < first && walker.referenceOf(operation.valueOperand()).isValue()) {
            if (TypeID asked = isAskedAboutAsItStands ? firstAsItStands->askedOfStoredValue : typeAskedOfStoredValueForGuesses(walker, operation, typeInTypedRun(operation.objectOperand(), 0), typeInTypedRun(operation.valueOperand(), 0)))
                testsOfStoredValues.append({ operation.valueOperand(), asked });
        }

        bool counts = false;
        bool countsForTwo = false;
        bool goesWithWhatItTakes = false;
        bool runsNoCode = false;
        TypeID foundOnObjectAsMade;
        TypeID methodFoundBehindLooks;
        TypeID foundInFilledSlot;
        TypeID givenByTypedForm;
        if (leans) {
            walker.takeFormAskedAhead();
            bool formIsAskedAsItStands = isAskedAboutAsItStands && (testsOfStoredValues.isEmpty() ? !firstAsItStands->askedOfStoredValue : !namesValueOfGuess(operation.valueOperand()));
            TypedForm form = formIsAskedAsItStands ? firstAsItStands->form : typedFormForGuesses(walker, operation, inputsOfTypedRun, mayCallBuiltin ? std::optional(callOfBuiltinInTypedRun) : std::nullopt);
            Walker::FormAskedAhead askedAhead = formIsAskedAsItStands ? firstAsItStands->askedAhead : walker.takeFormAskedAhead();
            if (askedAhead.level == KnowledgeLevel::Expected && !walker.isInLoop())
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
                goesWithWhatItTakes = !counts && form == TypedForm::Built && familyOf(opcode) == OperationFamily::Operator && opcode != Opcode::ToString;
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
                for (auto [read, look] : readsAnsweredByLooks) {
                    if (read == object)
                        holder = look;
                }
                bool isAtHand = object.index() < first ? namesValueOfGuess(object) : holder && form == TypedForm::BuiltBehindFilledSlot;
                counts = slotsAreAsFound && isAtHand;
                PropertyKeyID key = keyOfAccessToSlot(walker.program(), operation);
                bool isStoredInStretch = slotsStoredInStretch.contains(std::pair { holder, key });
                size_t placeOfLook = stretch.firstAccessesToSlots.findIf([&](const LookAtSlotOfGuess& earlier) {
                    return earlier.holder == holder && keyOfAccessToSlot(walker.program(), walker.asWalked(index.value(earlier.access))) == key;
                });
                bool isLookedAtAlready = form == TypedForm::BuiltBehindLookAtCell || (form != TypedForm::BuiltBehindEmptySlot && isStoredInStretch) || placeOfLook != notFound;
                if (!isLookedAtAlready && storesIntoSlotsForGuesses.contains(opcode) && looksAtSlotOfObjectBeingMade(walker, operation))
                    counts = false;
                if (counts && !isLookedAtAlready)
                    stretch.firstAccessesToSlots.append({ operation.id(), holder });
                if (counts && (form == TypedForm::BuiltBehindLookAtCell || form == TypedForm::BuiltBehindFilledSlotAndLookAtCell))
                    stretch.storesThatLookAtCell.append(operation.id());
                if (counts && form == TypedForm::BuiltBehindLookAtCell)
                    methodFoundBehindLooks = typeOfMethodFoundBehindLooks(walker, operation, typeBehindTest);
                if (counts && storesIntoSlotsForGuesses.contains(opcode))
                    slotsStoredInStretch.append({ holder, key });
                if (counts && form == TypedForm::BuiltBehindFilledSlot && readsOutOfSlotsForGuesses.contains(opcode) && !isStoredInStretch) {
                    readsAnsweredByLooks.append({ operation.id(), placeOfLook != notFound ? stretch.firstAccessesToSlots[placeOfLook].access : operation.id() });
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
        if (!counts)
            testsOfStoredValues.shrink(numberOfTestsOfStoredValuesInFront);
        if (place == begin && !counts)
            return stretch;
        if (mayEndProofOfGuess(opcode) && !(counts && runsNoCode) && !isTypedEitherWay)
            slotsAreAsFound = false;

        bool isReadInStretch = firstUser && (firstUser->kind == BodyUseKind::Operand ? firstUser->value.index() - first < limit - begin : !index.hasOneUse(operation.id()));
        std::optional<Draft> draftOfRow;
        TypeID inClosedArm = isReadInStretch ? typeByRowForGuesses(typing, operation, typeInClosedArm, draftOfRow) : TypeID();
        TypeID inTypedRun = isReadInStretch && takesWhatRestsOnProof ? typeByRowForGuesses(typing, operation, typeInTypedRun, draftOfRow) : inClosedArm;
        if (foundOnObjectAsMade)
            inTypedRun = foundOnObjectAsMade;
        if (methodFoundBehindLooks)
            inTypedRun = methodFoundBehindLooks;
        if (foundInFilledSlot)
            inTypedRun = foundInFilledSlot;
        if (givenByTypedForm)
            inTypedRun = givenByTypedForm;
        typesInClosedArm.append(inClosedArm);
        typesInTypedRun.append(inTypedRun);
        if (inTypedRun && inTypedRun != inClosedArm && !saysNothingForGuesses(types, inTypedRun))
            knownBetter.set(place - begin);
        if (isReadInStretch && takesWhatRestsOnProof && !saysNothingForGuesses(types, inTypedRun) && !types.isSubset(inClosedArm, inTypedRun))
            restsOnProof.set(place - begin);
        if (opcode == Opcode::RequireObjectCoercible)
            counts = false;
        if (counts && readsMethodForItsCall && inTypedRun && types.at(inTypedRun).isObject() && types.at(inTypedRun).hasOnlyClosures()) {
            ValueID call = index.onlyUsingValue(operation.id());
            if (block.holds(call) && call.index() - block.firstValue().index() < limit && (!callOfMethodFound || callOfMethodFound < call))
                callOfMethodFound = call;
        }
        if (counts && !readsMethodForItsCall) {
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
    }

    if (chosen.asksForElementOfNumbers)
        stretch.placesThatRestOnProof = WTF::move(restsOnProof);
    return stretch;
}

static TestOfGuess secondTestOfGuess(const TestOfGuess& first)
{
    TestOfGuess second { first.failingSideAsksFor, { } };
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
    bool isInBounds = positionIsKnownToLieInsideList(walker, list, position);
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
        isPresent.setIsInBounds(isInBounds);
        Reference test = walker.build(WTF::move(isPresent));
        BlockID found = walker.addBlock();
        walker.branch(test, found, closedBlock);
        walker.continueIn(found);

        Draft element(Opcode::GetArrayElement, origin);
        walker.chooseConservativeFields(element);
        element.setArrayOperand(list);
        element.setPositionOperand(position);
        element.setArrayStorage(kind);
        element.setLevel(KnowledgeLevel::Proven);
        element.setKeyIsIntegral(true);
        element.setIsPresent(true);
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
    bool takesInBranch = !failingWayBeginsTurnAgain && armsOfGuessTakeBranch(walker, inputBlock, begin, end, chosen.isPassedAlready ? Reference() : value, stretch.placesThatTypedRunKnowsBetter, branchAskedAbout);
    bool failingWayLeavesCopy = !failingWayBeginsTurnAgain && !(takesInBranch && walker.isInLoop()) && chosen.admitsAllThatIsExpected && !chosen.failingSideAsksFor && !chosen.failingSideAsksForNothing && !chosen.failingSideIsAsLikely && walker.mayLeaveCopyForCall();
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
            return look.access == subject && !look.holder;
        });
        TypeID found = isLoadedByLook ? typeFoundInFilledSlot(walker, walker.asWalked(index.value(subject)), walker.types().withoutNullish(walker.types().intersectionOf(walker.typeOf(value), chosen.tested))) : TypeID();
        if (found && !isNeverType(found) && walker.types().at(found).isInt32() && isTakenBySwitchAloneForGuesses(walker, inputBlock, subject, end))
            subjectOfSwitch = subject;
    }

    Origin origin = originInFrontOfOperationForGuesses(walker, index.value(ValueID(first)));
    BlockID closedBlock;
    if (failingWayLeavesCopy)
        closedBlock = walker.blockWhereCopyLeavesForCall();
    Reference lastTest;
    unsigned numberOfTests = 0;
    bool lastTestIsAskedInFront = false;
    auto goOnWhereTestPasses = [&](Reference test, bool passesWithYes = true) {
        lastTest = passesWithYes ? test : Reference();
        ++numberOfTests;
        lastTestIsAskedInFront = failingWayBeginsTurnAgain && walker.branchInFrontOfTypedCopy(test, passesWithYes);
        if (lastTestIsAskedInFront)
            return;
        BlockID passed = walker.addBlock();
        if (!closedBlock)
            closedBlock = chosen.failingSideIsAsLikely || (chosen.failingSideAsksForNothing && !walker.isInLoop()) ? walker.addBlock() : walker.addBlock(BlockFrequency::Rare);
        walker.branch(test, passesWithYes ? passed : closedBlock, passesWithYes ? closedBlock : passed);
        walker.continueIn(passed);
    };

    auto buildTestWhereItMayStand = [&](Reference tested, const TestOfGuess& asked, bool refusesMarkedObjects = false) {
        Walker::InFrontOfTypedCopy inFront(walker, failingWayBeginsTurnAgain && walker.standsInFrontOfTypedCopy(tested));
        return buildTestOfGuess(walker, tested, asked, origin, refusesMarkedObjects);
    };
    auto buildProofWhereTestWasAsked = [&](Reference tested, Reference test, const TestOfGuess& asked) {
        Walker::InFrontOfTypedCopy inFront(walker, lastTestIsAskedInFront);
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
        lookAtCellInFrontOfStores(walker, stretch.storesThatLookAtCell.span(), proven, scopedLambda<void(Reference)>([&](Reference test) {
            goOnWhereTestPasses(test);
        }));
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
    auto askWhetherLoadedValueIsThere = [&](Reference loaded) -> std::optional<Reference> {
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
    BlockID blockOfEmptySlot;
    for (const LookAtSlotOfGuess& look : stretch.firstAccessesToSlots) {
        Reference holder = proven;
        if (look.holder) {
            size_t place = loadedByLooks.findIf([&](const auto& loaded) {
                return loaded.first == look.holder;
            });
            holder = place != notFound ? loadedByLooks[place].second : Reference();
            while (Reference checked = holder ? walker.state().checkedNameOf(holder) : Reference())
                holder = checked;
            std::optional<Reference> rested = askWhetherLoadedValueIsThere(holder);
            if (!rested)
                break;
            holder = *rested;
        }
        bool asksForFilledSlot = index.opcodeOf(look.access) != Opcode::PrivateDefine;
        Reference loaded = holder && asksForFilledSlot && readsOutOfSlotsForGuesses.contains(index.opcodeOf(look.access)) ? valueHeldOfFilledSlot(walker, walker.asWalked(index.value(look.access)), holder) : Reference();
        bool markAnswers = !loaded && numberOfTests && holder && walker.state().factsOfCell(holder).contains(FactOfCell::BearsNoMark) && markOfCellAnswersForRead(walker, walker.asWalked(index.value(look.access)), walker.typeOf(holder));
        Reference test = holder && !loaded && !markAnswers ? buildTestOfSlot(walker, walker.asWalked(index.value(look.access)), holder) : Reference();
        if (!test && !loaded && !markAnswers)
            break;
        if (test)
        if (asksForFilledSlot && (takesInBranch || !walker.isInLoop()) && &look == &stretch.firstAccessesToSlots.last() && !failingWayBeginsTurnAgain && !blockOfEmptySlot && emptySlotIsUndefinedBehindLooks(walker, walker.asWalked(index.value(look.access)), holder)) {
            lastTest = { };
            ++numberOfTests;
            lastTestIsAskedInFront = false;
            BlockID passed = walker.addBlock();
            BlockID empty = walker.addBlock();
            walker.branch(test, passed, empty);
            walker.continueIn(empty);
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
                walker.noteEmptySlotTakenAsCase();
            }
            walker.continueIn(passed);
        } else
            goOnWhereTestPasses(test, asksForFilledSlot);
        if (asksForFilledSlot) {
            if (!loaded) {
                Walker::InFrontOfTypedCopy inFront(walker, test && lastTestIsAskedInFront);
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
    bool asksMembersByTheirBytes = walker.program().admitsReplacedBuiltins && !stretch.someMemberRestsOnByteOfClass;
    if (stretch.looksWhetherValueIsAsMade && proven && !walker.isUnreachable()) {
        Walker::InFrontOfTypedCopy inFront(walker, isPrimitive && failingWayBeginsTurnAgain && walker.turnsOfTypedCopyWriteNothingAboveValues());
        isAsMade = buildTestOfObjectAsMade(walker, proven, origin, asksMembersByTheirBytes ? TestOfObjectAsMade::AsksObjectAlone : TestOfObjectAsMade::AsksWhatWasWrittenAboveToo);
    }
    if (isAsMade) {
        goOnWhereTestPasses(isAsMade);
        if (!isPrimitive)
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
            noteWhatReadOfViewGivesForGuesses(walker, walker.asWalked(index.value(ValueID(first))), proven, buildReadOfElementOfViewBehindTestOfLength(walker, walker.asWalked(index.value(ValueID(first))), proven, origin, closedBlock));
        else
            buildReadOfElementOfNumbersForGuesses(walker, walker.asWalked(index.value(ValueID(first))), proven, origin, closedBlock);
        if (walker.hasFailed())
            return;
    }
    if (!closedBlock && !(failingWayBeginsTurnAgain && numberOfTests))
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
        if (numberOfTests == 1 && !chosen.isPassedAlready)
            bindNamesBehindFailedTest(walker, value, lastTest, chosen.layouts ? OperandIndex::ShapeTest::object : OperandIndex::TypeTest::value, names.span(), origin);
        Reference refused = (chosen.failingSideAsksForNothing || chosen.failingSideAsksFor) && !names.isEmpty() && !walker.isUnreachable() ? walker.referenceOf(names[0]) : Reference();
        if (chosen.failingSideAsksFor && refused && refused.isValue() && walker.types().overlap(walker.typeOf(refused), chosen.failingSideAsksFor) && !walker.types().isSubset(walker.typeOf(refused), chosen.failingSideAsksFor))
            openGuessWithTest(walker, refused, secondTestOfGuess(chosen), inputBlock, begin, StretchOfGuess { end, 0, { } });
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

    for (uint32_t place = 0; place < size; ++place) {
        if (arms.isEmpty() || !valuesMentionedLater.contains(ValueID(first + place)))
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
}

static void buildTruthOfObjectOrNothing(Walker& walker, const BodyValue& operation, Reference value, BlockID inputBlock, uint32_t begin)
{
    Origin origin = originInFrontOfOperationForGuesses(walker, operation);
    auto answerWith = [&](BlockID meetingBlock, bool answer) {
        Draft written(Opcode::Boolean, origin);
        written.setBoolean(answer);
        if (Reference constant = walker.constant(WTF::move(written)))
            walker.jumpTo(meetingBlock, singleElementSpan(constant));
    };
    Reference isObject = buildTestOfGuess(walker, value, TestOfGuess { TypeTable::object, { } }, origin);
    if (!isObject)
        return;
    BlockID onTheRest;
    walker.letGuessFailUnasked(onTheRest, origin);
    BlockID onObject = walker.addBlock();
    BlockID onOthers = walker.addBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.noteGuessOpened();
    Reference met = walker.addParameter(meetingBlock, TypeTable::boolean, walker.formOfParameter(TypeTable::boolean, { }));
    walker.branch(isObject, onObject, onOthers);
    walker.continueIn(onObject);
    answerWith(meetingBlock, true);

    walker.continueIn(onOthers);
    Reference isNothing = buildTestOfGuess(walker, value, TestOfGuess { TypeTable::nullish, { } }, origin);
    if (!isNothing)
        return;
    BlockID onNothing = walker.addBlock();
    if (!onTheRest)
        onTheRest = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isNothing, onNothing, onTheRest);
    walker.continueIn(onNothing);
    answerWith(meetingBlock, false);

    walker.continueIn(onTheRest);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(inputBlock, begin, begin + 1);
        if (walker.hasFailed())
            return;
        Reference closed = walker.referenceOf(operation.id());
        if (!walker.isUnreachable())
            walker.jumpTo(meetingBlock, singleElementSpan(closed));
    }
    walker.continueIn(meetingBlock);
    walker.bind(operation.id(), met);
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

static bool testOfGuessStaysOperationOfItsOwn(const Walker& walker, Reference value)
{
    if (!walker.isInLoop())
        return false;
    std::optional<uint32_t> innermost = walker.innermostOpenLoop();
    return !innermost || walker.standsInFrontOfOpenLoop(*innermost, value) || walker.mayBeginTurnAgainInClosedCopy();
}

static bool buildLoneReadOfGuess(Walker& walker, const BodyValue& operation, Reference value, const TestOfGuess& chosen, BlockID inputBlock, uint32_t begin, const StretchOfGuess& stretch, BranchOfGuessAskedAbout& branchAskedAbout)
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

static bool buildLoneStoreOfGuess(Walker& walker, const BodyValue& operation, Reference value, const TestOfGuess& chosen, BlockID inputBlock, uint32_t begin, const StretchOfGuess& stretch, BranchOfGuessAskedAbout& branchAskedAbout)
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
        if (index.isParameter(source) || index.opcodeOf(source) != Opcode::GetModuleVariable)
            return false;
        const FactTables::FunctionGivenToVariable* given = walker.facts().findFunctionGivenTo(index.value(source).as<Opcode::GetModuleVariable>().variable());
        if (!given)
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

static bool nothingIsExpectedOfOperandsForGuesses(Walker& walker, const BodyValue& operation, ExpectationForGuesses& firstExpected, uint32_t& indexOfFirstExpected)
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

static bool isReadOfVariableForGuesses(const BodyIndex& index, ValueID value)
{
    value = index.valueBehindCasts(value);
    while (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::CheckInitialized)
        value = index.valueBehindCasts(index.value(value).operand(0));
    return value && !index.isParameter(value) && (index.opcodeOf(value) == Opcode::GetEnvironmentField || index.opcodeOf(value) == Opcode::GetModuleVariable);
}

static bool testAdmitsAllThatIsExpectedForGuesses(Walker& walker, const BodyValue& operation, ValueID operand, uint32_t operandIndex, TypeID expected, TypeID tested)
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

bool openGuessAtElementOfNumbersOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !walker.has(Mechanism::Folding))
        return false;
    Reference list = walker.referenceOf(read.objectOperand());
    Reference position = walker.referenceOf(read.propertyOperand());
    if (!list || !list.isValue() || !position)
        return false;
    TypeID typeOfList = walker.typeOf(list);
    TypeID elementInsideView = walker.isInLoop() && !walker.testsEveryPosition() && walker.mayBeginTurnAgainInClosedCopy() ? typeOfElementInsideViewOfNumbers(walker, read, walker.types().withoutNullish(typeOfList), walker.typeOf(position)) : TypeID();
    if (elementInsideView && walker.types().at(typeOfList).mayBeUndefinedOrNull()) {
        if (splitNullishOffReceiverOfAccess(walker, read))
            return true;
        list = walker.referenceOf(read.objectOperand());
        typeOfList = walker.typeOf(list);
        if (!list.isValue() || walker.types().at(typeOfList).mayBeUndefinedOrNull())
            return false;
    }
    if (elementInsideView && (positionIsKnownToLieInsideList(walker, list, position) || walker.counterHoldsAccessBelowRoomOfView(read.id(), list)))
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
    TestOfGuess chosen { typeOfList, { }, true };
    chosen.asksForElementOfNumbers = true;
    chosen.elementInsideView = elementInsideView;
    chosen.admitsAllThatIsExpected = !!elementInsideView;
    StretchOfGuess stretch = elementInsideView ? StretchOfGuess { begin + 1, least, { } } : findStretchOfGuess(walker, list, chosen, block, begin, limit);
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

static bool takeApartCallThatFindsBuiltinForGuesses(Walker& walker, const BodyValue& read)
{
    if (!walker.facts().held().callersThatFindBuiltins.get(walker.currentFunction().index()) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::DirectCalls))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!isReadOfMethodForItsCallForGuesses(index, read))
        return false;
    ValueID call = index.onlyUsingValue(read.id());
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

static bool askGuardOfNameReadOffConstructorFunction(Walker& walker, const BodyValue& read)
{
    Reference function = walker.has(Mechanism::TypedOperations) && !read.isFreeName() ? walker.referenceOf(read.objectOperand()) : Reference();
    if (!function || !function.isRecord() || !readOffConstructorFunctionAwaitsGuard(walker, read, walker.typeOf(function)))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    ValueID call = index.onlyUsingValue(read.id());
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

static bool nameMayOpenGuessOfKeyOrdersForGuesses(const Walker& walker, const BodyValue& access)
{
    if (access.isFreeName())
        return false;
    PropertyKeyID name = access.key();
    if (!name || liesInThread(name))
        return false;
    const LinkTables& linked = walker.facts().linked();
    return linked.keysOfAccessorsExpectedInFields.get(name.index()) || linked.keysOfStaticFields.get(name.index());
}

constinit const OpcodeSet opcodesThatOpenGuessAtTakes = operationsThatOpenGuesses;
static_assert(opcodesThatOpenGuessAtTakesInChains == operationsThatOpenGuesses);

GuessOpened openGuessAtOfItsOpcodes(Walker& walker, const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (!operationsThatOpenGuesses.contains(opcode))
        return GuessOpened::None;
    if (guessThatOperandBesideStringHasText(walker, operation))
        return GuessOpened::WithArms;
    bool isInLoop = walker.isInLoop();
    if (noGuessIsOpenedWhereWalkStandsForGuesses(walker, isInLoop))
        return GuessOpened::None;
    if (opcode == Opcode::Call && (forwardCallBehindTestOfCallee(walker, operation) || openGuessAtCalleeOfReturnedCall(walker, operation)))
        return GuessOpened::WithArms;
    if (Reference holder = opcode == Opcode::PropGet && walker.program().admitsReplacedBuiltins ? walker.referenceOf(operation.objectOperand()) : Reference(); holder && findMemberAskedByItsByteOffType(walker, walker.typeOf(holder), operation.key()))
        return askBytesOfMembersReadOffBuiltins(walker, operation, holder) ? GuessOpened::WithArms : GuessOpened::None;
    if (opcodesThatOpenGuessOfKeyOrdersTakes.contains(opcode) && nameMayOpenGuessOfKeyOrdersForGuesses(walker, operation) && openGuessOfKeyOrders(walker, operation))
        return GuessOpened::WithArms;
    if (opcode == Opcode::PropGet && (callGetterExpectedAtRead(walker, operation) || (isInLoop && callMethodsBehindTestsOfShape(walker, operation)) || takeApartCallThatFindsBuiltinForGuesses(walker, operation) || askGuardOfNameReadOffConstructorFunction(walker, operation)))
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
    if (begin >= limit)
        return GuessOpened::None;

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
            if (isUnderPrivateName && operand == object)
                expectation = expectationOfHolderOfPrivateNameForGuesses(walker, operation, expectation);
            else if (operand == object && expectation.type && holdsLayoutsBesideOtherObjectsForGuesses(walker.types().at(expectation.type)))
                expectation.type = narrowedByNameAskedForGuesses(walker, expectation.type, operation.key());
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
        if (least > 1 && formAsItStands == TypedForm::BuiltOnObjectAsMade && readsMethodForItsCall) {
            Type receiver = walker.types().at(walker.types().withoutNullish(expectation.type));
            const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), receiver);
            if (!receiver.hasObjects() || (onlyClass && onlyClass->builtinClass == BuiltinClasses::Array))
                least = 1;
        }
        bool callIsLoopOverTable = false;
        if (readsMethodForItsCall && expectation.isDeclared && walker.has(Mechanism::Guesses) && walker.has(Mechanism::Inlining) && callOfMethodIsHandedClosureMadeForItForGuesses(index, operation)) {
            const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), walker.types().at(walker.types().withoutNullish(expectation.type)));
            ValueID call = index.onlyUsingValue(operation.id());
            uint32_t firstValue = block.firstValue().index();
            bool stretchReachesCall = block.holds(call) && call.index() - firstValue < limit;
            for (uint32_t place = begin; stretchReachesCall && firstValue + place <= call.index(); ++place)
                stretchReachesCall = !standsInNoStretchOfGuess(index, walker.asWalked(index.value(ValueID(firstValue + place))));
            callIsLoopOverTable = stretchReachesCall && onlyClass && (onlyClass->builtinClass == BuiltinClasses::Map || onlyClass->builtinClass == BuiltinClasses::Set);
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
        if ((least > 1 || readsMethodForItsCall) && mostOperationsThatStretchCountsForGuesses(walker, value, lacksOnlyFilledSlot, block, begin, limit, least) < least)
            return buildAsOneOperationOrLeaveClosed();
        if (!chosen)
            chosen = chooseTestOfGuess(walker, value, expectation.type);
        StretchOfGuess stretch = chosen ? findStretchOfGuess(walker, value, *chosen, block, begin, limit, lacksOnlyFilledSlot ? &asItStands : nullptr) : StretchOfGuess();
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
        if (stretch.numberOfTypedOperations < least)
            return buildAsOneOperationOrLeaveClosed();
        chosen->admitsAllThatIsExpected = testAdmitsAllThatIsExpectedForGuesses(walker, operation, operand, operandIndex, expectation.type, chosen->tested);
        if (const LinkedCallThatFindsBuiltin* found = callsOfValuesForGuesses.contains(operation.opcode()) ? walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), operation.id()) : nullptr) {
            if (found->callee && chosen->tested == walker.types().forFunction(found->callee) && operand == operation.calleeOperand() && isTestableBuiltin(walker.program().builtins(), found->builtin)) {
                chosen->failingSideAsksFor = walker.types().forBuiltin(found->builtin);
                chosen->failingSideIsAsLikely = true;
            }
            stretch.end = std::max(stretch.end, endOfWhatTakesResultForGuesses(walker, block, begin, limit));
        }
        BranchOfGuessAskedAbout branchAskedAbout;
        if (opcode == Opcode::ToBoolean && !chosen->isPassedAlready && stretch.end == begin + 1 && walker.types().at(chosen->tested).isObject()) {
            if (chosen->layouts && !armsOfGuessTakeBranch(walker, block.id(), begin, stretch.end, value, stretch.placesThatTypedRunKnowsBetter, &branchAskedAbout) && !standsBehindTestOfGuess(walker.code(), value, TestOfGuess { TypeTable::object, { } })) {
                buildTruthOfObjectOrNothing(walker, operation, value, block.id(), begin);
                opened = GuessOpened::WithArms;
                return;
            }
            chosen->failingSideAsksForNothing = true;
        }
        if (!(chosen->admitsAllThatIsExpected && (walker.mayBeginTurnAgainInClosedCopy() || walker.mayLeaveCopyForCall())) && (buildLoneReadOfGuess(walker, operation, value, *chosen, block.id(), begin, stretch, branchAskedAbout) || buildLoneStoreOfGuess(walker, operation, value, *chosen, block.id(), begin, stretch, branchAskedAbout))) {
            opened = GuessOpened::AsOneOperation;
            return;
        }
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
        if (!chosen || chosen->failingSideAsksFor || !testAdmitsAllThatIsExpectedForGuesses(walker, operation, operand, operandIndex, expectation.type, chosen->tested) || walker.withoutFunctionsThatMayNotBeMentioned(chosen->tested) != chosen->tested)
            return;
        TypeID held = typesOfInputs(operand);
        if (!held || types.isSubset(held, chosen->tested) || !types.overlap(held, chosen->tested))
            return;
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

void proveExpectedTypeOrBeginTurnAgain(Walker& walker, ValueID input, TypeID expected, Origin origin)
{
    Reference value = walker.referenceOf(input);
    std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, value, expected);
    if (!chosen)
        return;
    origin.isTailCall = false;
    Type known = walker.types().at(walker.typeOf(value));
    bool refusesMarkedObjects = chosen->layouts && !isBeingMadeByBodyBeingRead(walker, input) && (!known.hasOnlyLayouts() || known.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow));
    uint32_t firstNewValue = walker.code().numberOfValues();
    BlockID blockOfTest = walker.currentBlock();
    Reference test;
    {
        Walker::InFrontOfTypedCopy inFront(walker, walker.standsInFrontOfTypedCopy(value));
        test = buildTestOfGuess(walker, value, *chosen, origin, refusesMarkedObjects);
    }
    if (!test)
        return;
    bool isAskedHere = test.isValue() && test.id().index() >= firstNewValue && walker.code().blockOf(test.id()) == blockOfTest;
    bool isAskedInFront = walker.branchInFrontOfTypedCopy(test, true);
    if (!isAskedInFront) {
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
        walker.branch(test, passed, failed);
        if (walker.failsGuesses()) [[unlikely]] {
            walker.continueIn(passed);
            walker.letGuessFailUnasked(failed, origin);
            passed = walker.currentBlock();
        }
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

bool guessesMayTypeOperationInCleanTurn(Walker& walker, const BodyValue& operation, const TypesOfInputs& typesOfInputs)
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
        return (callee && !saysNothingForGuesses(walker.types(), callee)) || !!expectationOfCalleeForGuesses(walker, operation, OperandIndex::Call::callee).type;
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
    std::optional<Effects> effects = effectsOfTypedFormAhead(walker, operation, TypesOfInputs(typeBehindTests), &halfRunBehindLooks);
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

static bool listConditionsOfPlainInstanceForGuesses(const Walker& walker, LayoutID layout, Vector<MemberConditionRecord, 16>& conditions)
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
    for (PropertyKeyID name : names) {
        if (std::optional<FieldIndex> field = plan.slots().findField(program, layout, name); field && plan.slot(layout, *field).holdsAccessor)
            return false;
    }

    auto isOfLibrary = [&](FunctionID function) {
        return function && isLibraryCode(program, program.at(function));
    };
    LayoutID classLayout = declaredLayoutOf(program, layout);
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        if (!numberOfClassesLeft-- || program.at(current).kind != LayoutKind::Instance || !plan.hasClassRecord(current) || !facts.isEvaluatedAtMostOnce(current))
            return false;
        for (uint32_t place : facts.placesOfMembersUnder(current, ClassSide::Prototype, { })) {
            if (!isOfLibrary(facts.membersOf(current)[place].function))
                return false;
        }
        for (uint32_t place : link.membersUnder(current, ClassSide::Prototype, { })) {
            if (!link.members[place].everyWriterIsKnown || !isOfLibrary(link.members[place].function))
                return false;
        }
        bool accessorIsOfProgram = false;
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
            return isAccessor;
        });
        if (accessorIsOfProgram)
            return false;
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return false;
        current = parent;
    }
    if (names.isEmpty())
        conditions.append({ MemberConditionKind::PrototypeLinkUnchanged, classLayout, ClassSide::Prototype, { } });
    for (PropertyKeyID name : names)
        conditions.append({ MemberConditionKind::MemberPassedByStores, classLayout, ClassSide::Prototype, name });
    return true;
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
    Draft question(Opcode::FindsDeclaredMember, walker.originOf(operation));
    walker.chooseConservativeFields(question);
    question.setObjectOperand(value);
    question.setKey(operation.key());
    question.setReceivers(receivers);
    walker.bind(operation.id(), walker.build(WTF::move(question)));
    walker.noteGuessOpened();
    return true;
}

bool askCellWhetherItIsPlainInstance(Walker& walker, const BodyValue& operation)
{
    if (!walker.has(Mechanism::TypedOperations) || !operation.constructorOperand())
        return false;
    const Program& program = walker.program();
    Reference value = walker.referenceOf(operation.objectOperand());
    LayoutID classLayout = classNamedByOperandForGuesses(walker, operation.constructorOperand());
    if (!value || !value.isValue() || !classLayout || objectPrototypeMayGainName(program))
        return false;
    Vector<MemberConditionRecord, 16> conditions;
    if (!findConditionsOfOrdinaryInstanceOf(walker, walker.typeOf(walker.referenceOf(operation.constructorOperand())), conditions) || !listConditionsOfPlainInstanceForGuesses(walker, classLayout, conditions))
        return false;
    Vector<LayoutID, 16> taken { classLayout };
    if (operation.opcode() == Opcode::IsPlainHeirOf) {
        for (LayoutID layout : program.layoutsExpectedBelow(classLayout)) {
            if (program.at(layout).kind == LayoutKind::Instance && !program.at(layout).isDerived() && walker.plan().isInUse(layout) && listConditionsOfPlainInstanceForGuesses(walker, layout, conditions))
                taken.append(layout);
        }
    }
    TypeArena& types = walker.types();
    LayoutSetID receivers = types.at(types.forLayouts(taken.span())).layouts;
    if (shapesOfLayoutsForGuesses(walker, receivers) != ShapesOfLayoutsForGuesses::Testable)
        return false;
    Draft question(Opcode::IsPlainInstanceOf, walker.originOf(operation));
    walker.chooseConservativeFields(question);
    question.setObjectOperand(value);
    question.setReceivers(receivers);
    question.setConditions(conditions.span());
    Reference asked = walker.build(WTF::move(question));
    walker.bind(operation.id(), asked);
    if (asked && asked.isValue())
        noteKnown(walker, KnownNote::AnswerOfQuestionStillStands, Reference::value(walker.code().at(asked.id()).objectOperand()), asked, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards }), 1, true);
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
        if (operand && operand.index() < firstValueOfLoop && !mentioned.contains(operand))
            mentioned.append(operand);
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mention(operand);
            });
            if (!operationsThatOpenGuesses.contains(operation.opcode()) || isReadOfMethodForItsCallForGuesses(index, operation))
                continue;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                ValueID inFront = valueBehindCastsOfLoopForGuesses(index, operand, firstValueOfLoop);
                if (inFront.index() >= firstValueOfLoop || isOnlyHandedOnForGuesses(operation, operandIndex))
                    return;
                Reference value = walker.referenceOf(inFront);
                bool isListed = guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
                    return guess.value == value;
                });
                if (!value || !value.isValue() || isListed)
                    return;

                ExpectationForGuesses expectation = callsOfValuesForGuesses.contains(operation.opcode())
                    ? expectationOfCalleeForGuesses(walker, operation, operandIndex)
                    : findExpectationForGuesses(walker, operand);
                std::optional<TestOfGuess> chosen = expectation.type ? chooseTestOfGuess(walker, value, expectation.type) : std::nullopt;
                if (!chosen)
                    return;
                bool rulesOutNullish = accessesToSlotsForGuesses.contains(operation.opcode()) && operand == operation.objectOperand() && types.withoutNullish(walker.typeOf(value)) != walker.typeOf(value);
                TypeID typeBehindTest = types.intersectionOf(walker.typeOf(value), chosen->tested);
                auto typeWithProof = [&](ValueID input) -> TypeID {
                    ValueID behindCasts = valueBehindCastsOfLoopForGuesses(index, input, firstValueOfLoop);
                    return behindCasts.index() < firstValueOfLoop && walker.referenceOf(behindCasts) == value ? typeBehindTest : typeInLook(input);
                };
                TypesOfInputs typesWithProof(typeWithProof);
                if (typedFormForGuesses(walker, operation, typesWithProof) == TypedForm::None)
                    return;
                bool isTypedWithoutProof = typedFormForGuesses(walker, operation, typesOfLook) != TypedForm::None;
                if (isTypedWithoutProof && !rulesOutNullish && (!mayRunCodeAsOperatorForGuesses(walker, operation, typesOfLook) || mayRunCodeAsOperatorForGuesses(walker, operation, typesWithProof)))
                    return;
                guesses.append(GuessInFrontOfLoop { value, chosen->tested, chosen->layouts, { } });
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
    for (GuessInFrontOfLoop& guess : guesses) {
        for (ValueID name : mentioned) {
            if (walker.referenceOf(name) == guess.value)
                guess.names.append(name);
        }
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

Vector<GuessInFrontOfLoop, 4> findMembersToldApartInFrontOfLoop(Walker& walker, BlockID inputHead, const LoopPreview& preview)
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

    Vector<GuessInFrontOfLoop, 4> members;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!accessesToldApartInFrontOfLoop.contains(operation.opcode()))
                continue;
            ValueID operand = operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().calleeOperand() : operation.objectOperand();
            ValueID inFront = operand ? valueBehindCastsOfLoopForGuesses(index, operand, firstValueOfLoop) : ValueID();
            if (!inFront || inFront.index() >= firstValueOfLoop)
                continue;
            if ((operation.opcode() == Opcode::PropGet || operation.opcode() == Opcode::PropSet) && operation.isFreeName())
                continue;
            Reference value = walker.referenceOf(inFront);
            if (!value || !value.isValue() || typedFormForGuesses(walker, operation, typesOfLook) != TypedForm::None)
                continue;
            TypeID proven = walker.typeOf(value);
            TypeID expected = saysNothingForGuesses(types, proven) ? findExpectationForGuesses(walker, operand).type : proven;
            if (!expected)
                continue;
            for (TypeID part : membersOfSetForGuesses(walker, expected)) {
                if (operation.opcode() == Opcode::Call && !copyOfCalleeCalledInLoopPays(walker, singleFunctionOf(types, types.at(part)), operation.as<Opcode::Call>().argumentsOperands().size()))
                    continue;
                std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, value, part);
                TypeID typeBehindTest = chosen ? types.intersectionOf(proven, chosen->tested) : TypeID();
                auto typeWithProof = [&](ValueID input) -> TypeID {
                    ValueID behindCasts = valueBehindCastsOfLoopForGuesses(index, input, firstValueOfLoop);
                    return behindCasts.index() < firstValueOfLoop && walker.referenceOf(behindCasts) == value ? typeBehindTest : typeInLook(input);
                };
                TypesOfInputs typesWithProof(typeWithProof);
                if (chosen && typedFormForGuesses(walker, operation, typesWithProof) != TypedForm::None)
                    members.append(GuessInFrontOfLoop { value, chosen->tested, chosen->layouts, { } });
            }
            if (members.size() >= 2)
                break;
            members.shrink(0);
        }
        if (!members.isEmpty())
            break;
    }
    if (members.isEmpty())
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
    bool isToldApartByBody { false };
    uint16_t nullishAskedFor { 0 };
};

struct WorkerOfParameterGuards {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WorkerOfParameterGuards);

    explicit WorkerOfParameterGuards(TypeTable& table)
        : types(table, ArenaKeeps::InProgram)
    {
    }

    TypeArena types;
    KeptBetweenUnits kept;
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
            if (!looksAtParameter || !(operationsThatOpenGuesses.contains(opcode) || opcode == Opcode::NewObject))
                continue;
            if (typedFormForGuesses(walker, operation, inputsBehindGuard) == TypedForm::None)
                continue;
            if (typedFormForGuesses(walker, operation, inputsInFrontOfGuard) != TypedForm::None) {
                static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
                if (!operators.contains(opcode))
                    continue;
                OperatorForm formInFrontOfGuard = formAllowedForTypedOperator(walker, opcode, operation, InputsOfTypedOperation(walker, &inputsInFrontOfGuard));
                if (formAllowedForTypedOperator(walker, opcode, operation, InputsOfTypedOperation(walker, &inputsBehindGuard)) == formInFrontOfGuard)
                    continue;
                if (!mayCallRuntime(opcode, formInFrontOfGuard))
                    continue;
            }
            numberThatLean += index.loopDepthOf(block.id()) ? roundsOfLoopForParameterGuards : 1;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                if (uint16_t named = namedParameters[operand.index()])
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

static void letThroughNullishThatBodyAsksFor(TypeArena& types, ParameterOfGuard& parameter)
{
    if (parameter.isToldApartByBody || !parameter.nullishAskedFor)
        return;
    parameter.tested = types.withPrimitives(parameter.tested, parameter.nullishAskedFor);
    parameter.behindGuard = types.intersectionOf(parameter.inFrontOfGuard, parameter.tested);
}

static uint32_t countOperationsThatLookAtParameters(Walker& walker, std::span<const ParameterOfGuard> parameters)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<bool, 256> namesParameter;
    namesParameter.fill(false, index.numberOfValues());
    for (const ParameterOfGuard& parameter : parameters)
        namesParameter[parameter.entryParameter] = true;

    uint32_t numberThatLook = 0;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            bool looksAtParameter = false;
            bool firstIsNamed = false;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!namesParameter[operand.index()])
                    return;
                if (!operandIndex)
                    firstIsNamed = true;
                looksAtParameter |= !isOnlyHandedOnForGuesses(operation, operandIndex) || (storesIntoSlotsForGuesses.contains(opcode) && operand == operation.valueOperand()) || opcode == Opcode::NewObject;
            });
            if (firstIsNamed && inputOperationsThatHandOn.contains(opcode)) {
                namesParameter[operation.id().index()] = true;
                continue;
            }
            if (looksAtParameter && (operationsThatOpenGuesses.contains(opcode) || opcode == Opcode::NewObject))
                numberThatLook += index.loopDepthOf(block.id()) ? roundsOfLoopForParameterGuards : 1;
            if (firstIsNamed && opcode == Opcode::IteratorOpen)
                numberThatLook += 2 * roundsOfLoopForParameterGuards;
        }
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            if (result.subject && namesParameter[result.subject.index()] && result.parameter && index.numberOfIncomingEdges(index.blockOf(result.parameter)) == 1)
                namesParameter[result.parameter.index()] = true;
        });
    }
    return numberThatLook;
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
    if (!walker.standInFrontOfBodyOf(id) || walker.currentBodyIndex().block(entryBlock).numberOfParameters() != numberOfEntryParameters)
        return { };

    TypeArena& types = walker.types();
    uint32_t firstPositionalParameter = function.firstParameterIndex();
    uint32_t endOfPositionalParameters = firstPositionalParameter + function.numberOfPositionalParameters();
    Vector<TypeID, 8> typesInFrontOfGuard;
    Vector<ParameterOfGuard, 8> parameters;
    for (uint32_t index = 0; index < numberOfEntryParameters; ++index) {
        typesInFrontOfGuard.append(walker.typeArrivingAtEntryParameter(index));
        bool isTested = function.entryParameterKind(index) == EntryParameter::This || (index >= firstPositionalParameter && index < endOfPositionalParameters);
        TypeID tested = isTested ? typeTestedByParameterGuard(walker, findExpectationForGuesses(walker, ValueID(index)).type, typesInFrontOfGuard.last()) : TypeID();
        tested = typeTestedInPlaceOfWrittenType(walker, id, index, tested);
        if (tested && isNeverType(types.intersectionOf(typesInFrontOfGuard.last(), tested)))
            tested = { };
        if (!tested)
            continue;
        parameters.append(ParameterOfGuard { index, tested, typesInFrontOfGuard.last(), types.intersectionOf(typesInFrontOfGuard.last(), tested), false, { }, false });
        TypeID whole = types.at(tested).hasObjects() ? findWholeExpectationForGuesses(walker, ValueID(index)).type : TypeID();
        if (!saysNothingForGuesses(types, whole) && partThatOneTestTellsForGuesses(types, whole) != whole)
            parameters.last().testedWithPrimitives = typeTestedByParameterGuard(walker, whole, typesInFrontOfGuard.last());
    }
    if (parameters.isEmpty())
        return { };
    Walker::ChoiceOfCost guardIsDrawn = walker.drawChoice(DrawnFor::ParameterGuard, id.index(), 0);
    if (guardIsDrawn == Walker::ChoiceOfCost::No)
        return { };
    if (guardIsDrawn == Walker::ChoiceOfCost::AsItPays && static_cast<uint64_t>(countOperationsThatLookAtParameters(walker, parameters.span())) * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues())
        return { };
    Vector<ParameterOfGuard, 8> asOneTestTells = parameters;
    findParametersThatBodyTellsApart(walker, parameters.mutableSpan());
    for (ParameterOfGuard& parameter : parameters)
        letThroughNullishThatBodyAsksFor(types, parameter);
    uint32_t numberThatLean = findParametersThatBodyLeansOn(walker, parameters.mutableSpan(), typesInFrontOfGuard.span());
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
    if (guardIsDrawn == Walker::ChoiceOfCost::AsItPays && static_cast<uint64_t>(numberThatLean) * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues())
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
            workers[worker] = makeUnique<WorkerOfParameterGuards>(program.types);
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            Walker walker(inputs, workers[worker]->types, inputs.arrivals, workers[worker]->kept);
            Vector<TypeID, 8> typesBehindGuard = typesBehindParameterGuard(walker, FunctionID(index));
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
            TypeID tested = types.withoutNullish(types.testablePartOf(types.withLiteralStringsWidened(widenedForTestOfGuess(walker, expectation.type))));
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
        TypeID tested = types.withoutNullish(types.testablePartOf(types.withLiteralStringsWidened(widenedForTestOfGuess(walker, expectation.type))));
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

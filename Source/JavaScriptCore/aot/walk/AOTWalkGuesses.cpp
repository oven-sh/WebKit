/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Guesses: tests of ours, the stretch of one input block that leans on them built with the proof, the same stretch built closed, and a block where the two meet. Behind that block the proof is gone.
// The tests stand in a row in front of both arms: what the value is, then whether the slots that the stretch goes to are filled. Each leads to the one closed arm where it fails.
// At a suspension: a stretch ends in front of it, so no arm is open where a frame is saved.
// At a throw: either arm throws to the handler of the input block as any block does. The proof lies in the names and the state of the typed run, and a handler gets neither.
// Withheld: shouldOpenGuess() says no and nothing is built. What is not proven takes its closed form.

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
#include "AOTWalkJSONWrites.h"
#include "AOTWalkTextBuilderRows.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkLoops.cpp
TypeID topsOfKindsInLoops(TypeArena&, TypeID);
// ir/AOTResultRulesDeclared.cpp
TypeID tryTypeOfFieldRead(const TypingContext&, TypeID receiver, PropertyKeyID);

// How many operations a guess has to turn from closed into typed.
static constexpr uint32_t minimumOperationsPerGuess = 2;
static constexpr uint32_t minimumOperationsPerGuessInLoop = 1;
// A test of a shape in front of one access to a slot is a cache whose shape is known when the program is built: a few instructions and a load in the place of the twenty of a closed read, and no memory that is written.
static constexpr uint32_t minimumOperationsPerGuessOfShape = 1;

// What callDirectly() takes by the type of the callee.
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
// Typed, they write at most the slot that they name, but for TypedForm::BuiltAsCall. Any other typed form may have its closed operation inside it, or enters a function.
static constexpr OpcodeSet operationsWhoseTypedFormsRunNoCodeForGuesses = accessesToSlotsForGuesses | opcodesOfFamily(OperationFamily::Operator) | OpcodeSet { Opcode::RequireObjectCoercible };
// What makes the arguments of a call. They may run code, which changes shapes and no primitive. The making of an object is none of them: behind the rejoin nobody knows any more that it is being made.
static constexpr OpcodeSet operationsThatEndNoProofOfPrimitiveForGuesses = opcodesOfFamily(OperationFamily::Operator) | OpcodeSet { Opcode::PropGet, Opcode::PrivateGet, Opcode::ElemGet, Opcode::RequireObjectCoercible };

namespace {

struct ExpectationForGuesses {
    TypeID type;
    // The source declares it, or Link holds it. A weaker one is what was seen to arrive beside `unknown`.
    bool isDeclared { false };
    // Somewhere on the road to it nothing was written, and Facts::held() answered.
    bool isHeld { false };
    // Of what was expected in front of the cut to `type`. False too where nobody has looked.
    bool admitsNoNumberOrBoolean { false };
};

struct TestOfGuess {
    TypeID tested;
    // Null: a TypeTest.
    LayoutSetID layouts;
    // The value is proven to be that. Only its slots are looked at.
    bool isPassedAlready { false };
    // Where the test fails the value is asked for this in its turn, and the stretch is walked once more behind that test. Null: the failing side is the closed arm at once.
    TypeID failingSideAsksFor;
    // The stretch is the truth of the value alone. Whoever asks for that expects nothing as well: where the test fails, undefined and null are asked for in front of the closed operation.
    bool failingSideAsksForNothing { false };
    // The value is a proven list of which numbers alone are written, and the first operation of the stretch reads it under an integer. In front of the arms it is asked for storage that holds numbers alone and for an element at the position.
    bool asksForElementOfNumbers { false };
    // ExpectationForGuesses::admitsNoNumberOrBoolean of the guess: `expectsCell` of the test.
    bool expectsCell { false };
    // All that is expected of the value passes, so nobody expects the test to fail: in the typed copy of a loop the turn begins again there. Walker::mayBeginTurnAgainInClosedCopy().
    bool admitsAllThatIsExpected { false };
    // Beside failingSideAsksFor: nobody knows which of the two a run meets more often. The failing side is no rare block, and the typed run behind its test opens guesses as that of the first does.
    bool failingSideIsAsLikely { false };
    // Of the test on such a failing side, which stands under the ClosedArm of the first.
    bool typedRunOpensGuesses { false };
};

struct LookAtSlotOfGuess {
    // The first access of the stretch that goes to the slot.
    ValueID access;
    // `a.b.c`: the access further up in the row whose look has loaded the object that has the slot. Null: the value of the guess has it.
    ValueID holder;
    // `this.handler(n)`: what the look loads is called, and the slot is held to the closures of one function or nothing. It is asked whether it is there.
    bool isCalled { false };
    // `scanner.scan()`: what the look loads is called, the slot is held to nothing, and one function was seen to arrive at the field. What is loaded is compared with it.
    TypeID calleeSeenToArrive;
};

struct TestOfStoredValueOfGuess {
    // It is defined in front of the stretch.
    ValueID value;
    // What the slots take that the stretch stores it into.
    TypeID tested;
};

struct StretchOfGuess {
    // Behind the last operation that is counted.
    uint32_t end { 0 };
    // Built typed with the proof, and closed without it.
    uint32_t numberOfTypedOperations { 0 };
    // Behind the operators that stand right behind `end` and take what the stretch has read. 0: there is none. They are typed either way, so they count for nothing, and whoever builds arms walks them in each arm.
    uint32_t endOfOperatorsBehind { 0 };
    // One for each slot that is looked at in front of the arms: TypedForm::BuiltBehindFilledSlot.
    Vector<LookAtSlotOfGuess, 4> firstAccessesToSlots;
    // In front of the arms the value is asked whether it is as its class makes it: TypedForm::BuiltOnObjectAsMade.
    bool looksWhetherValueIsAsMade { false };
    // In front of the arms the cell of the value is asked, once for all of them, whether it takes these stores: TypedForm::BuiltBehindLookAtCell, BuiltBehindFilledSlotAndLookAtCell.
    Vector<ValueID, 4> storesThatLookAtCell;
    // In front of the arms each value is asked whether it fits the slots that the stretch stores it into: a typed store writes the bits as they are.
    Vector<TestOfStoredValueOfGuess, 4> testsOfStoredValues;
    // The one test of a guess inside the typed run of another: whether the value lists its keys as a maker of the program leaves them. Who fails it goes through the stretch with all that the outer tests have shown.
    std::optional<uint32_t> orderWordAskedFor;
    // Of a guess that asksForElementOfNumbers, by place from the begin on: of what the operation gives the typed run knows more than the closed arm. Where the arms meet such a value is boxed.
    BitVector placesThatRestOnProof;
    // By place from the begin on: the typed run is known to give a type that says something, and the closed arm is not known to give as much.
    BitVector placesThatTypedRunKnowsBetter;
};

// An arm that has come through the stretch.
struct ArmOfGuess {
    // Left open.
    BlockID lastBlock;
    // What it has built for each value that is read behind the stretch.
    Vector<Reference, 8> values;
};

} // anonymous namespace

// ---- What is expected of a value

static bool saysNothingForGuesses(const TypeArena& types, TypeID type)
{
    return types.saysNothing(type);
}

// `Node | string`, and a Node that was seen to arrive beside anything, where every primitive is listed too: no one test tells such a type. The objects are what has members to find.
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
    // `function Node(this: Node, kind) { this.kind = kind; .. }`: whatever is written of `this`, `new` hands the function an object of the layout that it constructs.
    const Function& record = walker.program().at(function);
    if (record.takesThisFromConstruction() && record.constructedClass && record.thisParameterIndex() == entryParameter && walker.plan().isInUse(record.constructedClass)) {
        TypeID expected = typeOfObjectsMadeBy(walker, function);
        // `function Below(a, b) { Base.call(this, a); .. }`, `_super.call(this, a) || this`: a function that is called too is handed the objects of the functions below it.
        // `class Below extends Base { constructor() { super(..) } }`: the construction hands it an object of the class.
        bool isCalledToo = !link.function(function).everyEntryIsConstruction;
        for (LayoutID below : walker.program().layoutsExpectedBelow(record.constructedClass)) {
            if ((isCalledToo || !walker.program().at(below).has(LayoutFlags::IsConstructorFunction)) && walker.plan().isInUse(below))
                expected = walker.types().unionOf(expected, walker.types().forLayout(below));
        }
        return { expected, true };
    }
    return { parameters[entryParameter].expected, parameters[entryParameter].expectedIsDeclared };
}

// `const sig = new Signature(..); sig.declaration = ..`: whatever is no function throws, and what the one function is entered for is an object of the layout that it constructs.
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
        // Under the flag the type is that of what the result settles to. It says nothing of the result.
        if (!expected.flags.contains(ExpectedResultFlags::IsRowsOfQuery))
            return { expected.type, resultIsAnnotated };
        if (TypeID promise = typeOfPromiseExpectedToGive(walker, expected.type))
            return { promise, resultIsAnnotated };
    }
    if (resultIsAnnotated)
        return { writtenResult->expected().predicted(), true };
    return { };
}

// `getSignaturesOfType(type, kind).length`: the call names its callee. What is written of the result there is expected of it, else what Link holds.
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

// The row's own rule over the types that `typeOfOperand` gives. Null: the row gives no result, or one that is read through ResultAt.
template<typename TypeOfOperand>
static TypeID typeByRowForGuesses(const TypingContext& typing, const BodyValue& operation, const TypeOfOperand& typeOfOperand)
{
    Opcode opcode = operation.opcode();
    if (opcode == Opcode::NarrowByTest)
        return typeOfOperand(operation.operand(OperandIndex::NarrowByTest::left), OperandIndex::NarrowByTest::left);
    if (opcodesThatStateTypes.contains(opcode))
        return typeOfOperand(operation.operand(0), 0);
    if (resultRuleOf(opcode) == ResultRule::None || opcode == Opcode::ResultAt)
        return { };

    // A rule asks of an operand whether it is there, and its type. It never follows the reference.
    Draft draft(opcode);
    draft.flags = static_cast<uint8_t>(operation.flags());
    copyFields(operation, draft, [](ValueID operand) {
        return Reference::value(operand);
    });
    if (opcodesThatMayHaveSeveralResults.contains(opcode) && numberOfResultsOf(typing.program, draft))
        return { };

    Vector<TypeID, 8> operandTypes;
    operandTypes.fill(TypeID(), numberOfNamedOperandsOf(opcode) + operation.variadicOperands().size());
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        operandTypes[operandIndex] = typeOfOperand(operand, operandIndex);
    });
    return resultTypeOf(typing, draft, OperandTypes { operandTypes.span() });
}

// What is awaited is read like a receiver: the rule gives what a promise of the written type settles to.
static std::optional<uint8_t> placeOfReceiverForGuesses(Opcode opcode)
{
    if (opcode == Opcode::Await)
        return OperandIndex::Await::value;
    return receiverOf(opcode);
}

// At the level Expected the rule goes by what is written of the field, the variable, the capture or the contents. `receiver`: what is expected of the receiver. Null: what is proven of it.
static TypeID typeOfReadForGuesses(Walker& walker, const TypingContext& typing, const BodyValue& read, TypeID receiver)
{
    // The row of a read by name comes to this, or to `unknown`, which says nothing to whoever asks here: ResultRules::declared(operation, key).
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

// WHERE NOTHING IS WRITTEN, WHAT LINK HOLDS IS EXPECTED (Facts::held()): its flow read as if unseen code only read and called. It is no proof, and a guess needs none: its test stands at each use.
// The rows answer as they do for what is proven, from the other tables.
static TypeID typeOfReadByWhatIsHeldForGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    return typeOfReadForGuesses(walker, { walker.program(), walker.types(), walker.heldPositions(), TypeLevel::Proven }, read, receiver);
}

static TypeID typeHeldOfEntryParameterForGuesses(const Walker& walker, uint32_t entryParameter)
{
    const LinkTables& held = walker.facts().held();
    FunctionID function = walker.currentFunction();
    if (function.index() >= held.functions.size())
        return { };
    std::span<const LinkedParameter> parameters = held.parametersOf(function);
    return entryParameter < parameters.size() ? parameters[entryParameter].proven : TypeID();
}

static TypeID typeHeldOfResultOfCallForGuesses(const Walker& walker, const BodyValue& call)
{
    const LinkTables& held = walker.facts().held();
    FunctionID callee = held.calleeAt(walker.currentFunction(), call.id());
    // What `return` hands over in a coroutine is not what its caller gets.
    if (callee && callee.index() < held.functions.size() && walker.program().at(callee).isCoroutine())
        return typeHeldOfResultOfCoroutine(walker, callee);
    if (!callee || callee.index() >= held.functions.size())
        return { };
    return held.function(callee).result;
}

// `for (const item of items)`: what a step hands out is read off what is iterated, like `items[i]`. Null: the value is no such result.
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
    // By block, and one more: where the list of the block begins in incomingEdges.
    Vector<uint32_t> firstIncomingEdges;
    Vector<BodyIncomingEdge> incomingEdges;
    // By value of the body: the value that it stands for. Empty: the body asks no value for a name.
    Vector<uint32_t> rootsOfValues;
    // The names, and beside each the value that is asked for it, ascending.
    Vector<uint32_t> rootsAskedForNames;
    Vector<PropertyKeyID> namesUsedOffValues;
    // The value above each Convert that hands it on, ascending.
    Vector<uint64_t> castsOfRoots;
    // Each of the two is made when it is first asked for, by one thread, and never changes then: most bodies are asked for one of them or for none.
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
    // Each list is filled from its begin, which moves to its end, the begin of the next.
    forEachEdge([&](const BodyIncomingEdge& edge) {
        kept.incomingEdges[kept.firstIncomingEdges[edge.target.block().index()]++] = edge;
    });
    for (uint32_t next = numberOfBlocks; next; --next)
        kept.firstIncomingEdges[next] = kept.firstIncomingEdges[next - 1];
    kept.firstIncomingEdges[0] = 0;
}

// The walker stands in the body.
// `(location as SourceFile).locals`, `case SyntaxKind.ModuleDeclaration: location.body`: behind a written type that names layouts and behind a test of a field, the value is asked for what one member of its type has.
// That says nothing of the value in front. What is asked there narrows what is expected of the value behind.
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
    // What only hands a value on stands for it.
    Vector<uint32_t> roots(numberOfValues, [](size_t number) { return static_cast<uint32_t>(number); });
    // The value above the name.
    Vector<uint64_t> uses;
    // The value that is handed over above the parameter that takes it.
    Vector<uint64_t> arrivals;
    Vector<uint64_t> casts;
    auto use = [&](ValueID object, PropertyKeyID name) {
        if (object && name)
            uses.append(static_cast<uint64_t>(roots[object.index()]) << 32 | name.index());
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        BodyBlock block = index.block(BlockID(number));
        // BodyIndex::valueNarrowedInto() of each parameter, with one look at the edge for all of them. It answers by the first proof that fills the parameter.
        if (std::optional<BodyIncomingEdge> edge = number && block.numberOfParameters() ? index.onlyIncomingEdge(BlockID(number)) : std::nullopt) {
            for (uint32_t proofIndex = static_cast<uint32_t>(edge->target.proofs().size()); proofIndex--;) {
                std::optional<BodyEdgeResult> result = index.edgeResult(edge->from, edge->targetIndex, edge->target, proofIndex);
                if (!result || !result->parameter)
                    continue;
                roots[result->parameter.index()] = result->subject && result->subject < result->parameter ? roots[result->subject.index()] : result->parameter.index();
            }
        }
        // The index knows where each operation begins: nobody has to measure the one in front of it.
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
            default:
                // Under whatever view the cast shows the value: it is the same value.
                if (opcode == Opcode::Convert && operation.operand(0) && operation.operand(0) < operation.id())
                    casts.append(static_cast<uint64_t>(roots[operation.operand(0).index()]) << 32 | operation.id().index());
                if (ValueID handedOn = inputOperationsThatHandOn.contains(opcode) ? operation.operand(0) : ValueID(); handedOn && handedOn < operation.id() && !showsValueUnderAnotherViewForGuesses(walker.program(), operation))
                    roots[operation.id().index()] = roots[handedOn.index()];
                break;
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
    // `const t = c ? a : b; t.x`: whichever of the two it is, it is asked for `x`. Through a row of such parameters as far as the blocks are deep: each round goes one further.
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
    kept.rootsOfValues = WTF::move(roots);
}

// Both follow from the body as it is written, so a copy of the function in another unit asks for nothing anew.
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
    if (frame.expectations.isEmpty())
        frame.expectations.fill({ }, frame.index->numberOfValues());
    return frame.expectations[value.index()];
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

std::span<const uint64_t> Walker::castsOf(ValueID value)
{
    const BodyIndex& index = *m_frames.last().index;
    KeptWithBodyForGuesses& kept = keptWithBodyForGuesses(index);
    makeListKeptWithBodyForGuesses(kept, kept.hasNamesUsedOffValues, [&] {
        listNamesUsedOffValuesForGuesses(*this, index, kept);
    });
    if (kept.rootsOfValues.isEmpty())
        return { };
    auto found = std::ranges::equal_range(kept.castsOfRoots, static_cast<uint64_t>(kept.rootsOfValues[value.index()]), { }, [](uint64_t cast) { return cast >> 32; });
    return kept.castsOfRoots.span().subspan(found.begin() - kept.castsOfRoots.begin(), found.size());
}

// A round over fewer layouts costs less than the look for its answer.
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

// One of the names that namesUsedOff() lists for the value, found without the list: what the first mention of the value asks it for, through what only hands the value on. Null: the first mention asks for none.
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
        // The list leaves out what stands in a block that nothing reaches.
        return name && index.isReachable(index.blockOf(operation.id())) ? name : PropertyKeyID();
    }
}

// The last resort: facts/AOTLayoutsByNames.h.
static ExpectationForGuesses expectationByNamesUsedForGuesses(Walker& walker, ValueID input)
{
    // Most values are asked for a name that no instance has, and the first mention shows it: the list of the body's names is not made for that.
    if (PropertyKeyID name = findNameUsedOffValueForGuesses(walker.program(), walker.currentBodyIndex(), input); name && !walker.facts().layoutsByNames().isNameOfSomeInstance(name))
        return { };
    LayoutID only = walker.facts().layoutsByNames().findOnlyLayoutWithAll(walker.namesUsedOff(input));
    return only ? ExpectationForGuesses { walker.types().forLayout(only), false } : ExpectationForGuesses();
}

// `node.child` is written or held to be one of two layouts. There may be no one test for the two, or a name that is read off it may be a field of one alone, so that no typed form serves both.
// What is read there is asked for names that one of the two alone has: that one is expected.
static TypeID narrowedByNamesUsedForGuesses(Walker& walker, ValueID input, TypeID expected)
{
    TypeArena& types = walker.types();
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures())
        return expected;
    std::span<const LayoutID> layouts = types.listOf(type.layouts);
    if (layouts.size() < 2)
        return expected;
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    if (names.empty())
        return expected;
    // Null: the type stays as it is.
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
    // A layout of which no object is made arrives nowhere: it neither stays nor goes.
    const LayoutsByNames& index = walker.facts().layoutsByNames();
    // A name that one of them alone has settles it without a look at the others: each of them lacks that name. A family may be every layout of the program that has the names that a written type requires.
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

// `if (x.kind === 1) s += x.a; else s += x.b;`, x: A | B. All the names that the body uses off `x` no member has, so narrowedByNamesUsedForGuesses() leaves the union as it is, and no typed form of `x.a` serves a B.
// Behind the program's own test of a tag the members are cut by their written tags, cutExpectationByTestedField(); a stranger whose tag nobody wrote stays. Of what is left, AT THE ACCESS, the one member in use that has the name is expected. Several have it, or all: it stays.
static TypeID narrowedByNameAskedForGuesses(Walker& walker, TypeID expected, PropertyKeyID name)
{
    TypeArena& types = walker.types();
    if (!expected || !name)
        return expected;
    Type type = types.at(expected);
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
    // A layout of which no object is made arrives nowhere: it neither stays nor goes. Where no other member is in use nothing is told apart.
    if (only && std::ranges::none_of(layouts, [&](LayoutID layout) { return layout != only && index.isInUse(layout); }))
        only = { };
    if (isKept)
        walker.keepRound(Walker::RoundKeptByWalker::OnlyLayoutWithName, type.layouts, name.index(), only.bits());
    return narrowedTo(only);
}

static ExpectationForGuesses findExpectationForGuesses(Walker&, ValueID input);

// Undefined and null apart: whoever reads or stores through them throws.
static bool admitsNoNumberOrBooleanForGuesses(const TypeArena& types, TypeID expected)
{
    return expected && !(types.at(expected).primitives & ~(cellPrimitives | Primitive::UndefinedOrNull));
}

// What is proven of a value is expected of it. `user` is defined behind `input`, so every road ends.
static ExpectationForGuesses findWhatIsProvenOrExpectedForGuesses(Walker& walker, ValueID input, ValueID user)
{
    ExpectationForGuesses proven { provenTypeOfInputForGuesses(walker, input), true };
    proven.admitsNoNumberOrBoolean = admitsNoNumberOrBooleanForGuesses(walker.types(), proven.type);
    if (input >= user)
        return saysNothingForGuesses(walker.types(), proven.type) ? ExpectationForGuesses() : proven;
    if (saysNothingForGuesses(walker.types(), proven.type))
        return findExpectationForGuesses(walker, input);

    // `const list = holder.nodes; .. list[i].kind`: of what the field holds Link proves the class, and a type never proves contents. What is written of them stands in what is expected of the value.
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
    return { expectedObject, expected.isDeclared, expected.isHeld, expected.admitsNoNumberOrBoolean };
}

// A parameter of a block has a type where the source declares that of the variable. Of any other, what is expected of the values that arrive is expected: `const t = c ? a : b`, `for (let n = first; n; n = n.next)`.
// What comes round a loop is made of the parameter itself and is left out. So which number, text or boolean enters says nothing of the next turn, and only its kind is expected: `let sum = 0`.
// An edge that brings a value of which nothing is expected makes the guess a weaker one.
static ExpectationForGuesses findExpectationOfWhatArrivesForGuesses(Walker& walker, ValueID parameter)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    BlockID block = index.blockOf(parameter);
    ExpectationForGuesses joined { { }, true, false, true };
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
            joined.admitsNoNumberOrBoolean &= arriving.type && arriving.admitsNoNumberOrBoolean;
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

// `function textOf(node: Identifier | Literal) { return node.text; }`: Link expects the family around the two, and a Block has no `text`: no typed form serves the family. There what is written is expected as it is written.
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

// The edge on which `Array.isArray(..)` has said yes, or the jump out of a block that this edge alone enters.
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

// What fills the parameter, where that is what a test of a type hands over on an edge.
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

// Of a parameter that no test of a type fills.
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
    // `Array.isArray(x)`: the ways on which the answer is yes meet.
    if (index.numberOfIncomingEdges(block) < 2 || index.isEnteredByHandlerEdge(block) || index.isLoopHead(block))
        return findExpectationForGuesses(walker, input);
    uint32_t position = input.index() - index.block(block).parameter(0).index();
    ExpectationForGuesses joined;
    for (const BodyIncomingEdge& arriving : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = arriving.target.arguments();
        ExpectationForGuesses whole = position < arguments.size() && arguments[position] < input ? findWholeExpectationForGuesses(walker, arguments[position]) : ExpectationForGuesses();
        if (saysNothingForGuesses(types, whole.type))
            return { };
        // The function has said yes to what the look at the cell had turned away. That is expected of nothing.
        if (comesFromWhereIsArraySaysYesForGuesses(index, arriving) && !types.overlap(whole.type, types.builtinClass(BuiltinClasses::Array)))
            continue;
        if (joined.type)
            whole.type = types.unionOf(joined.type, whole.type);
        joined = whole;
    }
    return joined;
}

// `function size(x: string | string[]) { return typeof x === "string" ? x.length : x[0].length; }`: what is expected of `x` leaves out the text, because no one test tells a text or a list.
// The program tells them apart itself: behind its tests each arm has its part of ALL that is written or held of an entry parameter, and of what is expected of any other value.
// A row of tests of one value is gone through from the last back to the first of which the answer is kept, and forward again: each keeps its own.
static ExpectationForGuesses findWholeExpectationForGuesses(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    auto keep = [&](ValueID parameter, const ExpectationForGuesses& whole) {
        walker.keepWholeExpectationOf(parameter, { whole.type, whole.isDeclared, whole.isHeld, true, whole.admitsNoNumberOrBoolean });
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
            whole = { kept->type, kept->isDeclared, kept->isHeld, kept->admitsNoNumberOrBoolean };
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
        // Whoever guesses leaves undefined and null out, so a test for the two cuts nothing that a guess goes by.
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
    // Each arrival has been cut by itself.
    bool isJoinOfArrivals = false;
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
            if (saysNothingForGuesses(types, expectation.type)) {
                expectation = findExpectationOfWhatArrivesForGuesses(walker, input);
                isJoinOfArrivals = true;
            }
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
            expectation = { typeExpectedOfResultOfCall(walker, definition), false };
    } else if (opcode == Opcode::CallDirect)
        expectation = expectationOfDirectCallForGuesses(walker, definition);
    else if (readsOfDeclaredPlacesForGuesses.contains(opcode)) {
        // `a.b.c`: what is written of the field, in what is expected of the receiver.
        ExpectationForGuesses ofReceiver { { }, true };
        std::optional<uint8_t> receiver = placeOfReceiverForGuesses(opcode);
        if (receiver)
            ofReceiver = findWhatIsProvenOrExpectedForGuesses(walker, definition.operand(*receiver), input);
        if (!receiver || ofReceiver.type) {
            expectation = { typeOfReadAtLevelExpectedForGuesses(walker, definition, ofReceiver.type), ofReceiver.isDeclared, ofReceiver.isHeld };
            if (saysNothingForGuesses(types, expectation.type))
                expectation = { typeOfReadByWhatIsHeldForGuesses(walker, definition, ofReceiver.type), ofReceiver.isDeclared, true };
        }
        // `items[i]`, of a list of which nothing is written and that unseen code has: what the program was seen to put into it.
        if (opcode == Opcode::ElemGet && saysNothingForGuesses(types, expectation.type))
            expectation = { walker.facts().held().resultOfReadByKey(walker.currentFunction(), input), true, true };
    } else if (opcode == Opcode::ResultAt) {
        if (ValueID iterable = iterableThatStepReadsForGuesses(index, definition)) {
            ExpectationForGuesses ofIterable = findWhatIsProvenOrExpectedForGuesses(walker, iterable, input);
            if (ofIterable.type) {
                TypeID iterated = types.withoutNullish(ofIterable.type);
                TypeID pairs = typeOfPairsThatStepsHandOut(walker, iterated);
                expectation = { pairs ? pairs : typeOfContentsRead(expecting, iterated, ContentsSlot::Element), ofIterable.isDeclared, ofIterable.isHeld };
            }
        }
    }

    if (!saysNothingForGuesses(types, expectation.type)) {
        expectation.admitsNoNumberOrBoolean = (!isJoinOfArrivals || expectation.admitsNoNumberOrBoolean) && admitsNoNumberOrBooleanForGuesses(types, expectation.type);
        expectation.type = narrowedByNamesUsedForGuesses(walker, input, partThatOneTestTellsForGuesses(types, expectation.type));
        if (expectation.type && types.at(expectation.type).isUndefinedOrNull()) {
            if (ExpectationForGuesses byNames = expectationByNamesUsedForGuesses(walker, input); byNames.type)
                return byNames;
        }
        return expectation;
    }
    if (!behind && !definition.isParameter() && inputOperationsThatHandOn.contains(opcode))
        behind = definition.operand(0);
    // `ToObject(this)`: by its row the operation returns its operand or makes something of it. What is expected to be an object is expected to be returned.
    if (!behind && !definition.isParameter() && numberOfNamedOperandsOf(opcode) == 1 && !hasVariadicOperand(opcode) && definition.operandUses(0).containsOnly({ OperandUse::Returns }))
        behind = definition.operand(0);
    if (behind && behind < input) {
        ExpectationForGuesses ofValueBehind = findExpectationForGuesses(walker, behind);
        if (!saysNothingForGuesses(types, ofValueBehind.type))
            return ofValueBehind;
    }
    // `function createBaseDeclaration<T extends Declaration>(kind) { const node = createBaseNode(kind); node.symbol = undefined; .. return node; }`, copied in: of `node` the helper writes its own `T`, which says nothing.
    // It is what the copy hands back, and whoever called has written what he expects of the answer.
    // Of objects alone, as below: whether a number is an int32 is for whoever holds it to show. `function size(): int32 { return box.size; }`, copied in: what the caller expects of the answer is the callee's own `int32`:
    // stated of `box.size` the promise would vouch for itself, and the build would not stop where it has to (TS100031; docs/compiler/types.mdx: "Where it can't, it stops").
    if (TypeID ofAnswer = walker.typeExpectedOfAnswerOfCopy(); ofAnswer && types.at(ofAnswer).hasObjects() && admitsNoNumberOrBooleanForGuesses(types, ofAnswer) && walker.copyHandsBack(input)) {
        ExpectationForGuesses stated { ofAnswer, walker.answerOfCopyIsDeclared() };
        stated.admitsNoNumberOrBoolean = admitsNoNumberOrBooleanForGuesses(types, stated.type);
        stated.type = narrowedByNamesUsedForGuesses(walker, input, partThatOneTestTellsForGuesses(types, stated.type));
        return stated;
    }
    // `const node = flow.node; matches(reference, node); .. node.parent.kind`: nothing is written of the field, and the parameter that takes the value declares a node. It is one value wherever it is used, and what is stated of it at one use is expected at every other.
    // Of objects alone: whether a number is an int32 is for whoever holds it to show.
    for (uint64_t cast : walker.castsOf(input)) {
        DeclaredType declared = index.value(ValueID(static_cast<uint32_t>(cast))).as<Opcode::Convert>().declared();
        ExpectationForGuesses stated { targetOfConvert(expecting, declared), !declared.isExpected() };
        if (saysNothingForGuesses(types, stated.type) || !types.at(stated.type).hasObjects() || !admitsNoNumberOrBooleanForGuesses(types, stated.type))
            continue;
        stated.admitsNoNumberOrBoolean = true;
        stated.type = narrowedByNamesUsedForGuesses(walker, input, partThatOneTestTellsForGuesses(types, stated.type));
        return stated;
    }
    return expectationByNamesUsedForGuesses(walker, input);
}

ExpectationOfValue expectationOfAnswerOfCallBeingCopied(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return { };
    // A look may stand in the body of a callee, whose values have numbers of their own.
    const BodyIndex& index = walker.currentBodyIndex();
    if (call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return { };
    // All that the caller would be told of the answer behind the call: what the call writes, a cast that is stated of it later, and, where the answer is what the caller's own copy hands back, what is expected of that.
    ExpectationForGuesses expected = findExpectationForGuesses(walker, call.id());
    if (saysNothingForGuesses(walker.types(), expected.type))
        return { };
    return { expected.type, expected.isDeclared };
}

// It is a guess, so it may be kept though more has been proven of a receiver on the road since.
static ExpectationForGuesses findExpectationForGuesses(Walker& walker, ValueID input)
{
    if (Walker::RememberedExpectation remembered = walker.rememberedExpectationOf(input); remembered.isKnown)
        return { remembered.type, remembered.isDeclared, remembered.isHeld, remembered.admitsNoNumberOrBoolean };
    ExpectationForGuesses expectation = findExpectationAnewForGuesses(walker, input);
    refineExpectationOfValue(walker, input, expectation.type, expectation.isDeclared);
    // A test for a closure mentions the function.
    if (expectation.type) {
        bool mayBeTestedFor = true;
        forEachFunctionOf(walker.types(), walker.types().at(expectation.type), scopedLambda<void(FunctionID)>([&](FunctionID named) {
            mayBeTestedFor = mayBeTestedFor && walker.facts().linked().mayBeMentionedBy(walker.unit(), named);
        }));
        if (!mayBeTestedFor)
            expectation = { };
    }
    walker.rememberedExpectationOf(input) = { expectation.type, expectation.isDeclared, expectation.isHeld, true, expectation.admitsNoNumberOrBoolean };
    if (Vector<ValueID, 16>* rememberedForLook = walker.expectationsRememberedForLook())
        rememberedForLook->append(input);
    return expectation;
}

void sayWhatIsExpectedOfComparedOperands(Walker& walker, const BodyValue& equality, Draft& draft)
{
    TypeArena& types = walker.types();
    bool someIsNumber = false;
    bool someIsOther = false;
    equality.forEachOperand([&](ValueID operand, uint32_t) {
        TypeID type = provenTypeOfInputForGuesses(walker, operand);
        if (saysNothingForGuesses(types, type))
            type = findExpectationForGuesses(walker, operand).type;
        TypeID present = type ? types.withoutNullish(type) : TypeID();
        if (!present || isNeverType(present) || saysNothingForGuesses(types, present))
            return;
        (types.at(present).isNumber() ? someIsNumber : someIsOther) = true;
    });
    draft.setExpectsInt32s(someIsNumber && !someIsOther);
}

ExpectationOfValue findExpectation(Walker& walker, ValueID input)
{
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, input);
    return { expectation.type, expectation.isDeclared };
}

// All that the value can be: closures of one function, beside built-ins. Null: anything else.
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

// `total += a * b`: of what nobody knows the product is a number or a big integer. Beside a number the operator throws for a big integer, so a number is what goes on.
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

// Link says who a call enters, not that what is called is a closure of that function. That is the guess, and nobody wrote it.
static ExpectationForGuesses expectationOfCalleeForGuesses(Walker& walker, const BodyValue& call, uint32_t operandIndex)
{
    if (!walker.has(Mechanism::DirectCalls) || !call.operandUses(operandIndex).contains(OperandUse::Calls))
        return { };
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        return { walker.types().forFunction(callee), false };
    // `super(..)`: what is written behind `extends`. Whoever has the class object in hand may lay another function above it, so it is a guess.
    if (constructionsOfParentForGuesses.contains(call.opcode())) {
        LayoutID above = walker.program().at(call.layout()).base;
        FunctionID constructor = above ? walker.program().at(above).constructor : FunctionID();
        if (!constructor)
            return { };
        // Whoever takes the result goes by the rule that a construction hands back an object. Behind the test it is what the body of the constructor returns, called or copied,
        // which the compiler establishes as an object for every class that it lowers. Where nobody has, the closed form stays, which asks.
        TypeID returned = walker.facts().linked().function(constructor).result;
        if (isClassConstructorOf(walker.program().at(constructor).kind) && (!returned || !walker.types().isSubset(returned, TypeTable::object)))
            return { };
        return { walker.types().forFunction(constructor), true };
    }
    // `const { F } = require("./f")`: what comes out of another file's record is held to be what that file puts there.
    if (FunctionID callee = walker.facts().held().calleeAt(walker.currentFunction(), call.id()))
        return { walker.types().forFunction(callee), true, true };

    // `table.handler(x)`: the read has given a value of which Link has proven all that it can be. The test tells the closures from the built-ins, and what it refuses is left to the rows of those.
    if (Reference value = walker.referenceOf(call.operand(operandIndex)); value && value.isValue()) {
        if (FunctionID callee = onlyFunctionBesideBuiltinsForGuesses(walker.types(), walker.types().at(walker.typeOf(value))); callee && walker.facts().linked().mayBeMentionedBy(walker.unit(), callee))
            return { walker.types().forFunction(callee), false };
    }

    // `table.handler(x)`: LinkTables::callsThatFindBuiltins. Where the read has a form of its own the value is at hand: takeApartCallThatFindsBuiltinForGuesses(). Closed, the call reads the method itself.
    if (const LinkedCallThatFindsBuiltin* found = walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), call.id())) {
        Reference value = walker.referenceOf(call.operand(operandIndex));
        if (value && value.isValue() && walker.code().at(value.id()).opcode != Opcode::PropGet && walker.has(Mechanism::BuiltinCalls)) {
            if (found->callee && walker.facts().linked().mayBeMentionedBy(walker.unit(), found->callee))
                return { walker.types().forFunction(found->callee), false, true };
            if (isTestableBuiltin(walker.program().builtins(), found->builtin))
                return { walker.types().forBuiltin(found->builtin), false, true };
        }
    }

    // `const join = Array.prototype.join; .. join.call(list, ",")`: Link lists nobody, and what is written of the value that is called names one built-in. Where nothing locks the place that it was read from, that is a guess as well.
    // `list.join(",")` is one operation in closed code, which reads the method and calls it. A test between the two would take them apart.
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID called = index.valueBehindCasts(call.operand(operandIndex));
    // `isNaN(x)`: a free name is read through the record of its binding and called by value, two operations in closed code too.
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
        // The name is bound to the member of the global object whoever writes its type. The test pays where code of the built-in's own stands behind it: in line, or the operation in front of the four functions that code addresses.
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

// `o.#x`: the class declares the name, and the access throws on whatever has not gone through the construction of the class or of a class below. What is written of `o` counts where it says no less.
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

// ---- The test

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

// The test asks for shape numbers, so it refuses every object of a layout that is in use and has none. What is left of a family without such a layout is no family.
enum class ShapesOfLayoutsForGuesses : uint8_t { SomeLayoutInUseHasNone, NoLayoutHasAny, Testable };
static ShapesOfLayoutsForGuesses askEachLayoutForShapesForGuesses(const Walker&, LayoutSetID);

// The plan is done, so the round over a set is made once for the build: TypeArena::rememberRound(). AOTWalk.cpp has the fifty numbers in front of this one.
static ShapesOfLayoutsForGuesses shapesOfLayoutsForGuesses(const Walker& walker, LayoutSetID layouts)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::FirstOfWalk) + 50;
    static_assert(number < static_cast<uint8_t>(AnswerKeptInArena::CellsOfFieldArm));
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

// The source may declare a type that no object is made as: `ObjectType`, an interface below `Type`, which is what the maker of the objects is written as.
// Then the type speaks of the objects of the nearest types above it, by supertypes and bases, that have any. Null: there are none.
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

// `function nameOf(access: PropertyAccessExpression)`, where the type names three of the four hundred layouts that are written as `Node`: the test asks for those three,
// and what only they have is a slot behind it. A test for the whole family would leave the name a field of some layouts only, which has no typed form.
// `PropertyAccessExpression` names a member below each of `Node`, `Token` and `Identifier`, and the factories are seen to fill that of `Node` alone: the image has an order for it that goes on as they store.
// The test asks for such members where the set has any: a guess, like the order. The others go through the closed arm.
static TypeID partOfWrittenSetThatFactoriesFill(Walker& walker, TypeID expected, LayoutSetID set)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::FirstOfWalk) + 53;
    static_assert(number < static_cast<uint8_t>(AnswerKeptInArena::CellsOfFieldArm));
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
    TypeID widened = widenedToFamily(types, walker.facts().families(), expected);
    // Most of what is written is a family, and nobody goes over its members to hear so.
    if (widened == expected || !types.at(expected).hasObjects())
        return widened;
    Type objects = types.at(types.withOnlyObjects(expected));
    if (objects.hasOnlyLayouts() && !objects.hasOnlyClosures() && mayAccessThroughSetOfLayouts(walker, objects.layouts))
        return partOfWrittenSetThatFactoriesFill(walker, expected, objects.layouts);
    return widened;
}

// Nothing: no test for the type is worth its code.
static std::optional<TestOfGuess> chooseTestOfTypeAnewForGuesses(Walker& walker, TypeID expected)
{
    TypeArena& types = walker.types();
    TypeID tested = widenedForTestOfGuess(walker, expected);
    // Where a member is asked for, undefined and null throw, off a text or a number as off an object: the closed arm does that.
    if (TypeID neitherUndefinedNorNull = types.withoutNullish(tested); !isNeverType(neitherUndefinedNorNull))
        tested = neitherUndefinedNorNull;
    // To find which of the listed texts a text is takes a search.
    tested = types.testablePartOf(types.withLiteralStringsWidened(tested));

    Type type = types.at(tested);
    if (type.isNever() || !type.isJavaScriptValue() || type.hasUnboundedUnknown() || (type.hasObjects() && type.primitives))
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

// It follows from the type, the families and the plan, and every operation that looks at a value asks: it is found once for the build.
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

// Nothing: no test for the type is worth its code, it would tell nothing new of the value, or it would never pass.
static std::optional<TestOfGuess> chooseTestOfGuess(Walker& walker, Reference value, TypeID expected)
{
    if (!value || !value.isValue() || !expected)
        return std::nullopt;

    TypeArena& types = walker.types();
    std::optional<TestOfGuess> ofType = chooseTestOfTypeForGuesses(walker, expected);
    if (!ofType)
        return std::nullopt;
    TestOfGuess chosen = *ofType;

    // A test that the door would cut is not worth its arms.
    if (walker.withoutFunctionsThatMayNotBeMentioned(chosen.tested) != chosen.tested)
        return std::nullopt;
    TypeID proven = walker.typeOf(value);
    if (types.isSubset(proven, chosen.tested) || !types.overlap(proven, chosen.tested) || standsBehindTestOfGuess(walker.code(), value, chosen))
        return std::nullopt;
    return chosen;
}

// `function Response(req) { http.ServerResponse.call(this, req); .. }   util.inherits(Response, http.ServerResponse)`, in a module that may run again: the guards of such a class never begin,
// so nobody answers for what stands above its objects, and a store that may create a field has no typed form on them, nor on a set of layouts that they are among: whoAnswersForWhatStandsAboveObjectsOf().
// What is left where they are taken out of what is expected. They go through the closed arm then.
static TypeID partWhoseClassesHaveGuardsForGuesses(Walker& walker, TypeID expected)
{
    // The round over a set is made once for the build, as that of shapesOfLayoutsForGuesses() is.
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::FirstOfWalk) + 51;
    static_assert(number < static_cast<uint8_t>(AnswerKeptInArena::CellsOfFieldArm));
    TypeArena& types = walker.types();
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || holdsOneMember(type.layouts))
        return expected;
    // Null: all are left, or none.
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

// `location.locals`, location: Node: sixty of four hundred layouts have the name, so no typed form serves all that is expected of the value, or all that is proven of it.
// Whoever asks for the name expects one of the sixty. What is left where the members in use that lack the name are taken out: they go through the closed arm. All have it, or none has: the type as it is.
static TypeID partThatHasNameAskedForGuesses(Walker& walker, TypeID expected, PropertyKeyID name)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::FirstOfWalk) + 54;
    static_assert(number < static_cast<uint8_t>(AnswerKeptInArena::CellsOfFieldArm));
    TypeArena& types = walker.types();
    if (!expected || !name || liesInThread(name))
        return expected;
    Type type = types.at(expected);
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || holdsOneMember(type.layouts))
        return expected;
    // Null: all have the name, or none.
    auto askEachLayout = [&]() -> TypeID {
        const LayoutsByNames& index = walker.facts().layoutsByNames();
        std::span<const LayoutID> layouts = types.listOf(type.layouts);
        Vector<LayoutID, 16> holders;
        index.appendHoldersAmong(name, layouts, holders);
        // `function Node(kind) { .. }`, whose own layout keeps a name that factories append out of line, beside the kinds of nodes below it, which keep it in the cell: no one access serves all.
        // One serves those that keep it in the cell in one form, wherever in the cell: the form that most of them have, and of two such the first.
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

static Reference buildTestOfGuess(Walker& walker, Reference value, const TestOfGuess& chosen, Origin origin)
{
    if (chosen.layouts) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(value);
        test.setLayouts(chosen.layouts);
        test.setExpectsCell(chosen.expectsCell);
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

// ---- The stretch

// WHERE A SLOT IS HELD TO A TYPE A STORE LOOKS AT WHAT IT STORES: typed, it writes the bits as they are, so the value has to be proven to fit. Closed, the stub tests it.
// What the slots take. Null: the store asks nothing of its value, the value fits as it is, or none that fits can arrive.
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

// No parameter of a block can stand for it. Whatever gives several results or opens an iteration is named so by every user, so the first one tells.
static bool mustBeNamedByDefinitionForGuesses(const BodyIndex& index, ValueID value)
{
    std::optional<BodyUser> user = index.firstUser(value);
    if (!user)
        return false;
    switch (user->kind) {
    case BodyUseKind::Operand:
        return namesByDefinitionForGuesses(index.value(user->value), value);
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

static bool isReturnedAtOnceForGuesses(const BodyIndex& index, ValueID call)
{
    // The end of its block is the first to mention it.
    std::optional<BodyUser> user = index.firstUser(call);
    if (!user || user->kind != BodyUseKind::TerminatorOperand)
        return false;
    BodyBlock block = index.block(user->block);
    BodyTerminator end = block.terminator();
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call && block.holds(call);
}

static bool standsInNoStretchOfGuess(const BodyIndex& index, const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (maySuspend(opcode))
        return true;
    // What the walk replaces by operations and blocks of its own, but for a cast, which becomes its operand.
    if (opcodesWithoutEmitter().contains(opcode) && !inputOperationsThatHandOn.contains(opcode))
        return true;
    // Each arm returns what it has built for the call: openGuessWithTest().
    if (isReturnedAtOnceForGuesses(index, operation.id()))
        return false;
    return mustBeNamedByDefinitionForGuesses(index, operation.id());
}

// In its closed form.
static bool mayEndProofOfGuess(Opcode opcode)
{
    static const OpcodeSet operationsThatMayEndProof = opcodesWhere([](Opcode opcode) {
        const Effects& bound = declaredBoundInTable(opcode);
        return !bound.alwaysCompletes() || bound.writes.contains(AbstractHeap::ShapeWords) || operandsMayRunCode(opcode);
    });
    return operationsThatMayEndProof.contains(opcode);
}

// What the end of the block finds by its definition stays behind every stretch: the test whose outcome the edges hand on as proofs, which no parameter can stand for.
static uint32_t placeTakenByEndOfBlockForGuesses(const BodyIndex& index, const BodyBlock& block)
{
    uint32_t place = block.numberOfValues();
    auto keepBehindStretches = [&](ValueID value) {
        if (value && value >= block.firstValue() && block.holds(value))
            place = std::min(place, value.index() - block.firstValue().index());
    };

    BodyTerminator end = block.terminator();
    if (end.isConditionalBranch()) {
        // A proof that nothing reads is handed on to nobody: `if (type.flags & mask)`, `if (!type.members)`.
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            if (!result.parameter || index.useCount(result.parameter))
                keepBehindStretches(result.test);
        });
    }
    return place;
}

// Whoever walks a stretch, the typed run of another guess among them, has asked for no operation behind its end.
static uint32_t limitOfStretchOfGuess(Walker& walker, const BodyBlock& block)
{
    // Every operation of the block asks.
    Walker::PlaceTakenByEndOfBlock& askedLast = walker.placeTakenByEndOfBlockAskedLast();
    if (askedLast.block != block.id())
        askedLast = { block.id(), placeTakenByEndOfBlockForGuesses(walker.currentBodyIndex(), block) };
    uint32_t limit = std::min(askedLast.place, walker.endOfStretchBeingWalked());
    // The edges narrow by the record of the relation, as they do by a test whose outcome they hand on. It is the condition of the branch: where that stands behind the limit, or in another block, nobody asks what it proves.
    BodyTerminator end = block.terminator();
    ValueID condition = end.isConditionalBranch() ? end.operand(0) : ValueID();
    if (condition && condition >= block.firstValue() && block.holds(condition) && condition.index() - block.firstValue().index() < limit && relationThatNarrowsBehind(walker, end))
        limit = condition.index() - block.firstValue().index();
    return limit;
}

// What a guess of its own, opened in the typed run, would prove of another operand of the operation at which this one is opened: `a + b`, where nothing is proven of either.
static TypeID typeThatGuessWouldProveForGuesses(Walker& walker, ValueID input)
{
    Reference built = walker.referenceOf(input);
    ExpectationForGuesses expectation = findExpectationForGuesses(walker, input);
    if (!shouldOpenGuess(walker, built, expectation.type, expectation.isDeclared, UINT32_MAX))
        return { };
    std::optional<TestOfGuess> chosen = chooseTestOfGuess(walker, built, expectation.type);
    return chosen ? walker.types().intersectionOf(walker.typeOf(built), chosen->tested) : TypeID();
}

// `{ kind, parent }`: an allocation fills its slots without a test. What is not proven to fit its slot is defined by name behind the allocation, with all that is written behind it: settleAllocationOfObjectInChoices().
static bool allocationFillsAllItsSlotsForGuesses(Walker& walker, const BodyValue& operation, const TypesOfInputs& types)
{
    auto allocation = operation.as<Opcode::NewObject>();
    std::span<const ValueID> values = operation.variadicOperands();
    if (!walker.plan().layout(allocation.layout()).holdsCompiledObjects() || allocation.fields().size() != values.size())
        return false;
    for (size_t index = 0; index < values.size(); ++index) {
        TypeID held = walker.plan().slot(allocation.layout(), allocation.fields()[index]).type;
        if (held && !walker.types().isSubset(types(values[index]), held))
            return false;
    }
    return true;
}

// What the links of the chain make of the operation with operands of these types. They answer themselves: typedFormOf(), findOnlyCallee().
// `callOfBuiltin`: wouldCallBuiltin() of the same, where the caller has asked.
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
    if (wouldWriteJSONTextOfCall(walker, operation, types))
        return TypedForm::Built;
    FunctionID callee;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operation.operandUses(operandIndex).contains(OperandUse::Calls))
            callee = findOnlyCallee(walker, operation, types(operand));
    });
    // A constructor above that counts its arguments, or keeps the rest of them, takes a list of unknown length as it is: the test would stand in front of two closed calls.
    if (callee && operation.opcode() == Opcode::SuperConstructList && !listOfSuperCallCanBeHandedOver(walker, operation, callee))
        return TypedForm::None;
    return callee ? TypedForm::Built : TypedForm::None;
}

// `list.push(x)`: closed, the call reads the method itself, so a typed read saves nothing by itself. The two are the one operation that they are in the source, and the call is what counts.
static bool isReadOfMethodForItsCallForGuesses(const BodyIndex& index, const BodyValue& read)
{
    if (read.opcode() != Opcode::PropGet)
        return false;
    ValueID user = index.onlyUsingValue(read.id());
    return user && callsOfValuesForGuesses.contains(index.opcodeOf(user)) && !constructionsOfParentForGuesses.contains(index.opcodeOf(user)) && index.value(user).calleeOperand() == read.id();
}

// `members.forEach((symbol, id) => ..)`: the call is handed a closure that was made for it alone. A method of a built-in class that takes one runs it for each element: THE CALL IS A LOOP, wherever it stands.
// Closed, the closure goes to a callee that nobody knows, so it and the scope that it captures are cells of the heap at every call.
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

// ONE OPERATION ALONE. What findStretchOfGuess() counts takes the value, or what such an operation gives, and stands in front of whatever ends every stretch. So many there are at most, and no link of the chain is asked:
// `a + 1` and nothing more of `a`, `list.push(x)` and nothing more of `list`, outside a loop, where two have to count.
static uint32_t mostOperationsThatStretchCountsForGuesses(Walker& walker, Reference value, bool isPassedAlready, const BodyBlock& block, uint32_t begin, uint32_t limit, uint32_t enough)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t first = block.firstValue().index() + begin;
    BitVector mayRestOnProof;
    uint32_t most = 0;
    for (uint32_t place = begin; place < limit && most < enough; ++place) {
        BodyValue operation = walker.asWalked(index.value(ValueID(first + place - begin)));
        Opcode opcode = operation.opcode();
        if (maySuspend(opcode) || (opcodesWithoutEmitter().contains(opcode) && !inputOperationsThatHandOn.contains(opcode)))
            break;
        bool takesWhatMayRestOnProof = false;
        if (opcode != Opcode::ReleaseHint) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                takesWhatMayRestOnProof |= operand.index() >= first ? mayRestOnProof.get(operand.index() - first) : walker.referenceOf(operand) == value;
            });
        }
        if (!takesWhatMayRestOnProof) {
            if (mayEndProofOfGuess(opcode) && !operationsWhoseTypedFormsRunNoCodeForGuesses.contains(opcode))
                break;
            continue;
        }
        mayRestOnProof.set(place - begin);
        if (opcode != Opcode::RequireObjectCoercible && !isReadOfMethodForItsCallForGuesses(index, operation))
            most += opcode == Opcode::Construct || isPassedAlready ? 2 : 1;
    }
    return most;
}

// An operation leans on the proof where it looks at the value, or at a result of which the rows give more with the proof than without it.
// Behind the first, it does not lean where it also looks at what nothing is proven of: it stands behind the rejoin and opens a guess of its own.
// It counts where it is built typed with the proof and closed without it. The first operation has to count, and the stretch ends behind the last that does: what stays closed either way is built once.
static StretchOfGuess findStretchOfGuess(Walker& walker, Reference value, const TestOfGuess& chosen, const BodyBlock& block, uint32_t begin, uint32_t limit)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypingContext typing { walker.program(), types, walker.positions(), TypeLevel::Proven };
    TypeID typeBehindTest = types.intersectionOf(walker.typeOf(value), chosen.tested);
    uint32_t first = block.firstValue().index() + begin;

    // The other operands of the first operation of which nothing is proven, with what is guessed of them.
    Vector<std::pair<ValueID, TypeID>, 2> typesOfGuessesInside;
    index.value(ValueID(first)).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (isOnlyHandedOnForGuesses(index.value(ValueID(first)), operandIndex) || walker.referenceOf(operand) == value || !saysNothingForGuesses(types, provenTypeOfInputForGuesses(walker, operand)))
            return;
        if (TypeID guessed = typeThatGuessWouldProveForGuesses(walker, operand))
            typesOfGuessesInside.append({ operand, guessed });
    });

    // StretchOfGuess::testsOfStoredValues, and behind them the one that is being asked about.
    Vector<TestOfStoredValueOfGuess, 4> testsOfStoredValues;
    // By place in the stretch.
    Vector<TypeID, 32> typesInTypedRun;
    Vector<TypeID, 32> typesInClosedArm;
    BitVector restsOnProof;
    BitVector knownBetter;
    auto typeAmong = [&](const Vector<TypeID, 32>& typesOfStretch, ValueID operand) -> TypeID {
        TypeID type = typesOfStretch[operand.index() - first];
        return type ? type : TypeID(TypeTable::anything);
    };
    auto typeInTypedRun = [&](ValueID operand, uint32_t) -> TypeID {
        if (operand.index() >= first)
            return typeAmong(typesInTypedRun, operand);
        if (walker.referenceOf(operand) == value)
            return typeBehindTest;
        for (auto [guessedOperand, guessed] : typesOfGuessesInside) {
            if (guessedOperand == operand)
                return guessed;
        }
        TypeID proven = provenTypeOfInputForGuesses(walker, operand);
        for (const TestOfStoredValueOfGuess& test : testsOfStoredValues) {
            if (test.value == operand)
                proven = types.intersectionOf(proven, test.tested);
        }
        return proven;
    };
    auto typeInClosedArm = [&](ValueID operand, uint32_t) -> TypeID {
        return operand.index() >= first ? typeAmong(typesInClosedArm, operand) : provenTypeOfInputForGuesses(walker, operand);
    };
    auto typeOfInputInTypedRun = [&](ValueID operand) {
        return typeInTypedRun(operand, 0);
    };
    auto typeOfInputInClosedArm = [&](ValueID operand) {
        return typeInClosedArm(operand, 0);
    };
    TypesOfInputs inputsOfTypedRun(typeOfInputInTypedRun);
    TypesOfInputs inputsOfClosedArm(typeOfInputInClosedArm);

    StretchOfGuess stretch { begin, 0, { } };
    // What a look at a slot has found holds until something may write the field.
    bool slotsAreAsFound = true;
    // The slots into which the stretch has stored so far. Behind the store the slot is filled, as long as a look at it would hold.
    Vector<std::pair<ValueID, PropertyKeyID>, 8> slotsStoredInStretch;
    // The reads of the stretch that find what a look in front of the arms has loaded, each with the access that stands for the look.
    Vector<std::pair<ValueID, ValueID>, 8> readsAnsweredByLooks;
    // The call of the last method that was read off what rests on the proof. In front of it stands what makes its arguments.
    ValueID callAwaited;
    for (uint32_t place = begin; place < limit; ++place) {
        BodyValue operation = walker.asWalked(index.value(ValueID(first + place - begin)));
        Opcode opcode = operation.opcode();
        if (standsInNoStretchOfGuess(index, operation))
            break;

        auto restsOnProofOfGuess = [&](ValueID operand) {
            return operand.index() >= first ? restsOnProof.get(operand.index() - first) : walker.referenceOf(operand) == value;
        };
        bool takesWhatRestsOnProof = false;
        if (opcode != Opcode::ReleaseHint) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                takesWhatRestsOnProof |= restsOnProofOfGuess(operand);
            });
        }
        bool leans = false;
        bool looksAtWhatNothingIsProvenOf = false;
        // A call of a built-in looks at what it is handed: its rows go by the types of the operands. What takes nothing that rests on the proof leans on nothing, whatever it calls.
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
        // `isModifierKind(node.kind)`: the typed run has the copy of the callee, and the closed arm calls: Walker::walkInline(). What the copy makes of its parameter goes by what the run knows of the argument:
        // where the arms met in front of the call, the copy switched on a box.
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
        // `s.charCodeAt(i - 1)`: between the read of a method and its call stands what makes the arguments. What is built typed without the proof too, and runs no code so, ends nothing.
        bool isTypedEitherWay = !leans && operationsWhoseTypedFormsRunNoCodeForGuesses.contains(opcode) && typedFormForGuesses(walker, operation, inputsOfClosedArm) == TypedForm::Built;
        // `source.text.charCodeAt(source.position)`: the read of the position may run a getter. A string stays a string whatever runs, and nothing asks whether it is as its class makes it.
        // No further than the call: whoever went on to the end of the block for every guess would look at each of its places once for each of them.
        bool endsNoProofOfPrimitive = callAwaited && operation.id() < callAwaited && operationsThatEndNoProofOfPrimitiveForGuesses.contains(opcode) && !types.at(chosen.tested).hasObjects();
        if (!leans && !isTypedEitherWay && !endsNoProofOfPrimitive && mayEndProofOfGuess(opcode))
            break;
        if (leans && isReadOfMethodForItsCallForGuesses(index, operation))
            callAwaited = index.onlyUsingValue(operation.id());

        // The test of a value from in front of the stretch stands in the row in front of the arms. One that the stretch makes is typed by what makes it.
        size_t numberOfTestsOfStoredValuesInFront = testsOfStoredValues.size();
        if (leans && storesIntoSlotsForGuesses.contains(opcode) && operation.valueOperand().index() < first && walker.referenceOf(operation.valueOperand()).isValue()) {
            if (TypeID asked = typeAskedOfStoredValueForGuesses(walker, operation, typeInTypedRun(operation.objectOperand(), 0), typeInTypedRun(operation.valueOperand(), 0)))
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
            TypedForm form = typedFormForGuesses(walker, operation, inputsOfTypedRun, mayCallBuiltin ? std::optional(callOfBuiltinInTypedRun) : std::nullopt);
            // `list[0] = a`: at the level Expected the operation asks the list itself for all that it needs and has the closed operation inside it: three hundred bytes a store. That pays where it comes again and again.
            Walker::FormAskedAhead askedAhead = walker.takeFormAskedAhead();
            if (askedAhead.level == KnowledgeLevel::Expected && !walker.isInLoop())
                form = TypedForm::None;
            // `parts[parts.length - 1]`: the row of the read by name says nothing of a length. The operation that takes its place does, and the difference leans on that.
            if (form == TypedForm::Built && opcode == Opcode::PropGet && askedAhead.level == KnowledgeLevel::Proven)
                givenByTypedForm = widestPrimitiveResultOf(askedAhead.opcode);
            // The half of an accessor that is run behind looks counts as running none: most read or store a field. Where one does more the state forgets what the looks have found, and the accesses behind it ask anew.
            runsNoCode = form != TypedForm::BuiltAsCall && operationsWhoseTypedFormsRunNoCodeForGuesses.contains(opcode);
            // `list[i]`: behind the tests of the storage and of the element the read is a load, and what it gives is a number whoever has written the list.
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
                // The chain has just been asked about the first operation as it stands.
                counts = place == begin || typedFormForGuesses(walker, operation, inputsOfClosedArm, callOfBuiltinInClosedArm) == TypedForm::None;
                // `list[i] = list[i] + 1`: the store takes a number as it is held (storeTakesIndexOfProvenList()). Where the arms met in front of it, it took a box of which nobody knows that it holds a number.
                counts |= chosen.asksForElementOfNumbers && opcode == Opcode::ElemSet && restsOnProofOfGuess(operation.valueOperand()) && types.at(typeInTypedRun(operation.valueOperand(), 0)).isNumber();
                // `list[i] / 2`: closed, the operator asks what it is handed whether it is a number, and converts it out of line. Behind the tests it takes a double.
                counts |= chosen.asksForElementOfNumbers && operatorsThatMakeNumbers.contains(opcode);
                // Without the proof the native code converts the operands itself.
                counts |= callOfBuiltinInClosedArm && callOfBuiltinInTypedRun > *callOfBuiltinInClosedArm;
                // `node.kind === 3`, `type.flags & mask`: typed either way. In the typed run it takes the word as it comes out of the slot. Behind the rejoin that word is boxed for the join, and the operator asks at once what it is.
                // Whoever takes a text decides behind the join in what form its parts come.
                goesWithWhatItTakes = !counts && form == TypedForm::Built && familyOf(opcode) == OperationFamily::Operator && opcode != Opcode::ToString;
                // `stack.pop()`, `text.charCodeAt(at)`: closed, a stub reads the method and enters the function of the host. Behind the test the code of the row stands in line.
                // Of a primitive alone: its test is one look, and nothing asks whether it is as its class makes it.
                countsForTwo |= counts && codeOfRowStandsInLine && callOfBuiltinInClosedArm == BuiltinCallAskedAbout::None && !types.at(chosen.tested).hasObjects();
                // `new F(..)`: closed, a stub finds the entry, the run time makes `this`, and nothing is known of what comes back, so every access to the new object is closed too. Behind the test the object is made here under its layout.
                if (opcode == Opcode::Construct && callOfBuiltinInTypedRun == BuiltinCallAskedAbout::None) {
                    FunctionID constructor = findOnlyCallee(walker, operation, typeInTypedRun(operation.calleeOperand(), 0));
                    countsForTwo |= constructor && makesThisOfConstruction(walker, constructor);
                }
                // `super(..)`: closed, the run time finds the constructor above, its entry and the prototype of new.target.
                countsForTwo |= counts && constructionsOfParentForGuesses.contains(opcode);
                break;
            }
            case TypedForm::BuiltBehindFilledSlot:
            case TypedForm::BuiltBehindEmptySlot:
            case TypedForm::BuiltBehindLookAtCell:
            case TypedForm::BuiltBehindFilledSlotAndLookAtCell: {
                // The looks stand in a row in front of the arms, where the value itself is at hand, and what a look further up in the row has loaded: `a.b.c`. A cast becomes its operand.
                ValueID object = operation.objectOperand();
                while (object.index() >= first && inputOperationsThatHandOn.contains(index.opcodeOf(object)))
                    object = index.value(object).operand(0);
                ValueID holder;
                for (auto [read, look] : readsAnsweredByLooks) {
                    if (read == object)
                        holder = look;
                }
                bool isAtHand = object.index() < first ? walker.referenceOf(object) == value : holder && form == TypedForm::BuiltBehindFilledSlot;
                counts = slotsAreAsFound && isAtHand;
                PropertyKeyID key = keyOfAccessToSlot(walker.program(), operation);
                bool isStoredInStretch = slotsStoredInStretch.contains(std::pair { holder, key });
                size_t placeOfLook = stretch.firstAccessesToSlots.findIf([&](const LookAtSlotOfGuess& earlier) {
                    return earlier.holder == holder && keyOfAccessToSlot(walker.program(), walker.asWalked(index.value(earlier.access))) == key;
                });
                bool isLookedAtAlready = form == TypedForm::BuiltBehindLookAtCell || (form != TypedForm::BuiltBehindEmptySlot && isStoredInStretch) || placeOfLook != notFound;
                // `this.x = x` in a constructor: the slot is about to be filled for the first time, and a look at it fails.
                if (!isLookedAtAlready && storesIntoSlotsForGuesses.contains(opcode) && looksAtSlotOfObjectBeingMade(walker, operation))
                    counts = false;
                if (counts && !isLookedAtAlready)
                    stretch.firstAccessesToSlots.append({ operation.id(), holder });
                // What the order word shows holds as long as what a slot shows: until something may run code or change shapes.
                if (counts && (form == TypedForm::BuiltBehindLookAtCell || form == TypedForm::BuiltBehindFilledSlotAndLookAtCell))
                    stretch.storesThatLookAtCell.append(operation.id());
                if (counts && form == TypedForm::BuiltBehindLookAtCell)
                    methodFoundBehindLooks = typeOfMethodFoundBehindLooks(walker, operation, typeBehindTest);
                if (counts && storesIntoSlotsForGuesses.contains(opcode))
                    slotsStoredInStretch.append({ holder, key });
                // A filled slot holds a value of the type that its stores are held to.
                if (counts && form == TypedForm::BuiltBehindFilledSlot && readsOutOfSlotsForGuesses.contains(opcode) && !isStoredInStretch) {
                    readsAnsweredByLooks.append({ operation.id(), placeOfLook != notFound ? stretch.firstAccessesToSlots[placeOfLook].access : operation.id() });
                    foundInFilledSlot = typeFoundInFilledSlot(walker, operation, types.withoutNullish(typeInTypedRun(operation.objectOperand(), 0)));
                    // Where the holder itself is a guess, a wrong one would cost a call by name, which asks nothing, its test.
                    if (chosen.isPassedAlready && saysNothingForGuesses(types, foundInFilledSlot) && isReadOfMethodForItsCallForGuesses(index, operation)) {
                        TypeID seenToArrive = typeSeenToArriveInFilledSlot(walker, operation, types.withoutNullish(typeInTypedRun(operation.objectOperand(), 0)));
                        // A call of what is no object throws, which the closed arm does in its turn.
                        TypeID function = seenToArrive ? types.withOnlyObjects(seenToArrive) : TypeID();
                        FunctionID seen = function && !isNeverType(function) && types.at(function).isObject() ? singleFunctionOf(types, types.at(function)) : FunctionID();
                        // A test for a closure mentions the function, and the flow may never have seen the layout arrive at this read: then nothing leads from here to what its field holds.
                        if (seen && walker.facts().linked().mayBeMentionedBy(walker.unit(), seen)) {
                            foundInFilledSlot = function;
                            stretch.firstAccessesToSlots[placeOfLook != notFound ? placeOfLook : stretch.firstAccessesToSlots.size() - 1].calleeSeenToArrive = function;
                        }
                    }
                    // A call of undefined or null throws, which the closed arm does in its turn. Behind the question the call enters the function: findOnlyCallee().
                    TypeID functions = foundInFilledSlot ? types.withoutNullish(foundInFilledSlot) : TypeID();
                    if (functions && functions != foundInFilledSlot && !isNeverType(functions) && types.at(functions).hasOnlyClosures() && isReadOfMethodForItsCallForGuesses(index, operation)) {
                        foundInFilledSlot = functions;
                        stretch.firstAccessesToSlots[placeOfLook != notFound ? placeOfLook : stretch.firstAccessesToSlots.size() - 1].isCalled = true;
                    }
                }
                break;
            }
            case TypedForm::BuiltOnObjectAsMade: {
                // As a look at a slot: it stands in front of the arms, and what it finds holds until something may change shapes.
                ValueID object = operation.objectOperand();
                while (object.index() >= first && inputOperationsThatHandOn.contains(index.opcodeOf(object)))
                    object = index.value(object).operand(0);
                counts = slotsAreAsFound && object.index() < first && walker.referenceOf(object) == value;
                if (counts) {
                    stretch.looksWhetherValueIsAsMade = true;
                    foundOnObjectAsMade = typeFoundOnObjectAsMade(walker, operation, typeBehindTest);
                    // `set.size` on what is proven a Set: closed, it is a lookup and the call of a native function out of a stub, which saves every register around it. Against that stands a look at one bit.
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
            return { begin, 0, { } };
        if (mayEndProofOfGuess(opcode) && !(counts && runsNoCode) && !isTypedEitherWay)
            slotsAreAsFound = false;

        // Only a later operation of the stretch asks what this one gives.
        std::optional<BodyUser> firstUser = index.firstUser(operation.id());
        bool isReadInStretch = firstUser && (firstUser->kind == BodyUseKind::Operand ? firstUser->value.index() - first < limit - begin : !index.hasOneUse(operation.id()));
        TypeID inClosedArm = isReadInStretch ? typeByRowForGuesses(typing, operation, typeInClosedArm) : TypeID();
        TypeID inTypedRun = isReadInStretch && takesWhatRestsOnProof ? typeByRowForGuesses(typing, operation, typeInTypedRun) : inClosedArm;
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
        if (counts && !isReadOfMethodForItsCallForGuesses(index, operation)) {
            stretch.numberOfTypedOperations += countsForTwo ? 2 : 1;
            stretch.end = place + 1;
        }
        // Nothing but constants stands between it and what the arms hold already: what stays closed either way is built once.
        if (goesWithWhatItTakes && stretch.numberOfTypedOperations) {
            uint32_t reached = std::max(stretch.end, stretch.endOfOperatorsBehind);
            bool standsRightBehind = true;
            for (uint32_t between = reached; between < place; ++between)
                standsRightBehind &= primitiveConstants.contains(index.opcodeOf(ValueID(first + between - begin)));
            if (standsRightBehind)
                stretch.endOfOperatorsBehind = place + 1;
        }
    }
    stretch.testsOfStoredValues = WTF::move(testsOfStoredValues);
    stretch.placesThatTypedRunKnowsBetter = WTF::move(knownBetter);
    if (chosen.asksForElementOfNumbers)
        stretch.placesThatRestOnProof = WTF::move(restsOnProof);
    return stretch;
}

// ---- The arms

static TestOfGuess secondTestOfGuess(const TestOfGuess& first)
{
    TestOfGuess second { first.failingSideAsksFor, { } };
    second.typedRunOpensGuesses = first.failingSideIsAsLikely;
    return second;
}

static constexpr AbstractHeapSet heapsUnderNumberReadOutOfListForGuesses { AbstractHeap::ArrayStorage, AbstractHeap::Elements };

// Once for each kind that holds numbers alone: HasArrayStorage > IsElementPresent > GetArrayElement [isPresent]. Each failing edge leads to `closedBlock`. The walk goes on where the ways meet, with the number noted.
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
        if (!hasStorage)
            return;
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
        if (!test)
            return;
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
        if (!loaded)
            return;
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

// ---- `switch (node.kind)`: the arms meet on a raw integer

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

// The block ends in a Switch over int32 on the value, and behind the operations up to `end` nobody else takes it: no operation of the block, no edge, no block behind.
// Nor what an edge hands on of it: the rest may be anything, and `case 0` takes -0 too, which `1 / tag` tells from 0.
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
        // `case Kind.Call: .. (node as Call).callee`: the front end states that the holder has what the case has compared. That cuts types and reads nothing.
        std::optional<BodyUser> user = index.onlyUser(result.parameter);
        bool onlyCutsHolder = user && user->kind == BodyUseKind::Operand && index.opcodeOf(user->value) == Opcode::NarrowByField && index.value(user->value).operand(OperandIndex::NarrowByField::value) != result.parameter;
        isTakenElsewhere |= !onlyCutsHolder;
    });
    return !isTakenElsewhere;
}

// In the closed arm: the int32 that takes the case which the value takes. A case is compared as `===` compares: what is no number takes none, nor does a fraction, NaN, or what outgrows an int32;
// a double that holds an int32 passes the test for one; -0 equals 0. Whatever takes no case becomes an int32 that no case names. Null: nothing was built.
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

static void openGuessWithTest(Walker& walker, Reference value, const TestOfGuess& chosen, BlockID inputBlock, uint32_t begin, const StretchOfGuess& stretch)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Code& code = walker.code();
    uint32_t first = index.block(inputBlock).firstValue().index() + begin;
    uint32_t end = std::max(stretch.end, stretch.endOfOperatorsBehind);
    uint32_t size = end - begin;
    bool takesInCallReturnedAtOnce = size && isReturnedAtOnceForGuesses(index, ValueID(first + size - 1));
    // Asked in front of the first test: none of them does anything. No way of a closed arm comes back, so nothing has to be kept apart from it.
    bool failingWayBeginsTurnAgain = chosen.admitsAllThatIsExpected && !chosen.failingSideAsksFor && !chosen.failingSideAsksForNothing && walker.mayBeginTurnAgainInClosedCopy();
    bool takesInBranch = !failingWayBeginsTurnAgain && walker.armsTakeBranchForProofOf(inputBlock, begin, end, chosen.isPassedAlready ? Reference() : value, stretch.placesThatTypedRunKnowsBetter);

    // The names under which the stretch takes the value, and how often it mentions what it defines.
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

    // What the closed arm reads is anything, so the typed run boxed its int32 for the meeting, and the Switch behind it asked the box for its tag. The closed arm finds the integer instead.
    ValueID subjectOfSwitch;
    if (ValueID subject = index.terminator(inputBlock).kind() == TerminatorKind::Switch ? index.terminator(inputBlock).operand(0) : ValueID(); !takesInBranch && subject && valuesMentionedLater.contains(subject) && walker.has(Mechanism::UnboxedNumbers)) {
        // The typed run loads it out of a slot of the value that is held to an int32.
        bool isLoadedByLook = stretch.firstAccessesToSlots.containsIf([&](const LookAtSlotOfGuess& look) {
            return look.access == subject && !look.holder;
        });
        TypeID found = isLoadedByLook ? typeFoundInFilledSlot(walker, walker.asWalked(index.value(subject)), walker.types().withoutNullish(walker.types().intersectionOf(walker.typeOf(value), chosen.tested))) : TypeID();
        if (found && !isNeverType(found) && walker.types().at(found).isInt32() && isTakenBySwitchAloneForGuesses(walker, inputBlock, subject, end))
            subjectOfSwitch = subject;
    }

    Origin origin = walker.draftOf(index.value(ValueID(first))).origin();
    origin.isTailCall = false;
    BlockID closedBlock;
    Reference lastTest;
    unsigned numberOfTests = 0;
    // The branch on the last test stands in front of the typed copy of the loop, so what leans on the test alone may stand there too.
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

    // A test of a type or of a shape looks at the value and at words that are written once.
    auto buildTestWhereItMayStand = [&](Reference tested, const TestOfGuess& asked) {
        Walker::InFrontOfTypedCopy inFront(walker, failingWayBeginsTurnAgain && walker.standsInFrontOfTypedCopy(tested));
        return buildTestOfGuess(walker, tested, asked, origin);
    };
    auto buildProofWhereTestWasAsked = [&](Reference tested, Reference test, const TestOfGuess& asked) {
        Walker::InFrontOfTypedCopy inFront(walker, lastTestIsAskedInFront);
        return buildProofOfGuess(walker, tested, test, asked, origin);
    };

    // None of them writes anything, so what held in front of the first holds wherever one fails.
    // No proof and no failure: nothing is left of the value behind the test, and the block behind it has ended as one that is never reached.
    Reference proven = value;
    if (!chosen.isPassedAlready) {
        Reference test = buildTestWhereItMayStand(value, chosen);
        if (!test)
            return;
        goOnWhereTestPasses(test);
        proven = buildProofWhereTestWasAsked(value, test, chosen);
        // Where the arms part for good the proof goes on with the passing way, as a checked name in its state. It is not noted for every guess: where the arms meet, the meet drops it.
        if ((takesInBranch || failingWayBeginsTurnAgain) && proven && proven.isValue() && value.isValue() && !walker.isUnreachable()) {
            Reference last = value;
            while (Reference checked = walker.state().checkedNameOf(last))
                last = checked;
            if (last != proven)
                walker.state().noteCheckedName(last, proven);
        }
    }
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
    // Off undefined and null an access throws, and so does a call of them, which the closed arm does in its turn. Nothing: no test could be built.
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
    // Whatever else the slot holds is called by the closed arm, which reads it again by its name: nothing has run in between, and a filled slot is read without running anything.
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
    // By the access that stands for the look.
    Vector<std::pair<ValueID, Reference>, 4> loadedByLooks;
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
        Reference test = holder ? buildTestOfSlot(walker, walker.asWalked(index.value(look.access)), holder) : Reference();
        if (!test)
            break;
        // Only a definition under a private name asks for an empty slot.
        bool asksForFilledSlot = index.opcodeOf(look.access) != Opcode::PrivateDefine;
        goOnWhereTestPasses(test, asksForFilledSlot);
        if (asksForFilledSlot) {
            // The look has found the slot filled in front of the loop, and no turn writes what the look has read.
            Reference loaded;
            {
                Walker::InFrontOfTypedCopy inFront(walker, lastTestIsAskedInFront);
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
    if (Reference test = stretch.looksWhetherValueIsAsMade && proven && !walker.isUnreachable() ? buildTestOfObjectAsMade(walker, proven, origin) : Reference()) {
        goOnWhereTestPasses(test);
        noteListAsMade(walker, proven);
    }
    if (walker.hasFailed())
        return;
    if (chosen.asksForElementOfNumbers && proven && !walker.isUnreachable()) {
        if (!closedBlock)
            closedBlock = walker.addBlock(BlockFrequency::Rare);
        lastTest = { };
        numberOfTests += 2;
        buildReadOfElementOfNumbersForGuesses(walker, walker.asWalked(index.value(ValueID(first))), proven, origin, closedBlock);
        if (walker.hasFailed())
            return;
    }
    if (!closedBlock && !(failingWayBeginsTurnAgain && numberOfTests))
        return walker.fail("a guess is opened without a test"_s);
    BlockID typedBlock = walker.isUnreachable() ? BlockID() : walker.currentBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.noteGuessOpened();

    // What an arm has to hand on shows only when the other has come through.
    Vector<ArmOfGuess, 2> arms;
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
        ArmOfGuess arm { walker.currentBlock(), { } };
        for (ValueID mentioned : valuesMentionedLater) {
            Reference built = walker.referenceOf(mentioned);
            if (!built || !built.isRecord())
                return walker.fail("an arm of a guess has no value for what is read behind it"_s);
            arm.values.append(built);
        }
        arms.append(WTF::move(arm));
    };

    // Every test is asked in front of the loop: no way of the turn fails.
    if (closedBlock) {
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

    if (typedBlock) {
        walker.continueIn(typedBlock);
        std::optional<Walker::OpenArm> openArm;
        if (chosen.typedRunOpensGuesses)
            openArm.emplace(walker);
        // What the typed run of the second test has made of the values of the stretch was expected of them there, and is not what this run makes of them.
        if (chosen.failingSideIsAsLikely) {
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
        // The arms part for good, each with the branch. What the typed run brings is expected behind it as it is behind a rejoin.
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

    // `const floor = kit.math.floor; noise(); floor(2.5)`: the arm that was walked last has bound `kit.math` to what IT has proven, and the body never mentions that value again, so the meeting binds nothing in its place.
    // Whoever looks back from the call through the read of `floor` would find it, and take the proof of one arm for what holds behind both. So nothing stands for such a value any more: not known.
    // `if (math) return floor(2.5) + kit.bias;`: each arm has ended with the branch of the block, so none comes through and the meeting binds nothing. What the targets read arrives as names on their edges.
    for (uint32_t place = 0; place < size; ++place) {
        if (arms.isEmpty() || !valuesMentionedLater.contains(ValueID(first + place)))
            walker.bind(ValueID(first + place), Reference());
    }

    // As at any join, what arrives under one name keeps it: a constant, a value from in front of the test, all that a single arm brings.
    Vector<size_t, 8> positionsOfParameters;
    for (size_t position = 0; position < valuesMentionedLater.size() && !arms.isEmpty(); ++position) {
        Reference one = arms.first().values[position];
        Reference other = arms.last().values[position];
        Reference met = one;
        if (one != other) {
            // The values' own types: what the state knows beyond them ends where the arms meet.
            TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(one.id()).type, code.at(other.id()).type);
            Representation forms[] { walker.representationOf(one), walker.representationOf(other) };
            met = walker.addParameter(meetingBlock, type, walker.formOfParameter(type, forms));
            positionsOfParameters.append(position);
            // `symbol.links = links; symbol.links.flags = 0;`: the typed run, which is walked last, knows what it brings, and it is the arm that is expected to be taken. So where nothing else is expected of the value, that is.
            TypeID broughtByTypedRun = code.at(other.id()).type;
            if (saysNothingForGuesses(walker.types(), type) && !saysNothingForGuesses(walker.types(), broughtByTypedRun) && !findExpectationForGuesses(walker, valuesMentionedLater[position]).type)
                walker.rememberedExpectationOf(valuesMentionedLater[position]) = { partThatOneTestTellsForGuesses(walker.types(), broughtByTypedRun), false, false, true };
        }
        walker.bind(valuesMentionedLater[position], met);
    }

    // jumpTo() lets leave whatever a parameter becomes a second name of, brings each argument into the parameter's form, and meets the states of the arms.
    Vector<Reference, 8> arguments;
    for (const ArmOfGuess& arm : arms) {
        arguments.shrink(0);
        for (size_t position : positionsOfParameters)
            arguments.append(arm.values[position]);
        walker.continueIn(arm.lastBlock);
        walker.jumpTo(meetingBlock, arguments.span());
        if (walker.hasFailed())
            return;
    }
    // Neither arm comes through: the walk stands nowhere, and what is behind the stretch is not built.
    walker.continueIn(meetingBlock);
}

// `if (node.parent)`, `links.type || ..`, `list[i] && ..`: THE TRUTH OF WHAT IS EXPECTED TO BE AN OBJECT, where nobody behind reads what a test of its shape would prove.
// No more is asked than the answer needs: an object of any shape is true. Whoever asks for the truth expects nothing as well: undefined and null are false, in line. What is neither goes by the closed operation.
// None of the three does anything, so there is no stretch to walk and nothing to meet but the answer.
static void buildTruthOfObjectOrNothing(Walker& walker, const BodyValue& operation, Reference value, BlockID inputBlock, uint32_t begin)
{
    Origin origin = walker.draftOf(operation).origin();
    origin.isTailCall = false;
    auto answerWith = [&](BlockID meetingBlock, bool answer) {
        Draft written(Opcode::Boolean, origin);
        written.setBoolean(answer);
        if (Reference constant = walker.constant(WTF::move(written)))
            walker.jumpTo(meetingBlock, singleElementSpan(constant));
    };
    Reference isObject = buildTestOfGuess(walker, value, TestOfGuess { TypeTable::object, { } }, origin);
    if (!isObject)
        return;
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
    BlockID onTheRest = walker.addBlock(BlockFrequency::Rare);
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

// ---- What the walk asks

// Zero: no number of operations opens it.
// `leaning`: the operation at which the guess would open. One operation pays for the test where its closed form is a call with a lookup or a row of tests behind it and its typed form a load or a compare.
// Arithmetic on a written number does not: the typed form goes by doubles where the stub adds two int32 (TypeScript 6 +0.26 G with it). Calls, keys, relations and equalities buy nothing.
// The read of a method for its call does not either: 0.26% of that program's run for 1.27 G of a large application's build and 0.68 MB of its code.
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

static uint32_t leastOperationsThatOpenGuess(const Walker& walker, Reference value, TypeID expected, bool isDeclaredInSource, std::optional<Opcode> leaning = std::nullopt, bool leaningReadsMethodForItsCall = false)
{
    if (!walker.has(Mechanism::Guesses) || !value || !value.isValue() || !expected)
        return 0;
    if (walker.isInLoop())
        return minimumOperationsPerGuessInLoop;
    // What runs once gains nothing by a test in front of it: AccessEmitter::comesAgain().
    FunctionID function = walker.unit().function;
    if (walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody())
        return 0;
    // A test of a shape is cheap enough for a weaker guess too: what was only seen to arrive.
    Type type = walker.types().at(expected);
    if (type.hasObjects() && !(type.primitives & ~Primitive::UndefinedOrNull) && type.hasOnlyLayouts() && !type.hasOnlyClosures())
        return minimumOperationsPerGuessOfShape;
    // A test for one built-in compares the value with a cell.
    if (isDeclaredInSource && type.isObject() && singleBuiltinOf(walker.types(), type))
        return minimumOperationsPerGuessOfShape;
    if (!isDeclaredInSource)
        return 0;
    return leaning && oneOperationPaysForGuess(walker, expected, *leaning, leaningReadsMethodForItsCall) ? 1 : minimumOperationsPerGuess;
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

// A GUESS OF ONE READ IS ONE OPERATION. `node.kind`, outside a loop, with nothing else in the stretch: the test of the shape, the look at the slot, the load and the closed read on a rare path are what the arms come to as well, byte for byte,
// and behind the rejoin nothing is left of the proof. So the read is built at KnowledgeLevel::Expected and does all that by itself (codegen/AOTGuessedAccessEmitter.h): no blocks, no parameter, no second and third walk of the operation.
static bool testOfGuessStaysOperationOfItsOwn(const Walker& walker, Reference value)
{
    if (!walker.isInLoop())
        return false;
    std::optional<uint32_t> innermost = walker.innermostOpenLoop();
    return !innermost || walker.standsInFrontOfOpenLoop(*innermost, value) || walker.mayBeginTurnAgainInClosedCopy();
}

static bool buildLoneReadOfGuess(Walker& walker, const BodyValue& operation, Reference value, const TestOfGuess& chosen, uint32_t begin, const StretchOfGuess& stretch)
{
    if (operation.opcode() != Opcode::PropGet || stretch.end != begin + 1 || (!chosen.layouts && !chosen.isPassedAlready) || testOfGuessStaysOperationOfItsOwn(walker, value))
        return false;
    // A target of the branch of the block looks at the value again. An operation that tests for itself proves nothing to anybody.
    if (walker.armsTakeBranchForProofOf(walker.currentBodyIndex().blockOf(operation.id()), begin, stretch.end, chosen.isPassedAlready ? Reference() : value, stretch.placesThatTypedRunKnowsBetter))
        return false;
    // `switch (node.kind)`: one operation gives a box, which the Switch asks for its tag. Behind arms the slot's int32 goes into the Switch as it lies.
    if (walker.has(Mechanism::UnboxedNumbers) && isTakenBySwitchAloneForGuesses(walker, walker.currentBodyIndex().blockOf(operation.id()), operation.id(), stretch.end)) {
        TypeID found = typeFoundInFilledSlot(walker, operation, walker.types().withoutNullish(walker.types().intersectionOf(walker.typeOf(value), chosen.tested)));
        if (found && !isNeverType(found) && walker.types().at(found).isInt32())
            return false;
    }
    // The operation makes the tests that would stand in front of the arms, and no others.
    bool looksAtSlot = stretch.firstAccessesToSlots.size() == 1 && stretch.firstAccessesToSlots[0].access == operation.id() && !stretch.firstAccessesToSlots[0].holder && stretch.storesThatLookAtCell.isEmpty();
    bool looksAtCell = stretch.firstAccessesToSlots.isEmpty() && stretch.storesThatLookAtCell.size() == 1 && stretch.storesThatLookAtCell[0] == operation.id();
    if ((!looksAtSlot && !looksAtCell) || stretch.looksWhetherValueIsAsMade || !stretch.testsOfStoredValues.isEmpty())
        return false;
    TypeID guessed = walker.types().intersectionOf(walker.typeOf(value), chosen.tested);
    Reference read = looksAtSlot ? buildReadThatTestsShapeAndSlot(walker, operation, value, guessed, chosen.expectsCell) : buildReadThatTestsShapeAndCell(walker, operation, value, guessed, chosen.expectsCell);
    if (!read)
        return false;
    walker.bind(operation.id(), read);
    walker.noteGuessOpened();
    return true;
}

static bool buildLoneStoreOfGuess(Walker& walker, const BodyValue& operation, Reference value, const TestOfGuess& chosen, uint32_t begin, const StretchOfGuess& stretch)
{
    if (operation.opcode() != Opcode::PropSet || stretch.end != begin + 1 || (!chosen.layouts && !chosen.isPassedAlready) || testOfGuessStaysOperationOfItsOwn(walker, value))
        return false;
    if (!chosen.isPassedAlready && walker.armsTakeBranchForProofOf(walker.currentBodyIndex().blockOf(operation.id()), begin, stretch.end, value, stretch.placesThatTypedRunKnowsBetter))
        return false;
    // The operation makes the tests that would stand in front of the arms, and no others.
    bool looksAtOwnSlotAlone = stretch.firstAccessesToSlots.isEmpty() || (stretch.firstAccessesToSlots.size() == 1 && stretch.firstAccessesToSlots[0].access == operation.id() && !stretch.firstAccessesToSlots[0].holder);
    bool looksAtCellForItselfAlone = stretch.storesThatLookAtCell.isEmpty() || (stretch.storesThatLookAtCell.size() == 1 && stretch.storesThatLookAtCell[0] == operation.id());
    bool testsOwnValueAlone = stretch.testsOfStoredValues.isEmpty() || (stretch.testsOfStoredValues.size() == 1 && stretch.testsOfStoredValues[0].value == operation.valueOperand());
    if (!looksAtOwnSlotAlone || !looksAtCellForItselfAlone || !testsOwnValueAlone || stretch.looksWhetherValueIsAsMade || stretch.orderWordAskedFor)
        return false;
    if (!buildStoreThatTestsForItself(walker, operation, value, walker.types().intersectionOf(walker.typeOf(value), chosen.tested), chosen.expectsCell))
        return false;
    walker.noteGuessOpened();
    return true;
}

// `signature.parameters[pos]`, `declarations[0]`, outside a loop, off what is written a list, where too little leans on the list for arms. Code in line is 38 bytes a read more than the call of a stub, at sixty thousand reads of a large application.
// So the read stays a call of five bytes, of another stub: the closed read asks first whether the key is a text and looks for a name; this one looks for an element of a list first, and keeps the registers of the code around it.
static bool buildLoneReadOfElementOfGuess(Walker& walker, const BodyValue& operation, ValueID operand, Reference value, const ExpectationForGuesses& expectation)
{
    if (walker.isInLoop() || operation.opcode() != Opcode::ElemGet || operand != operation.objectOperand() || !expectation.type || !expectation.isDeclared || !walker.has(Mechanism::TypedOperations))
        return false;
    TypeArena& types = walker.types();
    Type expected = types.at(types.withoutNullish(expectation.type));
    const ClassPart* onlyClass = expected.isObject() ? singleBuiltinClassOf(types, expected) : nullptr;
    if (!onlyClass || onlyClass->builtinClass != BuiltinClasses::Array)
        return false;
    // Only a number is an index.
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

// `return join.call(list, ",")`: nothing meets behind the call that a ReturnCall returns, so it stands in no stretch. Native code comes back to its caller: behind the test the unit returns what the built-in gave,
// and where the test fails the call leaves as it is written. In a copy that does not return in place of the unit the two meet behind the call.
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

// `overrides()`: of the variable nothing is proven; it starts with one function, and stores behind the start are seen to bring one other (Facts::findFunctionGivenTo()). Each is tested for and called directly; whatever else is there is called by value.
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
    // What is proven to be closures is callDirectly()'s.
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
        // Nobody knows which of the two a run meets more often. Each is one compare away.
        tested = types.forFunction(given->atStart ? given->atStart : given->function);
        if (given->atStart)
            testedOnFailingSide = walker.withoutFunctionsThatMayNotBeMentioned(types.forFunction(given->function)) == types.forFunction(given->function) ? types.forFunction(given->function) : TypeID();
    }
    // What runs once gains nothing by a test in front of it.
    FunctionID function = walker.unit().function;
    if (walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody())
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

// THE CLOSED ROAD: nothing is expected of any value that the operation looks at, so the round over its operands below opens nothing: it is asked here, in front of all else that the round needs.
// What is expected of a value is kept with the value, so whoever goes on pays a look at what is kept. A call goes by who is listed for it, a private name says whose it is,
// and an access by name whose receiver is proven to be something may lack nothing but a look at its slot.
static bool nothingIsExpectedOfOperandsForGuesses(Walker& walker, const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (accessesUnderPrivateNamesForGuesses.contains(opcode))
        return false;
    if (accessesToSlotsForGuesses.contains(opcode) && !saysNothingForGuesses(walker.types(), provenTypeOfInputForGuesses(walker, operation.objectOperand())))
        return false;
    bool isCall = callsOfValuesForGuesses.contains(opcode);
    bool nothingIsExpected = true;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!nothingIsExpected || isOnlyHandedOnForGuesses(operation, operandIndex))
            return;
        Reference value = walker.referenceOf(operand);
        if (value && value.isValue())
            nothingIsExpected = !(isCall ? expectationOfCalleeForGuesses(walker, operation, operandIndex) : expectationOfOperandForGuesses(walker, operation, operand)).type;
    });
    return nothingIsExpected;
}

// WHAT RUNS ONCE GAINS NOTHING BY A TEST IN FRONT OF IT: leastOperationsThatOpenGuess() answers zero there whatever is expected, and every road of openGuessAt() asks shouldOpenGuess() in front of its first build().
static bool noGuessIsOpenedWhereWalkStandsForGuesses(const Walker& walker)
{
    if (walker.isInLoop())
        return false;
    FunctionID function = walker.unit().function;
    return walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody();
}

static bool isReadOfVariableForGuesses(const BodyIndex& index, ValueID value)
{
    value = index.valueBehindCasts(value);
    while (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::CheckInitialized)
        value = index.valueBehindCasts(index.value(value).operand(0));
    return value && !index.isParameter(value) && (index.opcodeOf(value) == Opcode::GetEnvironmentField || index.opcodeOf(value) == Opcode::GetModuleVariable);
}

// All that is expected of the operand passes the test. A written type that holds references holds undefined and null as well: off the two an access throws, and so does a call of them, which nobody expects either.
static bool testAdmitsAllThatIsExpectedForGuesses(Walker& walker, const BodyValue& operation, ValueID operand, uint32_t operandIndex, TypeID expected, TypeID tested)
{
    static constexpr OpcodeSet accessesThatThrowOffNothing { Opcode::PropGet, Opcode::PropSet, Opcode::ElemGet, Opcode::ElemSet };
    static constexpr OpcodeSet accessesByName { Opcode::PropGet, Opcode::PropSet };
    Opcode opcode = operation.opcode();
    bool throwsOffNothing = accessesThatThrowOffNothing.contains(opcode)
        ? operand == operation.objectOperand() && !(accessesByName.contains(opcode) && operation.isFreeName())
        : callsOfValuesForGuesses.contains(opcode) && operation.operandUses(operandIndex).contains(OperandUse::Calls);
    TypeArena& types = walker.types();
    // `let pos: number;` holds undefined until the first store, so what is written of a binding holds it too. The checker lets no operator on numbers take what may be undefined or null: nobody expects the two there.
    static constexpr OpcodeSet operatorsOnNumbers = allRelations | allBitwiseOperators | operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative });
    bool takesNumbersAlone = operatorsOnNumbers.contains(opcode) && types.at(types.withoutNullish(expected)).hasOnlyPrimitives(Primitive::Number | Primitive::Absent);
    return types.isSubset(throwsOffNothing || takesNumbersAlone ? types.withoutNullish(expected) : expected, tested);
}

// `list[i]`: the class of the list is proven, numbers alone are written of its elements, and the position is an integer. Of what it holds nothing is proven: whoever has the list may have stored anything. The kind of its storage says what it holds.
// No effect ends what is noted of a fresh list, and an append that the state follows changes its kind in silence: such a list is left to the forms that it has.
bool openGuessAtElementOfNumbersOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !walker.has(Mechanism::Folding))
        return false;
    Reference list = walker.referenceOf(read.objectOperand());
    Reference position = walker.referenceOf(read.propertyOperand());
    if (!list || !list.isValue() || !position)
        return false;
    TypeID typeOfList = walker.typeOf(list);
    if (!readTakesIndexOfProvenList(walker.types(), typeOfList, walker.typeOf(position)))
        return false;
    // The kind is a guess, so what is only expected of the list may name it: what is proven of a parameter need not keep what is written of the elements.
    if (!isDeclaredToHoldNumbersAlone(walker.program(), walker.types(), typeOfList)) {
        TypeID expected = findExpectationForGuesses(walker, read.objectOperand()).type;
        if (!expected || !isDeclaredToHoldNumbersAlone(walker.program(), walker.types(), walker.types().withoutNullish(expected)))
            return false;
    }
    if (findKnown(walker, KnownNote::NumberReadOutOfList, list, position))
        return false;
    // A row in front of the loop has asked the list for all of it.
    if (findListTestedInFrontOfLoop(walker, read))
        return false;
    // Link has seen all that comes out of the read, and the read is typed by that.
    if (TypeID proven = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id()); proven && !saysNothingForGuesses(walker.types(), proven))
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
    StretchOfGuess stretch = findStretchOfGuess(walker, list, chosen, block, begin, limit);
    if (stretch.numberOfTypedOperations < least)
        return false;
    // WHERE THE ARMS MEET A NUMBER IS BOXED: the closed arm brings what nobody knows. `s += list[i]`: the sum goes round the loop in a box, and the typed arm would unbox it, add, and box it again with the question for an int32.
    // So whatever the typed run knows more of than the closed arm is used up in the stretch. `list[i] / 2` is a number in both arms, and held as a double in both.
    uint32_t first = block.firstValue().index();
    // `line += list[i] + ","`: the writer of texts asks a box for its tag, and writes an int32 without a call. Of a double it makes a string by the run time.
    for (uint32_t place = begin + 1; place < stretch.end; ++place) {
        BodyValue operation = index.value(ValueID(first + place));
        if (!operationsThatJoinTexts.contains(operation.opcode()))
            continue;
        bool joinsNumbersAlone = true;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            // What the stretch itself makes: the element, what an operator has made of numbers, a number that is written.
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
    for (uint32_t place = begin; place < stretch.end; ++place) {
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
    return true;
}

// Behind the last operation of the block, in front of `limit`, that takes what the operation at `place` gives, itself or through others.
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

// `table.handler(x)`: LinkTables::callsThatFindBuiltins. The read of the method and its call are one operation in closed code, and a typed read saves nothing by itself. Here the call has something to ask the value for:
// so behind a look at the slot the value is loaded, and the stretch from the read to the call is walked with it. An empty slot: the call as written.
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

constinit const OpcodeSet opcodesThatOpenGuessAtTakes = operationsThatOpenGuesses;

GuessOpened openGuessAtOfItsOpcodes(Walker& walker, const BodyValue& operation)
{
    if (!operationsThatOpenGuesses.contains(operation.opcode()))
        return GuessOpened::None;
    if (guessThatOperandBesideStringHasText(walker, operation))
        return GuessOpened::WithArms;
    if (noGuessIsOpenedWhereWalkStandsForGuesses(walker))
        return GuessOpened::None;
    if (forwardCallBehindTestOfCallee(walker, operation) || openGuessAtCalleeOfReturnedCall(walker, operation) || openGuessOfKeyOrders(walker, operation) || callGetterExpectedAtRead(walker, operation))
        return GuessOpened::WithArms;
    if (takeApartCallThatFindsBuiltinForGuesses(walker, operation))
        return GuessOpened::WithArms;
    if (callFunctionGivenToVariable(walker, operation))
        return GuessOpened::WithArms;
    if (nothingIsExpectedOfOperandsForGuesses(walker, operation))
        return GuessOpened::None;

    const BodyIndex& index = walker.currentBodyIndex();
    // The block being walked, without a search for it.
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(operation.id()))
        block = index.block(index.blockOf(operation.id()));
    uint32_t begin = operation.id().index() - block.firstValue().index();
    uint32_t limit = limitOfStretchOfGuess(walker, block);
    if (begin >= limit)
        return GuessOpened::None;

    // The layouts of the receiver are proven, and all that the access lacks is a look at its slot. That the property is there is what the source says.
    // Or the class of the receiver is proven, and all that the read lacks is a look at whether the object is as its class makes it. That the class answers to the name is what the source says.
    // Or all that a store lacks beside that is a test of what it stores.
    // No access by name has a typed form on a receiver of which nothing is proven. A private name says whose it is.
    bool mayHaveTypedForm = accessesToSlotsForGuesses.contains(operation.opcode())
        && (accessesUnderPrivateNamesForGuesses.contains(operation.opcode()) || !saysNothingForGuesses(walker.types(), provenTypeOfInputForGuesses(walker, operation.objectOperand())));
    TypeID askedOfStoredValue;
    if (mayHaveTypedForm && storesIntoSlotsForGuesses.contains(operation.opcode()) && walker.referenceOf(operation.valueOperand()).isValue())
        askedOfStoredValue = typeAskedOfStoredValueForGuesses(walker, operation, provenTypeOfInputForGuesses(walker, operation.objectOperand()), provenTypeOfInputForGuesses(walker, operation.valueOperand()));
    TypedForm formAsItStands = mayHaveTypedForm ? typedFormOf(walker, operation, [&](ValueID input) {
        TypeID proven = provenTypeOfInputForGuesses(walker, input);
        return askedOfStoredValue && input == operation.valueOperand() ? walker.types().intersectionOf(proven, askedOfStoredValue) : proven;
    }) : TypedForm::None;
    bool lacksOnlyFilledSlot = formAsItStands == TypedForm::BuiltBehindFilledSlot || formAsItStands == TypedForm::BuiltBehindEmptySlot || formAsItStands == TypedForm::BuiltOnObjectAsMade
        || formAsItStands == TypedForm::BuiltBehindLookAtCell || formAsItStands == TypedForm::BuiltBehindFilledSlotAndLookAtCell || (askedOfStoredValue && formAsItStands == TypedForm::Built);

    GuessOpened opened = GuessOpened::None;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (opened != GuessOpened::None || isOnlyHandedOnForGuesses(operation, operandIndex))
            return;
        Reference value = walker.referenceOf(operand);
        if (!value || !value.isValue())
            return;

        ExpectationForGuesses expectation;
        std::optional<TestOfGuess> chosen;
        if (lacksOnlyFilledSlot) {
            if (operand != operation.objectOperand())
                return;
            expectation = { walker.typeOf(value), true };
            chosen = TestOfGuess { expectation.type, { }, true };
        } else {
            expectation = callsOfValuesForGuesses.contains(operation.opcode())
                ? expectationOfCalleeForGuesses(walker, operation, operandIndex)
                : expectationOfOperandForGuesses(walker, operation, operand);
            // In the copy of a callee the looks list no guess: the walk opens it here, and asks for what they have counted on: forEachTypeThatGuessWouldProveAt().
            if (expectation.type && walker.standsInTurnThatLooksCountOn() && !callsOfValuesForGuesses.contains(operation.opcode())) {
                ExpectationOfNumber found { expectation.type, expectation.isDeclared, expectation.isHeld };
                expectation.type = (isReadOfVariableForGuesses(index, operand) ? numbersExpectedOf(walker, operand, found) : numbersExpectedOfSlotThatTurnsStep(walker, operand, found)).type;
            }
            if (accessesUnderPrivateNamesForGuesses.contains(operation.opcode()) && operand == operation.objectOperand())
                expectation = expectationOfHolderOfPrivateNameForGuesses(walker, operation, expectation);
            else if (accessesToSlotsForGuesses.contains(operation.opcode()) && operand == operation.objectOperand() && !index.isParameter(operand) && index.opcodeOf(operand) == Opcode::NarrowByField) {
                TypeID narrowed = narrowedByNameAskedForGuesses(walker, expectation.type, operation.key());
                // Several of the members that the tag leaves have the name. Such a set stands in no table: it is asked member by member, and seldom answers by one slot. What is expected of the holder in front of the test stands.
                bool isOneLayout = narrowed && walker.types().at(narrowed).hasOnlyLayouts() && holdsOneMember(walker.types().at(narrowed).layouts);
                if (isOneLayout)
                    expectation.type = narrowed;
                else
                    expectation = findExpectationForGuesses(walker, index.value(operand).as<Opcode::NarrowByField>().valueOperand());
            }
        }
        // Where no number of operations would open it, the stretch is not looked at. None counts for more than two.
        uint32_t least = leastOperationsThatOpenGuess(walker, value, expectation.type, expectation.isDeclared, operation.opcode(), isReadOfMethodForItsCallForGuesses(index, operation));
        // `list.push(x)`, one call, outside of loops, and the walk has PROVEN that it is a list: one bit of the cell is asked, and behind it stands the method's own code, in line or entered with raw operands,
        // in the place of two stubs that ask for the class, for the bit and for the kind of storage. A table's method is entered by the same stub either way: there the look buys nothing.
        if (least > 1 && formAsItStands == TypedForm::BuiltOnObjectAsMade && isReadOfMethodForItsCallForGuesses(index, operation)) {
            const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), walker.types().at(walker.types().withoutNullish(expectation.type)));
            if (onlyClass && onlyClass->builtinClass == BuiltinClasses::Array)
                least = 1;
        }
        // Of a Map and a Set: behind the tests the method of the library is copied to where it is called, the closure's body into its loop, and the closure is never made.
        bool callIsLoopOverTable = false;
        if (expectation.isDeclared && walker.has(Mechanism::Guesses) && walker.has(Mechanism::Inlining) && callOfMethodIsHandedClosureMadeForItForGuesses(index, operation)) {
            const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), walker.types().at(walker.types().withoutNullish(expectation.type)));
            // `table.forEach(f, size(table) > 1 ? a : b)`: the call stands behind a branch, in another block. A stretch ends in the block in which it begins, below the limit, and holds nothing that stands in no stretch.
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
        if ((least > 1 || isReadOfMethodForItsCallForGuesses(index, operation)) && mostOperationsThatStretchCountsForGuesses(walker, value, lacksOnlyFilledSlot, block, begin, limit, least) < least)
            return buildAsOneOperationOrLeaveClosed();
        if (!chosen)
            chosen = chooseTestOfGuess(walker, value, expectation.type);
        StretchOfGuess stretch;
        if (chosen)
            stretch = findStretchOfGuess(walker, value, *chosen, block, begin, limit);
        if (TypeID part = lacksOnlyFilledSlot || !chosen ? expectation.type : partWhoseClassesHaveGuardsForGuesses(walker, expectation.type); part != expectation.type) {
            if (std::optional<TestOfGuess> testOfPart = chooseTestOfGuess(walker, value, part)) {
                StretchOfGuess stretchOfPart = findStretchOfGuess(walker, value, *testOfPart, block, begin, limit);
                if (stretchOfPart.numberOfTypedOperations > stretch.numberOfTypedOperations) {
                    chosen = testOfPart;
                    stretch = WTF::move(stretchOfPart);
                }
            }
        }
        // No test for all that is expected opens anything here, be it that the value has passed it already. The members that have the name are asked for.
        // Of what the source declares anywhere, of what was only seen to arrive or is held in a loop alone, as every weaker guess.
        bool asksForNameOfSomeMembers = ((expectation.isDeclared && !expectation.isHeld) || walker.isInLoop()) && !lacksOnlyFilledSlot && stretch.numberOfTypedOperations < least && accessesToSlotsForGuesses.contains(operation.opcode())
            && !accessesUnderPrivateNamesForGuesses.contains(operation.opcode()) && operand == operation.objectOperand();
        // `const step = it.next(); step.done`: what a callee that nobody knows has answered with may well be an object of the engine, which no test for layouts lets pass. Only what is written speaks for the members,
        // and for some of them it says less than for all.
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
        // `text.startsWith(roots[i] + "/")`: the test that alone takes the join at the end of the stretch is walked with it: in the typed arm the join is never made, buildTestAgainstJoinOfTexts().
        if (stretch.end > begin) {
            uint32_t firstValue = block.firstValue().index();
            if (ValueID test = findTestOfEndThatTakesJoinApart(walker, index.value(ValueID(firstValue + stretch.end - 1)), value); test && test.index() < firstValue + limit)
                stretch.end = test.index() + 1 - firstValue;
        }
        // A stretch counts what is built typed with the proof and closed without it, and a method of the library, which is entered either way, counts for nothing. Here the arm is wanted for what the copy of the method makes of the closure.
        if (callIsLoopOverTable && stretch.numberOfTypedOperations < least) {
            stretch.end = index.onlyUsingValue(operation.id()).index() + 1 - block.firstValue().index();
            stretch.numberOfTypedOperations = least;
        }
        if (stretch.numberOfTypedOperations < least)
            return buildAsOneOperationOrLeaveClosed();
        chosen->expectsCell = expectation.admitsNoNumberOrBoolean;
        chosen->admitsAllThatIsExpected = testAdmitsAllThatIsExpectedForGuesses(walker, operation, operand, operandIndex, expectation.type, chosen->tested);
        // The closure is asked for first, and where that fails the built-in.
        if (const LinkedCallThatFindsBuiltin* found = callsOfValuesForGuesses.contains(operation.opcode()) ? walker.facts().held().findCallThatFindsBuiltin(walker.currentFunction(), operation.id()) : nullptr) {
            if (found->callee && chosen->tested == walker.types().forFunction(found->callee) && operand == operation.calleeOperand() && isTestableBuiltin(walker.program().builtins(), found->builtin)) {
                chosen->failingSideAsksFor = walker.types().forBuiltin(found->builtin);
                chosen->failingSideIsAsLikely = true;
            }
            // What the closed arm gives is anything. Each arm goes on to the last operation that takes the answer, so that what the typed runs know of it is not lost where they meet the closed one.
            stretch.end = std::max(stretch.end, endOfWhatTakesResultForGuesses(walker, block, begin, limit));
        }
        if (operation.opcode() == Opcode::ToBoolean && !chosen->isPassedAlready && stretch.end == begin + 1 && walker.types().at(chosen->tested).isObject()) {
            if (chosen->layouts && !walker.armsTakeBranchForProofOf(block.id(), begin, stretch.end, value, stretch.placesThatTypedRunKnowsBetter) && !standsBehindTestOfGuess(walker.code(), value, TestOfGuess { TypeTable::object, { } })) {
                buildTruthOfObjectOrNothing(walker, operation, value, block.id(), begin);
                opened = GuessOpened::WithArms;
                return;
            }
            chosen->failingSideAsksForNothing = true;
        }
        // One operation that tests for itself does the closed operation where its test fails, and a turn that may begin again does not.
        if (!(chosen->admitsAllThatIsExpected && walker.mayBeginTurnAgainInClosedCopy()) && (buildLoneReadOfGuess(walker, operation, value, *chosen, begin, stretch) || buildLoneStoreOfGuess(walker, operation, value, *chosen, begin, stretch))) {
            opened = GuessOpened::AsOneOperation;
            return;
        }
        openGuessWithTest(walker, value, *chosen, block.id(), begin, stretch);
        opened = GuessOpened::WithArms;
    });
    return opened;
}

void forEachTypeThatGuessWouldProveAt(Walker& walker, const BodyValue& operation, const TypesOfInputs& typesOfInputs, NOESCAPE const ScopedLambda<void(ValueID operand, TypeID expected, TypeID proven)>& functor)
{
    if (!operationsThatOpenGuesses.contains(operation.opcode()) || accessesUnderPrivateNamesForGuesses.contains(operation.opcode()))
        return;
    TypeArena& types = walker.types();
    // The looks of one preview remember among themselves, and forget behind the preview.
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
        // `let pos: number`, a variable that closures share, of which Link has seen whole numbers alone. The failing side is the other copy: no arm, nothing comes back boxed, so an int32 is asked for where one is likely.
        // Of a read of a variable alone: what the turn itself has computed is typed as it is, and a test on it would only stand in the way of the turns.
        if (isReadOfVariableForGuesses(walker.currentBodyIndex(), operand))
            expectation.type = numbersExpectedOf(walker, operand, { expectation.type, expectation.isDeclared, expectation.isHeld }).type;
        else
            expectation.type = numbersExpectedOfSlotThatTurnsStep(walker, operand, { expectation.type, expectation.isDeclared, expectation.isHeld }).type;
        std::optional<TestOfGuess> chosen = chooseTestOfTypeForGuesses(walker, expectation.type);
        // Whatever is expected and fails would leave the typed copy for good.
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
    Reference test;
    {
        Walker::InFrontOfTypedCopy inFront(walker, walker.standsInFrontOfTypedCopy(value));
        test = buildTestOfGuess(walker, value, *chosen, origin);
    }
    if (!test)
        return;
    bool isAskedInFront = walker.branchInFrontOfTypedCopy(test, true);
    if (!isAskedInFront) {
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(BlockFrequency::Rare);
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
    Reference last = value;
    while (Reference checked = walker.state().checkedNameOf(last))
        last = checked;
    if (last != proven)
        walker.state().noteCheckedName(last, proven);
}

TypeID typeThatLookAtCalledSlotWouldProve(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    TypeArena& types = walker.types();
    if (!typeOfObject || !isReadOfMethodForItsCallForGuesses(walker.currentBodyIndex(), read))
        return { };
    TypeID seenToArrive = typeSeenToArriveInFilledSlot(walker, read, types.withoutNullish(typeOfObject));
    TypeID function = seenToArrive ? types.withOnlyObjects(seenToArrive) : TypeID();
    FunctionID seen = function && !isNeverType(function) && types.at(function).isObject() ? singleFunctionOf(types, types.at(function)) : FunctionID();
    return seen && walker.facts().linked().mayBeMentionedBy(walker.unit(), seen) ? function : TypeID();
}

bool guessesMayTypeOperationInCleanTurn(Walker& walker, const BodyValue& operation, const TypesOfInputs& typesOfInputs)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Opcode opcode = operation.opcode();
    if (callsOfValuesForGuesses.contains(opcode)) {
        // Who is entered is known, held or guessed: what the callee does shows where it is looked at.
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
    // As of a call: what the half does shows where it is looked at.
    return halfRunBehindLooks || (effects && !effects->mayRunOtherCode());
}

bool askGuardsAboutDeclaredMember(Walker& walker, const BodyValue& operation)
{
    if (!walker.has(Mechanism::TypedOperations))
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
    if (!type.hasObjects() || !type.hasOnlyLayouts() || type.hasOnlyClosures() || shapesOfLayoutsForGuesses(walker, type.layouts) != ShapesOfLayoutsForGuesses::Testable)
        return false;
    if (!recordsDeclareMemberAboveObjectsOf(walker, objects, operation.key()))
        return false;
    Draft question = walker.draftOf(operation);
    question.setReceivers(type.layouts);
    walker.bind(operation.id(), walker.build(WTF::move(question)));
    walker.noteGuessOpened();
    return true;
}

// Closed, the question is a call of a stub with a look into two tables behind it.
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

// The values that are not proven to fit their slots are tested in a row. All pass: the allocation lists all that is written. One fails: the allocation as it is built without the tests.
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
        TypeID held = walker.plan().slot(allocation.layout(), allocation.fields()[place]).type;
        Reference value = walker.referenceOf(values[place]);
        TypeID proven = value ? walker.typeOf(value) : TypeID();
        if (!held || !proven || types.isSubset(proven, held))
            continue;
        if (!value.isValue() || !types.overlap(proven, held) || standsBehindTestOfGuess(walker.code(), value, TestOfGuess { held, { } }))
            return false;
        // From here on each field would be defined by name.
        if (!stretch.numberOfTypedOperations)
            stretch.numberOfTypedOperations = static_cast<uint32_t>(values.size() - place);
        stretch.testsOfStoredValues.append({ values[place], held });
    }
    if (stretch.testsOfStoredValues.isEmpty())
        return false;

    TestOfStoredValueOfGuess first = stretch.testsOfStoredValues[0];
    stretch.testsOfStoredValues.removeAt(0);
    Reference value = walker.referenceOf(first.value);
    // A definition by name tests the value in its stub, so one of them pays for the test.
    if (!shouldOpenGuess(walker, value, first.tested, true, std::max(stretch.numberOfTypedOperations, minimumOperationsPerGuess)))
        return false;
    openGuessWithTest(walker, value, TestOfGuess { first.tested, { } }, block.id(), begin, stretch);
    return true;
}

// ---- In front of a loop that is built twice

// A cast becomes its operand.
static ValueID valueBehindCastsOfLoopForGuesses(const BodyIndex& index, ValueID value, uint32_t firstValueOfLoop)
{
    while (value.index() >= firstValueOfLoop && !index.isParameter(value) && inputOperationsThatHandOn.contains(index.opcodeOf(value)) && index.value(value).operand(0))
        value = index.value(value).operand(0);
    return value;
}

// `i < n`: beside a number a relation asks a value of which nothing is known in line, and converts what is no number, which may run code. So the look at the loop has to count on any code.
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

// The types of the look hold in every turn. An operation counts where it is built typed with the proof and closed with the types of the look alone, or where only the proof says that it runs no code.
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
                // The guard at the entry lets undefined and null pass. So each turn has the check that traps in front of the access, and what hangs on the check stays in the loop.
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

Reference buildTestInFrontOfLoop(Walker& walker, const GuessInFrontOfLoop& guess, Origin origin)
{
    return buildTestOfGuess(walker, guess.value, TestOfGuess { guess.tested, guess.layouts }, origin);
}

Reference buildProofInFrontOfLoop(Walker& walker, const GuessInFrontOfLoop& guess, Reference test, Origin origin)
{
    return buildProofOfGuess(walker, guess.value, test, TestOfGuess { guess.tested, guess.layouts }, origin);
}

// ---- The guard at the entry of a function

OperatorForm formAllowedForTypedOperator(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);

namespace {

struct ParameterOfGuard {
    uint32_t entryParameter { 0 };
    // What the guard lets through.
    TypeID tested;
    // What is proven of the parameter in front of the guard, and behind it.
    TypeID inFrontOfGuard;
    TypeID behindGuard;
    bool isLeanedOn { false };
    // With the primitives that stand beside the objects in what is written or held. Null: there are none, or no test tells the whole.
    TypeID testedWithPrimitives;
    bool isToldApartByBody { false };
    // Of undefined and null, which were cut away with those primitives: what a test of the body asks for by itself.
    uint16_t nullishAskedFor { 0 };
};

struct WorkerOfParameterGuards {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WorkerOfParameterGuards);

    WorkerOfParameterGuards(const WalkInputs& inputs, TypeTable& table)
        : types(table, ArenaKeeps::InProgram)
        , walker(inputs, types, inputs.arrivals)
    {
    }

    TypeArena types;
    Walker walker;
};

} // anonymous namespace

// The ramp of a coroutine hands its parameters on to a body that is entered again and again, the body of a module takes none, and whoever runs an initializer is making `this`: each keeps its one body.
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

// What is written holds undefined and null wherever it holds a reference, so the two pass. Null: no test for the type is worth its code, it would tell nothing new of the parameter, or it would never pass.
static TypeID typeTestedByParameterGuard(Walker& walker, TypeID expected, TypeID inFrontOfGuard)
{
    TypeArena& types = walker.types();
    if (saysNothingForGuesses(types, expected) || describesBagOfOptionsForGuesses(walker, expected) || !types.at(inFrontOfGuard).isJavaScriptValue())
        return { };
    // To find which of the listed texts a text is takes a search.
    TypeID tested = types.testablePartOf(types.withLiteralStringsWidened(widenedForTestOfGuess(walker, expected)));
    Type type = types.at(tested);
    if (type.isNever() || !type.isJavaScriptValue() || type.hasUnboundedUnknown() || !changesCodeWhereProven(types, tested))
        return { };

    // As chooseTestOfGuess() has it: a type that no object is made as speaks of the objects of the nearest types above it.
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
    // An object of a built-in class that is also of a layout: no type says what is left behind the test.
    return types.isSubset(types.intersectionOf(inFrontOfGuard, tested), tested) ? tested : TypeID();
}

// A slow version costs about as many bytes as the body has operations, and a guard gains about as many instructions as operations lean on it. So many operations of the body may stand against one that leans.
// One in a loop counts as often as a loop is taken to go round.
static constexpr uint32_t mostOperationsOfBodyForOneThatLeans = 4;
static constexpr uint32_t roundsOfLoopForParameterGuards = 8;

// An operation leans on a parameter where it looks at it, is built typed with what the guard proves and is closed without it: the code that builds it answers. It answers with how many lean, each in a loop counted for its rounds.
static uint32_t findParametersThatBodyLeansOn(Walker& walker, std::span<ParameterOfGuard> parameters, std::span<const TypeID> typesInFrontOfGuard)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    TypingContext typing { walker.program(), types, walker.positions(), TypeLevel::Proven };

    // By value of the body. One more than the place of the parameter that the value is a name of: the parameter itself, a cast of it, what a test on the one edge into a block leaves of it.
    Vector<uint16_t, 256> namedParameters;
    namedParameters.fill(0, index.numberOfValues());
    for (size_t place = 0; place < parameters.size(); ++place)
        namedParameters[parameters[place].entryParameter] = static_cast<uint16_t>(place + 1);

    // Nothing is built yet, so of any other operand no more is known than what it is made of: a constant, or what the source declares of the place that it is read from, which a guess of its own may prove where the walk comes to it.
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
    // What a test of a type has left of a parameter that the body tells apart.
    struct NarrowedName {
        TypeID behindGuard;
        TypeID inFrontOfGuard;
    };
    UncheckedKeyHashMap<uint32_t, NarrowedName, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> narrowedNames;
    // Behind the guard `Array.isArray(x)` says no to all that the look at the cell has left: the edge on which it says yes leads nowhere.
    BitVector blocksThatNoRunEnters;
    // By the block that ends in the branch: the edge itself, where it leads to a block that other edges enter too.
    std::array<BitVector, 2> branchesWithEdgeThatNoRunTakes;
    // By IteratorOpen.
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
        // `Array.isArray(x)`: the ways on which the answer is yes meet, and each brings the parameter as its tests have left it.
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
            // `else for (const unit of value)`: by the guard alone the arm has the proof that lets the iteration go by index without a test and without the protocol beside it, and each step goes by that.
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
            // An operator that takes one operand as any value asks that operand in line, each time. With the proof it has another form, which asks nothing.
            if (typedFormForGuesses(walker, operation, inputsInFrontOfGuard) != TypedForm::None) {
                static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
                if (!operators.contains(opcode))
                    continue;
                OperatorForm formInFrontOfGuard = formAllowedForTypedOperator(walker, opcode, operation, InputsOfTypedOperation(walker, &inputsInFrontOfGuard));
                if (formAllowedForTypedOperator(walker, opcode, operation, InputsOfTypedOperation(walker, &inputsBehindGuard)) == formInFrontOfGuard)
                    continue;
                // `x * y` of numbers asks nothing either. That they are integers saves a conversion at most, and whatever arrives with a fraction pays for it with the slow version.
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

// `function size(x: string | string[]) { return typeof x === "string" ? x.length : x[0].length; }`: the body asks which member it has. Where the guard lets all of them through, each arm has its proof and no member runs in the slow version.
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
                // `function filter(list, f) { if (list !== undefined) { for (..) .. list[i] .. } }`: that test passes every member or none: it tells no member from another.
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

// `function each(list, f) { if (list !== undefined) { for (..) f(list[i]); } }` is handed undefined as often as a list. A guard for the list alone sent every such call into the slow version.
// What the body itself sends another way passes the guard, and behind the test of the body the list is proven as before. null, for which this body does not ask, would stand beside the list in all of the loop.
static void letThroughNullishThatBodyAsksFor(TypeArena& types, ParameterOfGuard& parameter)
{
    if (parameter.isToldApartByBody || !parameter.nullishAskedFor)
        return;
    parameter.tested = types.withPrimitives(parameter.tested, parameter.nullishAskedFor);
    parameter.behindGuard = types.intersectionOf(parameter.inFrontOfGuard, parameter.tested);
}

// The operations that findParametersThatBodyLeansOn() would ask about, each in a loop counted for its rounds: no more than these can lean. It goes through the body as that does, and asks nobody.
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
            // The open, and its steps in their loop.
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

    // Nobody has left anything yet, so what arrives is what Link has proven of every caller.
    TypeArena& types = walker.types();
    uint32_t firstPositionalParameter = function.firstParameterIndex();
    uint32_t endOfPositionalParameters = firstPositionalParameter + function.numberOfPositionalParameters();
    Vector<TypeID, 8> typesInFrontOfGuard;
    Vector<ParameterOfGuard, 8> parameters;
    for (uint32_t index = 0; index < numberOfEntryParameters; ++index) {
        typesInFrontOfGuard.append(walker.typeArrivingAtEntryParameter(index));
        bool isTested = function.entryParameterKind(index) == EntryParameter::This || (index >= firstPositionalParameter && index < endOfPositionalParameters);
        // What a guess would expect of it: what is written, `this` of a function that `new` enters, else what Link holds.
        TypeID tested = isTested ? typeTestedByParameterGuard(walker, findExpectationForGuesses(walker, ValueID(index)).type, typesInFrontOfGuard.last()) : TypeID();
        tested = typeTestedInPlaceOfWrittenType(walker, id, index, tested);
        if (tested && isNeverType(types.intersectionOf(typesInFrontOfGuard.last(), tested)))
            tested = { };
        if (!tested)
            continue;
        parameters.append(ParameterOfGuard { index, tested, typesInFrontOfGuard.last(), types.intersectionOf(typesInFrontOfGuard.last(), tested), false, { }, false });
        // Only beside objects are primitives left out of what is expected.
        TypeID whole = types.at(tested).hasObjects() ? findWholeExpectationForGuesses(walker, ValueID(index)).type : TypeID();
        if (!saysNothingForGuesses(types, whole) && partThatOneTestTellsForGuesses(types, whole) != whole)
            parameters.last().testedWithPrimitives = typeTestedByParameterGuard(walker, whole, typesInFrontOfGuard.last());
    }
    if (parameters.isEmpty())
        return { };
    if (static_cast<uint64_t>(countOperationsThatLookAtParameters(walker, parameters.span())) * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues())
        return { };
    Vector<ParameterOfGuard, 8> asOneTestTells = parameters;
    findParametersThatBodyTellsApart(walker, parameters.mutableSpan());
    for (ParameterOfGuard& parameter : parameters)
        letThroughNullishThatBodyAsksFor(types, parameter);
    uint32_t numberThatLean = findParametersThatBodyLeansOn(walker, parameters.mutableSpan(), typesInFrontOfGuard.span());
    // With all of its union let through nothing leans on the parameter. A test for the members that one test tells may pay all the same, as it does in a body that asks nothing.
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
    if (static_cast<uint64_t>(numberThatLean) * mostOperationsOfBodyForOneThatLeans < walker.currentBodyIndex().numberOfValues())
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
            Vector<TypeID, 8> typesBehindGuard = typesBehindParameterGuard(workers[worker]->walker, FunctionID(index));
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

// The fields of explainClosedAccess(). The subject of `x.m(..)` is x, of every other call what is called.
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

    // What the emitter asks of the records that are built (AccessEmitter::isReadOfMethodCalledNext()), asked of the input: it says where the read and the call are two operations at run time.
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

// Fields, with tabs between: unit . inline frame . position . operation . where the receiver comes from . how far the walk came . why it came no further . what is proven of the receiver . what is expected of it.
// How far: the receiver's layouts are PROVEN . NOTHING is EXPECTED of it . what is expected says nothing, is a bag of options or has no test . it is DECLARED or SEEN TO ARRIVE, and with that taken for proven the access has a typed form or none.
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
    // A site without a position is found by its unit and its name. By key: what is proven of the key.
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

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// unions-and-narrowing, calls. `shape.area()` where the receivers find several methods under the name: the call goes through the method table of the plan (plan/AOTCallingConventions.h, MethodFamily).
// The run time writes the rows where the guard of a class begins or ends, and an entry asks the receiver whether it still stands where it was made. So a CallTable is right for every receiver of layouts that all have a row.
// It keeps nothing from one operation to the next. At a suspension and at a throw: a CallTable is a call. Withheld (DirectCalls): the call is copied in its closed form, for which the emitter takes the table where it can.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTTypeArena.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// A test of the receiver in front of one call through the table, or of one direct call, counts as one operation that is built typed.
static constexpr uint32_t operationsBuiltTypedBehindTestOfReceiver = 1;

namespace {

// `o.m(a, b)` as the body writes it: the read, which the call alone takes, and the call, further on in the block.
struct MethodCallInUnionCalls {
    BodyValue read;
    BodyValue call;
    ValueID receiver;
    PropertyKeyID key;
    unsigned numberOfArguments { 0 };
};

struct TableCallInUnionCalls {
    uint32_t selector { 0 };
    // What the rows of the receivers hold. Ascending, each once.
    Vector<FunctionID, 8> methods;
    bool someRowIsGuarded { false };
};

} // anonymous namespace

bool endWhereCalleeIsNeverReached(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::CallDirect)
        return false;
    FunctionID callee = call.as<Opcode::CallDirect>().function();
    if (!callee || walker.facts().linked().function(callee).isReached)
        return false;
    walker.endAsUnreachable(walker.originOf(call));
    return true;
}

static std::optional<MethodCallInUnionCalls> findMethodCallInUnionCalls(const Walker& walker, const BodyValue& readOrCall)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read;
    ValueID call;
    if (readOrCall.opcode() == Opcode::PropGet) {
        read = readOrCall.id();
        call = index.onlyUsingValue(read);
    } else if (readOrCall.opcode() == Opcode::Call) {
        call = readOrCall.id();
        read = readOrCall.as<Opcode::Call>().calleeOperand();
    }
    if (!read || !call || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.opcodeOf(call) != Opcode::Call)
        return std::nullopt;
    if (index.onlyUsingValue(read) != call || index.blockOf(read) != index.blockOf(call))
        return std::nullopt;

    auto written = index.value(call).as<Opcode::Call>();
    auto lookup = index.value(read).as<Opcode::PropGet>();
    if (lookup.isFreeName() || written.calleeOperand() != read || !written.thisValueOperand() || written.thisValueOperand() != lookup.objectOperand() || written.calleeKey() != lookup.key())
        return std::nullopt;
    if (written.argumentsOperands().size() > maxArgumentsOfTableCall || written.expectedResult())
        return std::nullopt;
    return MethodCallInUnionCalls { index.value(read), index.value(call), lookup.objectOperand(), lookup.key(), static_cast<unsigned>(written.argumentsOperands().size()) };
}

// The read comes first as the program is written, and the table is asked when the call is made. Between the two stands only what sees nothing and is not seen.
static bool onlyWhatNobodySeesStandsBetweenInUnionCalls(const Walker& walker, const MethodCallInUnionCalls& site)
{
    const BodyIndex& index = walker.currentBodyIndex();
    for (uint32_t between = site.read.id().index() + 1; between < site.call.id().index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (!opcodesThatStateTypes.contains(opcode) && !declaredBoundInTable(opcode).isUnobservable())
            return false;
    }
    return true;
}

// A ReturnCall names the call, as it is or behind casts.
static bool isReturnedByBlockInUnionCalls(const BodyIndex& index, ValueID call)
{
    BodyTerminator end = index.terminator(index.blockOf(call));
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) && index.valueBehindCasts(end.operand(0)) == call;
}

// Nothing: some receiver has no row, or fewer than two methods are found, which is a call of one function for whoever finds it.
static std::optional<TableCallInUnionCalls> planTableCallInUnionCalls(const Walker& walker, const MethodCallInUnionCalls& site, TypeID typeOfReceiver)
{
    // A unit of the table is where the table leads.
    if (!walker.buildsTableCalls() || isTableRole(walker.unit().role) || !typeOfReceiver)
        return std::nullopt;
    // The call as it is written leaves the frame.
    if (!walker.tableCallsHaveTailForm() && isReturnedByBlockInUnionCalls(walker.currentBodyIndex(), site.call.id()))
        return std::nullopt;
    TypeArena& types = walker.types();
    Type receiver = types.at(typeOfReceiver);
    if (!receiver.isObject() || !receiver.layouts || !receiver.hasOnlyLayouts())
        return std::nullopt;
    const Plan& plan = walker.plan();
    const CallingConventions& conventions = plan.conventions();
    std::optional<uint32_t> selector = conventions.findMethodSelector(site.key, site.numberOfArguments);
    if (!selector)
        return std::nullopt;

    const MethodFamily& family = conventions.familyOf(conventions.methodSelector(*selector));
    TableCallInUnionCalls planned;
    planned.selector = *selector;
    for (LayoutID layout : membersOf(types, receiver)) {
        if (!plan.isInUse(layout))
            continue;
        // The emitter loads by the shape word of whatever arrives.
        if (!plan.slots().holdsCompiledObjects(layout))
            return std::nullopt;
        auto row = std::ranges::lower_bound(family.rows, layout, { }, &MethodTableRow::layout);
        if (row == family.rows.end() || row->layout != layout)
            return std::nullopt;
        planned.methods.append(row->method);
        planned.someRowIsGuarded |= !!row->guardedClass;
    }
    std::ranges::sort(planned.methods);
    planned.methods.shrink(planned.methods.size() - std::ranges::unique(planned.methods).size());
    if (planned.methods.size() < 2)
        return std::nullopt;
    if (planned.someRowIsGuarded && !onlyWhatNobodySeesStandsBetweenInUnionCalls(walker, site))
        return std::nullopt;
    return planned;
}

// `shape.scale(2.5)`: the typed selector stands right behind the boxed one of its family and count. It is taken where every argument is proven to lie within what the selector takes, so that each crosses in the form of that type.
// Only a CallTable of the walk names it: whoever goes by CallingConventions::findMethodSelector() alone gets the boxed one.
static uint32_t chooseSelectorInUnionCalls(Walker& walker, const Draft& written, uint32_t boxed)
{
    const CallingConventions& conventions = walker.plan().conventions();
    // The rule of the result asks the positions, which answer while typed operations are built.
    if (!walker.has(Mechanism::TypedOperations) || boxed + 1 >= conventions.methodSelectors().size())
        return boxed;
    const MethodSelector& typed = conventions.methodSelector(boxed + 1);
    if (!typed.isTyped || typed.family != conventions.methodSelector(boxed).family || typed.numberOfArguments != conventions.methodSelector(boxed).numberOfArguments)
        return boxed;
    // A raw form needs a type that allows it, which the rule of the row has from the positions.
    if (typed.result != Representation::JSValue && walker.positions().resultOfSelector(TypeLevel::Proven, boxed + 1) != typed.typeOfResult)
        return boxed;
    // A call that leaves the frame returns in place of the unit.
    if (walker.unitReturnsAtOnce(written) && typed.result != conventions.resultRepresentation(walker.unit()))
        return boxed;
    const MethodFamily& family = conventions.familyOf(typed);
    std::span<const Reference> arguments = written.argumentsOperands();
    for (size_t index = 0; index < arguments.size(); ++index) {
        TypeID taken = family.typesOfArguments[index];
        if (taken && !walker.types().isSubset(walker.typeOf(arguments[index]), taken))
            return boxed;
    }
    return boxed + 1;
}

static void buildTableCallInUnionCalls(Walker& walker, const MethodCallInUnionCalls& site, const TableCallInUnionCalls& planned, Reference receiver)
{
    Draft written = walker.draftOf(site.call);
    if (walker.hasFailed())
        return;
    Draft table(Opcode::CallTable, written.origin());
    walker.chooseConservativeFields(table);
    table.setReceiverOperand(receiver);
    table.setArgumentsOperands(written.argumentsOperands());
    table.setKey(site.key);
    table.setSelector(chooseSelectorInUnionCalls(walker, written, planned.selector));
    table.expected = written.expected;
    table.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    table.inputValueGiven = written.inputValueGiven;

    // A row without a guard is proven: nothing under the name changes, and no receiver comes to stand below anything else. So the methods are all that is entered.
    Effects effects = Effects::none();
    for (FunctionID method : planned.methods) {
        const LinkedFunction& linked = walker.facts().linked().function(method);
        effects.merge(linked.effectsOnReturn);
        effects.merge(linked.effectsOnThrow);
    }
    effects.mayThrow |= ThrowEffect::onlyStackOverflow();
    if (Reference result = walker.build(WTF::move(table), planned.someRowIsGuarded ? nullptr : &effects))
        walker.bind(site.call.id(), result);
}

// The walk stands at the read. What stands between the two is walked, the call is built, and the read is never built: the table finds the method.
static bool callThroughTableAtReadInUnionCalls(Walker& walker, const MethodCallInUnionCalls& site)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(site.read.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t placeOfRead = site.read.id().index() - firstValue;
    uint32_t placeOfCall = site.call.id().index() - firstValue;
    if (placeOfCall >= walker.endOfStretchBeingWalked())
        return false;
    Reference receiver = walker.referenceOf(site.receiver);
    if (!receiver || !receiver.isValue())
        return false;
    std::optional<TableCallInUnionCalls> planned = planTableCallInUnionCalls(walker, site, walker.typeOf(receiver));
    if (!planned)
        return false;

    walker.walkStretch(block.id(), placeOfRead + 1, placeOfCall);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    // The call alone takes the read, and its draft is read for the arguments: draftOf() wants a record for every operand.
    walker.bind(site.read.id(), receiver);
    {
        // `figure.describe(line)`, where `line` grows in a loop: the call is not the operation being walked, and the walk stands where it is written. It gets a string as any reader does.
        std::optional<StringsInPlaceOfTextsInBuilders> strings;
        if (walker.hasTextsInStringBuilders())
            strings.emplace(walker, site.call);
        if (!walker.isUnreachable())
            buildTableCallInUnionCalls(walker, site, *planned, receiver);
    }
    // Link lists no one callee where several methods are found. It is told all the same, as Walker::walkOperation() tells it of a call that it walks.
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), site.call.id()))
        walker.arrivals().leaveAnything(callee);
    walker.walkStretch(block.id(), placeOfCall + 1, placeOfCall + 1);
    return true;
}

bool callThroughMethodTable(Walker& walker, const BodyValue& readOrCall)
{
    if (!walker.has(Mechanism::DirectCalls))
        return false;
    std::optional<MethodCallInUnionCalls> site = findMethodCallInUnionCalls(walker, readOrCall);
    if (!site)
        return false;
    if (readOrCall.opcode() == Opcode::PropGet)
        return callThroughTableAtReadInUnionCalls(walker, *site);

    // The read is built. It goes where nobody takes it and it does nothing that is seen: the sweep drops it. Any other read has looked the name up already, and the call takes what it has found.
    Reference callee = walker.referenceOf(site->read.id());
    Reference receiver = walker.referenceOf(site->receiver);
    if (!callee || !callee.isValue() || !receiver || !receiver.isValue() || !walker.code().at(callee.id()).traits().isRemovableWhenUnused())
        return false;
    if (findOnlyCallee(walker, site->call, callee))
        return false;
    std::optional<TableCallInUnionCalls> planned = planTableCallInUnionCalls(walker, *site, walker.typeOf(receiver));
    if (!planned)
        return false;
    buildTableCallInUnionCalls(walker, *site, *planned, receiver);
    return true;
}

bool wouldCallThroughMethodTable(Walker& walker, const BodyValue& call, const TypesOfInputs& types)
{
    if (call.opcode() != Opcode::Call || !walker.has(Mechanism::DirectCalls))
        return false;
    std::optional<MethodCallInUnionCalls> site = findMethodCallInUnionCalls(walker, call);
    return site && planTableCallInUnionCalls(walker, *site, types(site->receiver));
}

// ---- Behind one test of the receiver

static bool isReturnedAtOnceInUnionCalls(const BodyIndex& index, ValueID call)
{
    BodyTerminator end = index.terminator(index.blockOf(call));
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call;
}

// The block returns the call behind a cast: the arms would meet in front of the cast, and what meets is no call.
static bool isReturnedBehindCastsInUnionCalls(const BodyIndex& index, ValueID call)
{
    BodyTerminator end = index.terminator(index.blockOf(call));
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) && end.operand(0) != call && index.valueBehindCasts(end.operand(0)) == call;
}

// ShapeTest > the stretch with the receiver as what has passed > the stretch where the test has failed > the two meet. As openGuessWithTest() of AOTWalkGuesses.cpp, for a set of layouts that need be no family.
// `failingSideIsClosed`: what fails is out of sight, and goes through the closed forms in a rare block. Else it is what is left of the proven type, and goes through the chain under that type.
static void walkStretchOnBothSidesOfShapeTestInUnionCalls(Walker& walker, Reference receiver, LayoutSetID tested, BlockID inputBlock, uint32_t begin, uint32_t end, bool failingSideIsClosed)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Code& code = walker.code();
    uint32_t first = index.block(inputBlock).firstValue().index() + begin;
    uint32_t size = end - begin;
    bool takesInCallReturnedAtOnce = isReturnedAtOnceInUnionCalls(index, ValueID(first + size - 1));

    Vector<ValueID, 4> names;
    Vector<uint32_t, 16> mentions;
    mentions.fill(0, size);
    for (uint32_t place = 0; place < size; ++place) {
        index.value(ValueID(first + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand.index() >= first)
                ++mentions[operand.index() - first];
            else if (walker.referenceOf(operand) == receiver && !names.contains(operand))
                names.append(operand);
        });
    }
    Vector<ValueID, 8> valuesMentionedLater;
    for (uint32_t place = 0; place < size; ++place) {
        uint32_t uses = index.useCount(ValueID(first + place));
        if (uses == BodyIndex::maxCountedUses || uses > mentions[place])
            valuesMentionedLater.append(ValueID(first + place));
    }

    Origin origin = walker.originOf(index.value(ValueID(first)));
    origin.isTailCall = false;
    Draft test(Opcode::ShapeTest, origin);
    test.setObjectOperand(receiver);
    test.setLayouts(tested);
    Reference passes = walker.build(WTF::move(test));
    if (!passes)
        return;
    BlockID passed = walker.addBlock();
    BlockID failed = walker.addBlock(failingSideIsClosed ? BlockFrequency::Rare : BlockFrequency::Normal);
    BlockID meeting = walker.addBlock();
    walker.branch(passes, passed, failed);
    walker.noteGuessOpened();

    struct Arm {
        BlockID last;
        Vector<Reference, 8> values;
    };
    Vector<Arm, 2> arms;
    auto endArm = [&] {
        if (takesInCallReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        Arm arm { walker.currentBlock(), { } };
        for (ValueID mentioned : valuesMentionedLater) {
            Reference built = walker.referenceOf(mentioned);
            if (!built || !built.isRecord())
                return walker.fail("an arm behind a test of a receiver has no value for what is read behind it"_s);
            arm.values.append(built);
        }
        arms.append(WTF::move(arm));
    };
    auto walkUnderWhatIsLeft = [&](bool isOnTakenEdge) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(receiver);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        Reference left = walker.build(WTF::move(narrowed));
        if (!left)
            return;
        walker.state().noteShape(left, walker.typeOf(left));
        Vector<Reference, 4> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, left);
        }
        walker.walkStretch(inputBlock, begin, end);
        endArm();
        for (size_t place = 0; place < names.size(); ++place)
            walker.bind(names[place], boundBefore[place]);
    };

    walker.continueIn(failed);
    if (failingSideIsClosed) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(inputBlock, begin, end);
        endArm();
    } else
        walkUnderWhatIsLeft(false);
    if (walker.hasFailed())
        return;
    walker.continueIn(passed);
    walkUnderWhatIsLeft(true);
    if (walker.hasFailed())
        return;

    // As where the arms of a guess meet: what the last arm has bound to a value that the body never mentions again is the proof of one arm.
    for (uint32_t place = 0; place < size; ++place) {
        if (arms.isEmpty() || !valuesMentionedLater.contains(ValueID(first + place)))
            walker.bind(ValueID(first + place), Reference());
    }

    Vector<size_t, 8> positionsOfParameters;
    for (size_t position = 0; position < valuesMentionedLater.size() && !arms.isEmpty(); ++position) {
        Reference one = arms.first().values[position];
        Reference other = arms.last().values[position];
        Reference met = one;
        if (one != other) {
            TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(one.id()).type, code.at(other.id()).type);
            Representation forms[] { walker.representationOf(one), walker.representationOf(other) };
            met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms));
            positionsOfParameters.append(position);
        }
        walker.bind(valuesMentionedLater[position], met);
    }
    Vector<Reference, 8> arguments;
    for (const Arm& arm : arms) {
        arguments.shrink(0);
        for (size_t position : positionsOfParameters)
            arguments.append(arm.values[position]);
        walker.continueIn(arm.last);
        walker.jumpTo(meeting, arguments.span());
        if (walker.hasFailed())
            return;
    }
    walker.continueIn(meeting);
}

// What has failed a test of shapes is not tested again: one test a call.
static bool standsBehindFailedShapeTestInUnionCalls(const Walker& walker, Reference receiver)
{
    const Operation& record = walker.code().at(receiver.id());
    return record.opcode == Opcode::Narrowed && !record.isOnTakenEdge() && record.testOperand() && walker.code().at(record.testOperand()).opcode == Opcode::ShapeTest;
}

// The receivers that find the method which most of their rows name. Null: they are not proven to have rows, or all find one method.
static LayoutSetID receiversOfLikeliestMethodInUnionCalls(Walker& walker, const MethodCallInUnionCalls& site, TypeID typeOfReceiver)
{
    std::optional<TableCallInUnionCalls> planned = planTableCallInUnionCalls(walker, site, typeOfReceiver);
    if (!planned)
        return { };
    TypeArena& types = walker.types();
    const CallingConventions& conventions = walker.plan().conventions();
    const MethodFamily& family = conventions.familyOf(conventions.methodSelector(planned->selector));
    Vector<std::pair<LayoutID, FunctionID>, 8> rows;
    for (LayoutID layout : membersOf(types, types.at(typeOfReceiver))) {
        if (walker.plan().isInUse(layout))
            rows.append({ layout, std::ranges::lower_bound(family.rows, layout, { }, &MethodTableRow::layout)->method });
    }
    Vector<uint32_t, 8> numbersOfRows;
    numbersOfRows.fill(0, planned->methods.size());
    for (auto [layout, method] : rows)
        ++numbersOfRows[std::ranges::lower_bound(planned->methods, method) - planned->methods.begin()];
    FunctionID likeliest = planned->methods[std::ranges::max_element(numbersOfRows) - numbersOfRows.begin()];
    Vector<LayoutID, 8> receivers;
    for (auto [layout, method] : rows) {
        if (method == likeliest)
            receivers.append(layout);
    }
    return types.at(types.forLayouts(receivers.span())).layouts;
}

bool callThroughMethodTableBehindTest(Walker& walker, const BodyValue& read)
{
    // Both arms hold the read, so the test stands in front of it. Two arms at every call of a method cost more than they give: in loops alone.
    if (read.opcode() != Opcode::PropGet || !walker.has(Mechanism::DirectCalls) || !walker.isInLoop() || isTableRole(walker.unit().role))
        return false;
    std::optional<MethodCallInUnionCalls> site = findMethodCallInUnionCalls(walker, read);
    if (!site)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t begin = read.id().index() - block.firstValue().index();
    uint32_t end = site->call.id().index() - block.firstValue().index() + 1;
    if (end > walker.endOfStretchBeingWalked() || isReturnedBehindCastsInUnionCalls(index, site->call.id()))
        return false;
    Reference receiver = walker.referenceOf(site->receiver);
    if (!receiver || !receiver.isValue() || standsBehindFailedShapeTestInUnionCalls(walker, receiver))
        return false;

    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(receiver);
    if (LayoutSetID likeliest = receiversOfLikeliestMethodInUnionCalls(walker, *site, proven)) {
        if (!shouldOpenGuess(walker, receiver, types.forLayoutSet(likeliest), true, operationsBuiltTypedBehindTestOfReceiver))
            return false;
        walkStretchOnBothSidesOfShapeTestInUnionCalls(walker, receiver, likeliest, block.id(), begin, end, false);
        return true;
    }

    // A written type, and the names that are read off a value together, say more than one name does: the guess at that is opened by whoever finds it.
    if (findExpectation(walker, site->receiver).type)
        return false;
    const CallingConventions& conventions = walker.plan().conventions();
    std::optional<uint32_t> selector = conventions.findMethodSelector(site->key, site->numberOfArguments);
    LayoutSetID receivers = selector ? conventions.familyOf(conventions.methodSelector(*selector)).receivers : LayoutSetID();
    if (!receivers)
        return false;
    TypeID expected = types.forLayoutSet(receivers);
    if (types.isSubset(proven, expected) || !types.overlap(proven, expected))
        return false;
    auto typeBehindTest = [&](ValueID input) {
        return input == site->receiver ? types.intersectionOf(proven, expected) : walker.typeOf(walker.referenceOf(input));
    };
    // A family of one method: behind the test the read finds that method, and the call is a call of one function.
    if (!wouldCallThroughMethodTable(walker, site->call, TypesOfInputs(typeBehindTest)) && typedFormOf(walker, site->read, TypesOfInputs(typeBehindTest)) != TypedForm::Built)
        return false;
    if (!shouldOpenGuess(walker, receiver, expected, false, operationsBuiltTypedBehindTestOfReceiver))
        return false;
    walkStretchOnBothSidesOfShapeTestInUnionCalls(walker, receiver, receivers, block.id(), begin, end, true);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

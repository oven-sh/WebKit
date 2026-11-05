/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t mostReceiversToldApartByArmsInUnionTableCalls = 4;

namespace {

struct TableCallInUnionTableCalls {
    uint32_t selector { 0 };
    Vector<FunctionID, 4> methods;
    bool someRowIsGuarded { false };
    Vector<LayoutID, 8> receiversWithRows;
    bool someReceiverHasNoRow { false };
};

} // anonymous namespace

static bool isCallOfMethodReadInUnionTableCalls(const BodyIndex& index, const BodyValue& read, ValueID call)
{
    auto lookup = read.as<Opcode::PropGet>();
    if (lookup.isFreeName() || !lookup.objectOperand() || !call || index.opcodeOf(call) != Opcode::Call || index.blockOf(call) != index.blockOf(read.id()))
        return false;
    auto written = index.value(call).as<Opcode::Call>();
    return written.calleeOperand() == read.id() && written.thisValueOperand() == lookup.objectOperand() && written.argumentsOperands().size() <= maxArgumentsOfTableCall;
}

static bool readMayMoveDownToCallInUnionTableCalls(const BodyIndex& index, ValueID read, ValueID call)
{
    for (uint32_t between = read.index() + 1; between < call.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        const Effects& bound = declaredBoundInTable(opcode);
        if (!opcodesThatStateTypes.contains(opcode) && !(bound.isUnobservable() && bound.reads.containsOnly(bookkeepingHeaps) && !operandsMayRunCode(opcode)))
            return false;
    }
    return true;
}

static Effects effectsOfMethodInUnionTableCalls(Walker& walker, FunctionID method, std::span<const Reference> receiverAndArguments)
{
    const Function& function = walker.program().at(method);
    bool takesThis = function.numberOfEntryParameters() && function.entryParameterKind(0) == EntryParameter::This;
    std::span<const Reference> entered = takesThis ? receiverAndArguments : receiverAndArguments.subspan(1);
    if (function.numberOfEntryParameters() != entered.size() || function.numberOfPositionalParameters() != receiverAndArguments.size() - 1)
        return Effects::unknownCode();
    return effectsOfDirectCall(walker, method, entered);
}

enum class ArmsMayTakeCallInUnionTableCalls : bool { No, Yes };

static std::optional<TableCallInUnionTableCalls> planTableCallInUnionTableCalls(Walker& walker, const BodyValue& read, ValueID call, Reference receiver, ArmsMayTakeCallInUnionTableCalls armsMayTakeCall)
{
    if (isTableRole(walker.unit().role) || !receiver || !receiver.isValue())
        return std::nullopt;
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(receiver);
    if (!proven || !types.at(proven).isJavaScriptValue() || (types.at(proven).primitives & ~Primitive::UndefinedOrNull) || !types.at(proven).hasOnlyLayouts() || types.at(proven).traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || holdsOneMember(types.at(proven).layouts))
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    const Plan& plan = walker.plan();
    const CallingConventions& conventions = plan.conventions();
    std::optional<uint32_t> selector = conventions.findMethodSelector(read.as<Opcode::PropGet>().key(), static_cast<unsigned>(index.value(call).as<Opcode::Call>().argumentsOperands().size()));
    if (!selector)
        return std::nullopt;

    TableCallInUnionTableCalls planned { *selector, { }, false, { }, false };
    bool readOfEveryOtherReceiverIsConstant = true;
    size_t numberOfReceiversInUse = 0;
    bool someCopyWouldLearnOfThis = false;
    for (size_t place = 0; place < membersOf(types, types.at(proven)).size(); ++place) {
        LayoutID member = membersOf(types, types.at(proven))[place];
        if (!plan.isInUse(member))
            continue;
        if (!plan.slots().holdsCompiledObjects(member))
            return std::nullopt;
        ++numberOfReceiversInUse;
        const MethodTableRow* row = conventions.findRowOfMethodTable(*selector, member);
        if (!row) {
            if (!isFoundByShapeTest(walker.program().at(member)))
                return std::nullopt;
            TypeID tested = types.forLayout(member);
            auto typeBehindTest = [&](ValueID input) {
                return input == read.as<Opcode::PropGet>().objectOperand() ? tested : walker.typeOf(walker.referenceOf(input));
            };
            readOfEveryOtherReceiverIsConstant &= !findField(walker.program(), member, read.as<Opcode::PropGet>().key()) && typedFormOf(walker, read, TypesOfInputs(typeBehindTest)) == TypedForm::Built;
            planned.someReceiverHasNoRow = true;
            continue;
        }
        planned.receiversWithRows.append(member);
        if (walker.program().at(row->method).isCoroutine())
            return std::nullopt;
        planned.methods.append(row->method);
        planned.someRowIsGuarded |= !!row->guardedClass;
        TypeID own = walker.program().at(row->method).thisType.written();
        someCopyWouldLearnOfThis |= !own || !types.isSubset(types.withoutNullish(own), types.forLayout(member));
    }
    std::ranges::sort(planned.methods);
    planned.methods.shrink(std::ranges::unique(planned.methods).begin() - planned.methods.begin());
    if (planned.methods.size() < 2)
        return std::nullopt;
    if (armsMayTakeCall == ArmsMayTakeCallInUnionTableCalls::Yes && someCopyWouldLearnOfThis && numberOfReceiversInUse <= mostReceiversToldApartByArmsInUnionTableCalls && walker.isInLoop() && walker.has(Mechanism::Guesses) && walker.opensGuesses() && !walker.standsInArmOfRowOfShapeTests())
        return std::nullopt;
    if ((planned.someRowIsGuarded || (planned.someReceiverHasNoRow && !readOfEveryOtherReceiverIsConstant)) && !readMayMoveDownToCallInUnionTableCalls(index, read.id(), call))
        return std::nullopt;
    if (planned.someReceiverHasNoRow && (!walker.has(Mechanism::Guesses) || !walker.opensGuesses()))
        return std::nullopt;

    BodyTerminator end = index.terminator(index.blockOf(call));
    if (end.kind() == TerminatorKind::ReturnCall && end.operand(0) && index.valueBehindCasts(end.operand(0)) == call) {
        bool everyMethodComesBack = !planned.someRowIsGuarded && !planned.someReceiverHasNoRow && std::ranges::all_of(planned.methods, [&](FunctionID method) {
            return plan.endsItsTailCalls(method);
        });
        if (!everyMethodComesBack)
            return std::nullopt;
    }
    if (!walker.isShown(receiver, proven))
        return std::nullopt;
    return planned;
}

constinit const OpcodeSet opcodesThatLeaveReadOfMethodToTableCallTakes { Opcode::PropGet };

bool leaveReadOfMethodToTableCallOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID call = index.onlyUsingValue(read.id());
    if (!isCallOfMethodReadInUnionTableCalls(index, read, call))
        return false;
    auto lookup = read.as<Opcode::PropGet>();
    Reference receiver = walker.referenceOf(lookup.objectOperand());
    if (call.index() - index.block(index.blockOf(call)).firstValue().index() >= walker.endOfStretchBeingWalked())
        return false;
    if (!planTableCallInUnionTableCalls(walker, read, call, receiver, ArmsMayTakeCallInUnionTableCalls::Yes))
        return false;

    if (walker.types().at(walker.typeOf(receiver)).primitives) {
        Draft check(Opcode::RequireObjectCoercible, walker.originOf(read));
        check.setValueOperand(receiver);
        check.setNullishAccess(NullishAccess::Read);
        if (!liesInThread(lookup.key())) {
            if (const PropertyKey& key = walker.program().at(lookup.key()); key.kind == PropertyKeyKind::Text)
                check.setName(key.text);
        }
        Reference checked = walker.build(WTF::move(check));
        if (!checked || walker.isUnreachable())
            return true;
        walker.state().noteCheckedName(receiver, checked);
    }
    walker.bind(read.id(), Reference());
    return true;
}

static Reference narrowedByInUnionTableCalls(Walker& walker, Reference value, Reference test, bool isOnTakenEdge, Origin origin)
{
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(test);
    narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
    narrowed.setIsOnTakenEdge(isOnTakenEdge);
    return walker.build(WTF::move(narrowed));
}

constinit const OpcodeSet opcodesThatCallMethodThroughTableTakes { Opcode::Call };

bool callMethodThroughTableOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto written = call.as<Opcode::Call>();
    if (index.isParameter(written.calleeOperand()) || index.opcodeOf(written.calleeOperand()) != Opcode::PropGet)
        return false;
    BodyValue read = index.value(written.calleeOperand());
    if (index.onlyUsingValue(read.id()) != call.id() || !isCallOfMethodReadInUnionTableCalls(index, read, call.id()))
        return false;

    Reference receiver = walker.referenceOf(written.thisValueOperand());
    std::optional<TableCallInUnionTableCalls> planned;
    if (walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::DirectCalls))
        planned = planTableCallInUnionTableCalls(walker, read, call.id(), receiver, ArmsMayTakeCallInUnionTableCalls::No);
    if (!planned || walker.types().at(walker.typeOf(receiver)).primitives) {
        walker.copy(read);
        if (!walker.hasFailed() && !walker.isUnreachable())
            walker.copy(call);
        walker.bind(read.id(), Reference());
        return true;
    }

    Walker::ArmsThatMeet arms;
    BlockID meeting;
    BlockID withoutRow;
    Reference passes;
    Reference anyReceiver = receiver;
    if (planned->someReceiverHasNoRow) {
        Draft test(Opcode::ShapeTest, walker.originOf(call));
        test.setObjectOperand(receiver);
        test.setLayouts(walker.types().at(walker.types().forLayouts(planned->receiversWithRows.span())).layouts);
        passes = walker.build(WTF::move(test));
        if (!passes)
            return true;
        BlockID withRow = walker.addBlock();
        withoutRow = walker.addBlock(BlockFrequency::Rare);
        meeting = walker.addBlock();
        walker.noteGuessOpened();
        walker.branch(passes, withRow, withoutRow);
        walker.continueIn(withRow);
        receiver = narrowedByInUnionTableCalls(walker, anyReceiver, passes, true, walker.originOf(call));
        if (!receiver)
            return true;
        walker.state().noteShape(receiver, walker.typeOf(receiver));
    }

    walker.bind(read.id(), receiver);
    Draft asWritten = walker.draftOf(call);
    walker.bind(read.id(), Reference());
    if (walker.hasFailed())
        return true;
    Draft table(Opcode::CallTable, asWritten.origin());
    walker.chooseConservativeFields(table);
    table.setReceiverOperand(receiver);
    table.setArgumentsOperands(asWritten.argumentsOperands());
    table.setKey(read.as<Opcode::PropGet>().key());
    table.setSelector(planned->selector);
    table.expected = asWritten.expected;
    table.inputValueGiven = asWritten.inputValueGiven;

    Vector<Reference, 8> operands;
    operands.append(receiver);
    operands.append(asWritten.argumentsOperands());
    Effects effects = planned->someRowIsGuarded ? Effects::unknownCode() : Effects::none();
    for (FunctionID method : planned->methods)
        effects.merge(effectsOfMethodInUnionTableCalls(walker, method, operands.span()));
    walker.count(StatisticID::MethodCallsThroughTable);
    Reference result = walker.build(WTF::move(table), &effects);
    if (result)
        walker.bind(call.id(), result);
    if (!planned->someReceiverHasNoRow || walker.hasFailed())
        return true;

    if (result && !walker.isUnreachable())
        arms.append({ walker.currentBlock(), { result } });
    walker.continueIn(withoutRow);
    if (Reference others = narrowedByInUnionTableCalls(walker, anyReceiver, passes, false, walker.originOf(call))) {
        BodyBlock block = index.block(index.blockOf(call.id()));
        uint32_t first = block.firstValue().index();
        Reference boundBefore = walker.referenceBoundTo(written.thisValueOperand());
        walker.bind(written.thisValueOperand(), others);
        walker.bind(read.id(), Reference());
        walker.bind(call.id(), Reference());
        walker.walkStretch(block.id(), read.id().index() - first, read.id().index() - first + 1);
        if (!walker.isUnreachable() && !walker.hasFailed())
            walker.walkStretch(block.id(), call.id().index() - first, call.id().index() - first + 1);
        walker.bind(written.thisValueOperand(), boundBefore);
        if (walker.hasFailed())
            return true;
        if (Reference byName = walker.isUnreachable() ? Reference() : walker.referenceOf(call.id()))
            arms.append({ walker.currentBlock(), { byName } });
    }
    if (walker.hasFailed())
        return true;
    walker.bind(read.id(), Reference());
    Reference met;
    if (walker.letArmsMeet(arms.span(), meeting, std::span<Reference>(&met, 1)))
        walker.bind(call.id(), met);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

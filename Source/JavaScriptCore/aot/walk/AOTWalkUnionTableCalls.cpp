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
#include <array>
#include <optional>
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t mostReceiversToldApartByArmsInUnionTableCalls = 4;
static constexpr size_t mostReceiversOfSmallLeaves = 8;

namespace {

struct ReceiversOfTableCallInUnionTableCalls {
    bool areKeptForBuild { false };
    bool areRefused { false };
    bool someRowIsGuarded { false };
    bool someCopyPaysInLoop { false };
    bool someMethodIsSmallLeaf { false };
    bool everyMethodIsSmallLeaf { true };
    size_t numberInUse { 0 };
    Vector<FunctionID, 4> methods;
    Vector<LayoutID, 8> withRows;
    Vector<LayoutID, 2> withoutRows;
};

struct ReceiversKeptByUnionTableCalls : ReceiversOfTableCallInUnionTableCalls {
    struct EffectsOfMethods {
        bool areKnown { false };
        Effects ofMethodsThatAskNoOperand { Effects::none() };
        Vector<FunctionID, 2> methodsThatAskOperands;
    };
    std::array<EffectsOfMethods, 2> effectsByWhetherHandlerStandsAround;
};

struct KeptByUnionTableCalls final : Walker::KeptForBuild {
    HashMap<uint64_t, std::unique_ptr<ReceiversKeptByUnionTableCalls>> receiversOfSelectors;
};

struct TableCallInUnionTableCalls {
    uint32_t selector { 0 };
    ReceiversOfTableCallInUnionTableCalls* receivers { nullptr };
    bool isRequiredTailCall { false };
    bool needsShapeCheck { false };
};

} // anonymous namespace

static ALWAYS_INLINE bool isCallOfMethodReadInUnionTableCalls(const BodyIndex& index, const BodyValue& read, ValueID call)
{
    auto lookup = read.as<Opcode::PropGet>();
    if (lookup.isFreeName() || !lookup.objectOperand() || !call || index.opcodeOf(call) != Opcode::Call || index.blockOf(call) != index.blockOf(read.id()))
        return false;
    auto written = index.value(call).as<Opcode::Call>();
    return written.calleeOperand() == read.id() && written.thisValueOperand() == lookup.objectOperand() && written.argumentsOperands().size() <= maxArgumentsOfTableCall;
}

static bool callsLargerMethodInUnionTableCalls(const Walker& walker, const BodyIndex& index, const BodyValue& read, std::span<const LayoutID> members)
{
    BodyBlock block = index.block(index.blockOf(read.id()));
    ValueID receiver = index.stripCasts(read.as<Opcode::PropGet>().objectOperand());
    for (uint32_t place = 0; place < block.numberOfValues(); ++place) {
        BodyValue other = index.value(ValueID(block.firstValue().index() + place));
        if (other.opcode() != Opcode::PropGet || other.id() == read.id() || !other.as<Opcode::PropGet>().objectOperand() || index.stripCasts(other.as<Opcode::PropGet>().objectOperand()) != receiver)
            continue;
        ValueID call = index.onlyUsingValue(other.id());
        if (!isCallOfMethodReadInUnionTableCalls(index, other, call))
            continue;
        size_t numberOfArguments = index.value(call).as<Opcode::Call>().argumentsOperands().size();
        bool everyMethodIsSmallLeaf = std::ranges::all_of(members, [&](LayoutID member) {
            MemberFound found = walker.facts().linked().findMember(walker.program(), member, ClassSide::Prototype, other.as<Opcode::PropGet>().key());
            return found.kind == MemberFound::Kind::Method && found.method && isSmallLeaf(walker, found.method, numberOfArguments);
        });
        if (!everyMethodIsSmallLeaf)
            return true;
    }
    return false;
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

static void describeReceiversInUnionTableCalls(Walker& walker, uint32_t selector, LayoutSetID set, ReceiversOfTableCallInUnionTableCalls& receivers)
{
    TypeArena& types = walker.types();
    const Plan& plan = walker.plan();
    const CallingConventions& conventions = plan.conventions();
    receivers.areRefused = true;
    for (size_t place = 0; place < membersOf(types, set).size(); ++place) {
        LayoutID member = membersOf(types, set)[place];
        if (!plan.isInUse(member))
            continue;
        if (!plan.slots().holdsCompiledObjects(member))
            return;
        ++receivers.numberInUse;
        const MethodTableRow* row = conventions.findRowOfMethodTable(selector, member);
        if (!row) {
            if (!isFoundByShapeTest(walker.program().at(member)))
                return;
            receivers.withoutRows.append(member);
            continue;
        }
        receivers.withRows.append(member);
        if (walker.program().at(row->method).isCoroutine())
            return;
        receivers.methods.append(row->method);
        receivers.someRowIsGuarded |= !!row->guardedClass;
        TypeID own = walker.program().at(row->method).thisType.written();
        bool calleeIsSmallLeaf = isSmallLeaf(walker, row->method, conventions.methodSelector(selector).numberOfArguments);
        receivers.someCopyPaysInLoop |= !own || !types.isSubset(types.withoutNullish(own), types.forLayout(member));
        receivers.someMethodIsSmallLeaf |= calleeIsSmallLeaf;
        receivers.everyMethodIsSmallLeaf &= calleeIsSmallLeaf;
    }
    std::ranges::sort(receivers.methods);
    receivers.methods.shrink(std::ranges::unique(receivers.methods).begin() - receivers.methods.begin());
    receivers.areRefused = receivers.methods.size() < 2;
}

static ReceiversOfTableCallInUnionTableCalls& findReceiversInUnionTableCalls(Walker& walker, uint32_t selector, LayoutSetID set, std::optional<ReceiversOfTableCallInUnionTableCalls>& receiversOfThisCallAlone)
{
    TypeArena::SetOfRound round = walker.types().setOfRound(set);
    if (!round.isKeptForBuild) {
        receiversOfThisCallAlone.emplace();
        describeReceiversInUnionTableCalls(walker, selector, set, *receiversOfThisCallAlone);
        return *receiversOfThisCallAlone;
    }
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByUnionTableCalls();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByUnionTableCalls>();
    auto added = static_cast<KeptByUnionTableCalls&>(*kept).receiversOfSelectors.add((static_cast<uint64_t>(selector) + 1) << 32 | round.word, nullptr);
    if (added.isNewEntry) {
        added.iterator->value = makeUniqueWithoutFastMallocCheck<ReceiversKeptByUnionTableCalls>();
        ReceiversOfTableCallInUnionTableCalls& receivers = *added.iterator->value;
        describeReceiversInUnionTableCalls(walker, selector, set, receivers);
        receivers.areKeptForBuild = true;
        if (receivers.areRefused)
            receivers.methods.clear();
        if (receivers.areRefused)
            receivers.withRows.clear();
    }
    return *added.iterator->value;
}

static void mergeEffectsOfMethodsInUnionTableCalls(Walker& walker, ReceiversOfTableCallInUnionTableCalls& receivers, std::span<const Reference> receiverAndArguments, Effects& effects)
{
    if (!receivers.areKeptForBuild) {
        for (FunctionID method : receivers.methods)
            effects.merge(effectsOfMethodInUnionTableCalls(walker, method, receiverAndArguments));
        return;
    }
    ReceiversKeptByUnionTableCalls::EffectsOfMethods& kept = static_cast<ReceiversKeptByUnionTableCalls&>(receivers).effectsByWhetherHandlerStandsAround[walker.standsUnderHandler()];
    if (!kept.areKnown) {
        kept.areKnown = true;
        for (FunctionID method : receivers.methods) {
            if (effectsOfDirectCallHangOnArguments(walker, method))
                kept.methodsThatAskOperands.append(method);
            else
                kept.ofMethodsThatAskNoOperand.merge(effectsOfMethodInUnionTableCalls(walker, method, receiverAndArguments));
        }
    }
    effects.merge(kept.ofMethodsThatAskNoOperand);
    for (FunctionID method : kept.methodsThatAskOperands)
        effects.merge(effectsOfMethodInUnionTableCalls(walker, method, receiverAndArguments));
}

enum class ArmsMayTakeCallInUnionTableCalls : bool { No, Yes };

static ALWAYS_INLINE std::optional<TableCallInUnionTableCalls> planTableCallInUnionTableCalls(Walker& walker, const BodyValue& read, ValueID call, Reference receiver, ArmsMayTakeCallInUnionTableCalls armsMayTakeCall, std::optional<ReceiversOfTableCallInUnionTableCalls>& receiversOfThisCallAlone)
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

    ReceiversOfTableCallInUnionTableCalls& receivers = findReceiversInUnionTableCalls(walker, *selector, types.at(proven).layouts, receiversOfThisCallAlone);
    if (receivers.areRefused)
        return std::nullopt;
    bool readOfEveryOtherReceiverIsConstant = true;
    for (LayoutID member : receivers.withoutRows) {
        TypeID tested = types.forLayout(member);
        auto typeBehindTest = [&](ValueID input) {
            return input == read.as<Opcode::PropGet>().objectOperand() ? tested : walker.typeOf(walker.referenceOf(input));
        };
        bool holdsNameItself = findField(walker.program(), member, read.as<Opcode::PropGet>().key()) || plan.slots().findField(walker.program(), member, read.as<Opcode::PropGet>().key());
        readOfEveryOtherReceiverIsConstant &= !holdsNameItself && typedFormOf(walker, read, TypesOfInputs(typeBehindTest)) == TypedForm::Built;
    }
    bool someReceiverHasNoRow = !receivers.withoutRows.isEmpty();
    bool armsTakeCall = armsMayTakeCall == ArmsMayTakeCallInUnionTableCalls::Yes && (receivers.someCopyPaysInLoop || receivers.someMethodIsSmallLeaf) && receivers.numberInUse <= (receivers.everyMethodIsSmallLeaf ? mostReceiversOfSmallLeaves : mostReceiversToldApartByArmsInUnionTableCalls) && walker.isInLoop() && walker.has(Mechanism::Guesses) && walker.opensGuesses() && !walker.standsInArmOfRowOfShapeTests();
    if (armsTakeCall) {
        bool isLeftToTable = !receivers.someCopyPaysInLoop && (walker.receiverOfLargerMethod() == receiver || callsLargerMethodInUnionTableCalls(walker, index, read, receivers.withRows));
        if (!isLeftToTable)
            return std::nullopt;
        walker.receiverOfLargerMethod() = receiver;
    }
    if ((receivers.someRowIsGuarded || (someReceiverHasNoRow && !readOfEveryOtherReceiverIsConstant)) && !readMayMoveDownToCallInUnionTableCalls(index, read.id(), call))
        return std::nullopt;
    if (someReceiverHasNoRow && (!walker.has(Mechanism::Guesses) || !walker.opensGuesses()))
        return std::nullopt;

    bool isRequiredTailCall = false;
    BodyTerminator end = index.terminator(index.blockOf(call));
    if (end.kind() == TerminatorKind::ReturnCall && end.operand(0) && index.stripCasts(end.operand(0)) == call) {
        bool everyMethodComesBack = !receivers.someRowIsGuarded && !someReceiverHasNoRow && std::ranges::all_of(receivers.methods, [&](FunctionID method) {
            return plan.endsItsTailCalls(method);
        });
        isRequiredTailCall = !everyMethodComesBack;
        if (isRequiredTailCall && (receivers.someRowIsGuarded || end.operand(0) != call || !canTailCallMethodTable(walker, *selector, receivers.methods.span())))
            return std::nullopt;
        if (isRequiredTailCall && someReceiverHasNoRow && !walker.isInLoop() && !std::ranges::binary_search(receivers.methods, walker.currentFunction()))
            return std::nullopt;
    }
    bool isShown = someReceiverHasNoRow || walker.isShown(receiver, proven);
    if (!isShown && (!walker.has(Mechanism::Guesses) || !walker.opensGuesses()))
        return std::nullopt;
    return TableCallInUnionTableCalls { *selector, &receivers, isRequiredTailCall, someReceiverHasNoRow || !isShown };
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
    std::optional<ReceiversOfTableCallInUnionTableCalls> receiversOfThisCallAlone;
    if (!planTableCallInUnionTableCalls(walker, read, call, receiver, ArmsMayTakeCallInUnionTableCalls::Yes, receiversOfThisCallAlone))
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
        if (walker.receiverOfLargerMethod() == receiver)
            walker.receiverOfLargerMethod() = checked;
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

static uint32_t chooseSelectorInUnionTableCalls(Walker& walker, const Draft& written, uint32_t boxed)
{
    const CallingConventions& conventions = walker.plan().conventions();
    if (boxed + 1 >= conventions.methodSelectors().size())
        return boxed;
    const MethodSelector& typed = conventions.methodSelector(boxed + 1);
    if (!typed.isTyped || typed.family != conventions.methodSelector(boxed).family || typed.numberOfArguments != conventions.methodSelector(boxed).numberOfArguments)
        return boxed;
    if (typed.result != Representation::JSValue && walker.positions().resultOfSelector(TypeLevel::Proven, boxed + 1) != typed.typeOfResult)
        return boxed;
    const MethodFamily& family = conventions.familyOf(typed);
    std::span<const Reference> arguments = written.argumentsOperands();
    for (size_t place = 0; place < arguments.size(); ++place) {
        TypeID taken = family.typesOfArguments[place];
        if (taken && !walker.isShown(arguments[place], taken))
            return boxed;
    }
    return boxed + 1;
}

constinit const OpcodeSet opcodesThatCallMethodThroughTableTakes { Opcode::Call };

bool callMethodThroughTableOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto written = call.as<Opcode::Call>();
    if (index.isParameter(written.calleeOperand()))
        return false;
    BodyValue read = walker.asWalked(index.value(written.calleeOperand()));
    if (read.opcode() != Opcode::PropGet)
        return false;
    if (index.onlyUsingValue(read.id()) != call.id() || !isCallOfMethodReadInUnionTableCalls(index, read, call.id()))
        return false;

    Reference receiver = walker.referenceOf(written.thisValueOperand());
    std::optional<ReceiversOfTableCallInUnionTableCalls> receiversOfThisCallAlone;
    std::optional<TableCallInUnionTableCalls> planned;
    if (walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::DirectCalls))
        planned = planTableCallInUnionTableCalls(walker, read, call.id(), receiver, ArmsMayTakeCallInUnionTableCalls::No, receiversOfThisCallAlone);
    if (!planned || walker.types().at(walker.typeOf(receiver)).primitives) {
        walker.copy(read);
        if (!walker.hasFailed() && !walker.isUnreachable())
            walker.copy(call);
        walker.bind(read.id(), Reference());
        return true;
    }

    Walker::ArmsThatMeet arms;
    bool isReadBehind = !index.isUnused(call.id());
    BlockID meeting;
    BlockID withoutRow;
    Reference passes;
    Reference anyReceiver = receiver;
    bool someReceiverHasNoRow = !planned->receivers->withoutRows.isEmpty();
    auto emitGenericCall = [&](Reference other) {
        BodyBlock block = index.block(index.blockOf(call.id()));
        uint32_t first = block.firstValue().index();
        Reference boundBefore = walker.referenceBoundTo(written.thisValueOperand());
        walker.bind(written.thisValueOperand(), other);
        walker.bind(read.id(), Reference());
        walker.bind(call.id(), Reference());
        walker.walkStretch(block.id(), read.id().index() - first, read.id().index() - first + 1);
        if (!walker.isUnreachable() && !walker.hasFailed())
            walker.walkStretch(block.id(), call.id().index() - first, call.id().index() - first + 1);
        walker.bind(written.thisValueOperand(), boundBefore);
    };
    Draft returned(Opcode::Call, walker.originOf(call));
    returned.resultIsReturnedAtOnce = planned->isRequiredTailCall;
    bool returnsInArm = walker.unitReturnsAtOnce(returned);
    if (planned->needsShapeCheck) {
        TypeID withMethod = walker.types().forLayouts(planned->receivers->withRows.span());
        Origin origin = walker.originOf(call);
        origin.isTailCall = false;
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(receiver);
        test.setLayouts(walker.types().at(withMethod).layouts);
        test.setAsksInFull(!walker.isShown(receiver, walker.typeOf(receiver)));
        passes = walker.build(WTF::move(test));
        if (!passes)
            return true;
        BlockID withRow = walker.addBlock();
        withoutRow = walker.addBlock(BlockFrequency::Rare);
        meeting = walker.addBlock();
        walker.recordSpeculation();
        walker.branch(passes, withRow, withoutRow);
        if (returnsInArm) {
            walker.continueIn(withoutRow);
            if (Reference others = narrowedByInUnionTableCalls(walker, anyReceiver, passes, false, origin))
                emitGenericCall(others);
            if (walker.hasFailed())
                return true;
            if (!walker.isUnreachable())
                walker.returnAtOnce(walker.referenceOf(call.id()));
            walker.bind(call.id(), Reference());
        }
        walker.continueIn(withRow);
        receiver = narrowedByInUnionTableCalls(walker, anyReceiver, passes, true, origin);
        if (!receiver)
            return true;
        walker.state().noteShape(receiver, walker.typeOf(receiver));
    }

    if (walker.verifiesCode() && !walker.isShown(receiver, walker.typeOf(receiver))) {
        walker.fail("a receiver of the method table is not shown to be what its type says"_s);
        return true;
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
    uint32_t selector = chooseSelectorInUnionTableCalls(walker, asWritten, planned->selector);
    if (planned->isRequiredTailCall && !canTailCallMethodTable(walker, selector, planned->receivers->methods.span()))
        selector = planned->selector;
    table.setSelector(selector);
    table.resultIsReturnedAtOnce = asWritten.resultIsReturnedAtOnce;
    table.expected = asWritten.expected;
    table.inputValueGiven = asWritten.inputValueGiven;

    Vector<Reference, 8> operands;
    operands.append(receiver);
    operands.append(asWritten.argumentsOperands());
    Effects effects = planned->receivers->someRowIsGuarded ? Effects::unknownCode() : Effects::none();
    mergeEffectsOfMethodsInUnionTableCalls(walker, *planned->receivers, operands.span(), effects);
    walker.count(StatisticID::MethodCallsThroughTable);
    Reference result = walker.build(WTF::move(table), &effects);
    if (result)
        walker.bind(call.id(), result);
    if (!planned->needsShapeCheck || returnsInArm || walker.hasFailed())
        return true;

    if (result && !walker.isUnreachable())
        arms.append({ walker.currentBlock(), { result } });
    walker.continueIn(withoutRow);
    if (Reference others = narrowedByInUnionTableCalls(walker, anyReceiver, passes, false, walker.originOf(call))) {
        emitGenericCall(others);
        if (walker.hasFailed())
            return true;
        if (!walker.isUnreachable())
            arms.append({ walker.currentBlock(), { isReadBehind ? walker.referenceOf(call.id()) : Reference() } });
    }
    if (walker.hasFailed())
        return true;
    walker.bind(read.id(), Reference());
    Reference met;
    if (walker.joinArms(arms.span(), meeting, std::span<Reference>(&met, 1).first(isReadBehind)))
        walker.bind(call.id(), met);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

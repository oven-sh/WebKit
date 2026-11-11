/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkBirths.h"
#include "AOTWalkKnown.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTCallPlan.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTThinSummary.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantsListedAtBirth { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String };
static constexpr OpcodeSet operationsBetweenStoresOfBirth = constantsListedAtBirth | opcodesThatStateTypes | OpcodeSet { Opcode::FinishObject };

namespace {

enum class HeadOfBodyInBirths : uint8_t { EndsInFrontOfOtherOperations, IsWholeBody, IsWholeBodyThatReturnsUndefined, IsWholeBodyThatReturnsObject };

struct FunctionReadInBirths {
    bool laysKeys { false };
    HeadOfBodyInBirths head { HeadOfBodyInBirths::EndsInFrontOfOtherOperations };
};

struct StoresBehindAllocationInBirths {
    Vector<PropertyKeyID, 16> keys;
    Vector<Reference, 16> values;
    Vector<TypeID, 16> types;
    Vector<MemberConditionRecord, 2> guardsOfCallsOfBases;
    FunctionID constructor;
    HashMap<uint32_t, FunctionReadInBirths, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsRead;
    bool readsFunctionAgain { false };
    bool laysKeyTwice { false };
};

struct ValueAskedInFrontOfAllocationInBirths {
    Reference value;
    TypeID asked;
};

struct BodyReadInBirths {
    bool names(ValueID) const;

    FunctionID function;
    const BodyIndex& index;
    Vector<ValueID, 4> namesOfObject;
    ValueID callOfBase;
    HeadOfBodyInBirths bodyOfBase { HeadOfBodyInBirths::EndsInFrontOfOtherOperations };
    ValueID testOfWhatBaseReturned;
};

} // anonymous namespace

bool storeLaysWhatSlotOfFreshObjectHolds(Walker& walker, Reference object, Reference held, Reference stored)
{
    if (!held || !held.isRecord() || !stored || !stored.isRecord() || !walker.state().isFresh(object))
        return false;
    const Code& code = walker.code();
    ValueID first = underlyingCellOf(code, held.id());
    ValueID second = underlyingCellOf(code, stored.id());
    if (first == second)
        return true;
    const Operation& one = code.at(first);
    const Operation& other = code.at(second);
    return one.opcode == other.opcode && constantsListedAtBirth.contains(one.opcode) && std::ranges::equal(one.payload(), other.payload());
}

static bool markAnswersForSomeFieldInBirths(const Plan& plan, LayoutID layout)
{
    const PlannedLayout& planned = plan.layout(layout);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (planned.slots[field].isFilledAtBirth || planned.holdsRawValueFromBirth(field))
            return true;
    }
    return false;
}

bool lookMayFinishBirthOf(Walker& walker, Reference object)
{
    if (object && walker.state().cellFacts(object).contains(CellFact::BearsNoMark))
        return false;
    TypeID proven = object ? walker.typeOf(object) : TypeID();
    if (!proven)
        return true;
    Type type = walker.types().at(proven);
    if (type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return true;
    return std::ranges::any_of(membersOf(walker.types(), type.layouts), [&](LayoutID member) {
        return markAnswersForSomeFieldInBirths(walker.plan(), member);
    });
}

static void showWhatIsHeldFilledOfLayoutInBirths(Walker& walker, Reference object, LayoutID layout, Origin origin)
{
    const Plan& plan = walker.plan();
    TypeArena& types = walker.types();
    if (!plan.isInUse(layout) || !plan.layout(layout).holdsCompiledObjects() || walker.fieldsHeldFilledThatAreNotShown(object, layout).isEmpty())
        return;
    TypeID proven = walker.typeOf(object);
    TypeID tested = types.forLayout(layout);
    if (proven) {
        Type type = types.at(proven);
        bool namesLayoutAlone = !type.primitives && type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow);
        for (LayoutID member : membersOf(types, type.layouts))
            namesLayoutAlone &= member == layout;
        if (namesLayoutAlone) {
            walker.showThatObjectHoldsWhatIsHeldFilled(object, layout);
            return;
        }
    }
    if (plan.shapes().shapesOf(layout).isEmpty() || (proven && !types.overlap(proven, tested)))
        return;
    origin.isTailCall = false;
    HoistedSpeculation isOfLayout { object, tested, types.at(tested).layouts, { } };
    Reference test = buildTestInFrontOfLoop(walker, isOfLayout, origin);
    if (!test)
        return;
    BlockID asks = walker.addBlock();
    BlockID isOfAnother = walker.addBlock();
    BlockID behind = walker.addBlock();
    walker.branch(test, asks, isOfAnother);
    walker.continueIn(asks);
    if (Reference narrowed = buildProofInFrontOfLoop(walker, isOfLayout, test, origin))
        walker.showThatObjectHoldsWhatIsHeldFilled(narrowed, layout);
    if (!walker.isUnreachable())
        walker.jumpTo(behind);
    walker.continueIn(isOfAnother);
    walker.jumpTo(behind);
    walker.continueIn(behind);
}

void showWhatIsHeldFilledWhereConstructionEnds(Walker& walker, FunctionID constructor, Reference object, Origin origin)
{
    TypeID proven = object && !walker.isUnreachable() ? walker.typeOf(object) : TypeID();
    if (!proven)
        return;
    Vector<LayoutID, 4> members;
    for (LayoutID member : membersOf(walker.types(), walker.types().at(proven).layouts)) {
        if (walker.program().at(declaredLayoutOf(walker.program(), member)).constructor == constructor)
            members.append(member);
    }
    for (LayoutID member : members) {
        if (walker.isUnreachable())
            return;
        showWhatIsHeldFilledOfLayoutInBirths(walker, object, member, origin);
    }
}

void showWhatIsHeldFilledAtReturnOfClassConstructor(Walker& walker, Reference returned, Origin origin)
{
    if (!returned || !returned.isValue() || walker.isUnreachable())
        return;
    const Function& function = walker.program().at(walker.currentFunction());
    if (isClassConstructorOf(function.kind) && function.constructedClass)
        showWhatIsHeldFilledOfLayoutInBirths(walker, returned, function.constructedClass, origin);
}

void finishBirthAtReturnOfClassConstructor(Walker& walker, Reference returned, Origin origin)
{
    if (!returned || !returned.isValue() || walker.isUnreachable() || !walker.has(Mechanism::TypedOperations))
        return;
    const Plan& plan = walker.plan();
    TypeArena& types = walker.types();
    const Function& function = walker.program().at(walker.currentFunction());
    LayoutID own = function.constructedClass;
    if (!isClassConstructorOf(function.kind) || !own || !plan.isInUse(own) || !plan.layout(own).holdsCompiledObjects() || !markAnswersForSomeFieldInBirths(plan, own))
        return;
    if (walker.state().cellFacts(returned).contains(CellFact::BearsNoMark))
        return;
    origin.isTailCall = false;
    auto tryFinish = [&](Reference object) {
        Draft finish(Opcode::TryFinishBirth, origin);
        finish.setObjectOperand(object);
        walker.chooseConservativeFields(finish);
        walker.build(WTF::move(finish));
    };
    TypeID proven = walker.typeOf(returned);
    TypeID tested = types.forLayout(own);
    if (!proven || !types.overlap(proven, tested))
        return;
    Type type = types.at(proven);
    if (!type.primitives && type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow)) {
        tryFinish(returned);
        return;
    }

    if (plan.shapes().shapesOf(own).isEmpty())
        return;
    HoistedSpeculation isOfOwnClass { returned, tested, types.at(tested).layouts, { } };
    Reference test = buildTestInFrontOfLoop(walker, isOfOwnClass, origin);
    if (!test)
        return;
    BlockID finishes = walker.addBlock();
    BlockID leavesToClassBelow = walker.addBlock();
    BlockID behind = walker.addBlock();
    walker.branch(test, finishes, leavesToClassBelow);
    walker.continueIn(finishes);
    if (Reference narrowed = buildProofInFrontOfLoop(walker, isOfOwnClass, test, origin))
        tryFinish(narrowed);
    walker.jumpTo(behind);
    walker.continueIn(leavesToClassBelow);
    walker.jumpTo(behind);
    walker.continueIn(behind);
}

static Reference constantWrittenInBodyForBirths(Walker& walker, const BodyValue& written)
{
    Opcode opcode = written.opcode();
    Draft constant(opcode);
    if (opcode == Opcode::Boolean)
        constant.setBoolean(written.boolean());
    else if (opcode == Opcode::Integer)
        constant.setInteger(written.integer());
    else if (opcode == Opcode::Double)
        constant.setNumber(written.number());
    else if (opcode == Opcode::String)
        constant.setString(written.string());
    return walker.constant(WTF::move(constant));
}

static ValueID valueBehindNamesInBodyForBirths(const BodyIndex& index, ValueID value)
{
    while (value && !index.isParameter(value) && (inputPassThroughOpcodes.contains(index.opcodeOf(value)) || index.opcodeOf(value) == Opcode::ToThis))
        value = index.value(value).operand(0);
    return value;
}

bool BodyReadInBirths::names(ValueID value) const
{
    return namesOfObject.contains(valueBehindNamesInBodyForBirths(index, value));
}

static HeadOfBodyInBirths collectStoresAtHeadOfBodyInBirths(Walker&, FunctionID, std::span<const Reference> passedOperands, StoresBehindAllocationInBirths&);

static FunctionID functionHeldByVariableInBirths(Walker& walker, VariableID variable)
{
    const DeclaredType& declared = walker.program().at(variable).declared;
    TypeID established = declared.isEstablished() ? walker.program().provenTypeOf(declared) : TypeID();
    return established ? singleFunctionOf(walker.types(), walker.types().at(established)) : FunctionID();
}

static FunctionID functionHeldByCaptureInBirths(Walker& walker, FunctionID function, uint32_t parameter)
{
    const Function& capturing = walker.program().at(function);
    if (capturing.entryParameterKind(parameter) != EntryParameter::Capture)
        return { };
    const LinkedCell& cell = walker.facts().linked().capture(function, parameter - capturing.numberOfEntryParametersBefore(EntryParameter::Capture));
    return cell.everyWriterIsKnown && cell.proven ? singleFunctionOf(walker.types(), walker.types().at(cell.proven)) : FunctionID();
}

static FunctionID functionWhoseCallIsReadInBirths(Walker& walker, const BodyReadInBirths& body, const BodyValue& read)
{
    if (read.opcode() != Opcode::PropGet || read.isFreeName())
        return { };
    const Program& program = walker.program();
    BuiltinID foundOnFunctionPrototype = findConstantMemberFrom(program, walker.facts(), Builtins::FunctionPrototype, read.key());
    if (!foundOnFunctionPrototype || program.builtins().canonicalID(foundOnFunctionPrototype) != Builtins::FunctionPrototypeCall)
        return { };
    ValueID holder = valueBehindNamesInBodyForBirths(body.index, read.objectOperand());
    if (!holder)
        return { };
    if (std::optional<uint32_t> parameter = body.index.block(entryBlock).indexOfParameter(holder))
        return functionHeldByCaptureInBirths(walker, body.function, *parameter);
    if (body.index.isParameter(holder) || body.index.opcodeOf(holder) != Opcode::GetModuleVariable)
        return { };
    return functionHeldByVariableInBirths(walker, body.index.value(holder).variable());
}

static Reference valueWrittenInBodyForBirths(Walker& walker, const BodyIndex& index, ValueID written, NOESCAPE const ScopedLambda<Reference(ValueID)>& valueInFrontOfAllocation)
{
    ValueID root = index.stripCasts(written);
    bool isWrittenConstant = !index.isParameter(root) && constantsListedAtBirth.contains(index.opcodeOf(root));
    return isWrittenConstant ? constantWrittenInBodyForBirths(walker, index.value(root)) : valueInFrontOfAllocation(root);
}

static bool collectStoresOfBaseCalledInBirths(Walker& walker, BodyReadInBirths& body, const BodyValue& call, ValueID readOfCall, NOESCAPE const ScopedLambda<Reference(ValueID)>& valueInFrontOfAllocation, StoresBehindAllocationInBirths& stores)
{
    const BodyIndex& index = body.index;
    auto written = call.as<Opcode::Call>();
    std::span<const ValueID> arguments = written.argumentsOperands();
    if (!readOfCall || valueBehindNamesInBodyForBirths(index, written.calleeOperand()) != readOfCall || arguments.empty() || !body.names(arguments[0]))
        return false;
    BodyValue read = index.value(readOfCall);
    if (valueBehindNamesInBodyForBirths(index, written.thisValueOperand()) != valueBehindNamesInBodyForBirths(index, read.objectOperand()))
        return false;
    FunctionID base = functionWhoseCallIsReadInBirths(walker, body, read);
    if (!base || base == stores.constructor)
        return false;
    auto found = stores.functionsRead.find(base.index());
    bool wasRead = found != stores.functionsRead.end();
    FunctionReadInBirths before = wasRead ? found->value : FunctionReadInBirths { };
    const Function& entered = walker.program().at(base);
    std::optional<MemberConditionRecord> guard = findGuardOfCallOffConstructorFunction(walker, base);
    if (!guard || entered.kind != FunctionKind::Plain || entered.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        return false;
    if (wasRead && !before.laysKeys) {
        body.callOfBase = call.id();
        body.bodyOfBase = before.head;
        if (before.head == HeadOfBodyInBirths::IsWholeBodyThatReturnsObject)
            body.namesOfObject.append(call.id());
        return before.head != HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
    }

    Vector<Reference, 8> passedOperands;
    size_t nextArgument = 1;
    for (unsigned parameter = 0; parameter < entered.numberOfEntryParameters(); ++parameter) {
        if (entered.entryParameterKind(parameter) != EntryParameter::Parameter)
            passedOperands.append(Reference());
        else if (nextArgument < arguments.size())
            passedOperands.append(valueWrittenInBodyForBirths(walker, index, arguments[nextArgument++], valueInFrontOfAllocation));
        else
            passedOperands.append(walker.constant(Draft(Opcode::Undefined)));
    }
    size_t numberOfStoresInFront = stores.keys.size();
    stores.functionsRead.set(base.index(), FunctionReadInBirths { });
    HeadOfBodyInBirths bodyOfBase = HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
    {
        SetForScope readsFunctionAgain(stores.readsFunctionAgain, stores.readsFunctionAgain || wasRead);
        bodyOfBase = collectStoresAtHeadOfBodyInBirths(walker, base, passedOperands.span(), stores);
    }
    stores.functionsRead.set(base.index(), FunctionReadInBirths { stores.keys.size() > numberOfStoresInFront, bodyOfBase });
    bool isWholeBody = bodyOfBase != HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
    if ((isWholeBody || stores.keys.size() > numberOfStoresInFront) && !stores.guardsOfCallsOfBases.contains(*guard))
        stores.guardsOfCallsOfBases.append(*guard);
    body.callOfBase = call.id();
    body.bodyOfBase = bodyOfBase;
    if (bodyOfBase == HeadOfBodyInBirths::IsWholeBodyThatReturnsObject)
        body.namesOfObject.append(call.id());
    return isWholeBody;
}

static bool collectStoresInBirths(Walker& walker, BodyReadInBirths& body, const BodyBlock& block, NOESCAPE const ScopedLambda<Reference(ValueID)>& valueInFrontOfAllocation, StoresBehindAllocationInBirths& stores)
{
    const BodyIndex& index = body.index;
    ValueID readOfCallThatAwaitsItsCall;
    for (BodyValue operation : block.values()) {
        Opcode opcode = operation.opcode();
        if (operationsBetweenStoresOfBirth.contains(opcode))
            continue;
        if (opcode == Opcode::ToThis && body.names(operation.id()))
            continue;
        if (opcode == Opcode::GetModuleVariable && functionHeldByVariableInBirths(walker, operation.variable()))
            continue;
        if (opcode == Opcode::ToBoolean && body.callOfBase && index.stripCasts(operation.operand(0)) == body.callOfBase && body.bodyOfBase != HeadOfBodyInBirths::IsWholeBody) {
            body.testOfWhatBaseReturned = operation.id();
            continue;
        }
        if (!readOfCallThatAwaitsItsCall && functionWhoseCallIsReadInBirths(walker, body, operation)) {
            readOfCallThatAwaitsItsCall = operation.id();
            continue;
        }
        if (opcode == Opcode::Call) {
            if (!collectStoresOfBaseCalledInBirths(walker, body, operation, std::exchange(readOfCallThatAwaitsItsCall, ValueID()), valueInFrontOfAllocation, stores))
                return false;
            continue;
        }
        if (readOfCallThatAwaitsItsCall || opcode != Opcode::PropSet || operation.isFreeName() || !body.names(operation.objectOperand()))
            return false;

        Reference value = valueWrittenInBodyForBirths(walker, index, operation.valueOperand(), valueInFrontOfAllocation);
        if (value && walker.hasStringsInBuilders() && isStringInBuilder(walker, value))
            return false;
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        if (!type)
            return false;
        if (stores.readsFunctionAgain) {
            ASSERT(stores.keys.contains(operation.key()));
            RELEASE_ASSERT(!walker.checksProofs() || stores.keys.contains(operation.key()));
            stores.laysKeyTwice = true;
            return false;
        }
        stores.keys.append(operation.key());
        stores.values.append(value);
        stores.types.append(type);
    }
    return !readOfCallThatAwaitsItsCall;
}

static std::span<const PropertyKeyID> keysThatBodyFillsOfThisInBirths(const ThinSummary& summary)
{
    for (const SummaryConstruction& construction : summary.constructions) {
        if (construction.object == SummaryConstruction::Object::This && construction.numberOfKeysFilledAtEnd)
            return summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd);
    }
    return { };
}

static HeadOfBodyInBirths collectStoresAtHeadOfBodyInBirths(Walker& walker, FunctionID function, std::span<const Reference> passedOperands, StoresBehindAllocationInBirths& stores)
{
    const Function& entered = walker.program().at(function);
    const BodyIndex* found = walker.facts().bodies().find(function);
    std::optional<unsigned> thisParameter = entered.thisParameterIndex();
    if (!found || !thisParameter)
        return HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
    const BodyIndex& body = *found;
    BodyBlock entry = body.block(entryBlock);
    if (entry.numberOfParameters() != passedOperands.size())
        return HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
    BodyReadInBirths read { function, body, { entry.parameter(*thisParameter) }, { }, HeadOfBodyInBirths::EndsInFrontOfOtherOperations, { } };
    auto nameParametersOf = [&](BodyTarget target) {
        BodyBlock reached = body.block(target.block());
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t place = 0; place < arguments.size(); ++place) {
            if (read.names(arguments[place]))
                read.namesOfObject.append(reached.parameter(place));
        }
    };
    auto valueHandedOver = [&](ValueID root) -> Reference {
        std::optional<uint32_t> parameter = entry.indexOfParameter(root);
        if (!parameter || entered.entryParameterKind(*parameter) != EntryParameter::Parameter)
            return { };
        return passedOperands[*parameter];
    };
    walker.count(StatisticID::BodiesReadForBirths);
    for (BlockID current = entryBlock;;) {
        BodyBlock block = body.block(current);
        if (block.hasHandler() || !collectStoresInBirths(walker, read, block, scopedLambda<Reference(ValueID)>(valueHandedOver), stores))
            return HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
        BodyTerminator end = block.terminator();
        if (end.kind() == TerminatorKind::Return) {
            if (!end.operand(0))
                return HeadOfBodyInBirths::IsWholeBody;
            if (read.names(end.operand(0)))
                return HeadOfBodyInBirths::IsWholeBodyThatReturnsObject;
            ValueID returned = body.stripCasts(end.operand(0));
            bool returnsUndefined = returned && !body.isParameter(returned) && body.opcodeOf(returned) == Opcode::Undefined;
            return returnsUndefined ? HeadOfBodyInBirths::IsWholeBodyThatReturnsUndefined : HeadOfBodyInBirths::IsWholeBody;
        }
        if (end.kind() == TerminatorKind::Branch && read.testOfWhatBaseReturned && end.operand(0) == read.testOfWhatBaseReturned) {
            BodyBlock takesWhatBaseReturned = body.block(end.taken().block());
            BlockID meeting = end.notTaken().block();
            bool isWrittenAsOr = body.numberOfIncomingEdges(takesWhatBaseReturned.id()) == 1 && !takesWhatBaseReturned.numberOfValues() && !takesWhatBaseReturned.hasHandler()
                && takesWhatBaseReturned.terminator().kind() == TerminatorKind::Jump && takesWhatBaseReturned.terminator().next().block() == meeting
                && body.numberOfIncomingEdges(meeting) == 2 && !body.isEnteredByHandlerEdge(meeting) && !body.isEnteredByHandlerEdge(takesWhatBaseReturned.id());
            if (!isWrittenAsOr)
                return HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
            if (read.bodyOfBase == HeadOfBodyInBirths::IsWholeBodyThatReturnsObject) {
                nameParametersOf(end.taken());
                body.forEachEdgeResult(current, [&](const BodyEdgeResult& result) {
                    if (result.isOnTakenEdge && read.names(result.subject))
                        read.namesOfObject.append(result.parameter);
                });
                nameParametersOf(takesWhatBaseReturned.terminator().next());
            } else
                nameParametersOf(end.notTaken());
            current = meeting;
            continue;
        }
        if (end.kind() != TerminatorKind::Jump)
            return HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
        BodyTarget next = end.next();
        if (next.numberOfIncomingValues() || body.numberOfIncomingEdges(next.block()) != 1 || body.isEnteredByHandlerEdge(next.block()))
            return HeadOfBodyInBirths::EndsInFrontOfOtherOperations;
        current = next.block();
    }
}

static bool copyTakesOperandsInBirths(Walker& walker, FunctionID callee, const Function& constructor, std::span<const Reference> passedOperands, unsigned thisParameter, TypeID typeOfThis)
{
    if (passedOperands.size() != constructor.numberOfEntryParameters())
        return false;
    TypeArena& types = walker.types();
    const CallingConventions& conventions = walker.plan().conventions();
    for (unsigned index = 0; index < passedOperands.size(); ++index) {
        EntryParameter kind = constructor.entryParameterKind(index);
        TypeID arriving = index == thisParameter ? typeOfThis : walker.typeOf(passedOperands[index]);
        if (!arriving && (passedOperands[index] || kind == EntryParameter::Callee))
            return false;
        const DeclaredType* declared = constructor.declaredEntryParameter(index);
        if (declared && declared->isEstablished() && (arriving || (kind != EntryParameter::Capture && kind != EntryParameter::ArgumentCount))) {
            TypeID established = walker.program().provenTypeOf(*declared);
            if (!arriving || !types.isSubset(arriving, established))
                return false;
        }
        TypeID tested = conventions.typeBehindGuard(callee, index);
        if (tested && (!arriving || !types.overlap(arriving, tested)))
            return false;
    }
    return true;
}

static Vector<ValueAskedInFrontOfAllocationInBirths, 4> findValuesToAskInFrontOfAllocationInBirths(Walker& walker, LayoutID constructed, StoresBehindAllocationInBirths& stores)
{
    Vector<ValueAskedInFrontOfAllocationInBirths, 4> values;
    TypeArena& types = walker.types();
    const SlotPlan& slots = walker.plan().slots();
    for (size_t index = 0; index < stores.keys.size(); ++index) {
        std::optional<FieldIndex> field = slots.findField(walker.program(), constructed, stores.keys[index]);
        TypeID taken = field ? slots.slot(constructed, *field).typeInLayoutRecord() : TypeID();
        if (!taken || !stores.values[index].isValue() || types.isSubset(stores.types[index], taken))
            continue;
        size_t place = values.findIf([&](const ValueAskedInFrontOfAllocationInBirths& each) {
            return each.value == stores.values[index];
        });
        if (place == notFound)
            values.append({ stores.values[index], taken });
        else
            values[place].asked = types.intersectionOf(values[place].asked, taken);
    }
    for (const ValueAskedInFrontOfAllocationInBirths& each : values) {
        if (!types.overlap(walker.typeOf(each.value), each.asked))
            return { };
    }
    for (size_t index = 0; index < stores.keys.size(); ++index) {
        for (const ValueAskedInFrontOfAllocationInBirths& each : values) {
            if (each.value == stores.values[index])
                stores.types[index] = types.intersectionOf(stores.types[index], each.asked);
        }
    }
    return values;
}

static bool looksBehindAllocationPayInBirths(const Walker& walker)
{
    if (!walker.has(Mechanism::Guesses))
        return false;
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.unit().function;
    return !walker.program().at(function).isModuleBody();
}

std::optional<GuardOfPrototypeAtConstruction> findGuardOfPrototypeAtConstruction(Walker& walker, FunctionID constructor, bool newCellIsLookedAt, bool guardsAreAskedAtBirth, Origin origin)
{
    const Program& program = walker.program();
    LayoutID classLayout = declaredLayoutOf(program, program.at(constructor).constructedClass);
    AccessContext context { program, WalkFacts(walker.facts()), walker.plan(), walker.types(), walker.currentFunction() };
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.isInLoop() || program.at(classLayout).constructor != constructor || !isConstructorPrototypeGuarded(context, classLayout))
        return std::nullopt;

    GuardOfPrototypeAtConstruction guard { { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Static, program.keyOf(BuiltinNames::prototype) } };
    guard.isKnownToHold = walker.state().guardsHaveBegun(classLayout) && walker.state().someGuardHoldsOf(classLayout);
    if (!guard.isKnownToHold && !newCellIsLookedAt && !(guardsAreAskedAtBirth && walker.state().guardsHaveBegun(classLayout)))
        return std::nullopt;
    origin.isTailCall = false;
    if (walker.state().guardOfReadsHolds(classLayout, guard.condition.key, ClassSide::Static))
        checkGuardsThatAreRemembered(walker, singleElementSpan(guard.condition), origin);
    return guard;
}

void checkPrototypeOfCellMadeBehindGuard(Walker& walker, Reference object, Reference constructor, LayoutID constructed, Origin origin)
{
    if (!walker.checksProofs() || walker.isUnreachable())
        return;
    origin.isTailCall = false;
    Draft look(Opcode::HasLayoutPrototype, origin);
    look.setObjectOperand(object);
    Reference passes = walker.buildTestOfCheck(WTF::move(look));
    if (passes && passes.isValue() && !walker.isUnreachable())
        walker.checkProof(passes, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);

    auto readOfPrototype = [&](bool readsClassObjectItself) {
        Draft read(Opcode::GetClassPrototype, origin);
        walker.chooseConservativeFields(read);
        read.setClassObjectOperand(constructor);
        read.setLayout(constructed);
        read.setReadsClassObjectItself(readsClassObjectItself);
        return read;
    };
    Reference named = walker.isUnreachable() ? Reference() : walker.buildTestOfCheck(readOfPrototype(false));
    walker.checkThatMemoryAnswersAsBelieved(named, readOfPrototype(true));
}

static Reference buildWhatConstructionGivesInBirths(Walker& walker, FunctionID callee, Reference returned, Reference object, Origin origin)
{
    TypeArena& types = walker.types();
    TypeID provenOfBody = walker.plan().typeHeldOfResult(walker.program(), walker.facts().linked(), callee);
    TypeID typeOfReturned = walker.typeOf(returned);
    if ((provenOfBody && !types.overlap(provenOfBody, TypeTable::object)) || (typeOfReturned && !types.overlap(typeOfReturned, TypeTable::object)))
        return object;
    if (typeOfReturned && types.isSubset(typeOfReturned, TypeTable::object))
        return returned;

    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(returned);
    test.setTestedType(TypeTable::object);
    walker.chooseConservativeFields(test);
    Reference returnsObject = walker.build(WTF::move(test));
    if (!returnsObject)
        return { };
    BlockID takesReturned = walker.addBlock(BlockFrequency::Unlikely);
    BlockID takesThis = walker.addBlock();
    BlockID behind = walker.addBlock();
    Reference result = walker.addParameter(behind, { }, Representation::JSValue);
    walker.branch(returnsObject, takesReturned, takesThis);
    walker.continueIn(takesReturned);
    walker.jumpTo(behind, singleElementSpan(returned));
    walker.continueIn(takesThis);
    walker.jumpTo(behind, singleElementSpan(object));
    walker.continueIn(behind);
    return result;
}

bool listStoresOfCopiedConstructorOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::Inlining) || walker.isUnreachable())
        return false;
    auto operation = call.as<Opcode::Construct>();
    if (operation.newTargetOperand() && operation.newTargetOperand() != operation.calleeOperand())
        return false;
    Reference closure = walker.referenceOf(operation.calleeOperand());
    FunctionID callee = closure ? findOnlyCallee(walker, call, closure) : FunctionID();
    if (!callee || !makesThisOfConstruction(walker, callee))
        return false;

    const Program& program = walker.program();
    const Function& constructor = program.at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    std::optional<unsigned> thisParameter = constructor.thisParameterIndex();
    if (!body || !thisParameter || constructor.isHiddenInStackTraces() || constructor.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        return false;
    std::span<const PropertyKeyID> keysFilledByBody = keysThatBodyFillsOfThisInBirths(walker.facts().summaries().at(callee));
    if (keysFilledByBody.empty() && !body->holdsAnyOf(OpcodeSet { Opcode::Call }))
        return false;
    LayoutID constructed = constructor.constructedClass;
    if (!chooseLayoutByTagAtConstruction(walker, call, callee, constructed))
        return false;

    Draft written = walker.draftOf(call);
    if (walker.hasFailed() || written.resultIsReturnedAtOnce)
        return false;
    Origin origin = written.origin();
    origin.isTailCall = false;
    Vector<Reference, 8> passedOperands = collectPassedOperands(written, constructor);

    Reference standIn = walker.constant(Draft(Opcode::Undefined, origin));
    Vector<Reference, 8> operandsAskedAbout;
    for (Reference operand : passedOperands)
        operandsAskedAbout.append(operand ? operand : standIn);
    Draft site(Opcode::CallDirect, origin);
    site.setFunction(callee);
    site.setIsConstruction(true);
    site.setVariadicOperands(operandsAskedAbout.span());
    if (!shouldCopyCallee(walker, site, callee, operandsAskedAbout.span()))
        return false;

    StoresBehindAllocationInBirths stores;
    stores.constructor = callee;
    collectStoresAtHeadOfBodyInBirths(walker, callee, passedOperands.span(), stores);
    if (stores.keys.isEmpty() || stores.laysKeyTwice || !copyTakesOperandsInBirths(walker, callee, constructor, passedOperands.span(), *thisParameter, walker.types().forLayout(constructed)))
        return false;

    Vector<ValueAskedInFrontOfAllocationInBirths, 4> valuesAsked = findValuesToAskInFrontOfAllocationInBirths(walker, constructed, stores);
    StoresListedAtBirth listed = findStoresThatAllocationMayList(walker, constructed, stores.keys.span(), stores.types.span(), keysFilledByBody);
    if (listed.fields.size() < stores.keys.size())
        return false;
    for (const MemberConditionRecord& guard : stores.guardsOfCallsOfBases) {
        if (!walker.state().guardOfReadsHolds(guard.layout, guard.key, ClassSide::Static))
            listed.conditions.append(guard);
    }
    if ((listed.asksForLayoutPrototype || !listed.conditions.isEmpty() || !valuesAsked.isEmpty()) && !looksBehindAllocationPayInBirths(walker))
        return false;
    std::optional<GuardOfPrototypeAtConstruction> guardOfPrototype = findGuardOfPrototypeAtConstruction(walker, callee, listed.asksForLayoutPrototype, !listed.conditions.isEmpty(), origin);
    bool someGuardOfClassIsAsked = guardOfPrototype && listed.conditions.containsIf([&](const MemberConditionRecord& condition) {
        return condition.layout == guardOfPrototype->condition.layout;
    });
    if (guardOfPrototype && !guardOfPrototype->isKnownToHold && !someGuardOfClassIsAsked)
        listed.conditions.append(guardOfPrototype->condition);
    bool beginsGuards = guardOfPrototype && !walker.state().guardsHaveBegun(guardOfPrototype->condition.layout);
    if (!beginsGuards)
        askWholeSideWhereGuardsMayNotHaveBegun(walker, declaredLayoutOf(program, constructed), listed.conditions);

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();

    BlockID closedBlock;
    auto continueIfGuardHolds = [&](Reference look) {
        BlockID passed = walker.addBlock();
        if (!closedBlock)
            closedBlock = walker.addBlock(BlockFrequency::Rare);
        walker.branch(look, passed, closedBlock);
        walker.continueIn(passed);
    };
    Reference guardsHold;
    uint32_t numberOfRecordsInFrontOfLook = walker.code().numberOfValues();
    if (!listed.conditions.isEmpty()) {
        if (beginsGuards) {
            Draft begin(Opcode::BeginMemberGuards, origin);
            begin.setLayout(guardOfPrototype->condition.layout);
            walker.build(WTF::move(begin));
        }
        for (const MemberConditionRecord& guard : stores.guardsOfCallsOfBases) {
            if (walker.state().guardsHaveBegun(guard.layout))
                continue;
            Draft begin(Opcode::BeginMemberGuards, origin);
            begin.setLayout(guard.layout);
            walker.build(WTF::move(begin));
        }
        Draft look(Opcode::AreMembersUnchanged, origin);
        look.setConditions(listed.conditions.span());
        guardsHold = walker.isUnreachable() ? Reference() : walker.build(WTF::move(look));
        if (!guardsHold)
            return true;
        continueIfGuardHolds(guardsHold);
    }
    if (walker.checksProofs() && guardsHold && !walker.wasBuiltInPlaceSince(numberOfRecordsInFrontOfLook, guardsHold))
        checkGuardsThatAreRemembered(walker, listed.conditions.span(), origin);
    checkGuardsThatAreRemembered(walker, listed.conditionsRemembered.span(), origin);

    for (const ValueAskedInFrontOfAllocationInBirths& each : valuesAsked) {
        Draft look(Opcode::TypeTest, origin);
        look.setValueOperand(each.value);
        look.setTestedType(each.asked);
        walker.chooseConservativeFields(look);
        Reference passes = walker.isUnreachable() ? Reference() : walker.build(WTF::move(look));
        if (!passes)
            return true;
        continueIfGuardHolds(passes);
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(each.value);
        proof.setTestOperand(passes);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        Reference narrowed = walker.build(WTF::move(proof));
        if (!narrowed)
            return true;
        for (Reference& value : stores.values) {
            if (value == each.value)
                value = narrowed;
        }
        for (Reference& operand : passedOperands) {
            if (operand == each.value)
                operand = narrowed;
        }
    }

    Reference prototype;
    if (!guardOfPrototype) {
        Draft read(Opcode::GetConstructPrototype, origin);
        read.setNewTargetOperand(closure);
        read.setLayout(declaredLayoutOf(program, constructed));
        walker.chooseConservativeFields(read);
        Effects ofRead = declaredBoundOf(Opcode::NewObject);
        ofRead.reads.add(AbstractHeap::NamedProperties);
        prototype = walker.build(WTF::move(read), &ofRead);
        if (!prototype)
            return true;
    }
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(constructed);
    if (prototype)
        allocation.setPrototypeOperand(prototype);
    allocation.setFields(listed.fields.span());
    allocation.setValuesOperands(stores.values.span().first(listed.fields.size()));
    walker.chooseConservativeFields(allocation);
    allocation.setEndsNoGuards(walker.isInLoop() && writerEndsNoGuard(walker, allocation));
    Reference object = walker.hasFailed() ? Reference() : walker.build(WTF::move(allocation));
    if (!object)
        return true;
    noteFreshObject(walker, object, listed.fields.size());
    if (program.at(declaredLayoutOf(program, constructed)).has(LayoutFlags::IsConstructorFunction))
        walker.noteBirthInTries(declaredLayoutOf(program, constructed));
    walker.count(StatisticID::ConstructorFieldsFoldedIntoAllocations, listed.fields.size());

    if (listed.asksForLayoutPrototype && !guardOfPrototype) {
        Draft look(Opcode::HasLayoutPrototype, origin);
        look.setObjectOperand(object);
        Reference passes = walker.build(WTF::move(look));
        if (!passes)
            return true;
        continueIfGuardHolds(passes);
    }
    if (guardOfPrototype)
        checkPrototypeOfCellMadeBehindGuard(walker, object, closure, constructed, origin);
    if (listed.asksForLayoutPrototype || guardOfPrototype)
        walker.state().recordCellFacts(object, CellFact::HasLayoutPrototype);
    if (guardsHold)
        noteGuardsShownByTest(walker, listed.conditions.span(), guardsHold);

    Vector<Walker::JoinArm, 2> arms;
    auto endArm = [&](Reference value) {
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append(Walker::JoinArm { walker.currentBlock(), { value } });
    };
    auto walkAsWritten = [&] {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm(walker.isUnreachable() ? Reference() : walker.referenceOf(call.id()));
    };

    auto constructAsWritten = [&] {
        Walker::ClosedArm closedArm(walker, !!guardOfPrototype);
        if (walker.isUnreachable())
            return;
        walker.leaveAnythingAt(callee);
        Draft asWritten = walker.draftOf(call);
        walker.chooseConservativeFields(asWritten);
        endArm(walker.isUnreachable() ? Reference() : buildCallAsWrittenThatGives(walker, WTF::move(asWritten), typeOfObjectsMadeThroughEntryOf(walker, callee)));
    };

    BlockID typedBlock = walker.currentBlock();
    if (closedBlock) {
        walker.recordSpeculation();
        walker.continueIn(closedBlock);
        constructAsWritten();
        if (walker.hasFailed())
            return true;
        walker.continueIn(typedBlock);
    }

    passedOperands[*thisParameter] = object;
    if (walker.walkInline(call, callee, passedOperands.span())) {
        Reference returned = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
        if (returned && listed.isBornUnfinished && lookMayFinishBirthOf(walker, object)) {
            Draft finish(Opcode::TryFinishBirth, origin);
            finish.setObjectOperand(object);
            walker.chooseConservativeFields(finish);
            walker.build(WTF::move(finish));
        }
        if (returned)
            showWhatIsHeldFilledWhereConstructionEnds(walker, callee, object, origin);
        endArm(returned && !walker.isUnreachable() ? buildWhatConstructionGivesInBirths(walker, callee, returned, object, origin) : Reference());
    } else
        walkAsWritten();
    if (walker.hasFailed())
        return true;

    if (!closedBlock) {
        if (arms.size() == 1 && arms[0].values[0])
            walker.bind(call.id(), arms[0].values[0]);
        return true;
    }

    BlockID meeting = walker.addBlock();
    Reference met;
    if (walker.joinArms(arms.span(), meeting, singleElementSpan(met)))
        walker.bind(call.id(), met);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

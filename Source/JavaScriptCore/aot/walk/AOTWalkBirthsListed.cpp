/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkBirths.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTThinSummary.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantsListedAtBirth { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String };
static constexpr OpcodeSet operationsBetweenStoresOfBirth = constantsListedAtBirth | opcodesThatStateTypes | OpcodeSet { Opcode::FinishObject };

namespace {

struct StoresBehindAllocationInBirths {
    Vector<PropertyKeyID, 16> keys;
    Vector<Reference, 16> values;
    Vector<TypeID, 16> types;
};

} // anonymous namespace

static ValueID recordBehindNamesAndFormsInBirths(const Code& code, ValueID record)
{
    while (opcodesThatGiveSameCell.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return record;
}

bool storeLaysWhatSlotOfFreshObjectHolds(Walker& walker, Reference object, Reference held, Reference stored)
{
    if (!held || !held.isRecord() || !stored || !stored.isRecord() || !walker.state().isFresh(object))
        return false;
    const Code& code = walker.code();
    ValueID first = recordBehindNamesAndFormsInBirths(code, held.id());
    ValueID second = recordBehindNamesAndFormsInBirths(code, stored.id());
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
    if (object && walker.state().factsOfCell(object).contains(FactOfCell::BearsNoMark))
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
    GuessInFrontOfLoop isOfLayout { object, tested, types.at(tested).layouts, { } };
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
    if (walker.state().factsOfCell(returned).contains(FactOfCell::BearsNoMark))
        return;
    origin.isTailCall = false;
    auto tryFinish = [&](Reference object) {
        Draft finish(Opcode::TryFinishBirth, origin);
        finish.setObjectOperand(object);
        walker.chooseConservativeFields(finish);
        walker.build(WTF::move(finish));
    };
    TypeID proven = walker.typeOf(returned);
    if (!proven)
        return;
    Type type = types.at(proven);
    if (!type.primitives && type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow)) {
        tryFinish(returned);
        return;
    }

    TypeID tested = types.forLayout(own);
    if (plan.shapes().shapesOf(own).isEmpty() || !types.overlap(proven, tested))
        return;
    GuessInFrontOfLoop isOfOwnClass { returned, tested, types.at(tested).layouts, { } };
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
    while (value && !index.isParameter(value) && inputOperationsThatHandOn.contains(index.opcodeOf(value)))
        value = index.value(value).operand(0);
    return value;
}

static bool collectStoresInBirths(Walker& walker, const BodyIndex& index, const BodyBlock& block, uint32_t begin, uint32_t end, ValueID object, NOESCAPE const ScopedLambda<Reference(ValueID)>& valueInFrontOfAllocation, StoresBehindAllocationInBirths& stores)
{
    uint32_t firstValue = block.firstValue().index();
    for (uint32_t place = begin; place < end; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        Opcode opcode = operation.opcode();
        if (operationsBetweenStoresOfBirth.contains(opcode))
            continue;
        if (opcode != Opcode::PropSet || operation.isFreeName() || valueBehindNamesInBodyForBirths(index, operation.objectOperand()) != object)
            return false;

        ValueID root = index.valueBehindCasts(operation.valueOperand());
        bool isWrittenConstant = !index.isParameter(root) && constantsListedAtBirth.contains(index.opcodeOf(root));
        Reference value = isWrittenConstant ? constantWrittenInBodyForBirths(walker, index.value(root)) : valueInFrontOfAllocation(root);
        if (value && walker.hasTextsInStringBuilders() && isTextInStringBuilder(walker, value))
            return false;
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        if (!type)
            return false;
        stores.keys.append(operation.key());
        stores.values.append(value);
        stores.types.append(type);
    }
    return true;
}

static std::span<const PropertyKeyID> keysThatBodyFillsOfThisInBirths(const ThinSummary& summary)
{
    for (const SummaryConstruction& construction : summary.constructions) {
        if (construction.object == SummaryConstruction::Object::This && construction.numberOfKeysFilledAtEnd)
            return summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd);
    }
    return { };
}

static void collectStoresAtHeadOfBodyInBirths(Walker& walker, const BodyIndex& body, const Function& constructor, unsigned thisParameter, std::span<const Reference> handedOver, StoresBehindAllocationInBirths& stores)
{
    BodyBlock entry = body.block(entryBlock);
    if (entry.numberOfParameters() != handedOver.size())
        return;
    auto valueHandedOver = [&](ValueID root) -> Reference {
        std::optional<uint32_t> parameter = entry.indexOfParameter(root);
        if (!parameter || constructor.entryParameterKind(*parameter) != EntryParameter::Parameter)
            return { };
        return handedOver[*parameter];
    };
    for (BlockID current = entryBlock;;) {
        BodyBlock block = body.block(current);
        if (block.hasHandler() || !collectStoresInBirths(walker, body, block, 0, block.numberOfValues(), entry.parameter(thisParameter), scopedLambda<Reference(ValueID)>(valueHandedOver), stores))
            return;
        BodyTerminator end = block.terminator();
        if (end.kind() != TerminatorKind::Jump)
            return;
        BodyTarget next = end.next();
        if (next.numberOfIncomingValues() || body.numberOfIncomingEdges(next.block()) != 1 || body.isEnteredByHandlerEdge(next.block()))
            return;
        current = next.block();
    }
}

static bool copyTakesOperandsInBirths(Walker& walker, FunctionID callee, const Function& constructor, std::span<const Reference> handedOver, unsigned thisParameter, TypeID typeOfThis)
{
    if (handedOver.size() != constructor.numberOfEntryParameters())
        return false;
    TypeArena& types = walker.types();
    const CallingConventions& conventions = walker.plan().conventions();
    for (unsigned index = 0; index < handedOver.size(); ++index) {
        EntryParameter kind = constructor.entryParameterKind(index);
        TypeID arriving = index == thisParameter ? typeOfThis : walker.typeOf(handedOver[index]);
        if (!arriving && (handedOver[index] || kind == EntryParameter::Callee))
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

static bool looksBehindAllocationPayInBirths(const Walker& walker)
{
    if (!walker.has(Mechanism::Guesses))
        return false;
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.unit().function;
    return !walker.program().at(function).isModuleBody();
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
    if (keysFilledByBody.empty())
        return false;
    LayoutID constructed = constructor.constructedClass;
    if (!chooseLayoutByTagAtConstruction(walker, call, callee, constructed))
        return false;

    Draft written = walker.draftOf(call);
    if (walker.hasFailed() || written.resultIsReturnedAtOnce)
        return false;
    Origin origin = written.origin();
    origin.isTailCall = false;
    Vector<Reference, 8> handedOver = operandsHandedToCopy(written, constructor);

    Reference standIn = walker.constant(Draft(Opcode::Undefined, origin));
    Vector<Reference, 8> operandsAskedAbout;
    for (Reference operand : handedOver)
        operandsAskedAbout.append(operand ? operand : standIn);
    Draft site(Opcode::CallDirect, origin);
    site.setFunction(callee);
    site.setIsConstruction(true);
    site.setVariadicOperands(operandsAskedAbout.span());
    if (!shouldCopyCallee(walker, site, callee, operandsAskedAbout.span()))
        return false;

    StoresBehindAllocationInBirths stores;
    collectStoresAtHeadOfBodyInBirths(walker, *body, constructor, *thisParameter, handedOver.span(), stores);
    if (stores.keys.isEmpty() || !copyTakesOperandsInBirths(walker, callee, constructor, handedOver.span(), *thisParameter, walker.types().forLayout(constructed)))
        return false;

    StoresListedAtBirth listed = findStoresThatAllocationMayList(walker, constructed, stores.keys.span(), stores.types.span(), keysFilledByBody);
    if (listed.fields.size() < stores.keys.size())
        return false;
    if ((listed.asksForLayoutPrototype || !listed.conditions.isEmpty()) && !looksBehindAllocationPayInBirths(walker))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();

    BlockID closedBlock;
    auto goOnWhereLookPasses = [&](Reference look) {
        BlockID passed = walker.addBlock();
        if (!closedBlock)
            closedBlock = walker.addBlock(BlockFrequency::Rare);
        walker.branch(look, passed, closedBlock);
        walker.continueIn(passed);
    };
    Reference guardsHold;
    if (!listed.conditions.isEmpty()) {
        Draft look(Opcode::AreMembersUnchanged, origin);
        look.setConditions(listed.conditions.span());
        guardsHold = walker.build(WTF::move(look));
        if (!guardsHold)
            return true;
        goOnWhereLookPasses(guardsHold);
    }

    Draft read(Opcode::GetConstructPrototype, origin);
    read.setNewTargetOperand(closure);
    read.setLayout(declaredLayoutOf(program, constructed));
    walker.chooseConservativeFields(read);
    Effects ofRead = declaredBoundOf(Opcode::NewObject);
    ofRead.reads.add(AbstractHeap::NamedProperties);
    Reference prototype = walker.build(WTF::move(read), &ofRead);
    if (!prototype)
        return true;
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(constructed);
    allocation.setPrototypeOperand(prototype);
    allocation.setFields(listed.fields.span());
    allocation.setValuesOperands(stores.values.span().first(listed.fields.size()));
    walker.chooseConservativeFields(allocation);
    Reference object = walker.hasFailed() ? Reference() : walker.build(WTF::move(allocation));
    if (!object)
        return true;
    noteFreshObject(walker, object, listed.fields.size());

    if (listed.asksForLayoutPrototype) {
        Draft look(Opcode::HasLayoutPrototype, origin);
        look.setObjectOperand(object);
        Reference passes = walker.build(WTF::move(look));
        if (!passes)
            return true;
        goOnWhereLookPasses(passes);
        walker.state().noteFactsOfCell(object, FactOfCell::HasLayoutPrototype);
    }
    if (guardsHold) {
        for (const MemberConditionRecord& condition : listed.conditions)
            walker.state().noteStoresPassAbove(condition.layout, condition.key, guardsHold);
    }

    Vector<Walker::ArmThatMeets, 2> arms;
    auto endArm = [&](Reference value) {
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append(Walker::ArmThatMeets { walker.currentBlock(), { value } });
    };
    auto walkAsWritten = [&] {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm(walker.isUnreachable() ? Reference() : walker.referenceOf(call.id()));
    };

    auto constructAsWritten = [&] {
        Walker::ClosedArm closedArm(walker);
        walker.arrivals().leaveAnything(callee);
        Draft asWritten = walker.draftOf(call);
        walker.chooseConservativeFields(asWritten);
        endArm(walker.isUnreachable() ? Reference() : buildCallAsWrittenThatGives(walker, WTF::move(asWritten), typeOfObjectsMadeThroughEntryOf(walker, callee)));
    };

    BlockID typedBlock = walker.currentBlock();
    if (closedBlock) {
        walker.noteGuessOpened();
        walker.continueIn(closedBlock);
        constructAsWritten();
        if (walker.hasFailed())
            return true;
        walker.continueIn(typedBlock);
    }

    handedOver[*thisParameter] = object;
    if (walker.walkInline(call, callee, handedOver.span())) {
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
    if (walker.letArmsMeet(arms.span(), meeting, singleElementSpan(met)))
        walker.bind(call.id(), met);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

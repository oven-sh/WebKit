/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors = 2;

static LayoutID onlyLayoutOfInKeyOrdersAccessors(const Walker& walker, TypeID listed)
{
    if (!listed)
        return { };
    Type type = walker.types().at(listed);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOneMember(type.layouts))
        return { };
    return singleLayoutOf(walker.types(), type);
}

static std::optional<FieldReference> findAccessorSlotInKeyOrdersAccessors(const Walker& walker, LayoutID layout, PropertyKeyID key)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    if (!layout || !key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text || program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto)
        return std::nullopt;
    if (!isFoundByShapeTest(program.at(layout)) || !plan.isInUse(layout) || !plan.slots().holdsCompiledObjects(layout))
        return std::nullopt;
    std::optional<FieldIndex> field = plan.slots().findField(program, layout, key);
    if (!field || !plan.slot(layout, *field).holdsAccessor || !plan.slot(layout, *field).hasStorage())
        return std::nullopt;
    return FieldReference { layout, *field };
}

static FunctionID findAccessorExpectedInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkTables& linked = walker.facts().linked();
    const LinkedAccessorsOfField* expected = linked.findAccessorsExpectedIn(field);
    FunctionID accessor = expected ? (isStore ? expected->setter : expected->getter) : FunctionID();
    return accessor && linked.mayBeMentionedBy(walker.unit(), accessor) ? accessor : FunctionID();
}

static bool isProvenHalfInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkedAccessorsOfField* laid = walker.facts().linked().findAccessorsExpectedIn(field);
    return laid && (isStore ? laid->setterIsProven : laid->getterIsProven);
}

static void checkProvenHalfInKeyOrdersAccessors(Walker& walker, const BodyValue& access, Reference object, FieldReference field)
{
    if (!walker.checksProofs())
        return;
    bool isStore = access.opcode() == Opcode::PropSet;
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;
    Draft load(isStore ? Opcode::GetSlotSetter : Opcode::GetSlotGetter, origin);
    load.setObjectOperand(object);
    load.setKey(access.key());
    load.setReceivers(walker.types().at(walker.types().forLayout(field.layout)).layouts);
    load.setLevel(KnowledgeLevel::Proven);
    if (isStore)
        load.setIsInStrictCode(access.mode() == ECMAMode::Strict);
    walker.buildTestOfCheck(WTF::move(load));
}

bool callProvenAccessorOfItsOpcodes(Walker& walker, const BodyValue& access)
{
    bool isStore = access.opcode() == Opcode::PropSet;
    if (access.isFreeName())
        return false;
    PropertyKeyID key = access.key();
    if (!key || liesInThread(key) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(key.index()))
        return false;
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;
    std::optional<FieldReference> field = findAccessorSlotInKeyOrdersAccessors(walker, onlyLayoutOfInKeyOrdersAccessors(walker, walker.typeOf(object)), key);
    if (!field || !isProvenHalfInKeyOrdersAccessors(walker, *field, isStore))
        return false;
    FunctionID half = findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore);
    if (!half)
        return false;
    checkProvenHalfInKeyOrdersAccessors(walker, access, object, *field);
    return walker.isUnreachable() || callAccessor(walker, access, half, object, stored);
}

static void fillSlotOperationInKeyOrdersAccessors(Walker& walker, Draft& draft, Reference object, FieldReference field, PropertyKeyID key)
{
    draft.setObjectOperand(object);
    draft.setKey(key);
    draft.setReceivers(walker.types().at(walker.types().forLayout(field.layout)).layouts);
    draft.setLevel(KnowledgeLevel::Proven);
}

bool callAccessorInSlot(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    bool isStore = opcode == Opcode::PropSet;
    if ((opcode != Opcode::PropGet && !isStore) || access.isFreeName())
        return false;
    if (PropertyKeyID name = access.key(); !name || liesInThread(name) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(name.index()))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;

    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(object);
    LayoutID layout = onlyLayoutOfInKeyOrdersAccessors(walker, proven);
    bool layoutIsProven = !!layout;
    if (!layoutIsProven) {
        ExpectationOfValue expectation = findExpectation(walker, access.objectOperand());
        TypeID expected = expectation.type && expectation.isDeclared ? types.withoutNullish(expectation.type) : TypeID();
        if (!expected || !proven || !types.overlap(proven, expected))
            return false;
        layout = onlyLayoutOfInKeyOrdersAccessors(walker, expected);
    }
    PropertyKeyID key = access.key();
    std::optional<FieldReference> field = findAccessorSlotInKeyOrdersAccessors(walker, layout, key);
    bool halfIsProven = field && isProvenHalfInKeyOrdersAccessors(walker, *field, isStore);
    FunctionID provenHalf = halfIsProven ? findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore) : FunctionID();
    if (!field || !shouldOpenGuess(walker, object, types.forLayout(layout), true, closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.noteGuessOpened();

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Walker::ArmsThatMeet arms;
    auto goOnWhereTestPasses = [&](Reference test, BlockID whereTestFails) {
        if (!test)
            return false;
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, whereTestFails);
        walker.continueIn(passed);
        return !walker.isUnreachable();
    };
    auto buildProof = [&](Reference value, Reference test, uint32_t operandIndex) {
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(value);
        proof.setTestOperand(test);
        proof.setOperandIndex(operandIndex);
        proof.setIsOnTakenEdge(true);
        return walker.build(WTF::move(proof));
    };
    auto callByValue = [&](Reference half) {
        Draft call(Opcode::Call, origin);
        if (!isStore)
            call.inputValueGiven = access.id();
        call.setCalleeOperand(half);
        call.setThisValueOperand(object);
        if (isStore)
            call.setArgumentsOperands(singleElementSpan(stored));
        Reference called = walker.build(WTF::move(call));
        if (!isStore)
            walker.bind(access.id(), called);
        walker.listArm(stretch, arms);
    };

    walker.letGuessFailUnasked(asWritten, origin);
    bool goesOn = true;
    if (!layoutIsProven) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(types.at(types.forLayout(layout)).layouts);
        Reference hasShape = walker.build(WTF::move(test));
        goesOn = goOnWhereTestPasses(hasShape, asWritten);
        if (goesOn) {
            object = buildProof(object, hasShape, OperandIndex::ShapeTest::object);
            goesOn = !!object;
            if (goesOn && provenHalf)
                checkProvenHalfInKeyOrdersAccessors(walker, access, object, *field);
            if (goesOn && provenHalf && callAccessor(walker, access, provenHalf, object, stored)) {
                walker.listArm(stretch, arms);
                goesOn = false;
            }
        }
    }
    if (goesOn) {
        Draft look(Opcode::IsSlotFilled, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, look, object, *field, key);
        goesOn = goOnWhereTestPasses(walker.build(WTF::move(look)), asWritten);
    }
    Reference half;
    if (goesOn) {
        Draft load(isStore ? Opcode::GetSlotSetter : Opcode::GetSlotGetter, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, load, object, *field, key);
        if (isStore)
            load.setIsInStrictCode(access.mode() == ECMAMode::Strict);
        else {
            if (!halfIsProven)
                load.representation = Representation::JSValue;
        }
        half = walker.build(WTF::move(load));
    }
    if (walker.hasFailed())
        return true;

    if (half && provenHalf) {
        if (callAccessor(walker, access, provenHalf, object, stored, half))
            walker.listArm(stretch, arms);
        else
            callByValue(half);
        if (walker.hasFailed())
            return true;
        half = { };
    }

    if (half) {
        FunctionID expected = findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore);
        BlockID otherFunction = expected ? walker.addBlock(BlockFrequency::Unlikely) : BlockID();
        Reference isExpected;
        if (expected) {
            Draft test(Opcode::TypeTest, origin);
            test.setValueOperand(half);
            test.setTestedType(types.forFunction(expected));
            isExpected = walker.build(WTF::move(test));
        }
        if (isExpected) {
            Reference closure = goOnWhereTestPasses(isExpected, otherFunction) ? buildProof(half, isExpected, OperandIndex::TypeTest::value) : Reference();
            if (closure && callAccessor(walker, access, expected, object, stored, closure))
                walker.listArm(stretch, arms);
            else if (closure)
                callByValue(closure);
            if (walker.hasFailed())
                return true;
            walker.continueIn(otherFunction);
        }
        if (!walker.isUnreachable())
            callByValue(half);
        if (walker.hasFailed())
            return true;
    }

    walker.walkClosedArm(asWritten, stretch, arms);
    if (!walker.hasFailed())
        walker.letArmsMeet(stretch, arms, meeting);
    return true;
}

static VariableID findVariableOfStaticFieldInKeyOrdersAccessors(Walker& walker, TypeID receiver, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!receiver || !key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(receiver);
    if (!type.isObject() || type.primitives || !type.hasOnlyClosures())
        return { };
    FunctionID constructor = singleFunctionOf(types, type);
    LayoutID classLayout = constructor ? classOfClassObject(program, constructor) : LayoutID();
    if (!classLayout || !walker.facts().isEvaluatedAtMostOnce(classLayout) || !walker.plan().slots().classCreation(classLayout).createsClassObjectOnMutation)
        return { };
    const Layout& record = program.at(classLayout);
    std::optional<uint32_t> field = record.findStaticField(key);
    if (!field)
        return { };
    if (classHolderIsMadeWithName(program, ClassSide::Static, key))
        return { };
    VariableID variable = record.staticFields[*field].variable;
    PlannedVariable planned = walker.plan().variable(variable);
    if (!planned.hasWordInStatics() || planned.encoding != VariableEncoding::Plain || planned.representation != SlotRepresentation::JSValue)
        return { };
    return variable;
}

bool accessStaticFieldInVariable(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    bool isStore = opcode == Opcode::PropSet;
    if ((opcode != Opcode::PropGet && !isStore) || access.isFreeName())
        return false;
    if (PropertyKeyID name = access.key(); !name || liesInThread(name) || !walker.facts().linked().keysOfStaticFields.get(name.index()))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;
    TypeArena& types = walker.types();
    VariableID variable = findVariableOfStaticFieldInKeyOrdersAccessors(walker, walker.typeOf(object), access.key());
    if (!variable)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    Draft read(Opcode::GetStaticFieldOrAbsent, origin);
    read.setVariable(variable);
    read.setValueType(types.withPrimitives(TypeTable::anything, Primitive::Absent));
    read.setClassObjectOperand(object);
    Reference held = walker.build(WTF::move(read));
    if (!held || walker.hasFailed())
        return true;
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(held);
    test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
    Reference isAbsent = walker.build(WTF::move(test));
    if (!isAbsent || walker.hasFailed())
        return true;

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID liesInVariable = walker.addBlock();
    BlockID meeting = walker.addBlock();
    walker.noteGuessOpened();
    walker.branch(isAbsent, asWritten, liesInVariable);

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Walker::ArmsThatMeet arms;

    walker.continueIn(liesInVariable);
    if (!walker.isUnreachable()) {
        if (isStore) {
            Draft store(Opcode::PutStaticField, origin);
            store.setVariable(variable);
            store.setClassObjectOperand(object);
            store.setValueOperand(stored);
            walker.build(WTF::move(store));
        } else {
            Draft proof(Opcode::Narrowed, origin);
            proof.setValueOperand(held);
            proof.setTestOperand(isAbsent);
            proof.setOperandIndex(OperandIndex::TypeTest::value);
            proof.setIsOnTakenEdge(false);
            walker.bind(access.id(), walker.build(WTF::move(proof)));
        }
        walker.listArm(stretch, arms);
    }
    if (walker.hasFailed())
        return true;

    walker.walkClosedArm(asWritten, stretch, arms);
    if (!walker.hasFailed())
        walker.letArmsMeet(stretch, arms, meeting);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

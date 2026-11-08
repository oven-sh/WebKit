/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTSlotPlan.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTStubsConstants.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfClasses()
{
    return {
        Opcode::ResolveHeritage, Opcode::NewClass, Opcode::GetClassPrototype, Opcode::DefineMethod, Opcode::FinishClass,
        Opcode::NewClassToken, Opcode::GetConstructPrototype, Opcode::SuperConstruct, Opcode::SuperConstructList,
    };
}

static StraightArgument operandOfNewClassOrEmpty(ValueID operand)
{
    return operand ? StraightArgument(operand) : StraightArgument::boxed(JSValue());
}

static void generateNewClass(StraightEmitter& emitter, const BodyValue& value)
{
    LayoutID classLayout = value.layout();
    if (!emitter.plan().hasClassRecord(classLayout)) {
        emitter.fail(value);
        return;
    }
    uint32_t wordsOfRequest[] { classLayout.index() };
    StraightArgument request = StraightArgument::number(emitter.mentions().request(RequestKind::NewClass, wordsOfRequest));
    emitter.callOperation(value, RuntimeOperation::NewClass, { request, value.constructorOperand(), operandOfNewClassOrEmpty(value.heritageOperand()), operandOfNewClassOrEmpty(value.prototypeParentOperand()) });
    emitter.define(value);
}

static bool classRecordCreatesMemberInStraightForm(StraightEmitter& emitter, const BodyValue& value)
{
    auto definition = value.as<Opcode::DefineMethod>();
    if (!definition.key() || definition.computedKeyOperand())
        return false;
    const BodyIndex& body = emitter.index();
    auto isMadeBy = [&](ValueID id, Opcode opcode) {
        return id && !body.isParameter(id) && body.opcodeOf(id) == opcode;
    };
    auto inFrontOfCasts = [&](ValueID id) {
        while (id && !body.isParameter(id) && opcodesThatStateTypes.contains(body.opcodeOf(id)))
            id = body.value(id).operand(0);
        return id;
    };
    ValueID closure = definition.functionOperand();
    if (!isMadeBy(closure, Opcode::NewClosure) || !body.hasOneUse(closure))
        return false;
    auto member = body.value(closure).as<Opcode::NewClosure>();

    LayoutID classLayout = definition.layout();
    std::span<const ClassMember> members = emitter.facts().membersOf(classLayout);
    const Vector<MemberCreation>& creations = emitter.plan().slots().classCreation(classLayout).members;
    uint32_t place = definition.index();
    if (place >= members.size() || place >= creations.size() || creations[place] != MemberCreation::ByClassRecord)
        return false;
    const ClassMember& listed = members[place];
    if (listed.function != member.function() || listed.key != definition.key() || listed.side != definition.side() || listed.kind != definition.methodKind())
        return false;

    std::span<const ValueID> capturesOfMember = member.capturesOperands();
    if (capturesOfMember.empty())
        return true;
    ValueID classObject = inFrontOfCasts(definition.classObjectOperand());
    if (capturesOfMember.size() != 1 || !isMadeBy(classObject, Opcode::NewClass))
        return false;
    auto newClass = body.value(classObject).as<Opcode::NewClass>();
    ValueID constructor = inFrontOfCasts(newClass.constructorOperand());
    if (newClass.layout() != classLayout || !isMadeBy(constructor, Opcode::NewClosure))
        return false;
    std::span<const ValueID> capturesOfClassObject = body.value(constructor).as<Opcode::NewClosure>().capturesOperands();
    return capturesOfClassObject.size() == 1 && inFrontOfCasts(capturesOfClassObject[0]) == inFrontOfCasts(capturesOfMember[0]);
}

bool StraightEmitter::classRecordCreatesMemberDefinedBy(const BodyValue& value)
{
    return classRecordCreatesMemberInStraightForm(*this, value);
}

static void generateDefineMethod(StraightEmitter& emitter, const BodyValue& value)
{
    if (classRecordCreatesMemberInStraightForm(emitter, value))
        return;
    auto definition = value.as<Opcode::DefineMethod>();
    PropertyKeyID key = definition.key();
    StraightArgument sideAndKind = StraightArgument::word(DefinedMethodBits::encode(definition.side(), definition.methodKind(), !key, definition.index()));
    if (!key) {
        emitter.callOperation(value, RuntimeOperation::DefineMethod, { definition.classObjectOperand(), definition.computedKeyOperand(), definition.functionOperand(), sideAndKind });
        return;
    }
    if (liesInThread(key) || emitter.program().at(key).kind == PropertyKeyKind::PrivateName) {
        emitter.fail(value);
        return;
    }

    constexpr GPRReg registerOfProperty = operationArgumentGPR(2);
    StubKey makesCell = createConstant(CreatedConstant::Key);
    std::optional<StubKey> makesCellInRegister = tryWithOperandRegister(makesCell, registerOfProperty);
    emitter.callStub(value, makesCellInRegister.value_or(makesCell), { }, { .numberedImmediate = NumberedImmediate { emitter.mentions().add(NumberSpace::KeyCell, key) } });
    StraightArgument property = StraightArgument::inRegister(makesCellInRegister ? registerOfProperty : emitter.resultOfCall());
    emitter.callOperation(value, RuntimeOperation::DefineMethod, { definition.classObjectOperand(), property, definition.functionOperand(), sideAndKind });
}

static void generateSuperConstruct(StraightEmitter& emitter, const BodyValue& value)
{
    Vector<ValueID, 8> arguments { value.newTargetOperand() };
    arguments.append(value.argumentsOperands());
    StraightArgument count = StraightArgument::word(arguments.size());
    emitter.callByConvention(value, AOT::constructValue(ConstructSyntax::SuperCall), { &count, 1 }, StraightArgument(value.parentOperand()), StraightArgument::boxed(jsUndefined()), arguments.span(), { .constructs = true });
    emitter.define(value);
}

void StraightEmitter::generateClass(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::ResolveHeritage:
        callOperation(value, RuntimeOperation::ResolveHeritage, { value.heritageOperand() });
        define(value);
        return;
    case Opcode::NewClass:
        generateNewClass(*this, value);
        return;
    case Opcode::GetClassPrototype:
        callOperation(value, RuntimeOperation::GetClassPrototype, { value.classObjectOperand() });
        define(value);
        return;
    case Opcode::DefineMethod:
        generateDefineMethod(*this, value);
        return;
    case Opcode::FinishClass:
        defineSameAs(value, value.classObjectOperand());
        return;
    case Opcode::NewClassToken:
        if (!plan().hasClassRecord(value.layout())) {
            fail(value);
            return;
        }
        callOperation(value, RuntimeOperation::NewClassToken, { StraightArgument::word(value.layout().index()) });
        define(value);
        return;
    case Opcode::GetConstructPrototype:
        callOperation(value, RuntimeOperation::GetConstructPrototype, { value.newTargetOperand() });
        define(value);
        return;
    case Opcode::SuperConstruct:
        generateSuperConstruct(*this, value);
        return;
    case Opcode::SuperConstructList:
        callStub(value, AOT::superConstructWithArray(), { value.parentOperand(), value.newTargetOperand(), value.listOperand() }, { .constructs = true });
        define(value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

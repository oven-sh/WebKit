/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTCallPlan.h"
#include "AOTConstantEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInstance.h"
#include "AOTLayout.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

void ClassEmitter::generateResolveHeritage(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::ResolveHeritage, { in<Boxed>(value->heritageOperand()) }));
}

void ClassEmitter::generateNewClass(const ValueContext& value)
{
    LayoutID classLayout = value->layout();
    if (!m_plan.hasClassRecord(classLayout)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    uint32_t wordsOfRequest[] { classLayout.index() };
    Word32 request = numberOf(m_mentions.request(RequestKind::NewClass, wordsOfRequest));
    Boxed constructor = boxCell(in<CellPointer>(value->constructorOperand()));
    Boxed heritage = inOptional<Boxed>(value->heritageOperand()).value_or(boxedConstant(JSValue()));
    Boxed prototypeParent = inOptional<Boxed>(value->prototypeParentOperand()).value_or(boxedConstant(JSValue()));
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewClass, { request, constructor, heritage, prototypeParent }));
}

std::optional<Word32> ClassEmitter::rankOfClassWithOnePrototypeObject(const ValueContext& value)
{
    LayoutID classLayout = declaredLayoutOf(m_program, value->layout());
    AccessContext context { m_program, WalkFacts(m_facts), m_plan, types(), { } };
    if (!m_plan.hasClassRecord(classLayout) || !classHasOneClassObject(context, classLayout))
        return std::nullopt;
    return numberOf(m_mentions.add(NumberSpace::ClassRank, classLayout));
}

void ClassEmitter::generateGetClassPrototype(const ValueContext& value)
{
    CellPointer classObject = in<CellPointer>(value->classObjectOperand());
    auto readByOperation = [&](bool isOnRarePath) {
        return callWithoutSite<CellPointer>(RuntimeOperation::GetClassPrototype, { classObject }, { .isOnRarePath = isOnRarePath });
    };
    std::optional<Word32> rank = value->readsClassObjectItself() ? std::nullopt : rankOfClassWithOnePrototypeObject(value);
    if (!rank) {
        define(value, readByOperation(false));
        return;
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::PrototypeInTable);

    static_assert(sizeof(JSCell*) == 1u << logScale(ScalePtr));
    Pointer prototypes = loadImmutablePointer(AOT_INSTANCE_OFFSET(classes.classPrototypes));
    CellPointer prototype(m_out.load64(m_out.baseIndex(m_heaps.AOTInstance_fieldsFilledOnce, prototypes.value(), zeroExtended(*rank).value(), ScalePtr)));
    define(value, either<CellPointer>(Condition(m_out.notZero64(prototype.value())), [&] {
        return prototype;
    }, [&] {
        return readByOperation(true);
    }));
}

void ClassEmitter::generateDefineMethod(const ValueContext& value)
{
    CellPointer classObject = in<CellPointer>(value->classObjectOperand());
    CellPointer function = in<CellPointer>(value->functionOperand());
    PropertyKeyID key = value->key();
    Boxed property = key ? boxCell(emitter<ConstantEmitter>().keyConstant(value, key)) : in<Boxed>(value->computedKeyOperand());
    Word32 sideAndKind = word32(DefinedMethodBits::encode(value->side(), value->methodKind(), !key, value->index()));
    callVoidOperation(value, RuntimeOperation::DefineMethod, { classObject, property, function, sideAndKind });
}

void ClassEmitter::generateFinishClass(const ValueContext& value)
{
    define(value, get(value->classObjectOperand()));
}

void ClassEmitter::generateBeginMemberGuards(const ValueContext& value)
{
    LayoutID constructorFunction = value->layout();
    if (!m_plan.hasClassRecord(constructorFunction)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    LBasicBlock wereNeverBegun = newBlock();
    LBasicBlock continuation = newBlock();
    branch(emitter<GuardEmitter>().guardsWereBegun(constructorFunction), { normalEdge(continuation), rareEdge(wereNeverBegun) });

    m_out.appendTo(wereNeverBegun);
    Operand arguments[] { word32(constructorFunction.index()), word32(value->endsTopLevelOfModule()) };
    callWithoutSite(RuntimeOperation::BeginMemberGuards, arguments, Representation::Void, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void ClassEmitter::generateNewClassToken(const ValueContext& value)
{
    if (!m_plan.hasClassRecord(value->layout())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewClassToken, { word32(value->layout().index()) }));
}

static bool newTargetIsAlwaysProgramFunction(const Type& newTarget)
{
    return newTarget.isObject() && newTarget.hasOnlyClosures();
}

void ClassEmitter::generateGetConstructPrototype(const ValueContext& value)
{
    Boxed newTarget = in<Boxed>(value->newTargetOperand());
    auto readByOperation = [&](bool isOnRarePath) {
        return callOperation<CellPointer>(value, RuntimeOperation::GetConstructPrototype, { newTarget }, { .isOnRarePath = isOnRarePath });
    };
    if (!newTargetIsAlwaysProgramFunction(typeOf(value->newTargetOperand()))) {
        define(value, readByOperation(false));
        return;
    }

    LayoutID classLayout = declaredLayoutOf(m_program, value->layout());
    FunctionID constructor = singleFunctionOf(types(), typeOf(value->newTargetOperand()));
    AccessContext context { m_program, WalkFacts(m_facts), m_plan, types(), { } };
    bool guardAnswers = constructor &&m_program.at(constructor).constructedClass == classLayout && isConstructorPrototypeGuarded(context, classLayout);
    if (std::optional<Word32> rank = guardAnswers ? rankOfClassWithOnePrototypeObject(value) : std::nullopt) {
        m_out.noteForm(Form::Open, StateOfKnowledge::PrototypeInTable);
        LBasicBlock guardHolds = newBlock();
        LBasicBlock wordIsFilled = newBlock();
        LBasicBlock readsProperty = newBlock();
        Join<CellPointer> join(m_out);
        branch(emitter<GuardEmitter>().holds(classLayout, ClassSide::Static, m_program.keyOf(BuiltinNames::prototype)), { normalEdge(guardHolds), rareEdge(readsProperty) });

        m_out.appendTo(guardHolds);
        static_assert(sizeof(JSCell*) == 1u << logScale(ScalePtr));
        Pointer prototypes = loadImmutablePointer(AOT_INSTANCE_OFFSET(classes.classPrototypes));
        CellPointer prototypeInTable(m_out.load64(m_out.baseIndex(m_heaps.AOTInstance_fieldsFilledOnce, prototypes.value(), zeroExtended(*rank).value(), ScalePtr)));
        branch(Condition(m_out.notZero64(prototypeInTable.value())), { normalEdge(wordIsFilled), rareEdge(readsProperty) });

        m_out.appendTo(wordIsFilled);
        join.add(prototypeInTable);

        m_out.appendTo(readsProperty);
        join.add(readByOperation(true));
        define(value, join.finish());
        return;
    }

    m_out.noteForm(Form::Closed, StateOfKnowledge::PrototypeReadByName);
    Boxed prototype = emitter<AccessEmitter>().getById(value, newTarget, m_program.keyOf(BuiltinNames::prototype));
    LBasicBlock isCellBlock = newBlock();
    LBasicBlock isObjectBlock = newBlock();
    LBasicBlock isNoObjectBlock = newBlock();
    Join<CellPointer> join(m_out);
    branch(isCell(prototype), { normalEdge(isCellBlock), rareEdge(isNoObjectBlock) });
    m_out.appendTo(isCellBlock);
    branch(isObjectCell(unboxCell(prototype)), { normalEdge(isObjectBlock), rareEdge(isNoObjectBlock) });
    m_out.appendTo(isObjectBlock);
    join.add(unboxCell(prototype));
    m_out.appendTo(isNoObjectBlock);
    join.add(readByOperation(true));
    define(value, join.finish());
}

void ClassEmitter::generateSuperConstruct(const ValueContext& value)
{
    Boxed parent = in<Boxed>(value->parentOperand());
    Boxed newTarget = in<Boxed>(value->newTargetOperand());
    define(value, emitter<CallEmitter>().constructValue(value, ConstructSyntax::SuperCall, parent, newTarget, value->argumentsOperands()));
}

void ClassEmitter::generateSuperConstructList(const ValueContext& value)
{
    Boxed parent = in<Boxed>(value->parentOperand());
    Boxed newTarget = in<Boxed>(value->newTargetOperand());
    CellPointer list = in<CellPointer>(value->listOperand());
    define(value, emitter<CallEmitter>().constructWithArray(value, ConstructSyntax::SuperCall, parent, newTarget, list));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

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
    // The run time defines the class by its record.
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

// The run time enters the prototype object of a class that is evaluated once in ClassWords::classPrototypes: at the definition of a class, at the first construction by a constructor function.
// A constructor function that captures something has a function object for each environment, and each of those has a prototype object of its own.
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
    std::optional<Word32> rank = rankOfClassWithOnePrototypeObject(value);
    if (!rank) {
        define(value, readByOperation(false));
        return;
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::PrototypeInTable);

    // The word is empty until the prototype object exists, and whoever reads it empty asks the operation. A filled word keeps its cell, which the run time keeps alive.
    // The operation reads the property, and the walk keeps the read behind whatever may store to that: walkGetClassPrototype().
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
    Word32 sideAndKind = word32(DefinedMethodBits::encode(value->side(), value->methodKind()));
    callVoidOperation(value, RuntimeOperation::DefineMethod, { classObject, property, function, sideAndKind });
}

void ClassEmitter::generateFinishClass(const ValueContext& value)
{
    define(value, get(value->classObjectOperand()));
}

// The guard of a side that holds says that the class has begun, and ending the guard of a name ends neither. So a program that has replaced a method pays two bytes here, and no call.
void ClassEmitter::generateBeginMemberGuards(const ValueContext& value)
{
    LayoutID constructorFunction = value->layout();
    LBasicBlock prototypeSideHasEnded = newBlock();
    LBasicBlock mayNotHaveBegun = newBlock();
    LBasicBlock continuation = newBlock();
    branch(emitter<GuardEmitter>().holds(constructorFunction, ClassSide::Prototype, { }), { normalEdge(continuation), rareEdge(prototypeSideHasEnded) });

    m_out.appendTo(prototypeSideHasEnded);
    branch(emitter<GuardEmitter>().holds(constructorFunction, ClassSide::Static, { }), { normalEdge(continuation), rareEdge(mayNotHaveBegun) });

    m_out.appendTo(mayNotHaveBegun);
    Operand arguments[] { word32(constructorFunction.index()) };
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

// The prototype property of a function of the program is a data property, so reading it runs no code and the operation may read it once more.
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

    // `Below.prototype = Object.create(Base.prototype)`, and `new Below()`: walkGetConstructPrototype() has left the one function object of a constructor function to its guards.
    // While the guard of `prototype` holds, nobody has stored there since the class began, so the property holds the object that the run time entered for the class.
    // Only the run time knows which object that is: a function that became a base first was entered with what it held then. So the word is read, and no store of the program is taken for it.
    // The failing side reads the property: for the first object, which begins the guards, and for every object once the program has stored to `prototype` again.
    LayoutID classLayout = declaredLayoutOf(m_program, value->layout());
    FunctionID constructor = singleFunctionOf(types(), typeOf(value->newTargetOperand()));
    AccessContext context { m_program, WalkFacts(m_facts), m_plan, types(), { } };
    bool guardAnswers = constructor &&m_program.at(constructor).constructedClass == classLayout && prototypeOfConstructorFunctionIsLeftToGuards(context, classLayout);
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
    define(value, emitter<CallEmitter>().superConstructWithArray(value, parent, newTarget, list));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

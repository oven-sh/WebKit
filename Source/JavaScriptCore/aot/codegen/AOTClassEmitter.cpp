/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTInstance.h"
#include "AOTLayout.h"
#include "AOTPlacement.h"
#include "AOTRuntimeValidation.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

void ClassEmitter::generateResolveHeritage(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::ResolveHeritage, { in<Boxed>(value->heritageOperand()) }));
}

void ClassEmitter::generateNewClass(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    Boxed constructor = boxCell(in<CellPointer>(value->constructorOperand()));
    Boxed heritage = inOptional<Boxed>(value->heritageOperand()).value_or(boxedConstant(JSValue()));
    Boxed prototypeParent = inOptional<Boxed>(value->prototypeParentOperand()).value_or(boxedConstant(JSValue()));
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewClass, { word32(request->value()), constructor, heritage, prototypeParent }));
}

// The run time enters the prototype object of a class that is evaluated once in ClassWords::classPrototypes: at the definition of a class, at the first construction by a constructor function.
std::optional<uint32_t> ClassEmitter::rankOfClassWithOnePrototypeObject(const ValueContext& value) const
{
    LayoutID classLayout = declaredLayoutOf(m_program, value->layout());
    if (!isEvaluatedOnce(m_program, m_program.at(classLayout)))
        return std::nullopt;
    return m_placement.rankOfClass(classLayout);
}

void ClassEmitter::failUnlessOperationReads(const ValueContext& value, CellPointer classObject, CellPointer prototypeInTable)
{
    LBasicBlock differs = newBlock();
    LBasicBlock continuation = newBlock();
    CellPointer prototype = callWithoutSite<CellPointer>(RuntimeOperation::GetClassPrototype, { classObject });
    branch(equal(prototype, prototypeInTable), { normalEdge(continuation), rareEdge(differs) });

    m_out.appendTo(differs);
    Operand arguments[] { boxCell(prototypeInTable), word32(static_cast<int32_t>(ValidationFailureKind::FastPathResultDiffers)), word32(m_id.index()), word32(value.id().index()) };
    callWithoutSite(RuntimeOperation::ReportValidationFailure, arguments, Representation::Void, { .isOnRarePath = true });
    m_out.unreachable();

    m_out.appendTo(continuation);
}

void ClassEmitter::generateGetClassPrototype(const ValueContext& value)
{
    CellPointer classObject = in<CellPointer>(value->classObjectOperand());
    auto readByOperation = [&](bool isOnRarePath) {
        return callWithoutSite<CellPointer>(RuntimeOperation::GetClassPrototype, { classObject }, { .isOnRarePath = isOnRarePath });
    };
    std::optional<uint32_t> rank = rankOfClassWithOnePrototypeObject(value);
    if (!rank) {
        define(value, readByOperation(false));
        return;
    }

    // The word is empty until the prototype object exists. A filled word keeps its cell, which the run time keeps alive.
    Pointer prototypes = loadImmutablePointer(AOT_INSTANCE_OFFSET(classes.classPrototypes));
    CellPointer prototype(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::AllocatorState), prototypes.value(), static_cast<ptrdiff_t>(*rank) * sizeof(JSCell*))));
    define(value, either<CellPointer>(Condition(m_out.notZero64(prototype.value())), [&] {
        if (generator().validates())
            failUnlessOperationReads(value, classObject, prototype);
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

void ClassEmitter::generateBeginMemberGuards(const ValueContext& value)
{
    Operand arguments[] { word32(value->layout().index()) };
    callWithoutSite(RuntimeOperation::BeginMemberGuards, arguments, Representation::Void, { .isOnRarePath = true });
}

void ClassEmitter::generateNewClassToken(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewClassToken, { word32(value->layout().index()) }));
}

// The prototype property of a function of the program is a data property, so reading it runs no code and the operation may read it once more.
static bool isAlwaysProgramFunction(const Type& type)
{
    return type.isObject() && type.hasOnlyKnownCallees() && !type.layouts && type.builtins.isEmpty();
}

void ClassEmitter::generateGetConstructPrototype(const ValueContext& value)
{
    Boxed newTarget = in<Boxed>(value->newTargetOperand());
    auto readByOperation = [&](bool isOnRarePath) {
        return callOperation<CellPointer>(value, RuntimeOperation::GetConstructPrototype, { newTarget }, { .isOnRarePath = isOnRarePath });
    };
    if (!isAlwaysProgramFunction(typeOf(value->newTargetOperand()))) {
        define(value, readByOperation(false));
        return;
    }

    Boxed prototype = emitter<AccessEmitter>().getById(value, newTarget, m_program.keyOf(BuiltinNames::prototype), SiteCacheID());
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
    define(value, noteObjectMadeByBuiltinBase(value, emitter<CallEmitter>().constructValue(value, ConstructSyntax::SuperCall, parent, newTarget, value->argumentsOperands())));
}

Boxed ClassEmitter::noteObjectMadeByBuiltinBase(const ValueContext& value, Boxed object)
{
    LayoutID layout = value->layout();
    BuiltinClassID base = layout ? builtinBaseClassOf(m_program, layout) : BuiltinClassID();
    if (BuiltinID parent = base ? m_builtins.at(base).constructor : BuiltinID())
        callVoidOperation(value, RuntimeOperation::DidConstructBuiltinBase, { unboxCell(object), in<Boxed>(value->parentOperand()), in<Boxed>(value->newTargetOperand()), word32(layout.index()), word32(parent.index()) });
    return object;
}

void ClassEmitter::generateSuperConstructList(const ValueContext& value)
{
    Boxed parent = in<Boxed>(value->parentOperand());
    Boxed newTarget = in<Boxed>(value->newTargetOperand());
    CellPointer list = in<CellPointer>(value->listOperand());
    define(value, noteObjectMadeByBuiltinBase(value, callOperation<Boxed>(value, RuntimeOperation::SuperConstructList, { parent, newTarget, list }, { .constructs = true })));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

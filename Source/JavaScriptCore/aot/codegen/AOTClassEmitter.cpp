/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinIDs.h"
#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

ClassEmitter::ClassEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

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

void ClassEmitter::generateGetClassPrototype(const ValueContext& value)
{
    define(value, callWithoutSite<CellPointer>(RuntimeOperation::GetClassPrototype, { in<CellPointer>(value->classObjectOperand()) }));
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
    define(value, emitter<CallEmitter>().constructValue(value, ConstructSyntax::SuperCall, parent, newTarget, value->argumentsOperands()));
}

void ClassEmitter::generateSuperConstructList(const ValueContext& value)
{
    Boxed parent = in<Boxed>(value->parentOperand());
    Boxed newTarget = in<Boxed>(value->newTargetOperand());
    CellPointer list = in<CellPointer>(value->listOperand());
    define(value, callOperation<Boxed>(value, RuntimeOperation::SuperConstructList, { parent, newTarget, list }, { .constructs = true }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

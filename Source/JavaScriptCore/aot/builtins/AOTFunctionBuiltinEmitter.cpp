/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTClosureWord.h"
#include "AOTElementAccessEmitter.h"
#include "AOTPlacement.h"
#include "AOTTypeTestEmitter.h"
#include "JSBoundFunction.h"

namespace JSC { namespace AOT { namespace Typed {

FunctionBuiltinEmitter::FunctionBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void FunctionBuiltinEmitter::missUnlessCallableClosure(const InlineCall& call, CellPointer target, const Type& type)
{
    constexpr ptrdiff_t offsetOfSlimClosureHeader = AOT_INSTANCE_OFFSET(slimClosureHeaders[0]);
    Word32 structureID = loadStructureID(target);
    auto hasStructureIDAt = [&](ptrdiff_t offsetOfHeader) {
        return equal(structureID, low32(loadImmutable64(offsetOfHeader)));
    };
    auto missUnlessFullClosure = [&](bool mayBeConstructor) {
        missUnless(call, hasStructureIDAt(AOT_INSTANCE_OFFSET(closures.closureHeaders[0])));
        if (mayBeConstructor)
            missUnless(call, isZero(bitAnd(loadField<Integer>(target, m_heaps.AOTClosure_entry), static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
    };
    if (FunctionID known = type.singleFunction()) {
        switch (m_placement.slots().closureFormOf(known)) {
        case ClosureForm::Slim:
            missUnless(call, hasStructureIDAt(offsetOfSlimClosureHeader));
            return;
        case ClosureForm::Full: {
            FunctionKind kind = m_program.at(known).kind;
            missUnlessFullClosure(kind == FunctionKind::BaseConstructor || kind == FunctionKind::DerivedConstructor);
            return;
        }
        case ClosureForm::Deferred:
        case ClosureForm::Singleton:
            break;
        }
    }
    LBasicBlock isCallableClosure = newBlock();
    LBasicBlock isNotSlim = newBlock();
    branch(hasStructureIDAt(offsetOfSlimClosureHeader), { normalEdge(isCallableClosure), unlikelyEdge(isNotSlim) });
    m_out.appendTo(isNotSlim);
    missUnlessFullClosure(true);
    m_out.jump(isCallableClosure);
    m_out.appendTo(isCallableClosure);
}

void FunctionBuiltinEmitter::generateFunctionBind(const InlineCall& call)
{
    RELEASE_ASSERT(call.numberOfArguments() <= 1 + JSBoundFunction::maxEmbeddedArgs);
    static_assert(JSBoundFunction::maxEmbeddedArgs == 3);
    static_assert(sizeof(JSBoundFunction) == 96 && JSBoundFunction::offsetOfCanConstruct() == JSBoundFunction::offsetOfBoundArgsLength() + sizeof(uint32_t) && JSBoundFunction::offsetOfCanConstruct() + sizeof(uint32_t) == sizeof(JSBoundFunction));
    auto argumentOr = [&](unsigned index, JSValue otherwise) {
        ValueID id = call.argumentOrNull(index);
        return id ? boxedOperand(call.context(), id) : boxedConstant(otherwise);
    };

    Boxed boundThis = argumentOr(0, jsUndefined());
    Boxed boundArgument0 = argumentOr(1, JSValue());
    Boxed boundArgument1 = argumentOr(2, JSValue());
    Boxed boundArgument2 = argumentOr(3, JSValue());
    unsigned numberOfBoundArguments = call.numberOfArguments() ? call.numberOfArguments() - 1 : 0;

    CellPointer target = objectOrMiss(call, call.receiver());
    missUnlessCallableClosure(call, target, typeOf(call.receiver()));

    LBasicBlock prepare = newBlock();
    CellPointer bound;
    {
        CodeGenerator::InitializationScope scope(generator());
        bound = emitter<CellAllocationEmitter>().allocateObject(InlineAllocation::BoundFunction, prepare);
        storeField(bound, m_heaps.JSNonArray_butterfly, pointerConstant(0));
        storeField(bound, m_heaps.JSCallee_scope, loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)));
        storeField(bound, m_heaps.JSFunction_executableOrRareData, loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.boundFunctionExecutable)));
        storeField(bound, m_heaps.JSBoundFunction_targetFunction, target);
        storeField(bound, m_heaps.JSBoundFunction_boundThis, boundThis);
        storeField(bound, m_heaps.JSBoundFunction_boundArg0, boundArgument0);
        storeField(bound, m_heaps.JSBoundFunction_boundArg1, boundArgument1);
        storeField(bound, m_heaps.JSBoundFunction_boundArg2, boundArgument2);
        storeField(bound, m_heaps.JSBoundFunction_nameMayBeNull, pointerConstant(0));
        storeField(bound, m_heaps.JSBoundFunction_length, integer(std::bit_cast<int64_t>(PNaN)));
        storeField(bound, m_heaps.JSBoundFunction_boundArgsLength, word32(numberOfBoundArguments));
        storeField(bound, m_heaps.JSBoundFunction_canConstruct, word32(static_cast<uint8_t>(TriState::Indeterminate)));
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    finish(call, bound);

    m_out.appendTo(prepare);
    callVoidOperation(call.context(), RuntimeOperation::PrepareBoundFunctionAllocation, { }, { .isOnRarePath = true });
    m_out.jump(missBlockOf(call));
}

void FunctionBuiltinEmitter::generateReflectApply(const InlineCall& call)
{
    RELEASE_ASSERT(call.numberOfArguments() == 3);
    Boxed thisValue = boxedOperand(call.context(), call.argument(1));
    CellPointer function = objectOrMiss(call, call.argument(0));
    missUnless(call, emitter<TypeTestEmitter>().isCallable(function));
    CellPointer list = objectOrMiss(call, call.argument(2));
    missUnless(call, emitter<ElementAccessEmitter>().isPristineArray(list, { PristineAspects::Storage, PristineAspects::Names }));
    finish(call, callOperation<Boxed>(call.context(), RuntimeOperation::Apply, { boxCell(function), thisValue, boxCell(list) }));
}

// Bytecode also constructs in place, so the stack has no frame for Reflect.construct, and a construct is never a tail call. The host function throws for invalid arguments, so that call stays where it is written.
void FunctionBuiltinEmitter::generateReflectConstruct(const InlineCall& call)
{
    RELEASE_ASSERT(call.numberOfArguments() == 2 || call.numberOfArguments() == 3);
    auto constructorOrMiss = [&](ValueID id) {
        Boxed constructor = boxCell(objectOrMiss(call, id));
        missUnless(call, callWithoutSite<Condition>(RuntimeOperation::IsConstructor, { constructor }));
        return constructor;
    };
    Boxed target = constructorOrMiss(call.argument(0));
    Boxed list = boxCell(objectOrMiss(call, call.argument(1)));
    Boxed newTarget = call.numberOfArguments() == 3 ? constructorOrMiss(call.argument(2)) : target;
    Origin origin = call.context().origin();
    origin.isTailCall = false;
    finish(call, callOperation<Boxed>(call.context(), RuntimeOperation::ConstructWithArray, { target, newTarget, list }, { .origin = origin, .constructs = true }));
}

void FunctionBuiltinEmitter::generateFunctionApply(const InlineCall& call)
{
    auto argumentOrUndefined = [&](unsigned index) {
        ValueID id = call.argumentOrNull(index);
        return id ? boxedOperand(call.context(), id) : boxedConstant(jsUndefined());
    };
    Boxed function = boxedOperand(call.context(), call.receiver());
    Boxed thisValue = argumentOrUndefined(0);
    Boxed list = argumentOrUndefined(1);
    // The operation has no name for what it refuses.
    if (!typeOf(call.receiver()).isSubsetOf(m_program.at(TypeTable::function), m_program))
        emitter<CallEmitter>().requireCallableBeforeTailCall(call.context(), function, CallEmitter::siteThatNamesCallee(*call.value()));
    finish(call, callOperation<Boxed>(call.context(), RuntimeOperation::Apply, { function, thisValue, list }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTBuiltinTable.h"
#include "AOTControlEmitter.h"
#include "AOTEdgeResults.h"
#include "AOTIdentifierNumbering.h"
#include "AOTInlineCall.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTable.h"
#include "AOTTypeTestEmitter.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t doneTargetIndex = 0;
static constexpr uint32_t missTargetIndex = 1;

static ValueID resultParameterOf(const Function& function, const TerminatorContext& terminator)
{
    RELEASE_ASSERT(terminator.kind() == TerminatorKind::TryCallInline);
    BlockID from = terminator.block();
    if (!numberOfEdgeResults(function, from, doneTargetIndex))
        return { };
    RELEASE_ASSERT(edgeResultAt(function, { from, doneTargetIndex, 0 }).kind == EdgeResultKind::DeclaredResult);
    return function.at(terminator->done().block).parameters[firstEdgeResultParameter(function, from, doneTargetIndex)];
}

Boxed OperationEmitter::boxed(ValueID id) const
{
    return in<Boxed>(id);
}

Integer OperationEmitter::integerOf(ValueID id)
{
    Operand operand = get(id);
    if (operand.is<Word32>())
        return signExtended(operand.as<Word32>());
    return operand.as<Integer>();
}

BuiltinID OperationEmitter::functionOf(BuiltinOverloadID overload) const
{
    return m_builtins.canonicalID(m_builtins.at(overload).builtin);
}

Operand OperationEmitter::callOperation(const OperationContext& context, RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    return emitter<RuntimeCallEmitter>().callOperation(context, operation, arguments, result, options);
}

Operand OperationEmitter::callWithoutSite(RuntimeOperation operation, std::span<const Operand> arguments, Representation result, const CallOptions& options)
{
    return emitter<RuntimeCallEmitter>().callWithoutSite(operation, arguments, result, options);
}

Operand OperationEmitter::callLeaf(RuntimeOperation operation, std::span<const Operand> arguments, Representation result)
{
    return emitter<RuntimeCallEmitter>().callLeaf(operation, arguments, result);
}

StubCallResult OperationEmitter::callStub(const OperationContext& context, StubKey key, std::span<const LValue> operands, const CallOptions& options)
{
    return emitter<RuntimeCallEmitter>().callStub(context, key, operands, options);
}

void OperationEmitter::throwAt(const OperationContext& context, ThrowKind kind, std::optional<Operand> operand, uint32_t a, std::optional<uint32_t> b)
{
    emitter<ThrowEmitter>().throwAt(context, ThrowSite { kind, a, b }, operand);
}

void OperationEmitter::requireOrThrow(const OperationContext& context, Condition mustHold, ThrowKind kind, std::optional<Operand> operand, uint32_t a, std::optional<uint32_t> b)
{
    emitter<ThrowEmitter>().requireOrThrow(context, mustHold, ThrowSite { kind, a, b }, operand);
}

const TypeTestPlan& OperationEmitter::typeTestPlanFor(TypeID from, Representation representation, TypeID to)
{
    return emitter<TypeTestEmitter>().planFor(from, representation, to);
}

Condition OperationEmitter::hasType(Operand operand, const TypeTestPlan& plan, CodeShape shape)
{
    return emitter<TypeTestEmitter>().emitBoolean(operand, plan, shape);
}

void OperationEmitter::branchOnType(Operand operand, const TypeTestPlan& plan, BranchTargets targets, CodeShape shape)
{
    emitter<TypeTestEmitter>().emitBranch(operand, plan, targets, shape);
}

void OperationEmitter::barrierAfterStore(const ValueContext& store, CellPointer owner, Operand stored, const Type& storedType)
{
    emitter<BarrierEmitter>().barrierAfterStore(store, owner, stored, storedType);
}

Representation OperationEmitter::resultRepresentationOf(const InlineCall& call) const
{
    if (const ValueContext* value = call.value())
        return value->representation();
    ValueID parameter = resultParameterOf(m_function, *call.terminator());
    return parameter ? representationOf(parameter) : Representation::Void;
}

const Type& OperationEmitter::resultTypeOf(const InlineCall& call) const
{
    if (const ValueContext* value = call.value())
        return typeOf(value->id());
    ValueID parameter = resultParameterOf(m_function, *call.terminator());
    return parameter ? typeOf(parameter) : m_program.at(TypeTable::undefined);
}

LBasicBlock OperationEmitter::missBlockOf(const InlineCall& call)
{
    RELEASE_ASSERT(call.canMiss());
    if (!call.m_missBlock)
        call.m_missBlock = emitter<ControlEmitter>().countedEdgeTo(*call.terminator(), missTargetIndex).block();
    return call.m_missBlock;
}

void OperationEmitter::finishInlineCall(const InlineCall& call, Operand result)
{
    if (const ValueContext* value = call.value()) {
        define(*value, result);
        return;
    }
    std::span<const Operand> declaredResults;
    if (result.representation != Representation::Void)
        declaredResults = singleElementSpan(result);
    if (!inputs().branchCounters.isCounting()) {
        m_generator.jumpTo(*call.terminator(), doneTargetIndex, declaredResults);
        return;
    }
    m_out.jump(emitter<ControlEmitter>().countedEdgeTo(*call.terminator(), doneTargetIndex, declaredResults).block());
}

Pointer OperationEmitter::argumentBuffer(std::span<const Boxed> values)
{
    return emitter<RuntimeCallEmitter>().argumentBuffer(values);
}

Word32 OperationEmitter::identifierNumber(PropertyKeyID key)
{
    std::optional<uint32_t> number = build().identifierNumbering().numberOf(key);
    if (!number) {
        build().diagnostics().add(InternalError { .kind = InternalErrorKind::OpcodeWithoutCode, .function = m_id, .value = valueOfOrigin(m_out.origin()) });
        return word32(0);
    }
    return word32(static_cast<int32_t>(*number));
}

Pointer OperationEmitter::siteCacheAddress(SiteCacheID id)
{
    if (!id)
        return pointerConstant(0);
    return Pointer(m_out.relocatable(RelocationSpace::SiteCaches, id.index()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

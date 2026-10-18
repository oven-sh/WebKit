/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewError, JSCell*, (Instance* instance, uint32_t type, EncodedJSValue message, EncodedJSValue options))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    ErrorType errorType = static_cast<ErrorType>(type);
    constexpr bool useCurrentFrame = true;
    OPERATION_RETURN(scope, ErrorInstance::create(globalObject, globalObject->errorStructure(errorType), JSValue::decode(message), JSValue::decode(options), nullptr, TypeNothing, errorType, useCurrentFrame));
}

namespace Typed {

using namespace B3;

static ErrorType errorTypeOf(InlineBuiltin which)
{
    switch (which) {
    case InlineBuiltin::NewError:
        return ErrorType::Error;
    case InlineBuiltin::NewEvalError:
        return ErrorType::EvalError;
    case InlineBuiltin::NewRangeError:
        return ErrorType::RangeError;
    case InlineBuiltin::NewReferenceError:
        return ErrorType::ReferenceError;
    case InlineBuiltin::NewSyntaxError:
        return ErrorType::SyntaxError;
    case InlineBuiltin::NewTypeError:
        return ErrorType::TypeError;
    case InlineBuiltin::NewURIError:
        return ErrorType::URIError;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Writes> writesOfInlineErrorJSONGlobal(InlineBuiltin, std::span<const Type> arguments)
{
    for (auto& argument : arguments.first(std::min<size_t>(arguments.size(), 2))) {
        if (argument.hasObjects())
            return Writes::Heap;
    }
    return Writes::RuntimeState;
}

std::optional<Operand> Lowering::lowerInlineErrorJSONGlobal(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& arguments = value.arguments;
    OperationEffects effects { writes, true };
    Type errorClass = Type::engineClass(m_builtins.classes()[m_builtins.at(value.builtin()).result.engineClass]);
    if (isErrorThrownToIgnoringHandler(*m_currentBlock, result)) {
        m_isLazilyFetched.set(result.index());
        return Operand(value.rep, nullptr);
    }
    uint32_t inlineFrame = m_function.at(result).inlineFrame;
    auto& enclosingFunction = inlineFrame ? m_program.at(m_function.inlineFrames[inlineFrame].callee) : m_function;
    if (m_program.isHiddenInStackTraces(enclosingFunction) && !m_program.implementsBuiltin(enclosingFunction))
        return ensure(asCell(callBuiltinFunction(value, effects)), errorClass, typeOf(result));
    if (isErrorThrownAtCreation(*m_currentBlock, result) && isLazilyFetched(arguments[0])) {
        m_isLazilyFetched.set(result.index());
        return Operand(value.rep, nullptr);
    }
    auto argument = [&](unsigned i) {
        return i < arguments.size() ? in<Boxed>(arguments[i]) : undefined();
    };
    return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(NewError, effects, word32(static_cast<int32_t>(errorTypeOf(which))), argument(0), argument(1)), errorClass, typeOf(result));
}

bool Lowering::isErrorThrownAtCreation(const Block& block, ValueID id)
{
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::CallBuiltin || block.terminator.kind != TerminatorKind::Throw || !block.terminator.targets.isEmpty() || block.terminator.value != id || block.values.last() != id || useCountOf(id) != 1)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || familyOf(implementation.inlined) != BuiltinFamily::ErrorJSONGlobal)
        return false;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return false;
    auto& enclosingFunction = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
    if (m_program.isHiddenInStackTraces(enclosingFunction) && !m_program.implementsBuiltin(enclosingFunction))
        return false;
    return value.arguments.size() == 1 && m_function.at(value.arguments[0]).opcode == Opcode::String;
}

bool Lowering::isErrorThrownToIgnoringHandler(const Block& block, ValueID id)
{
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::CallBuiltin || useCountOf(id) != 1)
        return false;
    auto* thrower = &block.terminator;
    if (thrower->kind == TerminatorKind::Invoke && block.values.last() == id)
        thrower = &m_function.at(thrower->targets[0].block).terminator;
    if (thrower->kind != TerminatorKind::Throw || thrower->targets.isEmpty() || thrower->value != id)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || familyOf(implementation.inlined) != BuiltinFamily::ErrorJSONGlobal)
        return false;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return false;
    if (value.arguments.size() > 1 || (!value.arguments.isEmpty() && !typeOf(value.arguments[0]).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull)))
        return false;
    return ignoresCaughtValue(thrower->targets[0].block);
}

void Lowering::throwNewError(const Value& error)
{
    RELEASE_ASSERT(error.opcode == Opcode::CallBuiltin);
    ErrorType type = errorTypeOf(m_builtins.at(error.builtin()).implementation.inlined);
    auto& throwCases = m_currentBlock->terminator.cases;
    uint32_t callSite = m_nextSite;
    throwAt(throwCases.isEmpty() ? ThrowKind::TypeMismatch : ThrowKind::KindMismatch, std::nullopt, m_function.at(error.arguments[0]).string().index(), std::nullopt, static_cast<unsigned>(type) + 1);
    if (throwCases.isEmpty())
        return;
    RELEASE_ASSERT(m_nextSite == callSite + 1);
    uint32_t inlineFrame = throwCases.size() > 1 ? static_cast<uint32_t>(throwCases[1]) : 0;
    m_context.sitePositions.append({ static_cast<uint32_t>(throwCases[0]), inlineFrame });
    m_context.itemSites.append({ callSite, 0, PackedSite::pack(inlineFrame, CallSiteIndex(BytecodeIndex(m_nextSite++)).bits()) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

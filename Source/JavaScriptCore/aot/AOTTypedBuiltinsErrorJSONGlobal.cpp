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

// As constructErrorConstructor(), callErrorConstructor() and NativeErrorConstructor::constructImpl() where `new.target` is the constructor. They have a frame of their own,
// which the stack of the error leaves out. Here the frame at the top is the one that makes the error.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewError, JSCell*, (Instance* instance, uint32_t type, EncodedJSValue message, EncodedJSValue options))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    ErrorType errorType = static_cast<ErrorType>(type);
    constexpr bool useCurrentFrame = true;
    OPERATION_RETURN(scope, ErrorInstance::create(globalObject, globalObject->errorStructure(errorType), JSValue::decode(message), JSValue::decode(options), nullptr, TypeNothing, errorType, useCurrentFrame));
}

namespace Typed {

using namespace B3;

// The family aot/builtins/error-json-global.def.

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
    // ToString of the message, then [[Get]] of `cause`.
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
    if (isErrorThrownToHandlerThatIgnoresIt(*m_currentBlock, result)) {
        m_isFetchedLate.set(result.index());
        return Operand(value.rep, nullptr);
    }
    // A frame that stacks leave out is listed where it is at the very top. Under the frame of the engine's function it is not.
    uint32_t inlineFrame = m_function.at(result).inlineFrame;
    auto& maker = inlineFrame ? m_program.at(m_function.inlineFrames[inlineFrame].callee) : m_function;
    if (m_program.isHiddenInStackTraces(maker) && !m_program.standsForBuiltin(maker))
        return ensure(asCell(callBuiltinFunction(value, effects)), errorClass, typeOf(result));
    if (isErrorThrownWhereMade(*m_currentBlock, result) && isFetchedWhereNeeded(arguments[0])) {
        m_isFetchedLate.set(result.index());
        return Operand(value.rep, nullptr);
    }
    auto argument = [&](unsigned i) {
        return i < arguments.size() ? in<Boxed>(arguments[i]) : undefined();
    };
    return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(NewError, effects, word32(static_cast<int32_t>(errorTypeOf(which))), argument(0), argument(1)), errorClass, typeOf(result));
}

bool Lowering::isErrorThrownWhereMade(const Block& block, ValueID id)
{
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::CallBuiltin || block.terminator.kind != TerminatorKind::Throw || !block.terminator.targets.isEmpty() || block.terminator.value != id || block.values.last() != id || numberOfUsesOf(id) != 1)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || familyOf(implementation.inlined) != BuiltinFamily::ErrorJSONGlobal)
        return false;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return false;
    auto& maker = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
    if (m_program.isHiddenInStackTraces(maker) && !m_program.standsForBuiltin(maker))
        return false;
    return value.arguments.size() == 1 && m_function.at(value.arguments[0]).opcode == Opcode::String;
}

bool Lowering::isErrorThrownToHandlerThatIgnoresIt(const Block& block, ValueID id)
{
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::CallBuiltin || numberOfUsesOf(id) != 1)
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
    auto& placeOfThrow = m_currentBlock->terminator.cases;
    uint32_t callSite = m_nextSite;
    throwAt(placeOfThrow.isEmpty() ? ThrowKind::TypeMismatch : ThrowKind::KindMismatch, std::nullopt, m_function.at(error.arguments[0]).string().index(), std::nullopt, static_cast<unsigned>(type) + 1);
    if (placeOfThrow.isEmpty())
        return;
    RELEASE_ASSERT(m_nextSite == callSite + 1);
    uint32_t inlineFrame = placeOfThrow.size() > 1 ? static_cast<uint32_t>(placeOfThrow[1]) : 0;
    m_context.sitePositions.append({ static_cast<uint32_t>(placeOfThrow[0]), inlineFrame });
    m_context.itemSites.append({ callSite, 0, PackedSite::pack(inlineFrame, CallSiteIndex(BytecodeIndex(m_nextSite++)).bits()) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

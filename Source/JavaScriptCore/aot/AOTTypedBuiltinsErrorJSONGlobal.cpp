/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
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
    // A frame that stacks leave out is listed where it is at the very top. Under the frame of the engine's function it is not.
    uint32_t inlineFrame = m_function.at(result).inlineFrame;
    if (m_program.isHiddenInStackTraces(inlineFrame ? m_program.at(m_function.inlineFrames[inlineFrame].callee) : m_function))
        return ensure(asCell(callBuiltinFunction(value, effects)), errorClass, typeOf(result));
    auto argument = [&](unsigned i) {
        return i < arguments.size() ? in<Boxed>(arguments[i]) : undefined();
    };
    return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(NewError, effects, word32(static_cast<int32_t>(errorTypeOf(which))), argument(0), argument(1)), errorClass, typeOf(result));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTFunction.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static constinit Typed::HostClassEntryPoints s_hostClassEntryPoints;

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewResponse, EncodedJSValue, (Instance* instance, EncodedJSValue body, EncodedJSValue status, EncodedJSValue statusText, EncodedJSValue headers, uint32_t headerCount, const EncodedJSValue* headerNamesAndValues))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue response = s_hostClassEntryPoints.newResponse(globalObject, body, status, statusText, headers, headerCount, headerNamesAndValues);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, response);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewResponseFromResponse, EncodedJSValue, (Instance* instance, EncodedJSValue other))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue response = s_hostClassEntryPoints.newResponseFromResponse(globalObject, other);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, response);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetRequestOrResponseHeader, EncodedJSValue, (Instance* instance, EncodedJSValue holder, EncodedJSValue name, uint32_t knownName))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue header = s_hostClassEntryPoints.getRequestOrResponseHeader(globalObject, holder, name, knownName);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, header);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCallHeadersMethod, EncodedJSValue, (Instance* instance, EncodedJSValue headersOrHolder, EncodedJSValue name, EncodedJSValue value, uint32_t methodAndKnownName))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    EncodedJSValue answer = s_hostClassEntryPoints.callHeadersMethod(globalObject, headersOrHolder, name, value, methodAndKnownName);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, answer);
}

namespace Typed {

using namespace B3;

void setHostClassEntryPoints(const HostClassEntryPoints& entryPoints)
{
    s_hostClassEntryPoints = entryPoints;
}

const HostClassEntryPoints& hostClassEntryPoints()
{
    return s_hostClassEntryPoints;
}

std::optional<Writes> writesOfInlineHost(InlineBuiltin which, std::span<const Type>)
{
    RELEASE_ASSERT(which == InlineBuiltin::NewResponse || which == InlineBuiltin::NewResponseFromResponse || which == InlineBuiltin::GetRequestOrResponseHeader || which == InlineBuiltin::CallHeadersMethod);
    return Writes::Heap;
}

std::optional<Operand> Lowering::lowerInlineHost(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    if (which == InlineBuiltin::NewResponseFromResponse)
        return ensure(AOT_TYPED_OPERATION(NewResponseFromResponse, in<Boxed>(value.arguments[0])), typeOf(result));
    if (which == InlineBuiltin::GetRequestOrResponseHeader || which == InlineBuiltin::CallHeadersMethod) {
        ValueID number = value.arguments.last();
        while (m_function.at(number).opcode == Opcode::Cast)
            number = m_function.at(number).child(0);
        Word32 known = m_function.at(number).opcode == Opcode::Integer ? word32(static_cast<int32_t>(m_function.at(number).integer())) : in<Word32>(number);
        if (which == InlineBuiltin::CallHeadersMethod)
            return ensure(AOT_TYPED_OPERATION(CallHeadersMethod, in<Boxed>(value.arguments[0]), in<Boxed>(value.arguments[1]), in<Boxed>(value.arguments[2]), known), typeOf(result));
        return ensure(AOT_TYPED_OPERATION(GetRequestOrResponseHeader, in<Boxed>(value.arguments[0]), in<Boxed>(value.arguments[1]), known), typeOf(result));
    }
    RELEASE_ASSERT(which == InlineBuiltin::NewResponse);
    constexpr unsigned firstHeaderOperand = 4;
    auto& arguments = value.arguments;
    unsigned headerOperandCount = arguments.size() - firstHeaderOperand;
    LValue buffer = transientBuffer(std::max(headerOperandCount, 1u) * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < headerOperandCount; ++i)
        m_out.store64(in<Boxed>(arguments[firstHeaderOperand + i]).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    return ensure(AOT_TYPED_OPERATION(NewResponse, in<Boxed>(arguments[0]), in<Boxed>(arguments[1]), in<Boxed>(arguments[2]), in<Boxed>(arguments[3]), word32(headerOperandCount / 2), Pointer(buffer)), typeOf(result));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

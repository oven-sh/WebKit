/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationContext.h"
#include "AOTSchemaInlineBuiltins.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class OperationEmitter;

class InlineCall {
    WTF_MAKE_NONCOPYABLE(InlineCall);
public:
    explicit InlineCall(const ValueContext& callInline)
        : InlineCall(callInline, callInline->inlineBuiltin(), callInline->inlineParameters())
    {
    }

    InlineCall(const ValueContext& builtinCall, InlineBuiltin builtin, std::span<const uint32_t> parameters)
        : m_context(builtinCall)
        , m_value(&builtinCall)
        , m_builtin(builtin)
        , m_parameters(parameters)
        , m_overload(builtinCall->overload())
        , m_receiver(builtinCall->receiverOperand())
        , m_arguments(builtinCall->argumentsOperands())
    {
    }

    explicit InlineCall(const TerminatorContext& tryCallInline)
        : m_context(tryCallInline)
        , m_terminator(&tryCallInline)
        , m_builtin(tryCallInline->inlineBuiltin())
        , m_parameters(tryCallInline->inlineParameters())
        , m_overload(tryCallInline->overload())
        , m_receiver(tryCallInline->receiverOperand())
        , m_arguments(tryCallInline->argumentsOperands())
    {
    }

    const OperationContext& context() const LIFETIME_BOUND { return m_context; }
    const ValueContext* value() const { return m_value; }
    const TerminatorContext* terminator() const { return m_terminator; }
    bool canMiss() const { return m_terminator; }

    InlineBuiltin builtin() const { return m_builtin; }
    BuiltinOverloadID overload() const { return m_overload; }
    unsigned numberOfParameters() const { return m_parameters.size(); }
    uint32_t parameterWord(unsigned index) const { return m_parameters[index]; }
    template<typename Enum> Enum parameter(unsigned index) const { return static_cast<Enum>(m_parameters[index]); }

    ValueID receiver() const { return m_receiver; }
    std::span<const ValueID> arguments() const LIFETIME_BOUND { return m_arguments; }
    unsigned numberOfArguments() const { return m_arguments.size(); }
    ValueID argument(unsigned index) const { return m_arguments[index]; }
    ValueID argumentOrNull(unsigned index) const { return index < m_arguments.size() ? m_arguments[index] : ValueID(); }


private:
    friend class OperationEmitter;

    const OperationContext& m_context;
    const ValueContext* m_value { nullptr };
    const TerminatorContext* m_terminator { nullptr };
    InlineBuiltin m_builtin;
    std::span<const uint32_t> m_parameters;
    BuiltinOverloadID m_overload;
    ValueID m_receiver;
    std::span<const ValueID> m_arguments;
    mutable LBasicBlock m_missBlock { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTKnownClosures.h"
#include "AOTResultRules.h"
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class FlowResults;
class FunctionAnalyses;

using CalleeFunctor = ScopedLambda<void(FunctionID)>;
using LayoutFunctor = ScopedLambda<void(LayoutID)>;
using ValueFunctor = ScopedLambda<void(ValueID)>;
using VariableFunctor = ScopedLambda<void(VariableID)>;

struct CalleesBeforeFlowInputs {
    const KnownClosures& closures;
    const FlowResults& noFlowResults;
    FunctionID id;
    FunctionAnalyses& analyses;
    const TypingContext& typing;
};

// In one function whose values are typed: which functions a call may enter and which members a read by name may find, as far as the program's own operations and the types
// as read say it. A callee that arrives any other way is not listed.
class CalleesBeforeFlow {
public:
    explicit CalleesBeforeFlow(const CalleesBeforeFlowInputs& inputs)
        : m_closures(inputs.closures)
        , m_noFlowResults(inputs.noFlowResults)
        , m_id(inputs.id)
        , m_analyses(inputs.analyses)
        , m_typing(inputs.typing)
        , m_program(inputs.typing.program)
        , m_function(inputs.typing.function)
        , m_types(inputs.typing.types)
    {
    }

    void forEachIncomingValue(BlockID, uint32_t parameterIndex, NOESCAPE const ValueFunctor&) const;
    // The layouts in the type and in the expected type of the value.
    void forEachExpectedLayout(ValueID, NOESCAPE const LayoutFunctor&) const;
    void forEachKnownCallee(ValueID callee, NOESCAPE const CalleeFunctor&) const;
    void forEachInheritedClosure(ClosureHolder::Kind, LayoutID, ClassSide, PropertyKeyID, NOESCAPE const CalleeFunctor&) const;
    void forEachStaticField(ValueID classObject, PropertyKeyID, NOESCAPE const VariableFunctor&) const;

    // What the declaration of the callee says, where no other function can be called.
    std::optional<Type> tryTypeOfResultOfCertainCallee(const Value& call) const;

private:
    void forEachDefinition(ValueID, NOESCAPE const ValueFunctor&) const;
    void forEachHeldClosure(ValueID, NOESCAPE const CalleeFunctor&) const;
    bool forEachClosureInHolderReadBy(ValueID definition, NOESCAPE const CalleeFunctor&) const;
    void forEachKnownMethod(const Value& read, NOESCAPE const CalleeFunctor&) const;
    void forEachKnownSuperMethod(const Value& read, NOESCAPE const CalleeFunctor&) const;
    void forEachConstructedClass(ValueID classObject, NOESCAPE const LayoutFunctor&) const;
    void forEachClosureIn(ClosureHolder, NOESCAPE const CalleeFunctor&) const;
    std::optional<uint32_t> captureIndexOf(ValueID) const;
    FunctionID findCertainCallee(const Value& call) const;
    std::optional<Type> tryTypeOfResultOfOnlyClosure(const Value& call) const;
    std::optional<Type> tryTypeOfResultOfLockedBuiltin(const Value& call) const;

    const KnownClosures& m_closures;
    const FlowResults& m_noFlowResults;
    FunctionID m_id;
    FunctionAnalyses& m_analyses;
    const TypingContext& m_typing;
    const Program& m_program;
    const Function& m_function;
    TypeArena& m_types;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

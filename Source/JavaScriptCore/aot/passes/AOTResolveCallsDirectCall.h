/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTVector.h"
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

using InsertValue = ScopedLambda<ValueID(Value&&)>;

struct ValueInsertion {
    const AnalysisContext& context;
    Function& function;
    TypeArena& types;
    Origin origin;
    const InsertValue& insert;
};

ValueID convertTo(IRBuilder&, ValueID, const DeclaredType& target);
ValueID convertTo(const ValueInsertion&, ValueID, const DeclaredType& target);

struct DirectCallRequest {
    FunctionID callee;
    std::span<const ValueID> captures;
    ValueID thisValue;
    ValueID calleeValue;
    ValueID newTarget;
    std::span<const ValueID> arguments;
    ValueID restArray;
    ValueID argumentCount;
    ValueID hiddenRest;
    AccessSiteID accessSite;
    BuiltinID calledBuiltin;
    bool isConstruction { false };
    bool isNeverInlined { false };
    bool isAlwaysInlined { false };
    bool isTailCall { false };
    // The caller has made the function object itself and has handed it to nobody.
    bool callsOwnFunctionObject { false };
    // The function object that is called, where a frame of the callee is named by it once a function object of the callee was renamed. The caller keeps it in a word of its own frame.
    ValueID calleeKeptInFrame;
    // Flow has not followed the callee to the call, so it does not count the caller among the callers of the callee.
    bool calleeIsPredicted { false };
};

ValueID appendDirectCall(IRBuilder&, const DirectCallRequest&);
Value makeDirectCall(const ValueInsertion&, const DirectCallRequest&);

Vector<ValueID, 8> appendCaptures(IRBuilder&, FunctionID, ValueID closure);
void emitReturnOfCall(IRBuilder&, ValueID result);
TailCallRequirement tailCallRequirementOf(const Function& body);

bool mayBeCalledDirectly(const Program&, FunctionID callee);
// Also a function whose calls by value record their callee from the first time on that a function object is renamed where nobody sees: whoever calls it directly stands behind AreFunctionNamesUnchanged.
bool mayBeCalledDirectlyWhileFunctionNamesAreUnchanged(const Program&, FunctionID callee);
// A function that records its callee for stack frames does so in its entrypoint, which a call by value enters.
bool mayBeCalledDirectlyOrThroughEntrypoint(const Program&, FunctionID callee);
bool canCallWithoutClosure(const Program&, FunctionID callee);
bool canConstructDirectly(const Program&, FunctionID constructor);
// The constructor makes what it returns itself, from new.target alone.
bool canConstructWithoutClosure(const Program&, FunctionID constructor);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

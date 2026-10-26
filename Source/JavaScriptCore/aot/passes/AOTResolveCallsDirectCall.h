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
};

ValueID appendDirectCall(IRBuilder&, const DirectCallRequest&);
Value makeDirectCall(const ValueInsertion&, const DirectCallRequest&);

Vector<ValueID, 8> appendCaptures(IRBuilder&, FunctionID, ValueID closure);
void emitReturnOfCall(IRBuilder&, ValueID result);
TailCallRequirement tailCallRequirementOf(const Function& body);

bool mayBeCalledDirectly(const Program&, FunctionID callee);
// A function that records its callee for stack frames does so in its entrypoint, which a call by value enters.
bool mayBeCalledDirectlyOrThroughEntrypoint(const Program&, FunctionID callee);
bool canCallWithoutClosure(const Program&, FunctionID callee);
bool canConstructDirectly(const Program&, FunctionID constructor);
// The constructor makes what it returns itself, from new.target alone.
bool canConstructWithoutClosure(const Program&, FunctionID constructor);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

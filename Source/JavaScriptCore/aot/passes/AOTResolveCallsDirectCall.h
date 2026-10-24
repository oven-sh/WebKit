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
    bool isConstruction { false };
    bool isNeverInlined { false };
    bool isDemotedTailCall { false };
};

ValueID appendDirectCall(IRBuilder&, const DirectCallRequest&);
Value makeDirectCall(const ValueInsertion&, const DirectCallRequest&);
// A validating build tests the callee of a call that its type alone made direct.
void appendCalleeValidation(IRBuilder&, ValueID callee);

Vector<ValueID, 8> appendCaptures(IRBuilder&, FunctionID, ValueID closure);
Vector<ValueID, 8> insertCaptures(const ValueInsertion&, FunctionID, ValueID closure);
void emitReturnOfCall(IRBuilder&, ValueID result);
TailCallRequirement tailCallRequirementOf(const Function& body);

bool canPassRestArguments(const Program&, const Function& callee);
bool mayBeCalledDirectly(const Program&, FunctionID callee);
bool canCallWithoutClosure(const Program&, FunctionID callee);
bool canConstructDirectly(const Program&, FunctionID constructor);
// The constructor makes what it returns itself, from new.target alone.
bool canConstructWithoutClosure(const Program&, FunctionID constructor);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

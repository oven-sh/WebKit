/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTForwardedCalls.h"
#include "AOTMethodDispatch.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The operations that ResolveCalls looks at.
inline constexpr OpcodeSet callSiteOpcodes {
    Opcode::Call,
    Opcode::CallList,
    Opcode::Construct,
    Opcode::ConstructList,
    Opcode::SuperConstruct,
    Opcode::SuperConstructList,
    Opcode::PropGet,
    Opcode::PropSet,
    Opcode::SuperGet,
    Opcode::SuperSet,
    Opcode::InstanceOf,
    Opcode::CallBuiltin,
};

bool canTakeAllocatedThis(const Program&, FunctionID constructor);
// The class of the constructor, or the layout derived from it that this version of the constructor allocates.
LayoutID layoutAllocatedBy(const Program&, FunctionID constructor);
bool isConstructorFunction(const Program&, const Function&);
// What `new` hands a constructor function that is its own new.target: an object with the layout of the function. `closure` is that function.
ValueID appendThisOfConstructorFunction(IRBuilder&, FunctionID constructor, ValueID closure);
// Ends the block: the object is returned unless the constructor has returned another.
void emitReturnOfConstruction(IRBuilder&, ValueID returned, ValueID object);

struct MemberRead {
    Vector<ValueSite, 2> links;
    // What a validating build looks at the links with. It goes with them.
    Vector<ValueID, 2> validations;
    bool canSinkToCall { false };

    bool isEmpty() const { return links.isEmpty(); }
    const ValueSite& read() const LIFETIME_BOUND { return links.first(); }
};

MemberRead findMemberRead(FunctionPassContext&, const ValueSite& call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

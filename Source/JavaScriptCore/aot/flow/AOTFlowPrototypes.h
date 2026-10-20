/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowPropertyLocations.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowDefaultPrototypeKind : uint8_t {
    Unknown,
    Null,
    Builtin,
};

struct FlowDefaultPrototype {
    FlowDefaultPrototypeKind kind { FlowDefaultPrototypeKind::Unknown };
    BuiltinID builtin;

    friend bool operator==(const FlowDefaultPrototype&, const FlowDefaultPrototype&) = default;
};

FlowDefaultPrototype defaultPrototypeOf(const Program&, const FlowObjectTable&, FlowObjectID);
FlowDefaultPrototype defaultPrototypeOf(const BuiltinTable&, BuiltinID);
BuiltinID prototypeOfFunctionObject(const Function&);
BuiltinID prototypeOfPrototypeProperty(const Function&);

// The table lists an own property that the engine creates with every instance under the prototype of the class, and no location of the instance stands for it.
void forEachKeyCreatedWithObject(const Program&, const FlowObjectTable&, FlowObjectID, NOESCAPE const ScopedLambda<void(PropertyKeyID)>&);
// The table lists a property that every instance of a built-in class is created with under the prototype of the class, and that is where a walk finds it.
// What holds the property is the first object from the start of the lookup upward that is created with it. On a way up without one the listing prototype answers.
void forEachHolderOfInstanceProperty(FlowSolver&, FlowObjectID start, PropertyKeyID, FlowObjectID listingPrototype, FlowStatementSite waiter, NOESCAPE const ScopedLambda<void(FlowObjectID)>&);

struct FlowPrototypeSet {
    Vector<FlowHolder, 2> holders;
    // Counts the holders that were left out too.
    size_t numberOfHolders { 0 };
    bool isNeverNull { false };
    bool isNeverUnknown { false };
};

// A walk leaves out the objects it has visited: a link may hold many, and many links the same.
FlowPrototypeSet prototypesOf(const FlowWalkInputs&, const FlowHolder&, FlowStatementSite waiter, const FlowObjectSet* leftOut = nullptr);
// For a built-in prototype on which names are closed as absent. What its link holds without the program changing it has come through the unknown object, which the run time refuses.
FlowPrototypeSet prototypesFixedAtStartUp(const FlowWalkInputs&, const FlowHolder&, FlowStatementSite waiter, const FlowObjectSet* leftOut = nullptr);

FlowNodeID ensurePrototypeLink(FlowSolver&, FlowObjectID);

FlowProgress refinePrototypeUses(FlowSolver&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

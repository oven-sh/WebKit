/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltins.h"

namespace JSC { namespace AOT { namespace Typed {

class CFG;
class UseLists;

// `JSON.parse(text) as T`, `await response.json()`, a call whose result is validated: a pass puts compiled parsers and validators in the place of the call, and they allocate objects with the layouts of the type.
struct ValueFlowTypedResult {
    // None where nothing is allocated by type.
    TypeID type;
    bool isValidated { false };
    // Ascending.
    Vector<LayoutID, 4> layouts;
    // The types with arrays among their values that the type reaches through elements and fields. Ascending. One array stands for all that are made for such a type.
    Vector<TypeID, 4> typesWithArrays;
    // The layouts that are also allocated where no type leads: as the value of a dictionary, an undeclared member, a member without a field.
    Vector<LayoutID, 4> layoutsAmongPlainData;

    explicit operator bool() const { return !!type; }

    // The two objects that any built-in allocates at a call come first, then the layouts, the arrays, and one array for those that the generic parser makes where a compiled one hands a part of the text to it.
    static constexpr uint32_t firstOrdinal = 2;
    uint32_t ordinalOfLayout(size_t index) const { return firstOrdinal + static_cast<uint32_t>(index); }
    uint32_t ordinalOfArray(size_t index) const { return ordinalOfLayout(layouts.size() + index); }
    uint32_t ordinalOfPlainArray() const { return ordinalOfArray(typesWithArrays.size()); }
    uint32_t numberOfObjects() const { return ordinalOfPlainArray() + 1 - firstOrdinal; }
};

// A type that reaches more has no typed result: every call that may be one pays for its objects in every set.
inline constexpr uint32_t maxObjectsOfTypedResult = 256;

// Depends on the program alone, so the bodies, the rules and the results get the same answer.
ValueFlowTypedResult findTypedResult(const Program&, const ValueFlowBuiltinTables&, const Function&, const UseLists&, ValueID call);
ValueFlowTypedResult findTypedResult(const Program&, const ValueFlowBuiltinTables&, FunctionID, ValueID call);

// For the bodies: the sites of every call of the function that has a typed result if its callee turns out to make one.
void appendSitesOfTypedResults(const Program&, const ValueFlowBuiltinTables&, const Function&, const CFG&, Vector<ValueFlowSite>&);

// For the rule of a built-in: `call` is the operation that applies the overload, `promise` the object that the overload answers, or none.
void allocateTypedResult(const BuiltinOverload&, const ValueFlowStatement& call, const ValueFlowBuiltinCall&, ValueFlowObjectID promise, ValueSetID plainData, ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

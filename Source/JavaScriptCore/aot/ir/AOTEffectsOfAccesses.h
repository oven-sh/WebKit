/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffects.h"
#include "AOTSortedSet.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct Type;

struct ProvenAccess {
    bool isComplete { false };
    bool mayFindNothing { false };
    // The run time looks the key up in the receiver and in the locked built-ins above it.
    bool isLookupByKey { false };
    // The prototype property of a function that was created with one: an own data property, which no layout lists.
    bool mayFindPrototypeProperty { false };
    // The run time finds or adds an own property of an ordinary object under the key, and runs nobody's code.
    bool definesOwnProperty { false };
    Vector<Opcode, 2> operations;
    Vector<FunctionID, 1> accessors;
    Vector<FunctionID, 1> methods;
    Vector<BuiltinID, 1> builtins;
    PropertyKeyID key;
    LayoutSetID receivers;

    static ProvenAccess unknown() { return { }; }

    friend bool operator==(const ProvenAccess&, const ProvenAccess&) = default;
};

ProvenAccess provenAccessOf(const Program&, const Function&, const Value& access);
// Without a copy: the answer is `storage`, or one that stays where it is for as long as the caches of the program stand (ProgramCaches).
const ProvenAccess& provenAccessOf(const Program&, const Function&, const Value& access, ProvenAccess& storage LIFETIME_BOUND);
ProvenAccess provenLookupUnderAnyKey(const Program&, const Type& receiver);
ProvenAccess provenReadOf(const Program&, const Type& receiver, BuiltinName);
ProvenAccess provenStepOf(const Program&, const Type& receiver, Protocol keylessProtocol);

// The types of all that the own properties of the objects hold. No answer where an object may have a property that is no field, or a field without a proven type.
std::optional<SortedSet<TypeID, 8>> provenOwnValueTypesOf(const Program&, const Type& layouts);

Effects effectsOfAccessForm(const Program&, Opcode operation, PropertyKeyID key, LayoutSetID receivers);
std::optional<Effects> effectsOfLookupFromBase(const Program&, const Function&, const Value&);

// The positions for which an element operation of an array touches no named property. The emitters ask the same question.
bool isArrayIndex(const Type& position);
enum class ElementPosition : uint8_t { Anywhere, InsideLength };
void refineElementEffects(Opcode operation, const Type& position, ElementPosition, Effects&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

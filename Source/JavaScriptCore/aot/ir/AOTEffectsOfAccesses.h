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
    bool isLookupByKey { false };
    bool mayFindPrototypeProperty { false };
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
const ProvenAccess& provenAccessOf(const Program&, const Function&, const Value& access, ProvenAccess& storage LIFETIME_BOUND);
ProvenAccess provenLookupUnderAnyKey(const Program&, const Type& receiver);
ProvenAccess provenReadOf(const Program&, const Type& receiver, BuiltinName);
ProvenAccess provenStepOf(const Program&, const Type& receiver, Protocol keylessProtocol);

std::optional<SortedSet<TypeID, 8>> provenOwnValueTypesOf(const Program&, const Type& layouts);

Effects effectsOfAccessForm(const Program&, Opcode operation, PropertyKeyID key, LayoutSetID receivers);
std::optional<Effects> effectsOfLookupFromBase(const Program&, const Function&, const Value&);

bool isArrayIndex(const Type& position);
enum class ElementPosition : uint8_t { Anywhere, InsideLength };
void refineElementEffects(Opcode operation, const Type& position, ElementPosition, Effects&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

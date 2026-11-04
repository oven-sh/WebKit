/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

struct UnobservedFieldDefinition {
    FunctionID initializer;
    ValueID definition;
    LayoutID classLayout;
    PropertyKeyID key;
    // Somebody may hold the object, and sees the value where a definition that follows throws.
    bool isObservedWhereObjectRefusesDefinition { false };
};

class UnobservedFieldDefinitions {
public:
    UnobservedFieldDefinitions() = default;
    UnobservedFieldDefinitions(const Program&, BuildContext&, NOESCAPE const ScopedLambda<bool(FunctionID)>& mayHoldDirectCall);

    const UnobservedFieldDefinition* find(FunctionID initializer, ValueID definition) const LIFETIME_BOUND;
    std::span<const UnobservedFieldDefinition> all() const LIFETIME_BOUND { return m_definitions.span(); }
    // `module.exports = f` on the one way that the body of a CommonJS file begins with: nobody finds the `undefined` that the variable starts with.
    // Another file reads the variable right behind the `require` that has begun the body. That is late enough where nothing runs ahead of the store, or where the `require` comes back behind the end of the body.
    bool isStoredAheadOfEveryRead(VariableID variable) const { return m_variablesStoredAheadOfEveryRead.get(variable.index()); }

private:
    Vector<UnobservedFieldDefinition> m_definitions;
    BitVector m_variablesStoredAheadOfEveryRead;
};

Vector<PropertyKeyID> keysDefinedRightAfterAllocation(const Program&, FunctionID constructor, ValueID allocation);

using KeysOfLiterals = HashMap<uint32_t, Vector<PropertyKeyID, 8>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
KeysOfLiterals keysDefinedBehindAllocationsOfLiterals(const Function&);

KeysOfLiterals keysStoredRightAfterAllocations(const Program&, const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

struct UnobservedFieldDefinition {
    FunctionID initializer;
    ValueID definition;
    LayoutID classLayout;
    PropertyKeyID key;
};

class UnobservedFieldDefinitions {
public:
    UnobservedFieldDefinitions() = default;
    UnobservedFieldDefinitions(const Program&, BuildContext&);

    bool contains(FunctionID initializer, ValueID definition) const;
    std::span<const UnobservedFieldDefinition> all() const LIFETIME_BOUND { return m_definitions.span(); }

private:
    Vector<UnobservedFieldDefinition> m_definitions;
};

Vector<PropertyKeyID> keysDefinedRightAfterAllocation(const Program&, FunctionID constructor, ValueID allocation);

// A literal defines the members that its allocation does not list behind it, and FinishObject hands the object out: until then nobody else holds it, whatever the values of the members run.
using KeysOfLiterals = HashMap<uint32_t, Vector<PropertyKeyID, 8>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
KeysOfLiterals keysDefinedBehindAllocationsOfLiterals(const Function&);

// `const o = {}; o.a = 1`, `constructor(w) { this.w = w }`: for each NewObject or Data of the function, the names of the strict stores that follow it in its block before any other use of the object.
KeysOfLiterals keysStoredRightAfterAllocations(const Program&, const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

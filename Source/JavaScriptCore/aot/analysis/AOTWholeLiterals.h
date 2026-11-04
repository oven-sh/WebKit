/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTKnownValue.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
struct ConstantMember;

BitVector collectSitesOfWholeLiterals(const Program&, const FlowResults&, BuildContext&);

std::optional<Vector<KnownValue, 8>> findConstantsOfWholeLiteral(const Program&, const FlowResults&, const Function&, ValueID);

std::optional<Vector<KnownValue, 8>> findConstantsOfWholeList(const Program&, const FlowResults&, const Function&, ValueID);

// The names and values of `{ year: "numeric", hour12: false }`, whoever may hold it. None where a value is no primitive constant.
std::optional<Vector<ConstantMember, 8>> findMembersOfObjectLiteral(const Program&, const Function&, const Value& literal);
// The same for a literal that nothing ever changes.
std::optional<Vector<ConstantMember, 8>> findMembersOfWholeObject(const Program&, const FlowResults&, const Function&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include <optional>

namespace JSC { namespace AOT {

struct State;

inline constexpr uint32_t noClassGuardBit = UINT32_MAX;

constexpr uint32_t byteOffsetOfClassGuardBit(uint32_t bit) { return bit / 8; }
constexpr uint8_t maskOfClassGuardBit(uint32_t bit) { return static_cast<uint8_t>(1u << bit % 8); }

inline constexpr uint8_t maskOfClassGuardsBegun = maskOfClassGuardBit(1);

bool classGuardHolds(const State&, const LayoutData& classLayout, ClassSide);
bool classGuardOfNameHolds(const State&, const LayoutData& classLayout, ClassSide, uint32_t key);

void endClassGuard(State&, const LayoutData& classLayout, ClassSide);
void endGuardsOfInheritedMembers(State&, const LayoutData& classLayout, ClassSide);
enum class GuardsOfNameToEnd : uint8_t { All, StoresStillPass, ReadsStillFind, OneObjectHidesName };
void endClassGuardsOfName(State&, const LayoutData& classLayout, ClassSide, std::optional<uint32_t> key, GuardsOfNameToEnd = GuardsOfNameToEnd::All);

bool declaresMember(const LayoutData& classLayout, ClassSide, uint32_t key);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

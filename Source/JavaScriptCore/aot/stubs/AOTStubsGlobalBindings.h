/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class GlobalBindingScope : uint8_t { ScriptsAndGlobalObject, GlobalObject };

// The immediate: the binding's number.
constexpr StubKey getGlobalBinding(GlobalBindingScope scope) { return StubKey(StubFamily::GetGlobalBinding).withForm(static_cast<uint8_t>(scope)); }

// The operand: the value. The immediate: the binding's number.
constexpr StubKey setGlobalBinding(GlobalBindingScope scope, bool isStrict) { return StubKey(StubFamily::SetGlobalBinding).withForm(static_cast<uint8_t>(2 * static_cast<uint8_t>(scope) + (isStrict ? 0 : 1))); }

static_assert(exists(getGlobalBinding(GlobalBindingScope::GlobalObject)) && !exists(getGlobalBinding(GlobalBindingScope::GlobalObject).withForm(2)));
static_assert(exists(setGlobalBinding(GlobalBindingScope::GlobalObject, false)) && !exists(setGlobalBinding(GlobalBindingScope::GlobalObject, false).withForm(4)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

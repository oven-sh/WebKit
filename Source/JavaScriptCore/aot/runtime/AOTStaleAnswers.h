/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC { namespace AOT {

struct Instance;

enum class KindOfAnswer : uint8_t { Value, Absent, Accessor, Present };

enum class StubThatAnswers : uint8_t { CallMethodAtSite };

constexpr uint32_t kindAndStubOfAnswer(KindOfAnswer kind, StubThatAnswers stub) { return static_cast<uint32_t>(kind) | static_cast<uint32_t>(stub) << 8; }

void stopAtStaleAnswer(Instance&, JSValue receiver, UniquedStringImpl* name, KindOfAnswer, JSValue value, ASCIILiteral whoAnswered);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

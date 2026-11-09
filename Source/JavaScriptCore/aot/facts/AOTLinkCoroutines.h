/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"
#include "AOTLinkTables.h"

namespace JSC { namespace AOT { namespace Typed {

struct FactTables;
struct Function;

void writeCoroutinesOfLink(const Program&, const LinkFlow&, TypeArena&, LinkTables&);
void forEachFunctionNamedByCoroutinesCalledBy(const Program&, const LinkTables&, FunctionID referrer, const ScopedLambda<void(FunctionID)>&);

constexpr bool slotSpeaksOfBodyOfCoroutine(SlotOfContents slot) { return slot == SlotOfContents::returnedAlone(); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

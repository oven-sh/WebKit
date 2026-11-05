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

// WriterOfLink::write(), behind writeFunctions(), in both readings. THE writer of LinkTables::coroutines. It reads nodes that have settled and adds none.
void writeCoroutinesOfLink(const Program&, const LinkFlow&, TypeArena&, LinkTables&);
// WriterOfLink::forEachNodeNamedBy(): functor(FunctionID) for each function that a type of LinkTables::coroutines names, of each coroutine that a listed call of `referrer` enters.
void forEachFunctionNamedByCoroutinesCalledBy(const LinkTables&, TypeReader, FunctionID referrer, const ScopedLambda<void(FunctionID)>&);

// StateOfLinkReach::bringAlive(). What a body yields and what it returns is the body's, whoever has got hold of the object that a call answered with. So no family of sites has a node for either:
// the node of the summary stands by itself, nothing leads out of it, and an object that escapes or meets another opens nothing there.
constexpr bool slotSpeaksOfBodyOfCoroutine(SlotOfContents slot) { return slot == SlotOfContents::yieldedAlone() || slot == SlotOfContents::returnedAlone(); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

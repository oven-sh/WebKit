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
// LinkFlow::toTypeID(), of LinkType::Classes::Site. The class of the site with what the family of the site was seen to hold as ClassPart::writtenContents: AN EXPECTATION AT EVERY LEVEL, in both readings.
// The contents of contents, and closures, are left out. The class alone: nobody has asked for the node, nothing or anything has arrived.
TypeID typeOfInstancesWithContentsSeen(const LinkFlow&, TypeArena&, const LinkType& instances);
// WriterOfLink::oneCalleeOf(), in front of its first return. The call enters built-ins alone, every row of each of them has one callback, called once before the row returns (BuiltinCallbackTimes::Once),
// and closures of one function are all that arrives at that operand. Null: it is no such call.
FunctionID findCallbackCalledOnce(const Program&, const LinkFlow&, FunctionID caller, const SummaryCall&, const LinkFlow::CalleesOfCall&);
// WriterOfLink::forEachNodeNamedBy(): functor(FunctionID) for each function that a type of LinkTables::coroutines names, of each coroutine that a listed call of `referrer` enters.
void forEachFunctionNamedByCoroutinesCalledBy(const LinkTables&, TypeReader, FunctionID referrer, const ScopedLambda<void(FunctionID)>&);
// computeFacts(), behind fillLayoutTablesForFacts(). THE writer of FactTables::objects for ObjectKind::CoroutineResult and for no other kind: ObjectFact::IsPristine where LinkedCoroutine::objectsStayAsMade in linked().
void fillFactsOfObjectsOfCoroutines(const Program&, const LinkTables& linked, FactTables&);
// CalleeSummarizerForLinkEffects::keepsDefaults(). An async function, no generator, no body of a module, whose summary lacks SummaryTrait::Suspends: its call runs the body to its end. False: the defaults stay.
bool callRunsWholeBodyOfCoroutine(const Program&, const ThinSummary&, FunctionID);

// ---- For the scan and for StateOfLinkReach. AOTLinkCoroutinePromises.cpp.

// StateOfLinkReach::bringAlive(). What a body yields and what it returns is the body's, whoever has got hold of the object that a call answered with. So no family of sites has a node for either:
// the node of the summary stands by itself, nothing leads out of it, and an object that escapes or meets another opens nothing there.
constexpr bool slotSpeaksOfBodyOfCoroutine(SlotOfContents slot) { return slot == SlotOfContents::yieldedAlone() || slot == SlotOfContents::returnedAlone(); }
// ScanOfBody::readOperation(), asked of every operation. `import("./cli.js")`: the layout of the one object that the loader resolves the promise with. Link follows the object from there, so the exports escape where it does.
// Null: the operation is no ImportModule, or nobody can tell what the promise gets: attributes are passed, a mock or a copy from disk may answer, the build does not know every name of the namespace, the module exports `then`.
LayoutID layoutOfNamespaceThatImportGives(const Program&, const BodyValue&);
// ScanOfBody::readTerminator(), of a coroutine. What the ramp of an async function and the settlement of its promise do, which no operation of the body shows. Of a generator nothing: its calls keep the defaults.
Effects effectsThatNoOperationOfCoroutineShows(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

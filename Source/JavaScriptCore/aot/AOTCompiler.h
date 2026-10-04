/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGraph.h"
#include "AOTRuntime.h"

namespace JSC {

class CCallHelpers;
class CodeBlock;
class JSScope;

namespace B3 { namespace Air {
class Code;
} }

namespace AOT {

struct CompiledCode;
JS_EXPORT_PRIVATE bool recordKnownFunctionUsesForImage(VM&, UnlinkedCodeBlock*, const CalleeHints*, const ModuleLinkage*, const FunctionSummaryMap&, const FunctionSummary*, VariableSummaries*);
JS_EXPORT_PRIVATE Type inferReturnTypeForImage(VM&, UnlinkedCodeBlock*, const CalleeHints*, const ModuleLinkage*, const FunctionSummary*, VariableSummaries*, unsigned summaryReader, Vector<const KnownFunction*>& calleesRead, Vector<const KnownFunction*>& calleesWithWidenedInputs, uint32_t& escapingParameters, const String& nameForLog = String());
JS_EXPORT_PRIVATE bool compileForImage(VM&, UnlinkedCodeBlock*, CompiledCode&, const CalleeHints* = nullptr, const ModuleLinkage* = nullptr, const FunctionSummary* = nullptr, VariableSummaries* = nullptr, const ProgramCode* = nullptr);
void appendToFacts(std::span<const char>);
bool mayBeAbsorbed(UnlinkedCodeBlock*, const FunctionSummary*);
void recordAllSitesOf(Graph&);

void emitEpilogueBeforeLeaving(CCallHelpers&, const Graph&, B3::Air::Code&);
void emitRestoreBeforeLeaving(CCallHelpers&, const Graph&, B3::Air::Code&);
bool hasNoFrame(const Graph&, B3::Air::Code&);

struct Node;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

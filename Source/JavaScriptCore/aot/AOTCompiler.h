/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

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

// Compiles a function, or the code of a program or a module, for an image. Can run on any thread, as long as nothing else uses the
// VM's heap meanwhile. Returns false if the code cannot be compiled ahead of time.
struct CompiledCode;
// See FunctionSummary. Can run on any thread, under the same condition. Returns false if the code cannot be analyzed.
JS_EXPORT_PRIVATE bool recordUsesOfKnownFunctionsForImage(VM&, UnlinkedCodeBlock*, const CalleeHints*, const ModuleLinkage*, const FunctionSummaryMap&, VariableSummaries*);
// See KnownFunction::returnType. Any thread, likewise.
JS_EXPORT_PRIVATE Type inferReturnTypeForImage(VM&, UnlinkedCodeBlock*, const CalleeHints*, const ModuleLinkage*, const FunctionSummary*, VariableSummaries*, unsigned summaryReader, Vector<const KnownFunction*>& calleesRead, Vector<const KnownFunction*>& calleesWithWidenedInputs, uint32_t& escapingParameters, const String& nameForLog = String());
JS_EXPORT_PRIVATE bool compileForImage(VM&, UnlinkedCodeBlock*, CompiledCode&, const CalleeHints* = nullptr, const ModuleLinkage* = nullptr, const FunctionSummary* = nullptr, VariableSummaries* = nullptr, const CodeOfProgram* = nullptr);
// Whether inlineCalls() could pick it.
bool mayBecomePartOfAnother(UnlinkedCodeBlock*, const FunctionSummary*);
void noteEverySiteOf(Graph&);

// What a function does before it returns, for what leaves it some other way.
void emitEpilogueBeforeLeaving(CCallHelpers&, const Graph&, B3::Air::Code&);
// The same, except that the frame is not released. For Stub::TailCallVarargs.
void emitRestoreBeforeLeaving(CCallHelpers&, const Graph&, B3::Air::Code&);
bool hasNoFrame(const Graph&, B3::Air::Code&); // Only valid once code generation is complete.


struct Node;

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

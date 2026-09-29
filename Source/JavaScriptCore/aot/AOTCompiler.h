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

// Compiles a function, or the code of a program or a module, for an image. Any thread, as long as nothing else is done with the VM's heap
// meanwhile. False if it is not for the static compiler.
struct CompiledCode;
// See ProgramFacts. Any thread, likewise. False: there is no telling what the code does.
JS_EXPORT_PRIVATE bool noteUsesOfProvenFunctionsForImage(VM&, UnlinkedCodeBlock*, const CalleeHints*, const ModuleLinkage*, const FactsOfExecutables&, VariableFacts*);
// See KnownFunction::returnType. Any thread, likewise.
JS_EXPORT_PRIVATE Type inferReturnTypeForImage(VM&, UnlinkedCodeBlock*, const CalleeHints*, const ModuleLinkage*, const ProgramFacts*, VariableFacts*, unsigned readerOfFacts, Vector<const KnownFunction*>& calleesConsulted, Vector<const KnownFunction*>& calleesGivenMore, uint32_t& parametersThatEscape, const String& nameForLog = String());
JS_EXPORT_PRIVATE bool compileForImage(VM&, UnlinkedCodeBlock*, CompiledCode&, const CalleeHints* = nullptr, const ModuleLinkage* = nullptr, const ProgramFacts* = nullptr, VariableFacts* = nullptr, const CodeOfProgram* = nullptr);
// Whether inlineCalls() could pick it.
bool mayBecomePartOfAnother(UnlinkedCodeBlock*, const ProgramFacts*);
void noteEverySiteOf(Graph&);

// What a function does before it returns, for what leaves it some other way.
void emitEpilogueBeforeLeaving(CCallHelpers&, const Graph&, B3::Air::Code&);
// All of that but for letting go of the frame, for Stub::TailCallVarargs.
void emitRestoreBeforeLeaving(CCallHelpers&, const Graph&, B3::Air::Code&);

void reportStatistics();
JS_EXPORT_PRIVATE void setOriginForStatistics(ASCIILiteral); // TEMPORARY-PROVABILITY-STATS

struct Node;
unsigned kindOfSite(Node*, bool isCompact); // TEMPORARY-SITE-COUNTS
JS_EXPORT_PRIVATE void dumpKindsOfSites();

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

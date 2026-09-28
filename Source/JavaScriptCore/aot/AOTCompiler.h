/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTGraph.h"
#include "AOTRuntime.h"

namespace JSC {

class CodeBlock;
class JSScope;

namespace AOT {

// Options::useAOT() without an image: compiles the function now, from its unlinked code and the shape of the scope chain, and
// makes the result the CodeBlock's code. False if the function is not for the static compiler, in which case nothing changed.
RefPtr<JITCode> tryCompile(VM&, ScriptExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSScope*);

// The same compilation, for an image. Any thread, as long as nothing else is done with the VM's heap meanwhile.
struct CompiledCode;
// hasDirectEntry: see CompiledFunctionInfo::directEntryOffset.
JS_EXPORT_PRIVATE bool compileForImage(VM&, UnlinkedCodeBlock*, CompiledCode&, const CalleeHints* = nullptr, const ModuleLinkage* = nullptr, bool hasDirectEntry = false);

ScopeChain scopeChainFor(JSScope*);

void reportStatistics();
JS_EXPORT_PRIVATE void setOriginForStatistics(ASCIILiteral); // TEMPORARY-PROVABILITY-STATS

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

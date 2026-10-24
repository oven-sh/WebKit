/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInterpreterTripwire.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeCounters.h"
#include "CodeBlock.h"
#include "Options.h"
#include "ScriptExecutable.h"
#include "SourceProvider.h"
#include <wtf/Assertions.h>
#include <wtf/DataLog.h>

namespace JSC::AOT {

void willRunInInterpreter(CodeBlock* codeBlock)
{
    VM& vm = codeBlock->vm();
    count(vm, counterFor(codeBlock->codeType()));

    if (!Options::crashIfInterpreterIsUsed()) [[likely]]
        return;
    if (codeBlock->source().provider()->provenance() != SourceProvenance::Static)
        return;
    // The text of an implicit class constructor is the engine's, whichever text declares the class. Only bytecode creates one, and that bytecode came through here.
    if (codeBlock->unlinkedCodeBlock()->isBuiltinDefaultClassConstructor())
        return;
    // Compiled code belongs to the realm of the program. Any other realm has its built-in functions as text, and what creates such a realm is declared to use the interpreter.
    if (vm.hasCompiledCode() && !Instance::of(codeBlock->globalObject()) && codeBlock->unlinkedCodeBlock()->isBuiltinFunction())
        return;

    dataLogLn("crashIfInterpreterIsUsed: ", codeBlock->codeType(), " ", codeBlock->inferredName(), " at ", codeBlock->ownerExecutable()->sourceURL(), ":", codeBlock->ownerExecutable()->firstLine());
    CRASH();
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)

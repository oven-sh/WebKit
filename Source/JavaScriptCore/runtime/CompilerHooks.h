#pragma once

#include "CompilationResult.h"
#include "JITCompilationEffort.h"
#include "JSExportMacros.h"
#include <wtf/Vector.h>

namespace JSC {

class CodeBlock;
class VM;

// Whatever turns code into machine code is got at through these and in no other way. An executable whose code was compiled ahead of time runs with Options::useJIT() off and never does
// that: if it is linked so that nothing calls installCompilers() (InitializeThreading.h), nothing refers to a compiler and the linker leaves them all out. The engine's objects are laid
// out the same either way, since it is the same object files that are linked.
// (A pointer to void: to a function of the type of the one that is named, which whoever calls it knows and this header need not.)
struct CompilerHooks {
    void (*enqueueBaselinePlan)(CodeBlock*) { nullptr };
    CompilationResult (*compileBaselineNow)(VM&, CodeBlock*, JITCompilationEffort) { nullptr };
    void* compileRegExp { nullptr }; // Yarr::jitCompile
    void* newBBQPlan { nullptr }; // Wasm::BBQPlan::create
    Vector<uint8_t> (*compileImage)(void* implOfBytecodeLinkEncoder) { nullptr };

    bool areInstalled() const { return !!enqueueBaselinePlan; }
};

extern JS_EXPORT_PRIVATE CompilerHooks g_compilerHooks;

void installImageCompiler(); // CachedTypes.cpp

} // namespace JSC

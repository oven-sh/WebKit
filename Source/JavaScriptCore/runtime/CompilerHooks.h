#pragma once

#include "CompilationResult.h"
#include "JITCompilationEffort.h"
#include "JSExportMacros.h"
#include <wtf/Vector.h>

namespace JSC {

class CodeBlock;
class VM;

// Everything that generates machine code is reached through these hooks and in no other way. An executable whose code was compiled
// ahead of time runs with Options::useJIT() off and never generates code. If it is linked so that nothing calls installCompilers()
// (InitializeThreading.h), nothing refers to a compiler, and the linker drops all of them. The engine's objects have the same
// layout either way, because the same object files are linked.
// (A void pointer stands for a pointer to a function with the type of the function that it is named after. The caller knows that
// type, and this header does not need to.)
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

#pragma once

#include "CompilationResult.h"
#include "JITCompilationEffort.h"
#include "JSExportMacros.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC {

namespace AOT {
class DiscoveredRegExps;
}

class CodeBlock;
class VM;

struct CompilerHooks {
    void (*enqueueBaselinePlan)(CodeBlock*) { nullptr };
    CompilationResult (*compileBaselineNow)(VM&, CodeBlock*, JITCompilationEffort) { nullptr };
    void* compileRegExp { nullptr };
    void* newBBQPlan { nullptr };
    Vector<uint8_t> (*compileImage)(void* imageCompilation) { nullptr };
    AOT::DiscoveredRegExps* (*newDiscoveredRegExps)(std::span<const uint8_t> typedProgramAsWritten) { nullptr };

    bool areInstalled() const { return !!enqueueBaselinePlan; }
};

extern JS_EXPORT_PRIVATE CompilerHooks g_compilerHooks;

void installImageCompiler();

} // namespace JSC

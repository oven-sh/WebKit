/*
 * Copyright (C) 2015-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <JavaScriptCore/CyclicModuleRecord.h>
#include <JavaScriptCore/ErrorInstance.h>
#include <JavaScriptCore/ParserModes.h>
#include <JavaScriptCore/SourceCode.h>

namespace JSC {

class ModuleGraphInstance;
class ModuleProgramExecutable;
class ModuleRecordInstance;

// Based on the Source Text Module Record
// http://www.ecma-international.org/ecma-262/6.0/#sec-source-text-module-records
class JSMap;
class FunctionExecutable;
class JSPromise;
class InternalFieldTuple;
class CodeBlock;


class JSModuleRecord final : public CyclicModuleRecord {
    friend class LLIntOffsetsExtractor;
public:
    using Base = CyclicModuleRecord;

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.jsModuleRecordSpace<mode>();
    }

    static size_t estimatedSize(JSCell*, VM&);

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    static JSModuleRecord* create(JSGlobalObject*, VM&, Structure*, const Identifier&, const SourceCode&, CodeFeatures);

    JS_EXPORT_PRIVATE JSValue evaluate(JSGlobalObject*, JSValue sentValue, JSValue resumeMode);

    bool isTopLevelExecutionFinished() const;

    void execute(JSGlobalObject*, JSPromise* = nullptr);
    void executeAsync(JSGlobalObject*);

    const SourceCode& sourceCode() const LIFETIME_BOUND { return m_sourceCode; }
    CodeFeatures features() const { return m_features; }

    ModuleProgramExecutable* getOrMakeExecutable(JSGlobalObject*);

    // Module graph instances. Instantiate this module and, recursively, every
    // source text module it depends on, a further time into `instance`, reusing
    // this record's ModuleProgramExecutable, CodeBlock and function executables,
    // then evaluate the instance with the module evaluation algorithm against
    // the instance's state (CyclicModuleRecord::evaluate(..., instance)).
    // The synchronous form requires the evaluation to complete synchronously
    // (throws for top-level await) and returns this module's environment in the
    // instance; the asynchronous form returns the evaluation promise.
    JS_EXPORT_PRIVATE JSModuleEnvironment* instantiateIntoGraphInstance(JSGlobalObject*, ModuleGraphInstance*, ModulePhase = ModulePhase::Evaluation);
    JS_EXPORT_PRIVATE JSPromise* instantiateIntoGraphInstanceAsync(JSGlobalObject*, ModuleGraphInstance*, ModulePhase = ModulePhase::Evaluation);
    // ExecuteModule against this record's environment in `instance`.
    void executeInstance(JSGlobalObject*, ModuleRecordInstance*, JSPromise* capability);
    JSValue evaluateInstance(JSGlobalObject*, ModuleRecordInstance*, JSValue sentValue, JSValue resumeMode);
    ModuleProgramExecutable* retainedExecutable() const { return m_retainedExecutable.get(); }
    void pinRetainedCodeBlock(VM&);
    // Once a record may be instantiated again it must keep its executable
    // (normally dropped after evaluation) and its top-level function executables.
    void retainForGraphInstances(VM&, ModuleProgramExecutable*, Vector<WriteBarrier<FunctionExecutable>>&&);

private:
    JSModuleRecord(VM&, Structure*, const Identifier&, const SourceCode&, CodeFeatures);

    void finishCreation(JSGlobalObject*, VM&);

    JSModuleEnvironment* createInstanceEnvironment(JSGlobalObject*, ModuleGraphInstance*, Vector<JSModuleRecord*>& created);

    SourceCode m_sourceCode;
    WriteBarrier<ModuleProgramExecutable> m_moduleProgramExecutable;
    // Kept for graph instances: the executable past evaluation, and the linked
    // executables of top-level function declarations (index = functionDecl(i)).
    WriteBarrier<ModuleProgramExecutable> m_retainedExecutable;
    // The program CodeBlock whose constants own the nested FunctionExecutables
    // every instance shares; held strongly so old-age jettison cannot re-link it
    // (which would mint fresh executables for later instances).
    WriteBarrier<CodeBlock> m_retainedCodeBlock;
    Vector<WriteBarrier<FunctionExecutable>> m_functionDeclExecutables;
    CodeFeatures m_features;
};

} // namespace JSC

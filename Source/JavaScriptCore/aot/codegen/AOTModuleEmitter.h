/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ModuleEmitter final : public OperationEmitter {
public:
    explicit ModuleEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateModuleNamespace(const ValueContext&);
    void generateImportModule(const ValueContext&);
    void generateRequireModule(const ValueContext&);
    void generateRecordRequire(const ValueContext&);
    void generateAreLoaderFunctionsStored(const ValueContext&);
    void generateWasLoadedSynchronously(const ValueContext&);
    void generateSeesMockOf(const ValueContext&);
    void generateIsLinkedBelow(const ValueContext&);
    void generateMockSeenBy(const ValueContext&);
    void generateNamespaceSeenBy(const ValueContext&);
    void generateInstanceNamespace(const ValueContext&);
    void generateInstanceImportMeta(const ValueContext&);
    void generateOriginalArgument(const ValueContext&);
    void generateLinkedMock(const ValueContext&);
    void generateKeepsOriginalNames(const ValueContext&);
    void generateHasNameInMock(const ValueContext&);
    void generateCountExecution(const ValueContext&);
    void generateResolveModule(const ValueContext&);
    void generateThrowModuleFailure(const ValueContext&);
    void generateImportDynamic(const ValueContext&);
    void generateRequireDynamic(const ValueContext&);
    void generateResolveDynamic(const ValueContext&);
    void generateBeginModuleEvaluation(const ValueContext&);
    void generateFinishModuleEvaluation(const ValueContext&);
    void generateEnterBodyThatRunsOnce(const ValueContext&);
    void generateEvaluateRequiredModule(const ValueContext&);

    static bool asksRunTimeForNamespaceAtEveryRead(const Program&, const Plan&, ModuleID, ImportPhase);
    bool hasSingleNamespaceInValueWord(ModuleID);
    Boxed loadModuleValue(ModuleID);
    Boxed loadRequireCacheValue(ModuleID);
    Condition hasBegun(ModuleID);

private:
    Word32 moduleIndex(ModuleID);
    TypedPointer valueWordOf(ModuleID, int32_t offsetFromValue);
    TypedPointer statusOf(ModuleID);
    void mentionRequireEdge(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

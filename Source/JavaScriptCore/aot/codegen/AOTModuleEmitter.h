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
    void generateCountExecution(const ValueContext&);
    void generateResolveModule(const ValueContext&);
    void generateThrowModuleFailure(const ValueContext&);
    void generateImportDynamic(const ValueContext&);
    void generateRequireDynamic(const ValueContext&);
    void generateResolveDynamic(const ValueContext&);
    void generateBeginModuleEvaluation(const ValueContext&);
    void generateFinishModuleEvaluation(const ValueContext&);
    void generateStopLoadingBehindFile(const ValueContext&);
    void generateEvaluateRequiredModule(const ValueContext&);
    void generateAbandonModuleEvaluation(const ValueContext&);

    bool hasSingleNamespaceInValueWord(ModuleID);
    Boxed loadModuleValue(ModuleID);
    Boxed loadRequireCacheValue(ModuleID);

private:
    Word32 moduleIndex(ModuleID);
    TypedPointer statusOf(ModuleID);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

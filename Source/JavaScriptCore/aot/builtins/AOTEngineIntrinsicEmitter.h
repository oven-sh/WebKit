/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class EngineIntrinsicEmitter final : public BuiltinFamilyEmitter {
public:
    explicit EngineIntrinsicEmitter(CodeGenerator&);

    void generateIsConstructor(const ValueContext&);
    void generateArraySpeciesCreate(const ValueContext&);
    void generateGetByValWithThis(const ValueContext&);
    void generateGetOwnProperty(const ValueContext&);
    void generateToLength(const ValueContext&);
    void generateThrowNewError(const ValueContext&);
    void generateResumeGenerator(const ValueContext&);
    void generateStepGenerator(const ValueContext&);
    void generateIsGeneratorSuspendedAtStart(const ValueContext&);
    void generateIsUnmodifiedGenerator(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

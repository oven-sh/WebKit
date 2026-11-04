/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTCompileFunctions.h"
#include "AOTStackCheckEmitter.h"
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class StraightEmitter;

Machine::ProcedureTraits machineTraitsOf(const BuildContext&, const CodeGenerationInputs&, const Code&);

// One unit, from swept Code to CompiledCode. It lives on the stack of generateCode().
class FunctionCodeGenerator {
    WTF_MAKE_NONCOPYABLE(FunctionCodeGenerator);
public:
    FunctionCodeGenerator(const CodeGenerationWorker&, const CodeGenerationInputs&, const Code&, CodeGenerationTotals&);

    // Empty after a failure.
    CompiledCode run();
    std::optional<CompiledCode> runStraight(const BodyIndex&);

private:
    // Append: the emitters.
    bool appendOperations();
    void appendStraightBody(StraightEmitter&);
    // Order, scan, allocate, slots, frame, encode, link, describe.
    CompiledCode emitCode();
    bool frameFitsImage();
    void reportLinkFailure(LinkFailure);

    const CodeGenerationWorker m_worker;
    const CodeGenerationInputs& m_inputs;
    const Code& m_code;
    CCallHelpers m_jit;
    RelocationRecorder m_relocations;
    CodeRecords m_records;
    StackCheckState m_stackChecks;
    StraightEmitter* m_straightEmitter { nullptr };
    // Its generators point into the members above.
    Machine::Procedure m_procedure;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

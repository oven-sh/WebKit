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
struct QuestionOfEntry;

Machine::ProcedureTraits machineTraitsOf(const BuildContext&, const CodeGenerationInputs&, const Code&);

class FunctionCodeGenerator {
    WTF_MAKE_NONCOPYABLE(FunctionCodeGenerator);
public:
    FunctionCodeGenerator(const CodeGenerationWorker&, const CodeGenerationInputs&, const Code&, CodeGenerationTotals&);

    CompiledCode run();
    std::optional<CompiledCode> runStraight(const BodyIndex&);
    CompiledCode runEntryWrittenByHand(std::span<const QuestionOfEntry>, bool nobodyEnters);

private:
    bool appendOperations();
    void appendStraightBody(StraightEmitter&);
    CompiledCode emitCode();
    bool frameFitsImage();
    COLD NEVER_INLINE void reportLinkFailure(LinkFailure);

    const CodeGenerationWorker m_worker;
    const CodeGenerationInputs& m_inputs;
    const Code& m_code;
    const Statistics::CountersOfThread m_statistics;
    CCallHelpers m_jit;
    RelocationRecorder m_relocations;
    CodeRecords m_records;
    StackCheckState m_stackChecks;
    StraightEmitter* m_straightEmitter { nullptr };
    Machine::Procedure m_procedure;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

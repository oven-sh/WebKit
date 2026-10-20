/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTB3Policy.h"
#include "AOTCodeRecords.h"
#include "AOTCompileFunctions.h"
#include "AOTFrameAddressValidation.h"
#include "AOTNarrowReferenceValidation.h"
#include "AOTRelocationLowering.h"
#include "AOTStackCheckEmitter.h"
#include "B3Procedure.h"
#include <memory>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT { namespace Typed {

FunctionBackEndTraits computeBackEndTraits(const CodeGenerationInputs&, FunctionID, const Function&, const FrameMaps&);
BitVector findColdAirBlocks(const CodeGenerationInputs&, FunctionID, const B3::Air::Code&);

class FunctionCodeGenerator {
    WTF_MAKE_NONCOPYABLE(FunctionCodeGenerator);
    WTF_MAKE_TZONE_ALLOCATED(FunctionCodeGenerator);
public:
    static std::unique_ptr<FunctionCodeGenerator> create(const CodeGenerationWorker&, const CodeGenerationInputs&, FunctionID, CodeGenerationTotals&);

    B3::Procedure& procedure() { return *m_procedure; }
    CompiledCode finish();

private:
    FunctionCodeGenerator(const CodeGenerationWorker&, const CodeGenerationInputs&, FunctionID, CodeGenerationTotals&);

    bool generateB3();
    CompiledCode emitCode();
    bool frameFitsImage();
    void reportLinkFailure(LinkFailure);
    bool reportBackEnd();

    const CodeGenerationWorker m_worker;
    const CodeGenerationInputs& m_inputs;
    const FunctionID m_id;
    const Function& m_function;
    CCallHelpers m_jit;
    RelocationRecorder m_relocations;
    ImageRelocationLowering m_relocationLowering;
    CodeRecords m_records;
    StackCheckState m_stackChecks;
    Vector<FrameAddressViolation> m_frameAddressViolations;
    NarrowReferenceFindings m_narrowReferenceFindings;
    uint32_t m_numberOfLateReferenceGuards { 0 };
    // Last, so that it is destroyed first: its generators and client phases point into the members above.
    std::unique_ptr<B3::Procedure> m_procedure;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

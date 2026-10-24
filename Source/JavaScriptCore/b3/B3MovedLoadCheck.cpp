/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3MovedLoadCheck.h"

#if ENABLE(B3_JIT)

#include "AllowMacroScratchRegisterUsage.h"
#include "B3MemoryValueInlines.h"
#include "B3PatchpointValue.h"
#include "B3ProcedureInlines.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"

namespace JSC { namespace B3 {

std::optional<MovedLoad> MovedLoad::of(const Procedure& proc, const Value* value)
{
    const Policy& policy = proc.policy();
    if (!policy.validation.checksMovedLoadsAtRunTime)
        return std::nullopt;
    const MemoryValue* load = value->as<MemoryValue>();
    if (load && policy.isWrittenBehindTheBackOfCode && policy.isWrittenBehindTheBackOfCode->run(load->range()))
        return std::nullopt;
    return describe(value);
}

std::optional<MovedLoad> MovedLoad::describe(const Value* value)
{
    const MemoryValue* load = value->as<MemoryValue>();
    if (!load || !load->isLoad() || load->isExotic() || load->type().isVector())
        return std::nullopt;
    return MovedLoad { load->opcode(), load->type(), load->offset(), load->origin() };
}

static void emitLoad(CCallHelpers& jit, const MovedLoad& load, CCallHelpers::Address location, GPRReg result)
{
    switch (load.opcode) {
    case Load8Z:
        jit.load8(location, result);
        return;
    case Load8S:
        jit.load8SignedExtendTo32(location, result);
        return;
    case Load16Z:
        jit.load16(location, result);
        return;
    case Load16S:
        jit.load16SignedExtendTo32(location, result);
        return;
    case Load:
        if (sizeofType(load.type) == 8)
            jit.load64(location, result);
        else
            jit.load32(location, result);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

PatchpointValue* createMovedLoadCheck(Procedure& proc, const MovedLoad& load, Value* address, Value* givenToUsers)
{
    return createMovedLoadCheck(proc, load, address, givenToUsers, proc.policy().reportMovedLoadThatDiffers);
}

PatchpointValue* createMovedLoadCheck(Procedure& proc, const MovedLoad& load, Value* address, Value* givenToUsers, RefPtr<DifferenceReport> report)
{
    RELEASE_ASSERT(givenToUsers->type() == load.type);
    PatchpointValue* check = proc.add<PatchpointValue>(Void, load.origin);
    check->append(ConstrainedValue(address, ValueRep::SomeRegister));
    check->append(ConstrainedValue(givenToUsers, ValueRep::SomeRegister));
    check->numGPScratchRegisters = 2;
    check->clobber(RegisterSet::macroClobberedGPRs());
    check->effects = Effects::none();
    check->effects.writesLocalState = true;
    check->setGenerator([load, report = WTF::move(report)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg found = params.gpScratch(0);
        GPRReg given = params.gpScratch(1);
        bool is64Bit = sizeofType(load.type) == 8;
        emitLoad(jit, load, CCallHelpers::Address(params[0].gpr(), load.offset), found);
        if (params[1].isGPR())
            given = params[1].gpr();
        else if (is64Bit)
            jit.moveDoubleTo64(params[1].fpr(), given);
        else
            jit.moveFloatTo32(params[1].fpr(), given);
        CCallHelpers::Jump differs = is64Bit ? jit.branch64(CCallHelpers::NotEqual, found, given) : jit.branch32(CCallHelpers::NotEqual, found, given);
        params.addLatePath([differs, report, origin = load.origin](CCallHelpers& jit) {
            differs.link(&jit);
            if (report)
                report->run(jit, origin);
            jit.breakpoint();
        });
    });
    return check;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

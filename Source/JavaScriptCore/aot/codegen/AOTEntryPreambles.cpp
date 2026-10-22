/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEntryEmitter.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTCodeRecords.h"
#include "AOTConvention.h"
#include "AOTStubsCallFrames.h"
#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Typed {

static unsigned numberOfGPRParameters(const ArgumentLocations& locations)
{
    unsigned count = 0;
    for (const ArgumentLocation& location : locations.parameters) {
        if (location.kind == ArgumentLocation::Kind::GPR)
            ++count;
    }
    return count;
}

static void emitSecondaryEntries(CCallHelpers& jit, std::span<const SecondaryEntry> entries, unsigned numberOfGPRs, CodeRecords& records)
{
    RELEASE_ASSERT(numberOfGPRs <= numberOfDirectArgumentGPRs);
    for (size_t index = 0; index < entries.size(); ++index) {
        const SecondaryEntry& entry = entries[index];
        unsigned endOfOmittedGPRs = index + 1 < entries.size() ? entries[index + 1].firstOmittedGPR : numberOfGPRs;
        RELEASE_ASSERT(entry.id != primaryEntry && entry.firstOmittedGPR < endOfOmittedGPRs);

        CCallHelpers::Label begin = jit.label();
        records.entries.append(RecordedEntry { entry.id, begin });
        RegisterSet written;
        for (unsigned gpr = entry.firstOmittedGPR; gpr < endOfOmittedGPRs; ++gpr) {
            jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), directArgumentGPR(gpr));
            written.add(directArgumentGPR(gpr));
        }
        records.units.addPlain(begin, jit.label(), written);
    }
}

static void emitArgumentRegisterSpill(CCallHelpers& jit, CodeRecords& records)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    CCallHelpers::Label begin = jit.label();
    records.entries.append(RecordedEntry { primaryEntry, begin });
    emitSpillArgumentRegisters(jit);
    RegisterSet written = RegisterSet::macroClobberedGPRs();
    written.add(CCallHelpers::stackPointerRegister);
    records.units.addPlain(begin, jit.label(), written);
    records.frame.stackUsedAtCalls += sizeOfSpilledArgumentRegisters;
    records.frame.spillsArgumentRegisters = true;
}

void emitEntryPreambles(CCallHelpers& jit, const CodeGenerationInputs& inputs, FunctionID id, CodeRecords& records)
{
    const CallingConventions& conventions = inputs.conventions;
    std::span<const SecondaryEntry> secondaryEntries = conventions.secondaryEntriesOf(id);
    bool spillsArgumentRegisters = conventions.of(inputs.program, id).spillsArgumentRegisters;
    RELEASE_ASSERT(secondaryEntries.empty() || !spillsArgumentRegisters);

    if (!secondaryEntries.empty())
        emitSecondaryEntries(jit, secondaryEntries, numberOfGPRParameters(conventions.locationsOf(inputs.program, id)), records);
    if (spillsArgumentRegisters)
        emitArgumentRegisterSpill(jit, records);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

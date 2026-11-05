/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTCompiledCode.h"
#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

void countEntryIntoCode(Machine::PatchOperation*);

class EntryEmitter final : public OperationEmitter {
public:
    explicit EntryEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateEntries();

    Word32 argumentCount() const;
    Pointer argumentList() const;
    uint32_t indexOfFirstArgumentInList() const;
    uint32_t numberOfArgumentsAlwaysInList() const;

private:
    void generateEntry(uint16_t entry, const ArgumentLocations&);
    Operand read(const ArgumentLocation&, EntryConvention);

    Word32 m_argumentCount;
    Pointer m_argumentList;
    uint32_t m_indexOfFirstArgumentInList { 0 };
};

// All of it falls through to the primary entry, but in a parameter guard, which begins with its way to the slow version: the way to the fast one has to come last.
[[nodiscard]] CCallHelpers::Jump emitEntryPreambles(CCallHelpers&, const CodeGenerationInputs&, CodeUnit, CodeRecords&);
EntryDescription describeEntry(const CodeGenerationInputs&, CodeUnit);
bool readsCallerFrame(const CodeGenerationInputs&, const Code&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

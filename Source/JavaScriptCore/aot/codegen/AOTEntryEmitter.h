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

class EntryEmitter final : public OperationEmitter {
public:
    explicit EntryEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    // For entry 0 of Code: a Machine entrypoint with the id of the entry, which reads the parameters of the entry's block where CallingConventions::locationsOf(unit) passes them and jumps to that block.
    // A resumption of a coroutine does not enter it: the stub that resumes copies the saved frame back to the stack and jumps to SavedFrame::resumeAddress(),
    // behind the call that suspended, whose results are the sent value and the mode (stubs/AOTStubsCoroutineResumption.cpp).
    void generateEntries();

    Word32 argumentCount() const;
    Pointer argumentList() const;
    uint32_t indexOfFirstArgumentInList() const;
    // A caller that passes fewer puts undefined in their registers, which the entry spills into the list.
    uint32_t numberOfArgumentsAlwaysInList() const;

private:
    void generateEntry(uint16_t entry, const ArgumentLocations&);
    Operand read(const ArgumentLocation&, EntryConvention);

    Word32 m_argumentCount;
    Pointer m_argumentList;
    uint32_t m_indexOfFirstArgumentInList { 0 };
};

// In front of the code of the procedure. A unit that is entered with its arguments in a buffer (CallingConventions::spillsArgumentRegisters()) stores the argument registers above its frame first:
// the label of that code is entry 0 in place of the procedure's own.
// All of it falls through to the primary entry, but in a parameter guard, which begins with its way to the slow version: the way to the fast one has to come last.
// There the caller links what is returned to the primary entry. Where nothing is returned, it records the primary entry as the fused call entry.
[[nodiscard]] CCallHelpers::Jump emitEntryPreambles(CCallHelpers&, const CodeGenerationInputs&, CodeUnit, CodeRecords&);
EntryDescription describeEntry(const CodeGenerationInputs&, CodeUnit);
bool readsCallerFrame(const CodeGenerationInputs&, const Code&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

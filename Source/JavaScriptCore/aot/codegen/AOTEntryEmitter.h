/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

struct ArgumentLocation;

class EntryEmitter final : public OperationEmitter {
public:
    explicit EntryEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateEntry();

    Word32 argumentCount() const;
    Pointer argumentList() const;
    uint32_t indexOfFirstArgumentInList() const;

private:
    Operand read(const ArgumentLocation&);

    Word32 m_argumentCount;
    Pointer m_argumentList;
    uint32_t m_indexOfFirstArgumentInList { 0 };
};

void emitEntryPreambles(CCallHelpers&, const CodeGenerationInputs&, FunctionID, CodeRecords&);
EntryDescription describeEntry(const CodeGenerationInputs&, FunctionID, const Function&);
bool readsCallerFrame(const CodeGenerationInputs&, FunctionID, const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

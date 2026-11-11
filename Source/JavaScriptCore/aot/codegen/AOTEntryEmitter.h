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
struct QuestionOfEntry {
    GPRReg value { InvalidGPRReg };
    uint16_t passingPrimitives { 0 };
    bool everyObjectPasses { false };
    bool shapeMayBearMark { false };
    std::optional<uint32_t> shapeWord;
    Vector<JSTypeRange, 2> cellTypes;
};
std::optional<Vector<QuestionOfEntry, 4>> findQuestionsOfEntryWrittenByHand(const CodeGenerationInputs&, TypeArena&, CodeUnit, NOESCAPE const ScopedLambda<TypeID(unsigned)>& typeAskedOfEntryParameter);
void emitEntryWrittenByHand(CCallHelpers&, const CodeGenerationInputs&, CodeRecords&, CodeUnit, std::span<const QuestionOfEntry>, bool frameIsSetUp, bool nobodyEnters);
CCallHelpers::JumpList emitQuestionOfEntryWrittenByHand(CCallHelpers&, const QuestionOfEntry&);
EntryDescription describeEntry(const CodeGenerationInputs&, CodeUnit);
bool readsCallerFrame(const CodeGenerationInputs&, const Code&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

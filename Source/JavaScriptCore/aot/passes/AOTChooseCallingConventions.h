/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <span>
#include <wtf/Forward.h>

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;

struct FunctionConvention {
    friend bool operator==(const FunctionConvention&, const FunctionConvention&) = default;

    bool resultIsBoxed { false };
    bool spillsArgumentRegisters { false };
    uint16_t reservedStackWords { 0 };
};

struct SecondaryEntry {
    friend bool operator==(const SecondaryEntry&, const SecondaryEntry&) = default;

    uint16_t id { 0 };
    uint8_t firstOmittedGPR { 0 };
};

struct CallingConventionTables {
    ConventionRegisters registers;
    Vector<FunctionConvention> conventions;
    Vector<uint32_t> firstSecondaryEntry;
    Vector<SecondaryEntry> secondaryEntries;
};

class CallingConventions {
public:
    CallingConventions();
    explicit CallingConventions(CallingConventionTables&&);

    ConventionRegisters registers() const { return m_tables.registers; }

    FunctionConvention of(const Program&, FunctionID) const;
    Representation parameterRepresentation(const Program&, FunctionID, unsigned entryParameterIndex) const;
    Representation resultRepresentation(const Program&, FunctionID, unsigned resultIndex = 0) const;
    ArgumentLocations locationsOf(const Program&, FunctionID) const;
    uint32_t stackWordsReservedByCallersOf(const Program&, FunctionID callee) const;
    uint32_t stackWordsReservedByCallersOf(const Program&, FunctionID callee, const ArgumentLocations& calleeLocations) const;

    std::span<const SecondaryEntry> secondaryEntriesOf(FunctionID) const LIFETIME_BOUND;
    SecondaryEntry findEntryOmittingArguments(FunctionID callee, unsigned firstUndefinedGPR, unsigned numberOfGPRArguments) const;

    void dump(PrintStream&, const Program&) const;

private:
    CallingConventionTables m_tables;
};

CallingConventions chooseCallingConventions(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

Representation declaredRepresentationOf(const Program&, const DeclaredType&);
ArgumentLocations computeArgumentLocations(const Program&, const Function& callee, FunctionConvention, ConventionRegisters);
Representation parameterRepresentationOf(const Program&, const Function& callee, unsigned entryParameterIndex);
Representation resultRepresentationOf(const Program&, const Function& callee, FunctionConvention, unsigned resultIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

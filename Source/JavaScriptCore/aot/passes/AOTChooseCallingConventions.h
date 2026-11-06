/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/Forward.h>

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;

struct FunctionConvention {
    friend bool operator==(const FunctionConvention&, const FunctionConvention&) = default;

    bool resultIsBoxed : 1 { false };
    bool spillsArgumentRegisters : 1 { false };
    bool resultIsImpure : 1 { false };
    bool hasImpureParameters : 1 { false };
    uint16_t reservedStackWords { 0 };
};

struct ImpureParameter {
    friend auto operator<=>(const ImpureParameter&, const ImpureParameter&) = default;

    FunctionID function;
    uint32_t entryParameterIndex { 0 };
};

struct SecondaryEntry {
    friend bool operator==(const SecondaryEntry&, const SecondaryEntry&) = default;

    uint16_t id { 0 };
    uint8_t firstOmittedGPR { 0 };
};

// `function f(x) { .. x = null; .. }`: the function lets go of what it was handed while it still runs.
struct ParameterLetGo {
    friend auto operator<=>(const ParameterLetGo&, const ParameterLetGo&) = default;

    FunctionID function;
    uint32_t entryParameterIndex { 0 };
};

struct CallingConventionTables {
    ConventionRegisters registers;
    Vector<FunctionConvention> conventions;
    Vector<uint32_t> firstSecondaryEntry;
    Vector<SecondaryEntry> secondaryEntries;
    Vector<ImpureParameter> impureParameters;
    // In order.
    Vector<ParameterLetGo> parametersLetGo;
};

class CallingConventions {
public:
    CallingConventions();
    explicit CallingConventions(CallingConventionTables&&);

    FunctionConvention of(const Program&, FunctionID) const;
    Representation parameterRepresentation(const Program&, FunctionID, unsigned entryParameterIndex) const;
    Representation resultRepresentation(const Program&, FunctionID, unsigned resultIndex = 0) const;
    ArgumentLocations locationsOf(const Program&, FunctionID) const;
    uint32_t stackWordsReservedByCallersOf(const Program&, FunctionID callee) const;
    uint32_t stackWordsReservedByCallersOf(const Program&, FunctionID callee, const ArgumentLocations& calleeLocations) const;

    std::span<const SecondaryEntry> secondaryEntriesOf(FunctionID) const LIFETIME_BOUND;
    SecondaryEntry findEntryOmittingArguments(FunctionID callee, unsigned firstUndefinedGPR, unsigned numberOfGPRArguments) const;

    // The store in the callee reaches no word of the caller's frame and no register that the caller leaves as it is. A caller that reads the argument no more keeps no copy of it across the call.
    bool letsGoOfParameter(FunctionID callee, unsigned entryParameterIndex) const;

    void dump(PrintStream&, const Program&) const;

private:
    CallingConventionTables m_tables;
};

CallingConventions chooseCallingConventions(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

Representation declaredRepresentationOf(const Program&, const DeclaredType&);

// Which parameter of the function a value is, behind casts and behind the tests that have narrowed it.
std::optional<uint32_t> entryParameterIndexOf(const Function&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

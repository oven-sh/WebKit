/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTBitVector.h"
#include "AOTCodeRole.h"
#include "AOTID.h"
#include "AOTOpcode.h"
#include "AOTSchemaEnums.h"
#include "AOTStackTestPlan.h"
#include "AOTTailCallPlan.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class Facts;
class Program;
struct PromotedEnvironments;

enum class StackCheck : uint8_t { Required, CoveredByCallers };

struct FunctionConvention {
    bool has(CodeRole role) const { return roles >> static_cast<unsigned>(role) & 1; }

    uint8_t roles { 0 };
    bool takesArgumentsInBuffer : 1 { false };
    bool callEntryTakesNewTarget : 1 { false };
    bool coroutineNeverSuspends : 1 { false };
    bool generatorNeverYields : 1 { false };
    bool callEntryWalksBody : 1 { false };
    bool returnsCapture : 1 { false };
    uint8_t borrowedArguments : 2 { 0 };
    uint16_t numberOfEntryParameters { 0 };
    uint16_t numberOfResults { 1 };
    uint32_t firstResult { 0 };
    uint32_t firstEntryParameter { 0 };
    uint32_t firstTypeBehindGuard { 0 };
};

struct ParameterGuardPlan {
    FunctionID function;
    Vector<TypeID, 8> typesBehindGuard;
};

struct GenericEntryPlan {
    bool takesNewTarget { false };
    bool allocatesThis { false };
    uint8_t borrowedArguments { 0 };
    CodeUnit target;
    CodeUnit constructEntry;
};

inline constexpr uint16_t fusedCallEntry = UINT16_MAX;

struct CallEntryMove {
    enum class Source : uint8_t { This, Closure, Argument, Capture, Undefined };

    Source source { Source::Undefined };
    uint32_t index { 0 };
    ArgumentLocation destination;
};

struct MethodTableRow {
    LayoutID layout;
    FunctionID method;
    LayoutID guardedClass;
};

struct MethodFamily {
    bool entersLookup() const { return hasGuardedRow; }

    PropertyKeyID key;
    Vector<MethodTableRow> rows;
    Vector<FunctionID> methods;
    Vector<TypeID, 4> typesOfArguments;
    bool hasGuardedRow { false };
};

struct MethodSelector {
    uint32_t family { 0 };
    uint32_t numberOfArguments { 0 };
    uint32_t firstUnit { 0 };
    bool isTyped { false };
    Representation result { Representation::JSValue };
    TypeID typeOfResult;
    BitVector methodsEnteredDirectly;
    bool everyMethodIsEnteredDirectly { false };
};

struct MethodTablePlan {
    Vector<MethodFamily> families;
    Vector<MethodSelector> selectors;
    uint32_t numberOfUnits { 0 };
};

struct CallingConventionParts {
    Vector<FunctionConvention> conventions;
    Vector<Representation> formsOfResults;
    Vector<Representation> formsOfEntryParameters;
    Vector<TypeID> typesBehindGuards;
    MethodTablePlan methodTable;
};

struct ScalarizedResult {
    FunctionID function;
    LayoutID layout;
    Vector<uint32_t, 8> fields;
    Vector<Representation, 8> forms;
};
using ScalarizedResults = Vector<ScalarizedResult>;

struct PlaceOfScalar {
    uint32_t field { 0 };
    LayoutID held;
    uint32_t fieldOfHeld { 0 };
};

struct ScalarizedParameter {
    FunctionID function;
    uint32_t entryParameter { 0 };
    uint32_t firstScalar { 0 };
    LayoutID layout;
    Vector<uint32_t, 8> fields;
    Vector<PlaceOfScalar, 8> places;
    Vector<Representation, 8> forms;
};
using ScalarizedParameters = Vector<ScalarizedParameter>;

static constexpr unsigned maxArgumentsOfTableCall = 4;

class CallingConventions {
    friend class FactMisstater;
public:
    CallingConventions() = default;
    JS_EXPORT_PRIVATE explicit CallingConventions(CallingConventionParts&&);

    const FunctionConvention& of(FunctionID id) const LIFETIME_BOUND { return id.index() < m_parts.conventions.size() ? m_parts.conventions[id.index()] : m_conventionOfNoFunction; }
    uint32_t numberOfFunctions() const { return static_cast<uint32_t>(m_parts.conventions.size()); }

    bool callEntryWalksBody(FunctionID id) const { return of(id).callEntryWalksBody; }
    bool isSeedOfFunctionInQueueFromStart(CodeUnit unit) const
    {
        if (unit.role == CodeRole::GenericCallEntry)
            return callEntryWalksBody(unit.function);
        return !namesOnlyUnitsOfItsFunction(unit.role) && (unit.role != CodeRole::SlowBody || of(unit.function).has(CodeRole::ParameterGuard));
    }
    bool exists(CodeUnit unit) const
    {
        if (isRoleOfOneFunction(unit.role))
            return !unit.numberOfBoundArguments && of(unit.function).has(unit.role);
        return existsBehindUnitsOfFunctions(unit);
    }
    bool isCoroutine(FunctionID id) const { return of(id).has(CodeRole::CoroutineBody); }
    CodeUnit directEntryOf(FunctionID id) const { return { id, hasParameterGuard(id) ? CodeRole::ParameterGuard : CodeRole::Body }; }
    CodeUnit entryBehindGuardOf(FunctionID id) const { return { id, CodeRole::Body }; }
    bool hasParameterGuard(FunctionID id) const { return of(id).has(CodeRole::ParameterGuard); }
    TypeID typeBehindGuard(FunctionID id, unsigned entryParameter) const
    {
        const FunctionConvention& convention = of(id);
        if (!convention.has(CodeRole::ParameterGuard) || entryParameter >= convention.numberOfEntryParameters)
            return { };
        return m_parts.typesBehindGuards[convention.firstTypeBehindGuard + entryParameter];
    }
    JS_EXPORT_PRIVATE void setParameterGuards(std::span<const ParameterGuardPlan>);
    JS_EXPORT_PRIVATE void setScalarizedResults(ScalarizedResults&&);
    JS_EXPORT_PRIVATE void setScalarizedParameters(const Program&, ScalarizedParameters&&);
    std::span<const ScalarizedParameter> scalarizedParametersOf(FunctionID id) const LIFETIME_BOUND
    {
        if (!m_functionsWithScalarizedParameters.get(id.index())) [[likely]]
            return { };
        auto found = std::ranges::equal_range(m_scalarizedParameters, id, { }, &ScalarizedParameter::function);
        return { found.begin(), found.end() };
    }
    bool isScalarizedParameter(FunctionID id, uint32_t entryParameter) const
    {
        return std::ranges::contains(scalarizedParametersOf(id), entryParameter, &ScalarizedParameter::entryParameter);
    }
    bool takesScalars(CodeUnit unit) const { return m_functionsWithScalarizedParameters.get(unit.function.index()) && unit.role == CodeRole::Body; }
    uint32_t numberOfScalarsTaken(CodeUnit unit) const
    {
        if (!takesScalars(unit)) [[likely]]
            return 0;
        uint32_t number = 0;
        for (const ScalarizedParameter& parameter : scalarizedParametersOf(unit.function))
            number += parameter.forms.size();
        return number;
    }
    const ScalarizedResult* findScalarizedResult(FunctionID id) const LIFETIME_BOUND
    {
        if (!m_functionsWithScalarizedResult.get(id.index())) [[likely]]
            return nullptr;
        return &*std::ranges::lower_bound(m_scalarizedResults, id, { }, &ScalarizedResult::function);
    }
    bool returnsScalars(CodeUnit unit) const { return m_functionsWithScalarizedResult.get(unit.function.index()) && returnsAsDeclared(unit.role, of(unit.function)); }
    JS_EXPORT_PRIVATE void setModuleStarts(const Program&, const BitVector& bodiesWithModuleStart);
    JS_EXPORT_PRIVATE void setRemovedCaptures(const Program&, const PromotedEnvironments&);
    CodeUnit closureEntryOf(FunctionID id) const { return { id, CodeRole::GenericCallEntry }; }
    template<typename Call>
    CodeUnit unitEnteredBy(const Call& call) const { return { call.function(), call.role(), static_cast<uint16_t>(call.numberOfBoundArguments()) }; }

    bool hasFusedCallEntry(FunctionID id) const { return m_functionsWithFusedCallEntry.get(id.index()); }
    JS_EXPORT_PRIVATE void fuseCallEntriesThatOnlyMove(const Program&, const Facts&, const ParallelLoops&);
    bool holdsFusedCallEntry(CodeUnit unit) const { return hasFusedCallEntry(unit.function) && unit == genericEntry(closureEntryOf(unit.function)).target; }
    bool callEntryIsNumberOfModuleStart(FunctionID id) const { return hasFusedCallEntry(id) && of(id).has(CodeRole::ModuleStart); }
    bool haveNeighboringClosureNumbers(FunctionID lower, FunctionID higher) const
    {
        ASSERT(lower < higher && higher.index() < m_parts.conventions.size());
        return m_functionsWithGenericEntry.findBit(lower.index() + 1, true) >= higher.index();
    }

    uint32_t numberOfUnits() const { return static_cast<uint32_t>(m_parts.conventions.size() * numberOfCodeRolesOfOneFunction) + m_parts.methodTable.numberOfUnits; }
    uint32_t indexOf(CodeUnit unit) const
    {
        if (!isRoleOfOneFunction(unit.role))
            return indexBehindUnitsOfFunctions(unit);
        RELEASE_ASSERT(unit.function.index() < numberOfFunctions() && !unit.numberOfBoundArguments);
        return unit.function.index() * numberOfCodeRolesOfOneFunction + static_cast<unsigned>(unit.role);
    }
    JS_EXPORT_PRIVATE CodeUnit unitAt(uint32_t index) const;

    EntryConvention entryConventionOf(CodeUnit unit) const
    {
        if (!isGenericEntry(unit.role))
            return EntryConvention::Direct;
        return of(unit.function).takesArgumentsInBuffer ? EntryConvention::GenericBuffer : EntryConvention::GenericRegisters;
    }
    bool spillsArgumentRegisters(CodeUnit unit) const { return entryConventionOf(unit) == EntryConvention::GenericBuffer; }

    ALWAYS_INLINE Representation parameterRepresentation(const Program& program, CodeUnit unit, unsigned parameterIndex) const
    {
        if (takesScalars(unit) && parameterIndex >= of(unit.function).numberOfEntryParameters) [[unlikely]]
            return formOfScalar(unit, parameterIndex);
        if (takesEntryParametersAsListed(unit.role) && !m_parts.formsOfEntryParameters.isEmpty())
            return formOfEntryParameterAsListed(program, m_parts.conventions[unit.function.index()], unit.role, parameterIndex);
        return parameterRepresentationByRole(program, unit, parameterIndex);
    }
    unsigned numberOfResults(CodeUnit unit) const
    {
        const FunctionConvention& convention = of(unit.function);
        return returnsAsDeclared(unit.role, convention) ? convention.numberOfResults : 1;
    }
    ALWAYS_INLINE Representation resultRepresentation(CodeUnit unit, unsigned resultIndex = 0) const
    {
        const FunctionConvention& convention = of(unit.function);
        if (returnsAsDeclared(unit.role, convention)) {
            RELEASE_ASSERT(resultIndex < convention.numberOfResults);
            return m_parts.formsOfResults[convention.firstResult + resultIndex];
        }
        RELEASE_ASSERT(!resultIndex);
        return isTableRole(unit.role) ? methodSelector(unit.placeOfSelector()).result : Representation::JSValue;
    }
    JS_EXPORT_PRIVATE ArgumentLocations locationsOf(const Program&, CodeUnit) const;
    JS_EXPORT_PRIVATE uint32_t stackWordsReservedByCallersOf(const Program&, CodeUnit callee) const;

    bool savesRegistersOfCallers(CodeUnit unit) const { return m_tailCalls.savesRegistersOfCallers(unit); }

    StackCheck stackCheckOf(CodeUnit unit) const { return m_stackTests.isCoveredByCallers(isBodyOfCoroutine(unit.role, isCoroutine(unit.function)) ? CodeUnit(unit.function, CodeRole::CoroutineBody) : unit) ? StackCheck::CoveredByCallers : StackCheck::Required; }
    bool testsStackInFrontOfTailCalls(CodeUnit unit) const { return unit.role != CodeRole::TableEntry && stackCheckOf(unit) == StackCheck::Required; }

    JS_EXPORT_PRIVATE GenericEntryPlan genericEntry(CodeUnit) const;

    const StackTestPlan& stackTests() const LIFETIME_BOUND { return m_stackTests; }
    void setTailCalls(TailCallPlan&& tailCalls) { m_tailCalls = WTF::move(tailCalls); }
    void setStackTests(StackTestPlan&& stackTests) { m_stackTests = WTF::move(stackTests); }

    void setMethodTable(MethodTablePlan&& methodTable) { m_parts.methodTable = WTF::move(methodTable); }
    std::span<const MethodFamily> methodFamilies() const LIFETIME_BOUND { return m_parts.methodTable.families.span(); }
    std::span<const MethodSelector> methodSelectors() const LIFETIME_BOUND { return m_parts.methodTable.selectors.span(); }
    const MethodSelector& methodSelector(uint32_t selector) const LIFETIME_BOUND { return m_parts.methodTable.selectors[selector]; }
    const MethodFamily& familyOf(const MethodSelector& selector) const LIFETIME_BOUND { return m_parts.methodTable.families[selector.family]; }
    JS_EXPORT_PRIVATE std::optional<uint32_t> findMethodSelector(PropertyKeyID, unsigned numberOfArguments) const;
    CodeUnit tableLookupOf(const MethodSelector& selector) const { return CodeUnit::ofMethodTable(familyOf(selector).methods[0], CodeRole::TableLookup, placeOf(selector)); }
    CodeUnit unitEnteredThroughTable(const Program&, const MethodSelector& selector, FunctionID method) const
    {
        const Vector<FunctionID>& methods = familyOf(selector).methods;
        size_t place = std::ranges::lower_bound(methods, method) - methods.begin();
        return selector.methodsEnteredDirectly.get(place) ? directEntryOf(method) : CodeUnit::ofMethodTable(method, CodeRole::TableEntry, placeOf(selector));
    }
    const MethodTableRow* findRowOfMethodTable(uint32_t selector, LayoutID receiver) const LIFETIME_BOUND
    {
        const Vector<MethodTableRow>& rows = familyOf(methodSelector(selector)).rows;
        auto row = std::ranges::lower_bound(rows, receiver, { }, &MethodTableRow::layout);
        return row != rows.end() && row->layout == receiver ? &*row : nullptr;
    }

private:
    static bool returnsAsDeclared(CodeRole role, const FunctionConvention& convention) { return (returnsResultOfFunction(role) || role == CodeRole::ModuleStart) && !convention.has(CodeRole::CoroutineBody); }
    static constexpr bool isRoleOfOneFunction(CodeRole role) { return static_cast<unsigned>(role) < numberOfCodeRolesOfOneFunction; }
    static constexpr bool takesEntryParametersAsListed(CodeRole role) { return role == CodeRole::Body || role == CodeRole::ModuleStart || role == CodeRole::ParameterGuard; }
    Representation formOfEntryParameterAsListed(const Program& program, const FunctionConvention& convention, CodeRole role, unsigned parameterIndex) const
    {
        RELEASE_ASSERT(parameterIndex < convention.numberOfEntryParameters);
        Representation form = m_parts.formsOfEntryParameters[convention.firstEntryParameter + parameterIndex];
        if (role != CodeRole::Body || !convention.has(CodeRole::ParameterGuard))
            return form;
        if (form != Representation::JSValue && form != Representation::Double && form != Representation::Int64 && form != Representation::OptionalDouble)
            return form;
        if (form != Representation::JSValue && !m_parts.typesBehindGuards[convention.firstTypeBehindGuard + parameterIndex])
            return form;
        return formBehindGuard(program, convention, parameterIndex);
    }
    JS_EXPORT_PRIVATE Representation formOfScalar(CodeUnit, unsigned parameterIndex) const;
    JS_EXPORT_PRIVATE Representation formBehindGuard(const Program&, const FunctionConvention&, unsigned parameterIndex) const;
    JS_EXPORT_PRIVATE Representation parameterRepresentationByRole(const Program&, CodeUnit, unsigned parameterIndex) const;
    JS_EXPORT_PRIVATE bool existsBehindUnitsOfFunctions(CodeUnit) const;
    JS_EXPORT_PRIVATE uint32_t indexBehindUnitsOfFunctions(CodeUnit) const;

    uint32_t placeOf(const MethodSelector& selector) const
    {
        size_t place = static_cast<size_t>(&selector - m_parts.methodTable.selectors.begin());
        RELEASE_ASSERT(place < m_parts.methodTable.selectors.size() && place < 1u << 24);
        return static_cast<uint32_t>(place);
    }

    CallingConventionParts m_parts;
    FunctionConvention m_conventionOfNoFunction;
    BitVector m_functionsWithGenericEntry;
    BitVector m_functionsWithFusedCallEntry;
    ScalarizedResults m_scalarizedResults;
    BitVector m_functionsWithScalarizedResult;
    ScalarizedParameters m_scalarizedParameters;
    BitVector m_functionsWithScalarizedParameters;
    TailCallPlan m_tailCalls;
    StackTestPlan m_stackTests;
};

JS_EXPORT_PRIVATE std::optional<Vector<CallEntryMove, 8>> planCallEntryMoves(const Program&, const CallingConventions&, FunctionID);

JS_EXPORT_PRIVATE CallingConventions computeCallingConventions(const Program&, const Facts&, const BitVector& layoutsMadeAsCompiledObjects, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallingConventions.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTCoroutinePlan.h"
#include "AOTFacts.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTOpcodeSets.h"
#include "AOTFieldGetters.h"
#include "AOTPromotedEnvironments.h"
#include "AOTRepresentation.h"
#include "AOTThreads.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfCodeRolesOfOneFunction <= sizeof(FunctionConvention::roles) * 8);

static_assert([] {
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        for (unsigned earlier = 0; earlier < index; ++earlier) {
            if (directArgumentGPR(earlier) == directArgumentGPR(index))
                return false;
        }
    }
    for (unsigned index = 0; index < numberOfDirectArgumentFPRs; ++index) {
        for (unsigned earlier = 0; earlier < index; ++earlier) {
            if (directArgumentFPR(earlier) == directArgumentFPR(index))
                return false;
        }
    }
    return true;
}());

static_assert([] {
    auto isScratch = [](GPRReg reg) {
#if CPU(X86_64)
        return reg == MacroAssembler::s_scratchRegister;
#elif CPU(ARM64)
        return reg == MacroAssembler::dataTempRegister || reg == MacroAssembler::memoryTempRegister;
#endif
    };
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        if (isScratch(directArgumentGPR(index)))
            return false;
    }
    return !isScratch(GPRInfo::returnValueGPR);
}());
static_assert(noOverlap(GPRInfo::returnValueGPR, instanceGPR, cellCageBaseGPR));

static constexpr unsigned numberOfParametersInFrontOfGenericArguments = 2;

static constexpr unsigned numberOfValuesThatNeverNeedStackWords = std::min(numberOfDirectArgumentGPRs, numberOfDirectArgumentFPRs);

static constexpr uint8_t bitOfRoleInConvention(CodeRole role)
{
    return static_cast<uint8_t>(1u << static_cast<unsigned>(role));
}

static constexpr uint8_t genericEntriesAmongRolesOfConvention = [] {
    uint8_t roles = 0;
    for (unsigned role = 0; role < numberOfCodeRolesOfOneFunction; ++role) {
        if (isGenericEntry(static_cast<CodeRole>(role)))
            roles |= bitOfRoleInConvention(static_cast<CodeRole>(role));
    }
    return roles;
}();

namespace {

struct LocationAllocatorOfConvention {
    ArgumentLocation allocate(Representation representation)
    {
        if (representation == Representation::Void)
            return { .kind = ArgumentLocation::Kind::None, .representation = representation };
        if (isPassedInFPR(representation)) {
            if (nextFPR < numberOfDirectArgumentFPRs) {
                nextGPR += rawValuesKeepTheirPlaces;
                return { .kind = ArgumentLocation::Kind::FPR, .reg = directArgumentFPR(nextFPR++), .representation = representation };
            }
        } else if (nextGPR < numberOfDirectArgumentGPRs)
            return { .kind = ArgumentLocation::Kind::GPR, .reg = directArgumentGPR(nextGPR++), .representation = representation };
        return { .kind = ArgumentLocation::Kind::Stack, .stackIndex = numberOfStackWords++, .representation = representation };
    }

    bool rawValuesKeepTheirPlaces { false };
    unsigned nextGPR { 0 };
    unsigned nextFPR { 0 };
    uint32_t numberOfStackWords { 0 };
};

enum class ConstructionThroughConvention : uint8_t { None, CallEntryTakesNewTarget, GenericConstructEntry, AllocatingConstructEntry };

} // anonymous namespace

template<typename FormOfValue, size_t inlineCapacity>
static ALWAYS_INLINE uint32_t assignLocationsOfConvention(size_t numberOfValues, const FormOfValue& formOfValue, Vector<ArgumentLocation, inlineCapacity>& locations, size_t numberOfCapturesBehind = 0, size_t numberOfEntryParameters = 0)
{
    LocationAllocatorOfConvention allocator { .rawValuesKeepTheirPlaces = !!numberOfCapturesBehind };
    if (!numberOfCapturesBehind) {
        locations.reserveInitialCapacity(numberOfValues);
        for (size_t index = 0; index < numberOfValues; ++index)
            locations.append(allocator.allocate(formOfValue(index)));
        return allocator.numberOfStackWords;
    }
    locations.fill(ArgumentLocation { }, numberOfValues);
    for (size_t place = 0; place < numberOfValues; ++place) {
        size_t index = place;
        if (place < numberOfEntryParameters)
            index = place + numberOfCapturesBehind < numberOfEntryParameters ? place + numberOfCapturesBehind : place + numberOfCapturesBehind - numberOfEntryParameters;
        locations[index] = allocator.allocate(formOfValue(index));
    }
    return allocator.numberOfStackWords;
}

static ArgumentLocation locationOfOnlyResultOfConvention(Representation representation)
{
    if (isPassedInFPR(representation))
        return { .kind = ArgumentLocation::Kind::FPR, .reg = FPRInfo::returnValueFPR, .representation = representation };
    return { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = representation };
}

template<typename FormOfParameter, typename FormOfResult>
static ALWAYS_INLINE ArgumentLocations locationsOfDirectConvention(size_t numberOfParameters, const FormOfParameter& formOfParameter, size_t numberOfResults, const FormOfResult& formOfResult, size_t numberOfCapturesBehind, size_t numberOfEntryParameters)
{
    ArgumentLocations locations;
    locations.convention = EntryConvention::Direct;
    locations.numberOfStackArguments = assignLocationsOfConvention(numberOfParameters, formOfParameter, locations.parameters, numberOfCapturesBehind, numberOfEntryParameters);

    if (!numberOfResults) {
        locations.results.append(ArgumentLocation { .kind = ArgumentLocation::Kind::None, .representation = Representation::JSValue });
        return locations;
    }
    if (numberOfResults == 1) {
        locations.results.append(locationOfOnlyResultOfConvention(formOfResult(0)));
        return locations;
    }
    locations.numberOfStackResults = assignLocationsOfConvention(numberOfResults, formOfResult, locations.results);
    return locations;
}

static ArgumentLocations locationsOfGenericConvention(EntryConvention convention, unsigned numberOfArguments)
{
    ArgumentLocations locations;
    locations.convention = convention;
    locations.parameters.reserveInitialCapacity(numberOfParametersInFrontOfGenericArguments + numberOfArguments);
    locations.parameters.append(ArgumentLocation { .kind = ArgumentLocation::Kind::GPR, .reg = calleeGPR, .representation = Representation::Cell });
    locations.parameters.append(ArgumentLocation { .kind = ArgumentLocation::Kind::GPR, .reg = thisGPR, .representation = Representation::JSValue });
    for (unsigned argument = 0; argument < numberOfArguments; ++argument) {
        if (argument < numberOfArgumentGPRs)
            locations.parameters.append(ArgumentLocation { .kind = ArgumentLocation::Kind::GPR, .reg = argumentGPR(argument), .representation = Representation::JSValue });
        else
            locations.parameters.append(ArgumentLocation { .kind = ArgumentLocation::Kind::Stack, .stackIndex = argument - numberOfArgumentGPRs, .representation = Representation::JSValue });
    }
    locations.numberOfStackArguments = numberOfArguments > numberOfArgumentGPRs ? numberOfArguments - numberOfArgumentGPRs : 0;
    locations.results.append(ArgumentLocation { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = Representation::JSValue });
    return locations;
}

static Representation boxedFormOfDeclaredPosition(const Program& program, const DeclaredType& declared)
{
    TypeID proven = program.provenTypeOf(declared);
    Type type = program.at(proven);
    return type.isJavaScriptValue() ? Representation::JSValue : representationOf(type);
}

static Representation boxedFormOfEntryParameter(const Program& program, const Function& function, unsigned entryParameter)
{
    const DeclaredType* declared = function.declaredEntryParameter(entryParameter);
    return declared ? boxedFormOfDeclaredPosition(program, *declared) : Representation::JSValue;
}

static Representation formOfEntryParameterInConvention(const CallingConventionParts& parts, const Program& program, FunctionID id, unsigned entryParameter)
{
    if (parts.formsOfEntryParameters.isEmpty())
        return boxedFormOfEntryParameter(program, program.at(id), entryParameter);
    const FunctionConvention& convention = parts.conventions[id.index()];
    RELEASE_ASSERT(entryParameter < convention.numberOfEntryParameters);
    return parts.formsOfEntryParameters[convention.firstEntryParameter + entryParameter];
}

static Representation formOfCoroutineDriver(const Function& coroutine)
{
    return coroutine.has(FunctionFlags::IsGenerator) ? Representation::Cell : Representation::JSValue;
}

static unsigned numberOfArgumentsTakenInRegistersBy(const CallingConventions& conventions, const Program& program, CodeUnit entry)
{
    return program.at(entry.function).numberOfPositionalParameters() + conventions.genericEntry(entry).takesNewTarget;
}

static unsigned numberOfParametersInConventionOf(const CallingConventions& conventions, const Program& program, CodeUnit unit)
{
    switch (unit.role) {
    case CodeRole::Body:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
        return conventions.of(unit.function).numberOfEntryParameters + conventions.numberOfScalarsTaken(unit);
    case CodeRole::CoroutineBody:
        return conventions.of(unit.function).numberOfEntryParameters + 1;
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
        switch (conventions.entryConventionOf(unit)) {
        case EntryConvention::Direct:
            break;
        case EntryConvention::GenericRegisters:
            return numberOfParametersInFrontOfGenericArguments + numberOfArgumentsTakenInRegistersBy(conventions, program, unit);
        case EntryConvention::GenericBuffer:
            return numberOfParametersInFrontOfGenericArguments;
        }
        break;
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return 1 + conventions.methodSelector(unit.placeOfSelector()).numberOfArguments;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t stackWordsTakenInConventionOf(const CallingConventions& conventions, const Program& program, CodeUnit unit)
{
    if (std::max(numberOfParametersInConventionOf(conventions, program, unit), conventions.numberOfResults(unit)) <= numberOfValuesThatNeverNeedStackWords)
        return 0;
    return conventions.locationsOf(program, unit).numberOfStackWords();
}

CallingConventions::CallingConventions(CallingConventionParts&& parts)
    : m_parts(WTF::move(parts))
    , m_functionsWithGenericEntry(m_parts.conventions.size())
{
    for (size_t index = 0; index < m_parts.conventions.size(); ++index) {
        if (m_parts.conventions[index].roles & genericEntriesAmongRolesOfConvention)
            m_functionsWithGenericEntry.quickSet(index);
    }
}

bool CallingConventions::existsBehindUnitsOfFunctions(CodeUnit unit) const
{
    switch (unit.role) {
    case CodeRole::TableEntry:
    case CodeRole::TableLookup: {
        if (unit.placeOfSelector() >= methodSelectors().size())
            return false;
        const MethodFamily& family = familyOf(methodSelector(unit.placeOfSelector()));
        return unit.role == CodeRole::TableLookup ? family.entersLookup() && unit.function == family.methods[0] : std::ranges::binary_search(family.methods, unit.function);
    }
    case CodeRole::Body:
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::CoroutineBody:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> CallingConventions::findMethodSelector(PropertyKeyID key, unsigned numberOfArguments) const
{
    std::span<const MethodFamily> families = methodFamilies();
    auto family = std::ranges::lower_bound(families, key, { }, &MethodFamily::key);
    if (family == families.end() || family->key != key)
        return std::nullopt;
    auto placeOf = [](const MethodSelector& selector) {
        return std::pair { selector.family, selector.numberOfArguments };
    };
    std::span<const MethodSelector> selectors = methodSelectors();
    std::pair wanted { static_cast<uint32_t>(family - families.begin()), static_cast<uint32_t>(numberOfArguments) };
    auto selector = std::ranges::lower_bound(selectors, wanted, { }, placeOf);
    if (selector == selectors.end() || placeOf(*selector) != wanted)
        return std::nullopt;
    return static_cast<uint32_t>(selector - selectors.begin());
}

uint32_t CallingConventions::indexBehindUnitsOfFunctions(CodeUnit unit) const
{
    uint32_t firstUnitBehindFunctions = numberOfFunctions() * numberOfCodeRolesOfOneFunction;
    switch (unit.role) {
    case CodeRole::TableEntry:
    case CodeRole::TableLookup: {
        const MethodSelector& selector = methodSelector(unit.placeOfSelector());
        uint32_t lookup = firstUnitBehindFunctions + selector.firstUnit;
        if (unit.role == CodeRole::TableLookup)
            return lookup;
        const Vector<FunctionID>& methods = familyOf(selector).methods;
        auto found = std::ranges::lower_bound(methods, unit.function);
        RELEASE_ASSERT(found != methods.end() && *found == unit.function);
        return lookup + 1 + static_cast<uint32_t>(found - methods.begin());
    }
    case CodeRole::Body:
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::CoroutineBody:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CodeUnit CallingConventions::unitAt(uint32_t index) const
{
    uint32_t numberOfUnitsOfFunctions = numberOfFunctions() * numberOfCodeRolesOfOneFunction;
    if (index < numberOfUnitsOfFunctions)
        return { FunctionID(index / numberOfCodeRolesOfOneFunction), static_cast<CodeRole>(index % numberOfCodeRolesOfOneFunction) };

    uint32_t indexAmongTableUnits = index - numberOfUnitsOfFunctions;
    RELEASE_ASSERT(indexAmongTableUnits < m_parts.methodTable.numberOfUnits);
    std::span<const MethodSelector> selectors = methodSelectors();
    const MethodSelector& selector = *std::prev(std::ranges::upper_bound(selectors, indexAmongTableUnits, { }, &MethodSelector::firstUnit));
    if (indexAmongTableUnits == selector.firstUnit)
        return tableLookupOf(selector);
    return CodeUnit::ofMethodTable(familyOf(selector).methods[indexAmongTableUnits - selector.firstUnit - 1], CodeRole::TableEntry, placeOf(selector));
}

static Representation formOfParameterProvenToBe(const Program& program, TypeID proven)
{
    if (!proven)
        return Representation::JSValue;
    Representation form = representationOf(program.at(proven));
    return form == Representation::Int32 || form == Representation::Int64 || form == Representation::Double ? form : Representation::JSValue;
}

Representation CallingConventions::formBehindGuard(const Program& program, const FunctionConvention& convention, unsigned parameterIndex) const
{
    TypeID tested = m_parts.typesBehindGuards[convention.firstTypeBehindGuard + parameterIndex];
    return tested ? formOfParameterProvenToBe(program, tested) : Representation::JSValue;
}

Representation CallingConventions::parameterRepresentationByRole(const Program& program, CodeUnit unit, unsigned parameterIndex) const
{
    switch (unit.role) {
    case CodeRole::CoroutineBody:
        if (parameterIndex == of(unit.function).numberOfEntryParameters)
            return formOfCoroutineDriver(program.at(unit.function));
        return formOfEntryParameterInConvention(m_parts, program, unit.function, parameterIndex);
    case CodeRole::Body:
    case CodeRole::ModuleStart:
    case CodeRole::ParameterGuard:
        RELEASE_ASSERT(m_parts.formsOfEntryParameters.isEmpty());
        return boxedFormOfEntryParameter(program, program.at(unit.function), parameterIndex);
    case CodeRole::SlowBody:
        if (formOfEntryParameterInConvention(m_parts, program, unit.function, parameterIndex) == Representation::Void)
            return Representation::Void;
        return boxedFormOfEntryParameter(program, program.at(unit.function), parameterIndex);
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
        RELEASE_ASSERT(parameterIndex < numberOfParametersInConventionOf(*this, program, unit));
        return parameterIndex ? Representation::JSValue : Representation::Cell;
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        RELEASE_ASSERT(parameterIndex < numberOfParametersInConventionOf(*this, program, unit));
        if (const MethodSelector& selector = methodSelector(unit.placeOfSelector()); parameterIndex && selector.isTyped)
            return formOfParameterProvenToBe(program, familyOf(selector).typesOfArguments[parameterIndex - 1]);
        return parameterIndex ? Representation::JSValue : Representation::Cell;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static size_t numberOfCapturesPassedBehindParameters(const CallingConventions& conventions, const Program& program, CodeUnit unit)
{
    size_t numberOfCaptures = program.at(unit.function).captures.size();
    if (!numberOfCaptures)
        return 0;
    bool takesParametersOfFunction = unit.role == CodeRole::Body || unit.role == CodeRole::SlowBody || unit.role == CodeRole::ParameterGuard || unit.role == CodeRole::CoroutineBody;
    unsigned numberOfValues = conventions.of(unit.function).numberOfEntryParameters + conventions.isCoroutine(unit.function);
    return takesParametersOfFunction && numberOfValues <= GPRInfo::numberOfArgumentRegisters ? numberOfCaptures : 0;
}

ArgumentLocations CallingConventions::locationsOf(const Program& program, CodeUnit unit) const
{
    EntryConvention convention = entryConventionOf(unit);
    switch (convention) {
    case EntryConvention::Direct: {
        auto formOfResult = [&](size_t index) {
            return resultRepresentation(unit, static_cast<unsigned>(index));
        };
        size_t numberOfCapturesBehind = numberOfCapturesPassedBehindParameters(*this, program, unit);
        size_t numberOfEntryParameters = numberOfCapturesBehind ? of(unit.function).numberOfEntryParameters : 0;
        if (takesEntryParametersAsListed(unit.role) && !m_parts.formsOfEntryParameters.isEmpty()) {
            const FunctionConvention& ofFunction = of(unit.function);
            return locationsOfDirectConvention(ofFunction.numberOfEntryParameters + numberOfScalarsTaken(unit), [&](size_t index) {
                if (index >= ofFunction.numberOfEntryParameters)
                    return formOfScalar(unit, static_cast<unsigned>(index));
                return formOfEntryParameterAsListed(program, ofFunction, unit.role, static_cast<unsigned>(index));
            }, numberOfResults(unit), formOfResult, numberOfCapturesBehind, numberOfEntryParameters);
        }
        return locationsOfDirectConvention(numberOfParametersInConventionOf(*this, program, unit), [&](size_t index) {
            return parameterRepresentation(program, unit, static_cast<unsigned>(index));
        }, numberOfResults(unit), formOfResult, numberOfCapturesBehind, numberOfEntryParameters);
    }
    case EntryConvention::GenericRegisters:
        return locationsOfGenericConvention(convention, numberOfArgumentsTakenInRegistersBy(*this, program, unit));
    case EntryConvention::GenericBuffer:
        return locationsOfGenericConvention(convention, 0);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t CallingConventions::stackWordsReservedByCallersOf(const Program& program, CodeUnit callee) const
{
    if (isGenericEntry(callee.role))
        return 0;

    if (isTableRole(callee.role))
        return stackWordsTakenInConventionOf(*this, program, callee);

    const FunctionConvention& convention = of(callee.function);
    if (std::max<unsigned>(convention.numberOfEntryParameters + 1 + numberOfScalarsTaken({ callee.function, CodeRole::Body }), convention.numberOfResults) <= numberOfValuesThatNeverNeedStackWords)
        return 0;

    uint32_t words = stackWordsTakenInConventionOf(*this, program, callee);
    for (CodeRole role : { CodeRole::Body, CodeRole::SlowBody, CodeRole::ParameterGuard, CodeRole::CoroutineBody }) {
        if (role != callee.role && convention.has(role))
            words = std::max(words, stackWordsTakenInConventionOf(*this, program, { callee.function, role }));
    }
    return words;
}

static uint8_t argumentsThatEntryOnlyCalls(const Program& program, const Facts& facts, FunctionID id, const FunctionConvention& convention)
{
    static_assert(ClosureWord::numberOfBorrowedArguments == 2, "FunctionConvention::borrowedArguments has a bit for each");
    const Function& function = program.at(id);
    if (!convention.has(CodeRole::Body) || !convention.has(CodeRole::GenericCallEntry) || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) || convention.callEntryTakesNewTarget || function.newTargetParameterIndex())
        return 0;
    uint8_t onlyCalled = 0;
    for (unsigned argument = 0; argument < std::min<unsigned>(ClosureWord::numberOfBorrowedArguments, function.numberOfPositionalParameters()); ++argument) {
        if (facts.linked().onlyCallsWhatArrivesAt(id, function.firstParameterIndex() + argument))
            onlyCalled |= 1u << argument;
    }
    return onlyCalled;
}

GenericEntryPlan CallingConventions::genericEntry(CodeUnit unit) const
{
    const FunctionConvention& convention = of(unit.function);
    GenericEntryPlan plan;
    plan.target = directEntryOf(unit.function);
    switch (unit.role) {
    case CodeRole::GenericCallEntry:
        plan.takesNewTarget = convention.callEntryTakesNewTarget;
        plan.borrowedArguments = convention.borrowedArguments;
        if (convention.has(CodeRole::AllocatingConstructEntry))
            plan.constructEntry = { unit.function, CodeRole::AllocatingConstructEntry };
        else if (convention.has(CodeRole::GenericConstructEntry))
            plan.constructEntry = { unit.function, CodeRole::GenericConstructEntry };
        if (convention.has(CodeRole::ModuleStart))
            plan.target = { unit.function, CodeRole::ModuleStart };
        return plan;
    case CodeRole::GenericConstructEntry:
        plan.takesNewTarget = true;
        return plan;
    case CodeRole::AllocatingConstructEntry:
        plan.takesNewTarget = true;
        plan.allocatesThis = true;
        return plan;
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CallingConventions::setParameterGuards(std::span<const ParameterGuardPlan> guards)
{
    for (const ParameterGuardPlan& guard : guards) {
        FunctionConvention& convention = m_parts.conventions[guard.function.index()];
        RELEASE_ASSERT(convention.has(CodeRole::Body) && !convention.has(CodeRole::ParameterGuard) && guard.typesBehindGuard.size() == convention.numberOfEntryParameters);
        convention.roles |= bitOfRoleInConvention(CodeRole::SlowBody) | bitOfRoleInConvention(CodeRole::ParameterGuard);
        convention.firstTypeBehindGuard = safeCast<uint32_t>(m_parts.typesBehindGuards.size());
        m_parts.typesBehindGuards.appendVector(guard.typesBehindGuard);
    }
}

void CallingConventions::setScalarizedResults(ScalarizedResults&& results)
{
    m_functionsWithScalarizedResult = BitVector(numberOfFunctions());
    for (const ScalarizedResult& result : results) {
        FunctionConvention& convention = m_parts.conventions[result.function.index()];
        RELEASE_ASSERT(convention.numberOfResults == 1 && result.forms.size() > 1 && result.forms.size() == result.fields.size());
        convention.numberOfResults = safeCast<uint16_t>(result.forms.size());
        convention.firstResult = safeCast<uint32_t>(m_parts.formsOfResults.size());
        m_parts.formsOfResults.append(result.forms.span());
        m_functionsWithScalarizedResult.quickSet(result.function.index());
    }
    m_scalarizedResults = WTF::move(results);
}

void CallingConventions::setScalarizedParameters(const Program& program, ScalarizedParameters&& parameters)
{
    m_functionsWithScalarizedParameters = BitVector(numberOfFunctions());
    if (parameters.isEmpty())
        return;
    if (m_parts.formsOfEntryParameters.isEmpty()) {
        for (uint32_t index = 0; index < numberOfFunctions(); ++index) {
            FunctionConvention& convention = m_parts.conventions[index];
            convention.firstEntryParameter = safeCast<uint32_t>(m_parts.formsOfEntryParameters.size());
            for (unsigned entryParameter = 0; entryParameter < convention.numberOfEntryParameters; ++entryParameter)
                m_parts.formsOfEntryParameters.append(boxedFormOfEntryParameter(program, program.functions[index], entryParameter));
        }
    }
    for (const ScalarizedParameter& parameter : parameters) {
        const FunctionConvention& convention = m_parts.conventions[parameter.function.index()];
        RELEASE_ASSERT(parameter.entryParameter < convention.numberOfEntryParameters && parameter.firstScalar >= convention.numberOfEntryParameters);
        m_parts.formsOfEntryParameters[convention.firstEntryParameter + parameter.entryParameter] = Representation::Void;
        m_functionsWithScalarizedParameters.quickSet(parameter.function.index());
    }
    m_scalarizedParameters = WTF::move(parameters);
}

Representation CallingConventions::formOfScalar(CodeUnit unit, unsigned parameterIndex) const
{
    for (const ScalarizedParameter& parameter : scalarizedParametersOf(unit.function)) {
        if (parameterIndex >= parameter.firstScalar && parameterIndex - parameter.firstScalar < parameter.forms.size())
            return parameter.forms[parameterIndex - parameter.firstScalar];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Vector<CallEntryMove, 8>> planCallEntryMoves(const Program& program, const CallingConventions& conventions, FunctionID id)
{
    const Function& function = program.at(id);
    CodeUnit entry = conventions.closureEntryOf(id);
    if (!conventions.exists(entry) || function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor)
        return std::nullopt;
    GenericEntryPlan plan = conventions.genericEntry(entry);
    CodeUnit target = plan.target;
    if (conventions.entryConventionOf(entry) != EntryConvention::GenericRegisters || plan.takesNewTarget || !target || !conventions.exists(target))
        return std::nullopt;
    if (target.role == CodeRole::ModuleStart)
        return std::nullopt;
    if (conventions.takesScalars(target) || conventions.numberOfResults(target) != 1 || !haveSameRegisterRepresentation(conventions.resultRepresentation(target), conventions.resultRepresentation(entry)))
        return std::nullopt;
    if (conventions.stackWordsReservedByCallersOf(program, target))
        return std::nullopt;

    ArgumentLocations taken = conventions.locationsOf(program, target);
    unsigned numberOfEntryParameters = function.numberOfEntryParameters();
    if (taken.parameters.size() != numberOfEntryParameters || taken.numberOfStackArguments)
        return std::nullopt;

    Vector<CallEntryMove, 8> moves;
    unsigned firstParameter = function.firstParameterIndex();
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        const ArgumentLocation& destination = taken.parameters[index];
        if (destination.kind == ArgumentLocation::Kind::None)
            continue;
        bool takesBoxedValue = destination.kind == ArgumentLocation::Kind::GPR && destination.representation == Representation::JSValue;
        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            if (closureCaptureRepresentationOf(program, conventions, id, index) != destination.representation)
                return std::nullopt;
            moves.append(CallEntryMove { CallEntryMove::Source::Capture, closureCaptureSlotOf(program, conventions, id, index), destination });
            break;
        case EntryParameter::This:
            if (!takesBoxedValue)
                return std::nullopt;
            moves.append(CallEntryMove { CallEntryMove::Source::This, 0, destination });
            break;
        case EntryParameter::Callee:
            if (destination.kind != ArgumentLocation::Kind::GPR || !isCellOrBoxedValue(destination.representation))
                return std::nullopt;
            moves.append(CallEntryMove { CallEntryMove::Source::Closure, 0, destination });
            break;
        case EntryParameter::NewTarget:
            if (!takesBoxedValue)
                return std::nullopt;
            moves.append(CallEntryMove { CallEntryMove::Source::Undefined, 0, destination });
            break;
        case EntryParameter::Parameter: {
            unsigned argument = index - firstParameter;
            if (!takesBoxedValue || argument >= function.numberOfPositionalParameters() || argument >= numberOfArgumentGPRs)
                return std::nullopt;
            TypeID written = function.parameters[argument].written();
            if (written && (program.at(written).primitives & Primitive::Absent))
                return std::nullopt;
            moves.append(CallEntryMove { CallEntryMove::Source::Argument, argument, destination });
            break;
        }
        case EntryParameter::ArgumentCount:
        case EntryParameter::HiddenRest:
            return std::nullopt;
        }
    }
    return moves;
}

static bool entryByValueHasToAskForConventions(const Program& program, const Facts& facts, FunctionID id)
{
    const Function& function = program.at(id);
    bool takesWhatCallersLeave = id.index() < facts.linked().functions.size() && facts.linked().bodyTakesWhatCallersLeave(id);
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        EntryParameter kind = function.entryParameterKind(index);
        if ((kind == EntryParameter::This || kind == EntryParameter::Parameter || kind == EntryParameter::NewTarget) && (takesWhatCallersLeave || facts.typeOfEntryParameter(id, index).proven))
            return true;
    }
    return false;
}

static bool runTimeEntersBodyByNumberOfItsStart(const Program& program, const FunctionConvention& convention, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.isModuleBody() || !function.module || !convention.has(CodeRole::ModuleStart) || !convention.has(CodeRole::GenericCallEntry))
        return false;
    const Module& module = program.at(function.module);
    if (module.body != id)
        return false;
    return module.kind == ModuleKind::CommonJs || (module.kind == ModuleKind::SourceText && !module.flags.contains(ModuleFlags::HasTopLevelAwait));
}

void CallingConventions::fuseCallEntriesThatOnlyMove(const Program& program, const Facts& facts, const ParallelLoops& loops)
{
    static constexpr uint32_t functionsInRange = 256;
    static_assert(!(functionsInRange % 64));
    uint32_t numberOfFunctions = this->numberOfFunctions();
    BitVector functionsWithFusedCallEntry(numberOfFunctions);
    forEachIndexInParallel(loops, (numberOfFunctions + functionsInRange - 1) / functionsInRange, [&](unsigned range) {
        uint32_t endOfRange = std::min(numberOfFunctions, (range + 1) * functionsInRange);
        for (uint32_t index = range * functionsInRange; index < endOfRange; ++index) {
            if (runTimeEntersBodyByNumberOfItsStart(program, of(FunctionID(index)), FunctionID(index)) || (!entryByValueHasToAskForConventions(program, facts, FunctionID(index)) && planCallEntryMoves(program, *this, FunctionID(index))))
                functionsWithFusedCallEntry.quickSet(index);
        }
    });
    m_functionsWithFusedCallEntry = WTF::move(functionsWithFusedCallEntry);
}

static bool constructEntryAllocatesThisOf(const Program& program, const Function& function, const BitVector& layoutsMadeAsCompiledObjects)
{
    if (function.kind != FunctionKind::Plain || function.isCoroutine() || !function.has(FunctionFlags::HasThis) || function.results.size() != 1 || !function.constructedClass)
        return false;
    LayoutID declared = declaredLayoutOf(program, function.constructedClass);
    return declared && program.at(declared).has(LayoutFlags::IsConstructorFunction) && !isConstructorFunctionBelowValue(program.at(declared)) && layoutsMadeAsCompiledObjects.get(function.constructedClass.index());
}

static ConstructionThroughConvention constructionThroughConventionOf(const Program& program, const Function& function, const BitVector& layoutsMadeAsCompiledObjects)
{
    if (!function.has(FunctionFlags::HasNewTarget))
        return constructEntryAllocatesThisOf(program, function, layoutsMadeAsCompiledObjects) ? ConstructionThroughConvention::AllocatingConstructEntry : ConstructionThroughConvention::None;
    switch (function.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return ConstructionThroughConvention::CallEntryTakesNewTarget;
    case FunctionKind::Plain:
        return function.isCoroutine() ? ConstructionThroughConvention::None : ConstructionThroughConvention::GenericConstructEntry;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return ConstructionThroughConvention::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void chooseRolesOfConvention(const Program& program, const Facts& facts, FunctionID id, const BitVector& layoutsMadeAsCompiledObjects, FunctionConvention& convention)
{
    const Function& function = program.at(id);
    if (!function.hasBody() || !hasSourceBodyOf(function.role) || !canBeCalledByValueOf(function.role))
        return;

    auto add = [&](CodeRole role) {
        convention.roles |= bitOfRoleInConvention(role);
    };

    add(CodeRole::GenericCallEntry);
    convention.takesArgumentsInBuffer = function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount });
    convention.callEntryWalksBody = id.index() < facts.linked().functions.size() && facts.linked().function(id).callEntryWalksBody;

    bool mayBeReached = facts.mayBeReached(id);
    switch (constructionThroughConventionOf(program, function, layoutsMadeAsCompiledObjects)) {
    case ConstructionThroughConvention::None:
        break;
    case ConstructionThroughConvention::CallEntryTakesNewTarget:
        convention.callEntryTakesNewTarget = true;
        break;
    case ConstructionThroughConvention::GenericConstructEntry:
        if (mayBeReached)
            add(CodeRole::GenericConstructEntry);
        break;
    case ConstructionThroughConvention::AllocatingConstructEntry:
        if (mayBeReached)
            add(CodeRole::AllocatingConstructEntry);
        break;
    }
    if (!mayBeReached)
        return;

    add(CodeRole::Body);
    if (function.isCoroutine()) {
        add(CodeRole::CoroutineBody);
        convention.coroutineNeverSuspends = isAsyncFunctionInWhichNothingSuspends(program, facts, id);
        convention.generatorNeverYields = isNonYieldingGenerator(program, facts, id);
    }
}

void CallingConventions::setModuleStarts(const Program& program, const BitVector& bodiesWithModuleStart)
{
    for (size_t index : bodiesWithModuleStart) {
        FunctionConvention& convention = m_parts.conventions[index];
        if (convention.has(CodeRole::Body) && program.functions[index].isModuleBody())
            convention.roles |= bitOfRoleInConvention(CodeRole::ModuleStart);
    }
}

void CallingConventions::setRemovedCaptures(const Program& program, const PromotedEnvironments& promoted)
{
    static_assert(!static_cast<unsigned>(EntryParameter::Capture));
    if (promoted.removedCaptures.isEmpty())
        return;
    if (m_parts.formsOfEntryParameters.isEmpty()) {
        for (uint32_t index = 0; index < numberOfFunctions(); ++index) {
            FunctionConvention& convention = m_parts.conventions[index];
            convention.firstEntryParameter = safeCast<uint32_t>(m_parts.formsOfEntryParameters.size());
            for (unsigned entryParameter = 0; entryParameter < convention.numberOfEntryParameters; ++entryParameter)
                m_parts.formsOfEntryParameters.append(boxedFormOfEntryParameter(program, program.functions[index], entryParameter));
        }
    }
    for (const RemovedCapture& removed : promoted.removedCaptures)
        m_parts.formsOfEntryParameters[m_parts.conventions[removed.function.index()].firstEntryParameter + removed.capture] = Representation::Void;
}

static constexpr Range uint32RangeInConvention { 0, std::numeric_limits<uint32_t>::max() };

static Representation formOfResultProvenToBe(const Program& program, TypeID proven)
{
    if (!proven)
        return Representation::JSValue;
    const Type& type = program.at(proven);
    switch (representationOf(type)) {
    case Representation::Boolean:
        return Representation::Boolean;
    case Representation::Int32:
        return Representation::Int32;
    case Representation::Int64:
        return type.integers.isSubsetOf(uint32RangeInConvention) ? Representation::Int64 : Representation::Double;
    case Representation::Double:
        return Representation::Double;
    case Representation::OptionalInt64:
        return Representation::OptionalInt64;
    case Representation::OptionalDouble:
        return Representation::OptionalDouble;
    default:
        return Representation::JSValue;
    }
}

static bool tellsAbsentFromUndefinedInConvention(const Program& program, const DeclaredType& parameter)
{
    TypeID written = parameter.written();
    return written && (program.at(written).primitives & Primitive::Absent);
}

static Representation formOfParameterInConvention(const Program& program, const DeclaredType& parameter, TypeID proven)
{
    TypeID written = parameter.written();
    bool isOptionalNumber = written && proven && representationOf(program.at(written)) == Representation::OptionalDouble && representationOf(program.at(proven)) == Representation::OptionalDouble;
    return isOptionalNumber ? Representation::OptionalDouble : formOfParameterProvenToBe(program, proven);
}

static bool isIntegerOfLoopInConvention(const Facts& facts, FunctionID id, unsigned entryParameter, bool hasNoFraction)
{
    static constexpr size_t mostNames = 8;
    static constexpr OpcodeSet accesses { Opcode::ElemGet, Opcode::ElemSet };
    static constexpr OpcodeSet steps = opcodesThatStateTypes | OpcodeSet { Opcode::Increment, Opcode::Decrement, Opcode::ToNumeric };
    const BodyIndex* body = facts.bodies().find(id);
    if (!body || !body->hasLoops() || (!hasNoFraction && !body->holdsAnyOf(accesses)) || entryParameter >= body->block(entryBlock).numberOfParameters())
        return false;
    Vector<ValueID, mostNames> names { body->block(entryBlock).parameter(entryParameter) };
    auto addName = [&](ValueID name) {
        if (names.size() < mostNames && !names.contains(name))
            names.append(name);
    };
    for (size_t known = 0; known != names.size();) {
        known = names.size();
        for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
            BlockID block(number);
            if (!body->isReachable(block))
                continue;
            bool isInLoop = !!body->loopOf(block);
            for (BodyValue operation : body->block(block).values()) {
                if (isInLoop && accesses.contains(operation.opcode()) && names.contains(operation.propertyOperand()))
                    return true;
                if (steps.contains(operation.opcode()) && names.contains(operation.operand(0)))
                    addName(operation.id());
            }
            for (BodyTarget target : body->terminator(block).targets()) {
                std::span<const ValueID> arguments = target.arguments();
                for (uint32_t position = 0; position < arguments.size(); ++position) {
                    if (!names.contains(arguments[position]) || position >= body->block(target.block()).numberOfParameters())
                        continue;
                    if (hasNoFraction && body->isLoopHead(target.block()))
                        return true;
                    addName(body->block(target.block()).parameter(position));
                }
            }
        }
    }
    return false;
}

static bool declaresNumberInConvention(const Program& program, const DeclaredType& position)
{
    TypeID written = position.written();
    return written && program.at(written).isNumber() && !program.at(written).isInteger();
}

static bool returnsValueCarriedByLoop(const Facts& facts, FunctionID id)
{
    const BodyIndex* body = facts.bodies().find(id);
    if (!body || !body->hasLoops())
        return false;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BlockID block(number);
        BodyTerminator end = body->isReachable(block) ? body->terminator(block) : BodyTerminator();
        if (!end || end.kind() != TerminatorKind::Return)
            continue;
        ValueID returned = body->stripCasts(end.as<TerminatorKind::Return>().valueOperand());
        if (returned && body->isParameter(returned) && body->isLoopHead(body->blockOf(returned)))
            return true;
    }
    return false;
}

static void fillFormsOfEntryParameters(const Program& program, const Facts& facts, FunctionID id, bool mayTakeUnboxedNumbers, std::span<Representation> forms)
{
    const Function& function = program.at(id);
    unsigned numberOfEntryParameters = static_cast<unsigned>(forms.size());
    unsigned firstPositionalParameter = function.firstParameterIndex();
    unsigned numberOfPositionalParameters = function.numberOfPositionalParameters();
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        Representation form = boxedFormOfEntryParameter(program, function, index);
        bool isPositional = index >= firstPositionalParameter && index - firstPositionalParameter < numberOfPositionalParameters;
        if (mayTakeUnboxedNumbers && isPositional && form == Representation::JSValue && !tellsAbsentFromUndefinedInConvention(program, function.parameters[index - firstPositionalParameter]))
            form = formOfParameterInConvention(program, function.parameters[index - firstPositionalParameter], facts.typeOfEntryParameter(id, index).proven);
        if (form == Representation::Double && !declaresNumberInConvention(program, function.parameters[index - firstPositionalParameter]) && isIntegerOfLoopInConvention(facts, id, index, program.at(facts.typeOfEntryParameter(id, index).proven).isNumberWithoutFraction()))
            form = Representation::JSValue;
        forms[index] = form;
    }
}

static constexpr RepresentationSet formsTakenAsDoublesInConvention { Representation::Int32, Representation::Int64, Representation::Double };

static void boxResultsOfMethodsWhoseNamesakesReturnOtherwise(const Program& program, const LinkTables& linked, BitVector& functionsThatReturnRawValues, CallingConventionParts& parts)
{
    auto forEachMethodThatSharesCalls = [&](const auto& visit) {
        for (const LinkedMember& member : linked.members) {
            if (member.key && member.written && member.kind == MethodKind::Method && member.side == ClassSide::Prototype && !linked.function(member.written).everyCallHasOneCallee)
                visit(member.key, member.written);
        }
    };
    Vector<Representation> formsUnderKeys(FillWith { }, program.types.numberOfPropertyKeys(), Representation::Unchosen);
    BitVector keysWhoseMethodsDiffer(program.types.numberOfPropertyKeys());
    forEachMethodThatSharesCalls([&](PropertyKeyID key, FunctionID method) {
        Representation form = functionsThatReturnRawValues.get(method.index()) ? parts.formsOfResults[parts.conventions[method.index()].firstResult] : Representation::JSValue;
        Representation& known = formsUnderKeys[key.index()];
        static constexpr RepresentationSet formsOfSelectors { Representation::JSValue, Representation::Int32, Representation::Int64, Representation::Double };
        if (known != Representation::Unchosen && known != form && formsTakenAsDoublesInConvention.contains(known) && formsTakenAsDoublesInConvention.contains(form))
            known = form = Representation::Double;
        if (!formsOfSelectors.contains(form) || (known != Representation::Unchosen && known != form))
            keysWhoseMethodsDiffer.quickSet(key.index());
        known = form;
    });
    forEachMethodThatSharesCalls([&](PropertyKeyID key, FunctionID method) {
        if (!functionsThatReturnRawValues.get(method.index()))
            return;
        Representation& form = parts.formsOfResults[parts.conventions[method.index()].firstResult];
        if (keysWhoseMethodsDiffer.quickGet(key.index())) {
            functionsThatReturnRawValues.clear(method.index());
            form = Representation::JSValue;
        } else if (formsTakenAsDoublesInConvention.contains(form))
            form = formsUnderKeys[key.index()];
    });
}

static BitVector collectDirectlyCalledClosures(const Program& program, const LinkTables& linked, const LinkTables& held)
{
    BitVector closures(program.functions.size());
    auto addCallees = [&](std::span<const LinkedCall> calls) {
        for (const LinkedCall& call : calls) {
            FunctionKind kind = program.at(call.callee).kind;
            if (!call.isInTailPosition && (kind == FunctionKind::Plain || kind == FunctionKind::Arrow))
                closures.quickSet(call.callee.index());
        }
    };
    addCallees(linked.calls);
    addCallees(held.calls);
    for (const LinkedFunctionWrittenOverBuiltin& written : linked.functionsWrittenOverBuiltins)
        closures.quickSet(written.function.index());
    return closures;
}

static bool isUnboxedWiderThanInt32(Representation form)
{
    return form == Representation::Int64 || form == Representation::Double || form == Representation::OptionalDouble;
}

static void boxResultsThatNobodyTakesRaw(const Program& program, const LinkTables& linked, const BitVector& directlyCalledClosures, BitVector& functionsThatReturnRawValues, CallingConventionParts& parts)
{
    auto returnsRawValue = [&](FunctionID id) {
        return functionsThatReturnRawValues.get(id.index());
    };

    Vector<uint32_t> numberOfTakers(FillWith { }, parts.conventions.size(), 0u);
    for (const LinkedCall& call : linked.calls) {
        if (returnsRawValue(call.callee) && (!call.isInTailPosition || returnsRawValue(call.caller)))
            ++numberOfTakers[call.callee.index()];
    }
    auto hasTakersThatLinkDoesNotList = [&](FunctionID id) {
        FunctionKind kind = program.at(id).kind;
        return kind == FunctionKind::Getter || (kind == FunctionKind::Method && !linked.function(id).everyCallHasOneCallee);
    };
    Vector<FunctionID> functionsWithoutTakers;
    for (size_t index : functionsThatReturnRawValues) {
        FunctionID id(static_cast<uint32_t>(index));
        if (!numberOfTakers[index] && !hasTakersThatLinkDoesNotList(id) && !directlyCalledClosures.get(index))
            functionsWithoutTakers.append(id);
    }
    while (!functionsWithoutTakers.isEmpty()) {
        FunctionID boxed = functionsWithoutTakers.takeLast();
        functionsThatReturnRawValues.clear(boxed.index());
        parts.formsOfResults[parts.conventions[boxed.index()].firstResult] = Representation::JSValue;
        for (const LinkedCall& call : std::ranges::equal_range(linked.calls, boxed, { }, &LinkedCall::caller)) {
            if (call.isInTailPosition && returnsRawValue(call.callee) && !--numberOfTakers[call.callee.index()] && !directlyCalledClosures.get(call.callee.index()))
                functionsWithoutTakers.append(call.callee);
        }
    }
}

static bool returnsValueReadFromList(const Program& program, const ThinSummary& summary, TypeID result)
{
    bool takesOut = false;
    bool mayHaveFraction = program.at(result).primitives & Primitive::FractionalNumber;
    for (const SummaryEdge& edge : summary.edges) {
        if (summary.nodes[edge.to].kind != SummaryNode::Kind::Result)
            continue;
        const SummaryNode& returned = summary.nodes[edge.from];
        if (returned.kind == SummaryNode::Kind::ResultOfAccess && !mayHaveFraction && !summary.accesses[returned.first].key) {
            takesOut = true;
            continue;
        }
        if (returned.kind != SummaryNode::Kind::ResultOfCall)
            return false;
        const SummaryCall& call = summary.calls[returned.first];
        BuiltinName name = call.callee == SummaryCall::Callee::Name && call.key ? program.builtinNameOf(call.key) : BuiltinName();
        if (name != BuiltinNames::pop && name != BuiltinNames::shift)
            return false;
        takesOut = true;
    }
    return takesOut;
}

CallingConventions computeCallingConventions(const Program& program, const Facts& facts, const BitVector& layoutsMadeAsCompiledObjects, const ParallelLoops& loops)
{
    static_assert(!(indicesInRangeOfLoop % 64));
    bool unboxesNumbers = facts.mechanisms().containsAll({ Mechanism::UnboxedNumbers, Mechanism::Arrivals });
    const LinkTables& linked = facts.linked();
    bool returnsRawValues = unboxesNumbers && linked.functions.size() == program.functions.size();
    uint32_t numberOfFunctions = safeCast<uint32_t>(program.functions.size());

    CallingConventionParts parts;
    parts.conventions.fill(FunctionConvention { }, numberOfFunctions);
    size_t numberOfResults = 0;
    size_t numberOfEntryParameters = 0;
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        const Function& function = program.functions[index];
        FunctionConvention& convention = parts.conventions[index];
        convention.numberOfEntryParameters = safeCast<uint16_t>(function.numberOfEntryParameters());
        convention.numberOfResults = safeCast<uint16_t>(function.results.size());
        convention.firstResult = safeCast<uint32_t>(numberOfResults);
        numberOfResults += convention.numberOfResults;
        if (!unboxesNumbers)
            continue;
        convention.firstEntryParameter = safeCast<uint32_t>(numberOfEntryParameters);
        numberOfEntryParameters += convention.numberOfEntryParameters;
    }
    parts.formsOfResults.fill(Representation::JSValue, numberOfResults);
    parts.formsOfEntryParameters.fill(Representation::JSValue, numberOfEntryParameters);

    BitVector functionsThatReturnRawValues(numberOfFunctions);
    BitVector directlyCalledClosures = returnsRawValues ? collectDirectlyCalledClosures(program, linked, facts.held()) : BitVector();
    forEachRangeOfIndicesInParallel(loops, numberOfFunctions, [&](size_t firstFunction, size_t endOfFunctions) {
        for (size_t index = firstFunction; index < endOfFunctions; ++index) {
            FunctionID id(static_cast<uint32_t>(index));
            const Function& function = program.at(id);
            FunctionConvention& convention = parts.conventions[index];
            chooseRolesOfConvention(program, facts, id, layoutsMadeAsCompiledObjects, convention);
            convention.borrowedArguments = argumentsThatEntryOnlyCalls(program, facts, id, convention);
            convention.returnsCapture = convention.has(CodeRole::Body) && convention.has(CodeRole::GenericCallEntry) && !facts.everyCallerIsKnown(id) && !!findReturnedCapture(program, function);

            std::span<Representation> formsOfResults = parts.formsOfResults.mutableSpan().subspan(convention.firstResult, convention.numberOfResults);
            for (size_t result = 0; result < formsOfResults.size(); ++result)
                formsOfResults[result] = boxedFormOfDeclaredPosition(program, function.results[result]);
            if (returnsRawValues && convention.has(CodeRole::Body) && mayReturnInFormOfItsOwn(function) && formsOfResults.back() == Representation::JSValue) {
                const LinkedFunction& proven = linked.function(id);
                if (proven.ringsOfTailCallsAreListed && (proven.everyTakerOfResultKnowsCallee || directlyCalledClosures.get(index))) {
                    Representation form = formOfResultProvenToBe(program, proven.result);
                    if (form == Representation::OptionalDouble && returnsValueReadFromList(program, facts.summaries().at(id), proven.result))
                        form = Representation::JSValue;
                    if (form == Representation::Double && !declaresNumberInConvention(program, function.results.last()) && program.at(proven.result).isNumberWithoutFraction() && returnsValueCarriedByLoop(facts, id))
                        form = Representation::JSValue;
                    if (proven.everyTakerOfResultKnowsCallee || isUnboxedWiderThanInt32(form))
                        formsOfResults.back() = form;
                }
                if (formsOfResults.back() != Representation::JSValue)
                    functionsThatReturnRawValues.quickSet(index);
            }
            if (!unboxesNumbers)
                continue;
            bool mayTakeUnboxedNumbers = convention.has(CodeRole::Body) && facts.everyCallerIsKnown(id) && !convention.takesArgumentsInBuffer && !function.isCoroutine();
            fillFormsOfEntryParameters(program, facts, id, mayTakeUnboxedNumbers, parts.formsOfEntryParameters.mutableSpan().subspan(convention.firstEntryParameter, convention.numberOfEntryParameters));
        }
    });
    boxResultsOfMethodsWhoseNamesakesReturnOtherwise(program, linked, functionsThatReturnRawValues, parts);
    boxResultsThatNobodyTakesRaw(program, linked, directlyCalledClosures, functionsThatReturnRawValues, parts);
    return CallingConventions(WTF::move(parts));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

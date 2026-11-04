/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallingConventions.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTCoroutinePlan.h"
#include "AOTFacts.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTJSONSerializerPlan.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
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

// The scratch registers are dead at every call and at every return.
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

// The closure and `this`.
static constexpr unsigned numberOfParametersInFrontOfGenericArguments = 2;

// Each value takes the next register of its bank, so this many values need no stack word whatever their forms are.
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
        // Nobody hands it over.
        if (representation == Representation::Void)
            return { .kind = ArgumentLocation::Kind::None, .representation = representation };
        if (isPassedInFPR(representation)) {
            if (nextFPR < numberOfDirectArgumentFPRs)
                return { .kind = ArgumentLocation::Kind::FPR, .reg = directArgumentFPR(nextFPR++), .representation = representation };
        } else if (nextGPR < numberOfDirectArgumentGPRs)
            return { .kind = ArgumentLocation::Kind::GPR, .reg = directArgumentGPR(nextGPR++), .representation = representation };
        return { .kind = ArgumentLocation::Kind::Stack, .stackIndex = numberOfStackWords++, .representation = representation };
    }

    unsigned nextGPR { 0 };
    unsigned nextFPR { 0 };
    uint32_t numberOfStackWords { 0 };
};

enum class ConstructionThroughConvention : uint8_t { None, CallEntryTakesNewTarget, GenericConstructEntry, AllocatingConstructEntry };

} // anonymous namespace

template<typename FormOfValue, size_t inlineCapacity>
static ALWAYS_INLINE uint32_t assignLocationsOfConvention(size_t numberOfValues, const FormOfValue& formOfValue, Vector<ArgumentLocation, inlineCapacity>& locations)
{
    LocationAllocatorOfConvention allocator;
    locations.reserveInitialCapacity(numberOfValues);
    for (size_t index = 0; index < numberOfValues; ++index)
        locations.append(allocator.allocate(formOfValue(index)));
    return allocator.numberOfStackWords;
}

static ArgumentLocation locationOfOnlyResultOfConvention(Representation representation)
{
    if (isPassedInFPR(representation))
        return { .kind = ArgumentLocation::Kind::FPR, .reg = FPRInfo::returnValueFPR, .representation = representation };
    return { .kind = ArgumentLocation::Kind::GPR, .reg = GPRInfo::returnValueGPR, .representation = representation };
}

template<typename FormOfParameter, typename FormOfResult>
static ALWAYS_INLINE ArgumentLocations locationsOfDirectConvention(size_t numberOfParameters, const FormOfParameter& formOfParameter, size_t numberOfResults, const FormOfResult& formOfResult)
{
    ArgumentLocations locations;
    locations.convention = EntryConvention::Direct;
    locations.numberOfStackArguments = assignLocationsOfConvention(numberOfParameters, formOfParameter, locations.parameters);

    if (!numberOfResults) {
        // Such a call returns undefined. No register or stack word carries it: the caller makes the boxed constant.
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

ArgumentLocations locationsOfDirectSignature(std::span<const Representation> parameters, std::span<const Representation> results)
{
    return locationsOfDirectConvention(parameters.size(), [&](size_t index) {
        return parameters[index];
    }, results.size(), [&](size_t index) {
        return results[index];
    });
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

// An awaited call enters the body of an async function without a promise: undefined stands in its place.
static Representation formOfCoroutineDriver(const Function& coroutine)
{
    return coroutine.has(FunctionFlags::IsGenerator) ? Representation::Cell : Representation::JSValue;
}

static std::optional<size_t> findPlaceOfBoundFunction(std::span<const BoundFunctionPlan> plans, FunctionID target, unsigned numberOfBoundArguments)
{
    auto found = std::ranges::lower_bound(plans, std::pair(target, numberOfBoundArguments), { }, [](const BoundFunctionPlan& plan) {
        return std::pair(plan.target, static_cast<unsigned>(plan.numberOfBoundArguments));
    });
    if (found == plans.end() || found->target != target || found->numberOfBoundArguments != numberOfBoundArguments)
        return std::nullopt;
    return static_cast<size_t>(found - plans.begin());
}

static const BoundFunctionPlan& boundFunctionOfUnit(const CallingConventions& conventions, CodeUnit unit)
{
    const BoundFunctionPlan* plan = conventions.boundFunction(unit.function, unit.numberOfBoundArguments);
    RELEASE_ASSERT(plan);
    return *plan;
}

static unsigned numberOfParametersBehindCapturedArguments(const Program& program, const BoundFunctionPlan& plan)
{
    unsigned numberOfPositionalParameters = program.at(plan.target).numberOfPositionalParameters();
    RELEASE_ASSERT(plan.numberOfCapturedArguments <= numberOfPositionalParameters);
    return numberOfPositionalParameters - plan.numberOfCapturedArguments;
}

// new.target comes first where the entry takes it.
static unsigned numberOfArgumentsTakenInRegistersBy(const CallingConventions& conventions, const Program& program, CodeUnit entry)
{
    unsigned numberOfPositionalParameters = isBoundRole(entry.role)
        ? numberOfParametersBehindCapturedArguments(program, boundFunctionOfUnit(conventions, entry))
        : program.at(entry.function).numberOfPositionalParameters();
    return numberOfPositionalParameters + conventions.genericEntry(entry).takesNewTarget;
}

static unsigned numberOfParametersInConventionOf(const CallingConventions& conventions, const Program& program, CodeUnit unit)
{
    switch (unit.role) {
    case CodeRole::Body:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
        return conventions.of(unit.function).numberOfEntryParameters;
    case CodeRole::CoroutineBody:
        return conventions.of(unit.function).numberOfEntryParameters + 1;
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::GenericEntryOfBoundFunction:
        switch (conventions.entryConventionOf(unit)) {
        case EntryConvention::Direct:
            break;
        case EntryConvention::GenericRegisters:
            return numberOfParametersInFrontOfGenericArguments + numberOfArgumentsTakenInRegistersBy(conventions, program, unit);
        case EntryConvention::GenericBuffer:
            return numberOfParametersInFrontOfGenericArguments;
        }
        break;
    case CodeRole::BoundFunction: {
        const BoundFunctionPlan& plan = boundFunctionOfUnit(conventions, unit);
        return static_cast<unsigned>(plan.captures.size()) + numberOfParametersBehindCapturedArguments(program, plan);
    }
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return 1 + conventions.methodSelector(unit.numberOfBoundArguments).numberOfArguments;
    case CodeRole::Serializer:
        return numberOfSerializerParameters;
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
    case CodeRole::BoundFunction:
    case CodeRole::GenericEntryOfBoundFunction:
        return !!boundFunction(unit.function, unit.numberOfBoundArguments);
    case CodeRole::Serializer:
        return unit && serializerOf(unit.layoutOfSerializer()) == unit;
    case CodeRole::TableEntry:
    case CodeRole::TableLookup: {
        if (unit.numberOfBoundArguments >= methodSelectors().size())
            return false;
        const MethodFamily& family = familyOf(methodSelector(unit.numberOfBoundArguments));
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
    uint32_t firstBoundUnit = numberOfFunctions() * numberOfCodeRolesOfOneFunction;
    switch (unit.role) {
    case CodeRole::BoundFunction:
    case CodeRole::GenericEntryOfBoundFunction: {
        std::optional<size_t> place = findPlaceOfBoundFunction(boundFunctions(), unit.function, unit.numberOfBoundArguments);
        RELEASE_ASSERT(place);
        return firstBoundUnit + static_cast<uint32_t>(*place) * numberOfBoundRoles + (static_cast<unsigned>(unit.role) - numberOfCodeRolesOfOneFunction);
    }
    case CodeRole::TableEntry:
    case CodeRole::TableLookup: {
        const MethodSelector& selector = methodSelector(unit.numberOfBoundArguments);
        uint32_t lookup = firstBoundUnit + static_cast<uint32_t>(m_parts.boundFunctions.size()) * numberOfBoundRoles + selector.firstUnit;
        if (unit.role == CodeRole::TableLookup)
            return lookup;
        const Vector<FunctionID>& methods = familyOf(selector).methods;
        auto found = std::ranges::lower_bound(methods, unit.function);
        RELEASE_ASSERT(found != methods.end() && *found == unit.function);
        return lookup + 1 + static_cast<uint32_t>(found - methods.begin());
    }
    case CodeRole::Serializer:
        RELEASE_ASSERT(exists(unit));
        return firstSerializer() + unit.layoutOfSerializer().index();
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

    uint32_t indexAmongBoundUnits = index - numberOfUnitsOfFunctions;
    uint32_t numberOfBoundUnits = static_cast<uint32_t>(m_parts.boundFunctions.size()) * numberOfBoundRoles;
    if (indexAmongBoundUnits < numberOfBoundUnits) {
        const BoundFunctionPlan& plan = m_parts.boundFunctions[indexAmongBoundUnits / numberOfBoundRoles];
        return { plan.target, static_cast<CodeRole>(numberOfCodeRolesOfOneFunction + indexAmongBoundUnits % numberOfBoundRoles), plan.numberOfBoundArguments };
    }

    // Null: the layout of the place has no serializer.
    if (index >= firstSerializer() && index - firstSerializer() < numberOfPlacesOfSerializers())
        return serializerOf(LayoutID(index - firstSerializer()));

    uint32_t indexAmongTableUnits = indexAmongBoundUnits - numberOfBoundUnits;
    RELEASE_ASSERT(indexAmongTableUnits < m_parts.methodTable.numberOfUnits);
    std::span<const MethodSelector> selectors = methodSelectors();
    const MethodSelector& selector = *std::prev(std::ranges::upper_bound(selectors, indexAmongTableUnits, { }, &MethodSelector::firstUnit));
    if (indexAmongTableUnits == selector.firstUnit)
        return tableLookupOf(selector);
    return { familyOf(selector).methods[indexAmongTableUnits - selector.firstUnit - 1], CodeRole::TableEntry, placeOf(selector) };
}

static Representation formOfParameterProvenToBe(const Program& program, TypeID proven)
{
    if (!proven)
        return Representation::JSValue;
    Representation form = representationOf(program.at(proven));
    return form == Representation::Int32 || form == Representation::Double ? form : Representation::JSValue;
}

Representation CallingConventions::parameterRepresentationByRole(const Program& program, CodeUnit unit, unsigned parameterIndex) const
{
    switch (unit.role) {
    case CodeRole::CoroutineBody:
        if (parameterIndex == of(unit.function).numberOfEntryParameters)
            return formOfCoroutineDriver(program.at(unit.function));
        return formOfEntryParameterInConvention(m_parts, program, unit.function, parameterIndex);
    // What the guard has proven a number comes raw: the guard unboxes it once, and a caller that has it raw hands it over so.
    case CodeRole::Body: {
        Representation form = formOfEntryParameterInConvention(m_parts, program, unit.function, parameterIndex);
        TypeID tested = form == Representation::JSValue && !m_parts.formsOfEntryParameters.isEmpty() ? typeBehindGuard(unit.function, parameterIndex) : TypeID();
        return tested ? formOfParameterProvenToBe(program, tested) : form;
    }
    case CodeRole::ModuleStart:
    case CodeRole::ParameterGuard:
        return formOfEntryParameterInConvention(m_parts, program, unit.function, parameterIndex);
    // It leans on nothing that is proven of a caller. The guard boxes what arrives raw.
    case CodeRole::SlowBody:
        return boxedFormOfEntryParameter(program, program.at(unit.function), parameterIndex);
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::GenericEntryOfBoundFunction:
        RELEASE_ASSERT(parameterIndex < numberOfParametersInConventionOf(*this, program, unit));
        return parameterIndex ? Representation::JSValue : Representation::Cell;
    case CodeRole::BoundFunction: {
        const BoundFunctionPlan& plan = boundFunctionOfUnit(*this, unit);
        if (parameterIndex < plan.captures.size())
            return plan.captures[parameterIndex].representation;
        unsigned indexBehindCapturedArguments = parameterIndex - static_cast<unsigned>(plan.captures.size());
        RELEASE_ASSERT(indexBehindCapturedArguments < numberOfParametersBehindCapturedArguments(program, plan));
        return formOfEntryParameterInConvention(m_parts, program, unit.function, program.at(unit.function).firstParameterIndex() + plan.numberOfCapturedArguments + indexBehindCapturedArguments);
    }
    case CodeRole::Serializer:
        RELEASE_ASSERT(parameterIndex < numberOfSerializerParameters);
        return static_cast<SerializerParameter>(parameterIndex) == SerializerParameter::LevelsAround ? Representation::Int32 : Representation::Cell;
    // The receiver, then the arguments as they are written.
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        RELEASE_ASSERT(parameterIndex < numberOfParametersInConventionOf(*this, program, unit));
        if (const MethodSelector& selector = methodSelector(unit.numberOfBoundArguments); parameterIndex && selector.isTyped)
            return formOfParameterProvenToBe(program, familyOf(selector).typesOfArguments[parameterIndex - 1]);
        return parameterIndex ? Representation::JSValue : Representation::Cell;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ArgumentLocations CallingConventions::locationsOf(const Program& program, CodeUnit unit) const
{
    EntryConvention convention = entryConventionOf(unit);
    switch (convention) {
    case EntryConvention::Direct: {
        auto formOfResult = [&](size_t index) {
            return resultRepresentation(unit, static_cast<unsigned>(index));
        };
        if (takesEntryParametersAsListed(unit.role) && !m_parts.formsOfEntryParameters.isEmpty()) {
            const FunctionConvention& ofFunction = of(unit.function);
            std::span<const Representation> forms = m_parts.formsOfEntryParameters.subspan(ofFunction.firstEntryParameter, ofFunction.numberOfEntryParameters);
            std::span<const TypeID> typesBehindGuard;
            if (unit.role == CodeRole::Body && ofFunction.has(CodeRole::ParameterGuard))
                typesBehindGuard = m_parts.typesBehindGuards.subspan(ofFunction.firstTypeBehindGuard, ofFunction.numberOfEntryParameters);
            return locationsOfDirectConvention(forms.size(), [&](size_t index) {
                TypeID tested = forms[index] == Representation::JSValue && !typesBehindGuard.empty() ? typesBehindGuard[index] : TypeID();
                return tested ? formOfParameterProvenToBe(program, tested) : forms[index];
            }, numberOfResults(unit), formOfResult);
        }
        return locationsOfDirectConvention(numberOfParametersInConventionOf(*this, program, unit), [&](size_t index) {
            return parameterRepresentation(program, unit, static_cast<unsigned>(index));
        }, numberOfResults(unit), formOfResult);
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
    // Whoever calls by value leaves room for the arguments it passes, which may be fewer than the entry reads.
    if (isGenericEntry(callee.role))
        return 0;

    // A serializer goes on to no unit of the function that it is listed under.
    if (callee.role == CodeRole::Serializer)
        return stackWordsTakenInConventionOf(*this, program, callee);

    if (isTableRole(callee.role))
        return stackWordsTakenInConventionOf(*this, program, callee);

    const FunctionConvention& convention = of(callee.function);
    if (callee.role != CodeRole::BoundFunction && std::max<unsigned>(convention.numberOfEntryParameters + 1, convention.numberOfResults) <= numberOfValuesThatNeverNeedStackWords)
        return 0;

    uint32_t words = stackWordsTakenInConventionOf(*this, program, callee);
    for (CodeRole role : { CodeRole::Body, CodeRole::SlowBody, CodeRole::ParameterGuard, CodeRole::CoroutineBody }) {
        if (role != callee.role && convention.has(role))
            words = std::max(words, stackWordsTakenInConventionOf(*this, program, { callee.function, role }));
    }
    return words;
}

GenericEntryPlan CallingConventions::genericEntry(CodeUnit unit) const
{
    const FunctionConvention& convention = of(unit.function);
    GenericEntryPlan plan;
    plan.target = directEntryOf(unit.function);
    switch (unit.role) {
    case CodeRole::GenericCallEntry:
        plan.takesNewTarget = convention.callEntryTakesNewTarget;
        if (convention.has(CodeRole::AllocatingConstructEntry))
            plan.constructEntry = { unit.function, CodeRole::AllocatingConstructEntry };
        else if (convention.has(CodeRole::GenericConstructEntry))
            plan.constructEntry = { unit.function, CodeRole::GenericConstructEntry };
        // Only the run time enters the body of a module by value. Compiled code calls the start and the body directly.
        if (convention.has(CodeRole::ModuleStart))
            plan.target = { unit.function, CodeRole::ModuleStart };
        return plan;
    case CodeRole::GenericConstructEntry:
        plan.takesNewTarget = true;
        return plan;
    case CodeRole::AllocatingConstructEntry:
        // Whoever constructs passes new.target, though here it is the closure.
        plan.takesNewTarget = true;
        plan.allocatesThis = true;
        return plan;
    case CodeRole::GenericEntryOfBoundFunction:
        RELEASE_ASSERT(exists(unit));
        plan.target = { unit.function, CodeRole::BoundFunction, unit.numberOfBoundArguments };
        return plan;
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
    case CodeRole::BoundFunction:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
    case CodeRole::Serializer:
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

// Each condition is one under which BuilderOfEntries::buildGenericEntry() of walk/AOTWalkEntries.cpp builds more than reads of captures and a tail call of the target.
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
    // It has no code: a call of it is a call of the stub StartModule.
    if (target.role == CodeRole::ModuleStart)
        return std::nullopt;
    if (conventions.numberOfResults(target) != 1 || !haveSameRegisterRepresentation(conventions.resultRepresentation(target), conventions.resultRepresentation(entry)))
        return std::nullopt;
    // Whoever calls by value leaves no room for them.
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
            // Only the count tells an argument that was left out from undefined.
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

void CallingConventions::fuseCallEntriesThatOnlyMove(const Program& program, const ParallelLoops& loops)
{
    uint32_t numberOfFunctions = this->numberOfFunctions();
    Vector<uint8_t> isFused(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0));
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        // UnitQueue may close the entry of such a function, and then its target may have no code.
        isFused[index] = !indirectCallersAreAllListed(FunctionID(index)) && planCallEntryMoves(program, *this, FunctionID(index));
    });
    m_functionsWithFusedCallEntry = BitVector(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (isFused[index])
            m_functionsWithFusedCallEntry.quickSet(index);
    }
}

std::optional<unsigned> targetEntryParameterOf(const Function& target, const BoundCapture& capture)
{
    switch (capture.kind) {
    case BoundCaptureKind::TargetCapture:
        RELEASE_ASSERT(capture.index < target.captures.size());
        return capture.index;
    case BoundCaptureKind::BoundThis:
        RELEASE_ASSERT(target.thisParameterIndex());
        return target.thisParameterIndex();
    case BoundCaptureKind::BoundArgument:
        RELEASE_ASSERT(capture.index < target.numberOfPositionalParameters());
        return target.firstParameterIndex() + capture.index;
    case BoundCaptureKind::TargetClosure:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const BoundFunctionPlan* CallingConventions::boundFunction(FunctionID target, unsigned numberOfBoundArguments) const
{
    std::optional<size_t> place = findPlaceOfBoundFunction(boundFunctions(), target, numberOfBoundArguments);
    return place ? &m_parts.boundFunctions[*place] : nullptr;
}

static bool isRunTimeCodeForConventions(const Program& program, const Function& function)
{
    if (function.role == FunctionRole::Library || function.enclosingRuntimeFunction)
        return true;
    return function.module && program.at(function.module).kind == ModuleKind::RuntimeSource;
}

static bool constructEntryAllocatesThisOf(const Program& program, const Function& function, const BitVector& layoutsMadeAsCompiledObjects)
{
    if (function.kind != FunctionKind::Plain || function.isCoroutine() || !function.has(FunctionFlags::HasThis) || function.results.size() != 1 || !function.constructedClass)
        return false;
    // The run time constructs the constructor functions of its own modules by name.
    if (isRunTimeCodeForConventions(program, function))
        return false;
    LayoutID declared = declaredLayoutOf(program, function.constructedClass);
    return declared && program.at(declared).has(LayoutFlags::IsConstructorFunction) && layoutsMadeAsCompiledObjects.get(function.constructedClass.index());
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

static void chooseRolesOfConvention(const Program& program, const Facts& facts, FunctionID id, const BitVector& bodiesWithModuleStart, const BitVector& layoutsMadeAsCompiledObjects, FunctionConvention& convention)
{
    // A model describes native code, and a signature has no body.
    const Function& function = program.at(id);
    // The walk writes a parser out of the type that it is made for. Compiled code alone enters it, directly.
    if (function.role == FunctionRole::Parser) {
        if (facts.mayBeReached(id))
            convention.roles |= bitOfRoleInConvention(CodeRole::Body);
        return;
    }
    if (!function.hasBody() || !hasSourceBodyOf(function.role) || !canBeCalledByValueOf(function.role))
        return;

    auto add = [&](CodeRole role) {
        convention.roles |= bitOfRoleInConvention(role);
    };

    // The word of every closure names this unit, and Image numbers the functions that closures are made of by it. A closure of a function that is never entered may still be made.
    add(CodeRole::GenericCallEntry);
    convention.takesArgumentsInBuffer = function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount });

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
    }
    if (function.isModuleBody() && bodiesWithModuleStart.get(id.index()))
        add(CodeRole::ModuleStart);
}

// An integer past int32 is returned as the double that it is.
static Representation formOfResultProvenToBe(const Program& program, TypeID proven)
{
    if (!proven)
        return Representation::JSValue;
    switch (representationOf(program.at(proven))) {
    case Representation::Boolean:
        return Representation::Boolean;
    case Representation::Int32:
        return Representation::Int32;
    case Representation::Int64:
    case Representation::Double:
        return Representation::Double;
    default:
        return Representation::JSValue;
    }
}

// A generic entry hands such a parameter the marker for what the caller left out, which is no number.
static bool tellsAbsentFromUndefinedInConvention(const Program& program, const DeclaredType& parameter)
{
    TypeID written = parameter.written();
    return written && (program.at(written).primitives & Primitive::Absent);
}

static void appendFormsOfEntryParameters(const Program& program, const Facts& facts, FunctionID id, bool mayTakeUnboxedNumbers, Vector<Representation>& forms)
{
    const Function& function = program.at(id);
    unsigned numberOfEntryParameters = function.numberOfEntryParameters();
    unsigned firstPositionalParameter = function.firstParameterIndex();
    unsigned numberOfPositionalParameters = function.numberOfPositionalParameters();
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        Representation form = boxedFormOfEntryParameter(program, function, index);
        bool isPositional = index >= firstPositionalParameter && index - firstPositionalParameter < numberOfPositionalParameters;
        if (mayTakeUnboxedNumbers && isPositional && form == Representation::JSValue && !tellsAbsentFromUndefinedInConvention(program, function.parameters[index - firstPositionalParameter]))
            form = formOfParameterProvenToBe(program, facts.typeOfEntryParameter(id, index).proven);
        forms.append(form);
    }
}

// A call in tail position out of a function that returns boxed values is a jump where the callee returns boxed values too, and else comes back to box what it got.
// So a function whose result nobody else takes returns a boxed value. That may leave the functions in which it ends itself with nobody either.
static void boxResultsThatNobodyTakesRaw(const LinkTables& linked, BitVector& functionsThatReturnRawValues, CallingConventionParts& parts)
{
    auto returnsRawValue = [&](FunctionID id) {
        return functionsThatReturnRawValues.get(id.index());
    };

    Vector<uint32_t> numberOfTakers(FillWith { }, parts.conventions.size(), 0u);
    for (const LinkedCall& call : linked.calls) {
        if (returnsRawValue(call.callee) && (!call.isInTailPosition || returnsRawValue(call.caller)))
            ++numberOfTakers[call.callee.index()];
    }
    Vector<FunctionID> functionsWithoutTakers;
    for (size_t index : functionsThatReturnRawValues) {
        if (!numberOfTakers[index])
            functionsWithoutTakers.append(FunctionID(static_cast<uint32_t>(index)));
    }
    while (!functionsWithoutTakers.isEmpty()) {
        FunctionID boxed = functionsWithoutTakers.takeLast();
        functionsThatReturnRawValues.clear(boxed.index());
        parts.formsOfResults[parts.conventions[boxed.index()].firstResult] = Representation::JSValue;
        for (const LinkedCall& call : std::ranges::equal_range(linked.calls, boxed, { }, &LinkedCall::caller)) {
            if (call.isInTailPosition && returnsRawValue(call.callee) && !--numberOfTakers[call.callee.index()])
                functionsWithoutTakers.append(call.callee);
        }
    }
}

CallingConventions computeCallingConventions(const Program& program, const Facts& facts, const BitVector& bodiesWithModuleStart, const BitVector& layoutsMadeAsCompiledObjects)
{
    // The body takes what Link has proven of a parameter through Arrivals alone. Without it the parameter holds anything there, which no raw form allows.
    bool unboxesNumbers = facts.mechanisms().containsAll({ Mechanism::UnboxedNumbers, Mechanism::Arrivals });
    const LinkTables& linked = facts.linked();
    // Whoever calls the Body takes the result as the convention says, and every entry by value comes back for it and boxes it. So a raw result pays at a direct call and costs at a call by value:
    // a function returns one where Link lists every call that may enter it.
    bool returnsRawValues = unboxesNumbers && linked.functions.size() == program.functions.size();
    uint32_t numberOfFunctions = safeCast<uint32_t>(program.functions.size());

    CallingConventionParts parts;
    parts.conventions.fill(FunctionConvention { }, numberOfFunctions);
    BitVector functionsThatReturnRawValues(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        FunctionConvention& convention = parts.conventions[index];
        convention.numberOfEntryParameters = safeCast<uint16_t>(function.numberOfEntryParameters());
        convention.numberOfResults = safeCast<uint16_t>(function.results.size());
        convention.firstResult = safeCast<uint32_t>(parts.formsOfResults.size());
        chooseRolesOfConvention(program, facts, id, bodiesWithModuleStart, layoutsMadeAsCompiledObjects, convention);

        for (const DeclaredType& result : function.results)
            parts.formsOfResults.append(function.role == FunctionRole::Parser ? representationOf(program.at(program.provenTypeOf(result))) : boxedFormOfDeclaredPosition(program, result));
        if (returnsRawValues && convention.has(CodeRole::Body) && mayReturnInFormOfItsOwn(function) && parts.formsOfResults.last() == Representation::JSValue) {
            const LinkedFunction& proven = linked.function(id);
            if (proven.everyCallHasOneCallee && proven.ringsOfTailCallsAreListed)
                parts.formsOfResults.last() = formOfResultProvenToBe(program, proven.result);
            if (parts.formsOfResults.last() != Representation::JSValue)
                functionsThatReturnRawValues.quickSet(index);
        }
        if (!unboxesNumbers)
            continue;
        // What a generic entry reads out of the buffer has no proven type, and the ramp of a coroutine takes its parameters as the record declares them.
        bool mayTakeUnboxedNumbers = convention.has(CodeRole::Body) && facts.everyCallerIsKnown(id) && !convention.takesArgumentsInBuffer && !function.isCoroutine();
        convention.firstEntryParameter = safeCast<uint32_t>(parts.formsOfEntryParameters.size());
        appendFormsOfEntryParameters(program, facts, id, mayTakeUnboxedNumbers, parts.formsOfEntryParameters);
    }
    boxResultsThatNobodyTakesRaw(linked, functionsThatReturnRawValues, parts);
    planSerializers(parts, program, facts);
    return CallingConventions(WTF::move(parts));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

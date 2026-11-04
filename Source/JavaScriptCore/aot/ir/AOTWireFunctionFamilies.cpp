/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWireTypesInternal.h"

#if ENABLE(AOT)

#include "AOTBodyVerifier.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTObjects.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

bool isSignature(const Wire::Function& function)
{
    return function.role == static_cast<uint32_t>(FunctionRole::Signature);
}

static bool functionListLiesInWords(const Wire::Sections& sections, Wire::Span list)
{
    return list.start <= sections.words.size() && list.count <= sections.words.size() - list.start;
}

static std::span<const uint32_t> wordsOfFunctionList(const Wire::Sections& checkedSections, Wire::Span list)
{
    return checkedSections.words.subspan(list.start, list.count);
}

static std::optional<InternalErrorKind> findErrorInFamilyListsOfFunction(const Wire::Sections& sections, const BuiltinTable& builtins, const Wire::Function& function)
{
    std::span<const Wire::Function> functions = sections.functions;
    if (!functionListLiesInWords(sections, function.functionTypes) || !functionListLiesInWords(sections, function.builtinMembers))
        return InternalErrorKind::ListOutOfBounds;
    for (uint32_t functionType : wordsOfFunctionList(sections, function.functionTypes)) {
        if (functionType >= functions.size() || !isSignature(functions[functionType]))
            return InternalErrorKind::IdOutOfRange;
    }
    if (function.builtinMembers.count && !isSignature(function))
        return InternalErrorKind::IdOutOfRange;
    for (uint32_t member : wordsOfFunctionList(sections, function.builtinMembers)) {
        if (!builtins.contains(BuiltinID(member)) || !isCallableBuiltin(builtins, builtins.canonicalID(BuiltinID(member))))
            return InternalErrorKind::IdOutOfRange;
    }
    return std::nullopt;
}

// The error of the first function that has one.
static std::optional<InternalError> findFunctionWithInvalidFamilyLists(const ParallelLoops& loops, const Wire::Sections& sections, const BuiltinTable& builtins)
{
    Vector<InternalError> errors = collectInParallel<InternalError>(loops, static_cast<unsigned>(sections.functions.size()), [&](unsigned index, Vector<InternalError>& found) {
        if (auto kind = findErrorInFamilyListsOfFunction(sections, builtins, sections.functions[index]))
            found.append(InternalError { .kind = *kind, .function = FunctionID(index), .count = index });
    });
    if (errors.isEmpty())
        return std::nullopt;
    return WTF::move(errors.first());
}

// The types are read after the families, so nobody has checked their lists yet, and BitVector::quickSet() does not check its index.
static BitVector findFunctionsNamedByWireTypes(const ParallelLoops& loops, const Wire::Sections& sections)
{
    Vector<uint32_t> named = collectInParallel<uint32_t>(loops, static_cast<unsigned>(sections.types.size()), [&](unsigned type, Vector<uint32_t>& found) {
        Wire::Span list = sections.types[type].functions;
        if (functionListLiesInWords(sections, list))
            found.append(wordsOfFunctionList(sections, list));
    });

    size_t numberOfFunctions = sections.functions.size();
    BitVector result(numberOfFunctions);
    for (uint32_t function : named) {
        if (function < numberOfFunctions)
            result.quickSet(function);
    }
    return result;
}

// Visits the function itself and every function type that it converts to, in any number of steps. Each is visited once, so the walk ends even if function types form a cycle.
// Most walks visit a few, and a short list of those is searched sooner than a thread lays out a word for each function of the program.
// `stamps` is empty until a walk outgrows the list. From then on it has such a word. No two walks over the same stamps start at the same function.
static void forEachFunctionTypeAtOrAbove(const Wire::Sections& checkedSections, uint32_t start, Vector<uint32_t>& stamps, NOESCAPE const Invocable<void(uint32_t)> auto& visit)
{
    constexpr size_t mostVisitedInList = 16;
    uint32_t stamp = start + 1;
    Vector<uint32_t, mostVisitedInList> visited { start };
    bool goesByStamps = false;
    auto isVisitedForFirstTime = [&](uint32_t function) {
        if (goesByStamps)
            return std::exchange(stamps[function], stamp) != stamp;
        if (visited.contains(function))
            return false;
        if (visited.size() < mostVisitedInList) {
            visited.append(function);
            return true;
        }
        if (stamps.isEmpty())
            stamps.fill(0u, checkedSections.functions.size());
        for (uint32_t earlier : visited)
            stamps[earlier] = stamp;
        stamps[function] = stamp;
        goesByStamps = true;
        return true;
    };

    Vector<uint32_t, 16> reached { start };
    while (!reached.isEmpty()) {
        uint32_t function = reached.takeLast();
        visit(function);
        for (uint32_t functionType : wordsOfFunctionList(checkedSections, checkedSections.functions[function].functionTypes)) {
            if (isVisitedForFirstTime(functionType))
                reached.append(functionType);
        }
    }
}

static void addBuiltinMembersToFunctionFamilies(const ParallelLoops& loops, const Wire::Sections& checkedSections, const BuiltinTable& builtins, const BitVector& isNamedByType, Vector<FunctionFamily>& families)
{
    std::span<const Wire::Function> functions = checkedSections.functions;
    // Few have any, and the records of all are tens of megabytes.
    Vector<uint32_t> functionsWithBuiltinMembers = findIndicesInParallel(loops, static_cast<unsigned>(functions.size()), [&](unsigned index) {
        return !!functions[index].builtinMembers.count;
    });
    Vector<uint32_t> stamps;
    Vector<uint32_t> familiesWithBuiltins;
    Vector<BuiltinID, 8> canonicalMembers;
    for (uint32_t index : functionsWithBuiltinMembers) {
        canonicalMembers.shrink(0);
        for (uint32_t member : wordsOfFunctionList(checkedSections, functions[index].builtinMembers))
            canonicalMembers.append(builtins.canonicalID(BuiltinID(member)));
        forEachFunctionTypeAtOrAbove(checkedSections, index, stamps, [&](uint32_t reached) {
            if (!isNamedByType.quickGet(reached))
                return;
            Vector<BuiltinID>& members = families[reached].builtins;
            if (members.isEmpty())
                familiesWithBuiltins.append(reached);
            members.append(canonicalMembers.span());
        });
    }

    for (uint32_t family : familiesWithBuiltins) {
        Vector<BuiltinID>& members = families[family].builtins;
        AOT::sort(members);
        removeRepeatedElements(members);
    }
}

// Both lists of a family are in ascending order without repeats. The family of a function that no type names stays empty, because nobody asks for it.
Vector<FunctionFamily> collectFunctionFamilies(const ParallelLoops& loops, const Wire::Sections& checkedSections, const BuiltinTable& builtins)
{
    constexpr uint32_t functionsPerTask = 512;
    std::span<const Wire::Function> functions = checkedSections.functions;
    uint32_t numberOfFunctions = static_cast<uint32_t>(functions.size());
    unsigned numberOfTasks = (numberOfFunctions + functionsPerTask - 1) / functionsPerTask;
    BitVector isNamedByType = findFunctionsNamedByWireTypes(loops, checkedSections);

    Vector<Vector<uint32_t>> stampsOfWorkers(loops.width());
    Vector<Vector<uint64_t>> membersByTask(numberOfTasks);
    forEachIndexInParallel(loops, numberOfTasks, [&](unsigned task, unsigned worker) {
        Vector<uint32_t>& stamps = stampsOfWorkers[worker];
        Vector<uint64_t>& members = membersByTask[task];
        for (uint32_t index = task * functionsPerTask, end = std::min(numberOfFunctions, index + functionsPerTask); index < end; ++index) {
            if (isSignature(functions[index]))
                continue;
            if (!functions[index].functionTypes.count) {
                if (isNamedByType.quickGet(index))
                    members.append(pairOfWireIndexes(index, index));
                continue;
            }
            forEachFunctionTypeAtOrAbove(checkedSections, index, stamps, [&](uint32_t reached) {
                if (isNamedByType.quickGet(reached))
                    members.append(pairOfWireIndexes(reached, index));
            });
        }
    });

    Vector<Vector<FunctionID>> functionsOfFamilies(numberOfFunctions);
    listSecondsByFirst(loops, membersByTask.span(), functionsOfFamilies);

    Vector<FunctionFamily> families(numberOfFunctions);
    forEachRangeOfIndicesInParallel(loops, numberOfFunctions, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            families[index].functions = WTF::move(functionsOfFamilies[index]);
    });
    addBuiltinMembersToFunctionFamilies(loops, checkedSections, builtins, isNamedByType, families);
    return families;
}

static ObjectSetID internFunctionFamily(TypeArena& arena, const FunctionFamily& family, Vector<ObjectID>& members)
{
    if (family.functions.isEmpty() && family.builtins.isEmpty())
        return { };

    const ObjectTable& objects = arena.objects();
    members.shrink(0);
    for (BuiltinID builtin : family.builtins)
        members.append(objects.objectOf(builtin));
    for (FunctionID function : family.functions)
        members.append(objects.closureOf(function));
    // Neither closures nor the objects of built-ins are numbered in the order of what they stand for.
    return arena.objectSets().ofUnsorted(members.span());
}

// An empty family gets the null id, which readPartsWithoutClasses() treats as any function.
// It runs beside the families of layouts, whose errors come first: it reports nothing itself.
void WireTypeReader::computeFunctionFamilies()
{
    const BuiltinTable& builtins = m_program.builtins();
    // The reader's line of the families counts all of these.
    StepTimes steps(m_build, "ReadAndVerify: families of functions"_s);
    if (std::optional<InternalError> error = findFunctionWithInvalidFamilyLists(m_build.parallelLoops(), m_sections, builtins)) {
        m_errorInFunctionFamilies = WTF::move(error);
        m_functionFamilies.fill(ObjectSetID(), m_sections.functions.size());
        return;
    }
    steps.didFinish("lists are valid"_s);

    Vector<FunctionFamily> families = collectFunctionFamilies(m_build.parallelLoops(), m_sections, builtins);
    steps.didFinish("collect families"_s);

    // A type that names a family holds this id, so a family of many functions is stored once however many types name it.
    // The id of a set follows arrival, here as where readTypes() unites these sets: it is only compared for equality.
    constexpr size_t familiesPerTask = 256;
    const ParallelLoops& loops = m_build.parallelLoops();
    ArenasOfWorkers arenas(m_program.types, loops);
    m_functionFamilies.grow(families.size());
    forEachIndexInParallel(loops, static_cast<unsigned>((families.size() + familiesPerTask - 1) / familiesPerTask), [&](unsigned task, unsigned worker) {
        TypeArena& arena = arenas.of(worker);
        Vector<ObjectID> members;
        for (size_t index = task * familiesPerTask, end = std::min(families.size(), index + familiesPerTask); index < end; ++index)
            m_functionFamilies[index] = internFunctionFamily(arena, families[index], members);
    });
    steps.didFinish("intern the families"_s);

    families = { };
    steps.didFinish("free the families"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

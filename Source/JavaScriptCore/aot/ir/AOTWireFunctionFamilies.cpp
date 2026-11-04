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

// By function of the wire: the closures and the built-ins that may pass for it: itself, and whatever converts to it in any number of steps. Null: nobody, or no type names the function, so nobody asks.
// What passes for a function type passes for every function type that it converts to. So A FAMILY IS MADE OF THE FAMILIES OF ALL THAT CONVERTS TO IT, each behind all of those, by one union:
// a walk upward from every function takes a step for each pair where function types convert to one another in a row, as those of the methods of one name do in a row of classes.
static Vector<ObjectSetID> collectFunctionFamilies(const ParallelLoops& loops, const Wire::Sections& checkedSections, const BuiltinTable& builtins, TypeArena& arena)
{
    std::span<const Wire::Function> functions = checkedSections.functions;
    uint32_t numberOfFunctions = static_cast<uint32_t>(functions.size());
    const ObjectTable& objects = arena.objects();
    ObjectSets& sets = arena.objectSets();

    // By function type: what converts to it in one step. Only a signature is a function type, so only a signature has a family to wait for.
    Vector<Vector<uint64_t>> sourcesOfTypes(1);
    Vector<uint32_t> numberOfFamiliesLeft(FillWith { }, numberOfFunctions, 0u);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        for (uint32_t functionType : wordsOfFunctionList(checkedSections, functions[index].functionTypes)) {
            sourcesOfTypes[0].append(pairOfWireIndexes(functionType, index));
            numberOfFamiliesLeft[functionType] += isSignature(functions[index]);
        }
    }
    Vector<Vector<uint32_t>> sources(numberOfFunctions);
    listSecondsByFirst(loops, sourcesOfTypes.span(), sources);

    Vector<ObjectSetID> families(numberOfFunctions);
    Vector<ObjectSetID, 8> wholeFamilies;
    Vector<ObjectID, 8> membersByThemselves;
    auto makeFamilyOfSignature = [&](uint32_t index) {
        wholeFamilies.shrink(0);
        membersByThemselves.shrink(0);
        for (uint32_t member : wordsOfFunctionList(checkedSections, functions[index].builtinMembers))
            membersByThemselves.append(objects.objectOf(builtins.canonicalID(BuiltinID(member))));
        for (uint32_t source : sources[index]) {
            if (isSignature(functions[source]))
                wholeFamilies.append(families[source]);
            else
                membersByThemselves.append(objects.closureOf(FunctionID(source)));
        }
        // Neither closures nor the objects of built-ins are numbered in the order of what they stand for.
        wholeFamilies.append(sets.ofUnsorted(membersByThemselves.span()));
        return sets.unionOf(wholeFamilies.span());
    };

    Vector<uint32_t> ready;
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (isSignature(functions[index]) && !numberOfFamiliesLeft[index])
            ready.append(index);
    }
    while (!ready.isEmpty()) {
        uint32_t index = ready.takeLast();
        families[index] = makeFamilyOfSignature(index);
        for (uint32_t functionType : wordsOfFunctionList(checkedSections, functions[index].functionTypes)) {
            if (!--numberOfFamiliesLeft[functionType])
                ready.append(functionType);
        }
    }
    // Function types may convert to one another in a ring. What stands in one, or above one, is made, and made again whenever somebody has joined what converts to it.
    Vector<uint32_t> waiting;
    BitVector isWaiting(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (numberOfFamiliesLeft[index]) {
            waiting.append(index);
            isWaiting.quickSet(index);
        }
    }
    for (size_t place = 0; place < waiting.size(); ++place) {
        uint32_t index = waiting[place];
        isWaiting.quickClear(index);
        ObjectSetID family = makeFamilyOfSignature(index);
        if (std::exchange(families[index], family) == family)
            continue;
        for (uint32_t functionType : wordsOfFunctionList(checkedSections, functions[index].functionTypes)) {
            if (!isWaiting.quickSet(functionType))
                waiting.append(functionType);
        }
    }

    BitVector isNamedByType = findFunctionsNamedByWireTypes(loops, checkedSections);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (!isNamedByType.quickGet(index))
            families[index] = { };
        else if (!isSignature(functions[index]))
            families[index] = ObjectSets::of(objects.closureOf(FunctionID(index)));
    }
    return families;
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

    // A type that names a family holds this id, so a family of many functions is stored once however many types name it.
    // The id of a set follows arrival, here as where readTypes() unites these sets: it is only compared for equality.
    TypeArena arena(m_program.types, ArenaKeeps::InProgram);
    m_functionFamilies = collectFunctionFamilies(m_build.parallelLoops(), m_sections, builtins, arena);
    steps.didFinish("collect families"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

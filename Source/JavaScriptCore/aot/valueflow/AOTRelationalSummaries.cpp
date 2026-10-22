/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelationalSummaries.h"

#if ENABLE(AOT)

#include "AOTIRPrinter.h"
#include <algorithm>
#include <wtf/PrintStream.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RelationalSummaries);

bool KeysOfMembers::add(PropertyKeyID key)
{
    if (mayBeAnyKey)
        return false;
    auto position = std::ranges::lower_bound(keys, key);
    if (position != keys.end() && *position == key)
        return false;
    if (keys.size() == mostKeys)
        *this = any();
    else
        keys.insert(position - keys.begin(), key);
    return true;
}

bool KeysOfMembers::unite(const KeysOfMembers& other)
{
    if (mayBeAnyKey)
        return false;
    if (other.mayBeAnyKey) {
        *this = any();
        return true;
    }
    bool hasChanged = false;
    for (PropertyKeyID key : other.keys)
        hasChanged |= add(key);
    return hasChanged;
}

bool LinkedFate::unite(const LinkedFate& other)
{
    bool hasChanged = !itself.containsAll(other.itself) || !contents.containsAll(other.contents) || (other.results & ~results) || (other.storedIntoParameters & ~storedIntoParameters)
        || (other.mayBeStoredDeeper && !mayBeStoredDeeper) || (other.storedIntoResults & ~storedIntoResults);
    itself.add(other.itself);
    contents.add(other.contents);
    results |= other.results;
    storedIntoParameters |= other.storedIntoParameters;
    mayBeStoredDeeper |= other.mayBeStoredDeeper;
    storedIntoResults |= other.storedIntoResults;
    hasChanged |= keysRead.unite(other.keysRead);
    hasChanged |= keysWritten.unite(other.keysWritten);
    return hasChanged;
}

RelationalSummaries::RelationalSummaries() = default;
RelationalSummaries::~RelationalSummaries() = default;

static thread_local uint32_t t_levelOfAsker;
static thread_local uint32_t t_componentOfAsker;

void RelationalSummaries::setAsker(uint32_t level, uint32_t component)
{
    t_levelOfAsker = level;
    t_componentOfAsker = component;
}

bool RelationalSummaries::isKnownToAsker(FunctionID function) const
{
    if (!function || function.index() >= m_functions.size())
        return false;
    if (!m_orderWhileLinking) [[likely]]
        return true;
    return m_orderWhileLinking->levelOf[function.index()] < t_levelOfAsker || m_orderWhileLinking->componentOf[function.index()] == t_componentOfAsker;
}

const LinkedFate& RelationalSummaries::fateOfParameter(FunctionID function, uint32_t entryParameter) const
{
    if (!isKnownToAsker(function) || entryParameter >= m_functions[function.index()].parameters.size())
        return m_anything;
    return m_functions[function.index()].parameters[entryParameter];
}

const LinkedFate& RelationalSummaries::fateApartFromCalledParameters(FunctionID function, uint32_t entryParameter) const
{
    if (!isKnownToAsker(function) || m_functions[function.index()].parametersApartFromCalledParameters.isEmpty())
        return fateOfParameter(function, entryParameter);
    if (entryParameter >= m_functions[function.index()].parametersApartFromCalledParameters.size())
        return m_anything;
    return m_functions[function.index()].parametersApartFromCalledParameters[entryParameter];
}

std::span<const HandedToCalledParameter> RelationalSummaries::handedToCalledParameters(FunctionID function) const
{
    if (!isKnownToAsker(function))
        return { };
    return m_functions[function.index()].handedToCalledParameters.span();
}

const ResultOrigin& RelationalSummaries::originOfResult(FunctionID function, uint32_t result) const
{
    if (!isKnownToAsker(function) || result >= m_functions[function.index()].results.size())
        return m_unknownOrigin;
    return m_functions[function.index()].results[result];
}

// Bit i of the mask speaks of entry parameter i. Parameters from 32 on have no bit, and whoever sets a mask says "stored in an unknown object" for them.
static uint32_t withoutRemovedBits(uint32_t mask, const BitVector& removed, bool& hasLostBit)
{
    uint32_t result = 0;
    unsigned next = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (removed.get(bit)) {
            hasLostBit |= mask >> bit & 1;
            continue;
        }
        result |= (mask >> bit & 1) << next++;
    }
    return result;
}

void RelationalSummaries::removeEntryParameters(FunctionID function, const BitVector& removedEntryParameters)
{
    if (!function || function.index() >= m_functions.size())
        return;
    OfFunction& summary = m_functions[function.index()];
    Vector<uint32_t> newIndexes(summary.numberOfEntryParameters);
    uint32_t numberOfRemainingParameters = 0;
    for (uint32_t parameter = 0; parameter < summary.numberOfEntryParameters; ++parameter) {
        newIndexes[parameter] = numberOfRemainingParameters;
        numberOfRemainingParameters += !removedEntryParameters.get(parameter);
    }
    auto removeFrom = [&](Vector<LinkedFate>& fates) {
        if (fates.isEmpty())
            return;
        RELEASE_ASSERT(fates.size() == newIndexes.size());
        for (uint32_t parameter = 0; parameter < fates.size(); ++parameter) {
            if (removedEntryParameters.get(parameter))
                continue;
            LinkedFate fate = WTF::move(fates[parameter]);
            if (!fate.isAnything()) {
                // What it was stored into is no longer handed in, so nobody can say where that is.
                bool hasLostBit = false;
                fate.storedIntoParameters = withoutRemovedBits(fate.storedIntoParameters, removedEntryParameters, hasLostBit);
                if (hasLostBit)
                    fate.itself.add(ValueFate::IsStoredInUnknownObject);
            }
            fates[newIndexes[parameter]] = WTF::move(fate);
        }
        fates.shrink(numberOfRemainingParameters);
    };
    // A value that is called and no longer handed in is whatever the function finds in its place: what it was passed counts as handed to unknown code, as fateOfParameter() has it.
    for (const HandedToCalledParameter& row : summary.handedToCalledParameters) {
        if (removedEntryParameters.get(row.called) && row.handed < summary.parametersApartFromCalledParameters.size())
            summary.parametersApartFromCalledParameters[row.handed] = summary.parameters[row.handed];
    }
    summary.handedToCalledParameters.removeAllMatching([&](const HandedToCalledParameter& row) {
        return removedEntryParameters.get(row.called) || removedEntryParameters.get(row.handed);
    });
    for (HandedToCalledParameter& row : summary.handedToCalledParameters) {
        row.called = newIndexes[row.called];
        row.handed = newIndexes[row.handed];
    }
    removeFrom(summary.parameters);
    summary.numberOfEntryParameters = numberOfRemainingParameters;
    removeFrom(summary.parametersApartFromCalledParameters);

    for (ResultOrigin& result : summary.results) {
        bool namesRemovedParameter = std::ranges::any_of(result.origins, [&](const LinkedOrigin& origin) {
            return origin.kind == LinkedOrigin::Kind::Parameter && (origin.parameter >= newIndexes.size() || removedEntryParameters.get(origin.parameter));
        });
        if (namesRemovedParameter) {
            result = { };
            continue;
        }
        for (LinkedOrigin& origin : result.origins) {
            if (origin.kind == LinkedOrigin::Kind::Parameter)
                origin.parameter = newIndexes[origin.parameter];
        }
    }
}

static void printFates(PrintStream& out, OptionSet<ValueFate> fates)
{
    static constexpr std::pair<ValueFate, ASCIILiteral> names[] {
        { ValueFate::IsReturned, "returned"_s }, { ValueFate::IsThrown, "thrown"_s }, { ValueFate::IsYielded, "yielded"_s }, { ValueFate::IsStoredInVariable, "stored-in-variable"_s },
        { ValueFate::IsStoredInEnvironment, "stored-in-environment"_s }, { ValueFate::IsStoredInUnknownObject, "stored-in-unknown-object"_s }, { ValueFate::IsHandedToUnknownCode, "handed-to-unknown-code"_s },
        { ValueFate::IsCalled, "called"_s }, { ValueFate::IsConstructed, "constructed"_s }, { ValueFate::HasMembersRead, "members-read"_s }, { ValueFate::HasMembersWritten, "members-written"_s },
        { ValueFate::HasMembersDeletedOrDefined, "members-deleted-or-defined"_s }, { ValueFate::HasKeysEnumerated, "keys-enumerated"_s }, { ValueFate::IsIterated, "iterated"_s }, { ValueFate::IsCompared, "compared"_s },
        { ValueFate::IsConvertedToPrimitive, "converted-to-primitive"_s },
    };
    if (fates.contains(ValueFate::IsIncomplete)) {
        out.print("anything"_s);
        return;
    }
    if (fates.isEmpty()) {
        out.print("nothing"_s);
        return;
    }
    CommaPrinter comma(" "_s);
    for (auto [fate, name] : names) {
        if (fates.contains(fate))
            out.print(comma, name);
    }
}

static void printKeys(PrintStream& out, const Program& program, ASCIILiteral label, const KeysOfMembers& keys)
{
    if (keys.isEmpty())
        return;
    out.print(" "_s, label, "=["_s);
    if (keys.mayBeAnyKey)
        out.print("any"_s);
    CommaPrinter comma(", "_s);
    for (PropertyKeyID key : keys.keys) {
        out.print(comma);
        printPropertyKey(out, program, key);
    }
    out.print("]"_s);
}

static void printFate(PrintStream& out, const Program& program, const LinkedFate& fate)
{
    printFates(out, fate.itself);
    if (fate.isAnything())
        return;
    out.print(" | contents: "_s);
    printFates(out, fate.contents);
    if (fate.results)
        out.print(" results="_s, fate.results);
    if (fate.storedIntoParameters)
        out.print(" stored-into-parameters="_s, fate.storedIntoParameters, fate.mayBeStoredDeeper ? " or-deeper"_s : ""_s);
    if (fate.storedIntoResults)
        out.print(" stored-into-results="_s, fate.storedIntoResults);
    printKeys(out, program, "read"_s, fate.keysRead);
    printKeys(out, program, "written"_s, fate.keysWritten);
}

static void printPlace(PrintStream& out, PlaceAtCall place)
{
    switch (place.kind) {
    case PlaceAtCall::Kind::Callee:
        out.print("callee"_s);
        break;
    case PlaceAtCall::Kind::This:
        out.print("this"_s);
        break;
    case PlaceAtCall::Kind::NewTarget:
        out.print("new-target"_s);
        break;
    case PlaceAtCall::Kind::Argument:
        out.print("argument"_s, place.argument);
        break;
    case PlaceAtCall::Kind::ElementOfList:
        out.print("element-of-list"_s);
        break;
    }
    if (place.isConstruction)
        out.print(" of a construction"_s);
}

void RelationalSummaries::dump(PrintStream& out, const Program& program) const
{
    for (uint32_t index = 0; index < m_functions.size(); ++index) {
        const OfFunction& summary = m_functions[index];
        if (summary.parameters.isEmpty() && summary.results.isEmpty())
            continue;
        out.println("f"_s, index);
        for (uint32_t parameter = 0; parameter < summary.parameters.size(); ++parameter) {
            out.print("  p"_s, parameter, ": "_s);
            printFate(out, program, summary.parameters[parameter]);
            out.println();
            if (summary.parametersApartFromCalledParameters.isEmpty() || summary.parametersApartFromCalledParameters[parameter] == summary.parameters[parameter])
                continue;
            out.print("  p"_s, parameter, " apart from called parameters: "_s);
            printFate(out, program, summary.parametersApartFromCalledParameters[parameter]);
            out.println();
        }
        for (const HandedToCalledParameter& row : summary.handedToCalledParameters) {
            out.print("  calls p"_s, row.called, " with "_s, row.isReadThroughHanded ? "what is read through "_s : ""_s, "p"_s, row.handed, " as "_s);
            printPlace(out, row.place);
            out.print(" at f"_s, row.function.index(), " v"_s, row.call.index(), ", and of what that answers becomes: "_s);
            printFates(out, row.resultOfCall);
            out.println();
        }
        for (uint32_t result = 0; result < summary.results.size(); ++result) {
            const ResultOrigin& origin = summary.results[result];
            out.println("  result"_s, result, ": "_s, origin);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrivals.h"

#if ENABLE(AOT)

#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTWalk.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static const LinkTables& linkTablesOfArrivals(const Facts& facts)
{
    return facts.linked();
}

static const Families& familiesOfArrivals(const Facts& facts)
{
    return facts.families();
}

template<typename MemberID, typename SetID>
static SetID copySetBetweenArenasOfArrivals(const TypeArena& from, SetsOf<MemberID, SetID>& to, SetID set)
{
    if (!liesInThread(set))
        return set;
    Vector<MemberID, 64> members;
    from.at(set).forEach([&](MemberID member) {
        members.append(member);
    });
    return to.ofSorted(members.span());
}

static TypeID copyTypeBetweenArenasOfArrivals(const TypeArena& from, TypeArena& to, TypeID type)
{
    if (!liesInThread(type))
        return type;

    TypeParts parts = from.at(type);
    parts.stringLiterals = copySetBetweenArenasOfArrivals(from, to.stringSets(), parts.stringLiterals);
    parts.symbolLiterals = copySetBetweenArenasOfArrivals(from, to.keySets(), parts.symbolLiterals);
    parts.objects = copySetBetweenArenasOfArrivals(from, to.objectSets(), parts.objects);
    parts.unnamedLayouts = copySetBetweenArenasOfArrivals(from, to.layoutSets(), parts.unnamedLayouts);
    if (liesInThread(parts.unnamedClasses)) {
        Vector<ClassPart, 8> classes(from.at(parts.unnamedClasses));
        for (ClassPart& part : classes) {
            for (TypeID& written : part.writtenContents)
                written = copyTypeBetweenArenasOfArrivals(from, to, written);
        }
        parts.unnamedClasses = to.internClassParts(classes.span());
    }
    return to.intern(parts);
}

static TypeID joinSettledForArrivals(const TypeArena& types, TypeID joined)
{
    return types.at(joined).isAnything() ? TypeTable::anything : joined;
}

Arrivals::Arrivals(const Program& program, const Facts& facts, TypeTable& types)
    : m_program(program)
    , m_facts(facts)
    , m_types(types)
    , m_joined(linkTablesOfArrivals(facts).parameters.size())
    , m_locks(program.functions.size())
    , m_joins(m_types, ArenaKeeps::UntilReset)
{
    RELEASE_ASSERT(linkTablesOfArrivals(facts).functions.size() == m_program.functions.size());
}

void Arrivals::leave(TypeArena& types, FunctionID callee, uint32_t entryParameter, TypeID proven)
{
    const LinkedFunction& linked = linkTablesOfArrivals(m_facts).function(callee);
    if (entryParameter >= linked.numberOfParameters)
        return;
    proven = proven ? joinSettledForArrivals(types, proven) : TypeTable::anything;

    Locker locker { m_locks[callee.index()] };
    TypeID& joined = m_joined[linked.firstParameter + entryParameter];
    if (joined == TypeTable::anything)
        return;
    if (proven == TypeTable::anything) {
        joined = proven;
        return;
    }

    const Families& families = familiesOfArrivals(m_facts);
    if (liesInThread(joined) || liesInThread(proven)) {
        Locker joinsLocker { m_joinsLock };
        TypeID kept = copyTypeBetweenArenasOfArrivals(types, m_joins, proven);
        joined = joined ? joinSettledForArrivals(m_joins, joinOf(m_joins, families, joined, kept)) : kept;
        return;
    }
    if (!joined || joined == proven) {
        joined = proven;
        return;
    }

    TypeID united = joinSettledForArrivals(types, joinOf(types, families, joined, proven));
    if (liesInThread(united)) {
        Locker joinsLocker { m_joinsLock };
        united = copyTypeBetweenArenasOfArrivals(types, m_joins, united);
    }
    joined = united;
}

void Arrivals::leaveUndefinedFrom(TypeArena& types, FunctionID callee, uint32_t firstMissingArgument)
{
    const Function& function = m_program.at(callee);
    uint32_t firstPositionalParameter = function.firstParameterIndex();
    uint32_t endOfPositionalParameters = firstPositionalParameter + function.numberOfPositionalParameters();
    uint32_t numberOfParameters = linkTablesOfArrivals(m_facts).function(callee).numberOfParameters;
    for (uint32_t entryParameter = firstMissingArgument; entryParameter < numberOfParameters; ++entryParameter) {
        bool isPositional = entryParameter >= firstPositionalParameter && entryParameter < endOfPositionalParameters;
        leave(types, callee, entryParameter, isPositional ? TypeTable::undefined : TypeTable::anything);
    }
}

void Arrivals::leaveAnything(FunctionID callee)
{
    const LinkedFunction& linked = linkTablesOfArrivals(m_facts).function(callee);
    Locker locker { m_locks[callee.index()] };
    for (TypeID& joined : m_joined.mutableSpan().subspan(linked.firstParameter, linked.numberOfParameters))
        joined = TypeTable::anything;
}

bool Arrivals::takesWhatWasLeft(FunctionID function) const
{
    const LinkedFunction& linked = linkTablesOfArrivals(m_facts).function(function);
    return linked.everyCallerIsKnown && linked.everyCallHasOneCallee && !linked.isNamedInItsCycle;
}

TypeID Arrivals::provenAt(FunctionID function, uint32_t entryParameter) const
{
    const LinkTables& link = linkTablesOfArrivals(m_facts);
    const LinkedFunction& linked = link.function(function);
    if (!linked.everyCallerIsKnown || entryParameter >= linked.numberOfParameters)
        return { };

    uint32_t index = linked.firstParameter + entryParameter;
    if (takesWhatWasLeft(function)) {
        TypeID joined = m_joined[index];
        if (joined && joined != TypeTable::anything && !liesInThread(joined))
            return joined;
    }
    return link.parameters[index].proven;
}

TypeID Arrivals::provenAt(TypeArena& types, FunctionID function, uint32_t entryParameter) const
{
    const LinkedFunction& linked = linkTablesOfArrivals(m_facts).function(function);
    if (entryParameter < linked.numberOfParameters && takesWhatWasLeft(function)) {
        TypeID joined = m_joined[linked.firstParameter + entryParameter];
        if (liesInThread(joined)) {
            Locker locker { m_joinsLock };
            return copyTypeBetweenArenasOfArrivals(m_joins, types, joined);
        }
    }
    return provenAt(function, entryParameter);
}

void takeArrivals(Walker& walker)
{
    FunctionID function = walker.unit().function;
    BodyBlock entry = walker.currentBody().entry();
    TypeArena& types = walker.types();
    const Arrivals& arrivals = walker.arrivals();
    Code& code = walker.code();

    for (uint32_t entryParameter = 0; entryParameter < entry.numberOfParameters(); ++entryParameter) {
        Reference parameter = walker.referenceOf(entry.parameter(entryParameter));
        if (!parameter || !parameter.isValue())
            continue;
        TypeID proven = arrivals.provenAt(types, function, entryParameter);
        if (!proven)
            continue;
        TypeID given = code.at(parameter.id()).type;
        if (!types.isSubset(given, proven))
            continue;
        code.addFactUse(FactUse {
            .record = parameter.id(),
            .kind = FactKind::NarrowedTypes,
            .subjectKind = FactSubject::Kind::EntryParameter,
            .subject = function.index(),
            .index = entryParameter,
            .type = given,
        });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAdmitAssertedObjects.h"

#if ENABLE(AOT)

#include "AOTAssertedObjects.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTInterningPhases.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <functional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct MentionInFunction {
    Mention mention;
    uint32_t function { 0 };

    friend bool operator==(const MentionInFunction&, const MentionInFunction&) = default;
    friend auto operator<=>(const MentionInFunction&, const MentionInFunction&) = default;
};

struct ArgumentOfUnknownCallee {
    FunctionID function;
    ValueID value;
    uint32_t position { 0 };
    uint32_t inlineFrame { 0 };

    friend bool operator==(const ArgumentOfUnknownCallee&, const ArgumentOfUnknownCallee&) = default;
    friend auto operator<=>(const ArgumentOfUnknownCallee&, const ArgumentOfUnknownCallee&) = default;
};

enum class Admission : uint8_t { Unchanged, Marked, Widened };

struct RoundCounts {
    std::array<uint32_t, 4> widenedPositionsByKind { };
    uint32_t positionsAdmittingOtherKinds { 0 };
    uint32_t markedPositions { 0 };
    uint32_t resultsWithNewOrigins { 0 };
};

class AssertedObjectAdmission {
public:
    AssertedObjectAdmission(Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
        , m_closures(program, build)
        , m_types(program.types)
        , m_originsOfResults(program.functions.size())
        , m_numberOfSources(FillWith { }, program.functions.size(), 0u)
    {
    }

    void run(Vector<uint32_t>&& functionsWithSources);

private:
    Vector<AssertedObjectFindings> analyze(const Vector<uint32_t>& functions);
    Admission admit(DeclaredType&);
    Admission admitElements(DeclaredType&);
    void admitOtherKinds(PositionReference, ValueKinds, Vector<uint32_t>& readers);
    void admitMembersOfData(Vector<uint32_t>& readers);
    void keepEachFunctionWithBodyOnce(Vector<uint32_t>& functions) const;
    bool addOriginsOfResult(const ReachedPosition&);
    void addReadersOf(Mention, Vector<uint32_t>& functions);
    void addReadersOfResultOfClosureIn(ClosureHolder, Vector<uint32_t>& functions);
    void addReadersOfFieldNamed(PropertyKeyID, Vector<uint32_t>& functions);
    void addReadersOf(PositionReference, Vector<uint32_t>& functions);
    void addDecline(Wire::DeclineKind, FunctionID, AssertedObjectUse);
    Vector<uint32_t> apply(const Vector<uint32_t>& functions, const Vector<AssertedObjectFindings>&);
    void reportArgumentsOfUnknownCallees();
    void logSources(const Vector<uint32_t>& functions) const;
    void logRound(const Vector<AssertedObjectFindings>&);

    Program& m_program;
    BuildContext& m_build;
    KnownClosures m_closures;
    TypeArena m_types;
    BitVector m_keysOfAdmittingFields;
    MentionSet m_holdersOfAdmittingResults;
    Vector<AssertedOrigins> m_originsOfResults;
    AdmittedKinds m_admittedKinds;
    FlowResults m_noFlowResults;
    Vector<uint32_t> m_numberOfSources;
    Vector<MentionInFunction> m_mentions;
    bool m_hasCollectedMentions { false };
    Vector<ArgumentOfUnknownCallee> m_argumentsOfUnknownCallees;
    RoundCounts m_roundCounts;
    uint32_t m_numberOfRounds { 0 };
};

} // anonymous namespace

static bool hasAssertion(const Program& program, const Function& function)
{
    for (const Value& value : function.values) {
        if (value.opcode == Opcode::ClaimDeclared && mayYieldUnnamedObject(program, value.declared()))
            return true;
    }
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        for (TailCast cast : block.terminator.casts()) {
            if (mayYieldUnnamedObject(program, cast.declared))
                return true;
        }
    }
    return false;
}

static void collectMentions(const Function& function, uint32_t index, Vector<MentionInFunction>& mentions)
{
    size_t first = mentions.size();
    for (const Value& value : function.values) {
        if (std::optional<Mention> mention = mentionOf(value))
            mentions.append({ *mention, index });
    }
    std::span<MentionInFunction> added = mentions.mutableSpan().subspan(first);
    std::ranges::sort(added);
    mentions.shrink(first + (std::ranges::unique(added).begin() - added.begin()));
}

Vector<AssertedObjectFindings> AssertedObjectAdmission::analyze(const Vector<uint32_t>& functions)
{
    const ParallelLoops& loops = m_build.parallelLoops();
    Vector<AssertedObjectFindings> findings(functions.size());
    AssertedObjectInputs inputs { m_program, m_closures, m_keysOfAdmittingFields, m_holdersOfAdmittingResults, m_originsOfResults.span(), m_admittedKinds, m_noFlowResults };
    // The largest functions begin first. What is interned does not depend on the order: a task is the place of its function in the list.
    Vector<uint64_t> order(functions.size(), [&](size_t position) {
        return static_cast<uint64_t>(UINT32_MAX - m_program.functions[functions[position]].values.size()) << 32 | position;
    });
    parallelSort(loops, order.mutableSpan(), std::less<uint64_t> { });
    auto phases = makeUnique<InterningPhases>(m_program, functions.size());
    forEachIndexInParallel(loops, order.size(), [&](unsigned turn) {
        unsigned position = static_cast<unsigned>(order[turn]);
        TaskArenas arenas(*phases, position);
        uint32_t function = functions[position];
        findings[position] = findAssertedObjects(inputs, FunctionID(function), m_program.functions[function], m_numberOfSources[function], arenas.types, m_build);
        m_numberOfSources[function] = findings[position].numberOfSources;
    });
    BodyRemaps remaps = phases->finish();
    m_build.destroyLater(WTF::move(phases));
    if (!remaps.isIdentity()) {
        forEachIndexInParallel(loops, functions.size(), [&](unsigned position) {
            if (!remaps.types.isIdentityFor(position))
                applyRemaps(remaps, m_program.functions[functions[position]]);
        });
    }
    return findings;
}

Admission AssertedObjectAdmission::admit(DeclaredType& declared)
{
    TypeID& tested = declared.*testedPartOf(declared, m_build.options().recorded.policy());
    TypeID admitted = m_types.unionOf(tested, TypeTable::object);
    if (admitted == tested && admitsObjects(declared))
        return Admission::Unchanged;
    Admission result = admitted == tested ? Admission::Marked : Admission::Widened;
    tested = admitted;
    declared.flags.add(PositionFlags::AdmitsObjects);
    return result;
}

Admission AssertedObjectAdmission::admitElements(DeclaredType& declared)
{
    Type type = m_types.at(declared.type);
    for (ClassPart& part : type.classes) {
        if (part.builtinClass == BuiltinClasses::Array && !part.contents.isEmpty() && part.contents[0])
            part.contents[0] = m_types.unionOf(part.contents[0], TypeTable::object);
    }
    TypeID admitted = m_types.intern(type);
    if (admitted == declared.type)
        return Admission::Unchanged;
    declared.type = admitted;
    declared.flags.add(PositionFlags::AdmitsObjects);
    return Admission::Widened;
}

void AssertedObjectAdmission::admitOtherKinds(PositionReference position, ValueKinds kinds, Vector<uint32_t>& readers)
{
    if (!m_admittedKinds.add(position, kinds))
        return;
    admitKinds(*findPosition(m_program, position), kinds, m_types);
    addReadersOf(position, readers);
    ++m_roundCounts.positionsAdmittingOtherKinds;
    m_build.statistics().countChanges(PassID::AdmitAssertedObjects, 1);
    m_build.statistics().count(StatisticID::PositionsAdmittingOtherKinds);
}

void AssertedObjectAdmission::admitMembersOfData(Vector<uint32_t>& readers)
{
    if (m_build.options().recorded.policy() == MismatchPolicy::Report)
        return;
    forEachDataMemberThatAlwaysFailsConversion(m_program, m_types, [&](FieldReference field, ValueKinds kinds) {
        admitOtherKinds(PositionReference::field(field), kinds, readers);
    });
}

void AssertedObjectAdmission::keepEachFunctionWithBodyOnce(Vector<uint32_t>& functions) const
{
    std::ranges::sort(functions);
    functions.shrink(std::ranges::unique(functions).begin() - functions.begin());
    functions.removeAllMatching([&](uint32_t function) {
        return !m_program.functions[function].hasBody();
    });
}

bool AssertedObjectAdmission::addOriginsOfResult(const ReachedPosition& reached)
{
    if (reached.position.kind != PositionReference::Kind::Result)
        return false;
    AssertedOrigins& origins = m_originsOfResults[reached.position.owner];
    if (origins.containsAll(reached.origins))
        return false;
    origins.add(reached.origins);
    return true;
}

void AssertedObjectAdmission::addReadersOf(Mention mention, Vector<uint32_t>& functions)
{
    if (!std::exchange(m_hasCollectedMentions, true)) {
        const ParallelLoops& loops = m_build.parallelLoops();
        m_mentions = collectInParallel<MentionInFunction>(loops, m_program.functions.size(), [&](unsigned index, Vector<MentionInFunction>& mentions) {
            collectMentions(m_program.functions[index], index, mentions);
        });
        parallelSort(loops, m_mentions.mutableSpan(), std::less<MentionInFunction> { });
    }
    auto readers = std::ranges::equal_range(m_mentions, mention, { }, &MentionInFunction::mention);
    for (const MentionInFunction& reader : readers)
        functions.append(reader.function);
}

void AssertedObjectAdmission::addReadersOfFieldNamed(PropertyKeyID key, Vector<uint32_t>& functions)
{
    if (!key)
        return;
    m_keysOfAdmittingFields.set(key.index());
    addReadersOf(Mention { Mention::Kind::Key, key.index() }, functions);
}

void AssertedObjectAdmission::addReadersOfResultOfClosureIn(ClosureHolder holder, Vector<uint32_t>& functions)
{
    auto addCallersThrough = [&](Mention mention) {
        m_holdersOfAdmittingResults.add(mention);
        addReadersOf(mention, functions);
    };
    switch (holder.kind) {
    case ClosureHolder::Kind::Variable:
        return addCallersThrough({ Mention::Kind::Variable, holder.owner });
    case ClosureHolder::Kind::Capture:
        functions.append(holder.owner);
        return;
    case ClosureHolder::Kind::EnvironmentField:
        return addCallersThrough({ Mention::Kind::Layout, holder.owner });
    case ClosureHolder::Kind::Member:
        return addCallersThrough({ Mention::Kind::Key, holder.index });
    case ClosureHolder::Kind::Setter:
        return;
    case ClosureHolder::Kind::Getter:
        addReadersOfFieldNamed(holder.key(), functions);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AssertedObjectAdmission::addReadersOf(PositionReference position, Vector<uint32_t>& functions)
{
    switch (position.kind) {
    case PositionReference::Kind::EntryParameter:
        functions.append(position.owner);
        return;
    case PositionReference::Kind::Result:
        m_holdersOfAdmittingResults.add({ Mention::Kind::Function, position.owner });
        addReadersOf(Mention { Mention::Kind::Function, position.owner }, functions);
        for (ClosureHolder holder : m_closures.holdersOf(position.function()))
            addReadersOfResultOfClosureIn(holder, functions);
        return;
    case PositionReference::Kind::Variable:
        addReadersOf(Mention { Mention::Kind::Variable, position.owner }, functions);
        if (LayoutID owner = m_program.at(position.variable()).owner) {
            for (const StaticField& field : m_program.at(owner).staticFields) {
                if (field.variable == position.variable())
                    addReadersOfFieldNamed(field.key, functions);
            }
        }
        return;
    case PositionReference::Kind::Field:
        addReadersOf(Mention { Mention::Kind::Layout, position.owner }, functions);
        addReadersOfFieldNamed(m_program.at(position.layout()).fields[position.index].key, functions);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AssertedObjectAdmission::addDecline(Wire::DeclineKind kind, FunctionID function, AssertedObjectUse use)
{
    if (!m_build.options().collectsDeclines())
        return;
    Wire::Witness witness { .function = function.index(), .value = use.value.index(), .location = m_program.at(function).locationOf(use.origin) };
    m_build.addDecline({ .kind = kind, .limit = LimitID::None, .count = 0, .reason = 0, .witness = witness });
}

Vector<uint32_t> AssertedObjectAdmission::apply(const Vector<uint32_t>& functions, const Vector<AssertedObjectFindings>& allFindings)
{
    Statistics& statistics = m_build.statistics();
    Vector<uint32_t> readers;
    for (size_t index = 0; index < functions.size(); ++index) {
        const AssertedObjectFindings& findings = allFindings[index];
        statistics.count(StatisticID::RequestsAdmittingObjects, findings.numberOfAdmittingRequests);
        statistics.count(StatisticID::RequestsAdmittingOtherKinds, findings.numberOfRequestsAdmittingOtherKinds);
        for (AssertedObjectUse use : findings.argumentsOfUnknownCallees)
            m_argumentsOfUnknownCallees.append({ FunctionID(functions[index]), use.value, use.origin.position, use.origin.inlineFrame });
        for (const ReachedPosition& reached : findings.positions) {
            if (!reached.otherKinds.isEmpty()) {
                admitOtherKinds(reached.position, reached.otherKinds, readers);
                continue;
            }
            DeclaredType& declared = *findPosition(m_program, reached.position);
            Admission admission = reached.part == PositionPart::Elements ? admitElements(declared) : admit(declared);
            bool hasNewOrigins = addOriginsOfResult(reached);
            if (admission == Admission::Unchanged && !hasNewOrigins)
                continue;
            addReadersOf(reached.position, readers);
            m_roundCounts.resultsWithNewOrigins += hasNewOrigins;
            if (admission == Admission::Unchanged)
                continue;
            statistics.countChanges(PassID::AdmitAssertedObjects, 1);
            if (admission == Admission::Marked) {
                ++m_roundCounts.markedPositions;
                continue;
            }
            ++m_roundCounts.widenedPositionsByKind[static_cast<size_t>(reached.position.kind)];
            addDecline(Wire::DeclineKind::PositionAdmitsObjects, FunctionID(functions[index]), reached.use);
            statistics.count(StatisticID::PositionsAdmittingObjects);
        }
    }
    keepEachFunctionWithBodyOnce(readers);
    return readers;
}

void AssertedObjectAdmission::reportArgumentsOfUnknownCallees()
{
    std::ranges::sort(m_argumentsOfUnknownCallees);
    m_argumentsOfUnknownCallees.shrink(std::ranges::unique(m_argumentsOfUnknownCallees).begin() - m_argumentsOfUnknownCallees.begin());
    m_build.statistics().count(StatisticID::AssertedObjectsPassedToUnknownCallees, m_argumentsOfUnknownCallees.size());
    for (const ArgumentOfUnknownCallee& argument : m_argumentsOfUnknownCallees)
        addDecline(Wire::DeclineKind::AssertedObjectPassedToUnknownCallee, argument.function, { argument.value, { argument.position, argument.inlineFrame } });
}

// What the assertions of the program promise, what else they let through, and which operation defines what they are applied to.
void AssertedObjectAdmission::logSources(const Vector<uint32_t>& functions) const
{
    uint32_t toPrimitives = 0;
    uint32_t toObjects = 0;
    uint32_t lettingEveryObjectThrough = 0;
    std::array<uint32_t, numberOfOpcodes> byDefinition { };
    for (uint32_t index : functions) {
        const Function& function = m_program.functions[index];
        for (const Value& value : function.values) {
            if (value.opcode != Opcode::ClaimDeclared || !mayYieldUnnamedObject(m_program, value.declared()))
                continue;
            bool promisesObjects = m_program.at(value.declared().written).hasObjects();
            toObjects += promisesObjects;
            toPrimitives += !promisesObjects;
            lettingEveryObjectThrough += m_program.at(value.declared().type).anyObject;
            ++byDefinition[static_cast<size_t>(function.at(definitionThroughSameValues(function, value.valueOperand())).opcode)];
        }
    }
    dataLog("AOT asserted objects: ", functions.size(), " functions with sources | ", toPrimitives, " assertions to primitives, ", toObjects, " to functions and classes | ", lettingEveryObjectThrough, " let every object through | applied to");
    for (size_t opcode = 0; opcode < byDefinition.size(); ++opcode) {
        if (byDefinition[opcode])
            dataLog(" ", nameOf(static_cast<Opcode>(opcode)), " ", byDefinition[opcode]);
    }
    dataLogLn();
}

void AssertedObjectAdmission::logRound(const Vector<AssertedObjectFindings>& findings)
{
    auto count = [&](bool AssertedObjectFindings::* member) {
        return std::ranges::count_if(findings, [&](const AssertedObjectFindings& finding) {
            return finding.*member;
        });
    };
    const std::array<uint32_t, 4>& widened = m_roundCounts.widenedPositionsByKind;
    dataLogLn("AOT asserted objects: round ", m_numberOfRounds, " | ", findings.size(), " functions, ", count(&AssertedObjectFindings::didTypeAllValues), " typed, ", count(&AssertedObjectFindings::didFollowAssertedObjects), " followed | widened ",
        widened[0], " parameters, ", widened[1], " results, ", widened[2], " variables, ", widened[3], " fields | ", m_roundCounts.positionsAdmittingOtherKinds, " took other kinds | ", m_roundCounts.markedPositions, " marked | ", m_roundCounts.resultsWithNewOrigins, " results with new origins");
}

void AssertedObjectAdmission::run(Vector<uint32_t>&& functionsWithSources)
{
    bool logs = m_build.options().logging.logsPhaseTimes;
    if (logs) [[unlikely]]
        logSources(functionsWithSources);

    // A position changes once and the result of a function gains each origin once, so the rounds end. A round looks at the functions that mention what the round before it changed.
    PhaseTimes times(m_build, PassID::AdmitAssertedObjects);
    Vector<uint32_t> functions = WTF::move(functionsWithSources);
    admitMembersOfData(functions);
    keepEachFunctionWithBodyOnce(functions);
    for (; !functions.isEmpty(); ++m_numberOfRounds) {
        Vector<AssertedObjectFindings> findings = analyze(functions);
        times.didFinish("Analyze"_s);
        m_roundCounts = { };
        functions = apply(functions, findings);
        times.didFinish("Apply"_s);
        if (logs) [[unlikely]]
            logRound(findings);
    }
    reportArgumentsOfUnknownCallees();
}

static bool hasDataMemberThatAlwaysFailsConversion(Program& program)
{
    bool found = false;
    TypeArena types(program.types);
    forEachDataMemberThatAlwaysFailsConversion(program, types, [&](FieldReference, ValueKinds) {
        found = true;
    });
    return found;
}

void admitAssertedObjects(Program& program, BuildContext& build)
{
    bool hasAssertionSite = std::ranges::any_of(program.typeSites, [](const TypeSite& site) {
        return site.kind == TypeSiteKind::Assertion;
    });
    bool throwsOnMismatch = build.options().recorded.policy() == MismatchPolicy::Throw;
    Vector<uint32_t> functions = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return false;
        return (hasAssertionSite && hasAssertion(program, function)) || (throwsOnMismatch && mayHaveConversionThatAlwaysFails(program, function));
    });
    if (!functions.isEmpty() || (throwsOnMismatch && hasDataMemberThatAlwaysFailsConversion(program)))
        AssertedObjectAdmission(program, build).run(WTF::move(functions));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

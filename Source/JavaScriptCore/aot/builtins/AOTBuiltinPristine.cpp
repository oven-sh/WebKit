/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinPristine.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTInstance.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr PristineAspectSet guardedAspects { PristineAspects::Storage, PristineAspects::Names };

std::optional<Receiver> receiverWithTestedStructure(BuiltinClassID builtinClass)
{
    if (builtinClass == BuiltinClasses::Buffer)
        return Receiver::Buffer;
    return std::nullopt;
}

PristineTest pristineTestOf(const BuiltinTable& table, BuiltinClassID builtinClass)
{
    switch (table.at(builtinClass).pristineTest) {
    case BuiltinPristineTest::Mark:
        return table.mayHavePristineInstances(builtinClass) ? PristineTest::MayPass : PristineTest::NeverPasses;
    case BuiltinPristineTest::ClosureAsCreated:
        return PristineTest::ClosureAsCreated;
    case BuiltinPristineTest::InstanceStructure:
        return table.hasLockedPrototypeChain(builtinClass) && receiverWithTestedStructure(builtinClass) ? PristineTest::InstanceStructure : PristineTest::NeverPasses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinClassID classBelowWithTestedStructure(const BuiltinTable& table, BuiltinClassID builtinClass)
{
    BuiltinClassID below = BuiltinClasses::Buffer;
    if (below == builtinClass || !table.isSameOrSubclassOf(below, builtinClass) || pristineTestOf(table, below) != PristineTest::InstanceStructure)
        return { };
    return below;
}

PristineAspects aspectThatFindsMember(const BuiltinTable& table, BuiltinClassID receiverClass, BuiltinID member)
{
    if (table.at(member).kind != BuiltinKind::Accessor || pristineTestOf(table, receiverClass) != PristineTest::MayPass)
        return PristineAspects::Names;
    BuiltinClassID below = classBelowWithTestedStructure(table, receiverClass);
    if (!below)
        return PristineAspects::Names;
    BuiltinMemberLookup foundBelow = table.findForInstance(below, table.at(member).name);
    if (foundBelow.outcome != BuiltinMemberLookup::Outcome::Found || table.canonicalID(foundBelow.member) != table.canonicalID(member))
        return PristineAspects::Names;
    return PristineAspects::NamesKeptBelow;
}

static bool sharesInstanceTestWithParent(const BuiltinTable& table, const BuiltinClass& instanceClass)
{
    if (!instanceClass.parent || instanceClass.instanceTest != BuiltinInstanceTest::JSTypeEquals)
        return false;
    const BuiltinClass& parent = table.at(instanceClass.parent);
    return parent.instanceTest == BuiltinInstanceTest::JSTypeEquals && parent.type == instanceClass.type;
}

bool canOnlyBeProvenPristine(const BuiltinTable& table, BuiltinClassID builtinClass)
{
    const BuiltinClass& instanceClass = table.at(builtinClass);
    return instanceClass.isNeverPristine
        && instanceClass.pristineTest != BuiltinPristineTest::ClosureAsCreated
        && sharesInstanceTestWithParent(table, instanceClass)
        && !instanceClass.isPrimitiveWrapper
        && table.hasLockedPrototypeChain(builtinClass)
        && table.mayHavePristineInstances(instanceClass.parent);
}

static PristineProof proofSinceAllocation(const PristineQuery& query, ValueID value, BuiltinClassID builtinClass, ProgramPoint point)
{
    ValueID definition = query.analyses->sameObjectThroughCasts(value);
    const Value& allocation = query.function.at(definition);
    if (!proofsOfAllocation(query.program, query.function, allocation).contains(Proof::Pristine))
        return PristineProof::Unknown;
    if (!query.program.at(allocation.type).findClassPart(builtinClass))
        return PristineProof::Unknown;
    if (query.analyses->guards().stateAt(definition, ProofSet { Proof::Pristine }, point) != GuardState::Available)
        return PristineProof::Unknown;
    return PristineProof::Proven;
}

static PristineAspectSet aspectsProvenByGuards(const PristineQuery& query, ValueID value, BuiltinClassID builtinClass, ProgramPoint point)
{
    if (!query.analyses->hasPristineTest())
        return { };
    const GuardAvailability& guards = query.analyses->guards();
    ValueID subject = query.analyses->identityOf(value);
    PristineAspectSet proven;
    for (ValueID guard : guards.guards()) {
        PristineAspectSet tested;
        for (const GuardFact& fact : guards.factsOf(guard).facts) {
            const GuardOrigin* origin = fact.singleOrigin();
            if (!origin || origin->source != GuardSource::Test || !fact.proves(Proof::Pristine) || fact.subject != subject)
                continue;
            const Value& test = query.function.at(origin->test);
            if (test.opcode == Opcode::IsPristine && test.builtinClass() == builtinClass)
                tested.add(test.aspects());
        }
        if (proven.containsAll(tested))
            continue;
        if (guards.stateAt(guard, ProofSet { Proof::Pristine }, subject, point) == GuardState::Available)
            proven.add(tested);
    }
    return proven;
}

static PristineAspectSet collectAspectsProvenAsTheyAre(const PristineQuery& query, ValueID value, const PristineRequirement& wanted, ProgramPoint point)
{
    PristineAspectSet proven = pristineAspectsProvenBy(query.program.typeOf(query.function, value).withoutNullish(), wanted.builtinClass);
    for (const Value& test : query.passedTests) {
        if (test.opcode == Opcode::IsPristine && test.builtinClass() == wanted.builtinClass)
            proven.add(test.aspects());
    }
    if (proven.containsAll(wanted.aspects) || !query.analyses)
        return proven;
    if (query.asksAboutPlace)
        *query.asksAboutPlace = true;
    if (proofSinceAllocation(query, value, wanted.builtinClass, point) == PristineProof::Proven) {
        proven.add(guardedAspects);
        return proven;
    }
    proven.add(aspectsProvenByGuards(query, value, wanted.builtinClass, point));
    return proven;
}

static PristineAspectSet collectProvenAspects(const PristineQuery& query, ValueID value, const PristineRequirement& wanted, ProgramPoint point)
{
    PristineRequirement asTested = wanted;
    if (asTested.aspects.contains(PristineAspects::NamesKeptBelow))
        asTested.aspects.add(PristineAspects::Names);
    PristineAspectSet proven = collectAspectsProvenAsTheyAre(query, value, asTested, point);
    if (proven.contains(PristineAspects::Names))
        proven.add(PristineAspects::NamesKeptBelow);
    return proven;
}

PristineAspectSet aspectsProvenAt(const PristineQuery& query, ValueID value, BuiltinClassID builtinClass, ProgramPoint point)
{
    return collectProvenAspects(query, value, { builtinClass, guardedAspects }, point);
}

PristineProof pristineProofOf(const PristineQuery& query, ValueID value, const PristineRequirement& requirement, ProgramPoint point)
{
    if (!requirement)
        return PristineProof::Proven;
    if (collectProvenAspects(query, value, requirement, point).containsAll(requirement.aspects))
        return PristineProof::Proven;
    return PristineProof::Unknown;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

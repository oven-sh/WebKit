/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClaims.h"
#include "AOTIRTestSupport.h"
#include "AOTLedgerVariables.h"
#include "AOTNativeTest.h"

namespace JSC { namespace AOT { namespace Typed {

using Report = LedgerOfVariables::Report;

static constexpr std::array everyReport { Report::OperationsThatNamePlaces, Report::BodiesWithoutSolve, Report::ScopesOfEvaluatedText, Report::Modules, Report::HoldersOfVariables, Report::AccessesThroughHolders };

static_assert(!canWrite(PlaceKind::EnvironmentField, Actor::NativeCode) && canWrite(PlaceKind::Variable, Actor::Loader));
static_assert(actorsThatCanWrite(PlaceKind::Variable) == everyActor);
static_assert(actorsThatCanWrite(PlaceKind::EnvironmentField) == OptionSet<Actor> { Actor::Statements, Actor::Creation, Actor::UnseenCode });
static_assert(!canRead(PlaceKind::Variable, Actor::Creation));

static Write storeThatNamesPlace(OptionSet<WriteTrait> more = { })
{
    return { Actor::Statements, WriteForm::Store, { }, { }, FunctionID(0u), ValueID(0u), traitsOfOperationThatNamesPlace | more };
}

static void ledgerAnActorThatIsOpenLeavesEveryClaimUnknown()
{
    for (Actor actor : actorsInOrder) {
        Writers writers { { }, actor };
        AOT_TEST_CHECK(!writers.all() && writers.open() == actor);
        AOT_TEST_CHECK(writerStateOf(writers).state == WriterState::Unknown && writerStateOf(writers).cause == causeOf(actor) && causeOf(actor) != UnprovenCause::Unknown);
        AOT_TEST_CHECK(storeCountOf(writers).state == StoreCount::Unknown && storeNamingOf(writers).state == StoreNaming::Unknown);
        Readers readers { { }, actor };
        AOT_TEST_CHECK(readStateOf(readers).state == ReadState::Unknown && !isReadOnlyByOperationsThatNameIt(readers) && !isReadOnlyByListedOperations(readers));
        AOT_TEST_CHECK(namespaceUseOf({ { }, actor }, { { }, { } }).state == NamespaceUse::Unknown);
        Reach made { Actor::Loader, { } };
        AOT_TEST_CHECK(namespaceUseOf({ singleElementSpan(made), { } }, readers).state == NamespaceUse::Unknown);
    }
}

static void ledgerClaimsFollowTheWritesThatAreListed()
{
    Writers nobody { { }, { } };
    AOT_TEST_CHECK(writerStateOf(nobody).state == WriterState::AllKnown && storeCountOf(nobody).state == StoreCount::Never && storeNamingOf(nobody).state == StoreNaming::EveryStoreNamesLocation);

    Write once = storeThatNamesPlace(WriteTrait::RunsAtMostOnce);
    AOT_TEST_CHECK(storeCountOf({ singleElementSpan(once), { } }).state == StoreCount::AtMostOnce);
    Write again = storeThatNamesPlace();
    AOT_TEST_CHECK(writerStateOf({ singleElementSpan(again), { } }).state == WriterState::AllKnown && storeCountOf({ singleElementSpan(again), { } }).state == StoreCount::Unknown);
    std::array twice { once, once };
    AOT_TEST_CHECK(storeCountOf({ twice, { } }).state == StoreCount::Unknown && storeNamingOf({ twice, { } }).state == StoreNaming::EveryStoreNamesLocation);

    Write throughObject = again;
    throughObject.traits.remove(WriteTrait::NamesPlace);
    AOT_TEST_CHECK(writerStateOf({ singleElementSpan(throughObject), { } }).state == WriterState::AllKnown && storeNamingOf({ singleElementSpan(throughObject), { } }).state == StoreNaming::Unknown);

    auto causeWith = [&](Write write) {
        std::array writes { once, write };
        Claim<WriterState> claim = writerStateOf({ writes, { } });
        AOT_TEST_CHECK(claim.state == WriterState::Unknown && storeCountOf({ writes, { } }).state == StoreCount::Unknown && storeNamingOf({ writes, { } }).state == StoreNaming::Unknown);
        return claim.cause;
    };
    AOT_TEST_CHECK(causeWith({ Actor::KeysNobodyKnows, WriteForm::Delete }) == UnprovenCause::FieldDeleted);
    AOT_TEST_CHECK(causeWith({ Actor::KeysNobodyKnows, WriteForm::Define }) == UnprovenCause::PropertyRedefined);
    AOT_TEST_CHECK(causeWith({ Actor::KeysNobodyKnows, WriteForm::Freeze }) == UnprovenCause::PropertyRedefined);
    AOT_TEST_CHECK(causeWith({ Actor::Table, WriteForm::Store }) == UnprovenCause::StoreOfOtherType);
    Write ofOtherType = throughObject;
    ofOtherType.traits.remove(WriteTrait::KeepsDeclaredType);
    AOT_TEST_CHECK(causeWith(ofOtherType) == UnprovenCause::StoreOfOtherType);
    Write withoutValue = throughObject;
    withoutValue.traits.remove(WriteTrait::SpellsItsValue);
    AOT_TEST_CHECK(causeWith(withoutValue) == UnprovenCause::UnknownWriter);

    AOT_TEST_CHECK(writerStateOf(writersOfFieldThatPassAdds()).state == WriterState::AllKnown && storeNamingOf(writersOfFieldThatPassAdds()).state == StoreNaming::EveryStoreNamesLocation);
    AOT_TEST_CHECK(writerStateOf(writersOfVariableThatPassFillsOnce()).state == WriterState::AllKnown && storeCountOf(writersOfVariableThatPassFillsOnce()).state == StoreCount::AtMostOnce);
}

static void ledgerClaimsFollowTheReadsThatAreListed()
{
    AOT_TEST_CHECK(readStateOf({ { }, { } }).state == ReadState::NeverRead);
    Read called { Actor::Statements, ReadForm::AsCallee, { }, { }, { }, true };
    Read taken { Actor::Statements, ReadForm::AsValue, { }, { }, { }, true };
    Read byLoader { Actor::Loader, ReadForm::AsValue };
    Read throughObject { Actor::Statements, ReadForm::AsCallee, { }, FunctionID(0u), ValueID(0u), false };
    AOT_TEST_CHECK(readStateOf({ singleElementSpan(called), { } }).state == ReadState::OnlyCalled);
    std::array both { called, taken };
    AOT_TEST_CHECK(readStateOf({ both, { } }).state == ReadState::Unknown && isReadOnlyByOperationsThatNameIt({ both, { } }));
    AOT_TEST_CHECK(readStateOf({ singleElementSpan(byLoader), { } }).state == ReadState::Unknown);
    AOT_TEST_CHECK(!isReadOnlyByOperationsThatNameIt({ singleElementSpan(byLoader), { } }) && !isReadOnlyByListedOperations({ singleElementSpan(byLoader), { } }));
    AOT_TEST_CHECK(!isReadOnlyByOperationsThatNameIt({ singleElementSpan(throughObject), { } }) && isReadOnlyByListedOperations({ singleElementSpan(throughObject), { } }));

    Reach made { Actor::Loader, { } };
    AOT_TEST_CHECK(namespaceUseOf({ { }, { } }, { { }, everyActor }).state == NamespaceUse::NeverCreated);
    AOT_TEST_CHECK(namespaceUseOf({ singleElementSpan(made), { } }, { { }, { } }).state == NamespaceUse::NamedReadsOnly);
    AOT_TEST_CHECK(namespaceUseOf({ singleElementSpan(made), { } }, { singleElementSpan(byLoader), { } }).state == NamespaceUse::Unknown);
}

// Whoever has not reported is open on every variable.
static void ledgerOfVariablesIsClosedOnlyByEveryReport()
{
    TestProgram test;
    test.program.variables.append(Variable { });
    Place place = Place::variable(VariableID(0u));

    for (std::optional<Report> missing : { std::optional<Report>(), std::optional(Report::OperationsThatNamePlaces), std::optional(Report::BodiesWithoutSolve), std::optional(Report::ScopesOfEvaluatedText), std::optional(Report::Modules), std::optional(Report::HoldersOfVariables), std::optional(Report::AccessesThroughHolders) }) {
        LedgerOfVariables ledger(test.program);
        for (Report report : everyReport) {
            if (report != missing)
                ledger.didHear(report);
        }
        ledger.close();
        OptionSet<Actor> owing;
        for (Actor actor : actorsInOrder) {
            if (missing && LedgerOfVariables::reportsOwedBy(actor).contains(*missing))
                owing.add(actor);
        }
        AOT_TEST_CHECK(!missing || !owing.isEmpty());
        AOT_TEST_CHECK(ledger.writersOf(place)->open() == owing);
        AOT_TEST_CHECK(ledger.readersOf(place)->open() == (owing & actorsThatCanRead(PlaceKind::Variable)));
        AOT_TEST_CHECK((writerStateOf(*ledger.writersOf(place)).state == WriterState::AllKnown) == !missing);
    }

    LedgerOfVariables silent(test.program);
    silent.close();
    AOT_TEST_CHECK(silent.writersOf(place)->open() == everyActor && silent.readersOf(place)->open() == actorsThatCanRead(PlaceKind::Variable));
}

static void ledgerOfVariablesListsWhatItIsHandedByPlace()
{
    TestProgram test;
    test.program.variables.append(Variable { });
    test.program.variables.append(Variable { });
    LedgerOfVariables ledger(test.program);
    for (Report report : everyReport)
        ledger.didHear(report);
    ledger.addWritesThatNamePlaces({ { 1, storeThatNamesPlace(WriteTrait::RunsAtMostOnce) } });
    ledger.note(1, LedgerOfVariables::ReadNoted::AsCalleeByOperationThatNamesPlace);
    ledger.leaveOpenToWriter(0, Actor::Loader);
    ledger.leaveOpenToReader(0, Actor::UnseenCode);
    ledger.close();

    AOT_TEST_CHECK(ledger.writersAt(0).open() == Actor::Loader && ledger.readersAt(0).open() == Actor::UnseenCode);
    AOT_TEST_CHECK(ledger.writersAt(0).seen().predicted().empty() && ledger.readersAt(0).seen().predicted().empty());
    AOT_TEST_CHECK(storeCountOf(ledger.writersAt(1)).state == StoreCount::AtMostOnce && readStateOf(ledger.readersAt(1)).state == ReadState::OnlyCalled);
    AOT_TEST_CHECK(!ledger.writersOf(Place::prototypeLink({ })));
}

#define FOR_EACH_AOT_LEDGER_TEST(v) \
    v(ledgerAnActorThatIsOpenLeavesEveryClaimUnknown) v(ledgerClaimsFollowTheWritesThatAreListed) v(ledgerClaimsFollowTheReadsThatAreListed) \
    v(ledgerOfVariablesIsClosedOnlyByEveryReport) v(ledgerOfVariablesListsWhatItIsHandedByPlace)

std::span<const NativeTest> ledgerTests()
{
#define AOT_LEDGER_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_LEDGER_TEST(AOT_LEDGER_TEST_ENTRY) };
#undef AOT_LEDGER_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLedgerEntries.h"
#include "AOTPlaces.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// Every claim of ir/schema/flow-records.def is a function of who writes, who reads and who reaches, and of nothing else.
// Whoever makes a claim, flow or a pass that has looked for itself, makes it here.

struct UnknownBecause {
    UnprovenCause cause;
};
constexpr UnknownBecause unknown(UnprovenCause cause) { return { cause }; }

template<typename State>
struct Claim {
    constexpr Claim(State state)
        : state(state)
    {
    }
    constexpr Claim(UnknownBecause because)
        : cause(because.cause)
    {
    }

    State state { State::Unknown };
    UnprovenCause cause { UnprovenCause::Unknown };
};

// A pass takes a variable or an environment field for a word that only stores change. What else is done to the place says why it is none.
inline std::optional<UnprovenCause> whyPlaceIsMoreThanStoredInto(std::span<const Write> writes)
{
    std::optional<UnprovenCause> found;
    auto rankOf = [](UnprovenCause cause) {
        if (cause == UnprovenCause::FieldDeleted)
            return 0;
        if (cause == UnprovenCause::PropertyRedefined)
            return 1;
        return cause == UnprovenCause::StoreOfOtherType ? 2 : 3;
    };
    auto note = [&](UnprovenCause cause) {
        if (!found || rankOf(cause) < rankOf(*found))
            found = cause;
    };
    for (const Write& write : writes) {
        switch (write.form) {
        case WriteForm::Delete:
            note(UnprovenCause::FieldDeleted);
            continue;
        case WriteForm::Define:
        case WriteForm::DefineAccessor:
        case WriteForm::Freeze:
        case WriteForm::Link:
            note(UnprovenCause::PropertyRedefined);
            continue;
        case WriteForm::AtCreation:
        case WriteForm::Store:
            break;
        }
        if (write.isOperationOfBody() && !write.traits.contains(WriteTrait::SpellsItsValue))
            note(UnprovenCause::UnknownWriter);
        else if (!write.isOperationOfBody() || !write.traits.contains(WriteTrait::KeepsDeclaredType))
            note(UnprovenCause::StoreOfOtherType);
    }
    return found;
}

inline Claim<WriterState> writerStateOf(const Writers& writers)
{
    auto all = writers.all();
    if (!all)
        return unknown(causeOf(writers.open()));
    if (std::optional<UnprovenCause> cause = whyPlaceIsMoreThanStoredInto(*all))
        return unknown(*cause);
    return WriterState::AllKnown;
}

inline Claim<StoreCount> storeCountOf(const Writers& writers)
{
    auto all = writers.all();
    if (!all)
        return unknown(causeOf(writers.open()));
    if (std::optional<UnprovenCause> cause = whyPlaceIsMoreThanStoredInto(*all))
        return unknown(*cause);
    if (all->value().empty())
        return StoreCount::Never;
    if (all->value().size() == 1 && all->value().front().traits.contains(WriteTrait::RunsAtMostOnce))
        return StoreCount::AtMostOnce;
    return StoreCount::Unknown;
}

inline Claim<StoreNaming> storeNamingOf(const Writers& writers)
{
    auto all = writers.all();
    if (!all)
        return unknown(causeOf(writers.open()));
    if (std::optional<UnprovenCause> cause = whyPlaceIsMoreThanStoredInto(*all))
        return unknown(*cause);
    for (const Write& write : all->value()) {
        if (!write.traits.contains(WriteTrait::NamesPlace))
            return StoreNaming::Unknown;
    }
    return StoreNaming::EveryStoreNamesLocation;
}

inline Claim<ReadState> readStateOf(const Readers& readers)
{
    auto all = readers.all();
    if (!all)
        return unknown(causeOf(readers.open()));
    bool isCalled = false;
    for (const Read& read : all->value()) {
        switch (read.form) {
        case ReadForm::AsValue:
        case ReadForm::Test:
            return ReadState::Unknown;
        case ReadForm::AsCallee:
            isCalled = true;
            break;
        }
    }
    return isCalled ? ReadState::OnlyCalled : ReadState::NeverRead;
}

// The integers that arrive behind a check of one version arrive unchecked in another.
inline bool isStoredIntegersThatOnlyChecksBound(const Writers& writers)
{
    return std::ranges::any_of(writers.seen().predicted(), [](const Write& write) {
        return write.traits.contains(WriteTrait::KeepsDeclaredType) && !write.traits.contains(WriteTrait::KeepsDeclaredIntegers);
    });
}

// A read that names the place finds it without the object that shows it.
inline bool isReadOnlyByOperationsThatNameIt(const Readers& readers)
{
    auto all = readers.all();
    return all && std::ranges::all_of(all->value(), &Read::namesPlace);
}

// Every look at the place is an operation of a body that flow has solved.
inline bool isReadOnlyByListedOperations(const Readers& readers)
{
    auto all = readers.all();
    return all && std::ranges::all_of(all->value(), [](const Read& read) {
        return read.namesPlace || read.function;
    });
}

inline Claim<NamespaceUse> namespaceUseOf(const Reachers& reachersOfNamespace, const Readers& readersUnderNamesNobodyLists)
{
    auto all = reachersOfNamespace.all();
    if (!all)
        return unknown(causeOf(reachersOfNamespace.open()));
    if (all->value().empty())
        return NamespaceUse::NeverCreated;
    auto reads = readersUnderNamesNobodyLists.all();
    if (!reads)
        return unknown(causeOf(readersUnderNamesNobodyLists.open()));
    return reads->value().empty() ? NamespaceUse::NamedReadsOnly : NamespaceUse::Unknown;
}

// A pass that adds a variable or a field to an environment lays every store into it itself, each an operation that names the place. No record of a module, no object and no scope of evaluated text lists the place.
inline Writers writersOfFieldThatPassAdds()
{
    static constexpr Write store { Actor::Statements, WriteForm::Store, { }, { }, { }, { }, traitsOfOperationThatNamesPlace };
    return { std::span<const Write>(&store, 1), { } };
}

// The one store that the pass lays stands where the value was made, in a body that runs once.
inline Writers writersOfVariableThatPassFillsOnce()
{
    static constexpr Write store { Actor::Statements, WriteForm::Store, { }, { }, { }, { }, traitsOfOperationThatNamesPlace | WriteTrait::RunsAtMostOnce };
    return { std::span<const Write>(&store, 1), { } };
}

// What the writes and the reads of a variable or of an environment field let anybody say of it. Order says the rest.
inline void claimWritesOf(LocationFlow& flow, const Writers& writers)
{
    Claim<WriterState> state = writerStateOf(writers);
    flow.writers = state.state;
    flow.cause = state.cause;
    flow.stores = storeCountOf(writers).state;
    flow.storeNaming = storeNamingOf(writers).state;
    flow.isUnchangedDuringActivation = flow.stores == StoreCount::Never;
}

inline void claimReadsOf(LocationFlow& flow, const Readers& readers)
{
    flow.reads = readStateOf(readers).state;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

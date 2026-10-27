/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLedger.h"

namespace JSC { namespace AOT { namespace Typed {

// What flow claims about classes, constructor functions and function objects. Each claim is a function of who writes a place and who reaches an object, and of nothing else that the solve has found.

namespace ClaimsAboutClassesAndClosures {

inline bool isUnderName(const Write& write)
{
    ValueFlowNameShape shape = shapeOf(write.under.kind);
    return shape.reach == ValueFlowNameReach::OneKey && shape.isValueOrAccessor();
}

inline PropertyKeyID keyOf(const Write& write)
{
    return isUnderName(write) ? PropertyKeyID(write.under.payload) : PropertyKeyID();
}

// The place holds for ever what was laid there when its holder was made. `putsBackWhatIsThere` says so of a later write that brings nothing else.
inline MemberMutability mutabilityOf(const Writers& writers, NOESCAPE const Invocable<bool(const Write&)> auto& putsBackWhatIsThere)
{
    auto all = writers.all();
    if (!all)
        return MemberMutability::Unknown;
    bool isKept = std::ranges::all_of(all->value(), [&](const Write& write) {
        return write.form == WriteForm::AtCreation || putsBackWhatIsThere(write);
    });
    return isKept ? MemberMutability::Fixed : MemberMutability::Unknown;
}

inline MemberMutability mutabilityOf(const Writers& writers)
{
    return mutabilityOf(writers, [](const Write&) {
        return false;
    });
}

// The holder is written behind its creation, and every such write stands under a name.
inline bool isStoredToUnderNamesAlone(const Writers& writersOfEveryOwnProperty)
{
    auto all = writersOfEveryOwnProperty.all();
    if (!all)
        return false;
    bool isStoredTo = false;
    for (const Write& write : all->value()) {
        if (write.form == WriteForm::AtCreation || write.form == WriteForm::Freeze)
            continue;
        if (!isUnderName(write))
            return false;
        isStoredTo = true;
    }
    return isStoredTo;
}

// Nothing makes a property of the holder read-only.
inline bool isNeverFrozen(const Writers& writersOfEveryOwnProperty)
{
    auto all = writersOfEveryOwnProperty.all();
    if (!all)
        return false;
    return std::ranges::none_of(all->value(), [](const Write& write) {
        return write.form == WriteForm::Freeze;
    });
}

// The holder has no own property under a name but those that `isListed` accepts. Taking a property away adds no name, and neither does making it read-only.
enum class Redefinitions : uint8_t { MayAddNames, AreListedUnderTheirNames };

inline bool namesAreComplete(const Writers& writersOfEveryOwnProperty, Redefinitions redefinitions, NOESCAPE const Invocable<bool(PropertyKeyID)> auto& isListed)
{
    auto all = writersOfEveryOwnProperty.all();
    if (!all)
        return false;
    return std::ranges::all_of(all->value(), [&](const Write& write) {
        if (write.form == WriteForm::AtCreation)
            return true;
        if (PropertyKeyID key = keyOf(write))
            return isListed(key);
        switch (write.form) {
        case WriteForm::Freeze:
            return true;
        case WriteForm::Delete:
        case WriteForm::Define:
            return write.actor == Actor::Statements && redefinitions == Redefinitions::AreListedUnderTheirNames;
        case WriteForm::AtCreation:
        case WriteForm::Store:
        case WriteForm::DefineAccessor:
        case WriteForm::Link:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

// The names that are written on a function object behind its creation, where there are no others.
inline std::optional<Vector<PropertyKeyID>> keysAddedTo(const Writers& writersOfEveryOwnProperty, const Writers& writersOfElements, const Reachers& reachers)
{
    auto all = writersOfEveryOwnProperty.all();
    if (!all || !reachers.all())
        return std::nullopt;
    // What is written on the holder under a key that nobody knows stands in the list of every own property and is judged there.
    if (!(writersOfElements.open() - Actor::KeysNobodyKnows).isEmpty())
        return std::nullopt;
    Vector<PropertyKeyID> keys;
    for (const Write& write : all->value()) {
        if (PropertyKeyID key = keyOf(write)) {
            if (keys.isEmpty() || keys.last() != key)
                keys.append(key);
            continue;
        }
        bool addsNoName = write.actor == Actor::Statements && (write.form == WriteForm::Freeze || write.form == WriteForm::Delete || write.form == WriteForm::Define);
        if (!addsNoName)
            return std::nullopt;
    }
    return keys;
}

// Nobody does more with the function object than hand it on and call it.
inline ClosureUse useOfClosure(const Reachers& reachers)
{
    auto all = reachers.all();
    if (!all)
        return ClosureUse::Unknown;
    return all->value().empty() ? ClosureUse::CalledOnly : ClosureUse::Unknown;
}

// Every hand that holds the object is a position of a body that flow has solved. Native code that is handed it says in its row what it does.
inline bool isInSeenHandsAlone(OptionSet<Actor> open)
{
    return !open.containsAny({ Actor::UnseenCode, Actor::ReceiversNobodyKnows });
}

} // namespace ClaimsAboutClassesAndClosures

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

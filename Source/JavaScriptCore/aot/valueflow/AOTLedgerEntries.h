/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTActors.h"
#include "AOTExpected.h"
#include "AOTTypeKinds.h"
#include "AOTValueFlow.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

enum class WriteForm : uint8_t { AtCreation, Store, Define, DefineAccessor, Delete, Link, Freeze };
enum class ReadForm : uint8_t { AsValue, AsCallee, Test };
// What a position does with an object beyond handing it on or calling it.
enum class ReachForm : uint8_t { LooksInto, TakesOutOfMember, Changes, LinksAbove };
inline constexpr unsigned numberOfReachForms = 4;

// Each is a claim about one write. A write that nobody has looked at has none.
enum class WriteTrait : uint8_t {
    // It is an operation of a body that flow has solved: whoever walks the bodies finds it.
    IsOperationOfBody = 1 << 5,
    // The operation names the variable or the field itself. Any other write came through an object that shows the place.
    NamesPlace = 1 << 0,
    // One of its operands is the value that arrives.
    SpellsItsValue = 1 << 1,
    // What arrives lies inside the declared type of the place, in every version of the body.
    KeepsDeclaredType = 1 << 2,
    // So do the integers that arrive.
    KeepsDeclaredIntegers = 1 << 3,
    // It runs at most once in the life of the program.
    RunsAtMostOnce = 1 << 4,
};

// Every store into the position is held to its type, and the stored value is an operand.
inline constexpr OptionSet<WriteTrait> traitsOfOperationThatNamesPlace { WriteTrait::IsOperationOfBody, WriteTrait::NamesPlace, WriteTrait::SpellsItsValue, WriteTrait::KeepsDeclaredType, WriteTrait::KeepsDeclaredIntegers };

// `by` is the statement of flow that wrote, where there is one. An operation that flows without a rule has none: function and site name it.
struct Write {
    Actor actor { Actor::Statements };
    WriteForm form { WriteForm::Store };
    ValueFlowStatementID by;
    ValueSetID arrives;
    FunctionID function;
    ValueID site;
    OptionSet<WriteTrait> traits;
    ValueFlowName under { };
    ValueSetID through;
    ValueSetID seenToArrive;

    bool isOperationOfBody() const { return traits.contains(WriteTrait::IsOperationOfBody); }
};

struct Read {
    Actor actor { Actor::Statements };
    ReadForm form { ReadForm::AsValue };
    ValueFlowStatementID by;
    FunctionID function;
    ValueID site;
    bool namesPlace { false };
};

struct Reach {
    Actor actor { Actor::Statements };
    ValueFlowNodeID position;
    ReachForm form { ReachForm::LooksInto };
};

// What was seen, and who has not accounted for the place. A list is closed only by signatures: nobody's silence closes it.
template<typename Entry>
class AccountOf {
public:
    AccountOf(std::span<const Entry> seen, OptionSet<Actor> open)
        : m_seen(seen)
        , m_open(open)
    {
    }

    std::optional<ProvenValue<std::span<const Entry>>> all() const { return m_open.isEmpty() ? std::optional(Proven::byClosedSet(m_seen)) : std::nullopt; }
    Expected<std::span<const Entry>> seen() const { return m_seen; }
    OptionSet<Actor> open() const { return m_open; }

private:
    std::span<const Entry> m_seen;
    OptionSet<Actor> m_open;
};

using Writers = AccountOf<Write>;
using Readers = AccountOf<Read>;
using Reachers = AccountOf<Reach>;

constexpr UnprovenCause causeOf(OptionSet<Actor> open)
{
    for (Actor actor : { Actor::UnseenCode, Actor::NativeCode, Actor::Table, Actor::ReceiversNobodyKnows, Actor::KeysNobodyKnows, Actor::Loader, Actor::Creation, Actor::Statements }) {
        if (!open.contains(actor))
            continue;
        switch (actor) {
        case Actor::NativeCode:
        case Actor::Table:
            return UnprovenCause::ReachesNativeCode;
        case Actor::ReceiversNobodyKnows:
            return UnprovenCause::UnknownObject;
        case Actor::KeysNobodyKnows:
            return UnprovenCause::UnknownKeyStore;
        case Actor::UnseenCode:
        case Actor::Loader:
        case Actor::Creation:
        case Actor::Statements:
            return UnprovenCause::UnknownWriter;
        }
    }
    return UnprovenCause::Unknown;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

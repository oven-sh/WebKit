/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpected.h"
#include "AOTPlaces.h"
#include "AOTTypeKinds.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsInternal.h"

namespace JSC { namespace AOT { namespace Typed {

enum class WriteForm : uint8_t { AtCreation, Store, Define, DefineAccessor, Delete, Link, Freeze };
enum class ReadForm : uint8_t { AsValue, AsCallee, Test };

// `by` is the statement of flow that wrote, where there is one. An operation that flows without a rule has none: function and site name it.
struct Write {
    Actor actor { Actor::Statements };
    WriteForm form { WriteForm::Store };
    ValueFlowStatementID by;
    ValueSetID arrives;
    FunctionID function;
    ValueID site;
};

struct Read {
    Actor actor { Actor::Statements };
    ReadForm form { ReadForm::AsValue };
    ValueFlowStatementID by;
    FunctionID function;
    ValueID site;
};

struct Reach {
    Actor actor { Actor::Statements };
    ValueFlowNodeID position;
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
        case Actor::UnseenCode:
            return UnprovenCause::UnseenCode;
        case Actor::NativeCode:
        case Actor::Table:
            return UnprovenCause::ReachesNativeCode;
        case Actor::ReceiversNobodyKnows:
            return UnprovenCause::UnknownObject;
        case Actor::KeysNobodyKnows:
            return UnprovenCause::UnknownKeyStore;
        case Actor::Loader:
        case Actor::Creation:
        case Actor::Statements:
            return UnprovenCause::UnknownWriter;
        }
    }
    return UnprovenCause::Unknown;
}

// The actor of a write is a function of the statement that asked for the written node, of the objects it writes through and of the name it writes under.
inline Actor actorOf(const ValueFlowResultInputs& inputs, const ValueFlowStatement& statement, ValueSetID writtenThrough, ValueFlowName name)
{
    switch (statement.kind) {
    case ValueFlowStatementKind::Operation:
    case ValueFlowStatementKind::Lookup:
    case ValueFlowStatementKind::CallGroup:
    case ValueFlowStatementKind::Coroutine:
        if (ValueSets::isComplement(writtenThrough) || inputs.sets().meet(writtenThrough, inputs.universe().placeholdersAlone()))
            return Actor::ReceiversNobodyKnows;
        if (shapeOf(name.kind).reach == ValueFlowNameReach::UnknownKey || shapeOf(name.kind).sort == ValueFlowNameSort::Copy)
            return Actor::KeysNobodyKnows;
        return statement.kind == ValueFlowStatementKind::Operation && flowOf(statement.opcode) == ValueFlowEdgeClass::Allocates ? Actor::Creation : Actor::Statements;
    case ValueFlowStatementKind::Behavior:
    case ValueFlowStatementKind::UsedBuiltins:
        return Actor::Table;
    case ValueFlowStatementKind::Escape:
        return Actor::ReceiversNobodyKnows;
    case ValueFlowStatementKind::HostRoots:
        return Actor::NativeCode;
    case ValueFlowStatementKind::ModuleEvaluation:
    case ValueFlowStatementKind::ModulesLoadedByName:
        return Actor::Loader;
    case ValueFlowStatementKind::UnseenCode:
        return Actor::UnseenCode;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline WriteForm formOf(const ValueFlowStatement& statement, ValueFlowName name)
{
    switch (shapeOf(name.kind).sort) {
    case ValueFlowNameSort::Getter:
    case ValueFlowNameSort::Setter:
        return WriteForm::DefineAccessor;
    case ValueFlowNameSort::Deletion:
        return WriteForm::Delete;
    case ValueFlowNameSort::Redefinition:
        return WriteForm::Define;
    case ValueFlowNameSort::PrototypeLink:
        return WriteForm::Link;
    case ValueFlowNameSort::Value:
    case ValueFlowNameSort::Copy:
    case ValueFlowNameSort::Other:
        return statement.kind == ValueFlowStatementKind::Operation && flowOf(statement.opcode) == ValueFlowEdgeClass::Allocates ? WriteForm::AtCreation : WriteForm::Store;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// THE ONE RELATION, asked of one object: the actors that have not accounted for it. An object that no claim takes out is open.
// Statements, KeysNobodyKnows and ReceiversNobodyKnows are seen writes: the solve has listed each of them, so they are open on nothing but the two objects that stand for others.
inline OptionSet<Actor> actorsOpenOn(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    if (ValueFlowResultInputs::isPlaceholder(object))
        return everyActor;

    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    OptionSet<ObjectMutation> marked = markedMutationsOf(inputs, object);
    OptionSet<Actor> open;

    if (marked.contains(ObjectMutation::ReachUnknownJavaScript) || inputs.isLost(object))
        open.add(Actor::UnseenCode);
    if (inputs.isHeldByNativeCode(object) || !inputs.sets().contains(tables.objectsWithoutPropertiesOfNativeCode(), object))
        open.add(Actor::NativeCode);

    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
        if (inputs.sets().contains(tables.holdersOfUndeclaredPropertiesUnderIndexes(), object))
            open.add(Actor::Table);
        break;
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::CoroutineResult:
        open.add({ Actor::Table, Actor::Creation });
        break;
    case ValueFlowObjectKind::Namespace:
        open.remove(Actor::NativeCode);
        break;
    case ValueFlowObjectKind::Site:
        switch (universe.siteOf(object)->origin) {
        case ValueFlowSiteOrigin::Operation:
        case ValueFlowSiteOrigin::DataNode:
            break;
        case ValueFlowSiteOrigin::BuiltinResult:
        case ValueFlowSiteOrigin::TypedResult:
            open.add({ Actor::Table, Actor::Creation });
            break;
        }
        break;
    case ValueFlowObjectKind::Closure:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::Prototype:
        break;
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::Key:
        return { };
    case ValueFlowObjectKind::UnknownObject:
        return everyActor;
    }
    return open;
}

inline OptionSet<Actor> actorsOpenOn(const ValueFlowResultInputs& inputs, ValueSetID holders)
{
    if (ValueSets::isComplement(holders))
        return everyActor;
    OptionSet<Actor> open;
    inputs.sets().forEachMember(holders, [&](ValueFlowObjectID object) {
        open.add(actorsOpenOn(inputs, object));
    });
    return open;
}

// One part for each kind of place, each filled in a file of its own. A part answers nothing for a place that is not its own.
class LedgerPart {
public:
    virtual ~LedgerPart() = default;
    virtual std::optional<Writers> writersOf(const Place&) const { return std::nullopt; }
    virtual std::optional<Readers> readersOf(const Place&) const { return std::nullopt; }
    virtual std::optional<Reachers> reachersOf(ValueFlowObjectID) const { return std::nullopt; }
};

enum class LedgerPartKind : uint8_t { Variables, Fields, ClassesAndClosures, BuiltinLocations };
inline constexpr unsigned numberOfLedgerParts = 4;

class Ledger {
    WTF_MAKE_NONCOPYABLE(Ledger);
public:
    explicit Ledger(const ValueFlowResultInputs& inputs)
        : m_inputs(inputs)
    {
    }

    void install(LedgerPartKind kind, const LedgerPart& part) { m_parts[static_cast<unsigned>(kind)] = &part; }

    // A place that no part knows is open to everybody who can write there.
    Writers writersOf(const Place& place) const
    {
        for (const LedgerPart* part : m_parts) {
            if (std::optional<Writers> writers = part ? part->writersOf(place) : std::nullopt)
                return *writers;
        }
        return { { }, actorsThatCanWrite(place.kind()) };
    }

    Readers readersOf(const Place& place) const
    {
        for (const LedgerPart* part : m_parts) {
            if (std::optional<Readers> readers = part ? part->readersOf(place) : std::nullopt)
                return *readers;
        }
        return { { }, actorsThatCanRead(place.kind()) };
    }

    Reachers reachersOf(ValueFlowObjectID object) const
    {
        for (const LedgerPart* part : m_parts) {
            if (std::optional<Reachers> reachers = part ? part->reachersOf(object) : std::nullopt)
                return *reachers;
        }
        return { { }, everyActor };
    }

    OptionSet<Actor> openOn(ValueFlowObjectID object) const { return actorsOpenOn(m_inputs, object); }
    OptionSet<Actor> openOn(ValueSetID holders) const { return actorsOpenOn(m_inputs, holders); }
    bool isOpenTo(Actor actor, ValueFlowObjectID object) const { return openOn(object).contains(actor); }

private:
    const ValueFlowResultInputs& m_inputs;
    std::array<const LedgerPart*, numberOfLedgerParts> m_parts { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

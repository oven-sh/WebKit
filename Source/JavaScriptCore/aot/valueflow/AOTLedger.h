/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLedgerEntries.h"
#include "AOTPlaces.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsInternal.h"

namespace JSC { namespace AOT { namespace Typed {

// The actor of a write is a function of the statement that asked for the written node, of the name it writes under, and of whether the statement lists the object or reaches it through one that stands for others.
inline Actor actorOf(const ValueFlowStatement& statement, ValueFlowName name, bool receiverIsListed)
{
    switch (statement.kind) {
    case ValueFlowStatementKind::Operation:
    case ValueFlowStatementKind::Lookup:
    case ValueFlowStatementKind::CallGroup:
    case ValueFlowStatementKind::Coroutine:
        if (!receiverIsListed)
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

inline Actor actorOf(const ValueFlowResultInputs& inputs, const ValueFlowStatement& statement, ValueSetID writtenThrough, ValueFlowName name)
{
    return actorOf(statement, name, !ValueSets::isComplement(writtenThrough) && !inputs.sets().meet(writtenThrough, inputs.universe().placeholdersAlone()));
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
// A part that does not follow a write through one of the two to the objects that it stands for says so itself.
// `classesAbove`: what whoever has walked the chain of the object found there. Nobody has, unless he says so.
enum class ClassesAbove : uint8_t { MayListNoOwnNames, ListOwnNames };

inline OptionSet<Actor> actorsOpenOn(const ValueFlowResultInputs& inputs, ValueFlowObjectID object, ClassesAbove classesAbove = ClassesAbove::MayListNoOwnNames)
{
    if (ValueFlowResultInputs::isPlaceholder(object))
        return everyActor;

    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    OptionSet<ObjectMutation> marked = markedMutationsOf(inputs, object);
    OptionSet<Actor> open;

    if (marked.contains(ObjectMutation::ReachUnknownJavaScript))
        open.add(Actor::UnseenCode);
    if (FunctionID function = universe.closureFunction(object); function && (!inputs.functionsNamedByUnsolvedCode || inputs.functionsNamedByUnsolvedCode->get(function.index())))
        open.add(Actor::UnseenCode);
    bool isMadeWithoutNativeCode = universe.kindOf(object) == ValueFlowObjectKind::Namespace || inputs.sets().contains(tables.objectsWithoutPropertiesOfNativeCode(), object);
    // A built-in makes the object, or the constructor at the top of its classes may be one. The table lists what each such class lays on a new instance.
    bool isAnsweredForByClasses = classesAbove == ClassesAbove::ListOwnNames && inputs.sets().contains(tables.objectsThatTheirClassAnswersFor(), object);
    if (inputs.isHeldByNativeCode(object) || (!isMadeWithoutNativeCode && !isAnsweredForByClasses))
        open.add(Actor::NativeCode);

    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
        if (inputs.sets().contains(tables.holdersOfUndeclaredPropertiesUnderIndexes(), object))
            open.add(Actor::Table);
        break;
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::CoroutineResult:
        if (!isAnsweredForByClasses)
            open.add({ Actor::Table, Actor::Creation });
        break;
    case ValueFlowObjectKind::Site:
        switch (universe.siteOf(object)->origin) {
        case ValueFlowSiteOrigin::Operation:
        case ValueFlowSiteOrigin::DataNode:
            break;
        case ValueFlowSiteOrigin::TypedResult:
            // The rule of the call writes each declared field and what may lie under any other key, and it marks the object.
            break;
        case ValueFlowSiteOrigin::BuiltinResult:
            if (!isAnsweredForByClasses)
                open.add({ Actor::Table, Actor::Creation });
            break;
        }
        break;
    case ValueFlowObjectKind::Namespace:
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

// A class speaks through its prototype object, which stands on the chain of whatever is made below it.
inline ClassesAbove classesAbove(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, ValueFlowObjectID object)
{
    const ValueFlowHeapTables& tables = inputs.universe().builtinTables().heap();
    if (!inputs.sets().contains(tables.objectsThatTheirClassAnswersFor(), object))
        return ClassesAbove::MayListNoOwnNames;
    ValueSetID chain = heap.chainOf(maker, maker.of(object));
    bool listOwnNames = !ValueSets::isComplement(chain) && !inputs.sets().meet(chain, tables.prototypeObjectsOfClassesThatListNoOwnNames());
    return listOwnNames ? ClassesAbove::ListOwnNames : ClassesAbove::MayListNoOwnNames;
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

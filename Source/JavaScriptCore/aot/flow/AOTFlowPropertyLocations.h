/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowPropertyOperations.h"
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowWalkInputs {
    const Program& program;
    const FlowObjectTable& objects;
    const FlowGraph& graph;
    const FlowRecords& records;
    const FlowPropertyState& state;
    FlowSolver* solver { nullptr };

    static FlowWalkInputs of(FlowSolver&);
    static FlowWalkInputs of(const FlowSolution&);

    bool isSolving() const { return solver; }
    const BuiltinTable& builtins() const { return program.builtins(); }
    bool hasReachedNativeCode(FlowObjectID object) const { return hasReached(graph.nativeCode(), ObjectMutation::ReachNativeCode, object); }
    bool hasReachedUnknownJavaScript(FlowObjectID object) const { return hasReached(graph.unknownJavaScript(), ObjectMutation::ReachUnknownJavaScript, object); }

private:
    // The solver records the mutation on each object that it adds to the node, but for the unknown object.
    bool hasReached(FlowNodeID unseenCode, ObjectMutation mutation, FlowObjectID object) const
    {
        if (object == unknownFlowObject)
            return graph.valueOf(unseenCode).objects.containsUnknownObject();
        return records.possibleMutations(object).contains(mutation);
    }
};

struct FlowHolder {
    FlowObjectID object;
    BuiltinID builtin;

    friend bool operator==(const FlowHolder&, const FlowHolder&) = default;
};

FlowHolder holderOf(const FlowObjectTable&, FlowObjectID);
FlowHolder holderOf(const FlowWalkInputs&, BuiltinID);

enum class FlowHolderMutability : uint8_t {
    Ordinary,
    Immutable,
};

FlowHolderMutability mutabilityOf(const FlowWalkInputs&, const FlowHolder&);
// A built-in object with locked members is ordinary as a whole, and immutable at those members, at the keys it is closed for and at its prototype link.
FlowHolderMutability mutabilityOf(const FlowWalkInputs&, const FlowHolder&, PropertyKeyID);
FlowHolderMutability mutabilityOfIndexes(const FlowWalkInputs&, const FlowHolder&);
FlowHolderMutability mutabilityOfPrototypeLink(const FlowWalkInputs&, const FlowHolder&);

// What is stored, defined or deleted through an unknown receiver may hit an object that has reached native code, so such an object shares its locations with the unknown object.
// So does a built-in constructor that the program may hold as an unknown value. It has not reached native code for that: nothing calls it, and no member follows it.
bool sharesLocationsWithUnknownObject(const FlowWalkInputs&, FlowObjectID);
bool aliasesUnknownObject(const FlowWalkInputs&, const FlowHolder&);

enum class FlowOwnLookup : uint8_t {
    Unknown,
    Absent,
    Found,
};

enum class FlowOwnPresence : uint8_t {
    MayBeAbsent,
    ShownByValue,
};

enum class FlowOwnEnumerability : uint8_t {
    MayBeEnumerable,
    NeverEnumerable,
};

struct FlowOwnProperty {
    FlowOwnLookup lookup { FlowOwnLookup::Unknown };
    FlowOwnPresence presence { FlowOwnPresence::MayBeAbsent };
    PropertyKeyID key;
    FlowNodeID value;
    FlowNodeID getter;
    FlowNodeID setter;
    BuiltinID builtin;
    FlowOwnEnumerability enumerability { FlowOwnEnumerability::MayBeEnumerable };
};

enum class FlowOwnListing : uint8_t {
    MayHaveOthers,
    Complete,
};

using FlowOwnPropertyFunctor = ScopedLambda<void(const FlowOwnProperty&)>;

FlowOwnProperty findOwnProperty(const FlowWalkInputs&, const FlowHolder&, PropertyKeyID);
FlowOwnListing forEachOwnProperty(const FlowWalkInputs&, const FlowHolder&, NOESCAPE const FlowOwnPropertyFunctor&);
FlowOwnListing ownListingOf(const FlowWalkInputs&, const FlowHolder&);
// An accessor or a read-only property keeps a store to an object that inherits it from creating a property of that object.
bool stopsStoresFromBelow(const FlowWalkInputs&, const FlowHolder&, PropertyKeyID);
// A function that is created with a prototype property has it for good: the property is not configurable, so nothing deletes it or turns it into an accessor.
bool isPrototypePropertyOfFunction(const FlowWalkInputs&, const FlowHolder&, PropertyKeyID);

FlowOwnProperty findOwnBuiltinProperty(const FlowWalkInputs&, const FlowHolder&, PropertyKeyID);
FlowOwnListing forEachOwnBuiltinProperty(const FlowWalkInputs&, const FlowHolder&, NOESCAPE const FlowOwnPropertyFunctor&);
// For an operation that has the holder among its receivers and a key nobody can name.
void noteWritesToMembersOf(FlowSolver&, FlowObjectID holder, const Witness&);

FlowNodeID ensureOwnValue(FlowSolver&, FlowObjectID holder, PropertyKeyID);
FlowNodeID ensureOwnAccessor(FlowSolver&, FlowObjectID holder, PropertyKeyID, MethodKind);

constexpr std::optional<FlowOpenPart> unnamedKeyPartOf(FlowKeyClass keyClass)
{
    switch (keyClass) {
    case FlowKeyClass::String:
        return FlowOpenPart::StringProperties;
    case FlowKeyClass::Index:
        return FlowOpenPart::IndexedProperties;
    case FlowKeyClass::Symbol:
        return FlowOpenPart::SymbolProperties;
    case FlowKeyClass::PrivateName:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool holdsUnnamedKeys(FlowOpenPart part)
{
    switch (part) {
    case FlowOpenPart::IndexedProperties:
    case FlowOpenPart::StringProperties:
    case FlowOpenPart::SymbolProperties:
        return true;
    case FlowOpenPart::PrototypeLink:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

std::optional<FlowOpenPart> unnamedKeyPartOf(FlowNodeKind);

// No such node says under which key a value lies, so what it holds beside the unknown object predicts nothing about an access.
inline bool holdsValuesOfUnnamedKeys(FlowNodeKind kind)
{
    return kind == FlowNodeKind::UndeclaredPart || unnamedKeyPartOf(kind);
}

FlowKeySet keysHeldBy(FlowOpenPart unnamedKeyPart);
bool hasUnnamedKeyStores(const FlowWalkInputs&, FlowObjectID holder, FlowOpenPart);
// The constructor of a built-in class has put the characters of a string on the object, under indexes. IndexedProperties holds them from its creation on.
bool isCreatedWithCharacters(const FlowWalkInputs&, FlowObjectID);

struct FlowIndexedLocation {
    FlowNodeID node;
    PropertyKeyID key;
};

enum class FlowIndexedStorage : uint8_t {
    OpenPart,
    ElementParts,
};

FlowIndexedStorage forEachIndexedLocation(const FlowWalkInputs&, FlowObjectID holder, Range indexes, NOESCAPE const ScopedLambda<void(const FlowIndexedLocation&)>&);

VariableID findStaticFieldVariable(const Program&, FunctionID classConstructor, PropertyKeyID);

TypeID enforcedTypeOf(const FlowGraph&, FlowNodeID location);

void linkUnnamedKeyStores(FlowSolver&, FlowObjectID holder, FlowOpenPart);
void linkAliasOfLocation(FlowSolver&, FlowObjectID escapedObject, PropertyKeyID, FlowNodeID location);
void linkAliasOfUnnamedKeys(FlowSolver&, FlowObjectID escapedObject, FlowOpenPart);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

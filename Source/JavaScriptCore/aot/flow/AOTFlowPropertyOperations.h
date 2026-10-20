/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolution.h"
#include <memory>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

std::unique_ptr<FlowPropertyState, FlowPropertyStateDeleter> createFlowPropertyState(FlowSolver&);

enum class FlowWalkOperation : uint8_t {
    Get,
    Set,
    DefineOwnProperty,
    CopyDataProperties,
    HasProperty,
    Delete,
};

enum class FlowBuiltinMemberLookup : uint8_t {
    Every,
    ExceptConstructorLinks,
    None,
};

// A built-in that names no member runs the members the program defined: the engine's own reach no code of the program but through those and through the contents.
inline FlowBuiltinMemberLookup builtinMembersFoundByProtocol(const FlowKeySet& keys)
{
    return keys.hasUnnamedKeys() ? FlowBuiltinMemberLookup::None : FlowBuiltinMemberLookup::ExceptConstructorLinks;
}

struct FlowPropertyWalk {
    FlowWalkOperation operation { FlowWalkOperation::Get };
    bool startsAtPrototype { false };
    FlowStatementSite waiter;
    bool isOwnOnly { false };
    bool isEnumerableOnly { false };
    FlowBuiltinMemberLookup builtinMembers { FlowBuiltinMemberLookup::Every };
    // The node that the receiver came out of. None where the rule names the receiver itself.
    FlowNodeID receivers;
    // The object at which the walk starts is looked at on behalf of another one, which the operation names beside it: the base of an access through super.
    bool startsAboveReceiver { false };
};

enum class FlowPropertySourceKind : uint8_t {
    Unknown,
    Absent,
    Value,
    Getter,
    Setter,
    BuiltinMember,
    BuiltinGetter,
    BuiltinSetter,
};

struct FlowPropertySource {
    FlowPropertySourceKind kind { FlowPropertySourceKind::Unknown };
    FlowCertainty certainty { FlowCertainty::Predicted };
    bool isOwn { false };
    FlowObjectID holder;
    PropertyKeyID key;
    FlowNodeID node;
    BuiltinID builtin;
    // The table has the member under the key, and nothing says that the receiver inherits from its holder.
    bool isFoundThroughUnknownObject { false };

    friend bool operator==(const FlowPropertySource&, const FlowPropertySource&) = default;
};

// Whatever a location of a holder in nativeCode holds has reached nativeCode too. With the unknown object in it, the location says no more than the unknown source does.
// Taking such a source as a node is as sound: the answer may change between two walks.
inline bool saysNoMoreThanUnknownSource(const FlowSolver& solver, const FlowPropertySource& source)
{
    return source.holder
        && solver.valueOf(source.node).hasUnknownObject()
        && solver.records().possibleMutations(source.holder).contains(ObjectMutation::ReachNativeCode);
}

struct FlowPrimitiveReceiver {
    uint16_t primitives { 0 };
};

using FlowPropertySourceFunctor = ScopedLambda<void(const FlowPropertySource&)>;

FlowLocationEvent readEventOf(const FlowSolver&, const FlowPropertySource&, FlowReadUse);

void runFieldFilter(FlowSolver&, FlowStatementSite, const FlowFilter&);

void forEachPropertySource(FlowSolver&, FlowObjectID receiver, const FlowKeySet&, const FlowPropertyWalk&, NOESCAPE const FlowPropertySourceFunctor&);
void forEachPropertySource(const FlowSolution&, FlowObjectID receiver, const FlowKeySet&, const FlowPropertyWalk&, NOESCAPE const FlowPropertySourceFunctor&);
void forEachPropertySource(FlowSolver&, FlowPrimitiveReceiver, const FlowKeySet&, const FlowPropertyWalk&, NOESCAPE const FlowPropertySourceFunctor&);

// A kept lookup registers no waiter. Whatever wakes the waiters on something also calls the function for that thing here.
void forgetAnswersOfLookupsThrough(FlowSolver&, FlowObjectID);
void keepWhatProvenLookupsRelyOn(FlowSolver&);
void forgetAnswersOfLookupsWaitingOnMutations(FlowSolver&, FlowObjectID);
void forgetAnswersOfLookupsWaitingOnProperty(FlowSolver&, FlowObjectID, PropertyKeyID);

void forEachPrototype(FlowSolver&, FlowObjectID, FlowStatementSite waiter, NOESCAPE const ScopedLambda<void(FlowObjectID)>&);
void forEachPrototype(const FlowSolution&, FlowObjectID, NOESCAPE const ScopedLambda<void(FlowObjectID)>&);

enum class FlowPrototypeEnd : uint8_t {
    Unknown,
    Null,
};

FlowPrototypeEnd prototypeEndOf(const FlowSolution&, FlowObjectID);

// A store of the program under one key has written the property of its receiver when it completes. A built-in may leave a store out: it copies what its source has
// at that time, and defines what its descriptor says. Where it knows that it leaves nothing out, its access says so.
enum class FlowStoreCompletion : bool { MayLeaveStoreOut, HasStored };

struct FlowAccess {
    FlowStatementSite site;
    ValueID origin;
    FlowAccessKind kind { FlowAccessKind::Load };
    FlowReadUse use { FlowReadUse::Value };
    OptionSet<FlowAccessFlag> flags;
    FlowNodeID receiver;
    FlowNodeID value;
    FlowNodeID thisValue;
    FlowNodeID thrown;
    FlowBuiltinMemberLookup builtinMembers { FlowBuiltinMemberLookup::Every };
    FlowStoreCompletion completion { FlowStoreCompletion::MayLeaveStoreOut };
};

// The unknown object stands for every object in nativeCode: what a statement does to it, it does to them. So where the node of the receivers holds the unknown object,
// the lookup from a receiver in nativeCode only predicts, and an operation has an arm for each predicted receiver or for none.
// Answers the receivers without those that are too many to predict, or nothing where every receiver has its lookup.
std::optional<FlowNewObjects> receiversBesideUnknownObject(FlowSolver&, const FlowNewObjects& receivers, FlowNodeID receiverNode);
// For an operation that reads whatever its holders hold. A lookup by name through the unknown object finds the members of built-in objects in the table, and such an operation names nothing.
std::optional<FlowNewObjects> holdersBesideUnknownObject(FlowSolver&, const FlowNewObjects& holders, FlowNodeID holderNode);

void accessProperties(FlowSolver&, const FlowAccess&, const FlowNewObjects& receivers, const FlowKeySet&);
void accessProperties(FlowSolver&, const FlowAccess&, FlowPrimitiveReceiver, const FlowKeySet&);

enum class FlowCopyOperation : uint8_t {
    CopyDataProperties,
    Assign,
};

struct FlowPropertyCopyRequest {
    FlowStatementSite site;
    ValueID origin;
    FlowCopyOperation operation { FlowCopyOperation::CopyDataProperties };
    FlowNodeID sources;
    FlowNodeID targets;
    FlowNodeID thrown;
    FlowKeySet excludedKeys;
    bool mayRunRepeatedly { true };
    // Each run copies from every object that the node of the sources holds.
    bool copiesFromEverySource { false };
};

void copyProperties(FlowSolver&, const FlowPropertyCopyRequest&);
void definePropertiesFromDescriptor(FlowSolver&, const FlowAccess&, const FlowNewObjects& receivers, const FlowKeySet&, FlowNodeID descriptor);
void definePropertiesFromDescriptors(FlowSolver&, const FlowAccess&, const FlowNewObjects& receivers, FlowNodeID descriptorMap);
// The properties under the keys may stop being writable, enumerable, configurable data properties.
void noteAttributeChanges(FlowSolver&, const FlowAccess&, const FlowNewObjects& receivers, const FlowKeySet&);

void connectCreationPrototype(FlowSolver&, FlowObjectID, FlowNodeID prototypes);
// For an object that is created where only the unknown object stands for it. Nobody walks on from the unknown object, which stands for what has reached native code, so what the new object inherits from goes there.
// A link of the unknown object would say that every escaped object may change its prototype. The caller waits on the node.
void connectCreationPrototypeOfUnknownObject(FlowSolver&, FlowNodeID prototypes, const Witness&);
void changePrototype(FlowSolver&, const FlowObjectSet& objects, FlowNodeID prototypes, const Witness&);
void loadPrototypes(FlowSolver&, FlowStatementSite, const FlowNewObjects& objects, FlowNodeID result, const Witness&);
void loadPrototypes(FlowSolver&, FlowPrimitiveReceiver, FlowNodeID result);

void didCreateLocation(FlowSolver&, FlowObjectID, PropertyKeyID, FlowNodeID);
void didEscape(FlowSolver&, FlowObjectID);
// What the unknown object holds under the name constructor is the constructor of whatever it stands for, and the read answers unknown.
void letConstructorsShareLocationsWithUnknownObject(FlowSolver&);

void runLocationAccess(FlowSolver&, FlowStatementSite, const FlowLocationAccess&);
void runPropertyAccess(FlowSolver&, FlowStatementSite, const FlowPropertyAccess&);
void runElementAccess(FlowSolver&, FlowStatementSite, const FlowElementAccess&);
void runPrototypeAccess(FlowSolver&, FlowStatementSite, const FlowPrototypeAccess&);
void runPropertyCopy(FlowSolver&, FlowStatementSite, const FlowPropertyCopy&);
void runMethodDefinition(FlowSolver&, FlowStatementSite, const FlowMethodDefinition&);

FlowProgress refineConstructions(FlowSolver&);

struct FlowConstructionResult {
    ThisLeak thisLeak { ThisLeak::Unknown };
    FieldCreation fieldCreation { FieldCreation::Unknown };
    Vector<FieldOrder> constructorOrders;
    BitVector fieldsSetAtCreation;
    Witness leakWitness { noWitness };
};

FlowConstructionResult constructionOf(const FlowSolution&, LayoutID);
bool seesOnlyFinishedObjects(const FlowSolution&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

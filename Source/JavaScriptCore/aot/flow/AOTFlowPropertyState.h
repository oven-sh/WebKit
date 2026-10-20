/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowConstruction.h"
#include "AOTFlowPropertyOperations.h"
#include "AOTFlowSymbols.h"
#include <algorithm>
#include <array>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowPropertyKeys {
    PropertyKeyID prototype;
    PropertyKeyID constructor;
    PropertyKeyID value;
    PropertyKeyID get;
    PropertyKeyID set;
    PropertyKeyID writable;
    PropertyKeyID enumerable;
    PropertyKeyID configurable;
};

struct FlowMemberOfBuiltin {
    BuiltinID holder;
    BuiltinID member;
};

using FlowMembersOfBuiltins = Vector<FlowMemberOfBuiltin, 4>;

using FlowAbsentName = std::pair<BuiltinID, PropertyKeyID>;

// What start-up fixes because a walk has answered by it: members taken for constants, names passed as absent, holders passed on to the prototype that the table names.
struct FlowFixedAtStartUp {
    bool isEmpty() const { return members.isEmpty() && absentNames.isEmpty() && prototypeLinks.isEmpty(); }

    Vector<BuiltinID> members;
    Vector<FlowAbsentName> absentNames;
    Vector<BuiltinID> prototypeLinks;
};

// What a walk from one receiver under one set of keys reports to any statement, kept until something that the walk read changes.
struct FlowLookup {
    Vector<FlowPropertySource> sources;
    // What the walk has looked at under a name and reports to nobody, because it is no answer to the operation.
    Vector<FlowPropertySource> passedMembers;
    Vector<FlowLookupWait> waits;
    // A walk does not wait on what something else covers for the statement that walked. Whoever asks later gets what a walk finds then.
    Vector<FlowObjectID> visitedObjects;
    // Of the visited objects and of what the walk waits on.
    uint64_t sumOfVersions { 0 };
    uint32_t numberOfHolders { 0 };
    bool readsUnnamedKeysOfUnknownObject { false };
    bool passesHolderWithoutName { false };
    bool isCurrent { false };
    // None for a primitive.
    FlowObjectID receiver;
    // Of every walk of the lookup: whoever took an earlier answer has acted on it.
    FlowFixedAtStartUp reliesOn;
};

struct FlowLookups {
    void forgetAnswers()
    {
        for (FlowLookup& lookup : all)
            lookup.isCurrent = false;
    }

    void didChange(FlowObjectID object)
    {
        size_t index = object.index();
        if (index >= versionOfObject.size())
            versionOfObject.insertFill(versionOfObject.size(), 0u, std::max(index + 1, versionOfObject.size() * 2) - versionOfObject.size());
        ++versionOfObject[index];
        ++numberOfChanges;
    }

    void didChangeMutations(FlowObjectID object)
    {
        didChange(object);
        if (object == unknownFlowObject)
            ++versionOfMutationsOfUnknownObject;
    }

    void didChangeProperty(FlowObjectID object, PropertyKeyID key)
    {
        didChange(object);
        if (object == unknownFlowObject)
            ++versionsOfNamesOfUnknownObject.add(key, 0).iterator->value;
    }

    uint64_t versionOf(FlowObjectID object) const { return object.index() < versionOfObject.size() ? versionOfObject[object.index()] : 0; }

    // Every escape creates locations on the unknown object, so a wait on its mutations or on one of its names has a version of its own.
    uint64_t versionOf(const FlowLookupWait& wait) const
    {
        if (wait.object != unknownFlowObject || wait.node)
            return versionOf(wait.object);
        if (!wait.mutations.isEmpty())
            return versionOfMutationsOfUnknownObject;
        return wait.key ? versionsOfNamesOfUnknownObject.get(wait.key) : versionOf(unknownFlowObject);
    }

    void noteAnswerRelyingOnStartUp(uint32_t lookup, FlowNodeID receivers) { answersRelyingOnStartUp.add((static_cast<uint64_t>(lookup) + 1) << 32 | receivers.index()); }

    uint64_t sumOfVersions(const FlowLookup& lookup) const
    {
        uint64_t sum = 0;
        for (FlowObjectID object : lookup.visitedObjects)
            sum += versionOf(object);
        for (const FlowLookupWait& wait : lookup.waits)
            sum += versionOf(wait);
        return sum;
    }

    // By receiver, operation and flags of the walk, and by the one name or the number of the set of keys.
    HashMap<std::pair<uint64_t, uint64_t>, uint32_t> indexes;
    SegmentedVector<FlowLookup, 256> all;
    Vector<FlowKeySet> keySets;
    HashedIndex numbersOfKeySets;
    // Goes up with each mutation of the object, each location created on it, each change of its prototype link, its first store under unnamed keys and each wake-up of those who wait on it.
    Vector<uint32_t> versionOfObject;
    uint64_t versionOfMutationsOfUnknownObject { 0 };
    uint64_t numberOfChanges { 0 };
    HashMap<PropertyKeyID, uint32_t> versionsOfNamesOfUnknownObject;
    bool answersAreOfFixpointCheck { false };
    FlowLookup* lookupBeingWalked { nullptr };
    // One more than the index of a lookup that relies on what start-up fixes, above the index of the node that its receiver came out of for somebody who took the answer.
    HashSet<uint64_t> answersRelyingOnStartUp;
    uint64_t numberOfWalks { 0 };
    uint64_t numberOfAnswersTaken { 0 };
};

// In the order in which they were added.
struct FlowAccessorList {
    using Accessor = std::pair<FlowObjectID, PropertyKeyID>;

    void add(FlowObjectID holder, PropertyKeyID key)
    {
        if (listed.add({ holder, key }).isNewEntry)
            accessors.append({ holder, key });
    }

    Vector<Accessor> accessors;
    HashSet<Accessor> listed;
};

struct FlowPropertyState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowPropertyState);
    WTF_MAKE_NONCOPYABLE(FlowPropertyState);

    FlowPropertyState() = default;

    bool hasLinkedUnnamedKeys(FlowObjectID object, FlowOpenPart part) const { return objectsWithLinkedUnnamedKeys[static_cast<unsigned>(part)].get(object.index()); }
    bool setHasLinkedUnnamedKeys(FlowObjectID object, FlowOpenPart part)
    {
        lookups.didChange(object);
        return objectsWithLinkedUnnamedKeys[static_cast<unsigned>(part)].set(object.index());
    }

    void takeForConstant(BuiltinID member)
    {
        membersTakenForConstants.set(member.index());
        lookups.lookupBeingWalked->reliesOn.members.appendIfNotContains(member);
    }

    void takeForAbsent(const FlowAbsentName& name)
    {
        namesTakenForAbsent.add(name);
        lookups.lookupBeingWalked->reliesOn.absentNames.appendIfNotContains(name);
    }

    void takeLinkForConstant(BuiltinID holder)
    {
        holdersWithLinkTakenForConstant.set(holder.index());
        lookups.lookupBeingWalked->reliesOn.prototypeLinks.appendIfNotContains(holder);
    }

    FlowPropertyKeys keys;
    // The own properties that every instance of a class with indexed elements is created with.
    Vector<PropertyKeyID, 2> ownKeysOfLists;
    TypeID objectOrNull;
    std::array<BitVector, numberOfFlowOpenParts> objectsWithLinkedUnnamedKeys;
    // By name, what a lookup through the unknown object still finds on the built-in objects. A member that has escaped leaves the list for good, unless a store can run its setter.
    HashMap<PropertyKeyID, FlowMembersOfBuiltins> membersFoundByName;
    // The members of the realm's built-in objects that are no prototypes, sorted by name.
    Vector<FlowMemberOfBuiltin> membersOutsidePrototypes;
    // The setters on built-in prototypes whose entries do more to the receiver than store a property. Any other setter is native code that changes nothing it is handed.
    FlowMembersOfBuiltins settersChangingReceivers;
    // The members that start-up can make immutable, and among them the ones that a lookup has answered with although the holder aliases the unknown object: start-up does make those immutable.
    BitVector membersLockedWhileConstant;
    BitVector membersTakenForConstants;
    // The names that a lookup has passed as absent on a built-in prototype that unseen code reaches: the run time refuses whoever defines one of them there.
    HashSet<FlowAbsentName> namesTakenForAbsent;
    // What the program itself puts on a built-in object: the names that an operation with the object among its receivers stores or defines, and the objects that one writes under a key nobody can name.
    HashSet<FlowAbsentName> namesWrittenOnBuiltins;
    BitVector builtinsWrittenUnderUnnamedKeys;
    // The built-in prototypes without a locked prototype link that a lookup has passed under a name taken for absent. Start-up fixes the link, so it changes only where the holder itself is among the receivers.
    BitVector holdersWithLinkTakenForConstant;
    BitVector builtinsWithPrototypeChangedByProgram;
    // Every built-in constructor of the realm, from the first read of the name constructor or of a key nobody can name through the unknown object on: the program may hold any of them as an unknown value.
    Vector<FlowObjectID> constructorsHeldAsUnknownValues;
    // The node whose objects a built-in makes the prototype of the object. No node where calls with different nodes do that.
    HashMap<FlowObjectID, FlowNodeID> prototypesGivenByBuiltin;
    // The node whose objects a map of descriptors describes, for a map that a call in the instance has made of the own properties of its operand.
    HashMap<std::pair<FlowObjectID, FlowInstanceID>, FlowNodeID> objectsDescribedByMap;
    // The keys under which an element access has accessed the receivers that came out of its cursor.
    HashMap<FlowStatementSite, FlowKeySet> keysOfElementAccesses;
    FlowLookups lookups;
    // The accessors that an operation defines and that a store has taken to be there at every lookup of the name. The order of execution confirms each one or takes it back for good.
    FlowAccessorList accessorsAssumedPresent;
    // What a definition has found a descriptor node to be so far (FlowDescriptorForm), for the receivers that came out of its cursor.
    HashMap<std::pair<FlowStatementSite, FlowNodeID>, uint8_t> formsOfDescriptors;
    // Whether a call that copies properties has found one object per argument so far, for the sources that came out of its cursor.
    HashMap<FlowStatementSite, bool> copiesWithOneObjectPerArgument;
    FlowSymbolState symbols;
    FlowConstructionState constructions;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

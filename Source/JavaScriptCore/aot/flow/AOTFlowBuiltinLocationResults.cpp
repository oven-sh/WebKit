/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowBuiltinLocations.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowResultFamilies.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<FlowObjectEvent> unnamedKeyWriteEvents {
    FlowObjectEvent::StoredByUnknownKey,
    FlowObjectEvent::DeletedByUnknownKey,
};

static constexpr OptionSet<FlowLocationEvent> locationWriteEvents {
    FlowLocationEvent::WrittenByUnknownCode,
    FlowLocationEvent::StoredByOperation,
    FlowLocationEvent::StoredByManyOperations,
    FlowLocationEvent::Deleted,
    FlowLocationEvent::Redefined,
};

namespace {

struct BuiltinObjectScan {
    explicit BuiltinObjectScan(unsigned builtinCount)
        : holdersWithUnnamedWrites(builtinCount)
        , holdersWithChangedPrototype(builtinCount)
        , escapedObjectByHolder(builtinCount)
        , writtenMembers(builtinCount)
        , firstWriteByMember(FillWith { }, builtinCount, noWitness)
    {
    }

    BitVector holdersWithUnnamedWrites;
    BitVector holdersWithChangedPrototype;
    Vector<FlowObjectID> escapedObjectByHolder;
    BitVector writtenMembers;
    Vector<Witness> firstWriteByMember;
    Vector<BuiltinLocationFlow> addedProperties;
};

struct LocationWrites {
    void add(const Witness& witness)
    {
        mayHappen = true;
        keepLowerWitness(first, witness);
    }

    bool mayHappen { false };
    Witness first { noWitness };
};

struct MemberLocation {
    BuiltinLocationFlow flow;
    bool isListed { false };
};

} // anonymous namespace

static FlowLocationFacts locationFactsOf(const FlowRecords& records, const FlowProperty& property)
{
    FlowLocationFacts facts;
    for (FlowNodeID node : { property.value, property.getter, property.setter }) {
        if (node)
            facts.add(records.recordOf(node));
    }
    if (property.getter || property.setter)
        facts.events.add(FlowLocationEvent::Redefined);
    return facts;
}

static void scanBuiltinObject(const FlowFamilyInputs& inputs, FlowObjectID object, BuiltinObjectScan& scan)
{
    BuiltinID holder = holderOf(inputs.objects(), object).builtin;
    const FlowObjectFacts& facts = inputs.factsOf(object);

    if (facts.mutations.contains(ObjectMutation::ReachUnknownJavaScript) || facts.events.containsAny(unnamedKeyWriteEvents))
        scan.holdersWithUnnamedWrites.quickSet(holder.index());
    if (facts.mutations.contains(ObjectMutation::ChangePrototype))
        scan.holdersWithChangedPrototype.quickSet(holder.index());
    if (facts.mutations.contains(ObjectMutation::ReachNativeCode) && !scan.escapedObjectByHolder[holder.index()])
        scan.escapedObjectByHolder[holder.index()] = object;

    for (const FlowProperty& property : inputs.graph().propertiesOf(object)) {
        FlowLocationFacts location = locationFactsOf(inputs.records(), property);
        if (!location.events.containsAny(locationWriteEvents))
            continue;

        BuiltinID member = findOwnMember(inputs.program(), holder, property.key);
        if (!member) {
            scan.addedProperties.append(BuiltinLocationFlow { holder, property.key, BuiltinLocationState::Unknown, location.firstStore });
            continue;
        }
        scan.writtenMembers.quickSet(member.index());
        keepLowerWitness(scan.firstWriteByMember[member.index()], location.firstStore);
    }
}

static BuiltinObjectScan scanBuiltinObjects(const FlowFamilyInputs& inputs)
{
    const FlowObjectTable& objects = inputs.objects();
    Vector<uint32_t> builtinObjects = findIndicesInParallel(inputs.loops(), objects.size(), [&](unsigned index) {
        return !!holderOf(objects, FlowObjectID(index)).builtin;
    });

    BuiltinObjectScan scan(inputs.solution.builtins().builtinCount());
    for (uint32_t index : builtinObjects)
        scanBuiltinObject(inputs, FlowObjectID(index), scan);
    return scan;
}

static LocationWrites findWritesThroughHolder(const FlowFamilyInputs& inputs, const BuiltinObjectScan& scan, BuiltinID member)
{
    LocationWrites writes;
    const FlowBuiltinUse& use = inputs.records().builtinUse(member);
    if (use.uses.contains(BuiltinUse::Written))
        writes.add(use.firstWrite);
    if (scan.writtenMembers.quickGet(member.index()))
        writes.add(scan.firstWriteByMember[member.index()]);
    return writes;
}

static void addWritesThroughUnknownObject(const FlowFamilyInputs& inputs, const BuiltinObjectScan& scan, BuiltinID member, PropertyKeyID key, LocationWrites& writes)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    FlowObjectID escapedHolder = scan.escapedObjectByHolder[builtins.canonicalID(builtins.at(member).holder).index()];
    if (!escapedHolder || !aliasedEventsOf(inputs.solution, escapedHolder, key).containsAny(locationWriteEvents))
        return;
    const FlowProperty* alias = inputs.graph().findProperty(unknownFlowObject, key);
    writes.add(alias ? locationFactsOf(inputs.records(), *alias).firstStore : noWitness);
}

bool isLockedLocation(const BuiltinTable& builtins, BuiltinID member)
{
    return builtins.at(member).mutability == BuiltinMutability::AlwaysLocked || builtins.isLocationOfLockedObject(member);
}

bool canLockAtStartUp(const BuiltinTable& builtins, BuiltinID member)
{
    const Builtin& entry = builtins.at(member);
    if (!entry.holder || !entry.name || entry.mutability != BuiltinMutability::ConstantUnlessWritten || isLockedLocation(builtins, member))
        return false;
    // A realm has no object of a built-in that exists once per module, and start-up finds a holder by its path from the global object.
    return entry.scope == BuiltinScope::Realm && builtins.isUnder(member, Builtins::GlobalThis);
}

bool canCloseAbsentNames(const BuiltinTable& builtins, BuiltinID holder)
{
    if (!holder)
        return false;
    const Builtin& entry = builtins.at(holder);
    return entry.kind == BuiltinKind::Prototype && entry.scope == BuiltinScope::Realm && !entry.flags.contains(BuiltinFlag::IsLockedObject);
}

// A lookup that passes a holder goes on to the prototype that the table names.
bool canFixPrototypeLinkAtStartUp(const BuiltinTable& builtins, BuiltinID holder)
{
    return canCloseAbsentNames(builtins, holder) && !builtins.hasLockedPrototypeLink(holder);
}

// Where the program changes the link with the holder among the receivers, start-up leaves it open.
static Vector<BuiltinID> listHoldersWithFixedPrototypeLink(const FlowPropertyState& properties)
{
    Vector<BuiltinID> holders;
    for (size_t index : properties.holdersWithLinkTakenForConstant) {
        if (!properties.builtinsWithPrototypeChangedByProgram.get(index))
            holders.append(BuiltinID(index));
    }
    return holders;
}

// A name that the program defines after a lookup has passed it is found by the lookup that the definition wakes, and is open.
static Vector<ClosedAbsentName> listClosedAbsentNames(const FlowPropertyState& properties)
{
    Vector<ClosedAbsentName> names;
    for (const FlowAbsentName& name : properties.namesTakenForAbsent) {
        if (!properties.namesWrittenOnBuiltins.contains(name) && !properties.builtinsWrittenUnderUnnamedKeys.get(name.first.index()) && !properties.builtinsWithPrototypeChangedByProgram.get(name.first.index()))
            names.append({ name.first, name.second });
    }
    std::ranges::sort(names, { }, [](const ClosedAbsentName& name) {
        return std::pair { name.holder, name.key };
    });
    return names;
}

static bool hasOnlyNamedWrites(const BuiltinTable& builtins, const BuiltinObjectScan& scan, BuiltinID holder)
{
    return !scan.holdersWithUnnamedWrites.quickGet(builtins.canonicalID(holder).index());
}

// A lookup that misses on an intact holder goes on to the prototype the table names.
static bool isHolderIntact(const BuiltinTable& builtins, const BuiltinObjectScan& scan, BuiltinID holder)
{
    return hasOnlyNamedWrites(builtins, scan, holder) && !scan.holdersWithChangedPrototype.quickGet(builtins.canonicalID(holder).index());
}

static BitVector findIntactHolders(const FlowFamilyInputs& inputs, const BuiltinObjectScan& scan)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    unsigned builtinCount = builtins.builtinCount();
    BitVector intactHolders(builtinCount);
    if (inputs.records().unseenCodeCanRun())
        return intactHolders;
    for (unsigned index = 0; index < builtinCount; ++index) {
        if (inputs.isEnabled(FlowResultKind::ConstantBuiltinLocations, index) && isHolderIntact(builtins, scan, BuiltinID(index)))
            intactHolders.quickSet(index);
    }
    return intactHolders;
}

// Start-up makes a member immutable that the solver took for a constant, so whatever reaches its holder by another way than an operation with the holder among its receivers is refused.
// Nothing guards any other member.
static BuiltinLocationState stateOfUnlockedLocation(const FlowFamilyInputs& inputs, const BuiltinObjectScan& scan, BuiltinID member, const LocationWrites& writes, bool isLockedAtStartUp)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    const Builtin& entry = builtins.at(member);
    switch (entry.mutability) {
    case BuiltinMutability::AlwaysLocked:
        return BuiltinLocationState::Constant;
    case BuiltinMutability::ChangesOnItsOwn:
        return BuiltinLocationState::Unknown;
    case BuiltinMutability::ConstantUnlessWritten:
        break;
    }

    if (!inputs.isEnabled(FlowResultKind::ConstantBuiltinLocations, member.index()) || writes.mayHappen)
        return BuiltinLocationState::Unknown;
    if (isLockedAtStartUp)
        return BuiltinLocationState::Constant;
    if (inputs.records().mayRunUnknownJavaScript() || !hasOnlyNamedWrites(builtins, scan, entry.holder))
        return BuiltinLocationState::Unknown;
    // Start-up makes a location immutable that this answers to be constant, if it finds the holder. Code that an event lets in changes any other.
    bool isOutOfReach = !inputs.records().unseenCodeCanRun() || canLockAtStartUp(builtins, member);
    return isOutOfReach ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown;
}

static MemberLocation computeMemberLocation(const FlowFamilyInputs& inputs, const FlowPropertyState& properties, const BuiltinObjectScan& scan, BuiltinID member)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    const Builtin& entry = builtins.at(member);
    const FlowBuiltinUse& use = inputs.records().builtinUse(member);
    PropertyKeyID key = inputs.program().keyOf(entry.name);
    bool isMentioned = !use.uses.isEmpty();

    // A call of the setter and a store to an own property of an instance are uses of the member too. What writes the location itself is among the locked writes.
    if (isLockedLocation(builtins, member))
        return { { entry.holder, key, BuiltinLocationState::Constant, noWitness }, isMentioned };

    bool isTakenForConstant = properties.membersTakenForConstants.get(member.index());
    LocationWrites writes = findWritesThroughHolder(inputs, scan, member);
    if (!isTakenForConstant)
        addWritesThroughUnknownObject(inputs, scan, member, key, writes);
    return { { entry.holder, key, stateOfUnlockedLocation(inputs, scan, member, writes, isTakenForConstant), writes.first }, isMentioned || isTakenForConstant || writes.mayHappen };
}

static bool isLocationBefore(const BuiltinLocationFlow& location, const BuiltinLocationFlow& other)
{
    return std::tie(location.holder, location.key) < std::tie(other.holder, other.key);
}

static void mergeDuplicateLocations(Vector<BuiltinLocationFlow>& ascendingLocations)
{
    size_t size = 0;
    for (size_t index = 0; index < ascendingLocations.size(); ++index) {
        BuiltinLocationFlow location = ascendingLocations[index];
        if (!size || isLocationBefore(ascendingLocations[size - 1], location)) {
            ascendingLocations[size++] = location;
            continue;
        }
        BuiltinLocationFlow& merged = ascendingLocations[size - 1];
        if (location.state == BuiltinLocationState::Unknown)
            merged.state = BuiltinLocationState::Unknown;
        keepLowerWitness(merged.witness, location.witness);
    }
    ascendingLocations.shrink(size);
}

void computeBuiltinLocationResults(const FlowFamilyInputs& inputs, FlowBuiltinTables& tables)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    unsigned builtinCount = builtins.builtinCount();
    BuiltinObjectScan scan = scanBuiltinObjects(inputs);
    const FlowPropertyState& properties = FlowWalkInputs::of(inputs.solution).state;

    Vector<uint8_t> isConstant(FillWith { }, builtinCount, uint8_t { 0 });
    Vector<BuiltinLocationFlow> locations = collectInParallel<BuiltinLocationFlow>(inputs.loops(), builtinCount, [&](unsigned index, Vector<BuiltinLocationFlow>& listed) {
        BuiltinID member(index);
        if (!builtins.at(member).holder || !builtins.at(member).name)
            return;
        MemberLocation location = computeMemberLocation(inputs, properties, scan, member);
        isConstant[index] = location.flow.state == BuiltinLocationState::Constant;
        if (location.isListed)
            listed.append(location.flow);
    });

    tables.constantMembers = BitVector(builtinCount);
    for (unsigned index = 0; index < builtinCount; ++index) {
        if (isConstant[index])
            tables.constantMembers.quickSet(index);
    }
    tables.intactHolders = findIntactHolders(inputs, scan);
    tables.closedAbsentNames = listClosedAbsentNames(properties);
    tables.holdersWithFixedPrototypeLink = listHoldersWithFixedPrototypeLink(properties);

    locations.appendVector(WTF::move(scan.addedProperties));
    locations.append(inputs.records().lockedBuiltinWrites());
    parallelSort(inputs.loops(), locations.mutableSpan(), isLocationBefore);
    mergeDuplicateLocations(locations);
    tables.locations = WTF::move(locations);
}

Vector<BuiltinID> constantBuiltinMembersToLock(const Program& program, const FlowResults& flow)
{
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinID> members;
    for (const BuiltinLocationFlow& location : flow.builtinLocations()) {
        if (location.state != BuiltinLocationState::Constant)
            continue;
        BuiltinID member = findOwnMember(program, location.holder, location.key);
        if (member && canLockAtStartUp(builtins, member))
            members.append(member);
    }
    std::ranges::sort(members, { }, [&](BuiltinID member) {
        return std::pair { builtins.at(member).holder, member };
    });
    return members;
}

bool mayBeReplaced(const Program& program, const FlowResults& flow, BuiltinID builtin)
{
    const BuiltinTable& builtins = program.builtins();
    const Builtin& entry = builtins.at(builtin);
    if (!entry.holder || !entry.name || isLockedLocation(builtins, builtin))
        return false;
    // No object stands for the holder of a compiler helper, so no store reaches the helper.
    if (entry.flags.contains(BuiltinFlag::IsCompilerHelper))
        return false;
    return flow.builtinLocation(builtin) != BuiltinLocationState::Constant;
}

bool isWritten(const BuiltinLocationFlow& record)
{
    return hasFunction(record.witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

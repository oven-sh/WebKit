/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowHeapTables.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTThreads.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueSets.h"
#include "AOTVector.h"
#include <array>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

bool isMadeWithCodeOfLibrary(const BuiltinTable& builtins, BuiltinID member)
{
    member = builtins.canonicalID(member);
    const Builtin& entry = builtins.at(member);
    if (entry.origin != BuiltinOrigin::Host)
        return false;
    if (!builtins.calledFunctionsOf(member).empty() || !builtins.moduleLoadsOf(member).empty())
        return true;
    return !isObjectKind(entry.kind) && std::ranges::any_of(builtins.overloadsOf(member), [](const BuiltinOverload& overload) {
        return !!overload.code.implementation.function;
    });
}

namespace {

using Objects = Vector<ValueFlowObjectID, 32>;

// The holders of one name of the table, as a program spells the name and as the engine's own code does.
struct HoldersOfName {
    Vector<ValueFlowObjectID> holders;
    Vector<ValueFlowObjectID> holdersUnderPrivateName;
    Vector<BuiltinID> entriesOfHolders;
    // Of the holders on which a name may be closed, those that have the name among their members, their closed keys or their open names.
    Vector<BuiltinID> entriesWithClosedKeysThatListName;
    // The holders whose member under the name lets in unseen code, however the name is spelled.
    Vector<ValueFlowObjectID> holdersOfEvaluators;
    // The prototype objects of the classes whose new instances get the name.
    Vector<ValueFlowObjectID> prototypesOfInstancesWithName;
    ValueSetID instancesThatKeepProperty { ValueSets::nothing };
};

// The objects of one layout that declares a field under the key.
struct ObjectsDeclaringField {
    PropertyKeyID key;
    ValueSetID objects;
};

struct ObjectCreatedWithKey {
    PropertyKeyID key;
    ValueFlowObjectID object;
};

// What the allocations of one stretch of the universe create.
struct ObjectsCreatedWithFields {
    Vector<ObjectCreatedWithKey> underKeys;
    Vector<ValueFlowObjectID> objects;
    Vector<ObjectCreatedWithKey> storedToUnderKeys;
};

} // anonymous namespace

// Turns the number of entries of each key into where the entries of the key start in one list of all of them. Whoever fills the list moves each start on, so that it ends as the end.
static uint32_t layOutByKey(Vector<uint32_t>& numberOfEntriesOfKey)
{
    uint32_t numberOfEntries = 0;
    for (uint32_t& entries : numberOfEntriesOfKey)
        entries = std::exchange(numberOfEntries, numberOfEntries + entries);
    return numberOfEntries;
}

template<typename T>
static std::span<const T> entriesOfKey(const Vector<T>& entries, const Vector<uint32_t>& endOfKey, size_t key)
{
    uint32_t first = key ? endOfKey[key - 1] : 0;
    return entries.subspan(first, endOfKey[key] - first);
}

static ValueSetID kindsOf(ValueSetMaker& maker, uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return maker.of(members.span());
}

// The language lets nobody delete these own properties of instances or make accessors of them. The name and the length of a function can be deleted, and not every error has a message or a cause.
static bool isKeptByInstancesForLife(BuiltinID member)
{
    return member == Builtins::ArrayPrototypeLength || member == Builtins::StringPrototypeLength || member == Builtins::RegExpPrototypeLastIndex;
}

// By the entry that they inherit from, and then in the order of the table.
struct EntryBelow {
    BuiltinID parent;
    BuiltinID entry;

    auto operator<=>(const EntryBelow&) const = default;
};

static Vector<EntryBelow> listEntriesBelowEntries(const BuiltinTable& builtins)
{
    Vector<EntryBelow> list;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (isObjectKind(entry.kind) && entry.prototypeLink == BuiltinPrototypeLink::Builtin)
            list.append({ builtins.canonicalID(entry.inheritsFrom), BuiltinID(index) });
    }
    AOT::sort(list);
    return list;
}

static ValueSetID entriesRightBelow(const BuiltinTable& builtins, const ValueFlowUniverse& universe, ValueSetMaker& maker, std::span<const EntryBelow> entriesBelowEntries, BuiltinID prototype)
{
    Objects entries;
    BuiltinID parent = builtins.canonicalID(prototype);
    auto first = std::ranges::lower_bound(entriesBelowEntries, parent, { }, &EntryBelow::parent);
    for (; first != entriesBelowEntries.end() && first->parent == parent; ++first)
        universe.appendObjectsOf(first->entry, entries);
    return maker.of(entries.span());
}

void ValueFlowHeapTables::collectNamesReadByNativeCode(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker)
{
    const BuiltinTable& builtins = program.builtins();
    // What one entry only inspects another may keep.
    auto appendNamesOf = [&](const BuiltinNamesReadByNativeCode& record, ValueFlowNamesReadByNativeCode& listed) {
        for (BuiltinName name : record.names) {
            bool isOnlyInspected = std::ranges::binary_search(record.namesOnlyInspected, name);
            size_t position = listed.names.findIf([&](const ValueFlowNameReadByNativeCode& other) {
                return other.name == name;
            });
            if (position == notFound)
                listed.names.append({ name, program.keyOf(name), isOnlyInspected });
            else
                listed.names[position].isOnlyInspected &= isOnlyInspected;
        }
    };

    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const BuiltinNamesReadByNativeCode* record = builtins.namesReadByNativeCode(BuiltinID(index));
        if (!record)
            continue;
        ValueFlowObjectID object = universe.objectOf(BuiltinID(index));
        ValueFlowNamesReadByNativeCode listed { object, maker.of(object), { } };
        appendNamesOf(*record, listed);
        m_namesReadByNativeCode.append(WTF::move(listed));
    }

    for (const ValueFlowClassOfEntries& classOfEntries : m_classesOfEntries) {
        bool everyEntryHasList = true;
        ValueFlowNamesReadByNativeCode listed { classOfEntries.prototype, classOfEntries.entries, { } };
        maker.sets().forEachMember(classOfEntries.entries, [&](ValueFlowObjectID entry) {
            if (const BuiltinNamesReadByNativeCode* record = builtins.namesReadByNativeCode(builtins.canonicalID(universe.builtinOf(entry))))
                appendNamesOf(*record, listed);
            else
                everyEntryHasList = false;
        });
        if (everyEntryHasList)
            m_namesReadByNativeCode.append(WTF::move(listed));
    }
}

const ValueFlowNameReadByNativeCode* ValueFlowNamesReadByNativeCode::find(PropertyKeyID key) const
{
    for (const ValueFlowNameReadByNativeCode& read : names) {
        if (read.key == key)
            return &read;
    }
    return nullptr;
}

const ValueFlowNamesReadByNativeCode* ValueFlowHeapTables::namesReadByNativeCodeOn(ValueFlowObjectID object) const
{
    for (const ValueFlowNamesReadByNativeCode& listed : m_namesReadByNativeCode) {
        if (listed.object == object)
            return &listed;
    }
    return nullptr;
}

ValueFlowHeapTables::ValueFlowHeapTables(const Program& program, const ValueFlowUniverse& universe, ValueSets& allSets, ValueSetMaker& maker, const ParallelLoops& loops, NOESCAPE const ScopedLambda<void()>& runBesideTables)
{
    const BuiltinTable& builtins = program.builtins();
    auto without = [&](ValueSetID set, ValueSetID leftOut) {
        return maker.intersect(set, ValueSets::complement(leftOut));
    };

    ValueSetID unknownObject = universe.unknownObjectAlone();
    ValueSetID placeholders = universe.placeholdersAlone();
    ValueSetID builtinObjects = without(universe.builtinObjects(), placeholders);
    m_builtinObjectsAndPlaceholders = maker.unite(builtinObjects, placeholders);
    m_stringAndSymbolKinds = kindsOf(maker, Primitive::String | Primitive::Symbol);
    m_numberKinds = kindsOf(maker, Primitive::Number);
    m_booleanKinds = kindsOf(maker, Primitive::Boolean);
    m_startsOfLookups = ValueSets::complement(maker.unite(maker.unite(placeholders, universe.keys()), kindsOf(maker, Primitive::UndefinedOrNull | Primitive::Absent)));
    m_keysNobodyKnows = ValueSets::complement(maker.unite(universe.keys(), m_numberKinds));
    m_accessors = maker.unite(maker.unite(universe.callables(), builtinObjects), placeholders);
    m_undescribedNativeCode = universe.objectOf(Builtins::UndeclaredNativeCode);
    if (BuiltinMemberLookup found = builtins.findMember(Builtins::ObjectPrototype, BuiltinNames::__proto__); found.outcome == BuiltinMemberLookup::Outcome::Found)
        m_accessorOfPrototypeLink = universe.objectOf(found.member);
    m_knownAccessors = without(m_accessors, maker.unite(unknownObject, maker.of(m_undescribedNativeCode)));
    m_whatIsNewToNativeCode = ValueSets::complement(builtinObjects);
    m_objectsOfRealm = universe.objectsOfRealm();
    m_prototypeKey = program.keyOf(BuiltinNames::prototype);
    m_constructorKey = program.keyOf(BuiltinNames::constructor);
    m_rawKey = program.keyOf(BuiltinNames::raw);
    m_lengthKey = program.keyOf(BuiltinNames::length);
    m_calleeKey = program.keyOf(BuiltinNames::callee);
    m_nameKey = program.keyOf(BuiltinNames::name);

    uint32_t numberOfKeys = universe.numberOfObjects() - universe.keyConstant(PropertyKeyID(0u)).index();
    ValueSetID knownObjectsOfProgram = without(universe.knownObjects(), builtinObjects);
    Vector<ValueFlowObjectID> closuresWithPrototypeProperty;
    Vector<ValueFlowObjectID> templateObjects;
    Vector<ValueFlowObjectID> argumentsObjects;
    Vector<ValueFlowObjectID> arraysThatOperationsAllocate;
    Vector<ValueFlowObjectID> ordinaryObjects;
    Vector<ValueFlowObjectID> objectsHoldingOnlyWhatWasWritten;
    Vector<uint32_t> endOfObjectsCreatedWithKey;
    Vector<ValueFlowObjectID> objectsCreatedWithKeys;
    Vector<ValueFlowObjectID> objectsCreatedWithFields;
    Vector<uint32_t> endOfObjectsStoredToUnderKey;
    Vector<ValueFlowObjectID> objectsStoredToUnderKeys;

    auto describeMembersOfTable = [&] {
        m_hasGetEntry.ensureSize(builtins.builtinCount());
        m_hasSetEntry.ensureSize(builtins.builtinCount());
        m_letsInUnseenCode.ensureSize(builtins.builtinCount());
        Vector<ValueFlowObjectID, 32> objectsThatLetInUnseenCode;
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.dispatchOf(BuiltinID(index), BuiltinCallMode::Get))
                m_hasGetEntry.quickSet(index);
            if (builtins.dispatchOf(BuiltinID(index), BuiltinCallMode::Set))
                m_hasSetEntry.quickSet(index);
            if (builtins.letsInUnseenCode(builtins.canonicalID(BuiltinID(index))))
                m_letsInUnseenCode.quickSet(index);
            if (builtins.canonicalID(BuiltinID(index)) == BuiltinID(index) && builtins.letsInUnseenCode(BuiltinID(index)))
                universe.appendObjectsOf(BuiltinID(index), objectsThatLetInUnseenCode);
        }
        m_objectsThatLetInUnseenCode = maker.of(objectsThatLetInUnseenCode.span());
        Vector<ValueFlowObjectID, 32> objectsOfNativeModules;
        for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
            const RuntimeModuleData& module = builtins.at(RuntimeModuleID(index));
            // `process` and `Bun` are also members of the global object: code comes to them without a load.
            if (module.namespaceObject && module.flags.contains(RuntimeModuleFlag::IsPublic) && !module.flags.contains(RuntimeModuleFlag::HasSource) && !builtins.at(builtins.canonicalID(module.namespaceObject)).holder)
                objectsOfNativeModules.append(universe.objectOf(module.namespaceObject));
        }
        m_objectsOfNativeModules = maker.of(objectsOfNativeModules.span());

        Vector<EntryBelow> entriesBelowEntries = listEntriesBelowEntries(builtins);
        Objects prototypeObjects;
        Objects prototypeObjectsOfLanguage;
        Objects prototypeObjectsOfEntries;
        Objects prototypeObjectsOfHost;
        Objects prototypesOfTypedArrays;
        Objects prototypeObjectsThatListingDoesNotName;
        Objects prototypesAboveNativeElements;
        for (uint32_t index = 0; index < builtins.classCount(); ++index) {
            const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
            if (!builtinClass.prototype)
                continue;
            ValueFlowObjectID prototype = universe.objectOf(builtinClass.prototype);
            // No program gets hold of an AsyncFromSyncIterator or of its prototype object: the engine makes one where `for await` goes over an iterator that is not asynchronous, and steps it by itself.
            bool reachesPrograms = builtinClass.id != BuiltinClasses::AsyncFromSyncIterator;
            if (builtinClass.instancesAreEntries) {
                m_classesOfEntries.append({ prototype, entriesRightBelow(builtins, universe, maker, entriesBelowEntries.span(), builtinClass.prototype) });
                prototypeObjectsOfEntries.append(prototype);
            } else if (reachesPrograms) {
                prototypeObjects.append(prototype);
                if (builtinClass.origin == BuiltinOrigin::Host) {
                    prototypeObjectsOfHost.append(prototype);
                    if (ValueSetID entries = entriesRightBelow(builtins, universe, maker, entriesBelowEntries.span(), builtinClass.prototype); entries != ValueSets::nothing)
                        m_classesOfHostWithEntries.append({ prototype, entries });
                }
            }
            if (builtinClass.origin == BuiltinOrigin::Engine && reachesPrograms)
                prototypeObjectsOfLanguage.append(prototype);
            if (builtinClass.origin == BuiltinOrigin::Engine && !builtinClass.isPrimitiveWrapper)
                prototypeObjectsThatListingDoesNotName.append(prototype);
            if (builtinClass.typedArrayType != NotTypedArray)
                prototypesOfTypedArrays.append(prototype);
            if (builtinClass.typedArrayType != NotTypedArray || builtins.definesCharactersOnNewInstances(builtinClass.id))
                prototypesAboveNativeElements.append(prototype);
        }
        collectNamesReadByNativeCode(program, universe, maker);
        m_prototypeObjects = maker.of(prototypeObjects.span());
        m_prototypeObjectsOfLanguage = maker.of(prototypeObjectsOfLanguage.span());
        m_prototypeObjectsOfHost = maker.of(prototypeObjectsOfHost.span());
        m_prototypesOfTypedArrays = maker.of(prototypesOfTypedArrays.span());
        m_prototypeObjectsThatListingDoesNotName = maker.of(prototypeObjectsThatListingDoesNotName.span());
        m_prototypesAboveNativeElements = maker.of(prototypesAboveNativeElements.span());

        Objects hostObjectsKeepingStrings;
        std::span<const BuiltinHostObject> hostObjects = builtins.hostObjects();
        for (uint32_t index = 0; index < hostObjects.size(); ++index) {
            if (hostObjects[index].keepsStringsOfStoredValues)
                hostObjectsKeepingStrings.append(universe.hostObject(index));
        }
        m_hostObjectsKeepingStrings = maker.of(hostObjectsKeepingStrings.span());
        m_stringsNumbersAndBooleans = maker.unite(kindsOf(maker, Primitive::String | Primitive::LiteralString | Primitive::Number | Primitive::Boolean), without(universe.keys(), universe.symbols()));

        Vector<HoldersOfName> holdersOfNames;
        auto holdersOf = [&](BuiltinName name) -> HoldersOfName& {
            if (name.index() >= holdersOfNames.size())
                holdersOfNames.grow(name.index() + 1);
            return holdersOfNames[name.index()];
        };

        Objects holdersWithMembers;
        Objects holdersWithMembersUnderNumericNames;
        Objects holdersOfMembersThatHostMakes;
        Objects holdersThatListAllMembers;
        Objects lockedObjects;
        Objects holdersWithLockedIndexes;
        Objects holdersWithLockedPrototypeLink;
        Objects holdersClosedToOtherNames;
        // The holders on which a name that is none of their members may be closed.
        Vector<BuiltinID> entriesWithClosedKeys;
        Objects functionsOfEngine;
        Objects prototypesOfInstancesWithNames;
        Objects objectsOfGlobal;
        universe.appendObjectsOf(Builtins::GlobalThis, objectsOfGlobal);
        m_membersOfHolder.fill({ ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, false, ValueSets::nothing }, builtins.builtinCount());
        m_hasNumericName.ensureSize(builtins.builtinCount());
        m_compiledModuleOfHolder.fill(ModuleID(), builtins.builtinCount());
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            const Module& module = program.modules[index];
            BuiltinID object = module.kind == ModuleKind::RuntimeSource && module.runtimeModule ? builtins.namespaceOf(module.runtimeModule) : BuiltinID();
            if (object && compiledModuleWithObject(program, builtins.canonicalID(object)) == ModuleID(index))
                m_compiledModuleOfHolder[builtins.canonicalID(object).index()] = ModuleID(index);
        }
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinID holder(index);
            const Builtin& entry = builtins.at(holder);
            if (!isObjectKind(entry.kind) || builtins.canonicalID(holder) != holder)
                continue;

            Objects objects;
            universe.appendObjectsOf(holder, objects);
            // Whatever the table does not list of the object of a compiled module lies on what the body of the module makes.
            if (entry.flags.contains(BuiltinFlag::ListsAllMembers) || m_compiledModuleOfHolder[index])
                holdersThatListAllMembers.appendVector(objects);
            if (entry.flags.contains(BuiltinFlag::IsLockedObject))
                lockedObjects.appendVector(objects);
            if (builtins.closesIndexKeys(holder))
                holdersWithLockedIndexes.appendVector(objects);
            if (builtins.hasLockedPrototypeLink(holder))
                holdersWithLockedPrototypeLink.appendVector(objects);
            if (entry.flags.contains(BuiltinFlag::IsClosedToOtherNames))
                holdersClosedToOtherNames.appendVector(objects);
            if (entry.flags.contains(BuiltinFlag::IsClosedToOtherNames) || !builtins.closedKeysOf(holder).empty())
                entriesWithClosedKeys.append(holder);
            if (builtins.isFunctionOfEngine(holder))
                functionsOfEngine.appendVector(objects);
            if (entry.kind == BuiltinKind::Prototype && entry.instanceClass) {
                // What native code puts under a name whose type the class says lies on the object that stands for the instance: whoever allocates the instance writes it there.
                bool listsNameWithoutType = false;
                for (BuiltinName name : builtins.at(entry.instanceClass).namesDefinedOnNewInstances) {
                    if (builtins.typeOfNameDefinedOnNewInstances(entry.instanceClass, name))
                        continue;
                    holdersOf(name).prototypesOfInstancesWithName.appendVector(objects);
                    listsNameWithoutType = true;
                }
                if (listsNameWithoutType)
                    prototypesOfInstancesWithNames.appendVector(objects);
            }
            if (holder == Builtins::GlobalThis)
                holdersOf(entry.name).holders.appendVector(objects);
            if (!entry.memberCount)
                continue;
            holdersWithMembers.appendVector(objects);

            Objects values;
            Objects accessors;
            Objects accessorsWithGetEntry;
            Objects accessorsWithSetEntry;
            ValueSetID primitives = ValueSets::nothing;
            bool hasNativeBinding = false;
            Objects leftOut;
            for (BuiltinID member : builtins.membersOf(holder)) {
                const Builtin& memberEntry = builtins.at(member);
                ValueFlowObjectID object = universe.objectOf(member);
                bool isEvaluator = letsInUnseenCode(member);
                if (hasGetEntry(member))
                    accessorsWithGetEntry.append(object);
                if (hasSetEntry(member))
                    accessorsWithSetEntry.append(object);
                if (isEvaluator) {
                    if (isObjectKind(memberEntry.kind)) {
                        leftOut.append(object);
                        for (BuiltinID held : builtins.membersOf(builtins.canonicalID(member))) {
                            if (isObjectKind(builtins.at(held).kind))
                                leftOut.append(universe.objectOf(held));
                        }
                    }
                } else {
                    switch (memberEntry.kind) {
                    case BuiltinKind::Namespace:
                    case BuiltinKind::Constructor:
                    case BuiltinKind::Function:
                    case BuiltinKind::Prototype:
                    case BuiltinKind::Method:
                        values.append(object);
                        break;
                    case BuiltinKind::Accessor:
                    case BuiltinKind::DataProperty:
                        accessors.append(object);
                        break;
                    case BuiltinKind::Constant:
                        primitives = universe.anyPrimitive();
                        break;
                    case BuiltinKind::NativeBinding:
                        hasNativeBinding = true;
                        break;
                    }
                }

                // A module's namespace binds `default` to the object. The object has no such property.
                if (!memberEntry.name || memberEntry.flags.contains(BuiltinFlag::IsDefaultExport))
                    continue;
                if (isMadeWithCodeOfLibrary(builtins, member))
                    holdersOfMembersThatHostMakes.appendVector(objects);
                if (mayBeElementKey(classifyName(builtins.textOf(memberEntry.name)))) {
                    m_hasNumericName.quickSet(member.index());
                    holdersWithMembersUnderNumericNames.appendVector(objects);
                }
                HoldersOfName& holders = holdersOf(memberEntry.name);
                holders.holders.appendVector(objects);
                holders.entriesOfHolders.append(holder);
                if (isKeptByInstancesForLife(builtins.canonicalID(member)) && entry.instanceClass) {
                    holders.instancesThatKeepProperty = maker.unite(holders.instancesThatKeepProperty, universe.sureInstancesOf(entry.instanceClass));
                    if (PropertyKeyID key = program.keyOf(memberEntry.name))
                        m_propertiesKeptForLife.append({ key, entry.instanceClass });
                    if (entry.instanceClass == BuiltinClasses::String)
                        holders.instancesThatKeepProperty = maker.unite(holders.instancesThatKeepProperty, kindsOf(maker, Primitive::AnyString));
                }
                if (memberEntry.flags.contains(BuiltinFlag::HasPrivateAlias))
                    holders.holdersUnderPrivateName.appendVector(objects);
                if (isEvaluator)
                    holders.holdersOfEvaluators.appendVector(objects);
                // The host's own modules reach some of its functions under private names of the global object.
                if (holder == Builtins::PrivateGlobals) {
                    holders.holdersUnderPrivateName.appendVector(objectsOfGlobal);
                    if (isEvaluator)
                        holders.holdersOfEvaluators.appendVector(objectsOfGlobal);
                }
            }
            m_membersOfHolder[index] = { maker.unite(maker.of(values.span()), primitives), maker.of(accessors.span()), maker.of(accessorsWithGetEntry.span()), maker.of(accessorsWithSetEntry.span()), hasNativeBinding, maker.of(leftOut.span()) };
        }
        m_holdersWithMembers = maker.of(holdersWithMembers.span());
        m_holdersWithMembersUnderNumericNames = maker.of(holdersWithMembersUnderNumericNames.span());
        m_holdersOfMembersThatHostMakes = maker.of(holdersOfMembersThatHostMakes.span());
        ValueSetID holdersThatListNotAllMembers = maker.unite(without(builtinObjects, maker.of(holdersThatListAllMembers.span())), maker.of(m_undescribedNativeCode));
        m_holdersOfUndeclaredPropertiesUnderIndexes = holdersThatListNotAllMembers;
        m_holdersOfUndeclaredPropertiesUnderAnyKey = maker.unite(holdersThatListNotAllMembers, maker.of(prototypesOfInstancesWithNames.span()));

        m_lockedObjects = maker.of(lockedObjects.span());
        m_holdersWithLockedIndexes = maker.of(holdersWithLockedIndexes.span());
        m_holdersWithLockedPrototypeLink = maker.of(holdersWithLockedPrototypeLink.span());

        // The first three records are those of a key that the table does not have: a symbol or a private name of the program, a private name of the engine, under which no holder lists all it has, and a text.
        constexpr uint32_t recordOfOtherKey = 0;
        constexpr uint32_t recordOfEnginePrivateName = 1;
        constexpr uint32_t recordOfText = 2;
        m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, holdersThatListNotAllMembers, m_lockedObjects, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing });
        m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, builtinObjects, m_lockedObjects, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing });
        m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, holdersThatListNotAllMembers, maker.unite(m_lockedObjects, maker.of(holdersClosedToOtherNames.span())), ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing });
        for (BuiltinID holder : entriesWithClosedKeys) {
            auto listsName = [&](BuiltinName name) {
                if (name)
                    holdersOf(name).entriesWithClosedKeysThatListName.append(holder);
            };
            listsName(builtins.at(holder).name);
            for (BuiltinID member : builtins.membersOf(holder))
                listsName(builtins.at(member).name);
            for (BuiltinName name : builtins.closedKeysOf(holder))
                listsName(name);
            for (BuiltinName name : builtins.openNamesOf(holder))
                listsName(name);
        }
        enum class Answer : uint8_t { NotAsked, No, Yes };
        std::array<Vector<Answer>, MembersUnderKey::numberOfClasses> answersToClassOfKey;
        BitVector listsNameOfKey(builtins.builtinCount());

        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> recordOfName;
        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> recordOfPrivateName;

        ParallelArray<BuiltinMemberName> nameOfKey(loops, numberOfKeys, [&](unsigned index) {
            return builtinMemberNameOf(program, PropertyKeyID(index));
        });
        m_recordOfKey = Vector<uint32_t>(numberOfKeys, [&](size_t index) -> uint32_t {
            PropertyKeyID key(static_cast<uint32_t>(index));
            BuiltinMemberName name = nameOfKey[index];
            bool isPrivateName = program.at(key).kind == PropertyKeyKind::EnginePrivateName;
            if (!name) {
                if (isPrivateName)
                    return recordOfEnginePrivateName;
                return program.at(key).kind == PropertyKeyKind::Text ? recordOfText : recordOfOtherKey;
            }
            holdersOf(name.name);

            return (isPrivateName ? recordOfPrivateName : recordOfName).ensure(name.name.index(), [&] {
                const HoldersOfName& holdersOfName = holdersOfNames[name.name.index()];
                ValueSetID holders = maker.of((isPrivateName ? holdersOfName.holdersUnderPrivateName : holdersOfName.holders).span());
                ValueSetID undeclared = without(isPrivateName ? builtinObjects : holdersThatListNotAllMembers, holders);
                if (!isPrivateName)
                    undeclared = maker.unite(undeclared, maker.of(holdersOfName.prototypesOfInstancesWithName.span()));

                Objects locked;
                MembersUnderKey membersUnderKey(program, key);
                auto appendIfLocked = [&](BuiltinID holder) {
                    if (membersUnderKey.isLockedLocation(holder))
                        universe.appendObjectsOf(holder, locked);
                };
                for (BuiltinID holder : holdersOfName.entriesOfHolders)
                    appendIfLocked(holder);
                for (BuiltinID holder : holdersOfName.entriesWithClosedKeysThatListName)
                    listsNameOfKey.quickSet(holder.index());
                Vector<Answer>& answers = answersToClassOfKey[membersUnderKey.classOfKey()];
                if (answers.isEmpty())
                    answers.fill(Answer::NotAsked, entriesWithClosedKeys.size());
                for (size_t position = 0; position < entriesWithClosedKeys.size(); ++position) {
                    BuiltinID holder = entriesWithClosedKeys[position];
                    if (listsNameOfKey.quickGet(holder.index())) {
                        appendIfLocked(holder);
                        continue;
                    }
                    Answer& answer = answers[position];
                    if (answer == Answer::NotAsked)
                        answer = membersUnderKey.isLockedLocation(holder) ? Answer::Yes : Answer::No;
                    ASSERT((answer == Answer::Yes) == membersUnderKey.isLockedLocation(holder));
                    if (answer == Answer::Yes)
                        universe.appendObjectsOf(holder, locked);
                }
                for (BuiltinID holder : holdersOfName.entriesWithClosedKeysThatListName)
                    listsNameOfKey.quickClear(holder.index());
                // There are thousands of them, and four names.
                if (!isPrivateName && builtins.isClosedOnHostFunctions(name.name))
                    locked.appendVector(functionsOfEngine);
                Objects membersOfEngineAndLibrary;
                maker.sets().forEachMember(holders, [&](ValueFlowObjectID holder) {
                    BuiltinID member = membersUnderKey.findOwnMember(universe.builtinOf(holder));
                    if (member && builtins.at(member).origin != BuiltinOrigin::Host)
                        membersOfEngineAndLibrary.append(universe.objectOf(member));
                });
                m_membersUnderKey.append({ holders, maker.intersect(holders, m_prototypeObjects), undeclared, maker.unite(m_lockedObjects, maker.of(locked.span())), maker.intersect(holders, maker.of(holdersOfName.holdersOfEvaluators.span())),
                    isPrivateName ? ValueSets::nothing : holdersOfName.instancesThatKeepProperty, maker.intersect(holders, maker.of(prototypeObjectsOfEntries.span())), maker.of(membersOfEngineAndLibrary.span()) });
                return static_cast<uint32_t>(m_membersUnderKey.size() - 1);
            }).iterator->value;
        });
    };
    auto listObjectsCreatedWithKeys = [&] {
        // The allocation in the constructor of a base class also makes what stands for the instances of that class and of the classes below it.
        const ValueSets& sets = maker.sets();
        // By the class: the instances of the classes below it, found the way ValueFlowUniverse::objectAllocatedUnder() goes up from a class.
        Vector<Vector<ValueFlowObjectID>> instancesOfClassesBelow(program.layouts.size());
        sets.forEachMember(universe.instancesOfClasses(), [&](ValueFlowObjectID instance) {
            LayoutID classOfInstance = universe.layoutOf(instance);
            LayoutID current = classOfInstance;
            for (size_t steps = 0; steps < program.layouts.size() && current && program.at(current).has(LayoutFlags::HasHeritage); ++steps) {
                current = predictedHeritageOf(program.at(current));
                if (!current || current == classOfInstance)
                    continue;
                Vector<ValueFlowObjectID>& instancesBelow = instancesOfClassesBelow[current.index()];
                if (instancesBelow.isEmpty() || instancesBelow.last() != instance)
                    instancesBelow.append(instance);
            }
        });
        auto forEachInstanceAllocatedAt = [&](ValueFlowObjectID site, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) {
            if (!universe.siteOf(site) || !sets.contains(universe.objectsThatPickTheirLayout(), site))
                return;
            LayoutID allocated = universe.siteOf(site)->layout;
            LayoutID ownClass = declaredLayoutOf(program, allocated);
            if (allocated == ownClass && sets.contains(universe.instancesOfClasses(), universe.instanceOf(ownClass)))
                functor(universe.instanceOf(ownClass));
            for (ValueFlowObjectID instance : instancesOfClassesBelow[ownClass.index()])
                functor(instance);
        };
        // Where a constructor has more than one such allocation, nothing is said of what an instance is created with.
        HashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfAllocationsOfInstance;
        sets.forEachMember(universe.objectsThatPickTheirLayout(), [&](ValueFlowObjectID object) {
            forEachInstanceAllocatedAt(object, [&](ValueFlowObjectID instance) {
                ++numberOfAllocationsOfInstance.add(instance.index(), 0).iterator->value;
            });
        });

        // The objects of the sites of one function lie side by side. Each stretch fills a list of its own, and a set takes its members in any order.
        constexpr uint32_t objectsInStretch = 4096;
        Vector<ObjectsCreatedWithFields> createdInStretches((universe.numberOfObjects() + objectsInStretch - 1) / objectsInStretch);
        forEachIndexInParallel(loops, createdInStretches.size(), [&](unsigned stretch) {
            ObjectsCreatedWithFields createdInStretch;
            FunctionID functionOfLiterals;
            KeysOfLiterals keysOfLiterals;
            FunctionID functionOfStores;
            KeysOfLiterals keysOfStores;
            auto addStores = [&](FunctionID function, ValueID allocation, std::span<const ValueFlowObjectID> created) {
                if (function != functionOfStores) {
                    functionOfStores = function;
                    keysOfStores = keysStoredRightAfterAllocations(program, program.at(function));
                }
                auto stores = keysOfStores.find(allocation.index());
                if (stores == keysOfStores.end())
                    return;
                for (PropertyKeyID key : stores->value) {
                    for (ValueFlowObjectID createdObject : created)
                        createdInStretch.storedToUnderKeys.append({ key, createdObject });
                }
            };
            FunctionID dataFunction;
            ValueID dataOperation;
            Vector<std::pair<uint32_t, const DataNode::Object*>, 8> dataObjects;
            uint32_t end = std::min(universe.numberOfObjects(), (stretch + 1) * objectsInStretch);
            for (uint32_t index = stretch * objectsInStretch; index < end; ++index) {
                ValueFlowObjectID object(index);
                const ValueFlowSite* site = universe.siteOf(object);
                if (!site)
                    continue;
                if (site->origin == ValueFlowSiteOrigin::DataNode) {
                    FunctionID function = universe.functionOf(object);
                    if (function != dataFunction || site->value != dataOperation) {
                        dataFunction = function;
                        dataOperation = site->value;
                        dataObjects.shrink(0);
                        forEachDataNodeInPreorder(program.at(program.at(function).at(site->value).data()).root, [&](uint32_t ordinal, const DataNode& node) {
                            if (const auto* dataObject = node.tryAs<DataNode::Object>())
                                dataObjects.append({ ordinal, dataObject });
                        });
                    }
                    if (!site->ordinal)
                        addStores(function, site->value, singleElementSpan(object));
                    auto found = std::ranges::lower_bound(dataObjects, site->ordinal, { }, &std::pair<uint32_t, const DataNode::Object*>::first);
                    if (found == dataObjects.end() || found->first != site->ordinal || found->second->members.isEmpty())
                        continue;
                    for (const DataMember& member : found->second->members)
                        createdInStretch.underKeys.append({ member.key, object });
                    createdInStretch.objects.append(object);
                    continue;
                }
                if (site->origin != ValueFlowSiteOrigin::Operation || site->ordinal)
                    continue;
                FunctionID function = universe.functionOf(object);
                const Value& allocation = program.at(function).at(site->value);
                if (allocation.opcode != Opcode::NewObject || program.at(allocation.layout()).isEnvironment())
                    continue;

                Vector<ValueFlowObjectID, 4> created { object };
                forEachInstanceAllocatedAt(object, [&](ValueFlowObjectID instance) {
                    if (numberOfAllocationsOfInstance.get(instance.index()) == 1)
                        created.append(instance);
                });
                bool isListed = false;
                auto add = [&](PropertyKeyID key) {
                    if (!key)
                        return;
                    for (ValueFlowObjectID createdObject : created)
                        createdInStretch.underKeys.append({ key, createdObject });
                    if (!std::exchange(isListed, true))
                        createdInStretch.objects.appendVector(created);
                };
                for (uint32_t field : allocation.fields())
                    add(program.at(allocation.layout()).fields[field].key);
                for (PropertyKeyID key : keysDefinedRightAfterAllocation(program, function, site->value))
                    add(key);
                if (function != functionOfLiterals) {
                    functionOfLiterals = function;
                    keysOfLiterals = keysDefinedBehindAllocationsOfLiterals(program.at(function));
                }
                if (auto literal = keysOfLiterals.find(site->value.index()); literal != keysOfLiterals.end()) {
                    for (PropertyKeyID key : literal->value)
                        add(key);
                }
                addStores(function, site->value, created.span());
            }
            createdInStretches[stretch] = WTF::move(createdInStretch);
        });
        endOfObjectsCreatedWithKey.fill(0u, numberOfKeys);
        for (const ObjectsCreatedWithFields& createdInStretch : createdInStretches) {
            for (const ObjectCreatedWithKey& created : createdInStretch.underKeys)
                ++endOfObjectsCreatedWithKey[created.key.index()];
        }
        objectsCreatedWithKeys.grow(layOutByKey(endOfObjectsCreatedWithKey));
        for (const ObjectsCreatedWithFields& createdInStretch : createdInStretches) {
            for (const ObjectCreatedWithKey& created : createdInStretch.underKeys)
                objectsCreatedWithKeys[endOfObjectsCreatedWithKey[created.key.index()]++] = created.object;
            objectsCreatedWithFields.appendVector(createdInStretch.objects);
        }
        endOfObjectsStoredToUnderKey.fill(0u, numberOfKeys);
        for (const ObjectsCreatedWithFields& createdInStretch : createdInStretches) {
            for (const ObjectCreatedWithKey& storedTo : createdInStretch.storedToUnderKeys)
                ++endOfObjectsStoredToUnderKey[storedTo.key.index()];
        }
        objectsStoredToUnderKeys.grow(layOutByKey(endOfObjectsStoredToUnderKey));
        for (const ObjectsCreatedWithFields& createdInStretch : createdInStretches) {
            for (const ObjectCreatedWithKey& storedTo : createdInStretch.storedToUnderKeys)
                objectsStoredToUnderKeys[endOfObjectsStoredToUnderKey[storedTo.key.index()]++] = storedTo.object;
        }
    };
    auto uniteObjectsDeclaringFields = [&] {
        ValueSetMaker maker(allSets);
        auto without = [&](ValueSetID set, ValueSetID leftOut) {
            return maker.intersect(set, ValueSets::complement(leftOut));
        };
        // Thousands of layouts declare a field under one name: their objects are listed by the key first, so that each key takes one union.
        Vector<ObjectsDeclaringField> declaredFields;
        Vector<uint32_t> endOfObjectsDeclaringField;
        endOfObjectsDeclaringField.fill(0u, numberOfKeys);
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            ValueSetID objects = without(universe.objectsWith(LayoutID(index)), universe.objectsThatMayBeAnything());
            if (objects == ValueSets::nothing)
                continue;
            for (const Field& field : program.layouts[index].fields) {
                if (!field.key)
                    continue;
                declaredFields.append({ field.key, objects });
                ++endOfObjectsDeclaringField[field.key.index()];
            }
        }
        Vector<ValueSetID> objectsDeclaringFields(layOutByKey(endOfObjectsDeclaringField));
        for (const ObjectsDeclaringField& declared : declaredFields)
            objectsDeclaringFields[endOfObjectsDeclaringField[declared.key.index()]++] = declared.objects;
        m_objectsDeclaringField = Vector<ValueSetID>(numberOfKeys, [&](size_t key) {
            return maker.uniteAll(entriesOfKey(objectsDeclaringFields, endOfObjectsDeclaringField, key));
        });
    };
    runTasksInParallel(loops, {
        [&] {
            runTasksInParallel(loops, {
                [&] {
                    describeMembersOfTable();
                },
                [&] {
                    listObjectsCreatedWithKeys();
                },
            });
            m_holdersThatAnswer.reserveInitialCapacity(numberOfKeys);
            m_holdersThatMayLoseMember.reserveInitialCapacity(numberOfKeys);
            m_objectsStoredToAtCreation.reserveInitialCapacity(numberOfKeys);
            for (uint32_t key = 0; key < numberOfKeys; ++key) {
                const ValueFlowMembersUnderKey& members = under(PropertyKeyID(key));
                ValueSetID objects = maker.of(entriesOfKey(objectsCreatedWithKeys, endOfObjectsCreatedWithKey, key));
                ValueSetID objectsStoredTo = maker.of(entriesOfKey(objectsStoredToUnderKeys, endOfObjectsStoredToUnderKey, key));
                m_holdersThatAnswer.append(maker.unite(members.holders, objects));
                m_holdersThatMayLoseMember.append(maker.unite(maker.unite(without(members.holders, members.holdersWithLockedLocation), objects), objectsStoredTo));
                m_objectsStoredToAtCreation.append(objectsStoredTo);
            }
            m_holdersThatAnswerUnderSomeKey = maker.unite(m_holdersWithMembers, maker.of(objectsCreatedWithFields.span()));
        },
        [&] {
            uniteObjectsDeclaringFields();
        },
        [&] {
            auto hasHeritage = [&](LayoutID layout) {
                return layout && program.at(layout).has(LayoutFlags::HasHeritage);
            };
            auto noteLayout = [&](ValueFlowObjectID object) {
                LayoutID layout = universe.layoutOf(object);
                LayoutKind kind = layout ? program.at(layout).kind : LayoutKind::ModuleNamespace;
                if (kind == LayoutKind::Record || kind == LayoutKind::Instance || kind == LayoutKind::Container)
                    objectsHoldingOnlyWhatWasWritten.append(object);
            };
            maker.sets().forEachMember(knownObjectsOfProgram, [&](ValueFlowObjectID object) {
                switch (universe.kindOf(object)) {
                case ValueFlowObjectKind::RestArray:
                    arraysThatOperationsAllocate.append(object);
                    return;
                case ValueFlowObjectKind::FunctionPrototype:
                case ValueFlowObjectKind::ConstructedObject:
                case ValueFlowObjectKind::Prototype:
                    ordinaryObjects.append(object);
                    return;
                case ValueFlowObjectKind::Instance:
                    noteLayout(object);
                    if (!hasHeritage(universe.layoutOf(object)))
                        ordinaryObjects.append(object);
                    return;
                case ValueFlowObjectKind::Site: {
                    noteLayout(object);
                    const ValueFlowSite& site = *universe.siteOf(object);
                    if (site.origin == ValueFlowSiteOrigin::BuiltinResult || hasHeritage(site.layout))
                        return;
                    if (!site.builtinClass || site.builtinClass == BuiltinClasses::Object)
                        ordinaryObjects.append(object);
                    else if (site.builtinClass == BuiltinClasses::Array)
                        arraysThatOperationsAllocate.append(object);
                    else if (site.builtinClass == BuiltinClasses::Arguments)
                        argumentsObjects.append(object);
                    return;
                }
                case ValueFlowObjectKind::Primitive:
                case ValueFlowObjectKind::UnknownObject:
                case ValueFlowObjectKind::Builtin:
                case ValueFlowObjectKind::ModuleBuiltin:
                case ValueFlowObjectKind::Namespace:
                case ValueFlowObjectKind::Closure:
                case ValueFlowObjectKind::CoroutineResult:
                case ValueFlowObjectKind::ResultOfBuiltin:
                case ValueFlowObjectKind::Key:
                    return;
                }
            });
        },
        [&] {
            m_prototypesAtCreation = std::make_unique<std::atomic<uint64_t>[]>(universe.numberOfObjects());
            unsigned bitsOfBlocks = std::clamp<unsigned>(std::bit_width(static_cast<uint64_t>(universe.numberOfObjects())) + 2, 12, 20);
            m_shiftOfBlocks = 64 - bitsOfBlocks;
            m_prototypesOfBlocks = std::make_unique<std::atomic<uint64_t>[]>(static_cast<size_t>(5) << bitsOfBlocks);
            for (size_t entry = 0; entry < static_cast<size_t>(1) << bitsOfBlocks; ++entry)
                m_prototypesOfBlocks[entry * 5 + 1].store(noBlockKept, std::memory_order_relaxed);
        },
        [&] {
            maker.sets().forEachMember(universe.arrays(), [&](ValueFlowObjectID object) {
                const ValueFlowSite* site = universe.siteOf(object);
                if (site && site->origin == ValueFlowSiteOrigin::Operation && program.at(universe.functionOf(object)).at(site->value).opcode == Opcode::NewTemplateObject)
                    templateObjects.append(object);
            });
        },
        [&] {
            // A function of the library is created without the property. One that constructs gets it from $toClass or from a store, in front of every read, and only $toClass puts such a function below another.
            for (uint32_t index = 0; index < program.functions.size(); ++index) {
                const Function& function = program.functions[index];
                if (hasPrototypeProperty(program, function) || (function.constructedClass && isInRuntimeModule(program, function)))
                    closuresWithPrototypeProperty.append(universe.closureOf(FunctionID(index)));
            }
        },
        [&] {
            runBesideTables();
        },
    });
    m_closuresWithPrototypeProperty = maker.of(closuresWithPrototypeProperty.span());
    m_templateObjects = maker.of(templateObjects.span());
    m_argumentsObjects = maker.of(argumentsObjects.span());
    m_arraysThatOperationsAllocate = maker.of(arraysThatOperationsAllocate.span());
    m_objectsHoldingOnlyWhatWasWritten = maker.of(objectsHoldingOnlyWhatWasWritten.span());
    m_objectsWithoutPropertiesOfNativeCode = maker.unite(maker.unite(maker.of(ordinaryObjects.span()), universe.closures()), maker.unite(m_argumentsObjects, m_arraysThatOperationsAllocate));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

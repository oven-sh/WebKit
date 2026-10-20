/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowHeapTables.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTIRProgram.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueSets.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Objects = Vector<ValueFlowObjectID, 32>;

// The holders of one name of the table, as a program spells the name and as the engine's own code does.
struct HoldersOfName {
    Vector<ValueFlowObjectID> holders;
    Vector<ValueFlowObjectID> holdersUnderPrivateName;
    Vector<BuiltinID> entriesOfHolders;
    // The prototype objects of the classes whose new instances get the name.
    Vector<ValueFlowObjectID> prototypesOfInstancesWithName;
};

} // anonymous namespace

static ValueSetID kindsOf(ValueSetMaker& maker, uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return maker.of(members.span());
}

ValueFlowHeapTables::ValueFlowHeapTables(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker)
{
    const BuiltinTable& builtins = program.builtins();
    auto without = [&](ValueSetID set, ValueSetID leftOut) {
        return maker.intersect(set, ValueSets::complement(leftOut));
    };

    ValueSetID unknownObject = universe.unknownObjectAlone();
    ValueSetID builtinObjects = without(universe.builtinObjects(), unknownObject);
    m_builtinObjectsAndUnknownObject = maker.unite(builtinObjects, unknownObject);
    m_stringAndSymbolKinds = kindsOf(maker, Primitive::String | Primitive::Symbol);
    m_numberKinds = kindsOf(maker, Primitive::Number);
    m_booleanKinds = kindsOf(maker, Primitive::Boolean);
    m_startsOfLookups = ValueSets::complement(maker.unite(maker.unite(unknownObject, universe.keys()), kindsOf(maker, Primitive::UndefinedOrNull | Primitive::Absent)));
    m_keysNobodyKnows = ValueSets::complement(maker.unite(universe.keys(), m_numberKinds));
    m_accessors = maker.unite(maker.unite(universe.callables(), builtinObjects), unknownObject);
    m_undescribedNativeCode = universe.objectOf(Builtins::UndeclaredNativeCode);
    m_knownAccessors = without(m_accessors, maker.unite(unknownObject, maker.of(m_undescribedNativeCode)));
    m_whatIsNewToNativeCode = ValueSets::complement(builtinObjects);
    m_prototypeKey = program.keyOf(BuiltinNames::prototype);
    m_constructorKey = program.keyOf(BuiltinNames::constructor);
    m_rawKey = program.keyOf(BuiltinNames::raw);

    m_hasGetEntry.ensureSize(builtins.builtinCount());
    m_hasSetEntry.ensureSize(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.dispatchOf(BuiltinID(index), BuiltinCallMode::Get))
            m_hasGetEntry.quickSet(index);
        if (builtins.dispatchOf(BuiltinID(index), BuiltinCallMode::Set))
            m_hasSetEntry.quickSet(index);
    }

    Objects prototypeObjects;
    Objects prototypesOfTypedArrays;
    Objects prototypesAboveNativeElements;
    for (uint32_t index = 0; index < builtins.classCount(); ++index) {
        const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
        if (!builtinClass.prototype)
            continue;
        ValueFlowObjectID prototype = universe.objectOf(builtinClass.prototype);
        prototypeObjects.append(prototype);
        if (builtinClass.typedArrayType != NotTypedArray)
            prototypesOfTypedArrays.append(prototype);
        if (builtinClass.typedArrayType != NotTypedArray || builtins.definesCharactersOnNewInstances(builtinClass.id))
            prototypesAboveNativeElements.append(prototype);
    }
    m_prototypeObjects = maker.of(prototypeObjects.span());
    m_prototypesOfTypedArrays = maker.of(prototypesOfTypedArrays.span());
    m_prototypesAboveNativeElements = maker.of(prototypesAboveNativeElements.span());

    Vector<HoldersOfName> holdersOfNames;
    auto holdersOf = [&](BuiltinName name) -> HoldersOfName& {
        if (name.index() >= holdersOfNames.size())
            holdersOfNames.grow(name.index() + 1);
        return holdersOfNames[name.index()];
    };

    Objects holdersWithMembers;
    Objects holdersThatListAllMembers;
    Objects lockedObjects;
    Objects holdersWithLockedIndexes;
    Objects holdersWithLockedPrototypeLink;
    Objects holdersClosedToOtherNames;
    // The holders on which a name that is none of their members may be closed.
    Vector<BuiltinID> entriesWithClosedKeys;
    Objects prototypesOfInstancesWithNames;
    Objects objectsOfGlobal;
    universe.appendObjectsOf(Builtins::GlobalThis, objectsOfGlobal);
    m_membersOfHolder.fill({ ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing }, builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID holder(index);
        const Builtin& entry = builtins.at(holder);
        if (!isObjectKind(entry.kind) || builtins.canonicalID(holder) != holder)
            continue;

        Objects objects;
        universe.appendObjectsOf(holder, objects);
        if (entry.flags.contains(BuiltinFlag::ListsAllMembers))
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
        if (entry.kind == BuiltinKind::Prototype && entry.instanceClass) {
            std::span<const BuiltinName> names = builtins.at(entry.instanceClass).namesDefinedOnNewInstances;
            for (BuiltinName name : names)
                holdersOf(name).prototypesOfInstancesWithName.appendVector(objects);
            if (!names.empty())
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
        ValueSetID primitivesAndUnknown = ValueSets::nothing;
        for (BuiltinID member : builtins.membersOf(holder)) {
            const Builtin& memberEntry = builtins.at(member);
            ValueFlowObjectID object = universe.objectOf(member);
            if (hasGetEntry(member))
                accessorsWithGetEntry.append(object);
            if (hasSetEntry(member))
                accessorsWithSetEntry.append(object);
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
                primitivesAndUnknown = maker.unite(primitivesAndUnknown, universe.anyPrimitive());
                break;
            case BuiltinKind::NativeBinding:
                primitivesAndUnknown = maker.unite(primitivesAndUnknown, maker.unite(universe.anyPrimitive(), unknownObject));
                break;
            }

            // A module's namespace binds `default` to the object. The object has no such property.
            if (!memberEntry.name || memberEntry.flags.contains(BuiltinFlag::IsDefaultExport))
                continue;
            HoldersOfName& holders = holdersOf(memberEntry.name);
            holders.holders.appendVector(objects);
            holders.entriesOfHolders.append(holder);
            if (memberEntry.flags.contains(BuiltinFlag::HasPrivateAlias))
                holders.holdersUnderPrivateName.appendVector(objects);
            // The host's own modules reach some of its functions under private names of the global object.
            if (holder == Builtins::PrivateGlobals)
                holders.holdersUnderPrivateName.appendVector(objectsOfGlobal);
        }
        m_membersOfHolder[index] = { maker.unite(maker.of(values.span()), primitivesAndUnknown), maker.of(accessors.span()), maker.of(accessorsWithGetEntry.span()), maker.of(accessorsWithSetEntry.span()) };
    }
    m_holdersWithMembers = maker.of(holdersWithMembers.span());
    ValueSetID holdersThatListNotAllMembers = maker.unite(without(builtinObjects, maker.of(holdersThatListAllMembers.span())), maker.of(m_undescribedNativeCode));
    m_holdersOfUndeclaredPropertiesUnderAnyKey = maker.unite(holdersThatListNotAllMembers, maker.of(prototypesOfInstancesWithNames.span()));

    m_lockedObjects = maker.of(lockedObjects.span());
    m_holdersWithLockedIndexes = maker.of(holdersWithLockedIndexes.span());
    m_holdersWithLockedPrototypeLink = maker.of(holdersWithLockedPrototypeLink.span());

    // The first three records are those of a key that the table does not have: a symbol or a private name of the program, a private name of the engine, under which no holder lists all it has, and a text.
    constexpr uint32_t recordOfOtherKey = 0;
    constexpr uint32_t recordOfEnginePrivateName = 1;
    constexpr uint32_t recordOfText = 2;
    m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, holdersThatListNotAllMembers, m_lockedObjects });
    m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, builtinObjects, m_lockedObjects });
    m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, holdersThatListNotAllMembers, maker.unite(m_lockedObjects, maker.of(holdersClosedToOtherNames.span())) });
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> recordOfName;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> recordOfPrivateName;

    uint32_t numberOfKeys = universe.numberOfObjects() - universe.keyConstant(PropertyKeyID(0u)).index();
    m_recordOfKey = Vector<uint32_t>(numberOfKeys, [&](size_t index) -> uint32_t {
        PropertyKeyID key(static_cast<uint32_t>(index));
        BuiltinMemberName name = builtinMemberNameOf(program, key);
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
            auto appendIfLocked = [&](BuiltinID holder) {
                if (isLockedLocation(program, holder, key))
                    universe.appendObjectsOf(holder, locked);
            };
            for (BuiltinID holder : holdersOfName.entriesOfHolders)
                appendIfLocked(holder);
            for (BuiltinID holder : entriesWithClosedKeys)
                appendIfLocked(holder);
            m_membersUnderKey.append({ holders, maker.intersect(holders, m_prototypeObjects), undeclared, maker.unite(m_lockedObjects, maker.of(locked.span())) });
            return static_cast<uint32_t>(m_membersUnderKey.size() - 1);
        }).iterator->value;
    });

    m_objectsDeclaringField.fill(ValueSets::nothing, numberOfKeys);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        ValueSetID objects = without(universe.objectsWith(LayoutID(index)), universe.objectsThatMayBeAnything());
        if (objects == ValueSets::nothing)
            continue;
        for (const Field& field : program.layouts[index].fields) {
            if (field.key)
                m_objectsDeclaringField[field.key.index()] = maker.unite(m_objectsDeclaringField[field.key.index()], objects);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

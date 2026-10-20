/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTValueFlow.h"
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
class ValueFlowUniverse;
class ValueSetMaker;

// What the table of built-ins has under one key of the program.
struct ValueFlowMembersUnderKey {
    // The built-in objects that have a member of their own under the key.
    ValueSetID holders;
    // Those among them that are the prototype objects of classes.
    ValueSetID prototypeObjects;
    // The built-in objects on which something may lie under the key that the table does not list.
    ValueSetID holdersOfUndeclaredProperties;
    // The built-in objects whose location under the key is locked: it holds what the table says, whatever the program or anybody else does.
    ValueSetID holdersWithLockedLocation;
};

// What reading every member of one built-in object of the realm gives.
struct ValueFlowMembersOfHolder {
    ValueSetID values;
    // The accessors and the data properties.
    ValueSetID accessors;
    ValueSetID accessorsWithGetEntry;
    ValueSetID accessorsWithSetEntry;
};

// What the rules of the heap ask of the table of built-ins and of the layouts. It is a function of the table and of the keys of the program, made before the first round, so that no run of a rule
// goes over the table: a question about a set of objects is one intersection with a mask.
class ValueFlowHeapTables {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapTables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowHeapTables);
public:
    ValueFlowHeapTables(const Program&, const ValueFlowUniverse&, ValueSetMaker&);

    const ValueFlowMembersUnderKey& under(PropertyKeyID key) const LIFETIME_BOUND { return m_membersUnderKey[m_recordOfKey[key.index()]]; }
    // The objects whose layout declares a field under the key: a store under it adds no name to them.
    ValueSetID objectsDeclaringField(PropertyKeyID key) const { return m_objectsDeclaringField[key.index()]; }

    // Of a canonical entry.
    const ValueFlowMembersOfHolder& membersOf(BuiltinID holder) const LIFETIME_BOUND { return m_membersOfHolder[holder.index()]; }
    ValueSetID holdersWithMembers() const { return m_holdersWithMembers; }
    // Under an index, and under a key nobody knows.
    ValueSetID holdersOfUndeclaredPropertiesUnderAnyKey() const { return m_holdersOfUndeclaredPropertiesUnderAnyKey; }
    // The built-in objects that are locked as a whole; those that take no index; those whose prototype link is fixed.
    ValueSetID lockedObjects() const { return m_lockedObjects; }
    ValueSetID holdersWithLockedIndexes() const { return m_holdersWithLockedIndexes; }
    ValueSetID holdersWithLockedPrototypeLink() const { return m_holdersWithLockedPrototypeLink; }
    bool hasGetEntry(BuiltinID member) const { return m_hasGetEntry.quickGet(member.index()); }
    bool hasSetEntry(BuiltinID member) const { return m_hasSetEntry.quickGet(member.index()); }

    // The prototype objects of all classes; of the typed array classes; and of those and the classes whose instances have characters under the indexes.
    ValueSetID prototypeObjects() const { return m_prototypeObjects; }
    ValueSetID prototypesOfTypedArrays() const { return m_prototypesOfTypedArrays; }
    ValueSetID prototypesAboveNativeElements() const { return m_prototypesAboveNativeElements; }

    // Whatever a lookup starts from: no unknown object, no undefined, no null, no constant key.
    ValueSetID startsOfLookups() const { return m_startsOfLookups; }
    ValueSetID stringAndSymbolKinds() const { return m_stringAndSymbolKinds; }
    ValueSetID numberKinds() const { return m_numberKinds; }
    ValueSetID booleanKinds() const { return m_booleanKinds; }
    // Neither a constant key nor a number.
    ValueSetID keysNobodyKnows() const { return m_keysNobodyKnows; }
    ValueSetID builtinObjectsAndUnknownObject() const { return m_builtinObjectsAndUnknownObject; }
    // The table's entry for native code that nothing describes. On a chain it stands for a prototype object that the table does not describe, among accessors for one that the table does not list.
    ValueFlowObjectID undescribedNativeCode() const { return m_undescribedNativeCode; }
    // What may stand among the accessors that a lookup finds; and the same without the unknown object and without native code that nothing describes.
    ValueSetID accessors() const { return m_accessors; }
    ValueSetID knownAccessors() const { return m_knownAccessors; }
    // Everything but the built-in objects.
    ValueSetID whatIsNewToNativeCode() const { return m_whatIsNewToNativeCode; }

    PropertyKeyID prototypeKey() const { return m_prototypeKey; }
    PropertyKeyID constructorKey() const { return m_constructorKey; }
    PropertyKeyID rawKey() const { return m_rawKey; }

private:
    Vector<ValueFlowMembersUnderKey> m_membersUnderKey;
    Vector<uint32_t> m_recordOfKey;
    Vector<ValueSetID> m_objectsDeclaringField;
    Vector<ValueFlowMembersOfHolder> m_membersOfHolder;
    BitVector m_hasGetEntry;
    BitVector m_hasSetEntry;
    ValueSetID m_holdersWithMembers;
    ValueSetID m_holdersOfUndeclaredPropertiesUnderAnyKey;
    ValueSetID m_lockedObjects;
    ValueSetID m_holdersWithLockedIndexes;
    ValueSetID m_holdersWithLockedPrototypeLink;
    ValueSetID m_prototypeObjects;
    ValueSetID m_prototypesOfTypedArrays;
    ValueSetID m_prototypesAboveNativeElements;
    ValueSetID m_startsOfLookups;
    ValueSetID m_stringAndSymbolKinds;
    ValueSetID m_numberKinds;
    ValueSetID m_booleanKinds;
    ValueSetID m_keysNobodyKnows;
    ValueSetID m_builtinObjectsAndUnknownObject;
    ValueSetID m_accessors;
    ValueSetID m_knownAccessors;
    ValueSetID m_whatIsNewToNativeCode;
    ValueFlowObjectID m_undescribedNativeCode;
    PropertyKeyID m_prototypeKey;
    PropertyKeyID m_constructorKey;
    PropertyKeyID m_rawKey;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

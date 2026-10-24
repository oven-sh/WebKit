/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTValueFlow.h"
#include "AOTVector.h"
#include <atomic>
#include <memory>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class Program;
struct FieldReference;
class ValueFlowUniverse;
class ValueSetMaker;
class ValueSets;

// What the table of built-ins has under one key of the program.
struct ValueFlowMembersUnderKey {
    // The built-in objects that have a member of their own under the key.
    ValueSetID holders;
    // Those among them that are the prototype objects of classes, but for the classes whose instances are entries.
    ValueSetID prototypeObjects;
    // The built-in objects on which something may lie under the key that the table does not list.
    ValueSetID holdersOfUndeclaredProperties;
    // The built-in objects whose location under the key is locked: it holds what the table says, whatever the program or anybody else does.
    ValueSetID holdersWithLockedLocation;
    // The holders whose member under the key lets in unseen code.
    ValueSetID holdersOfEvaluators;
    // The objects and the kinds of primitives that have a property of their own under the key for life, which the table lists at the prototype object of their class.
    ValueSetID instancesThatKeepProperty;
    // The holders that are the prototype objects of classes whose instances are entries.
    ValueSetID prototypeObjectsOfEntries;
    // What the holders have under the key, but for the members of the host.
    ValueSetID membersOfEngineAndLibrary;
};

// A key under which the instances of a class have a property of their own for life.
struct ValueFlowPropertyKeptForLife {
    PropertyKeyID key;
    BuiltinClassID instanceClass;
};

// A class whose instances are entries of the table: BuiltinClass::instancesAreEntries.
struct ValueFlowClassOfEntries {
    ValueFlowObjectID prototype;
    ValueSetID entries;
};

// A built-in object of the realm on which native code that was not handed it looks up the names of a list and no other: BuiltinTable::namesReadByNativeCode().
struct ValueFlowNameReadByNativeCode {
    BuiltinName name;
    // None: the program does not spell the name.
    PropertyKeyID key;
    // BuiltinNamesReadByNativeCode::namesOnlyInspected.
    bool isOnlyInspected { false };
};

struct ValueFlowNamesReadByNativeCode {
    const ValueFlowNameReadByNativeCode* find(PropertyKeyID) const LIFETIME_BOUND;

    ValueFlowObjectID object;
    // Where the lookups of native code start.
    ValueSetID entries;
    Vector<ValueFlowNameReadByNativeCode> names;
};

// What reading every member of one built-in object of the realm gives. A member that lets in unseen code is left out: it is read by itself, by whoever may read it.
struct ValueFlowMembersOfHolder {
    ValueSetID values;
    // The accessors and the data properties.
    ValueSetID accessors;
    ValueSetID accessorsWithGetEntry;
    ValueSetID accessorsWithSetEntry;
    // Nobody knows what such a member holds.
    bool hasNativeBinding { false };
    // The members that let in unseen code, which no lookup hands out, and the objects that those hold.
    ValueSetID leftOut;
};

// The host makes the member when it is first looked for, or computes it at each read, and runs code of the library for that: whoever looks for the member needs that code, whatever for.
bool isMadeWithCodeOfLibrary(const BuiltinTable&, BuiltinID member);

// What the rules of the heap ask of the table of built-ins and of the layouts.
class ValueFlowHeapTables {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapTables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowHeapTables);
public:
    ValueFlowHeapTables(const Program&, const ValueFlowUniverse&, ValueSets&, ValueSetMaker&, const ParallelLoops&, NOESCAPE const ScopedLambda<void()>& runBesideTables);

    const ValueFlowMembersUnderKey& under(PropertyKeyID key) const LIFETIME_BOUND { return m_membersUnderKey[m_recordOfKey[key.index()]]; }
    // The objects whose layout declares a field under the key: a store under it adds no name to them.
    ValueSetID objectsDeclaringField(PropertyKeyID key) const { return m_objectsDeclaringField[key.index()]; }
    ValueSetID holdersThatAnswerUnder(PropertyKeyID key) const { return m_holdersThatAnswer[key.index()]; }
    ValueSetID holdersThatMayLoseMemberUnder(PropertyKeyID key) const { return m_holdersThatMayLoseMember[key.index()]; }
    ValueSetID holdersThatAnswerUnderSomeKey() const { return m_holdersThatAnswerUnderSomeKey; }
    // They have the property from creation unless they inherit a setter: see keysStoredRightAfterAllocations().
    ValueSetID objectsStoredToAtCreationUnder(PropertyKeyID key) const { return m_objectsStoredToAtCreation[key.index()]; }

    // Of a canonical entry.
    const ValueFlowMembersOfHolder& membersOf(BuiltinID holder) const LIFETIME_BOUND { return m_membersOfHolder[holder.index()]; }
    // The module whose compiled body makes the object that the entry stands for: compiledModuleWithObject().
    ModuleID compiledModuleOf(BuiltinID holder) const { return m_compiledModuleOfHolder[holder.index()]; }
    ValueSetID holdersWithMembers() const { return m_holdersWithMembers; }
    // isMadeWithCodeOfLibrary() holds for a member.
    ValueSetID holdersOfMembersThatHostMakes() const { return m_holdersOfMembersThatHostMakes; }
    // NaN and Infinity read as numbers, so a lookup under an index finds a member of that name.
    ValueSetID holdersWithMembersUnderNumericNames() const { return m_holdersWithMembersUnderNumericNames; }
    bool hasNumericName(BuiltinID member) const { return m_hasNumericName.quickGet(member.index()); }
    // Under a key nobody knows. A name that native code defines on the new instances of a class is no index.
    ValueSetID holdersOfUndeclaredPropertiesUnderAnyKey() const { return m_holdersOfUndeclaredPropertiesUnderAnyKey; }
    ValueSetID holdersOfUndeclaredPropertiesUnderIndexes() const { return m_holdersOfUndeclaredPropertiesUnderIndexes; }
    // The built-in objects that are locked as a whole; those that take no index; those whose prototype link is fixed.
    ValueSetID lockedObjects() const { return m_lockedObjects; }
    ValueSetID holdersWithLockedIndexes() const { return m_holdersWithLockedIndexes; }
    ValueSetID holdersWithLockedPrototypeLink() const { return m_holdersWithLockedPrototypeLink; }
    bool hasGetEntry(BuiltinID member) const { return m_hasGetEntry.quickGet(member.index()); }
    bool hasSetEntry(BuiltinID member) const { return m_hasSetEntry.quickGet(member.index()); }
    // Applying the built-in is an event: it evaluates text, loads a module by a name nobody knows or runs code that the build has not seen.
    bool letsInUnseenCode(BuiltinID member) const { return m_letsInUnseenCode.quickGet(member.index()); }
    // The objects of those built-ins.
    ValueSetID objectsThatLetInUnseenCode() const { return m_objectsThatLetInUnseenCode; }
    // What the host answers for a public module that has no code of the library and that only a load leads to: a load under a name nobody knows may answer any of them.
    ValueSetID objectsOfNativeModules() const { return m_objectsOfNativeModules; }

    // The built-in objects of which the realm has one. The run time locks what compiled code relies on in them, so nobody changes them behind the back of the analysis but code that the build has not seen.
    ValueSetID objectsOfRealm() const { return m_objectsOfRealm; }

    // The prototype objects of all classes but those whose instances are entries; of the classes of the language; of the typed array classes; and of those and the classes whose instances have characters under the indexes.
    ValueSetID prototypeObjects() const { return m_prototypeObjects; }
    std::span<const ValueFlowClassOfEntries> classesOfEntries() const LIFETIME_BOUND { return m_classesOfEntries.span(); }
    // The prototype objects of the classes of the host among prototypeObjects(), and those of them that have entries of the table right below, which are instances from the start.
    ValueSetID prototypeObjectsOfHost() const { return m_prototypeObjectsOfHost; }
    std::span<const ValueFlowClassOfEntries> classesOfHostWithEntries() const LIFETIME_BOUND { return m_classesOfHostWithEntries.span(); }
    // Null: nothing says what native code looks up on the object, so it finds whatever the program puts there.
    const ValueFlowNamesReadByNativeCode* namesReadByNativeCodeOn(ValueFlowObjectID) const LIFETIME_BOUND;
    ValueSetID prototypeObjectsOfLanguage() const { return m_prototypeObjectsOfLanguage; }
    ValueSetID prototypesOfTypedArrays() const { return m_prototypesOfTypedArrays; }
    std::span<const ValueFlowPropertyKeptForLife> propertiesKeptForLife() const LIFETIME_BOUND { return m_propertiesKeptForLife.span(); }
    // The prototype objects of the language but those of String, Number, Boolean, BigInt and Symbol: see HeapRule::objectsThatListingNamesUnder().
    ValueSetID prototypeObjectsThatListingDoesNotName() const { return m_prototypeObjectsThatListingDoesNotName; }
    ValueSetID prototypesAboveNativeElements() const { return m_prototypesAboveNativeElements; }
    // BuiltinHostObject::keepsStringsOfStoredValues, and the values that no row about a store into such an object speaks of.
    ValueSetID hostObjectsKeepingStrings() const { return m_hostObjectsKeepingStrings; }
    ValueSetID stringsNumbersAndBooleans() const { return m_stringsNumbersAndBooleans; }

    // Whatever a lookup starts from: no placeholder, no undefined, no null, no constant key.
    ValueSetID startsOfLookups() const { return m_startsOfLookups; }
    ValueSetID stringAndSymbolKinds() const { return m_stringAndSymbolKinds; }
    ValueSetID numberKinds() const { return m_numberKinds; }
    ValueSetID booleanKinds() const { return m_booleanKinds; }
    // Neither a constant key nor a number.
    ValueSetID keysNobodyKnows() const { return m_keysNobodyKnows; }
    ValueSetID builtinObjectsAndPlaceholders() const { return m_builtinObjectsAndPlaceholders; }
    // The table's entry for native code that nothing describes. On a chain it stands for a prototype object that the table does not describe, among accessors for one that the table does not list.
    ValueFlowObjectID undescribedNativeCode() const { return m_undescribedNativeCode; }
    // The accessor `__proto__` of Object.prototype. None where the table has none.
    ValueFlowObjectID accessorOfPrototypeLink() const { return m_accessorOfPrototypeLink; }
    // What HeapRule::prototypesAtCreationOf() has found for a block of a set (ValueSets::forEachBlock()): five words: a count, the name of the block, then up to six prototypes, each one more than its number, two a word.
    static constexpr uint64_t noBlockKept = UINT64_MAX;
    std::atomic<uint64_t>* prototypesOfBlock(uint64_t nameOfBlock) const { return &m_prototypesOfBlocks[((nameOfBlock * 0x9e3779b97f4a7c15ull) >> m_shiftOfBlocks) * 5]; }
    std::atomic<uint64_t>& prototypesAtCreationOf(ValueFlowObjectID object) const { return m_prototypesAtCreation[object.index()]; }
    // What may stand among the accessors that a lookup finds; and the same without the unknown object and without native code that nothing describes: what is left is called like any function.
    ValueSetID accessors() const { return m_accessors; }
    ValueSetID knownAccessors() const { return m_knownAccessors; }
    // Everything but the built-in objects.
    ValueSetID whatIsNewToNativeCode() const { return m_whatIsNewToNativeCode; }

    // The closures that are created with a prototype property. Nothing deletes that property.
    ValueSetID closuresWithPrototypeProperty() const { return m_closuresWithPrototypeProperty; }
    // What a tagged template hands its tag first. It is frozen, and it has its elements and the property `raw` from its creation.
    ValueSetID templateObjects() const { return m_templateObjects; }
    ValueSetID argumentsObjects() const { return m_argumentsObjects; }
    ValueSetID arraysThatOperationsAllocate() const { return m_arraysThatOperationsAllocate; }
    ValueSetID objectsWithoutPropertiesOfNativeCode() const { return m_objectsWithoutPropertiesOfNativeCode; }
    // Only an object that the program has allocated with a layout holds nothing but what was written into it. A built-in object has its members and a namespace object its exports.
    ValueSetID objectsHoldingOnlyWhatWasWritten() const { return m_objectsHoldingOnlyWhatWasWritten; }

    PropertyKeyID prototypeKey() const { return m_prototypeKey; }
    PropertyKeyID constructorKey() const { return m_constructorKey; }
    PropertyKeyID rawKey() const { return m_rawKey; }
    PropertyKeyID lengthKey() const { return m_lengthKey; }
    PropertyKeyID calleeKey() const { return m_calleeKey; }
    PropertyKeyID nameKey() const { return m_nameKey; }

private:
    // After the classes whose instances are entries.
    void collectNamesReadByNativeCode(const Program&, const ValueFlowUniverse&, ValueSetMaker&);

    Vector<ValueFlowMembersUnderKey> m_membersUnderKey;
    Vector<uint32_t> m_recordOfKey;
    Vector<ValueSetID> m_objectsDeclaringField;
    Vector<ValueSetID> m_holdersThatAnswer;
    Vector<ValueSetID> m_holdersThatMayLoseMember;
    ValueSetID m_holdersThatAnswerUnderSomeKey;
    Vector<ValueSetID> m_objectsStoredToAtCreation;
    Vector<ValueFlowMembersOfHolder> m_membersOfHolder;
    Vector<ModuleID> m_compiledModuleOfHolder;
    BitVector m_hasGetEntry;
    BitVector m_hasSetEntry;
    BitVector m_letsInUnseenCode;
    ValueSetID m_objectsThatLetInUnseenCode;
    ValueSetID m_objectsOfNativeModules;
    BitVector m_hasNumericName;
    ValueSetID m_holdersWithMembers;
    ValueSetID m_holdersWithMembersUnderNumericNames;
    ValueSetID m_holdersOfMembersThatHostMakes;
    ValueSetID m_holdersOfUndeclaredPropertiesUnderAnyKey;
    ValueSetID m_holdersOfUndeclaredPropertiesUnderIndexes;
    ValueSetID m_lockedObjects;
    ValueSetID m_holdersWithLockedIndexes;
    ValueSetID m_holdersWithLockedPrototypeLink;
    ValueSetID m_objectsOfRealm;
    ValueSetID m_prototypeObjects;
    Vector<ValueFlowClassOfEntries> m_classesOfEntries;
    ValueSetID m_prototypeObjectsOfHost;
    Vector<ValueFlowClassOfEntries> m_classesOfHostWithEntries;
    Vector<ValueFlowNamesReadByNativeCode> m_namesReadByNativeCode;
    ValueSetID m_prototypeObjectsOfLanguage;
    ValueSetID m_prototypesOfTypedArrays;
    Vector<ValueFlowPropertyKeptForLife, 4> m_propertiesKeptForLife;
    ValueSetID m_prototypeObjectsThatListingDoesNotName;
    ValueSetID m_prototypesAboveNativeElements;
    ValueSetID m_hostObjectsKeepingStrings;
    ValueSetID m_stringsNumbersAndBooleans;
    ValueSetID m_startsOfLookups;
    ValueSetID m_stringAndSymbolKinds;
    ValueSetID m_numberKinds;
    ValueSetID m_booleanKinds;
    ValueSetID m_keysNobodyKnows;
    ValueSetID m_builtinObjectsAndPlaceholders;
    ValueSetID m_accessors;
    ValueSetID m_knownAccessors;
    ValueSetID m_whatIsNewToNativeCode;
    ValueSetID m_closuresWithPrototypeProperty;
    ValueSetID m_templateObjects;
    ValueSetID m_argumentsObjects;
    ValueSetID m_arraysThatOperationsAllocate;
    ValueSetID m_objectsWithoutPropertiesOfNativeCode;
    ValueSetID m_objectsHoldingOnlyWhatWasWritten;
    ValueFlowObjectID m_undescribedNativeCode;
    ValueFlowObjectID m_accessorOfPrototypeLink;
    std::unique_ptr<std::atomic<uint64_t>[]> m_prototypesAtCreation;
    std::unique_ptr<std::atomic<uint64_t>[]> m_prototypesOfBlocks;
    unsigned m_shiftOfBlocks { 64 };
    PropertyKeyID m_prototypeKey;
    PropertyKeyID m_constructorKey;
    PropertyKeyID m_rawKey;
    PropertyKeyID m_lengthKey;
    PropertyKeyID m_calleeKey;
    PropertyKeyID m_nameKey;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

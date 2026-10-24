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

struct ValueFlowMembersUnderKey {
    ValueSetID holders;
    ValueSetID prototypeObjects;
    ValueSetID holdersOfUndeclaredProperties;
    ValueSetID holdersWithLockedLocation;
    ValueSetID holdersOfEvaluators;
    ValueSetID instancesThatKeepProperty;
    ValueSetID prototypeObjectsOfEntries;
    ValueSetID membersOfEngineAndLibrary;
};

struct ValueFlowPropertyKeptForLife {
    PropertyKeyID key;
    BuiltinClassID instanceClass;
};

struct ValueFlowClassOfEntries {
    ValueFlowObjectID prototype;
    ValueSetID entries;
};

struct ValueFlowNameReadByNativeCode {
    BuiltinName name;
    PropertyKeyID key;
    bool isOnlyInspected { false };
};

struct ValueFlowNamesReadByNativeCode {
    const ValueFlowNameReadByNativeCode* find(PropertyKeyID) const LIFETIME_BOUND;

    ValueFlowObjectID object;
    ValueSetID entries;
    Vector<ValueFlowNameReadByNativeCode> names;
};

struct ValueFlowMembersOfHolder {
    ValueSetID values;
    ValueSetID accessors;
    ValueSetID accessorsWithGetEntry;
    ValueSetID accessorsWithSetEntry;
    bool hasNativeBinding { false };
    ValueSetID leftOut;
};

bool isMadeWithCodeOfLibrary(const BuiltinTable&, BuiltinID member);

class ValueFlowHeapTables {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapTables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowHeapTables);
public:
    ValueFlowHeapTables(const Program&, const ValueFlowUniverse&, ValueSets&, ValueSetMaker&, const ParallelLoops&, NOESCAPE const ScopedLambda<void()>& runBesideTables);

    const ValueFlowMembersUnderKey& under(PropertyKeyID key) const LIFETIME_BOUND { return m_membersUnderKey[m_recordOfKey[key.index()]]; }
    ValueSetID objectsDeclaringField(PropertyKeyID key) const { return m_objectsDeclaringField[key.index()]; }
    ValueSetID holdersThatAnswerUnder(PropertyKeyID key) const { return m_holdersThatAnswer[key.index()]; }
    ValueSetID holdersThatMayLoseMemberUnder(PropertyKeyID key) const { return m_holdersThatMayLoseMember[key.index()]; }
    ValueSetID holdersThatAnswerUnderSomeKey() const { return m_holdersThatAnswerUnderSomeKey; }
    ValueSetID objectsStoredToAtCreationUnder(PropertyKeyID key) const { return m_objectsStoredToAtCreation[key.index()]; }

    const ValueFlowMembersOfHolder& membersOf(BuiltinID holder) const LIFETIME_BOUND { return m_membersOfHolder[holder.index()]; }
    ModuleID compiledModuleOf(BuiltinID holder) const { return m_compiledModuleOfHolder[holder.index()]; }
    ValueSetID holdersWithMembers() const { return m_holdersWithMembers; }
    ValueSetID holdersOfMembersThatHostMakes() const { return m_holdersOfMembersThatHostMakes; }
    ValueSetID holdersWithMembersUnderNumericNames() const { return m_holdersWithMembersUnderNumericNames; }
    bool hasNumericName(BuiltinID member) const { return m_hasNumericName.quickGet(member.index()); }
    ValueSetID holdersOfUndeclaredPropertiesUnderAnyKey() const { return m_holdersOfUndeclaredPropertiesUnderAnyKey; }
    ValueSetID holdersOfUndeclaredPropertiesUnderIndexes() const { return m_holdersOfUndeclaredPropertiesUnderIndexes; }
    ValueSetID lockedObjects() const { return m_lockedObjects; }
    ValueSetID holdersWithLockedIndexes() const { return m_holdersWithLockedIndexes; }
    ValueSetID holdersWithLockedPrototypeLink() const { return m_holdersWithLockedPrototypeLink; }
    bool hasGetEntry(BuiltinID member) const { return m_hasGetEntry.quickGet(member.index()); }
    bool hasSetEntry(BuiltinID member) const { return m_hasSetEntry.quickGet(member.index()); }
    bool letsInUnseenCode(BuiltinID member) const { return m_letsInUnseenCode.quickGet(member.index()); }
    ValueSetID objectsThatLetInUnseenCode() const { return m_objectsThatLetInUnseenCode; }
    ValueSetID objectsOfNativeModules() const { return m_objectsOfNativeModules; }

    ValueSetID objectsOfRealm() const { return m_objectsOfRealm; }

    ValueSetID prototypeObjects() const { return m_prototypeObjects; }
    std::span<const ValueFlowClassOfEntries> classesOfEntries() const LIFETIME_BOUND { return m_classesOfEntries.span(); }
    ValueSetID prototypeObjectsOfHost() const { return m_prototypeObjectsOfHost; }
    std::span<const ValueFlowClassOfEntries> classesOfHostWithEntries() const LIFETIME_BOUND { return m_classesOfHostWithEntries.span(); }
    const ValueFlowNamesReadByNativeCode* namesReadByNativeCodeOn(ValueFlowObjectID) const LIFETIME_BOUND;
    ValueSetID prototypeObjectsOfLanguage() const { return m_prototypeObjectsOfLanguage; }
    ValueSetID prototypesOfTypedArrays() const { return m_prototypesOfTypedArrays; }
    std::span<const ValueFlowPropertyKeptForLife> propertiesKeptForLife() const LIFETIME_BOUND { return m_propertiesKeptForLife.span(); }
    ValueSetID prototypeObjectsThatListingDoesNotName() const { return m_prototypeObjectsThatListingDoesNotName; }
    ValueSetID prototypesAboveNativeElements() const { return m_prototypesAboveNativeElements; }
    ValueSetID hostObjectsKeepingStrings() const { return m_hostObjectsKeepingStrings; }
    ValueSetID stringsNumbersAndBooleans() const { return m_stringsNumbersAndBooleans; }

    ValueSetID startsOfLookups() const { return m_startsOfLookups; }
    ValueSetID stringAndSymbolKinds() const { return m_stringAndSymbolKinds; }
    ValueSetID numberKinds() const { return m_numberKinds; }
    ValueSetID booleanKinds() const { return m_booleanKinds; }
    ValueSetID keysNobodyKnows() const { return m_keysNobodyKnows; }
    ValueSetID builtinObjectsAndPlaceholders() const { return m_builtinObjectsAndPlaceholders; }
    ValueFlowObjectID undescribedNativeCode() const { return m_undescribedNativeCode; }
    ValueFlowObjectID accessorOfPrototypeLink() const { return m_accessorOfPrototypeLink; }
    static constexpr uint64_t noBlockKept = UINT64_MAX;
    std::atomic<uint64_t>* prototypesOfBlock(uint64_t nameOfBlock) const { return &m_prototypesOfBlocks[((nameOfBlock * 0x9e3779b97f4a7c15ull) >> m_shiftOfBlocks) * 5]; }
    std::atomic<uint64_t>& prototypesAtCreationOf(ValueFlowObjectID object) const { return m_prototypesAtCreation[object.index()]; }
    ValueSetID accessors() const { return m_accessors; }
    ValueSetID knownAccessors() const { return m_knownAccessors; }
    ValueSetID whatIsNewToNativeCode() const { return m_whatIsNewToNativeCode; }

    ValueSetID closuresWithPrototypeProperty() const { return m_closuresWithPrototypeProperty; }
    ValueSetID templateObjects() const { return m_templateObjects; }
    ValueSetID argumentsObjects() const { return m_argumentsObjects; }
    ValueSetID arraysThatOperationsAllocate() const { return m_arraysThatOperationsAllocate; }
    ValueSetID objectsWithoutPropertiesOfNativeCode() const { return m_objectsWithoutPropertiesOfNativeCode; }
    ValueSetID objectsHoldingOnlyWhatWasWritten() const { return m_objectsHoldingOnlyWhatWasWritten; }

    PropertyKeyID prototypeKey() const { return m_prototypeKey; }
    PropertyKeyID constructorKey() const { return m_constructorKey; }
    PropertyKeyID rawKey() const { return m_rawKey; }
    PropertyKeyID lengthKey() const { return m_lengthKey; }
    PropertyKeyID calleeKey() const { return m_calleeKey; }
    PropertyKeyID nameKey() const { return m_nameKey; }

private:
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

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

#define FOR_EACH_AOT_VALUE_OF_HEAP_TABLES(macro) \
    macro(ValueSetID, holdersThatAnswerUnderSomeKey) \
    macro(ValueSetID, objectsThatLetInUnseenCode) \
    macro(ValueSetID, objectsOfNativeModules) \
    macro(ValueSetID, holdersWithMembers) \
    macro(ValueSetID, holdersWithMembersUnderNumericNames) \
    macro(ValueSetID, holdersOfMembersThatHostMakes) \
    macro(ValueSetID, holdersOfUndeclaredPropertiesUnderAnyKey) \
    macro(ValueSetID, holdersOfUndeclaredPropertiesUnderIndexes) \
    macro(ValueSetID, lockedObjects) \
    macro(ValueSetID, holdersWithLockedIndexes) \
    macro(ValueSetID, holdersWithLockedPrototypeLink) \
    macro(ValueSetID, objectsOfRealm) \
    macro(ValueSetID, prototypeObjects) \
    macro(ValueSetID, prototypeObjectsOfHost) \
    macro(ValueSetID, prototypeObjectsOfLanguage) \
    macro(ValueSetID, prototypesOfTypedArrays) \
    macro(ValueSetID, prototypeObjectsThatListingDoesNotName) \
    macro(ValueSetID, prototypesAboveNativeElements) \
    macro(ValueSetID, hostObjectsKeepingStrings) \
    macro(ValueSetID, stringsNumbersAndBooleans) \
    macro(ValueSetID, startsOfLookups) \
    macro(ValueSetID, stringAndSymbolKinds) \
    macro(ValueSetID, numberKinds) \
    macro(ValueSetID, booleanKinds) \
    macro(ValueSetID, keysNobodyKnows) \
    macro(ValueSetID, builtinObjectsAndPlaceholders) \
    macro(ValueSetID, accessors) \
    macro(ValueSetID, knownAccessors) \
    macro(ValueSetID, whatIsNewToNativeCode) \
    macro(ValueSetID, closuresWithPrototypeProperty) \
    macro(ValueSetID, templateObjects) \
    macro(ValueSetID, argumentsObjects) \
    macro(ValueSetID, arraysThatOperationsAllocate) \
    macro(ValueSetID, objectsWithoutPropertiesOfNativeCode) \
    macro(ValueSetID, objectsHoldingOnlyWhatWasWritten) \
    macro(ValueFlowObjectID, undescribedNativeCode) \
    macro(ValueFlowObjectID, accessorOfPrototypeLink) \
    macro(PropertyKeyID, prototypeKey) \
    macro(PropertyKeyID, constructorKey) \
    macro(PropertyKeyID, rawKey) \
    macro(PropertyKeyID, lengthKey) \
    macro(PropertyKeyID, calleeKey) \
    macro(PropertyKeyID, nameKey)

class ValueFlowHeapTables {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapTables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowHeapTables);
public:
    ValueFlowHeapTables(const Program&, const ValueFlowUniverse&, ValueSets&, ValueSetMaker&, const ParallelLoops&, NOESCAPE const ScopedLambda<void()>& runBesideTables);

#define AOT_DECLARE_GETTER(Type, name) Type name() const { return m_##name; }
    FOR_EACH_AOT_VALUE_OF_HEAP_TABLES(AOT_DECLARE_GETTER)
#undef AOT_DECLARE_GETTER

    const ValueFlowMembersUnderKey& under(PropertyKeyID key) const LIFETIME_BOUND { return m_membersUnderKey[m_recordOfKey[key.index()]]; }
    ValueSetID objectsDeclaringField(PropertyKeyID key) const { return m_objectsDeclaringField[key.index()]; }
    ValueSetID holdersThatAnswerUnder(PropertyKeyID key) const { return m_holdersThatAnswer[key.index()]; }
    ValueSetID holdersThatMayLoseMemberUnder(PropertyKeyID key) const { return m_holdersThatMayLoseMember[key.index()]; }
    ValueSetID objectsStoredToAtCreationUnder(PropertyKeyID key) const { return m_objectsStoredToAtCreation[key.index()]; }

    const ValueFlowMembersOfHolder& membersOf(BuiltinID holder) const LIFETIME_BOUND { return m_membersOfHolder[holder.index()]; }
    ModuleID compiledModuleOf(BuiltinID holder) const { return m_compiledModuleOfHolder[holder.index()]; }
    bool hasNumericName(BuiltinID member) const { return m_hasNumericName.quickGet(member.index()); }
    bool hasGetEntry(BuiltinID member) const { return m_hasGetEntry.quickGet(member.index()); }
    bool hasSetEntry(BuiltinID member) const { return m_hasSetEntry.quickGet(member.index()); }
    bool letsInUnseenCode(BuiltinID member) const { return m_letsInUnseenCode.quickGet(member.index()); }

    std::span<const ValueFlowClassOfEntries> classesOfEntries() const LIFETIME_BOUND { return m_classesOfEntries.span(); }
    std::span<const ValueFlowClassOfEntries> classesOfHostWithEntries() const LIFETIME_BOUND { return m_classesOfHostWithEntries.span(); }
    const ValueFlowNamesReadByNativeCode* namesReadByNativeCodeOn(ValueFlowObjectID) const LIFETIME_BOUND;
    std::span<const ValueFlowPropertyKeptForLife> propertiesKeptForLife() const LIFETIME_BOUND { return m_propertiesKeptForLife.span(); }

    static constexpr uint64_t noBlockKept = UINT64_MAX;
    std::atomic<uint64_t>* prototypesOfBlock(uint64_t nameOfBlock) const { return &m_prototypesOfBlocks[((nameOfBlock * 0x9e3779b97f4a7c15ull) >> m_shiftOfBlocks) * 5]; }
    std::atomic<uint64_t>& prototypesAtCreationOf(ValueFlowObjectID object) const { return m_prototypesAtCreation[object.index()]; }

private:
    void collectNamesReadByNativeCode(const Program&, const ValueFlowUniverse&, ValueSetMaker&);

    Vector<ValueFlowMembersUnderKey> m_membersUnderKey;
    Vector<uint32_t> m_recordOfKey;
    Vector<ValueSetID> m_objectsDeclaringField;
    Vector<ValueSetID> m_holdersThatAnswer;
    Vector<ValueSetID> m_holdersThatMayLoseMember;
    Vector<ValueSetID> m_objectsStoredToAtCreation;
    Vector<ValueFlowMembersOfHolder> m_membersOfHolder;
    Vector<ModuleID> m_compiledModuleOfHolder;
    BitVector m_hasGetEntry;
    BitVector m_hasSetEntry;
    BitVector m_letsInUnseenCode;
    BitVector m_hasNumericName;
    Vector<ValueFlowClassOfEntries> m_classesOfEntries;
    Vector<ValueFlowClassOfEntries> m_classesOfHostWithEntries;
    Vector<ValueFlowNamesReadByNativeCode> m_namesReadByNativeCode;
    Vector<ValueFlowPropertyKeptForLife, 4> m_propertiesKeptForLife;
    std::unique_ptr<std::atomic<uint64_t>[]> m_prototypesAtCreation;
    std::unique_ptr<std::atomic<uint64_t>[]> m_prototypesOfBlocks;
    unsigned m_shiftOfBlocks { 64 };
#define AOT_DECLARE_MEMBER(Type, name) Type m_##name;
    FOR_EACH_AOT_VALUE_OF_HEAP_TABLES(AOT_DECLARE_MEMBER)
#undef AOT_DECLARE_MEMBER
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

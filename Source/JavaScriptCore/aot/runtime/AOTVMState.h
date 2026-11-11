/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <JavaScriptCore/CollectionScope.h>
#include <JavaScriptCore/Weak.h>
#include <JavaScriptCore/WriteBarrier.h>
#include <memory>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/UniqueArray.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class CallFrame;
class IdentifierArena;
class JSGlobalObject;
class JSObject;
class JSString;
class Structure;
class VM;

namespace AOT {

class CacheRegistry;
class CallSiteCache;
class Image;
class Modules;
class PagesOfVM;
class ProgramIdentifiers;
class RegExpLiterals;
class RuntimeCounts;
class RuntimeTable;
class VMPropertyCaches;
struct CyclesOfImports;
struct FormatPatternState;
struct FunctionTables;
struct ImportersOfModules;
struct Instance;
struct ReexportedNames;

class PlacesSeenByPlans {
public:
    uint32_t takeMark(size_t numberOfModules)
    {
        if (m_numberOfModules != numberOfModules || m_nextMark > std::numeric_limits<uint32_t>::max() - 3 - numberOfModules) {
            m_numberOfModules = numberOfModules;
            m_groups.clear();
            m_groups.grow((numberOfModules + modulesInGroup - 1) / modulesInGroup);
            m_nextMark = 1;
        }
        return m_nextMark++;
    }

    void setNextMark(uint32_t mark) { m_nextMark = mark; }

    uint32_t& operator[](size_t module)
    {
        std::unique_ptr<Group>& group = m_groups[module / modulesInGroup];
        if (!group)
            group = makeUnique<Group>();
        return group->places[module % modulesInGroup];
    }

private:
    static constexpr size_t modulesInGroup = 64;
    struct Group {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Group);
        std::array<uint32_t, modulesInGroup> places { };
    };
    Vector<std::unique_ptr<Group>> m_groups;
    size_t m_numberOfModules { 0 };
    uint32_t m_nextMark { 1 };
};

class StructuresWithFastIndexing {
public:
    void add(Structure* structure) { m_structures.append(structure); }
    size_t removeDeadEntries(VM&, CollectionScope);
    std::span<Structure* const> span() const LIFETIME_BOUND { return m_structures.span(); }

private:
    Vector<Structure*> m_structures;
};

struct FormatPatternStateDeleter {
    void operator()(FormatPatternState*) const;
};

struct CountedFiles {
    using Counts = Vector<uint64_t>;

    UncheckedKeyHashMap<String, std::unique_ptr<Counts>> byDescription;
    Vector<Counts*> byModule;
};

struct VMState {
    WTF_MAKE_NONCOPYABLE(VMState);
public:
    VMState();
    ~VMState();

    void attachImage(VM&);
    void countWithoutImage(VM&);
    void willShutDownHeap();
    void releaseImage(VM&);
    template<typename Visitor> void visitAggregate(Visitor&);

    void didChangeInstances();

    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(VMState, structureIDBase); }
    static constexpr ptrdiff_t offsetOfOnlyInstance() { return OBJECT_OFFSETOF(VMState, onlyInstance); }
    static constexpr ptrdiff_t offsetOfEntriesWithSeveralInstances() { return OBJECT_OFFSETOF(VMState, entriesWithSeveralInstances); }
    static constexpr ptrdiff_t offsetOfRegExpMatchOutput() { return OBJECT_OFFSETOF(VMState, regExpMatchOutput); }
    static constexpr ptrdiff_t offsetOfParentOfSuperCall() { return OBJECT_OFFSETOF(VMState, parentOfSuperCall); }
    static constexpr ptrdiff_t offsetOfNewTargetOfSuperCall() { return OBJECT_OFFSETOF(VMState, newTargetOfSuperCall); }
    static constexpr unsigned maxGroupsInRegExpMatchOutput = 15;

    struct PolledInstances {
        Lock lock;
        Vector<Instance*, 1> instances WTF_GUARDED_BY_LOCK(lock);
    };

    const Image* image { nullptr };
    std::unique_ptr<PagesOfVM> pages;
    std::unique_ptr<CacheRegistry> caches;
    std::unique_ptr<RuntimeCounts> counts;
    std::unique_ptr<RuntimeTable> runtimeTable;
    std::unique_ptr<VMPropertyCaches> propertyCaches;
    std::unique_ptr<CallSiteCache> callSiteCache;
    std::unique_ptr<FunctionTables> functionTables;
    std::unique_ptr<CyclesOfImports> cyclesOfImports;
    std::unique_ptr<ImportersOfModules> importersOfModules;
    std::optional<Vector<uint32_t>> modulesWithQuery;
    std::unique_ptr<ReexportedNames> reexportedNames;
    PlacesSeenByPlans placesSeenByPlans;
    std::unique_ptr<RegExpLiterals> regExpLiterals;
    std::unique_ptr<FormatPatternState, FormatPatternStateDeleter> formatPatternState;
    BitVector filesWithKnownFirstLoad;
    BitVector filesFirstLoadedSynchronously;
    BitVector filesWhoseTextHostKeeps;
    BitVector filesWhoseKeptTextWasCompiled;
    BitVector filesWhoseKeptTextWasReadBelowVitest;
    BitVector filesStartedByHost;
    BitVector filesReportedAsKept;
    BitVector modulesWithHostCachedSource;
    BitVector builtinMembersWrittenInSomeRealm;
    BitVector builtinHoldersWithNamesAddedInSomeRealm;
    JSCell* parentOfSuperCall { nullptr };
    JSCell* newTargetOfSuperCall { nullptr };
    bool hasCompiledCode { false };
    bool nativeTestChangesObjectsUnseen { false };
    bool didFinishCollection { false };
    bool hasObjectsBeingFilled { false };
    bool verifiesFastPathAnswers { false };
    bool someClassHasPrivateNames { false };
    bool somePrivateNameLiesBesideFields { false };
    bool someTextKnowsPrivateNames { false };
    JSObject* classPrototypeBeingDefined { nullptr };
    JSGlobalObject* realmWhoseDatesAreWrittenInPlace { nullptr };
    Modules* modulesWithEagerLoads { nullptr };
    uintptr_t structureIDBase { 0 };
    Instance* onlyInstance { nullptr };
    uint64_t entriesWithSeveralInstances { 0 };
    uint64_t movesOfEntriesInCollections { 0 };
    struct InstanceOfRun {
        uint64_t entries { 0 };
        void* entryFrame { nullptr };
        Instance* instance { nullptr };
        const void* lowestFrame { nullptr };
        const void* adapterFrame { nullptr };
    };
    InstanceOfRun instanceOfRecentRun;
    CallFrame* classCalledWithoutNew { nullptr };
    void* exceptionCatcherStackPointer { nullptr };
    const void* returnAddressOfCallThatCollected { nullptr };
    void* stackPointerOfImport { nullptr };
    CallFrame* importFrameInLoader { nullptr };
    bool requesterRejectsUncompiledCode { false };
    bool isResolvingImportCall { false };
    bool isResolvingImportCallWithoutAttributes { false };
    bool requireRunsMocks { false };
    std::unique_ptr<ProgramIdentifiers> identifiers;
    std::unique_ptr<IdentifierArena> identifiersOfLastSource;
    size_t lengthOfSourcesSincePrune { 0 };
    void didParseSource(VM&, size_t length, std::unique_ptr<IdentifierArena>&&);
    CountedFiles countedFiles;
    struct ComputedFunctionName {
        Weak<JSString> name;
        unsigned hash { 0 };
        unsigned length { 0 };
        bool isOneOfSeveral { false };
    };
    UncheckedKeyHashMap<uint32_t, ComputedFunctionName, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> computedFunctionNames;
    struct KeysComparedLastWithLongNames {
        UncheckedKeyHashMap<const UniquedStringImpl*, const JSString*> byName;
        uint64_t versionOfHeap { 0 };
    };
    KeysComparedLastWithLongNames keysComparedLastWithLongNames;
    UniqueArray<int> regExpMatchOutput;
    size_t sizeOfRegExpMatchOutput { 0 };
    WriteBarrier<Structure> moduleRecordStructure;
    WriteBarrier<Structure> stringBuilderStorageStructure;
    WriteBarrier<Structure> poisonedCellStructure;
    Lock instancesLock;
    Vector<Instance*, 1> instances;
    Vector<Instance*> instancesToDestroy;
    StructuresWithFastIndexing structuresWithFastIndexing;
    PolledInstances polledInstances;
    uint32_t momentOfLoads { 0 };
    uint32_t momentOfTestFile { 0 };
};

static_assert(sizeof(UniqueArray<int>) == sizeof(void*));

template<typename Visitor>
void VMState::visitAggregate(Visitor& visitor)
{
    visitor.append(moduleRecordStructure);
    visitor.append(stringBuilderStorageStructure);
    visitor.append(poisonedCellStructure);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

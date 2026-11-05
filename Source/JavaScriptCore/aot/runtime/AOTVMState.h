/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <JavaScriptCore/CollectionScope.h>
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
class JSGlobalObject;
class JSObject;
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
struct FormatPatternState;
struct FunctionTables;
struct Instance;

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

    void didChangeInstances()
    {
        onlyInstance = instances.size() == 1 ? instances.first() : nullptr;
        instanceOfRecentRun = { };
    }

    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(VMState, structureIDBase); }
    static constexpr ptrdiff_t offsetOfOnlyInstance() { return OBJECT_OFFSETOF(VMState, onlyInstance); }
    static constexpr ptrdiff_t offsetOfEntriesWithSeveralInstances() { return OBJECT_OFFSETOF(VMState, entriesWithSeveralInstances); }
    static constexpr ptrdiff_t offsetOfRegExpMatchOutput() { return OBJECT_OFFSETOF(VMState, regExpMatchOutput); }
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
    std::unique_ptr<RegExpLiterals> regExpLiterals;
    std::unique_ptr<FormatPatternState, FormatPatternStateDeleter> formatPatternState;
    BitVector filesWithKnownFirstLoad;
    BitVector filesFirstLoadedSynchronously;
    BitVector filesWhoseTextHostKeeps;
    bool hasCompiledCode { false };
    bool nativeTestChangesObjectsUnseen { false };
    bool didFinishCollection { false };
    bool hasObjectsBeingFilled { false };
    JSObject* classPrototypeBeingDefined { nullptr };
    JSGlobalObject* realmWhoseDatesAreWrittenInPlace { nullptr };
    Modules* modulesWhoseFilesRunAheadOfLoader { nullptr };
    uintptr_t structureIDBase { 0 };
    Instance* onlyInstance { nullptr };
    uint64_t entriesWithSeveralInstances { 0 };
    struct InstanceOfRun {
        uint64_t entries { 0 };
        void* entryFrame { nullptr };
        Instance* instance { nullptr };
        const void* lowestFrame { nullptr };
        const void* adapterFrame { nullptr };
    };
    InstanceOfRun instanceOfRecentRun;
    CallFrame* classCalledWithoutNew { nullptr };
    void* stackPointerOfExceptionTaker { nullptr };
    const void* returnAddressOfCallThatCollected { nullptr };
    void* stackPointerOfImport { nullptr };
    CallFrame* frameOfImportThatAsksLoader { nullptr };
    bool askerLoadsCompiledFilesAlone { false };
    bool isResolvingImportCall { false };
    bool requireRunsMocks { false };
    std::unique_ptr<ProgramIdentifiers> identifiers;
    CountedFiles countedFiles;
    UniqueArray<int> regExpMatchOutput;
    WriteBarrier<Structure> moduleRecordStructure;
    WriteBarrier<Structure> stringBuilderStorageStructure;
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
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

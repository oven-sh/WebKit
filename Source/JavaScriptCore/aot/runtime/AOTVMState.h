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

// Every live Structure whose objects store elements without asking the objects above them.
class StructuresWithFastIndexing {
public:
    void add(Structure* structure) { m_structures.append(structure); }
    size_t removeDeadEntries(VM&, CollectionScope);
    std::span<Structure* const> span() const LIFETIME_BOUND { return m_structures.span(); }

private:
    Vector<Structure*> m_structures;
};

// AOTIntlBuiltinOperations.cpp, which alone knows the type.
struct FormatPatternStateDeleter {
    void operator()(FormatPatternState*) const;
};

// Execution counts for `bun test --aot --coverage`. They belong to the VM, so a file that runs again in another realm adds to the same counts.
struct CountedFiles {
    // Indexed by the counter operand of CountExecution, which the compiler numbers within each file.
    using Counts = Vector<uint64_t>;

    // Keyed by the description operand of CountExecution: the file's path, then the source range of each counter.
    UncheckedKeyHashMap<String, std::unique_ptr<Counts>> byDescription;
    // Indexed by module number. Null until the module first counts.
    Vector<Counts*> byModule;
};

struct VMState {
    WTF_MAKE_NONCOPYABLE(VMState);
public:
    VMState();
    ~VMState();

    void attachImage(VM&);
    // A run that counts also counts what a VM without a compiled program hands to the interpreter.
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
    // regExpMatchOutput has room for the literals of the image, and for any pattern made at run time with so many groups.
    static constexpr unsigned maxGroupsInRegExpMatchOutput = 15;

    struct PolledInstances {
        Lock lock;
        Vector<Instance*, 1> instances WTF_GUARDED_BY_LOCK(lock);
    };

    const Image* image { nullptr };
    // Declared before the tables so that they are destroyed after them: each table releases its pages in its destructor.
    std::unique_ptr<PagesOfVM> pages;
    std::unique_ptr<CacheRegistry> caches;
    std::unique_ptr<RuntimeCounts> counts;
    std::unique_ptr<RuntimeTable> runtimeTable;
    std::unique_ptr<VMPropertyCaches> propertyCaches;
    std::unique_ptr<CallSiteCache> callSiteCache;
    std::unique_ptr<FunctionTables> functionTables;
    std::unique_ptr<RegExpLiterals> regExpLiterals;
    // Made by the first formatter with a pattern. It holds a weak reference, so it goes in front of the heap.
    std::unique_ptr<FormatPatternState, FormatPatternStateDeleter> formatPatternState;
    BitVector filesWithKnownFirstLoad;
    BitVector filesFirstLoadedSynchronously;
    bool hasCompiledCode { false };
    bool nativeTestChangesObjectsUnseen { false };
    bool didFinishCollection { false };
    // Some object has become a dictionary for the names that it is being given.
    bool hasObjectsBeingFilled { false };
    // It is marked as a prototype when it is created, but nothing inherits from it until the class definition ends. Only compared with.
    JSObject* classPrototypeBeingDefined { nullptr };
    // Its Date.prototype has the members that JSON.stringify reads on a date, and keeps them. Only compared with, never read through: a realm may be gone.
    JSGlobalObject* realmWhoseDatesAreWrittenInPlace { nullptr };
    // They run the CommonJS files of a graph in front of the jobs in which the loader of `bun run` would.
    Modules* modulesWhoseFilesRunAheadOfLoader { nullptr };
    uintptr_t structureIDBase { 0 };
    Instance* onlyInstance { nullptr };
    uint64_t entriesWithSeveralInstances { 0 };
    // The frame pointers from lowestFrame to adapterFrame are those of one run.
    struct InstanceOfRun {
        uint64_t entries { 0 };
        void* entryFrame { nullptr };
        Instance* instance { nullptr };
        const void* lowestFrame { nullptr };
        const void* adapterFrame { nullptr };
    };
    InstanceOfRun instanceOfRecentRun;
    // The frame of the host function that throws because a class was called without `new`, while it makes the error.
    CallFrame* classCalledWithoutNew { nullptr };
    // In the innermost native code that has asked for the Exception of what it calls, and so may list the frames of the throw.
    void* stackPointerOfExceptionTaker { nullptr };
    // Of the innermost call in compiled code when the heap was last full and collected, or could have.
    const void* returnAddressOfCallThatCollected { nullptr };
    // While an import() runs a module that `bun run` runs from a job: no stack trace lists the frames from here on down.
    void* stackPointerOfImport { nullptr };
    // What is on top of the stack while an import() of compiled code asks the loader. Bytecode calls a host function for that, and its frame is the first that a stack trace skips.
    CallFrame* frameOfImportThatAsksLoader { nullptr };
    // While the host loads what a file of a test run asked for under a name nobody knows, and an executable would hold the file too.
    bool askerLoadsCompiledFilesAlone { false };
    // An import() of compiled code asks the host what its name stands for.
    bool isResolvingImportCall { false };
    // A require() runs the mocks that wait below what it loads. It cannot wait for them.
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
    // One clock for the loads of graphs, the mocks that leave a module alone and the beginnings of test files: Modules::seesMockOf().
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

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
#include <wtf/Box.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/UniqueArray.h>

namespace JSC {

class CallFrame;
class JSCell;
class JSGlobalObject;
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
class RuntimeReporter;
class RuntimeTable;
class VMPropertyCaches;
struct FunctionTables;
struct QuotesOfPlaces;
struct Instance;
struct MockedModules;
struct RuntimeCounters;

// Every live Structure whose objects store elements without asking the objects above them.
class StructuresWithFastIndexing {
public:
    void add(Structure* structure) { m_structures.append(structure); }
    size_t removeDeadEntries(VM&, CollectionScope);
    std::span<Structure* const> span() const LIFETIME_BOUND { return m_structures.span(); }

private:
    Vector<Structure*> m_structures;
};

// Reads like the std::unique_ptr that owns the object: VMState::PartsOfImage.
template<typename T>
class PartOfRunningImage {
public:
    T* get() const { return m_part; }
    T* operator->() const { return m_part; }
    T& operator*() const { return *m_part; }
    explicit operator bool() const { return !!m_part; }
    void operator=(T* part) { m_part = part; }

private:
    T* m_part { nullptr };
};

struct VMState {
    WTF_MAKE_NONCOPYABLE(VMState);
public:
    VMState();
    ~VMState();

    // What a VM has once for each image that it runs. A VM runs several where images share their realms (Image::above()), and then the image that runs is the one
    // whose code was entered last: an operation finds the tables of its caller in the members of VMState, whichever image the caller is in.
    struct PartsOfImage {
        PartsOfImage();
        PartsOfImage(PartsOfImage&&);
        ~PartsOfImage();

        const Image* image { nullptr };
        std::unique_ptr<RuntimeTable> runtimeTable;
        std::unique_ptr<VMPropertyCaches> propertyCaches;
        std::unique_ptr<CallSiteCache> callSiteCache;
        std::unique_ptr<FunctionTables> functionTables;
        std::unique_ptr<RegExpLiterals> regExpLiterals;
        // The sets of mocked modules that the last realm went through, in that order.
        Vector<Box<MockedModules>> mockedModules;
    };

    void attachImage(VM&);
    JS_EXPORT_PRIVATE void run(const Image&);
    // The engine jumps to an adapter from many places. Where images share realms, the adapter itself sees to it that its image runs, and that the one before runs again when its frame is left.
    void runImageOfAdapter(const Image&, const void* adapterFrame);
    void restoreImageBelowAdapter();
    PartsOfImage& partsOfRunningImage();
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
    static constexpr ptrdiff_t offsetOfImage() { return OBJECT_OFFSETOF(VMState, image); }
    static constexpr ptrdiff_t offsetOfAdapterFrameThatRestoresImage() { return OBJECT_OFFSETOF(VMState, adapterFrameThatRestoresImage); }
    static constexpr ptrdiff_t offsetOfEntriesWithSeveralInstances() { return OBJECT_OFFSETOF(VMState, entriesWithSeveralInstances); }
    static constexpr ptrdiff_t offsetOfRegExpMatchOutput() { return OBJECT_OFFSETOF(VMState, regExpMatchOutput); }
    static constexpr ptrdiff_t offsetOfParentOfSuperCall() { return OBJECT_OFFSETOF(VMState, parentOfSuperCall); }
    static constexpr ptrdiff_t offsetOfNewTargetOfSuperCall() { return OBJECT_OFFSETOF(VMState, newTargetOfSuperCall); }
    // regExpMatchOutput has room for the literals of the image, and for any pattern made at run time with so many groups.
    static constexpr unsigned maxGroupsInRegExpMatchOutput = 15;

    struct PolledInstances {
        Lock lock;
        Vector<Instance*, 1> instances WTF_GUARDED_BY_LOCK(lock);
    };

    const Image* image { nullptr };
    bool hasCompiledCode { false };
    bool hasSeveralImages { false };
    struct ImageBelowAdapter {
        const void* adapterFrame;
        const Image* image;
    };
    // The innermost adapter frame that changed which image runs.
    const void* adapterFrameThatRestoresImage { nullptr };
    Vector<ImageBelowAdapter, 8> imagesBelowAdapters;
    bool validatesRuntime { false };
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
    struct InstanceOfRun {
        uint64_t entries { 0 };
        void* entryFrame { nullptr };
        Instance* instance { nullptr };
    };
    InstanceOfRun instanceOfRecentRun;
    // The frame of the host function that throws because a class was called without `new`, while it makes the error.
    CallFrame* classCalledWithoutNew { nullptr };
    // Set from when compiled code's `super(..)` enters a native constructor until that constructor requests the Structure for the new object, or returns. Only compared, never dereferenced.
    JSCell* parentOfSuperCall { nullptr };
    JSCell* newTargetOfSuperCall { nullptr };
    // In the innermost native code that has asked for the Exception of what it calls, and so may list the frames of the throw.
    void* stackPointerOfExceptionTaker { nullptr };
    // Of the innermost call in compiled code when the heap was last full and collected.
    const void* returnAddressOfCallThatCollected { nullptr };
    // While an import() runs a module that `bun run` runs from a job: no stack trace lists the frames from here on down.
    void* stackPointerOfImport { nullptr };
    // What is on top of the stack while an import() of compiled code asks the loader. Bytecode calls a host function for that, and its frame is the first that a stack trace skips.
    CallFrame* frameOfImportThatAsksLoader { nullptr };
    std::unique_ptr<PagesOfVM> pages;
    PartOfRunningImage<RuntimeTable> runtimeTable;
    std::unique_ptr<ProgramIdentifiers> identifiers;
    std::unique_ptr<CacheRegistry> caches;
    PartOfRunningImage<VMPropertyCaches> propertyCaches;
    std::unique_ptr<RuntimeCounters> counters;
    std::unique_ptr<RuntimeReporter> reporter;
    PartOfRunningImage<CallSiteCache> callSiteCache;
    PartOfRunningImage<FunctionTables> functionTables;
    PartOfRunningImage<RegExpLiterals> regExpLiterals;
    // By Image::position(). Its size never changes while the VM runs code, so the collector's threads may read it.
    Vector<PartsOfImage, 1> parts;
    UniqueArray<int> regExpMatchOutput;
    WriteBarrier<Structure> moduleRecordStructure;
    WriteBarrier<Structure> stringBuilderStorageStructure;
    Lock instancesLock;
    Vector<Instance*, 1> instances;
    Vector<Instance*> instancesToDestroy;
    StructuresWithFastIndexing structuresWithFastIndexing;
    PolledInstances polledInstances;
    // What error messages have quoted so far: AOTErrorMessages.cpp, which alone knows the type.
    std::unique_ptr<QuotesOfPlaces, void (*)(QuotesOfPlaces*)> quotesOfPlaces { nullptr, nullptr };
};

static_assert(sizeof(PartOfRunningImage<RuntimeTable>) == sizeof(void*));

bool belongsToOtherImageSlow(VM&, const JSCell*);
// For whoever has no VM at hand: in the VM of this thread, if it has one that runs compiled code.
JS_EXPORT_PRIVATE void runImageOnThisThread(const Image&);

// While it lives, the image is the one that runs.
class RunningImageScope {
    WTF_MAKE_NONCOPYABLE(RunningImageScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE RunningImageScope(VMState& state, const Image* image)
        : m_state(state)
    {
        if (state.image != image) [[unlikely]] {
            m_previous = state.image;
            state.run(*image);
        }
    }
    ALWAYS_INLINE ~RunningImageScope()
    {
        if (m_previous) [[unlikely]]
            m_state.run(*m_previous);
    }

private:
    VMState& m_state;
    const Image* m_previous { nullptr };
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

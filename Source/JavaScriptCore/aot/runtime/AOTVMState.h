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
struct Instance;
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

struct VMState {
    WTF_MAKE_NONCOPYABLE(VMState);
public:
    VMState();
    ~VMState();

    void attachImage(VM&);
    void willShutDownHeap();
    void releaseImage(VM&);
    template<typename Visitor> void visitAggregate(Visitor&);

    void didChangeInstances() { onlyInstance = instances.size() == 1 ? instances.first() : nullptr; }

    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(VMState, structureIDBase); }
    static constexpr ptrdiff_t offsetOfOnlyInstance() { return OBJECT_OFFSETOF(VMState, onlyInstance); }
    static constexpr ptrdiff_t offsetOfRegExpMatchOutput() { return OBJECT_OFFSETOF(VMState, regExpMatchOutput); }
    static constexpr ptrdiff_t offsetOfParentOfSuperCall() { return OBJECT_OFFSETOF(VMState, parentOfSuperCall); }
    static constexpr ptrdiff_t offsetOfNewTargetOfSuperCall() { return OBJECT_OFFSETOF(VMState, newTargetOfSuperCall); }
    // regExpMatchOutput has room for the literals of the image, and for any pattern made at run time with so many groups.
    static constexpr unsigned maxGroupsInRegExpMatchOutput = 15;

    struct LoopPollPages {
        Lock lock;
        Vector<void*, 1> pages WTF_GUARDED_BY_LOCK(lock);
    };

    const Image* image { nullptr };
    bool hasCompiledCode { false };
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
    std::unique_ptr<RuntimeTable> runtimeTable;
    std::unique_ptr<ProgramIdentifiers> identifiers;
    std::unique_ptr<CacheRegistry> caches;
    std::unique_ptr<VMPropertyCaches> propertyCaches;
    std::unique_ptr<RuntimeCounters> counters;
    std::unique_ptr<RuntimeReporter> reporter;
    std::unique_ptr<CallSiteCache> callSiteCache;
    std::unique_ptr<FunctionTables> functionTables;
    std::unique_ptr<RegExpLiterals> regExpLiterals;
    UniqueArray<int> regExpMatchOutput;
    WriteBarrier<Structure> moduleRecordStructure;
    WriteBarrier<Structure> stringBuilderStorageStructure;
    Lock instancesLock;
    Vector<Instance*, 1> instances;
    Vector<Instance*> instancesToDestroy;
    StructuresWithFastIndexing structuresWithFastIndexing;
    LoopPollPages loopPollPages;
};

static_assert(sizeof(std::unique_ptr<RuntimeTable>) == sizeof(void*));
static_assert(sizeof(UniqueArray<int>) == sizeof(void*));

template<typename Visitor>
void VMState::visitAggregate(Visitor& visitor)
{
    visitor.append(moduleRecordStructure);
    visitor.append(stringBuilderStorageStructure);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

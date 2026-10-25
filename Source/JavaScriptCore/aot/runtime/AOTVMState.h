/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <JavaScriptCore/WriteBarrier.h>
#include <memory>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/UniqueArray.h>

namespace JSC {

class Structure;
class VM;

namespace AOT {

class CacheRegistry;
class CallSiteCache;
class Image;
class ProgramIdentifiers;
class RegExpLiterals;
class RuntimeReporter;
class RuntimeTable;
class VMPropertyCaches;
struct FunctionTables;
struct Instance;
struct RuntimeCounters;

struct VMState {
    WTF_MAKE_NONCOPYABLE(VMState);
public:
    VMState();
    ~VMState();

    void attachImage(VM&);
    void willShutDownHeap();
    void releaseImage(VM&);
    template<typename Visitor> void visitAggregate(Visitor&);

    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(VMState, structureIDBase); }
    static constexpr ptrdiff_t offsetOfRegExpMatchOutput() { return OBJECT_OFFSETOF(VMState, regExpMatchOutput); }
    // regExpMatchOutput has room for the literals of the image, and for any pattern made at run time with so many groups.
    static constexpr unsigned maxGroupsInRegExpMatchOutput = 15;

    struct LoopPollPages {
        Lock lock;
        Vector<void*, 1> pages WTF_GUARDED_BY_LOCK(lock);
    };

    const Image* image { nullptr };
    bool hasCompiledCode { false };
    bool validatesRuntime { false };
    uintptr_t structureIDBase { 0 };
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

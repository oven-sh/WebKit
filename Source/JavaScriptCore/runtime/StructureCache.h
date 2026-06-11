/*
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include "Options.h"
#include "PrototypeKey.h"
#include "WeakGCMap.h"
#include <wtf/Lock.h>

namespace JSC {

class FunctionExecutable;
class JSGlobalObject;
class JSObject;
class Structure;
class TypeInfo;
class VM;

typedef uint8_t IndexingType;

// Tracks the canonical structure an object should be allocated with when inheriting from a given prototype.
//
// Threading (TSAN-TRIAGE §6.34/§8.34, family structure-cache): under
// Options::useJSThreads() with the GIL off, N mutator threads reach
// createEmptyStructure() concurrently (per SPEC-ungil §K, per-global caches
// are NOT blessed racy), so m_structures opts into the locking WeakGCMap
// configuration (WeakGCMapLocking::Yes, the same SPEC-ungil §LK.7 class-2
// cache leaf lock that closed family 32). Every map operation —
// get/addIfAbsent/set/clear and GC-side pruneStaleEntries — then serializes
// on the map's internal leaf lock, so no thread can walk a table that a
// concurrent set/rehash is freeing. Flag-off, the map is non-locking and the
// pre-existing m_lock discipline (mutation + concurrent-compiler-thread reads
// under m_lock; lock-free main-thread fast-path lookup) is unchanged.
class StructureCache {
public:
    explicit StructureCache(VM& vm)
        : m_structures(vm, Options::useJSThreads() ? WeakGCMapLocking::Yes : WeakGCMapLocking::No)
    {
    }

    JS_EXPORT_PRIVATE void clear();

    JS_EXPORT_PRIVATE Structure* emptyObjectStructureForPrototype(JSGlobalObject*, JSObject*, unsigned inlineCapacity, bool makePolyProtoStructure = false, FunctionExecutable* = nullptr);
    JS_EXPORT_PRIVATE Structure* emptyStructureForPrototypeFromBaseStructure(JSGlobalObject*, JSObject*, Structure*);
    JS_EXPORT_PRIVATE Structure* emptyObjectStructureConcurrently(JSObject* prototype, unsigned inlineCapacity);

    // WeakGCMap::forEach intentionally takes no internal lock (Func may
    // allocate, which is forbidden under a §LK leaf lock), so GIL-off this is
    // only safe from exclusive contexts. The sole caller is the haveABadTime
    // path (JSGlobalObject.cpp BadTimeFinder), which runs with the heap
    // deferred and, GIL-off, inside the §K.5 Class-4 stop (AB-10 LANDED:
    // haveABadTime routes its whole body through
    // JSThreadsSafepoint::stopTheWorldAndRun when gilOff) — no sibling
    // mutator can be inside get()/addIfAbsent() concurrently.
    template<typename Func>
    void forEach(Func func)
    {
        Locker locker { m_lock };
        m_structures.forEach(func);
    }

private:
    Structure* createEmptyStructure(JSGlobalObject*, JSObject* prototype, const TypeInfo&, const ClassInfo*, IndexingType, unsigned inlineCapacity, bool makePolyProtoStructure, FunctionExecutable*);

    WeakGCMap<PrototypeKey, Structure> m_structures;
    Lock m_lock;
};

} // namespace JSC

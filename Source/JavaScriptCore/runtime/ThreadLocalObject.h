/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSDestructibleObject.h"
#include "ThreadManager.h"

namespace JSC {

// new ThreadLocal() (SPEC-api 4.4/5.8): a per-thread value slot. The cell
// carries a process-unique, monotonically allocated uint64_t key (issued by
// ThreadManager); the storage lives in the *current* ThreadState's
// HashMap<uint64_t, Strong<Unknown>> threadLocals, accessed only via
// currentThreadState — so reads/writes are lock-free and inherently
// owner-thread-only, and writes are invisible cross-thread (I13). The value
// reads as undefined on every thread until that thread's own first write.
//
// Lifetime (SPEC-api 5.8/5.10, documented leak — I13): a stored value is
// rooted by its owning ThreadState until it is overwritten or the thread
// exits (spawned threads clear threadLocals in the completion sequence; lazy
// main/embedder ThreadStates clear them via the 5.10 finalizer hook at VM
// teardown). When a ThreadLocal cell dies, its slots in OTHER live threads'
// maps are NOT eagerly swept: they leak until those threads exit. This is
// accepted and documented, not an invariant violation. Keys are never
// reused, so a leaked slot can never alias a later-created ThreadLocal.
class JSThreadLocalObject final : public JSDestructibleObject {
public:
    using Base = JSDestructibleObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.destructibleObjectSpace();
    }

    static JSThreadLocalObject* create(VM&, Structure*);
    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    DECLARE_EXPORT_INFO;

    // Process-unique monotonic key into ThreadState::threadLocals
    // (SPEC-api 5.8). Allocated by ThreadManager at construction; never
    // reused, even after this cell dies (see class comment).
    uint64_t key() const { return m_key; }

private:
    JSThreadLocalObject(VM&, Structure*);

    uint64_t m_key;
};

} // namespace JSC

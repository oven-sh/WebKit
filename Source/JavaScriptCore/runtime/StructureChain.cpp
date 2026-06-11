/*
 * Copyright (C) 2008-2021 Apple Inc. All rights reserved.
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

#include "config.h"
#include "StructureChain.h"

#include "JSCInlines.h"
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
    
const ClassInfo StructureChain::s_info = { "StructureChain"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(StructureChain) };

StructureChain::StructureChain(VM& vm, Structure* structure, StructureID* vector)
    : Base(vm, structure)
    , m_vector(vector, WriteBarrierEarlyInit)
{
}

StructureChain* StructureChain::create(VM& vm, JSObject* head)
{
    // FIXME: Make StructureChain::create fail for large chain. Caching large chain is not so profitable.
    // By making the size <= UINT16_MAX, we can store length in a high bits of auxiliary pointer.
    // https://bugs.webkit.org/show_bug.cgi?id=200290
    size_t size = 0;
    for (JSObject* current = head; current; current = current->structure()->storedPrototypeObject(current))
        ++size;
    ++size; // Sentinel nullptr.
    size_t bytes = Checked<size_t>(size) * sizeof(StructureID);
    void* vector = vm.auxiliarySpace().allocate(vm, bytes, nullptr, AllocationFailureMode::Assert);
    static_assert(!StructureID().bits(), "Make sure the value we're going to memcpy below matches the default StructureID");
    // TSAN family structure-fields (r4 residual "StructureChain::create x
    // StructureID/canCachePropertyNameEnumerator"): this auxiliary block can
    // be recycled storage of a previously-published chain whose lanes foreign
    // mutators (canCachePropertyNameEnumerator walks) and concurrent marking
    // (visitChildrenImpl below) still read with relaxed atomics. Flag-on,
    // zero the lanes with relaxed atomic 32-bit stores so the writer side of
    // those pairs is C++-defined too; a reader observing the zero just hits
    // its sentinel/decline exit. Flag-off keeps today's memset (identical
    // behavior, no codegen change, per the flag-off rule).
    if (Options::useJSThreads()) [[unlikely]] {
        for (size_t i = 0; i < size; ++i)
            WTF::atomicStore(static_cast<uint32_t*>(vector) + i, 0u, std::memory_order_relaxed);
    } else
        memset(vector, 0, bytes);
    StructureChain* chain = new (NotNull, allocateCell<StructureChain>(vm)) StructureChain(vm, vm.structureChainStructure.get(), static_cast<StructureID*>(vector));
    chain->finishCreation(vm, head);
    return chain;
}

void StructureChain::finishCreation(VM& vm, JSObject* head)
{
    Base::finishCreation(vm);
    size_t i = 0;
    static_assert(sizeof(StructureID) == sizeof(uint32_t));
    for (JSObject* current = head; current; current = current->structure()->storedPrototypeObject(current)) {
        Structure* structure = current->structure();
        // TSAN family structure-fields: once the caller publishes this chain
        // (Structure::m_cachedPrototypeChain, written lock-free by
        // prototypeChain in StructureInlines.h), the StructureID lanes are
        // read by foreign mutators and concurrent marking; relaxed atomic
        // 32-bit stores (identical str/mov codegen) keep the word accesses
        // C++-defined against those readers on recycled auxiliary memory.
        WTF::atomicStore(reinterpret_cast<uint32_t*>(m_vector.get() + i), structure->id().bits(), std::memory_order_relaxed);
        ++i;
        vm.writeBarrier(this);
    }
    // UG §K publication: order the vector lanes (and the memset sentinel tail
    // written in create()) before the caller's lock-free publish store of the
    // chain pointer. Gated so flag-off codegen is unchanged (no arm64 dmb).
    if (Options::useJSThreads()) [[unlikely]]
        WTF::storeStoreFence();
}

template<typename Visitor>
void StructureChain::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    StructureChain* thisObject = uncheckedDowncast<StructureChain>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.markAuxiliary(thisObject->m_vector.get());
    // Relaxed atomic lane loads: concurrent marking walks the vector while a
    // mutator may be (re)writing lanes of a chain occupying recycled
    // auxiliary memory (finishCreation above). Same ldr/mov codegen.
    for (auto* current = thisObject->m_vector.get();; ++current) {
        StructureID structureID = std::bit_cast<StructureID>(WTF::atomicLoad(reinterpret_cast<uint32_t*>(current), std::memory_order_relaxed));
        if (!structureID)
            break;
        Structure* structure = structureID.decode();
        visitor.appendUnbarriered(structure);
    }
}

DEFINE_VISIT_CHILDREN(StructureChain);

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

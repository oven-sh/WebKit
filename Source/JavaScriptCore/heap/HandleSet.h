/*
 * Copyright (C) 2011-2021 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "Handle.h"
#include "HandleBlock.h"
#include "HeapCell.h"
#include <wtf/DoublyLinkedList.h>
#include <wtf/HashCountedSet.h>
#include <wtf/Lock.h>
#include <wtf/SentinelLinkedList.h>
#include <wtf/SinglyLinkedList.h>

namespace JSC {

class HandleSet;
class VM;
class JSValue;

class HandleNode final : public BasicRawSentinelNode<HandleNode> {
public:
    HandleNode() = default;
    
    HandleSlot slot();
    HandleSet* handleSet();

    static HandleNode* toHandleNode(HandleSlot slot)
    {
        return std::bit_cast<HandleNode*>(std::bit_cast<uintptr_t>(slot) - OBJECT_OFFSETOF(HandleNode, m_value));
    }

private:
    JSValue m_value { };
};

class HandleSet {
    friend class HandleBlock;
public:
    static HandleSet* heapFor(HandleSlot);

    HandleSet(VM&);
    ~HandleSet();

    VM& vm();

    // UNGIL §F.3 flag-off codegen seam (review fix): cached copy of the
    // owning VM's immutable m_gilOff byte, so the ALWAYS_INLINE strongHandle*
    // wrappers below can test the mode without pulling VM's definition into
    // this header (and without re-deriving from Options). NOTE the stamping
    // order (AB17c F4 root-cause fix): this HandleSet is a Heap member,
    // constructed in VM's ctor INIT LIST — i.e. BEFORE the ctor body's U0c
    // designation block computes VM::m_gilOff. The ctor therefore always
    // stamps false, and the U0c winner re-stamps via
    // noteOwnerVMDesignatedGILOff() immediately after setting its own bit,
    // while the VM is still single-threaded and unpublished (no lite is
    // registered, no Strong can yet be touched by another thread). After
    // that single pre-publication write the byte is immutable for the VM's
    // lifetime, exactly like VM::gilOff().
    bool gilOff() const { return m_gilOff; }

    // U0c re-stamp; see gilOff() above. Called exactly once, from the VM
    // ctor's designation block (VM.cpp), pre-publication.
    void noteOwnerVMDesignatedGILOff() { m_gilOff = true; }

    HandleSlot allocate();
    void deallocate(HandleSlot);

    template<typename Visitor> void visitStrongHandles(Visitor&);

    template<bool isCellOnly>
    void writeBarrier(HandleSlot, JSValue);

    // GIL-off arms of the strongHandle* wrappers below: the same operations under m_strongLock.
    JS_EXPORT_PRIVATE HandleSlot allocateSlow();
    JS_EXPORT_PRIVATE void deallocateSlow(HandleSlot);
    template<bool isCellOnly>
    JS_EXPORT_PRIVATE void writeBarrierSlow(HandleSlot, JSValue);

    unsigned NODELETE protectedGlobalObjectCount();

    template<typename Functor> void forEachStrongHandle(const Functor&, const HashCountedSet<JSCell*>& skipSet);

private:
    typedef HandleNode Node;

    JS_EXPORT_PRIVATE void grow();
    
#if ENABLE(GC_VALIDATION) || ASSERT_ENABLED
    JS_EXPORT_PRIVATE bool isLiveNode(Node*);
#endif

    VM& m_vm;
    bool m_gilOff { false }; // Stamped from vm.gilOff() in the ctor; immutable (see gilOff()).
    // Leaf lock: GIL-off it serializes m_strongList / m_freeList mutation and the statistics walks
    // of m_strongList. It is never held across user JS and nothing but malloc is acquired under it,
    // so it is legal to take from destructors running inside the sweep. The GC root scan
    // (visitStrongHandles) runs inside the stop-the-world with every mutator parked and takes no lock.
    Lock m_strongLock;
    DoublyLinkedList<HandleBlock> m_blockList;

    using NodeList = SentinelLinkedList<Node, BasicRawSentinelNode<Node>>;
    NodeList m_strongList;
    SinglyLinkedList<Node> m_freeList;
};

inline HandleSet* HandleSet::heapFor(HandleSlot handle)
{
    return HandleNode::toHandleNode(handle)->handleSet();
}

// SharedGC (T9): main-VM-only — the server's HandleSet is constructed with
// the main VM; Strong<> users (Strong.h/StrongInlines.h) pass it to
// JSLockHolder/set(), i.e. the main VM's API lock. GIL-phase sound (JSLock
// migration, I2); post-GIL Strong creation from secondary threads still goes
// through that one JSLock (deviation 8: one VM per thread group).
inline VM& HandleSet::vm()
{
    return m_vm;
}

inline HandleSlot HandleSet::allocate()
{
    if (m_freeList.isEmpty())
        grow();

    HandleSet::Node* node = m_freeList.pop();
    new (NotNull, node) HandleSet::Node();
    return node->slot();
}

inline void HandleSet::deallocate(HandleSlot handle)
{
    HandleSet::Node* node = HandleNode::toHandleNode(handle);
    if (node->isOnList())
        NodeList::remove(node);
    m_freeList.push(node);
}

inline HandleSlot HandleNode::slot()
{
    return &m_value;
}

inline HandleSet* HandleNode::handleSet()
{
    return HandleBlock::blockFor(this)->handleSet();
}

template<typename Functor> void HandleSet::forEachStrongHandle(const Functor& functor, const HashCountedSet<JSCell*>& skipSet)
{
    auto walk = [&] {
        for (Node& node : m_strongList) {
            JSValue value = *node.slot();
            if (!value || !value.isCell())
                continue;
            if (skipSet.contains(value.asCell()))
                continue;
            functor(value.asCell());
        }
    };
    // Statistics walk outside the GC stop: GIL-off, Strong set-slot/free
    // traffic from other threads splices m_strongList concurrently.
    if (gilOff()) [[unlikely]] {
        Locker locker { m_strongLock };
        walk();
        return;
    }
    walk();
}

template<bool isCellOnly>
inline void HandleSet::writeBarrier(HandleSlot slot, JSValue value)
{
    bool valueIsNonEmptyCell = value && (isCellOnly || value.isCell());
    bool slotIsNonEmptyCell = *slot && (isCellOnly || slot->isCell());
    if (valueIsNonEmptyCell == slotIsNonEmptyCell)
        return;

    Node* node = HandleNode::toHandleNode(slot);
#if ENABLE(GC_VALIDATION)
    if (node->isOnList())
        RELEASE_ASSERT(isLiveNode(node));
#endif
    if (!valueIsNonEmptyCell) {
        ASSERT(slotIsNonEmptyCell);
        ASSERT(node->isOnList());
        NodeList::remove(node);
        return;
    }

    ASSERT(!slotIsNonEmptyCell);
    ASSERT(!node->isOnList());
    m_strongList.push(node);

#if ENABLE(GC_VALIDATION)
    RELEASE_ASSERT(isLiveNode(node));
#endif
}

// Strong allocate / free / set-slot (Strong.h, StrongInlines.h) go through these wrappers.
// GIL-off, Strongs are created, settled and destroyed on foreign threads, so two threads can
// mutate m_freeList / m_strongList at once; that arm goes out of line and takes m_strongLock.
// GIL-on and flag-off compile to the inline list operations plus one predicted-false byte test.
ALWAYS_INLINE HandleSlot strongHandleAllocate(HandleSet& set)
{
    if (set.gilOff()) [[unlikely]]
        return set.allocateSlow();
    return set.allocate();
}

ALWAYS_INLINE void strongHandleDeallocate(HandleSet& set, HandleSlot slot)
{
    if (set.gilOff()) [[unlikely]] {
        set.deallocateSlow(slot);
        return;
    }
    set.deallocate(slot);
}

template<bool isCellOnly>
ALWAYS_INLINE void strongHandleWriteBarrier(HandleSet& set, HandleSlot slot, JSValue value)
{
    if (set.gilOff()) [[unlikely]] {
        set.writeBarrierSlow<isCellOnly>(slot, value);
        return;
    }
    set.writeBarrier<isCellOnly>(slot, value);
}

} // namespace JSC

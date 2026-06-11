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
    DoublyLinkedList<HandleBlock> m_blockList;

    using NodeList = SentinelLinkedList<Node, BasicRawSentinelNode<Node>>;
    NodeList m_strongList;
    SinglyLinkedList<Node> m_freeList;
};

// UNGIL §F.3 (U-T8 seams, wired by the review fix; RE-INLINED by the
// flag-off codegen review round): GIL-off, Strong allocate / free /
// set-slot traffic is inherently cross-thread (AsyncTicket Strongs settled
// and destroyed off-owner, ThreadObject completion Strongs, SD15 handoff
// records created on spawned threads), and two threads mutating
// m_freeList/m_strongList unlocked is heap corruption — so Strong.h /
// StrongInlines.h route their three call-site shapes through the
// strongHandle* wrappers (defined inline at the bottom of this header).
// Each wrapper tests the HandleSet's cached gilOff byte and falls through
// to the EXISTING inline list/free-list ops, so flag-off codegen is the
// pre-ungil inline shape plus one predicted-false byte test — no cross-TU
// call. Only the gilOff arm goes out of line, to the locked *Slow entry
// points below (HandleSet.cpp, m_strongLock).
JS_EXPORT_PRIVATE HandleSlot strongHandleAllocateSlow(HandleSet&);
JS_EXPORT_PRIVATE void strongHandleDeallocateSlow(HandleSet&, HandleSlot);
template<bool isCellOnly>
JS_EXPORT_PRIVATE void strongHandleWriteBarrierSlow(HandleSet&, HandleSlot, JSValue);

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
    for (Node& node : m_strongList) {
        JSValue value = *node.slot();
        if (!value || !value.isCell())
            continue;
        if (skipSet.contains(value.asCell()))
            continue;
        functor(value.asCell());
    }
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

// §F.3 Strong-mutation wrappers (see the seam comment above the class).
// GIL-on / flag-off: one predicted-false byte test, then the same inline
// bodies the pre-ungil call sites compiled to. GIL-off: the locked slow
// path (HandleSet.cpp).
ALWAYS_INLINE HandleSlot strongHandleAllocate(HandleSet& set)
{
    if (set.gilOff()) [[unlikely]]
        return strongHandleAllocateSlow(set);
    return set.allocate();
}

ALWAYS_INLINE void strongHandleDeallocate(HandleSet& set, HandleSlot slot)
{
    if (set.gilOff()) [[unlikely]] {
        strongHandleDeallocateSlow(set, slot);
        return;
    }
    set.deallocate(slot);
}

template<bool isCellOnly>
ALWAYS_INLINE void strongHandleWriteBarrier(HandleSet& set, HandleSlot slot, JSValue value)
{
    if (set.gilOff()) [[unlikely]] {
        strongHandleWriteBarrierSlow<isCellOnly>(set, slot, value);
        return;
    }
    set.writeBarrier<isCellOnly>(slot, value);
}

} // namespace JSC

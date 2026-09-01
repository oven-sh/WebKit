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

#include "config.h"
#include "HandleSet.h"

#include "HandleBlock.h"
#include "HandleBlockInlines.h"
#include "JSCJSValueInlines.h"
#include "VM.h"

namespace JSC {

// One HandleSet per VM, shared by every thread of a GIL-off VM: Strong lifetime is not
// thread-affine (a Strong is routinely created on one thread and cleared or destroyed on
// another), so the GIL-off arms below serialize on the leaf m_strongLock. Mutation additionally
// requires an entered thread with heap access, which is what lets visitStrongHandles scan the
// list lock-free inside the stop-the-world.

HandleSlot HandleSet::allocateSlow()
{
    ASSERT(gilOff());
    Locker locker { m_strongLock };
    return allocate();
}

void HandleSet::deallocateSlow(HandleSlot slot)
{
    ASSERT(gilOff());
    Locker locker { m_strongLock };
    deallocate(slot);
}

template<bool isCellOnly>
void HandleSet::writeBarrierSlow(HandleSlot slot, JSValue value)
{
    ASSERT(gilOff());
    Locker locker { m_strongLock };
    writeBarrier<isCellOnly>(slot, value);
}

template void HandleSet::writeBarrierSlow<true>(HandleSlot, JSValue);
template void HandleSet::writeBarrierSlow<false>(HandleSlot, JSValue);

HandleSet::HandleSet(VM& vm)
    : m_vm(vm)
    , m_gilOff(vm.gilOff())
{
    grow();
}

HandleSet::~HandleSet()
{
    while (!m_blockList.isEmpty())
        HandleBlock::destroy(m_blockList.removeHead());
}

void HandleSet::grow()
{
    HandleBlock* newBlock = HandleBlock::create(this);
    m_blockList.append(newBlock);

    for (int i = newBlock->nodeCapacity() - 1; i >= 0; --i) {
        Node* node = newBlock->nodeAtIndex(i);
        new (NotNull, node) Node;
        m_freeList.push(node);
    }
}

template<typename Visitor>
void HandleSet::visitStrongHandles(Visitor& visitor)
{
    // Root scan: runs inside the stop-the-world with every mutator parked, so the list is
    // quiescent and no lock is taken.
    for (Node& node : m_strongList) {
#if ENABLE(GC_VALIDATION)
        RELEASE_ASSERT(isLiveNode(&node));
#endif
        visitor.appendUnbarriered(*node.slot());
    }
}

template void HandleSet::visitStrongHandles(AbstractSlotVisitor&);
template void HandleSet::visitStrongHandles(SlotVisitor&);

unsigned HandleSet::protectedGlobalObjectCount()
{
    // Statistics walk outside the GC stop: GIL-off, Strong set-slot/free
    // traffic from other threads splices m_strongList concurrently.
    auto walk = [&] {
        unsigned count = 0;
        for (Node& node : m_strongList) {
            JSValue value = *node.slot();
            if (value.isObject() && asObject(value.asCell())->isGlobalObject())
                count++;
        }
        return count;
    };
    if (gilOff()) [[unlikely]] {
        Locker locker { m_strongLock };
        return walk();
    }
    return walk();
}

#if ENABLE(GC_VALIDATION) || ASSERT_ENABLED
bool HandleSet::isLiveNode(Node* node)
{
    if (node->prev()->next() != node)
        return false;
    if (node->next()->prev() != node)
        return false;
        
    return true;
}
#endif // ENABLE(GC_VALIDATION) || ASSERT_ENABLED

} // namespace JSC

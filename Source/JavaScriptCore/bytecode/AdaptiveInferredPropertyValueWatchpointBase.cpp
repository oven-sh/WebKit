/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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
#include "AdaptiveInferredPropertyValueWatchpointBase.h"

#include "JSCInlines.h"
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AdaptiveInferredPropertyValueWatchpointBase);

AdaptiveInferredPropertyValueWatchpointBase::AdaptiveInferredPropertyValueWatchpointBase(const ObjectPropertyCondition& key)
    : m_key(key)
{
    RELEASE_ASSERT(key.kind() == PropertyCondition::Equivalence);
}

void AdaptiveInferredPropertyValueWatchpointBase::initialize(const ObjectPropertyCondition& key)
{
    m_key = key;
    RELEASE_ASSERT(key.kind() == PropertyCondition::Equivalence);
}

bool AdaptiveInferredPropertyValueWatchpointBase::install(VM& vm)
{
    // With the flag on, another thread can transition the watched object's
    // structure between the caller's watchability check and this install
    // (DFGAdaptiveStructureWatchpoint.cpp). A refused install is handled by
    // the caller, so return false instead of asserting.
    if (Options::useJSThreads()) [[unlikely]] {
        if (!m_key.isWatchable(PropertyCondition::MakeNoChanges, Concurrency::MainThread))
            return false;
    } else
        ASSERT(m_key.isWatchable(PropertyCondition::MakeNoChanges)); // This is really costly.

    Structure* structure = m_key.object()->structure();

    if (!structure->addTransitionWatchpoint(&m_structureWatchpoint))
        return false;

    PropertyOffset offset = structure->get(vm, m_key.uid());
    // Flag-on the structure read above can already differ from the one the
    // watchability check saw (another thread transitioned or flattened the
    // object in between), and then have no replacement set at this offset, or
    // no such property at all: a refused install, like the fired-set case
    // below (seventh round; the amplifier found the null set at about 1 in 100
    // runs of jit/global-property-cache-vs-global-transitions.js GIL off).
    WatchpointSet* set = isValidOffset(offset) ? structure->propertyReplacementWatchpointSet(offset) : nullptr;
    ASSERT(set || Options::useJSThreads());
    if (set && set->add(&m_propertyWatchpoint))
        return true;

    // Flag-on only: the replacement set fired between the two links, or is
    // not there. Leave nothing linked, so a refused install never leaves a
    // half-armed pair.
    Locker locker { g_watchpointMembershipLock };
    if (m_structureWatchpoint.isOnList())
        m_structureWatchpoint.remove();
    return false;
}

void AdaptiveInferredPropertyValueWatchpointBase::fire(VM& vm, const FireDetail& detail)
{
    // One of the watchpoints fired, but the other one didn't. Make sure that neither of them are
    // in any set anymore. This simplifies things by allowing us to reinstall the watchpoints
    // wherever from scratch.
    if (Options::useJSThreads()) [[unlikely]] {
        // AB18-G: these unlink from sets (per-Structure transition /
        // replacement sets) that other mutators can concurrently add() to;
        // the check-and-remove must be one critical section under the
        // membership lock (see Watchpoint.h).
        Locker locker { g_watchpointMembershipLock };
        if (m_structureWatchpoint.isOnList())
            m_structureWatchpoint.remove();
        if (m_propertyWatchpoint.isOnList())
            m_propertyWatchpoint.remove();
    } else {
        if (m_structureWatchpoint.isOnList())
            m_structureWatchpoint.remove();
        if (m_propertyWatchpoint.isOnList())
            m_propertyWatchpoint.remove();
    }

    if (!isValid())
        return;

    // A refused install (flag-on: a set fired under us) is a failed adaptation.
    // Flag-on, when this fire runs inside a collection phase on this thread
    // (InferredValue clean-up at GC end), the re-adaptation must not CREATE
    // the replacement set: ensuring it allocates a StructureRareData cell,
    // which the allocator refuses while the mutator state is not Running (the
    // mirror harness hit that release assertion once, seventh round).
    // Single-threaded the structure re-read here is the one the condition was
    // installed on and already has the set; with threads it can be a structure
    // another thread moved the object to. Outside a collection the set is
    // ensured as before (objectmodel/indexing-transition-keeps-adaptive-
    // watchpoint.js depends on the re-install creating it).
    PropertyCondition::WatchabilityEffort effort = PropertyCondition::EnsureWatchability;
    if (Options::useJSThreads() && vm.heap.mutatorState() != MutatorState::Running) [[unlikely]]
        effort = PropertyCondition::MakeNoChanges; // inside a collection phase on this thread: adapt only if the set already exists
    if (m_key.isWatchable(effort) && install(vm))
        return;

    handleFire(vm, detail);
}

bool AdaptiveInferredPropertyValueWatchpointBase::isValid() const
{
    return true;
}

void AdaptiveInferredPropertyValueWatchpointBase::StructureWatchpoint::fireInternal(VM& vm, const FireDetail& detail)
{
    ptrdiff_t myOffset = OBJECT_OFFSETOF(AdaptiveInferredPropertyValueWatchpointBase, m_structureWatchpoint);

    AdaptiveInferredPropertyValueWatchpointBase* parent = std::bit_cast<AdaptiveInferredPropertyValueWatchpointBase*>(std::bit_cast<char*>(this) - myOffset);

    parent->fire(vm, detail);
}

void AdaptiveInferredPropertyValueWatchpointBase::PropertyWatchpoint::fireInternal(VM& vm, const FireDetail& detail)
{
    ptrdiff_t myOffset = OBJECT_OFFSETOF(AdaptiveInferredPropertyValueWatchpointBase, m_propertyWatchpoint);

    AdaptiveInferredPropertyValueWatchpointBase* parent = std::bit_cast<AdaptiveInferredPropertyValueWatchpointBase*>(std::bit_cast<char*>(this) - myOffset);
    
    parent->fire(vm, detail);
}
    
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

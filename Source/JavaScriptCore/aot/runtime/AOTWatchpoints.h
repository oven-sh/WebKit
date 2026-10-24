/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include "ObjectPropertyConditionSet.h"
#include <memory>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

// A watchpoint is not destroyed while it fires: its owner frees one that has fired at its next sweep or with the next watch in its place.
class PropertyValueWatchpoint final : public AdaptiveInferredPropertyValueWatchpointBase {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PropertyValueWatchpoint);
public:
    using Callback = void (*)(PropertyValueWatchpoint&);

    // Null where the holder has no own property of the name that can be watched, or its value is not accepted.
    static std::unique_ptr<PropertyValueWatchpoint> tryWatch(VM& vm, JSObject* holder, UniquedStringImpl* uid, const Invocable<bool(JSValue)> auto& accepts, Callback callback, void* owner, uint32_t number = 0)
    {
        ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, holder, uid);
        if (!condition || !accepts(condition.requiredValue()) || !condition.isWatchable(PropertyCondition::EnsureWatchability))
            return nullptr;
        auto watchpoint = std::unique_ptr<PropertyValueWatchpoint>(new PropertyValueWatchpoint(condition, callback, owner, number));
        watchpoint->install(vm);
        return watchpoint;
    }

    template<typename Owner> Owner& owner() const { return *static_cast<Owner*>(m_owner); }
    uint32_t number() const { return m_number; }
    bool hasFired() const { return !m_callback; }

private:
    PropertyValueWatchpoint(const ObjectPropertyCondition& condition, Callback callback, void* owner, uint32_t number)
        : AdaptiveInferredPropertyValueWatchpointBase(condition)
        , m_callback(callback)
        , m_owner(owner)
        , m_number(number)
    {
    }

    void handleFire(VM&, const FireDetail&) final { std::exchange(m_callback, nullptr)(*this); }

    Callback m_callback;
    void* m_owner;
    uint32_t m_number;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ClassTable;
struct State;

class MemberGuardWatchpoint final : public AdaptiveInferredPropertyValueWatchpointBase {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(MemberGuardWatchpoint);
public:
    MemberGuardWatchpoint(const ObjectPropertyCondition&, ClassTable&, const LayoutData& classLayout, ClassSide);

private:
    void handleFire(VM&, const FireDetail&) final;

    ClassTable& m_classes;
    const LayoutData& m_classLayout;
    ClassSide m_side;
};

class MemberGuardWatchpoints {
    WTF_MAKE_NONCOPYABLE(MemberGuardWatchpoints);
public:
    explicit MemberGuardWatchpoints(State&);
    ~MemberGuardWatchpoints();

    bool tryWatch(VM&, const LayoutData& classLayout, ClassSide, JSObject* holder, UniquedStringImpl*);
    void stopWatching(const LayoutData& classLayout);

private:
    using Watchpoints = Vector<std::unique_ptr<MemberGuardWatchpoint>>;

    State& m_state;
    UncheckedKeyHashMap<uint32_t, Watchpoints, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_watchpointsByClass;
};

bool declaresMember(const LayoutData& classLayout, ClassSide, uint32_t key);
void didEvaluateClassAgain(State&, const LayoutData& classLayout);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

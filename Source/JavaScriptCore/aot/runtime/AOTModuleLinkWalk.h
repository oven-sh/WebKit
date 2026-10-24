/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModules.h"
#include "AOTVector.h"
#include "ThrowScope.h"
#include <span>
#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

// The modules that a link reaches from a root and that have work in it, in the order of the requests.
class ModuleLinkWalk {
    WTF_MAKE_NONCOPYABLE(ModuleLinkWalk);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ModuleLinkWalk);
public:
    enum class Leaf : uint8_t { CapturesExports, IsUncompiled };

    ModuleLinkWalk(const Modules&, ModuleID root);

    ModuleID root() const { return m_root; }
    // Such a walk enters the root only. Of the modules below it, didLeave() hears of those that resolve names, whether another link has finished them or not.
    bool hasPlan() const { return !m_plan.empty(); }
    // Of a plan other than the entry point's.
    std::optional<ModuleID> memberOfCycle() const { return m_memberOfCycle; }

    // shouldEnter(module) answers for a module with requests, visitLeaf(leaf, module) may throw, didLeave(module, resolvesNames) follows the requests of an entered module.
    template<typename ShouldEnterFunctor, typename VisitLeafFunctor, typename DidLeaveFunctor>
    bool replay(ThrowScope&, const ShouldEnterFunctor&, const VisitLeafFunctor&, const DidLeaveFunctor&) const;

private:
    enum class StepKind : uint8_t { CapturesExports, IsUncompiled, Enter, Leave };

    struct Step {
        uint32_t module;
        uint32_t kind : 2;
        uint32_t resolvesNames : 1;
        uint32_t leave : 29;
    };
    static_assert(std::is_trivially_copyable_v<Step> && sizeof(Step) == 8);

    void append(ModuleID, StepKind, bool resolvesNames = false);
    template<typename ShouldEnterFunctor, typename VisitLeafFunctor, typename DidLeaveFunctor>
    bool replayPlan(ThrowScope&, const ShouldEnterFunctor&, const VisitLeafFunctor&, const DidLeaveFunctor&) const;

    ModuleID m_root;
    std::span<const LinkPlanStepData> m_plan;
    std::optional<ModuleID> m_memberOfCycle;
    Vector<Step, 64> m_steps;
};

template<typename ShouldEnterFunctor, typename VisitLeafFunctor, typename DidLeaveFunctor>
ALWAYS_INLINE bool ModuleLinkWalk::replayPlan(ThrowScope& scope, const ShouldEnterFunctor& shouldEnter, const VisitLeafFunctor& visitLeaf, const DidLeaveFunctor& didLeave) const
{
    if (!shouldEnter(m_root))
        return false;
    for (const LinkPlanStepData& step : m_plan) {
        if (step.resolvesNames) {
            didLeave(step.targetModule(), true);
            continue;
        }
        visitLeaf(Leaf::CapturesExports, step.targetModule());
        RETURN_IF_EXCEPTION(scope, true);
    }
    return true;
}

template<typename ShouldEnterFunctor, typename VisitLeafFunctor, typename DidLeaveFunctor>
ALWAYS_INLINE bool ModuleLinkWalk::replay(ThrowScope& scope, const ShouldEnterFunctor& shouldEnter, const VisitLeafFunctor& visitLeaf, const DidLeaveFunctor& didLeave) const
{
    if (hasPlan())
        return replayPlan(scope, shouldEnter, visitLeaf, didLeave);

    std::span<const Step> steps = m_steps.span();
    for (size_t index = 0; index < steps.size(); ++index) {
        const Step& step = steps[index];
        ModuleID module(step.module);
        switch (static_cast<StepKind>(step.kind)) {
        case StepKind::Leave:
            didLeave(module, !!step.resolvesNames);
            continue;
        case StepKind::Enter:
            if (shouldEnter(module))
                continue;
            if (!index)
                return false;
            index = step.leave;
            continue;
        case StepKind::CapturesExports:
        case StepKind::IsUncompiled:
            visitLeaf(static_cast<StepKind>(step.kind) == StepKind::CapturesExports ? Leaf::CapturesExports : Leaf::IsUncompiled, module);
            RETURN_IF_EXCEPTION(scope, !!index);
            if (!index)
                return false;
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return !steps.empty();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

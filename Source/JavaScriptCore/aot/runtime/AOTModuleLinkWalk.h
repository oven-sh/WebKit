/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleGraph.h"
#include "AOTModules.h"
#include "AOTVector.h"
#include "ThrowScope.h"
#include <span>
#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct WalksOfOneLink final : RefCounted<WalksOfOneLink> {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WalksOfOneLink);

    explicit WalksOfOneLink(const VisitedModuleRecords& visited)
        : visited(&visited)
    {
    }

    const VisitedModuleRecords* visited;
    Vector<uint64_t> modulesWalked;
    Vector<ModuleID, 8> roots;
};

class ModuleLinkWalk {
    WTF_MAKE_NONCOPYABLE(ModuleLinkWalk);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ModuleLinkWalk);
public:
    enum class Leaf : uint8_t { CapturesExports, IsUncompiled };

    ModuleLinkWalk(const Modules&, ModuleID root, RefPtr<WalksOfOneLink>&& earlier = nullptr);

    ModuleID root() const { return m_root; }
    bool bringUpToDate(const Modules&);
    bool hasPlan() const { return !m_plan.empty(); }
    std::span<const uint32_t> modulesThatOnlyBecomeLinked() const LIFETIME_BOUND { return m_modulesThatOnlyBecomeLinked.span(); }

    template<typename ShouldEnterFunctor, typename VisitLeafFunctor, typename DidLeaveFunctor>
    bool replay(ThrowScope&, const ShouldEnterFunctor&, const VisitLeafFunctor&, const DidLeaveFunctor&) const;

    void forEachModuleOfProgram(NOESCAPE const Invocable<void(ModuleID)> auto& functor) const
    {
        for (const Step& step : m_steps) {
            if (static_cast<StepKind>(step.kind) == StepKind::Enter || static_cast<StepKind>(step.kind) == StepKind::CapturesExports)
                functor(ModuleID(step.module));
        }
        for (uint32_t module : m_modulesThatOnlyBecomeLinked)
            functor(ModuleID(module));
    }

private:
    using StepKind = StepOfLinkWalk;

    struct Step {
        uint32_t module;
        uint32_t kind : 2;
        uint32_t resolvesNames : 1;
        uint32_t leave : 29;
    };
    static_assert(std::is_trivially_copyable_v<Step> && sizeof(Step) == 8);

    void append(ModuleID, StepKind, bool resolvesNames = false);

    ModuleID m_root;
    RefPtr<WalksOfOneLink> m_earlier;
    size_t m_numberOfEarlierRoots { 0 };
    unsigned m_linkEpoch;
    std::span<const LinkPlanStepData> m_plan;
    Vector<Step, 64> m_steps;
    Vector<uint32_t, 64> m_modulesThatOnlyBecomeLinked;
};

template<typename ShouldEnterFunctor, typename VisitLeafFunctor, typename DidLeaveFunctor>
ALWAYS_INLINE bool ModuleLinkWalk::replay(ThrowScope& scope, const ShouldEnterFunctor& shouldEnter, const VisitLeafFunctor& visitLeaf, const DidLeaveFunctor& didLeave) const
{
    if (hasPlan()) {
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

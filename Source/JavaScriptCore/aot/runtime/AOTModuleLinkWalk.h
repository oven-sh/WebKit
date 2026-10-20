/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModules.h"
#include "ThrowScope.h"
#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

// The modules that a link reaches from a root, in the order of the requests. No code of the program runs while they are recorded.
// A module leaves ModuleStatus::Unlinked together with everything it requests and never returns to it while it is linked,
// so each step of the link, in this job or a later one, replays the record and passes over what another link has finished since.
class ModuleLinkWalk {
    WTF_MAKE_NONCOPYABLE(ModuleLinkWalk);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ModuleLinkWalk);
public:
    enum class Leaf : uint8_t { CapturesExports, IsUncompiled };

    ModuleLinkWalk(const Modules&, ModuleID root);

    ModuleID root() const { return m_root; }

    // shouldEnter(module) answers for a module with requests, visitLeaf(leaf, module) may throw, didLeave(module, resolvesNames) follows the requests of an entered module.
    // Returns whether the root was entered.
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

    ModuleID m_root;
    Vector<Step, 64> m_steps;
};

template<typename ShouldEnterFunctor, typename VisitLeafFunctor, typename DidLeaveFunctor>
ALWAYS_INLINE bool ModuleLinkWalk::replay(ThrowScope& scope, const ShouldEnterFunctor& shouldEnter, const VisitLeafFunctor& visitLeaf, const DidLeaveFunctor& didLeave) const
{
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

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTVMState.h"
#include "VM.h"
#include <optional>

namespace JSC { namespace AOT {

// While it lives, the image that the cell is of is the one that runs: what the engine asks a compiled object or a closure, the tables of that image answer.
class ImageOfCellScope {
    WTF_MAKE_NONCOPYABLE(ImageOfCellScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE ImageOfCellScope(VM& vm, const JSCell* cell)
    {
        if (vm.aotState().hasSeveralImages) [[unlikely]] {
            if (Instance* owner = Instance::owning(cell))
                m_scope.emplace(vm.aotState(), owner->program);
        }
    }

    // For whoever has no VM at hand. A closure that never escapes lies on the stack, where no block says which VM it is of.
    ALWAYS_INLINE explicit ImageOfCellScope(const JSCell* cell)
    {
        if (Image::sharedByAllThreads()) [[likely]]
            return;
        runImageOf(cell);
    }

private:
    JS_EXPORT_PRIVATE void runImageOf(const JSCell*);

    std::optional<RunningImageScope> m_scope;
};

template<typename Functor>
NEVER_INLINE auto runInImageOfCellAmongSeveral(const JSCell* cell, const Functor& functor) -> decltype(functor())
{
    ImageOfCellScope imageOfCell(cell);
    return functor();
}

// For a short function that is asked often. A scope has something to restore at the end of the function, for which the function keeps a frame and saves registers
// whether or not there is a second image. Here a process with one image asks one word and runs the functor as if it stood there alone.
template<typename Functor>
ALWAYS_INLINE auto runInImageOfCell(const JSCell* cell, const Functor& functor) -> decltype(functor())
{
    if (Image::sharedByAllThreads()) [[likely]]
        return functor();
    return runInImageOfCellAmongSeveral(cell, functor);
}

// The tables of an instance create what they hand out under their own image, whichever image runs when they are asked.
class ImageOfInstanceScope {
    WTF_MAKE_NONCOPYABLE(ImageOfInstanceScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE explicit ImageOfInstanceScope(const Instance& instance)
    {
        VMState& state = instance.vm->aotState();
        if (state.hasSeveralImages) [[unlikely]]
            m_scope.emplace(state, instance.program);
    }

private:
    std::optional<RunningImageScope> m_scope;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "ModuleGraphLoadingState.h"

#include "JSCellInlines.h"
#include "JSPromise.h"
#include "ScriptFetcher.h"

namespace JSC {

const ClassInfo ModuleGraphLoadingState::s_info = { "ModuleGraphLoadingState"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleGraphLoadingState) };

ModuleGraphLoadingState::ModuleGraphLoadingState(VM& vm, Structure* structure, JSPromise* promise, RefPtr<ScriptFetcher> scriptFetcher, JSValue asyncContext)
    : Base(vm, structure)
    , m_promise(promise, WriteBarrierEarlyInit)
#if USE(BUN_JSC_ADDITIONS)
    , m_asyncContext(asyncContext, WriteBarrierEarlyInit)
#endif
    , m_scriptFetcher(WTF::move(scriptFetcher))
{
#if !USE(BUN_JSC_ADDITIONS)
    UNUSED_PARAM(asyncContext);
#endif
}

void ModuleGraphLoadingState::destroy(JSCell* cell)
{
    SUPPRESS_MEMORY_UNSAFE_CAST auto* thisObject = static_cast<ModuleGraphLoadingState*>(cell);
    thisObject->~ModuleGraphLoadingState();
}

void ModuleGraphLoadingState::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
}

template<typename Visitor>
void ModuleGraphLoadingState::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleGraphLoadingState>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_promise);
    visitor.append(thisObject->m_fulfillment);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_asyncContext);
#endif
    Locker locker { thisObject->cellLock() };
    visitor.append(thisObject->m_visited.begin(), thisObject->m_visited.end());
}

DEFINE_VISIT_CHILDREN(ModuleGraphLoadingState);

ModuleGraphLoadingState* ModuleGraphLoadingState::create(VM& vm, JSPromise* promise, RefPtr<ScriptFetcher> scriptFetcher, JSValue asyncContext)
{
    ModuleGraphLoadingState* instance = new (NotNull, allocateCell<ModuleGraphLoadingState>(vm)) ModuleGraphLoadingState(vm, vm.moduleGraphLoadingStateStructure.get(), promise, WTF::move(scriptFetcher), asyncContext);
    instance->finishCreation(vm);
    return instance;
}

void ModuleGraphLoadingState::appendVisited(VM& vm, CyclicModuleRecord* cyclic)
{
    Locker locker { cellLock() };
    m_visited.append(WriteBarrier(vm, this, cyclic));
    m_visitedSet.add(cyclic);
}

} // namespace JSC

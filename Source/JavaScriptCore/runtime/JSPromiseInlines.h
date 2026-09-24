/*
 * Copyright (C) 2026 Codeblog Corp. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "AsyncContextSwapScope.h"
#include "JSCellInlines.h"
#include "JSGlobalObject.h"
#include "JSPromise.h"

namespace JSC {

// JSPromise::create(), for a promise whose reject function is handed out (see ownerWhenMade()). In its caller, so
// that making one costs a program that has no owner no more than it did.
ALWAYS_INLINE JSPromise* JSPromise::createKeepingOwner(VM& vm, JSGlobalObject* globalObject, Structure* structure)
{
    JSPromise* promise = new (NotNull, allocateCell<JSPromise>(vm)) JSPromise(vm, structure);
    promise->finishCreation(vm);
    if (vm.isAsyncContextOwnerTracked()) [[unlikely]]
        promise->m_slot.setWithoutWriteBarrier(AsyncContextSwapScope::numberOfOwner(globalObject->m_asyncContextData->getInternalField(1)));
    return promise;
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)

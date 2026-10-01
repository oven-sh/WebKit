/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

#pragma once

#include "FunctionExecutable.h"
#include "InferredValueInlines.h"
#include "ScriptExecutableInlines.h"
#include "StructureCreateInlines.h"
#include "StaticHeap.h"

namespace JSC {

inline Structure* FunctionExecutable::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue proto)
{
    return Structure::create(vm, globalObject, proto, TypeInfo(FunctionExecutableType, StructureFlags), info());
}

inline Structure* FunctionExecutable::createStructureOfShortForm(VM& vm, JSGlobalObject* globalObject, JSValue proto)
{
    return Structure::create(vm, globalObject, proto, TypeInfo(ShortFunctionExecutableType, StructureFlags), info());
}

inline void FunctionExecutable::notifyCreation(VM& vm, JSFunction* function, const char* reason)
{
    if (isShortForm())
        return;
    inFull()->m_singleton.notifyWrite(vm, this, function, reason);
    if (inFull()->m_singleton.hasBeenInvalidated() && !inFull()->m_unlinkedExecutable->singletonHasBeenInvalidated())
        inFull()->m_unlinkedExecutable->setSingletonHasBeenInvalidated();
}

inline void FunctionExecutable::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope collectionScope)
{
    inFull()->m_singleton.reconcileWeakReferencesAtGCEnd(vm, collectionScope);
    jettisonCodeBlockEdgeIfDead(vm, m_codeBlockForCall);
    jettisonCodeBlockEdgeIfDead(vm, m_codeBlockForConstruct);
    vm.heap.functionExecutableSpaceAndSet.outputConstraintsSet.remove(this);
}

inline FunctionCodeBlock* FunctionExecutable::replaceCodeBlockWith(VM& vm, CodeSpecializationKind kind, CodeBlock* newCodeBlock)
{
    RELEASE_ASSERT(!isShortForm());
    if (kind == CodeSpecializationKind::CodeForCall) {
        FunctionCodeBlock* oldCodeBlock = codeBlockForCall();
        inFull()->m_codeBlockForCall.setMayBeNull(vm, this, newCodeBlock);
        return oldCodeBlock;
    }
    ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
    FunctionCodeBlock* oldCodeBlock = codeBlockForConstruct();
    inFull()->m_codeBlockForConstruct.setMayBeNull(vm, this, newCodeBlock);
    return oldCodeBlock;
}

inline JSString* FunctionExecutable::toString(JSGlobalObject* globalObject)
{
    // (An executable in the static heap is read-only, so the result is not cached.)
    if (!rareData() && StaticHeap::contains(this))
        return toStringSlow(globalObject);
    RareData& rareData = ensureRareData();
    if (!rareData.m_asString)
        return toStringSlow(globalObject);
    return rareData.m_asString.get();
}

} // namespace JSC


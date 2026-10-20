/*
 * Copyright (C) 2009-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2025 Codeblog CORP.
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
#include "CachedCall.h"

#include "ExceptionHelpers.h"
#include "Interpreter.h"
#include "JSFunction.h"
#include "ThrowScope.h"
#include "VMEntryScope.h"
#include <wtf/Scope.h>

namespace JSC {

static JSGlobalObject* calleeRealm(const CallData& callData)
{
    RELEASE_ASSERT(callData.type == CallData::Type::JS || callData.type == CallData::Type::Compiled);
    return callData.type == CallData::Type::JS ? callData.js.scope->realm() : callData.compiled.realm;
}

CachedCall::CachedCall(ConstructionKey, JSGlobalObject* globalObject, JSObject* callee, const CallData& callData, int argumentCount)
    : CallLinkInfoBase(CallSiteType::CachedCall)
    , m_vm(globalObject->vm())
    , m_entryScope(m_vm, calleeRealm(callData))
    , m_functionExecutable(callData.type == CallData::Type::JS ? callData.js.functionExecutable : nullptr)
    , m_scope(callData.type == CallData::Type::JS ? callData.js.scope : nullptr)
{
    VM& vm = m_vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
#if ASSERT_ENABLED
    auto updateValidStatus = makeScopeExit([&] {
        m_valid = !scope.exception();
    });
#endif
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return;
    }

    if (vm.disallowVMEntryCount) [[unlikely]] {
        VM::checkVMEntryPermission();
        throwStackOverflowError(globalObject, scope);
        return;
    }

    m_arguments.ensureCapacity(argumentCount);
    if (m_arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }

    if (callData.type == CallData::Type::Compiled) {
        m_addressForCall = const_cast<void*>(callData.compiled.entrypoint);
        m_protoCallFrame.init(nullptr, callData.compiled.realm, callee, jsUndefined(), nullptr, argumentCount + 1, const_cast<EncodedJSValue*>(m_arguments.data()));
        return;
    }

    auto* newCodeBlock = m_vm.interpreter.prepareForCachedCall(*this, uncheckedDowncast<JSFunction>(callee));
    if (scope.exception()) [[unlikely]]
        return;
    m_numParameters = newCodeBlock->numParameters();
    m_protoCallFrame.init(newCodeBlock, callee->realm(), callee, jsUndefined(), nullptr, argumentCount + 1, const_cast<EncodedJSValue*>(m_arguments.data()));
}

void CachedCall::relink()
{
    VM& vm = m_vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(m_functionExecutable);
    auto* codeBlock = m_vm.interpreter.prepareForCachedCall(*this, this->function());
    RETURN_IF_EXCEPTION(scope, void());
    m_protoCallFrame.setCodeBlock(codeBlock);
}

} // namespace JSC

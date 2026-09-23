/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSScriptExecutionOwnerEnvironment.h"

#if USE(BUN_JSC_ADDITIONS)

#include "JSCInlines.h"
#include "JSLexicalEnvironmentInlines.h"
#include "JSScriptExecutionOwnerEnvironmentInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

const ClassInfo JSScriptExecutionOwnerEnvironment::s_info = { "JSScriptExecutionOwnerEnvironment"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSScriptExecutionOwnerEnvironment) };

JSScriptExecutionOwnerEnvironment* JSScriptExecutionOwnerEnvironment::create(VM& vm, JSGlobalObject* globalObject, JSScope* currentScope, SymbolTable* symbolTable, JSValue initialValue)
{
    globalObject->didMakeScriptExecutionOwner(vm);
    // Made on first use: before the cell it is for.
    Structure* structure = globalObject->scriptExecutionOwnerEnvironmentStructure();
    auto* result = new (NotNull, allocateCell<JSScriptExecutionOwnerEnvironment>(vm, allocationSize(symbolTable)))
        JSScriptExecutionOwnerEnvironment(vm, structure, currentScope, symbolTable, initialValue);
    result->finishCreation(vm);
    return result;
}

inline JSScriptExecutionOwnerEnvironment::JSScriptExecutionOwnerEnvironment(VM& vm, Structure* structure, JSScope* currentScope, SymbolTable* symbolTable, JSValue initialValue)
    : Base(vm, structure, currentScope, symbolTable, initialValue)
{
    embedderObjectSlot().clear();
    evalEnabledSlot() = true;
}

template<typename Visitor>
void JSScriptExecutionOwnerEnvironment::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSScriptExecutionOwnerEnvironment>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->embedderObjectSlot());
}

DEFINE_VISIT_CHILDREN(JSScriptExecutionOwnerEnvironment);

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS)

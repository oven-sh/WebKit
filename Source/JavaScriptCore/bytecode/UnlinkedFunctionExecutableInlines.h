/*
 * Copyright (C) 2012-2022 Apple Inc. All rights reserved.
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

#include "BuiltinNames.h"
#include "CommonIdentifiers.h"
#include "StructureCreateInlines.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"

namespace JSC {

inline Structure* UnlinkedFunctionExecutable::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue proto)
{
    return Structure::create(vm, globalObject, proto, TypeInfo(UnlinkedFunctionExecutableType, StructureFlags), info());
}

// https://tc39.es/ecma262/#sec-exports-runtime-semantics-evaluation
// The parser binds an anonymous default export to the private name "*default*" and gives the function or class the name
// of that binding. SetFunctionName gives it the name "default".
inline const Identifier& UnlinkedFunctionExecutable::nameFromSyntaxTree(VM& vm, const Identifier& nameInSyntaxTree)
{
    // Every function gets here. The private name is one object for all VMs, so the compare is with a constant address.
    if (nameInSyntaxTree.impl() == std::bit_cast<SymbolImpl*>(&Symbols::starDefaultPrivateName)) [[unlikely]]
        return vm.propertyNames->defaultKeyword;
    return nameInSyntaxTree;
}

inline const Identifier& UnlinkedFunctionExecutable::bindingName(VM& vm) const
{
    ASSERT(m_hasName);
    const Identifier& name = ecmaName();
    // No declaration is written with the name "default": it is a reserved word.
    if (name == vm.propertyNames->defaultKeyword) [[unlikely]]
        return vm.propertyNames->starDefaultPrivateName;
    return name;
}

inline void UnlinkedFunctionExecutable::setEcmaName(VM& vm, const Identifier& nameInSyntaxTree)
{
    if (m_nameIsDeferred) [[unlikely]]
        materializeDeferredNameSlow();
    const Identifier& name = nameFromSyntaxTree(vm, nameInSyntaxTree);
    ASSERT(!m_hasName || name == m_ecmaName);
    m_ecmaName = name;
}

} // namespace JSC

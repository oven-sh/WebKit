/*
 * Copyright (C) 2017 Yusuke Suzuki <utatane.tea@gmail.com>.
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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
#include "ModuleNamespaceAccessCase.h"

#if ENABLE(JIT)

#include "CCallHelpers.h"
#include "InlineCacheCompiler.h"
#include "JSModuleEnvironment.h"
#include "JSModuleNamespaceObject.h"
#include "ModuleNamespaceExportLayout.h"
#include "PropertyInlineCache.h"

namespace JSC {

ModuleNamespaceAccessCase::ModuleNamespaceAccessCase(VM& vm, JSCell* owner, CacheableIdentifier identifier, JSModuleNamespaceObject* moduleNamespaceObject, JSModuleEnvironment* moduleEnvironment, ScopeOffset scopeOffset)
    : Base(vm, owner, AccessType::ModuleNamespaceLoad, identifier, invalidOffset, nullptr, ObjectPropertyConditionSet(), nullptr)
    , m_scopeOffset(scopeOffset)
{
    m_moduleNamespaceObject.set(vm, owner, moduleNamespaceObject);
    m_moduleEnvironment.set(vm, owner, moduleEnvironment);
}

ModuleNamespaceAccessCase::ModuleNamespaceAccessCase(VM& vm, JSCell* owner, CacheableIdentifier identifier, ModuleNamespaceExportLayout* exportLayout, unsigned exportIndex)
    : Base(vm, owner, AccessType::ModuleNamespaceLoad, identifier, invalidOffset, nullptr, ObjectPropertyConditionSet(), nullptr)
    , m_exportIndex(exportIndex)
{
    ASSERT(exportLayout->names()[exportIndex] == identifier.uid());
    m_exportLayout.set(vm, owner, exportLayout);
}

Ref<AccessCase> ModuleNamespaceAccessCase::create(VM& vm, JSCell* owner, CacheableIdentifier identifier, JSModuleNamespaceObject* moduleNamespaceObject, JSModuleEnvironment* moduleEnvironment, ScopeOffset scopeOffset)
{
    return adoptRef(*new ModuleNamespaceAccessCase(vm, owner, identifier, moduleNamespaceObject, moduleEnvironment, scopeOffset));
}

Ref<AccessCase> ModuleNamespaceAccessCase::createForExportLayout(VM& vm, JSCell* owner, CacheableIdentifier identifier, ModuleNamespaceExportLayout* exportLayout, unsigned exportIndex)
{
    return adoptRef(*new ModuleNamespaceAccessCase(vm, owner, identifier, exportLayout, exportIndex));
}

void ModuleNamespaceAccessCase::dumpImpl(PrintStream& out, CommaPrinter& comma, Indenter&) const
{
    if (m_exportLayout)
        out.print(comma, "exportLayout = "_s, RawPointer(m_exportLayout.get()), comma, "exportIndex = "_s, m_exportIndex);
    else
        out.print(comma, "moduleNamespaceObject = "_s, RawPointer(m_moduleNamespaceObject.get()), comma, "moduleEnvironment = "_s, RawPointer(m_moduleEnvironment.get()), comma, "scopeOffset = "_s, m_scopeOffset);
}

} // namespace JSC

#endif // ENABLE(JIT)

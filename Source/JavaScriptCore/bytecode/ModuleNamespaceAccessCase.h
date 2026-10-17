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

#pragma once

#if ENABLE(JIT)

#include "AccessCase.h"

namespace JSC {

class JSModuleEnvironment;
class JSModuleNamespaceObject;
class ModuleNamespaceExportLayout;

// A load of an exported name from a module namespace object. There are two kinds.
// For one namespace object: the case is for that object only and knows the variable the name is bound to, which is
// what lets the DFG turn the load into a closure variable load, or a constant.
// By export layout: the case is for every namespace object of one ModuleNamespaceExportLayout and reads the variable's
// address from the object's export slot at the name's position. An inline cache goes by layout from the second
// namespace object it sees (tryCacheGetBy). The inline caches of code that several module loaders share see one
// namespace object for each loader.
class ModuleNamespaceAccessCase final : public AccessCase {
public:
    using Base = AccessCase;
    friend class AccessCase;
    friend class InlineCacheCompiler;

    // Null in a case that goes by export layout.
    JSModuleNamespaceObject* moduleNamespaceObject() const LIFETIME_BOUND { return m_moduleNamespaceObject.get(); }
    JSModuleEnvironment* moduleEnvironment() const LIFETIME_BOUND { return m_moduleEnvironment.get(); }
    ScopeOffset scopeOffset() const { return m_scopeOffset; }

    // Null in a case for one namespace object.
    ModuleNamespaceExportLayout* exportLayout() const LIFETIME_BOUND { return m_exportLayout.get(); }
    unsigned exportIndex() const { return m_exportIndex; }

    static Ref<AccessCase> create(VM&, JSCell* owner, CacheableIdentifier, JSModuleNamespaceObject*, JSModuleEnvironment*, ScopeOffset);
    static Ref<AccessCase> createForExportLayout(VM&, JSCell* owner, CacheableIdentifier, ModuleNamespaceExportLayout*, unsigned exportIndex);

private:
    ModuleNamespaceAccessCase(VM&, JSCell* owner, CacheableIdentifier, JSModuleNamespaceObject*, JSModuleEnvironment*, ScopeOffset);
    ModuleNamespaceAccessCase(VM&, JSCell* owner, CacheableIdentifier, ModuleNamespaceExportLayout*, unsigned exportIndex);

    void dumpImpl(PrintStream&, CommaPrinter&, Indenter&) const;

    WriteBarrier<JSModuleNamespaceObject> m_moduleNamespaceObject;
    WriteBarrier<JSModuleEnvironment> m_moduleEnvironment;
    WriteBarrier<ModuleNamespaceExportLayout> m_exportLayout;
    ScopeOffset m_scopeOffset;
    unsigned m_exportIndex { 0 };
};

} // namespace JSC

#endif // ENABLE(JIT)

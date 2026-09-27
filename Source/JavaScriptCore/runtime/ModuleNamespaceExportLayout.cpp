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
#include "ModuleNamespaceExportLayout.h"

#include "JSCInlines.h"
#include "WeakGCSetInlines.h"
#include <wtf/Hasher.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ModuleNamespaceExportLayoutSet);

const ClassInfo ModuleNamespaceExportLayout::s_info = { "ModuleNamespaceExportLayout"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleNamespaceExportLayout) };

ModuleNamespaceExportLayout::ModuleNamespaceExportLayout(VM& vm, Structure* structure, Names&& names, unsigned hash)
    : Base(vm, structure)
    , m_names(WTF::move(names))
    , m_hash(hash)
{
}

void ModuleNamespaceExportLayout::destroy(JSCell* cell)
{
    static_cast<ModuleNamespaceExportLayout*>(cell)->ModuleNamespaceExportLayout::~ModuleNamespaceExportLayout();
}

Structure* ModuleNamespaceExportLayout::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

ModuleNamespaceExportLayout* ModuleNamespaceExportLayout::ensure(VM& vm, Names&& names)
{
    return vm.ensureModuleNamespaceExportLayouts().ensure(vm, WTF::move(names));
}

ModuleNamespaceExportLayoutSet::ModuleNamespaceExportLayoutSet(VM& vm)
    : m_set(vm)
{
}

ModuleNamespaceExportLayoutSet::~ModuleNamespaceExportLayoutSet() = default;

ModuleNamespaceExportLayout* ModuleNamespaceExportLayoutSet::ensure(VM& vm, ModuleNamespaceExportLayout::Names&& names)
{
    struct Key {
        ModuleNamespaceExportLayout::Names& names;
        unsigned hash;
    };
    struct Translator {
        static unsigned hash(const Key& key) { return key.hash; }
        static bool equal(const Weak<ModuleNamespaceExportLayout>& layout, const Key& key)
        {
            return layout && layout->hash() == key.hash && std::ranges::equal(layout->names(), key.names);
        }
    };

    Hasher hasher;
    for (auto& name : names)
        add(hasher, name->existingSymbolAwareHash());
    Key key { names, hasher.hash() };

    // The set is pruned by the collector, which must not run while it is being added to.
    DeferGC deferGC(vm);
    return m_set.ensureValue<Translator>(key, [&] {
        auto* layout = new (NotNull, allocateCell<ModuleNamespaceExportLayout>(vm)) ModuleNamespaceExportLayout(vm, vm.moduleNamespaceExportLayoutStructure.get(), WTF::move(key.names), key.hash);
        layout->finishCreation(vm);
        return Weak<ModuleNamespaceExportLayout>(layout);
    });
}

} // namespace JSC

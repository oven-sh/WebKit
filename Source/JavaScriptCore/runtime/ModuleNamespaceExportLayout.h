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

#pragma once

#include "JSCell.h"
#include "WeakGCSet.h"
#include <wtf/FixedVector.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

// The names a module namespace object exports, in the object's order. The namespace objects of a VM that export the
// same names share one layout, so a name is at the same position in each of them, and an inline cache entry that
// checks the layout and reads JSModuleNamespaceObject's export slot at that position serves all of them
// (ModuleNamespaceAccessCase). Code that several module loaders share reads each loader's own namespace objects
// through the same inline caches, where an entry for one namespace object misses for every loader but one.
// A namespace object gets its layout from the first [[Get]] of an exported name on it after the VM has made such an
// inline cache entry. No namespace object has a layout before that.
class ModuleNamespaceExportLayout final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleNamespaceExportLayoutSpace<mode>();
    }

    DECLARE_INFO;

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    using Names = Vector<RefPtr<UniquedStringImpl>>;

    // The VM's layout for these names in this order.
    static ModuleNamespaceExportLayout* ensure(VM&, Names&&);

    std::span<const RefPtr<UniquedStringImpl>> names() const LIFETIME_BOUND { return m_names.span(); }
    unsigned hash() const { return m_hash; }

private:
    friend class ModuleNamespaceExportLayoutSet;

    ModuleNamespaceExportLayout(VM&, Structure*, Names&&, unsigned hash);

    const FixedVector<RefPtr<UniquedStringImpl>> m_names;
    const unsigned m_hash;
};

// The layouts of a VM that are alive. The VM makes the set for its first layout and keeps it. Until then no inline
// cache reads export slots, and no namespace object fills them in.
class ModuleNamespaceExportLayoutSet {
    WTF_MAKE_TZONE_ALLOCATED(ModuleNamespaceExportLayoutSet);
    WTF_MAKE_NONCOPYABLE(ModuleNamespaceExportLayoutSet);
public:
    explicit ModuleNamespaceExportLayoutSet(VM&);
    ~ModuleNamespaceExportLayoutSet();

    ModuleNamespaceExportLayout* ensure(VM&, ModuleNamespaceExportLayout::Names&&);

private:
    struct Hash {
        static unsigned hash(const Weak<ModuleNamespaceExportLayout>& layout) { return layout ? layout->hash() : 0; }
        static bool equal(const Weak<ModuleNamespaceExportLayout>& a, const Weak<ModuleNamespaceExportLayout>& b) { return a && b && a.get() == b.get(); }
        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };

    WeakGCSet<ModuleNamespaceExportLayout, Hash> m_set;
};

} // namespace JSC

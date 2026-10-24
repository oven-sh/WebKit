/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSScope.h"
#include "VM.h"

namespace JSC { namespace AOT {

class EvalScope final : public JSScope {
public:
    using Base = JSScope;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*);
    static EvalScope* create(VM&, Structure*, JSScope* next, JSObject* bindings, JSValue newTarget);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

private:
    EvalScope(VM&, Structure*, JSScope* next, JSObject* bindings, JSValue newTarget);

    WriteBarrier<JSObject> m_bindings;
    WriteBarrier<Unknown> m_newTarget;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

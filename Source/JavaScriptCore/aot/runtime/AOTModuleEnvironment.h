/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCell.h"

namespace JSC { namespace AOT {

class Modules;

// Stands for a compiled module in a heap snapshot. The variables themselves are roots in the statics.
class ModuleEnvironment final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*);
    static ModuleEnvironment* create(VM&, Structure*, const Modules&, uint32_t indexAmongModulesWithNamedVariables);

    DECLARE_INFO;
    static void analyzeHeap(JSCell*, HeapAnalyzer&);

private:
    ModuleEnvironment(VM&, Structure*, const Modules&, uint32_t index);

    const Modules& m_modules;
    uint32_t m_index;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

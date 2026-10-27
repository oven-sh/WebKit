/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTLayoutData.h"
#include "AOTObjectVisiting.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "JSCell.h"
#include "JSScope.h"
#include "VM.h"
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct Instance;

struct EnvironmentShapes {
    WTF_MAKE_NONCOPYABLE(EnvironmentShapes);
    EnvironmentShapes() = default;

    const ProgramTables* tables { nullptr };
    Vector<CompiledStructure*, 2> structuresByPage;

    const EnvironmentData& at(uint16_t page, const JSCell*) const;
    CompiledStructure* structureFor(VM&, Instance&, uint32_t shape);
};

class Environment final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess mode>
    static CompleteSubspace* subspaceFor(VM& vm) { return vm.heap.aotClosureSpace<mode>(); }

    static CompiledStructure* createStructure(VM&, Instance&, const EnvironmentShapes&, uint16_t shapePage);
    static Environment* create(VM&, Structure*, const EnvironmentData&, size_t size);
    static uint64_t headerFor(Structure*, uint32_t shape);

    const EnvironmentData& layout() const;

    template<typename T> T& at(uint32_t offset) const { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(const_cast<Environment*>(this)) + offset); }

    template<typename Visitor>
    ALWAYS_INLINE static void visitFields(Environment* environment, Visitor& visitor)
    {
        visitReferences(environment, environment->layout(), false, visitor);
    }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    static size_t estimatedSize(JSCell*, VM&);
    static void analyzeHeap(JSCell*, HeapAnalyzer&);

private:
    Environment(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};
static_assert(sizeof(Environment) == offsetOfFirstEnvironmentVariable);

ALWAYS_INLINE const EnvironmentData& EnvironmentShapes::at(uint16_t page, const JSCell* cell) const
{
    auto* header = reinterpret_cast<const uint8_t*>(cell);
    return tables->environmentForShape((static_cast<uint32_t>(page) << environmentShapesPerPageShift) + header[offsetOfEnvironmentShapeLowByte] + (header[offsetOfEnvironmentShapeHighByte] << 8));
}

ALWAYS_INLINE const EnvironmentData& Environment::layout() const
{
    const CompiledStructure& compiled = *uncheckedDowncast<CompiledStructure>(structure());
    return compiled.environmentShapes().at(compiled.environmentShapePage(), this);
}

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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

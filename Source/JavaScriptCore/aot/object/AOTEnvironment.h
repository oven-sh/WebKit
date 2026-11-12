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
#include <wtf/Atomics.h>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct Instance;

struct EnvironmentShapes {
    WTF_MAKE_NONCOPYABLE(EnvironmentShapes);
    EnvironmentShapes() = default;

    const ProgramTables* tables { nullptr };
    Vector<CompiledStructure*, 2> structuresByPage;

    const EnvironmentData& at(uint16_t page, uint64_t header) const;
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

    bool isEscaped() const { return at<uint8_t>(offsetOfEnvironmentShapeHighByte) & environmentIsEscapedBit; }
    void markEscaped() { at<uint8_t>(offsetOfEnvironmentShapeHighByte) |= environmentIsEscapedBit; }

    template<typename Visitor>
    ALWAYS_INLINE static void visitFields(Environment* environment, const EnvironmentData& layout, Visitor& visitor)
    {
        visitReferences(environment, layout, false, visitor);
    }
    template<typename Visitor>
    ALWAYS_INLINE static bool tryVisitFields(Environment* environment, const CompiledStructure& structure, Visitor& visitor)
    {
        uint64_t header = environment->header();
        if (static_cast<uint32_t>(header) != structure.id().bits())
            return false;
        visitFields(environment, structure.environmentShapes().at(structure.environmentShapePage(), header), visitor);
        return true;
    }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    static size_t estimatedSize(JSCell*, VM&);
    static void analyzeHeap(JSCell*, HeapAnalyzer&);

private:
    uint64_t header() const { return WTF::atomicLoad<MemoryOrder::Relaxed>(&at<uint64_t>(0)); }

    Environment(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};
static_assert(sizeof(Environment) == offsetOfFirstEnvironmentVariable);

ALWAYS_INLINE const EnvironmentData& EnvironmentShapes::at(uint16_t page, uint64_t header) const
{
    auto byteOfHeader = [&](uint32_t offset) ALWAYS_INLINE_LAMBDA {
        return static_cast<uint8_t>(header >> 8 * offset);
    };
    return tables->environmentForShape((static_cast<uint32_t>(page) << environmentShapesPerPageShift) + environmentShapeInPage(byteOfHeader(offsetOfEnvironmentShapeLowByte), byteOfHeader(offsetOfEnvironmentShapeHighByte)));
}

ALWAYS_INLINE const EnvironmentData& Environment::layout() const
{
    const CompiledStructure& compiled = *uncheckedDowncast<CompiledStructure>(structure());
    return compiled.environmentShapes().at(compiled.environmentShapePage(), header());
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

    JSObject* bindings() const { return m_bindings.get(); }

private:
    EvalScope(VM&, Structure*, JSScope* next, JSObject* bindings, JSValue newTarget);

    WriteBarrier<JSObject> m_bindings;
    WriteBarrier<Unknown> m_newTarget;
};

enum class NameInEvalScope : uint8_t { NotEvalScope, Absent, Present };
NameInEvalScope findNameInEvalScope(JSGlobalObject*, JSScope*, PropertyName);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

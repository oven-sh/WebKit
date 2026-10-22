/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnvironment.h"

#if ENABLE(AOT)

#include "AOTImmortalCells.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTProgramIdentifiers.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "HeapAnalyzer.h"
#include "HeapCellInlines.h"
#include "JSCInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo Environment::s_info = { "Environment"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(Environment) };

static_assert(offsetOfEnvironmentShapeLowByte == JSCell::indexingTypeAndMiscOffset() && offsetOfEnvironmentShapeHighByte == JSCell::typeInfoFlagsOffset());

CompiledStructure* EnvironmentShapes::structureFor(VM& vm, Instance& instance, uint32_t shape)
{
    uint16_t page = shape >> environmentShapesPerPageShift;
    while (structuresByPage.size() <= page)
        structuresByPage.append(nullptr);
    if (!structuresByPage[page]) {
        CompiledStructure* created = Environment::createStructure(vm, instance, *this, page);
        structuresByPage[page] = created;
        instance.state->immortalCells->add(created);
    }
    return structuresByPage[page];
}

CompiledStructure* Environment::createStructure(VM& vm, Instance& instance, const EnvironmentShapes& shapes, uint16_t shapePage)
{
    return CompiledStructure::createForEnvironment(vm, instance.globalObject, TypeInfo(CompiledEnvironmentType, StructureFlags), info(), shapes, shapePage);
}

Environment* Environment::create(VM& vm, Structure* structure, const EnvironmentData& layout, size_t size)
{
    ASSERT(size >= layout.size);
    ASSERT(layout.kind != LayoutKind::Closure);
    void* cell = vm.heap.aotClosureSpace<SubspaceAccess::OnMainThread>()->allocate(vm, WTF::roundUpToMultipleOf<atomSize>(size), nullptr, AllocationFailureMode::Assert);
    static_cast<JSCell*>(cell)->clearStructure();
    Environment* environment = new (NotNull, cell) Environment(vm, structure);
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(environment) + sizeof(JSCell), size - sizeof(JSCell) });
    for (auto& variable : layout.describedVariables()) {
        if (traitsOf(variable.slot.representation()).absentBits)
            clearSlot(variable.slot.representation(), &environment->at<uint8_t>(variable.slot.offset));
    }
    environment->at<uint8_t>(offsetOfEnvironmentShapeLowByte) = layout.shape;
    environment->at<uint8_t>(offsetOfEnvironmentShapeHighByte) = layout.shape >> 8;
    ASSERT(environment->at<uint64_t>(0) == headerFor(structure, layout.shape));
    vm.mutatorFence();
    return environment;
}

uint64_t Environment::headerFor(Structure* structure, uint32_t shape)
{
    ASSERT(uncheckedDowncast<CompiledStructure>(structure)->describesEnvironment());
    ASSERT(uncheckedDowncast<CompiledStructure>(structure)->environmentShapePage() == shape >> environmentShapesPerPageShift);
    uint64_t header = cellHeaderFor(structure);
    header &= ~(0xffull << 8 * offsetOfEnvironmentShapeLowByte | 0xffull << 8 * offsetOfEnvironmentShapeHighByte);
    header |= static_cast<uint64_t>(shape & 0xff) << 8 * offsetOfEnvironmentShapeLowByte | static_cast<uint64_t>(shape >> 8 & 0xff) << 8 * offsetOfEnvironmentShapeHighByte;
    return header;
}

template<typename Visitor>
void Environment::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitFields(uncheckedDowncast<Environment>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(Environment);

size_t Environment::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

void Environment::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Environment* environment = uncheckedDowncast<Environment>(cell);
    Base::analyzeHeap(cell, analyzer);
    const ProgramIdentifiers& identifiers = *environment->vm().aotState().identifiers;
    uintptr_t cageBase = cageBaseOf(environment);
    for (auto& variable : environment->layout().describedVariables()) {
        if (!traitsOf(variable.slot.representation()).holdsCell)
            continue;
        JSCell* target = cellInSlot(variable.slot.representation(), &environment->at<uint8_t>(variable.slot.offset), cageBase);
        UniquedStringImpl* name = target ? identifiers.tryGetIdentifierConcurrently(variable.name) : nullptr;
        if (name)
            analyzer.analyzeVariableNameEdge(environment, target, name);
    }
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

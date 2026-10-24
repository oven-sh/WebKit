/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelocationLowering.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "B3ArgumentRegValue.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static_assert(static_cast<uint32_t>(RelocationSpace::B3RuntimeFunction) == B3::runtimeFunctionSpace);

static constexpr int64_t slotSize = sizeof(void*);

static B3::Value* insertImmutableLoad(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, B3::Value* base, int32_t offset)
{
    B3::MemoryValue* load = insertionSet.insert<B3::MemoryValue>(index, B3::Load, B3::pointerType(), origin, base, offset);
    load->setControlDependent(false);
    load->setReadsMutability(B3::Mutability::Immutable);
    return load;
}

static B3::Value* insertAddPtr(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, B3::Value* base, int64_t offset)
{
    if (!offset)
        return base;
    B3::Value* constant = insertionSet.insertIntConstant(index, origin, B3::pointerType(), offset);
    return insertionSet.insert<B3::Value>(index, B3::Add, origin, base, constant);
}

static B3::PatchpointValue* insertAddressPatchpoint(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin)
{
    B3::PatchpointValue* patchpoint = insertionSet.insert<B3::PatchpointValue>(index, B3::pointerType(), origin);
    patchpoint->effects = B3::Effects::none();
    patchpoint->resultConstraints = { B3::ValueRep::SomeRegister };
    patchpoint->setRunsWithoutFrame(true);
    return patchpoint;
}

auto ImageRelocationLowering::instanceRuntimeTable() -> PointerTable
{
    return {
        .space = RelocationSpace::B3RuntimeFunction,
        .base = instanceGPR,
        .offset = static_cast<int32_t>(AOT_INSTANCE_OFFSET(runtimeTable)),
        .offsetHoldsTablePointer = true,
        .runtimeFunctionSlot = nullptr,
    };
}

ImageRelocationLowering::ImageRelocationLowering(PointerTable table, RelocationRecorder& relocations)
    : m_table(table)
    , m_relocations(relocations)
{
    RELEASE_ASSERT(table.space == RelocationSpace::B3RuntimeFunction || table.space == RelocationSpace::WebAssemblyPointers);
}

B3::Value* ImageRelocationLowering::lower(B3::Procedure&, B3::InsertionSet& insertionSet, size_t index, const B3::RelocatableValue& value)
{
    B3::RelocationSymbol symbol = value.symbol();
    B3::Origin origin = value.origin();
    if (symbol.space > static_cast<uint32_t>(RelocationSpace::JumpTables))
        return nullptr;

    RelocationSpace space = static_cast<RelocationSpace>(symbol.space);
    B3::Value* address = nullptr;
    switch (space) {
    case RelocationSpace::B3RuntimeFunction: {
        if (!m_table.runtimeFunctionSlot || symbol.index >= B3::numberOfRuntimeFunctions)
            return nullptr;
        std::optional<uint32_t> slot = m_table.runtimeFunctionSlot(static_cast<B3::RuntimeFunction>(symbol.index));
        if (!slot)
            return nullptr;
        address = loadSlot(insertionSet, index, origin, *slot);
        break;
    }
    case RelocationSpace::WebAssemblyPointers:
        if (space != m_table.space)
            return nullptr;
        address = loadSlot(insertionSet, index, origin, symbol.index);
        break;
    case RelocationSpace::SiteCaches:
        if (m_table.space != RelocationSpace::B3RuntimeFunction)
            return nullptr;
        address = lowerSiteCacheAddress(insertionSet, index, origin, symbol.index);
        break;
    case RelocationSpace::Constants:
        address = lowerDataAddress(insertionSet, index, origin, { SymbolSpace::Constant, symbol.index });
        break;
    case RelocationSpace::JumpTables:
        address = lowerDataAddress(insertionSet, index, origin, { SymbolSpace::JumpTable, symbol.index });
        break;
    }
    if (!address)
        return nullptr;
    return insertAddPtr(insertionSet, index, origin, address, value.addend());
}

B3::Value* ImageRelocationLowering::loadSlot(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, uint32_t slot) const
{
    int64_t offset = slotSize * slot;
    if (!m_table.offsetHoldsTablePointer)
        offset += m_table.offset;
    if (!isRepresentableAs<int32_t>(offset))
        return nullptr;

    B3::Value* table = insertionSet.insert<B3::ArgumentRegValue>(index, origin, m_table.base);
    if (m_table.offsetHoldsTablePointer)
        table = insertImmutableLoad(insertionSet, index, origin, table, m_table.offset);
    return insertImmutableLoad(insertionSet, index, origin, table, static_cast<int32_t>(offset));
}

B3::Value* ImageRelocationLowering::lowerSiteCacheAddress(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, uint32_t siteCache)
{
    B3::PatchpointValue* patchpoint = insertAddressPatchpoint(insertionSet, index, origin);
    patchpoint->setGenerator([this, siteCache](CCallHelpers&, const B3::StackmapGenerationParams& params) {
        m_relocations.moveInstanceAddress({ SymbolSpace::SiteCache, siteCache }, instanceGPR, params[0].gpr());
    });
    return patchpoint;
}

B3::Value* ImageRelocationLowering::lowerDataAddress(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, CodeSymbol symbol)
{
    B3::PatchpointValue* patchpoint = insertAddressPatchpoint(insertionSet, index, origin);
    patchpoint->setGenerator([this, symbol](CCallHelpers&, const B3::StackmapGenerationParams& params) {
        m_relocations.moveAddress(symbol, params[0].gpr(), PCRelativeReach::Far);
    });
    return patchpoint;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

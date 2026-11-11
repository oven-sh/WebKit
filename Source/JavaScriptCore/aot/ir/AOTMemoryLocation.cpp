/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMemoryLocation.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NoLocationBits {
    struct Types {
        std::optional<uint8_t> locationBitOf(PropertyKeyID) const { return std::nullopt; }
    };

    std::optional<uint8_t> locationBitOf(VariableID) const { return std::nullopt; }
    std::optional<uint8_t> locationBitOf(LayoutID) const { return std::nullopt; }

    Types types;
};

} // anonymous namespace

static_assert(LocationMasks().masksAgreeWithHeaps());
static_assert(LocationMasks::top().masksAgreeWithHeaps());
static_assert(LocationMasks::top().without(indexedHeaps).masksAgreeWithHeaps());
static_assert(LocationMasks::top().restrictedTo(bookkeepingHeaps).masksAgreeWithHeaps());
static_assert(LocationMasks::top().intersectedWith(bookkeepingHeaps).masksAgreeWithHeaps());
static_assert([] {
    LocationMasks masks = LocationMasks::top();
    masks.intersect(bookkeepingHeaps);
    return masks.masksAgreeWithHeaps();
}());

uint64_t maskOf(const Program& program, const MemoryLocation& location)
{
    return locationMaskIn(program, location);
}

void LocationMasks::add(const MemoryLocation& location)
{
    add(location.heap(), locationMaskIn(NoLocationBits { }, location));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArgumentsEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTEntryEmitter.h"
#include "AOTMachineFrame.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallLists.h"
#include "AOTTypeArena.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "Interpreter.h"
#include <iterator>

namespace JSC { namespace AOT { namespace Typed {

Word32 ArgumentsEmitter::restLength(uint32_t firstArgument)
{
    Word32 count = emitter<EntryEmitter>().argumentCount();
    if (!firstArgument)
        return count;
    Word32 skipped = word32(static_cast<int32_t>(firstArgument));
    Condition hasRest = below(skipped, count);
    return select(hasRest, sub(count, skipped), 0);
}

Pointer ArgumentsEmitter::restList(uint32_t firstArgument)
{
    EntryEmitter& entry = emitter<EntryEmitter>();
    RELEASE_ASSERT(!entry.indexOfFirstArgumentInList());
    return offsetBy(entry.argumentList(), static_cast<ptrdiff_t>(firstArgument) * sizeOfStackWord);
}

void ArgumentsEmitter::generateGetArgument(const ValueContext& value)
{
    uint32_t index = value->argumentIndex();
    Pointer list = restList(0);
    auto load = [&] {
        return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, list.value(), static_cast<ptrdiff_t>(index) * sizeOfStackWord)));
    };
    if (index < emitter<EntryEmitter>().numberOfArgumentsAlwaysInList()) {
        define(value, load());
        return;
    }
    Word32 position = word32(static_cast<int32_t>(index));
    define(value, either<Boxed>(below(position, emitter<EntryEmitter>().argumentCount()), load, [&] {
        return undefined();
    }));
}

void ArgumentsEmitter::generateGetRestLength(const ValueContext& value)
{
    define(value, restLength(value->firstArgument()));
}

void ArgumentsEmitter::generateCreateRest(const ValueContext& value)
{
    uint32_t firstArgument = value->firstArgument();
    CellAllocationEmitter& cells = emitter<CellAllocationEmitter>();
    switch (value->storage()) {
    case AllocationStorage::Heap:
        define(value, cells.newArrayFromBuffer(value, restLength(firstArgument), restList(firstArgument)));
        return;
    case AllocationStorage::Frame:
        define(value, cells.newArrayInFrameFromBuffer(value, restLength(firstArgument), restList(firstArgument), maxLengthOfArrayInFrame));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

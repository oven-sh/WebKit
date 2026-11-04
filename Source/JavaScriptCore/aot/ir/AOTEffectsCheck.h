/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"

namespace JSC { namespace AOT { namespace Typed {

// Known trusts the table of effects, so the table is checked. Whoever asks these does so in checking builds and under --aot-validate alone.
// A no is InternalErrorKind::StoreOutsideDeclaredEffects, with the function and the operation.

enum class MemoryAccess : uint8_t { Load, Store };

// Every load and store that an emitter appends carries a heap of the machine, which lies below the heap of the schema that owns it, or has HeapTrait::IsOutsideDeclaredEffects for words that no operation
// of the program can see. The emitted code is checked in those terms: CodeGenerator::declareAccessesOf() turns effectsOf() the operation being emitted into heaps of the machine (AOTHeaps::appendHeaps()),
// and Machine::Builder::setDeclaredAccesses() holds each load and store to them. This is the same question for whoever has a heap of the schema in hand.
constexpr bool isWithinEffects(const Effects& ofOperationBeingEmitted, AbstractHeap heap, MemoryAccess access)
{
    // An operation that writes a heap may read it back.
    if (ofOperationBeingEmitted.writes.contains(heap))
        return true;
    return access == MemoryAccess::Load && ofOperationBeingEmitted.reads.contains(heap);
}

// A call of a stub or of the run time out of the code of an operation: what the callee declares lies within what the operation declares.
// And in the walk: what a handler built in place of one operation of the input, merged, lies within the effects of that operation with the operands it had.
constexpr bool isWithinEffects(const Effects& allowed, const Effects& found) { return found.isSubsetOf(allowed); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

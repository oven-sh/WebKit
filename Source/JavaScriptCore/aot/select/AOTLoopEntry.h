/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTNaturalLoops.h"
#include "AOTSelectionContext.h"
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

struct InvariantOperand {
    BlockID header;
    ValueID value;
};

std::optional<InvariantOperand> findOutermostInvariantLoop(SelectionContext&, BlockID user, ValueID operand);
std::optional<InvariantOperand> findOutermostInvariantLoop(SelectionContext&, BlockID user, ValueID operand, NOESCAPE const ScopedLambda<bool(ValueID)>& isUsable);

enum class LoopEntryResult : uint8_t { Unknown, NoSingleEntry, HandlerDiffers, Emitted };

using LoopEntryEmitter = ScopedLambda<void(IRBuilder&)>;

bool canEmitOnLoopEntry(SelectionContext&, BlockID header);
// Whether what is emitted on the entry stands in a new block under a handler that the block in front of the loop does not have.
bool entersLoopUnderNewHandlerEdge(SelectionContext&, BlockID header);
LoopEntryResult emitOnLoopEntry(SelectionContext&, IRBuilder&, BlockID header, NOESCAPE const LoopEntryEmitter&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

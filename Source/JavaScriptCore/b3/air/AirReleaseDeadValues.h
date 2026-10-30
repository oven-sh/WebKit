/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 { namespace Air {

class Code;
struct StackColoring;

// A patchpoint that says StackmapValue::releasesDeadValues() names one value: what a variable held until the program set it to null. A callee-saved register, a spill slot
// or an outgoing argument word keeps what was last put there, and a conservative collector reads all three. This follows the moves of that value backward from the patchpoint
// and clears the places that still hold it, are read no more, and would stay as they are until something may collect. Nothing else is cleared.
// A call that says StackmapValue::childrenLetGoByCallee() names the arguments whose parameters the callee sets to null. The store in the callee does not reach the frame
// of the caller, so the same is done for each of them in front of the call.
// It runs behind the allocation of stack slots and in front of the placement of the frame. A patchpoint with no register to clear goes.
JS_EXPORT_PRIVATE void releaseDeadValues(Code&, const StackColoring&);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

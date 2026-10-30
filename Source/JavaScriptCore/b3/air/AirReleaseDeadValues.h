/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 { namespace Air {

class Code;
struct StackColoring;

// Where a patchpoint says StackmapValue::releasesDeadValues(): a callee-saved register, a spill slot or an outgoing argument word keeps what was last put there,
// and a conservative collector reads all three. This clears those that this function has written a whole word to, reads no more, and would leave as they are
// until something may collect. It runs behind the allocation of stack slots and in front of the placement of the frame. A patchpoint with nothing to clear goes.
JS_EXPORT_PRIVATE void releaseDeadValues(Code&, const StackColoring&);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

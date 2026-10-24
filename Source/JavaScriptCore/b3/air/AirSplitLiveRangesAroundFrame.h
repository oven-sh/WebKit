/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 { namespace Air {

class Code;

// The register allocator gives a Tmp one home. What lives across the calls of a function's heavy part, or through a block that holds
// more values than the registers a function may write unsaved, takes a callee-save or a spill slot, and with one Tmp it takes it in the entry block too, so the frame
// covers the paths that need none. This gives each such Tmp a second Tmp inside the blocks that need the frame, with Shuffles on the border.
JS_EXPORT_PRIVATE bool splitLiveRangesAroundFrame(Code&);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

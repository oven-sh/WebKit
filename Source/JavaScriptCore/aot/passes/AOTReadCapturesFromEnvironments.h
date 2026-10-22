/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class ProgramEditor;

// A function declared inside another function is kept in a field of the environment, and a direct call reads the closure from there only to get at what it captures.
// Where every read of such a field ends in GetCapture, the captures are read from the environment itself: it is one of them, and the others get fields of their own.
// Nothing reads the field any more, so RemoveUnreadFields takes it away with its store, and the closure is never made.
// From here on a function hands its environment on where it used to take one out of a closure, so what was known about the bodies before no longer holds:
// the pass simplifies nothing itself and leaves that to the passes behind it.
void readCapturesFromEnvironments(ProgramEditor&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

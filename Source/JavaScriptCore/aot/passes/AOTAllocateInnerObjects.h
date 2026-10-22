/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class FlowResults;
class Heat;
class ProgramEditor;

// `{ b: 1, c: { d: 2, e: 3 } }` makes two objects.
void allocateInnerObjects(ProgramEditor&, const FlowResults&, const CallGraph&, const Heat&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class EffectSummaries;
class FlowResults;
class ProgramEditor;

// A function declared inside another function is kept in a field of the environment, and a direct call reads the closure from there only to get at what it captures.
// The summaries are those that the bodies were optimized with.
void readCapturesFromEnvironments(ProgramEditor&, FlowResults&, const EffectSummaries*, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

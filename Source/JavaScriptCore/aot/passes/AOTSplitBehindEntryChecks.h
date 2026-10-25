/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Heat;
class ProgramEditor;

// Without heat every function that can be split is.
void splitBehindEntryChecks(ProgramEditor&, const Heat*, BuildContext&);

// A caller whose own types answer some of the tests of an entrance enters a shorter one, which holds the others.
void skipAnsweredEntryChecks(ProgramEditor&, const Heat&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

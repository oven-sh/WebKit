/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ProgramEditor;

// A function that makes a string and returns it does not see what becomes of it.
void markResultsUsedAsKeys(ProgramEditor&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

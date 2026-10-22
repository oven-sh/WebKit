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

// A function that makes a string and returns it does not see what becomes of it. Where more of the direct calls that read the result look something up by it, in a Map, a Set or an object,
// than read it in another way, the string is hashed at once: the function makes it as a key, flat and shared with equal keys, and no rope that is flattened a moment later.
void markResultsUsedAsKeys(ProgramEditor&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

inline void checkPrivateNameProgram(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    checkOutputOfProgramInShell(programText, expectedOutput, { "references=wide"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "references=narrow"_s });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

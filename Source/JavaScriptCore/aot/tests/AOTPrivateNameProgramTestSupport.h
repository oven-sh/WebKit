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
    static constexpr ASCIILiteral words[] = {
        "validate"_s,
        "check=VerifyEachPass"_s,
        "heat=EverythingIsHot"_s,
        "heat=EverythingIsCold"_s,
        "disable=pass:InlineCalls"_s,
        "disable=pass:ResolveCalls"_s,
        "disable=pass:ResolveAccesses"_s,
        "disable=flow-results"_s,
        "references=narrow"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { });
    for (ASCIILiteral word : words)
        checkOutputOfProgramInShell(programText, expectedOutput, { word });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

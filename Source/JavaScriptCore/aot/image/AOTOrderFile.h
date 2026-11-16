/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeLayout.h"
#include "AOTLinkedFunctions.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

class BuildContext;
class FunctionNumbers;

namespace Typed {
class Plan;
class Program;
}

Vector<FinalJump> orderCodeByFiles(const Typed::Program&, const Typed::Plan&, const FunctionNumbers&, LinkedFunctions&, ResolvedCallees&, BuildContext&);

struct RegExpOfOrderFile {
    Vector<char16_t> pattern;
    uint16_t flags;
};
Vector<RegExpOfOrderFile> regExpsOfOrderFiles(std::span<const uint8_t>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

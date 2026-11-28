/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <initializer_list>
#include <source_location>
#include <wtf/OptionSet.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

OptionSet<ImageFlag> imageFlagsOf(ASCIILiteral programText, std::initializer_list<ASCIILiteral> optionWords = { });
Vector<Wire::WorkerStartRecord> workerStartsWithoutNameOf(ASCIILiteral programText, std::initializer_list<ASCIILiteral> optionWords = { });

struct ExpectedImageFlags {
    ASCIILiteral programText;
    OptionSet<ImageFlag> flags;
    std::initializer_list<ASCIILiteral> optionWords { };
    std::source_location location = std::source_location::current();
};

void checkImageFlags(std::initializer_list<ExpectedImageFlags>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

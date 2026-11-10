/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTImageForward.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class CodeIndex;
class ImageNumbers;
class LinkedFunctions;
struct CodeLayout;
struct ProgramTables;

namespace Typed {
class Plan;
class Program;
}

struct SharedCode;

struct FrameTable {
    Vector<ImageFrame> frames;
    Vector<uint32_t> frameOf;
};

std::optional<FrameTable> collectFrames(const LinkedFunctions&, BuildContext&);

struct FunctionTableInputs {
    const LinkedFunctions& functions;
    const ImageNumbers& numbers;
    const CodeLayout& layout;
    const SharedCode& sharedCode;
    const CodeIndex& index;
    std::span<const uint32_t> sideTableOffsets;
    const ProgramTables& programTables;
    bool hasPositions;
    const Typed::Program& program;
    const Typed::Plan& plan;
};

struct EncodedFunctionTables {
    Vector<uint8_t> functions;
    Vector<uint8_t> functionStarts;
    Vector<uint8_t> codeGranules;
    Vector<uint8_t> closureWords;
    Vector<uint8_t> methodTable;
};

EncodedFunctionTables encodeFunctionTables(const FunctionTableInputs&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

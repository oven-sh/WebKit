/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <wtf/text/WTFString.h>

namespace JSC {

namespace B3 {
class PCToOriginMap;
}

namespace AOT {

class ImageLinkBuffer;
struct RecordedOptions;

namespace Typed {

class Program;
struct CodeGenerationInputs;
struct Function;

String describeCodeSize(const Program&, const Function&, const B3::PCToOriginMap&, ImageLinkBuffer&);
String describeBranches(const CodeGenerationInputs&, FunctionID, const Function&, const RecordedOptions&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

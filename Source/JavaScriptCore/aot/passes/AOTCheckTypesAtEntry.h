/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;
class ProgramEditor;
struct Function;

void checkTypesAtEntry(ProgramEditor&, BuildContext&);

// For the reader, which has each body at hand once: it decodes a use of a checked parameter as a use of the check, so that no walk over the body follows.
Vector<unsigned, 8> findParametersCheckedAtEntry(const Program&, const BuildContext&, FunctionID, const Function& record, bool hasBody);
// The check of checkedParameters[position] gets the number of values that the function has now, plus the position. Every use names it already.
void appendChecksAtEntry(const Program&, BuildContext&, Function&, std::span<const unsigned> checkedParameters);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

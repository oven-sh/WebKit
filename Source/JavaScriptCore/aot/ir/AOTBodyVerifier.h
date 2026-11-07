/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
struct InternalError;

namespace Typed {

class BodyIndexTable;
class Program;
struct Function;

Vector<InternalError> findErrorsInWords(FunctionID, const Body&);

bool verifyBodies(const Program&, BuildContext&);
bool verifyBodies(const Program&, BuildContext&, BodyIndexTable*);

extern void (*printerOfBodiesForVerifier)(const Program&, FunctionID, ASCIILiteral heading);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

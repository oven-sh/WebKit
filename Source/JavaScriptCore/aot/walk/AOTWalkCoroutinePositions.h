/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

class CallingConventions;
class ObjectTable;
class TypeArena;
struct LinkTables;

TypeID typeHeldOfResultOfCoroutine(const Walker&, FunctionID callee);
TypeID typeOfPromiseExpectedToGive(Walker&, TypeID settledWith);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

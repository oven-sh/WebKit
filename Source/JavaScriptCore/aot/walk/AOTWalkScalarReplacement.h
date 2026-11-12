/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

enum class FateOfValue : uint8_t { Escapes, Returned, OnlyTakenApart };

FateOfValue fateOfCallResult(const Walker&, FunctionID, const BodyIndex&, ValueID call);
FateOfValue fateOfParameter(const Walker&, FunctionID, unsigned entryParameter);
bool returnsNewObject(const Walker&, FunctionID);
bool returnsUnsharedList(const Walker&, FunctionID);
bool isRemovableInLoops(const Walker&, FunctionID callee);
bool armsRemoveObjects(const Walker&, std::span<const FunctionID> methods, const BodyIndex&, ValueID call);
bool holdsNumbersAlone(const Walker&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

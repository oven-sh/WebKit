/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"

namespace JSC { namespace AOT { namespace Typed {

class Facts;

ValueID callThatBorrowsClosure(const Code&, ValueID closure);
ValueID callThatBorrowsObject(const Code&, ValueID object);
bool lentClosureMayStandInHeap(const Code&, ValueID closure);
uint8_t argumentsLentBy(const Code&, const Operation& call);
bool mayCaptureClosuresOfFrames(const Facts&, const Code&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

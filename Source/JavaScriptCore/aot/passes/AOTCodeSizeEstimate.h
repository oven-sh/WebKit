/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct Block;
struct Function;
struct Terminator;
struct Value;

unsigned estimatedCodeBytes(const Program&, const Function&, const Value&);
unsigned estimatedCodeBytes(const Terminator&);
unsigned estimatedDirectCallBytes(unsigned numberOfOperands);
unsigned estimatedCodeBytes(const Program&, const Function&, const Block&);

// `{ ...first, ...second }`: the CopyDataProperties right behind a CopyObject that copies all that its source holds into the copy of all that the first holds.
// The two get their code together: one operation is asked for the copy of both first. So they stay in one function, one behind the other.
ValueID findCopyIntoCopy(const Function&, const Block&, ValueID copyObject);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

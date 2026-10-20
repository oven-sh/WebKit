/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct Block;
struct Function;
struct Terminator;
struct Value;

unsigned estimatedCodeBytes(const Program&, const Function&, const Value&);
unsigned estimatedCodeBytes(const Terminator&);
unsigned estimatedCodeBytes(const Program&, const Function&, const Block&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

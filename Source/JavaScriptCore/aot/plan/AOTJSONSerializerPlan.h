/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

struct BuiltinParameter;

namespace Typed {

class Facts;
class Program;
struct CallingConventionParts;

// The words of a row's parameter are those of a value that is serialized and of nothing else: its own keys, its own values, toJSON and the conversion of boxed primitives, on all that it reaches, and it is only read.
bool isSerializedAsJSONStringifyDoes(const BuiltinParameter&);

// computeCallingConventions(), at its end: CallingConventionParts::functionsOfSerializers. CodeRole::Serializer.
// Nothing where no built-in that reached code comes by serializes, or where the walk writes no text: BuiltinCalls or TypedOperations withheld. Then no unit has a place, and every other unit has the number that it had.
// Else each layout whose objects a test of the shape finds and that reached code allocates is listed under the body of a module: the one that declares the layout, where that has code, else that of the first entry.
// A place is no code: a serializer gets code where finished code mentions it. Whether objects of the layout can be written at all is the walk's question: planJSONObject().
void planSerializers(CallingConventionParts&, const Program&, const Facts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

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

bool isSerializedAsJSONStringifyDoes(const BuiltinParameter&);

void planSerializers(CallingConventionParts&, const Program&, const Facts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

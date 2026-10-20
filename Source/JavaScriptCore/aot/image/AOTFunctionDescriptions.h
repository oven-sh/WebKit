/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

class Heat;
class Program;

Vector<String> describeFunctions(const Program&, const Heat&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

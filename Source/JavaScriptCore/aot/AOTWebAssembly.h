/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC {

class VM;

namespace AOT {

class ImageBuilder;

namespace Typed {
class Program;
}

void addWebAssemblyModules(VM&, const Typed::Program&, ImageBuilder&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

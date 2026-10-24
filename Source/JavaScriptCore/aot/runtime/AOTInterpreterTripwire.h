/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC {

class CodeBlock;

namespace AOT {

void willRunInInterpreter(CodeBlock*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

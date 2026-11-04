/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <memory>
#include <wtf/Forward.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

// Text to the bytes of the wire, which a program adopts and a Reader (ir/AOTReaderInternal.h) then reads into it. The program owns the bytes, so its bodies live as long as it does.
// Null where the text or the program is wrong: the build has the error.
std::unique_ptr<Program> parseProgram(StringView text, BuildContext&);

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

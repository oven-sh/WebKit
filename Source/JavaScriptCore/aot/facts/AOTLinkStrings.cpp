/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool keyThatBeginsSoMayNamePrototypeLink(const Program& program, const SummaryAccess& access)
{
    StringID beginningOfKey = access.beginningOfKey();
    if (!beginningOfKey)
        return true;
    std::span<const uint8_t> beginning = program.string(beginningOfKey);
    std::span<const uint8_t> nameOfLink = program.string(program.stringOf(BuiltinNames::__proto__));
    return beginning.size() <= nameOfLink.size() && std::ranges::equal(beginning, nameOfLink.first(beginning.size()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

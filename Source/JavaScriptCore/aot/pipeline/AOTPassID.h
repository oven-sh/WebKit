/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using PassID = Wire::PassID;

inline constexpr unsigned numberOfPasses = numberOfEnumerators<PassID>;
static_assert(numberOfPasses <= 256);
static_assert(PassID { } == PassID::None);

inline std::optional<PassID> passNamed(StringView name)
{
    std::optional<PassID> pass = parseEnum<PassID>(name);
    if (pass == PassID::None)
        return std::nullopt;
    return pass;
}

JS_EXPORT_PRIVATE bool isOptionalPass(PassID);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

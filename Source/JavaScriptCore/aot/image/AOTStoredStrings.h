/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

enum class VerdictOnStoredStrings : uint8_t {
    NothingLiesBesideStrings,
    BytesBesideStringsAreNotZero,
    CannotBeRead,
};

enum class StoredStringsForm : uint8_t { ZeroTerminated, JSONLines };

struct StoredStrings {
    VerdictOnStoredStrings verdict { VerdictOnStoredStrings::CannotBeRead };
    Vector<uint8_t> strings;
    Vector<uint8_t> report;
};

JS_EXPORT_PRIVATE StoredStrings readStoredStrings(std::span<const uint8_t> file, StoredStringsForm = StoredStringsForm::ZeroTerminated);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

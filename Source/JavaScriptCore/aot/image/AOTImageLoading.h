/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AOTWireEnums.h"
#include "JSExportMacros.h"
#include <array>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class Image;

struct ImageFile {
    intptr_t descriptor;
    int64_t offset;
};

JS_EXPORT_PRIVATE Wire::ImageVerdict useImage(std::span<const uint8_t> image, std::optional<ImageFile>, const std::array<uint8_t, 16>& identity);

bool mapPrivateCloneOfImagePages(const Image&, std::span<const uint8_t> pagesOfImage, void* target);
#if OS(LINUX) || OS(FREEBSD)
// The file that the image was loaded from, by its device and inode. The caller closes it. Less than 0: the image does not lie in the executable, or the file cannot be opened, or it is another file by now.
int openExecutableThatHolds(const Image&);
#endif

} } // namespace JSC::AOT

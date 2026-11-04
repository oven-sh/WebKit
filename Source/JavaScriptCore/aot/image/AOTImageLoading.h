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

// The image becomes the one of the calling thread: a VM that the thread creates from here on runs its program.
JS_EXPORT_PRIVATE Wire::ImageVerdict useImage(std::span<const uint8_t> image, std::optional<ImageFile>, const std::array<uint8_t, 16>& identity);

// Puts a copy-on-write clone of whole pages of the image at the target, read-only, in the place of what is mapped there. False: nothing is done, and the caller copies.
bool mapPrivateCloneOfImagePages(const Image&, std::span<const uint8_t> pagesOfImage, void* target);

} } // namespace JSC::AOT

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

struct NamedImageFlag {
    ImageFlag flag;
    ASCIILiteral name;
};

#define AOT_NAMED_IMAGE_FLAG(name) { ImageFlag::name, #name ""_s }

inline constexpr NamedImageFlag namedImageFlags[] {
    AOT_NAMED_IMAGE_FLAG(IsObfuscated),
    AOT_NAMED_IMAGE_FLAG(AdmitsUnknownJavaScript),
    AOT_NAMED_IMAGE_FLAG(LoadsModulesByName),
    AOT_NAMED_IMAGE_FLAG(AllowsLibrarySource),
    AOT_NAMED_IMAGE_FLAG(ShowsWaysBetweenModuleObjects),
    AOT_NAMED_IMAGE_FLAG(LoadsCodeOutsideImage),
    AOT_NAMED_IMAGE_FLAG(IsBuiltWithLibrary),
    AOT_NAMED_IMAGE_FLAG(RunsEntryPointsAgain),
    AOT_NAMED_IMAGE_FLAG(LetsGlobalObjectGo),
    AOT_NAMED_IMAGE_FLAG(SetsMainModulePath),
    AOT_NAMED_IMAGE_FLAG(RefusesOriginalsBesideMocks),
};

#undef AOT_NAMED_IMAGE_FLAG

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

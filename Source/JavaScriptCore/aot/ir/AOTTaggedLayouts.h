/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

struct TaggedLayout {
    int64_t tag { 0 };
    LayoutID layout;
    Vector<LayoutID, 2> declaringTypes;
};

struct TaggedFamily {
    LayoutID maker;
    PropertyKeyID tagKey;
    Vector<TaggedLayout> members;
};

void appendTaggedLayouts(Program&, std::span<const TaggedFamily>);
void appendFieldsOfTaggedLayouts(Program&, std::span<const TaggedFamily>);
void allocateTaggedLayouts(Program&, BuildContext&, std::span<const TaggedFamily>);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

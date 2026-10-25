/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <span>
#include <wtf/Variant.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

using LayoutTag = Variant<int64_t, StringID>;

struct TaggedLayout {
    Vector<LayoutTag, 1> tags;
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

void allocateTaggedLayouts(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

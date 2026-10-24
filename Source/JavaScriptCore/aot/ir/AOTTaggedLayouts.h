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

// The literal that the declaring types write for the tag: an integer or one string.
using LayoutTag = Variant<int64_t, StringID>;

// A layout is known by the set of its names, so the tags whose declaring types declare the same names have one. The tags are in the order of the members.
struct TaggedLayout {
    Vector<LayoutTag, 1> tags;
    LayoutID layout;
    Vector<LayoutID, 2> declaringTypes;
};

// The members are in the order of their first tags: the integers ascending, then the strings by id.
struct TaggedFamily {
    LayoutID maker;
    PropertyKeyID tagKey;
    Vector<TaggedLayout> members;
};

void appendTaggedLayouts(Program&, std::span<const TaggedFamily>);
void appendFieldsOfTaggedLayouts(Program&, std::span<const TaggedFamily>);

// The pass AllocateTaggedLayouts, for a program as read.
void allocateTaggedLayouts(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

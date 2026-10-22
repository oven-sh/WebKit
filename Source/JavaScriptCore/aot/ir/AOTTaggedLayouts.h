/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <span>
#include <wtf/Variant.h>
#include <wtf/Vector.h>

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

// The pass AllocateTaggedLayouts, for a program as read. A constructor function that is the maker of a family gets a copy of its body that allocates `this` itself,
// with the layout of the tag, and enters the copy where new.target is one of its own closures. Its own body stays as it is, for a `this` that somebody hands in.
// A base class constructor that is a maker chooses the layout where it allocates. The members of a family whose maker can do neither are never allocated.
void allocateTaggedLayouts(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

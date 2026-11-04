/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTMechanisms.h"
#include "AOTTaggedLayouts.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndexTable;
class LinkFlow;
class Program;
class ThinSummaries;

// One constructor makes the objects of a declared union that a tag tells apart (Program::taggedFamilies), and stores one of its parameters, as it came, under the key of the tag.
struct TaggedMaker {
    // In Program::taggedFamilies.
    uint32_t family { 0 };
    FunctionID constructor;
    // Among the entry parameters of the constructor: a positional one.
    uint32_t tagParameter { 0 };
    // Of the constructor of a class: the NewObject of its body that makes the object. Null: whoever constructs makes `this`.
    ValueID allocation;
};

// Ascending by constructor. By the records, the summaries and the bodies of the constructors alone, so it answers the same in front of Link and behind it. Empty, as with TypedSlots withheld: every object has its maker's layout. IT CLAIMS NOTHING.
Vector<TaggedMaker> findTaggedMakers(const Program&, const ThinSummaries&, const BodyIndexTable&, Mechanisms);
// Null: the function is no such constructor.
const TaggedMaker* findTaggedMaker(std::span<const TaggedMaker> ascendingByConstructor, FunctionID constructor);
// The member whose objects are made with the tag. Null: no member lists the tag, and the object has the maker's own layout.
LayoutID layoutMadeWithTag(const TaggedFamily&, const LayoutTag&);
// For comeAliveForUnions(): the constructor of a tagged maker is reached, so each member of its family is allocated (LinkFlow::noteAllocated()). Any other function: nothing. AOTLinkUnionTaggedMakers.cpp.
void noteLayoutsMadeByTag(LinkFlow&, FunctionID reached);
// Every layout that is written below the layout is a member of a tagged family of which it is the maker: no class extends it, so new.target selects no object that nobody has seen. False claims nothing.
bool onlyTaggedMembersLieBelow(const Program&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

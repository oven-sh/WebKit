/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMemoryLocation.h"

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTIRPrinter.h"
#include <bit>
#include <wtf/CommaPrinter.h>
#include <wtf/HashFunctions.h>

namespace JSC { namespace AOT { namespace Typed {

static uint64_t bitOf(uint32_t first, uint32_t second)
{
    return uint64_t { 1 } << (pairIntHash(first, second) & 63);
}

static uint64_t maskOfKey(PropertyKeyID key)
{
    bool idDependsOnThreadTiming = TypeTable::isProvisional(key);
    if (idDependsOnThreadTiming)
        return LocationMasks::entireHeap;
    return bitOf(key.index(), 0);
}

static uint64_t maskOfLayouts(const LayoutSet& layouts)
{
    if (!layouts.isComplete)
        return LocationMasks::entireHeap;
    uint64_t mask = 0;
    for (LayoutID member : layouts.members)
        mask |= bitOf(member.index(), 0);
    return mask;
}

static uint64_t maskOfElementBase(ElementBase base)
{
    switch (base) {
    case ElementBase::Unknown:
        return LocationMasks::entireHeap;
    case ElementBase::Array:
    case ElementBase::Arguments:
    case ElementBase::Object:
        return uint64_t { 1 } << static_cast<unsigned>(base);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint64_t computeMask(const MemoryLocation& location, NOESCAPE const Invocable<uint64_t(LayoutSetID)> auto& maskOfLayoutSet)
{
    if (location.isEntireHeap())
        return LocationMasks::entireHeap;

    switch (location.heap()) {
    case AbstractHeap::ModuleVariables:
    case AbstractHeap::EnvironmentFields:
    case AbstractHeap::MemberGuards:
        return bitOf(location.firstIndex(), location.secondIndex());
    case AbstractHeap::ObjectFields:
    case AbstractHeap::NamedProperties:
        return maskOfKey(location.key());
    case AbstractHeap::ShapeWords:
        return maskOfLayoutSet(location.layoutSet());
    case AbstractHeap::Elements:
        return maskOfElementBase(location.elementBase());
    case AbstractHeap::OrderWords:
    case AbstractHeap::ShapeEpoch:
    case AbstractHeap::PrototypeLinks:
    case AbstractHeap::ArrayStorage:
    case AbstractHeap::TypedArrayStorage:
    case AbstractHeap::TypedArrayElements:
    case AbstractHeap::CollectionStorage:
    case AbstractHeap::CollectionEntries:
    case AbstractHeap::InternalFields:
    case AbstractHeap::RegExpState:
    case AbstractHeap::PromiseState:
    case AbstractHeap::StringStorage:
    case AbstractHeap::StringBuilderContents:
    case AbstractHeap::ParserState:
    case AbstractHeap::ExternalState:
    case AbstractHeap::AllocatorState:
    case AbstractHeap::RuntimeState:
    case AbstractHeap::ValidationState:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t maskOf(const Program& program, const MemoryLocation& location)
{
    return computeMask(location, [&](LayoutSetID layouts) {
        return maskOfLayouts(program.at(layouts));
    });
}

void LocationMasks::add(const MemoryLocation& location)
{
    add(location.heap(), computeMask(location, [](LayoutSetID) {
        return entireHeap;
    }));
}

static LocationOverlap overlapOfLayouts(const LayoutSet& a, const LayoutSet& b)
{
    if (!a.isComplete || !b.isComplete || overlaps(a, b))
        return LocationOverlap::May;
    return LocationOverlap::None;
}

static LocationOverlap overlapOfFields(const Program& program, const MemoryLocation& a, const MemoryLocation& b)
{
    if (a.key() != b.key())
        return LocationOverlap::None;
    if (a == b)
        return LocationOverlap::Exact;
    if (!a.receivers() || !b.receivers())
        return LocationOverlap::May;
    return overlapOfLayouts(program.at(a.receivers()), program.at(b.receivers()));
}

static LocationOverlap overlapOfElementBases(ElementBase a, ElementBase b)
{
    if (a == ElementBase::Unknown || b == ElementBase::Unknown)
        return LocationOverlap::May;
    return a == b ? LocationOverlap::Exact : LocationOverlap::None;
}

LocationOverlap overlapOf(const Program& program, const MemoryLocation& a, const MemoryLocation& b)
{
    if (a.heap() != b.heap())
        return LocationOverlap::None;
    if (a.isEntireHeap() || b.isEntireHeap())
        return LocationOverlap::May;

    switch (a.heap()) {
    case AbstractHeap::ShapeWords:
        if (a == b)
            return LocationOverlap::Exact;
        return overlapOfLayouts(program.at(a.layoutSet()), program.at(b.layoutSet()));
    case AbstractHeap::Elements:
        return overlapOfElementBases(a.elementBase(), b.elementBase());
    case AbstractHeap::ObjectFields:
        return overlapOfFields(program, a, b);
    case AbstractHeap::ModuleVariables:
    case AbstractHeap::EnvironmentFields:
    case AbstractHeap::MemberGuards:
    case AbstractHeap::NamedProperties:
        return a == b ? LocationOverlap::Exact : LocationOverlap::None;
    case AbstractHeap::OrderWords:
    case AbstractHeap::ShapeEpoch:
    case AbstractHeap::PrototypeLinks:
    case AbstractHeap::ArrayStorage:
    case AbstractHeap::TypedArrayStorage:
    case AbstractHeap::TypedArrayElements:
    case AbstractHeap::CollectionStorage:
    case AbstractHeap::CollectionEntries:
    case AbstractHeap::InternalFields:
    case AbstractHeap::RegExpState:
    case AbstractHeap::PromiseState:
    case AbstractHeap::StringStorage:
    case AbstractHeap::StringBuilderContents:
    case AbstractHeap::ParserState:
    case AbstractHeap::ExternalState:
    case AbstractHeap::AllocatorState:
    case AbstractHeap::RuntimeState:
    case AbstractHeap::ValidationState:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpLayouts(IRPrinter& printer, const LayoutSet& layouts)
{
    CommaPrinter bar("|"_s);
    for (LayoutID member : layouts.members) {
        printer.out().print(bar);
        printer.print(member);
    }
    if (!layouts.isComplete)
        printer.out().print(bar, "?"_s);
}

void MemoryLocation::dump(PrintStream& out, const Program& program) const
{
    out.print(nameOf(m_heap));
    if (m_isEntireHeap)
        return;

    IRPrinter printer(out, program);
    out.print("["_s);
    switch (m_heap) {
    case AbstractHeap::ModuleVariables:
        printer.print(variable());
        break;
    case AbstractHeap::EnvironmentFields:
        printer.print(environmentLayout());
        out.print(", "_s);
        printer.print(environmentFieldIndex());
        break;
    case AbstractHeap::ObjectFields:
        printer.print(key());
        if (receivers()) {
            out.print(", "_s);
            dumpLayouts(printer, program.at(receivers()));
        }
        break;
    case AbstractHeap::NamedProperties:
        printer.print(key());
        break;
    case AbstractHeap::ShapeWords:
        dumpLayouts(printer, program.at(layoutSet()));
        break;
    case AbstractHeap::MemberGuards:
        printer.print(guardedLayout());
        break;
    case AbstractHeap::Elements:
        printer.print(elementBase());
        break;
    case AbstractHeap::OrderWords:
    case AbstractHeap::ShapeEpoch:
    case AbstractHeap::PrototypeLinks:
    case AbstractHeap::ArrayStorage:
    case AbstractHeap::TypedArrayStorage:
    case AbstractHeap::TypedArrayElements:
    case AbstractHeap::CollectionStorage:
    case AbstractHeap::CollectionEntries:
    case AbstractHeap::InternalFields:
    case AbstractHeap::RegExpState:
    case AbstractHeap::PromiseState:
    case AbstractHeap::StringStorage:
    case AbstractHeap::StringBuilderContents:
    case AbstractHeap::ParserState:
    case AbstractHeap::ExternalState:
    case AbstractHeap::AllocatorState:
    case AbstractHeap::RuntimeState:
    case AbstractHeap::ValidationState:
        RELEASE_ASSERT_NOT_REACHED();
    }
    out.print("]"_s);
}

void MemoryLocationDump::dump(PrintStream& out) const
{
    location.dump(out, program);
}

void LocationMasks::dump(PrintStream& out) const
{
    if (isEmpty()) {
        out.print("none"_s);
        return;
    }
    if (isTop()) {
        out.print("all"_s);
        return;
    }
    CommaPrinter comma;
    forEach([&](AbstractHeap heap, uint64_t mask) {
        out.print(comma, nameOf(heap));
        if (mask != entireHeap)
            out.print("/"_s, std::popcount(mask));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

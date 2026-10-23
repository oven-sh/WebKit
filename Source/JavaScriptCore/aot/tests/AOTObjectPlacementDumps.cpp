/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFrameAllocation.h"
#include "AOTFreshResults.h"
#include "AOTSinkAllocationsInternal.h"
#include "AOTSinkAllocationsState.h"
#include "AOTVirtualObject.h"
#include "AOTVirtualObjectShapes.h"
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

ASCIILiteral nameOf(VirtualObjectKind kind)
{
    switch (kind) {
    case VirtualObjectKind::Opaque:
        return "Opaque"_s;
    case VirtualObjectKind::Object:
        return "Object"_s;
    case VirtualObjectKind::Environment:
        return "Environment"_s;
    case VirtualObjectKind::Closure:
        return "Closure"_s;
    case VirtualObjectKind::Array:
        return "Array"_s;
    case VirtualObjectKind::ArgumentList:
        return "ArgumentList"_s;
    case VirtualObjectKind::Arguments:
        return "Arguments"_s;
    case VirtualObjectKind::StringRange:
        return "StringRange"_s;
    case VirtualObjectKind::BuiltinFields:
        return "BuiltinFields"_s;
    case VirtualObjectKind::BuiltinPrefix:
        return "BuiltinPrefix"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VirtualFieldKind kind)
{
    switch (kind) {
    case VirtualFieldKind::Named:
        return "Named"_s;
    case VirtualFieldKind::UnprovenNamed:
        return "UnprovenNamed"_s;
    case VirtualFieldKind::LayoutField:
        return "LayoutField"_s;
    case VirtualFieldKind::Element:
        return "Element"_s;
    case VirtualFieldKind::Capture:
        return "Capture"_s;
    case VirtualFieldKind::Length:
        return "Length"_s;
    case VirtualFieldKind::Capacity:
        return "Capacity"_s;
    case VirtualFieldKind::Prototype:
        return "Prototype"_s;
    case VirtualFieldKind::Callee:
        return "Callee"_s;
    case VirtualFieldKind::ArgumentCount:
        return "ArgumentCount"_s;
    case VirtualFieldKind::RestList:
        return "RestList"_s;
    case VirtualFieldKind::StringBase:
        return "StringBase"_s;
    case VirtualFieldKind::RangeStart:
        return "RangeStart"_s;
    case VirtualFieldKind::RangeEnd:
        return "RangeEnd"_s;
    case VirtualFieldKind::Operand:
        return "Operand"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VirtualObjectSupport support)
{
    switch (support) {
    case VirtualObjectSupport::Unknown:
        return "Unknown"_s;
    case VirtualObjectSupport::NotAnAllocation:
        return "NotAnAllocation"_s;
    case VirtualObjectSupport::NotFresh:
        return "NotFresh"_s;
    case VirtualObjectSupport::HasEffectsAtAllocation:
        return "HasEffectsAtAllocation"_s;
    case VirtualObjectSupport::HasChosenStorage:
        return "HasChosenStorage"_s;
    case VirtualObjectSupport::HasNoPartsForm:
        return "HasNoPartsForm"_s;
    case VirtualObjectSupport::ExceedsLimit:
        return "ExceedsLimit"_s;
    case VirtualObjectSupport::Supported:
        return "Supported"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(VirtualConstant constant)
{
    switch (constant) {
    case VirtualConstant::Undefined:
        return "Undefined"_s;
    case VirtualConstant::Null:
        return "Null"_s;
    case VirtualConstant::False:
        return "False"_s;
    case VirtualConstant::True:
        return "True"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void VirtualField::dump(PrintStream& out) const
{
    switch (kind) {
    case VirtualFieldKind::Named:
        out.print("named(k", key.index(), ")");
        return;
    case VirtualFieldKind::UnprovenNamed:
        out.print("unproven(k", key.index(), ")");
        return;
    case VirtualFieldKind::LayoutField:
        out.print("field(L", layout.index(), ".", index, ")");
        return;
    case VirtualFieldKind::Element:
        out.print("element(", index, ")");
        return;
    case VirtualFieldKind::Capture:
        out.print("capture(", index, ")");
        return;
    case VirtualFieldKind::Operand:
        out.print("operand(", index, ")");
        return;
    case VirtualFieldKind::Length:
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
        out.print(nameOf(kind));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpDescription(PrintStream& out, const VirtualObjectDescription& description)
{
    out.print("v", description.allocation.index(), " ", nameOf(description.kind));
    if (description.layout)
        out.print(" L", description.layout.index());
    if (description.function)
        out.print(" f", description.function.index());
    if (description.absentResult)
        out.print(" absent=", nameOf(*description.absentResult));
}

static void dumpShape(PrintStream& out, const VirtualShape& shape)
{
    switch (shape.kind) {
    case VirtualShapeKind::Cell:
        out.print("cell");
        return;
    case VirtualShapeKind::Constant:
        out.print(nameOf(shape.constant));
        return;
    case VirtualShapeKind::Fields:
        break;
    }
    out.print("description=", shape.descriptionIndex, " stored(");
    CommaPrinter comma(","_s);
    for (size_t field : shape.stored)
        out.print(comma, field);
    out.print(") order=", shape.order.index(), " length=", shape.length);
}

void VirtualFamily::dump(PrintStream& out) const
{
    for (uint32_t index = 0; index < m_descriptions.size(); ++index) {
        out.print("description ", index, ": ");
        dumpDescription(out, m_descriptions[index]);
        out.println();
    }
    for (uint32_t field = 0; field < m_fields.size(); ++field)
        out.println("field ", field, ": ", m_fields[field]);
    for (uint32_t shape = 0; shape < m_shapes.size(); ++shape) {
        out.print("s", shape, ": ");
        dumpShape(out, m_shapes[shape]);
        out.println();
    }
}

static void dumpVariant(PrintStream& out, const ObjectVariant& variant)
{
    if (variant.tagBit)
        out.print("b", static_cast<unsigned>(*variant.tagBit), ":");
    if (variant.shape == cellShape)
        out.print("cell");
    else
        out.print("s", variant.shape.index());
}

static void dumpObject(PrintStream& out, const ObjectEntry& entry)
{
    const ObjectState& state = entry.state;
    out.print("o", entry.object.index(), ": {");
    CommaPrinter variantSeparator(" "_s);
    for (const ObjectVariant& variant : state.variants) {
        out.print(variantSeparator);
        dumpVariant(out, variant);
    }
    out.print("}");

    if (!state.heldObjects.isEmpty()) {
        out.print(" holds(");
        CommaPrinter separator(","_s);
        for (const HeldObject& held : state.heldObjects)
            out.print(separator, held.field, "->o", held.object.index());
        out.print(")");
    }
    if (!state.fieldsValidInCell.isEmpty()) {
        out.print(" valid(");
        CommaPrinter separator(","_s);
        for (size_t field : state.fieldsValidInCell)
            out.print(separator, field);
        out.print(")");
    }
    if (state.cellJoinedAt)
        out.print(" joined(b", state.cellJoinedAt.index(), ")");
    out.print("\n");
}

static void dumpAlias(PrintStream& out, const AliasEntry& alias)
{
    out.print("v", alias.value.index(), "=");
    switch (alias.kind) {
    case AliasKind::Object:
        out.print("o", alias.object.index());
        return;
    case AliasKind::Guard:
        out.print("guard(o", alias.object.index(), ")");
        return;
    case AliasKind::Results:
        out.print("results(o", alias.object.index(), ")");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpTest(PrintStream& out, const TestEntry& entry)
{
    out.print("v", entry.test.index(), "=");
    if (entry.result) {
        out.print(*entry.result ? "true" : "false");
        return;
    }
    if (!entry.tagBitsWhenTrue) {
        out.print("unknown");
        return;
    }
    out.print("tag(o", entry.object.index(), ":");
    for (unsigned tagBit = 0; tagBit < maxTagBitsInInt32; ++tagBit) {
        if (entry.tagBitsWhenTrue >> tagBit & 1)
            out.print(" b", tagBit);
    }
    out.print(")");
}

void SinkingState::dump(PrintStream& out) const
{
    if (!isReachable) {
        out.print("unreachable\n");
        return;
    }

    for (const ObjectEntry& entry : objects)
        dumpObject(out, entry);

    if (!aliases.isEmpty()) {
        CommaPrinter separator(" "_s);
        for (const AliasEntry& alias : aliases) {
            out.print(separator);
            dumpAlias(out, alias);
        }
        out.print("\n");
    }
    if (!tests.isEmpty()) {
        CommaPrinter separator(" "_s);
        for (const TestEntry& entry : tests) {
            out.print(separator);
            dumpTest(out, entry);
        }
        out.print("\n");
    }
}

static ASCIILiteral sourceTextOf(VirtualConstant constant)
{
    switch (constant) {
    case VirtualConstant::Undefined:
        return "undefined"_s;
    case VirtualConstant::Null:
        return "null"_s;
    case VirtualConstant::False:
        return "false"_s;
    case VirtualConstant::True:
        return "true"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpType(PrintStream& out, TypeID type)
{
    using NamedType = std::pair<TypeID, ASCIILiteral>;
    static constexpr NamedType fixedTypes[] {
        { TypeTable::never, "never"_s },
        { TypeTable::undefined, "undefined"_s },
        { TypeTable::boolean, "boolean"_s },
        { TypeTable::int32, "int32"_s },
        { TypeTable::integer, "integer"_s },
        { TypeTable::number, "number"_s },
        { TypeTable::string, "string"_s },
        { TypeTable::function, "function"_s },
        { TypeTable::object, "object"_s },
        { TypeTable::anything, "anything"_s },
        { TypeTable::null, "null"_s },
        { TypeTable::nullish, "nullish"_s },
        { TypeTable::absent, "absent"_s },
        { TypeTable::guard, "guard"_s },
    };
    auto fixedType = std::ranges::find(fixedTypes, type, &NamedType::first);
    if (fixedType != std::end(fixedTypes)) {
        out.print(fixedType->second);
        return;
    }
    out.print("t", type.index());
}

static void dumpField(PrintStream& out, const FreshResultField& entry)
{
    if (entry.field.kind == VirtualFieldKind::Named)
        out.print("k", entry.field.key.index());
    else
        out.print(entry.field);
    out.print(":p", entry.part);
}

static void dumpVariant(PrintStream& out, const FreshResultVariant& variant)
{
    switch (variant.kind) {
    case VirtualShapeKind::Cell:
        out.print("cell");
        return;
    case VirtualShapeKind::Constant:
        out.print(sourceTextOf(variant.constant));
        return;
    case VirtualShapeKind::Fields:
        break;
    }

    if (variant.description.layout)
        out.print("L", variant.description.layout.index());
    else
        out.print(nameOf(variant.description.kind));
    if (variant.description.kind == VirtualObjectKind::Array)
        out.print("[", variant.length, "]");

    out.print("{");
    CommaPrinter space(" "_s);
    for (const FreshResultField& property : variant.properties) {
        out.print(space);
        dumpField(out, property);
    }
    for (const FreshResultField& part : variant.parts) {
        out.print(space);
        dumpField(out, part);
    }
    out.print("}");
}

void FreshResultShape::dump(PrintStream& out) const
{
    switch (state) {
    case FreshResultsState::Unknown:
        out.print("unknown");
        return;
    case FreshResultsState::NotFresh:
        out.print("not fresh");
        return;
    case FreshResultsState::Fresh:
        break;
    }

    out.print("fresh");
    if (hasTag())
        out.print(" tag");

    out.print(" variants=[");
    CommaPrinter variantComma;
    for (const FreshResultVariant& variant : variants) {
        out.print(variantComma);
        dumpVariant(out, variant);
    }

    out.print("] parts=[");
    CommaPrinter partComma;
    for (TypeID type : partTypes) {
        out.print(partComma);
        dumpType(out, type);
    }
    out.print("]");
}

static void dumpStorage(PrintStream& out, ASCIILiteral prefix, const BitVector& inFrames, const BitVector& onHeap)
{
    for (size_t index : inFrames) {
        if (onHeap.get(index))
            out.println(prefix, index, " may be in a frame");
        else
            out.println(prefix, index, " only in frames");
    }
}

void FrameAllocation::dump(PrintStream& out, const Program&) const
{
    RELEASE_ASSERT(m_isFinished);
    if (m_layoutsInFrames.isEmpty() && m_closuresInFrames.isEmpty()) {
        out.println("nothing in frames");
        return;
    }
    dumpStorage(out, "L"_s, m_layoutsInFrames, m_layoutsOnHeap);
    dumpStorage(out, "f"_s, m_closuresInFrames, m_closuresOnHeap);
    for (FieldReference field : m_fieldsHoldingFramePointers)
        out.println("L", field.layout.index(), ".", field.field, " holds frame pointers");
    for (CaptureReference capture : m_capturesHoldingFramePointers)
        out.println("f", capture.function.index(), " capture ", capture.capture, " holds frame pointers");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

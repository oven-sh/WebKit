/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTID.h"
#include "AOTPrimitives.h"
#include "AOTRepresentation.h"
#include "AOTType.h"
#include "AOTVector.h"
#include "ArrayBufferSharingMode.h"
#include "JSType.h"
#include <optional>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

enum class TypeTestVerdict : uint8_t { NeedsTest, AlwaysPasses, NeverPasses };
// Covers objects only: an IsSymbol test in cellTests applies under every verdict.
enum class ObjectsVerdict : uint8_t { NeedTest, DoNotOccur, AllPass, AllFail };

struct CellTest {
    CellTestKind kind;
    JSTypeRange cellTypes { CellType, CellType };
    LayoutSetID layouts { };
    BuiltinClassID builtinClass { };
    BuiltinID builtin { };
    FunctionID function { };
    PropertyKeyID symbol { };
    ArrayBufferSharingMode sharingMode { ArrayBufferSharingMode::Default };
    ModuleID module { };

    static CellTest isObject() { return { .kind = CellTestKind::IsObject }; }
    static CellTest isCallable() { return { .kind = CellTestKind::IsCallable }; }
    static CellTest cellTypeIs(JSTypeRange range) { return { .kind = CellTestKind::CellTypeIs, .cellTypes = range }; }
    static CellTest shapeIn(LayoutSetID layouts) { return { .kind = CellTestKind::ShapeIn, .layouts = layouts }; }
    static CellTest inheritsClassInfo(BuiltinClassID builtinClass) { return { .kind = CellTestKind::InheritsClassInfo, .builtinClass = builtinClass }; }
    static CellTest isPristine(BuiltinClassID builtinClass) { return { .kind = CellTestKind::IsPristine, .builtinClass = builtinClass }; }
    static CellTest isBuiltinCell(BuiltinID builtin) { return { .kind = CellTestKind::IsBuiltinCell, .builtin = builtin }; }
    static CellTest isClosureOf(FunctionID function) { return { .kind = CellTestKind::IsClosureOf, .function = function }; }
    static CellTest isSymbol(PropertyKeyID symbol) { return { .kind = CellTestKind::IsSymbol, .symbol = symbol }; }
    static CellTest sharingModeIs(ArrayBufferSharingMode sharingMode) { return { .kind = CellTestKind::SharingModeIs, .sharingMode = sharingMode }; }
    static CellTest isNamespaceOf(ModuleID module) { return { .kind = CellTestKind::IsNamespaceOf, .module = module }; }
};

struct TypeTestPlan {
    TypeTestVerdict verdict { TypeTestVerdict::NeedsTest };
    TypeID from;
    TypeID to;
    Representation representation { Representation::JSValue };
    uint16_t passingPrimitives { 0 };
    uint16_t failingPrimitives { 0 };
    std::optional<Range> integers;
    Vector<StringID, 4> stringLiterals;
    Vector<CellTest, 4> cellTests;
    ObjectsVerdict objects { ObjectsVerdict::NeedTest };

    uint16_t occurringPrimitives() const { return passingPrimitives | failingPrimitives; }
    bool cellsOccur() const { return (occurringPrimitives() & cellPrimitives) || objects != ObjectsVerdict::DoNotOccur; }
    bool someCellPasses() const { return (passingPrimitives & cellPrimitives) || !cellTests.isEmpty() || objects == ObjectsVerdict::AllPass; }
    bool everyCellPasses() const { return !(failingPrimitives & cellPrimitives) && stringLiterals.isEmpty() && (objects == ObjectsVerdict::DoNotOccur || objects == ObjectsVerdict::AllPass); }
    bool everyNonCellPasses() const { return !(failingPrimitives & ~cellPrimitives) && !integers; }

    void dump(PrintStream&) const;
};

TypeTestPlan computeTypeTestPlan(const Program&, TypeID from, Representation, TypeID to);
TypeTestPlan computeTypeTestPlanFromAnything(const Program&, const Type& to);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

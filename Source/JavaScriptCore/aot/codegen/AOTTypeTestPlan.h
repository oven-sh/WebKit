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
    LayoutID classLayout { };

    static CellTest isObject() { return { .kind = CellTestKind::IsObject }; }
};

struct TypeTestPlan {
    TypeTestVerdict verdict { TypeTestVerdict::NeedsTest };
    TypeID from { };
    TypeID to { };
    Representation representation { Representation::JSValue };
    uint16_t passingPrimitives { 0 };
    uint16_t failingPrimitives { 0 };
    std::optional<Range> integers { };
    Vector<StringID, 4> stringLiterals { };
    Vector<CellTest, 4> cellTests { };
    ObjectsVerdict objects { ObjectsVerdict::NeedTest };

    uint16_t occurringPrimitives() const { return passingPrimitives | failingPrimitives; }
    bool cellsOccur() const { return (occurringPrimitives() & cellPrimitives) || objects != ObjectsVerdict::DoNotOccur; }
    bool someCellPasses() const { return (passingPrimitives & cellPrimitives) || !cellTests.isEmpty() || objects == ObjectsVerdict::AllPass; }
    bool everyCellPasses() const { return !(failingPrimitives & cellPrimitives) && stringLiterals.isEmpty() && (objects == ObjectsVerdict::DoNotOccur || objects == ObjectsVerdict::AllPass); }
    bool everyNonCellPasses() const { return !(failingPrimitives & ~cellPrimitives) && !integers; }
    bool looksIntoCells() const { return verdict == TypeTestVerdict::NeedsTest && cellsOccur() && someCellPasses() && !everyCellPasses(); }

    void dump(PrintStream&) const;
};

TypeTestPlan computeTypeTestPlan(const Program&, TypeID from, Representation, TypeID to);
TypeTestPlan computeTypeTestPlanFromAnything(const Program&, const Type& to);
TypeTestPlan computeTypeTestPlanOfBoxedValue(const Program&, const Type& from, const Type& to);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

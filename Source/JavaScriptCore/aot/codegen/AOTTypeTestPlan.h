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
#include "AOTTypeArena.h"
#include "ArrayBufferSharingMode.h"
#include "JSType.h"
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class Plan;

enum class TypeTestVerdict : uint8_t { NeedsTest, AlwaysPasses, NeverPasses };
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
    bool asksForInstance { false };

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
};

struct KindsInLayoutSet {
    bool holdsNamespace { false };
    bool mayHaveShapes { false };
};
using KindsInLayoutSets = UncheckedKeyHashMap<LayoutSetID, KindsInLayoutSet>;

struct TypeTestContext {
    const Plan& plan;
    bool programHasOneInstance;
    KindsInLayoutSets* kindsInSetsOutsideThread { nullptr };
};

bool holdsOnlyCompiledObjects(TypeReader, const Plan&, const Type&);

TypeTestPlan computeTypeTestPlan(TypeArena&, const TypeTestContext&, TypeID from, Representation, TypeID to);
bool recordInTablesTellsType(TypeArena&, TypeID);
TypeTestPlan computeTypeTestPlanFromAnything(TypeReader, const Type& to);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

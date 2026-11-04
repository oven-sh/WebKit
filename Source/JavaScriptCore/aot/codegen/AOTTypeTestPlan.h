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

namespace JSC { namespace AOT { namespace Typed {

class Plan;

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
    // ShapeIn: a cell that has one of the shapes is asked for its instance too, and passes only if it is of this one.
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
    bool looksIntoCells() const { return verdict == TypeTestVerdict::NeedsTest && cellsOccur() && someCellPasses() && !everyCellPasses(); }
    // The emitter writes what the tests say, and the walk takes BelongsToInstance for answered behind the passing edge.
    bool asksEveryPassingObjectForInstance() const
    {
        if (verdict != TypeTestVerdict::NeedsTest || objects != ObjectsVerdict::NeedTest)
            return false;
        bool someObjectMayPass = false;
        for (const CellTest& test : cellTests) {
            if (test.kind == CellTestKind::IsSymbol)
                continue;
            if (!test.asksForInstance)
                return false;
            someObjectMayPass = true;
        }
        return someObjectMayPass;
    }
};

// What the code of a test goes by beside the two types.
struct TypeTestContext {
    const Plan& plan;
    bool programHasOneInstance;
};

// The object of a layout without compiled storage, such as a module namespace or an instance of a class below a built-in class, has no shape word.
bool holdsOnlyCompiledObjects(TypeReader, const Plan&, const Type&);

// `to` is in the words of tests: TypeArena::describedByShape(), then testablePartOf(). A test tells an object that the type names from others of its layout or class only where that object is a closure, a built-in or a namespace.
// Both types may lie in the arena, and so does whatever is made of them on the way. The plan lives no longer than they do.
TypeTestPlan computeTypeTestPlan(TypeArena&, const TypeTestContext&, TypeID from, Representation, TypeID to);
// Of a boxed value that may be anything, for a record of the run time: no test of it has asksForInstance. It makes no type, so whoever can read `to` may ask.
TypeTestPlan computeTypeTestPlanFromAnything(TypeReader, const Type& to);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

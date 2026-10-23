/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestPlan.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <tuple>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(DerivedArrayType == ArrayType + 1);

static void classifyPrimitive(TypeTestPlan& plan, uint16_t primitive, bool passes)
{
    (passes ? plan.passingPrimitives : plan.failingPrimitives) |= primitive;
}

static void classifyPlainPrimitives(TypeTestPlan& plan, const Type& from, const Type& to)
{
    for (uint16_t primitive : { Primitive::Undefined, Primitive::Null, Primitive::False, Primitive::True, Primitive::BigInt, Primitive::Absent, Primitive::WholeNumber, Primitive::FractionalNumber }) {
        if (from.primitives & primitive)
            classifyPrimitive(plan, primitive, to.primitives & primitive);
    }
}

static void classifyIntegers(TypeTestPlan& plan, const Type& from, const Type& to)
{
    if (!(from.primitives & Primitive::Integer))
        return;
    bool passes = (to.primitives & Primitive::Integer) && !from.integers.intersectWith(to.integers).isEmpty();
    classifyPrimitive(plan, Primitive::Integer, passes);
    if (passes && !from.integers.isSubsetOf(to.integers))
        plan.integers = to.integers;
}

static void classifyAnyString(TypeTestPlan& plan, const Type& to)
{
    classifyPrimitive(plan, Primitive::String, to.primitives & Primitive::AnyString);
    if (!(to.primitives & Primitive::String))
        plan.stringLiterals.append(to.stringLiterals.span());
}

static void classifyLiteralStrings(TypeTestPlan& plan, const Type& from, const Type& to)
{
    if (to.primitives & Primitive::String) {
        classifyPrimitive(plan, Primitive::LiteralString, true);
        return;
    }
    SortedSet<StringID> common = from.stringLiterals;
    common.intersect(to.stringLiterals);
    classifyPrimitive(plan, Primitive::LiteralString, !common.isEmpty());
    if (common.size() != from.stringLiterals.size())
        plan.stringLiterals.append(common.span());
}

static void classifyStrings(TypeTestPlan& plan, const Type& from, const Type& to)
{
    if (from.primitives & Primitive::String)
        classifyAnyString(plan, to);
    if (from.primitives & Primitive::LiteralString)
        classifyLiteralStrings(plan, from, to);
}

static void classifySymbols(TypeTestPlan& plan, const Type& from, const Type& to)
{
    if (!(from.primitives & Primitive::AnySymbol))
        return;
    bool fromListsSymbols = !from.symbolLiterals.isEmpty();
    bool everySymbolPasses = to.primitives & Primitive::Symbol;
    bool passes = everySymbolPasses || (fromListsSymbols && from.symbolLiterals.isSubsetOf(to.symbolLiterals));
    classifyPrimitive(plan, Primitive::Symbol, passes);
    if (passes)
        return;
    for (PropertyKeyID symbol : to.symbolLiterals) {
        if (!fromListsSymbols || from.symbolLiterals.contains(symbol))
            plan.cellTests.append(CellTest::isSymbol(symbol));
    }
}

static ObjectsVerdict computeObjectsVerdict(const Program& program, const Type& from, const Type& to, const Type& fromObjects)
{
    if (!from.isJavaScriptValue())
        return from.environment == to.environment && from.opaque == to.opaque ? ObjectsVerdict::AllPass : ObjectsVerdict::AllFail;
    if (!from.hasObjects())
        return ObjectsVerdict::DoNotOccur;
    Type toObjects = to.withOnlyObjects();
    if (fromObjects.isSubsetOf(toObjects, program))
        return ObjectsVerdict::AllPass;
    if (!to.hasObjects() || !fromObjects.overlaps(toObjects, program))
        return ObjectsVerdict::AllFail;
    return ObjectsVerdict::NeedTest;
}

static void appendCallableTest(TypeTestPlan& plan, const Program& program, const Type& to, const Type& fromObjects)
{
    if (to.anyFunction && Type::function().overlaps(fromObjects, program))
        plan.cellTests.append(CellTest::isCallable());
}

static CellTest createSharingModeTest(const Program& program, ArrayBufferSharingMode sharingMode, const Type& fromObjects)
{
    BuiltinClassID classWithOtherMode = sharingMode == ArrayBufferSharingMode::Default ? BuiltinClasses::SharedArrayBuffer : BuiltinClasses::ArrayBuffer;
    // A layout may be the one of a class that extends the other class of buffers.
    if (!fromObjects.layouts && !Type::builtinClass(classWithOtherMode).overlaps(fromObjects, program))
        return CellTest::cellTypeIs({ ArrayBufferType, ArrayBufferType });
    return CellTest::sharingModeIs(sharingMode);
}

static void appendInstanceTest(TypeTestPlan& plan, const Program& program, const BuiltinClass& builtinClass, const Type& fromObjects)
{
    switch (builtinClass.instanceTest) {
    case BuiltinInstanceTest::None:
        return;
    case BuiltinInstanceTest::JSTypeEquals:
        // Not DerivedArrayType beside ArrayType: that is Array.prototype, whose butterfly has no indexing header.
        plan.cellTests.append(CellTest::cellTypeIs({ builtinClass.type, builtinClass.type }));
        return;
    case BuiltinInstanceTest::ClassInfoInherits:
        plan.cellTests.append(CellTest::inheritsClassInfo(builtinClass.id));
        return;
    case BuiltinInstanceTest::SharingModeEquals:
        plan.cellTests.append(createSharingModeTest(program, builtinClass.sharingMode, fromObjects));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void mergeSharingModeTests(TypeTestPlan& plan)
{
    auto testsSharingMode = [](const CellTest& test) {
        return test.kind == CellTestKind::SharingModeIs;
    };
    auto hasTestOf = [&](ArrayBufferSharingMode sharingMode) {
        return plan.cellTests.containsIf([&](const CellTest& test) {
            return testsSharingMode(test) && test.sharingMode == sharingMode;
        });
    };
    if (!hasTestOf(ArrayBufferSharingMode::Default) || !hasTestOf(ArrayBufferSharingMode::Shared))
        return;
    plan.cellTests.removeAllMatching(testsSharingMode);
    plan.cellTests.append(CellTest::cellTypeIs({ ArrayBufferType, ArrayBufferType }));
}

static bool canTestClassPart(const Program& program, const Type& from, const Type& to, const ClassPart& part)
{
    bool listsRegExpLiterals = part.builtinClass == BuiltinClasses::RegExp && !to.regExpLiteralSet.isEmpty();
    if (!part.contentsAreProven && part.provenPristine.isEmpty() && !listsRegExpLiterals)
        return true;
    const ClassPart* known = from.findClassPart(part.builtinClass);
    if (!known || !isPartSubset(*known, part, program.types))
        return false;
    return !listsRegExpLiterals || (!from.regExpLiteralSet.isEmpty() && from.regExpLiteralSet.isSubsetOf(to.regExpLiteralSet));
}

static void appendClassTests(TypeTestPlan& plan, const Program& program, const Type& from, const Type& to, const Type& fromObjects)
{
    for (const ClassPart& part : to.classes) {
        if (!Type::builtinClass(part.builtinClass).overlaps(fromObjects, program))
            continue;
        if (!canTestClassPart(program, from, to, part))
            continue;
        appendInstanceTest(plan, program, program.builtins().at(part.builtinClass), fromObjects);
    }
    mergeSharingModeTests(plan);
}

static void appendFunctionAndBuiltinTests(TypeTestPlan& plan, const Program& program, const Type& to, const Type& fromObjects)
{
    for (FunctionID function : to.functions) {
        if (!to.anyFunction && Type::forFunction(function).overlaps(fromObjects, program))
            plan.cellTests.append(CellTest::isClosureOf(function));
    }
    for (BuiltinID builtin : to.builtins) {
        if (Type::forBuiltin(builtin).overlaps(fromObjects, program))
            plan.cellTests.append(CellTest::isBuiltinCell(builtin));
    }
}

static bool mayHoldLayout(const Program& program, const Type& objects, LayoutID layout)
{
    if (objects.anyObject)
        return true;
    if (!objects.layouts)
        return false;
    const LayoutSet& layouts = program.at(objects.layouts);
    return !layouts.isComplete || std::ranges::binary_search(layouts.members, layout);
}

// A namespace object has no shape.
static void appendLayoutTests(TypeTestPlan& plan, const Program& program, const Type& to, const Type& fromObjects)
{
    if (!to.layouts || !Type::forLayoutSet(to.layouts).overlaps(fromObjects, program))
        return;
    const LayoutSet& layouts = program.at(to.layouts);
    bool mayHaveShapes = !layouts.isComplete;
    for (LayoutID member : layouts.members) {
        const Layout& layout = program.at(member);
        if (layout.kind != LayoutKind::ModuleNamespace)
            mayHaveShapes = true;
        else if (mayHoldLayout(program, fromObjects, member))
            plan.cellTests.append(CellTest::isNamespaceOf(layout.namespaceOf));
    }
    if (mayHaveShapes)
        plan.cellTests.append(CellTest::shapeIn(to.layouts));
}

static void appendObjectTests(TypeTestPlan& plan, const Program& program, const Type& from, const Type& to, const Type& fromObjects)
{
    switch (plan.objects) {
    case ObjectsVerdict::DoNotOccur:
    case ObjectsVerdict::AllFail:
        return;
    case ObjectsVerdict::AllPass:
        if (to.anyObject && to.anyFunction)
            plan.cellTests.append(CellTest::isObject());
        return;
    case ObjectsVerdict::NeedTest:
        break;
    }
    if (to.anyObject) {
        plan.cellTests.append(CellTest::isObject());
        return;
    }
    appendCallableTest(plan, program, to, fromObjects);
    appendClassTests(plan, program, from, to, fromObjects);
    appendFunctionAndBuiltinTests(plan, program, to, fromObjects);
    appendLayoutTests(plan, program, to, fromObjects);
}

static unsigned orderOf(CellTestKind kind)
{
    switch (kind) {
    case CellTestKind::CellTypeIs:
        return 0;
    case CellTestKind::IsObject:
        return 1;
    case CellTestKind::ShapeIn:
        return 2;
    case CellTestKind::IsPristine:
        return 3;
    case CellTestKind::IsBuiltinCell:
        return 4;
    case CellTestKind::IsSymbol:
        return 5;
    case CellTestKind::IsClosureOf:
        return 6;
    case CellTestKind::SharingModeIs:
        return 7;
    case CellTestKind::IsCallable:
        return 8;
    case CellTestKind::InheritsClassInfo:
        return 9;
    case CellTestKind::IsNamespaceOf:
        return 10;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t subjectOf(const CellTest& test)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
        return 0;
    case CellTestKind::CellTypeIs:
        return test.cellTypes.first;
    case CellTestKind::ShapeIn:
        return test.layouts.index();
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
        return test.builtinClass.index();
    case CellTestKind::IsBuiltinCell:
        return test.builtin.index();
    case CellTestKind::IsClosureOf:
        return test.function.index();
    case CellTestKind::IsSymbol:
        return test.symbol.index();
    case CellTestKind::SharingModeIs:
        return static_cast<uint32_t>(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return test.module.index();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeTestPlan planWithVerdict(const TypeTestPlan& plan, TypeTestVerdict verdict)
{
    TypeTestPlan result;
    result.verdict = verdict;
    result.from = plan.from;
    result.to = plan.to;
    result.representation = plan.representation;
    return result;
}

static void settleVerdict(TypeTestPlan& plan)
{
    bool everyObjectPasses = plan.objects == ObjectsVerdict::DoNotOccur || plan.objects == ObjectsVerdict::AllPass;
    if (!plan.passingPrimitives && plan.cellTests.isEmpty() && plan.objects != ObjectsVerdict::AllPass)
        plan = planWithVerdict(plan, TypeTestVerdict::NeverPasses);
    else if (!plan.failingPrimitives && !plan.integers && plan.stringLiterals.isEmpty() && everyObjectPasses)
        plan = planWithVerdict(plan, TypeTestVerdict::AlwaysPasses);
}

static void fillPlan(TypeTestPlan& plan, const Program& program, const Type& from, const Type& to)
{
    classifyPlainPrimitives(plan, from, to);
    classifyIntegers(plan, from, to);
    classifyStrings(plan, from, to);
    classifySymbols(plan, from, to);
    Type fromObjects = from.withOnlyObjects();
    plan.objects = computeObjectsVerdict(program, from, to, fromObjects);
    appendObjectTests(plan, program, from, to, fromObjects);
    AOT::stableSort(plan.cellTests, { }, [](const CellTest& test) {
        return std::tuple { orderOf(test.kind), subjectOf(test) };
    });
    settleVerdict(plan);
}

static uint16_t primitivesIn(Representation representation)
{
    uint16_t primitives = widestTypeOf(representation).primitives;
    if (primitives & Primitive::String)
        primitives |= Primitive::LiteralString;
    if (primitives & Primitive::Symbol)
        primitives |= Primitive::LiteralSymbol;
    return primitives;
}

static void restrictToRepresentation(TypeTestPlan& plan)
{
    if (plan.verdict != TypeTestVerdict::NeedsTest)
        return;
    uint16_t primitives = primitivesIn(plan.representation);
    bool holdsObjects = widestTypeOf(plan.representation).hasObjects();
    plan.passingPrimitives &= primitives;
    plan.failingPrimitives &= primitives;
    if (!(plan.passingPrimitives & Primitive::Integer))
        plan.integers = std::nullopt;
    if (!(plan.passingPrimitives & Primitive::AnyString))
        plan.stringLiterals.clear();
    if (!holdsObjects)
        plan.objects = ObjectsVerdict::DoNotOccur;
    plan.cellTests.removeAllMatching([&](const CellTest& test) {
        return test.kind == CellTestKind::IsSymbol ? !(primitives & Primitive::Symbol) : !holdsObjects;
    });
    settleVerdict(plan);
}

TypeTestPlan computeTypeTestPlan(const Program& program, TypeID fromID, Representation representation, TypeID toID)
{
    RELEASE_ASSERT(fromID && toID);
    const Type& from = program.at(fromID);
    const Type& to = program.at(toID);
    TypeTestPlan plan;
    plan.from = fromID;
    plan.to = toID;
    plan.representation = representation;
    if (from.isSubsetOf(to, program))
        return planWithVerdict(plan, TypeTestVerdict::AlwaysPasses);
    if (!from.overlaps(to, program))
        return planWithVerdict(plan, TypeTestVerdict::NeverPasses);
    fillPlan(plan, program, from, to);
    restrictToRepresentation(plan);
    return plan;
}

TypeTestPlan computeTypeTestPlanFromAnything(const Program& program, const Type& to)
{
    TypeTestPlan plan;
    plan.from = TypeTable::anything;
    fillPlan(plan, program, program.at(TypeTable::anything), to);
    return plan;
}

static ASCIILiteral nameOf(TypeTestVerdict verdict)
{
    switch (verdict) {
    case TypeTestVerdict::NeedsTest:
        return "NeedsTest"_s;
    case TypeTestVerdict::AlwaysPasses:
        return "AlwaysPasses"_s;
    case TypeTestVerdict::NeverPasses:
        return "NeverPasses"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(ObjectsVerdict verdict)
{
    switch (verdict) {
    case ObjectsVerdict::NeedTest:
        return "NeedTest"_s;
    case ObjectsVerdict::DoNotOccur:
        return "DoNotOccur"_s;
    case ObjectsVerdict::AllPass:
        return "AllPass"_s;
    case ObjectsVerdict::AllFail:
        return "AllFail"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpPrimitives(PrintStream& out, uint16_t primitives)
{
    if (!primitives) {
        out.print("none");
        return;
    }
    CommaPrinter bar("|"_s);
    for (PrimitiveSet primitive : allMembersOf<PrimitiveSet>) {
        if (primitives & static_cast<uint16_t>(primitive))
            out.print(bar, nameOf(primitive));
    }
}

static void dumpSubject(PrintStream& out, const CellTest& test)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
        return;
    case CellTestKind::CellTypeIs:
        out.print(" ", test.cellTypes.first, "..", test.cellTypes.last);
        return;
    case CellTestKind::SharingModeIs:
        out.print(" ", test.sharingMode);
        return;
    case CellTestKind::ShapeIn:
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
    case CellTestKind::IsBuiltinCell:
    case CellTestKind::IsClosureOf:
    case CellTestKind::IsSymbol:
    case CellTestKind::IsNamespaceOf:
        out.print(" ", subjectOf(test));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TypeTestPlan::dump(PrintStream& out) const
{
    if (verdict != TypeTestVerdict::NeedsTest) {
        out.println(nameOf(verdict));
        return;
    }
    out.print(nameOf(verdict), " pass=");
    dumpPrimitives(out, passingPrimitives);
    out.print(" fail=");
    dumpPrimitives(out, failingPrimitives);
    if (integers)
        out.print(" integers=[", integers->minimum, ", ", integers->maximum, "]");
    else
        out.print(" integers=any");
    out.println(" strings=", stringLiterals.size(), " objects=", nameOf(objects));
    for (const CellTest& test : cellTests) {
        out.print("  cell ", nameOf(test.kind));
        dumpSubject(out, test);
        out.println();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

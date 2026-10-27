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
#include <array>
#include <tuple>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static void classifyPrimitives(TypeTestPlan& plan, const Type& from, const Type& to)
{
    auto classify = [&](uint16_t primitive, bool passes) {
        (passes ? plan.passingPrimitives : plan.failingPrimitives) |= primitive;
    };
    for (uint16_t primitive : { Primitive::Undefined, Primitive::Null, Primitive::False, Primitive::True, Primitive::BigInt, Primitive::Absent, Primitive::WholeNumber, Primitive::FractionalNumber }) {
        if (from.primitives & primitive)
            classify(primitive, to.primitives & primitive);
    }
    if (from.primitives & Primitive::Integer) {
        bool passes = (to.primitives & Primitive::Integer) && !from.integers.intersectWith(to.integers).isEmpty();
        classify(Primitive::Integer, passes);
        if (passes && !from.integers.isSubsetOf(to.integers))
            plan.integers = to.integers;
    }
    if (from.primitives & Primitive::String) {
        classify(Primitive::String, to.primitives & Primitive::AnyString);
        if (!(to.primitives & Primitive::String))
            plan.stringLiterals.append(to.stringLiterals.span());
    }
    if (from.primitives & Primitive::LiteralString && to.primitives & Primitive::String)
        classify(Primitive::LiteralString, true);
    else if (from.primitives & Primitive::LiteralString) {
        SortedSet<StringID> common = from.stringLiterals;
        common.intersect(to.stringLiterals);
        classify(Primitive::LiteralString, !common.isEmpty());
        if (common.size() != from.stringLiterals.size())
            plan.stringLiterals.append(common.span());
    }
    if (from.primitives & Primitive::AnySymbol) {
        bool fromListsSymbols = !from.symbolLiterals.isEmpty();
        bool passes = (to.primitives & Primitive::Symbol) || (fromListsSymbols && from.symbolLiterals.isSubsetOf(to.symbolLiterals));
        classify(Primitive::Symbol, passes);
        for (PropertyKeyID symbol : to.symbolLiterals) {
            if (!passes && (!fromListsSymbols || from.symbolLiterals.contains(symbol)))
                plan.cellTests.append({ .kind = CellTestKind::IsSymbol, .symbol = symbol });
        }
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

static void appendObjectTests(TypeTestPlan& plan, const Program& program, const Type& from, const Type& to, const Type& fromObjects)
{
    static constexpr JSTypeRange arrayBuffers { ArrayBufferType, ArrayBufferType };
    auto append = [&](CellTest test) { plan.cellTests.append(test); };
    auto occurs = [&](const Type& type) { return type.overlaps(fromObjects, program); };

    if (plan.objects != ObjectsVerdict::AllPass && plan.objects != ObjectsVerdict::NeedTest)
        return;
    if (to.anyObject && (to.anyFunction || plan.objects == ObjectsVerdict::NeedTest))
        append(CellTest::isObject());
    if (to.anyObject || plan.objects == ObjectsVerdict::AllPass)
        return;

    if (to.anyFunction && occurs(Type::function()))
        append({ .kind = CellTestKind::IsCallable });

    bool testsDefaultMode = false;
    bool testsSharedMode = false;
    for (const ClassPart& part : to.classes) {
        if (!occurs(Type::builtinClass(part.builtinClass)) || !canTestClassPart(program, from, to, part))
            continue;
        const BuiltinClass& builtinClass = program.builtins().at(part.builtinClass);
        switch (builtinClass.instanceTest) {
        case BuiltinInstanceTest::None:
            break;
        case BuiltinInstanceTest::JSTypeEquals:
            // Not DerivedArrayType beside ArrayType: that is Array.prototype, whose butterfly has no indexing header.
            append({ .kind = CellTestKind::CellTypeIs, .cellTypes = { builtinClass.type, builtinClass.type } });
            break;
        case BuiltinInstanceTest::ClassInfoInherits:
            append({ .kind = CellTestKind::InheritsClassInfo, .builtinClass = builtinClass.id });
            break;
        case BuiltinInstanceTest::SharingModeEquals: {
            bool isShared = builtinClass.sharingMode == ArrayBufferSharingMode::Shared;
            // A layout may be the one of a class that extends the other class of buffers.
            if (fromObjects.layouts || occurs(Type::builtinClass(isShared ? BuiltinClasses::ArrayBuffer : BuiltinClasses::SharedArrayBuffer)))
                (isShared ? testsSharedMode : testsDefaultMode) = true;
            else
                append({ .kind = CellTestKind::CellTypeIs, .cellTypes = arrayBuffers });
            break;
        }
        }
    }
    if (testsDefaultMode && testsSharedMode)
        append({ .kind = CellTestKind::CellTypeIs, .cellTypes = arrayBuffers });
    else if (testsDefaultMode || testsSharedMode)
        append({ .kind = CellTestKind::SharingModeIs, .sharingMode = testsSharedMode ? ArrayBufferSharingMode::Shared : ArrayBufferSharingMode::Default });

    for (FunctionID function : to.functions) {
        if (!to.anyFunction && occurs(Type::forFunction(function)))
            append({ .kind = CellTestKind::IsClosureOf, .function = function });
    }
    for (BuiltinID builtin : to.builtins) {
        if (occurs(Type::forBuiltin(builtin)))
            append({ .kind = CellTestKind::IsBuiltinCell, .builtin = builtin });
    }

    if (!to.layouts || !occurs(Type::forLayoutSet(to.layouts)))
        return;
    const LayoutSet* known = fromObjects.layouts ? &program.at(fromObjects.layouts) : nullptr;
    bool mayHaveShapes = false;
    for (LayoutID member : program.at(to.layouts).completeMembers()) {
        const Layout& layout = program.at(member);
        if (layout.kind != LayoutKind::ModuleNamespace)
            mayHaveShapes = true;
        else if (fromObjects.anyObject || (known && std::ranges::binary_search(known->completeMembers(), member)))
            append({ .kind = CellTestKind::IsNamespaceOf, .module = layout.namespaceOf });
    }
    if (mayHaveShapes)
        append({ .kind = CellTestKind::ShapeIn, .layouts = to.layouts });
}

static constexpr auto planOrder = [] {
    std::array<uint8_t, numberOfEnumerators<CellTestKind>> order { };
    uint8_t next = 0;
    for (CellTestKind kind : { CellTestKind::CellTypeIs, CellTestKind::IsObject, CellTestKind::ShapeIn, CellTestKind::IsPristine, CellTestKind::IsBuiltinCell, CellTestKind::IsSymbol, CellTestKind::IsClosureOf, CellTestKind::SharingModeIs, CellTestKind::IsCallable, CellTestKind::InheritsClassInfo, CellTestKind::IsNamespaceOf })
        order[static_cast<size_t>(kind)] = next++;
    return order;
}();

static uint32_t subjectOf(const CellTest& test)
{
    switch (test.kind) {
    case CellTestKind::CellTypeIs:
        return test.cellTypes.first;
    case CellTestKind::ShapeIn:
        return test.layouts.index();
    case CellTestKind::InheritsClassInfo:
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
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
    case CellTestKind::IsPristine:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void settleVerdict(TypeTestPlan& plan)
{
    bool everyObjectPasses = plan.objects == ObjectsVerdict::DoNotOccur || plan.objects == ObjectsVerdict::AllPass;
    if (!plan.passingPrimitives && plan.cellTests.isEmpty() && plan.objects != ObjectsVerdict::AllPass)
        plan = { .verdict = TypeTestVerdict::NeverPasses, .from = plan.from, .to = plan.to, .representation = plan.representation };
    else if (!plan.failingPrimitives && !plan.integers && plan.stringLiterals.isEmpty() && everyObjectPasses)
        plan = { .verdict = TypeTestVerdict::AlwaysPasses, .from = plan.from, .to = plan.to, .representation = plan.representation };
}

static void fillPlan(TypeTestPlan& plan, const Program& program, const Type& from, const Type& to)
{
    classifyPrimitives(plan, from, to);
    Type fromObjects = from.withOnlyObjects();
    plan.objects = computeObjectsVerdict(program, from, to, fromObjects);
    appendObjectTests(plan, program, from, to, fromObjects);
    AOT::stableSort(plan.cellTests, { }, [](const CellTest& test) {
        return std::tuple { planOrder[static_cast<size_t>(test.kind)], subjectOf(test) };
    });
    settleVerdict(plan);
}

static void restrictToRepresentation(TypeTestPlan& plan)
{
    if (plan.verdict != TypeTestVerdict::NeedsTest)
        return;
    Type widest = widestTypeOf(plan.representation);
    uint16_t primitives = widest.primitives;
    if (primitives & Primitive::String)
        primitives |= Primitive::LiteralString;
    if (primitives & Primitive::Symbol)
        primitives |= Primitive::LiteralSymbol;
    bool holdsObjects = widest.hasObjects();
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
    TypeTestPlan plan { .from = fromID, .to = toID, .representation = representation };
    if (from.isSubsetOf(to, program))
        plan.verdict = TypeTestVerdict::AlwaysPasses;
    else if (!from.overlaps(to, program))
        plan.verdict = TypeTestVerdict::NeverPasses;
    else {
        fillPlan(plan, program, from, to);
        restrictToRepresentation(plan);
    }
    return plan;
}

TypeTestPlan computeTypeTestPlanFromAnything(const Program& program, const Type& to)
{
    TypeTestPlan plan { .from = TypeTable::anything };
    fillPlan(plan, program, program.at(TypeTable::anything), to);
    return plan;
}

void TypeTestPlan::dump(PrintStream& out) const
{
    static constexpr std::array verdicts { "NeedsTest"_s, "AlwaysPasses"_s, "NeverPasses"_s };
    static constexpr std::array objectsVerdicts { "NeedTest"_s, "DoNotOccur"_s, "AllPass"_s, "AllFail"_s };
    auto dumpPrimitives = [&](ASCIILiteral label, uint16_t primitives) {
        out.print(label, primitives ? ""_s : "none"_s);
        CommaPrinter bar("|"_s);
        for (PrimitiveSet primitive : allMembersOf<PrimitiveSet>) {
            if (primitives & static_cast<uint16_t>(primitive))
                out.print(bar, nameOf(primitive));
        }
    };

    out.print(verdicts[static_cast<size_t>(verdict)]);
    if (verdict != TypeTestVerdict::NeedsTest)
        return out.println();
    dumpPrimitives(" pass="_s, passingPrimitives);
    dumpPrimitives(" fail="_s, failingPrimitives);
    if (integers)
        out.print(" integers=[", integers->minimum, ", ", integers->maximum, "]");
    else
        out.print(" integers=any");
    out.println(" strings=", stringLiterals.size(), " objects=", objectsVerdicts[static_cast<size_t>(objects)]);
    for (const CellTest& test : cellTests) {
        out.print("  cell ", nameOf(test.kind));
        if (test.kind == CellTestKind::CellTypeIs)
            out.print(" ", test.cellTypes.first, "..", test.cellTypes.last);
        else if (test.kind == CellTestKind::SharingModeIs)
            out.print(" ", test.sharingMode);
        else if (test.kind != CellTestKind::IsObject && test.kind != CellTestKind::IsCallable)
            out.print(" ", subjectOf(test));
        out.println();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

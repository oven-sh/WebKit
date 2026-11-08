/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestPlan.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <array>
#include <bit>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class ObjectsUnderTypeTest {
public:
    ObjectsUnderTypeTest() = default;
    ObjectsUnderTypeTest(TypeArena& arena, TypeID from, TypeID to)
        : m_arena(&arena)
        , m_from(from)
        , m_to(to)
    {
    }

    bool areAllWanted(const Type& to) const { return m_arena ? m_arena->isSubset(objects(), wantedObjects()) : to.anyObject; }
    bool mayBeWanted(const Type& to) const { return m_arena ? m_arena->overlap(objects(), wantedObjects()) : to.hasObjects(); }

    bool mayBeCallable() const { return !m_arena || m_arena->overlap(TypeTable::function, objects()); }
    bool mayBeInstanceOf(BuiltinClassID builtinClass) const { return !m_arena || m_arena->overlap(m_arena->builtinClass(builtinClass), objects()); }
    bool mayBeClosureOf(FunctionID function) const { return !m_arena || m_arena->overlap(m_arena->forFunction(function), objects()); }
    bool mayBeBuiltin(BuiltinID builtin) const { return !m_arena || m_arena->overlap(m_arena->forBuiltin(builtin), objects()); }
    bool mayHaveLayoutIn(LayoutSetID layouts) const { return !m_arena || m_arena->overlap(m_arena->forLayoutSet(layouts), objects()); }

private:
    TypeID objects() const
    {
        if (!m_objects)
            m_objects = m_arena->withOnlyObjects(m_from);
        return m_objects;
    }
    TypeID wantedObjects() const
    {
        if (!m_wantedObjects)
            m_wantedObjects = m_arena->withOnlyObjects(m_to);
        return m_wantedObjects;
    }

    TypeArena* m_arena { nullptr };
    TypeID m_from;
    TypeID m_to;
    mutable TypeID m_objects;
    mutable TypeID m_wantedObjects;
};

} // anonymous namespace

template<typename SetID>
static auto membersReadForTypeTest(TypeReader reader, SetID set) -> decltype(reader.at(set))
{
    if (!set)
        return { };
    return reader.at(set);
}

static void classifyPrimitivesForTypeTest(TypeTestPlan& plan, TypeReader reader, const Type& from, const Type& to)
{
    auto classify = [&](uint16_t primitive, bool passes) {
        (passes ? plan.passingPrimitives : plan.failingPrimitives) |= primitive;
    };
    static constexpr uint16_t primitivesOfOneBit = Primitive::Undefined | Primitive::Null | Primitive::False | Primitive::True | Primitive::BigInt | Primitive::Absent | Primitive::WholeNumber | Primitive::FractionalNumber;
    static_assert(std::popcount(primitivesOfOneBit) == 8);
    plan.passingPrimitives |= from.primitives & primitivesOfOneBit & to.primitives;
    plan.failingPrimitives |= from.primitives & primitivesOfOneBit & ~to.primitives;
    if (from.primitives & Primitive::Integer) {
        bool passes = (to.primitives & Primitive::Integer) && !from.integers.intersectWith(to.integers).isEmpty();
        classify(Primitive::Integer, passes);
        if (passes && !from.integers.isSubsetOf(to.integers))
            plan.integers = to.integers;
    }

    auto wantedStrings = membersReadForTypeTest(reader, to.stringLiterals);
    if (from.primitives & Primitive::String) {
        classify(Primitive::String, to.primitives & Primitive::AnyString);
        if (!(to.primitives & Primitive::String)) {
            wantedStrings.forEach([&](StringID literal) {
                plan.stringLiterals.append(literal);
            });
        }
    }
    if (from.primitives & Primitive::LiteralString && to.primitives & Primitive::String)
        classify(Primitive::LiteralString, true);
    else if (from.primitives & Primitive::LiteralString) {
        auto knownStrings = membersReadForTypeTest(reader, from.stringLiterals);
        Vector<StringID, 4> common;
        knownStrings.forEach([&](StringID literal) {
            if (wantedStrings.contains(literal))
                common.append(literal);
        });
        classify(Primitive::LiteralString, !common.isEmpty());
        if (common.size() != knownStrings.size())
            plan.stringLiterals.appendVector(common);
    }

    if (from.primitives & Primitive::AnySymbol) {
        auto knownSymbols = membersReadForTypeTest(reader, from.symbolLiterals);
        auto wantedSymbols = membersReadForTypeTest(reader, to.symbolLiterals);
        bool fromListsSymbols = !knownSymbols.isEmpty();
        bool passes = (to.primitives & Primitive::Symbol) || (fromListsSymbols && !wantedSymbols.isEmpty() && isSubset(knownSymbols.untyped(), wantedSymbols.untyped()));
        classify(Primitive::Symbol, passes);
        if (!passes) {
            wantedSymbols.forEach([&](PropertyKeyID symbol) {
                if (!fromListsSymbols || knownSymbols.contains(symbol))
                    plan.cellTests.append({ .kind = CellTestKind::IsSymbol, .symbol = symbol });
            });
        }
    }
}

static ObjectsVerdict objectsVerdictOfTypeTest(const ObjectsUnderTypeTest& tested, const Type& from, const Type& to)
{
    if (!from.isJavaScriptValue())
        return from.environment == to.environment && from.opaque == to.opaque ? ObjectsVerdict::AllPass : ObjectsVerdict::AllFail;
    if (!from.hasObjects())
        return ObjectsVerdict::DoNotOccur;
    if (!to.hasObjects())
        return ObjectsVerdict::AllFail;
    if (tested.areAllWanted(to))
        return ObjectsVerdict::AllPass;
    if (!tested.mayBeWanted(to))
        return ObjectsVerdict::AllFail;
    return ObjectsVerdict::NeedTest;
}

static bool isSurelyFoundByTypeTest(TypeReader reader, const ObjectRecord& record, const Type& to)
{
    if (record.traits.containsAny({ ObjectTrait::IsClosure, ObjectTrait::IsBuiltin }) || (to.anyFunction && record.has(ObjectTrait::IsCallable)))
        return true;
    if (record.has(ObjectTrait::HasClass))
        return hasInstanceTest(reader.builtins(), record.builtinClass);
    return record.has(ObjectTrait::HasLayout) && !builtinClassWrittenAbove(reader, record.layout);
}

// TypeArena::differenceOf() assumes that an object named by both types passed the test, and drops it from the failing side. So the test must not refuse such an object.
static void assertTestFindsObjectsNamedByBothTypes(TypeReader reader, const Type& from, const Type& to)
{
    if (!from.objects || !to.objects)
        return;
    auto namedByValue = reader.at(from.objects);
    reader.at(to.objects).forEach([&](ObjectID object) {
        const ObjectRecord& record = reader.at(object);
        if (namedByValue.contains(object) && !record.has(ObjectTrait::IsMerged))
            RELEASE_ASSERT(isSurelyFoundByTypeTest(reader, record, to));
    });
}

bool holdsOnlyCompiledObjects(TypeReader reader, const Plan& plan, const Type& type)
{
    const SlotPlan& slots = plan.slots();
    return type.hasOnlyLayouts() && !reader.at(type.layouts).untyped().anyWordOf([&](uint32_t wordOfLayouts, uint64_t layoutsInWord) {
        return slots.someLayoutInUseHoldsOtherThanCompiledObjects(wordOfLayouts, layoutsInWord);
    });
}

static bool shapeTestOfTypeTestAsksForInstance(TypeReader reader, const TypeTestContext* context, const Type& from)
{
    return context && !context->programHasOneInstance && !holdsOnlyCompiledObjects(reader, context->plan, from);
}

static void appendObjectTestsOfTypeTest(TypeTestPlan& plan, TypeReader reader, const TypeTestContext* context, const ObjectsUnderTypeTest& tested, const Type& from, const Type& to)
{
    static constexpr JSTypeRange arrayBuffers { ArrayBufferType, ArrayBufferType };
    auto append = [&](CellTest test) { plan.cellTests.append(test); };

    if (plan.objects != ObjectsVerdict::AllPass && plan.objects != ObjectsVerdict::NeedTest)
        return;
    if (to.anyObject && (to.anyFunction || plan.objects == ObjectsVerdict::NeedTest))
        append(CellTest::isObject());
    if (to.anyObject || plan.objects == ObjectsVerdict::AllPass)
        return;

    assertTestFindsObjectsNamedByBothTypes(reader, from, to);

    if (to.anyFunction && tested.mayBeCallable())
        append({ .kind = CellTestKind::IsCallable });

    bool testsDefaultMode = false;
    bool testsSharedMode = false;
    Vector<JSType, 8> typesOfCells;
    for (const ClassPart& part : classPartsOf(reader, to)) {
        if (!tested.mayBeInstanceOf(part.builtinClass))
            continue;
        const BuiltinClass& builtinClass = reader.builtins().at(part.builtinClass);
        switch (builtinClass.instanceTest) {
        case BuiltinInstanceTest::None:
            break;
        case BuiltinInstanceTest::JSTypeEquals:
            typesOfCells.append(builtinClass.type);
            break;
        case BuiltinInstanceTest::ClassInfoInherits:
            append({ .kind = CellTestKind::InheritsClassInfo, .builtinClass = builtinClass.id });
            break;
        case BuiltinInstanceTest::SharingModeEquals: {
            bool isShared = builtinClass.sharingMode == ArrayBufferSharingMode::Shared;
            if (from.layouts || tested.mayBeInstanceOf(isShared ? BuiltinClasses::ArrayBuffer : BuiltinClasses::SharedArrayBuffer))
                (isShared ? testsSharedMode : testsDefaultMode) = true;
            else
                append({ .kind = CellTestKind::CellTypeIs, .cellTypes = arrayBuffers });
            break;
        }
        }
    }
    std::ranges::sort(typesOfCells);
    for (size_t first = 0; first < typesOfCells.size();) {
        size_t last = first;
        while (last + 1 < typesOfCells.size() && typesOfCells[last + 1] <= typesOfCells[last] + 1)
            ++last;
        append({ .kind = CellTestKind::CellTypeIs, .cellTypes = { typesOfCells[first], typesOfCells[last] } });
        first = last + 1;
    }
    if (testsDefaultMode && testsSharedMode)
        append({ .kind = CellTestKind::CellTypeIs, .cellTypes = arrayBuffers });
    else if (testsDefaultMode || testsSharedMode)
        append({ .kind = CellTestKind::SharingModeIs, .sharingMode = testsSharedMode ? ArrayBufferSharingMode::Shared : ArrayBufferSharingMode::Default });

    if (!to.anyFunction) {
        forEachFunctionOf(reader, to, [&](FunctionID function) {
            if (tested.mayBeClosureOf(function))
                append({ .kind = CellTestKind::IsClosureOf, .function = function });
        });
    }
    forEachBuiltinOf(reader, to, [&](BuiltinID builtin) {
        if (tested.mayBeBuiltin(builtin))
            append({ .kind = CellTestKind::IsBuiltinCell, .builtin = builtin });
    });

    if (!to.layouts || !tested.mayHaveLayoutIn(to.layouts))
        return;
    const Program* program = reader.program();
    RELEASE_ASSERT(program);
    auto readKinds = [&] {
        KindsInLayoutSet kinds;
        for (LayoutID member : membersOf(reader, to.layouts)) {
            const Layout& layout = program->at(member);
            if (layout.kind == LayoutKind::ModuleNamespace)
                kinds.holdsNamespace = true;
            else if (!layout.writtenBuiltinBase || layout.mayBeOrdinaryObject)
                kinds.mayHaveShapes = true;
        }
        return kinds;
    };
    KindsInLayoutSet kinds = context && context->kindsInSetsOutsideThread && !liesInThread(to.layouts) ? context->kindsInSetsOutsideThread->ensure(to.layouts, readKinds).iterator->value : readKinds();
    if (kinds.holdsNamespace) {
        auto knownLayouts = membersReadForTypeTest(reader, from.layouts);
        for (LayoutID member : membersOf(reader, to.layouts)) {
            const Layout& layout = program->at(member);
            if (layout.kind == LayoutKind::ModuleNamespace && (from.anyObject || knownLayouts.contains(member)))
                append({ .kind = CellTestKind::IsNamespaceOf, .module = layout.namespaceOf });
        }
    }
    if (kinds.mayHaveShapes)
        append({ .kind = CellTestKind::ShapeIn, .layouts = to.layouts, .asksForInstance = shapeTestOfTypeTestAsksForInstance(reader, context, from) });
}

static constexpr auto orderOfCellTestKinds = [] {
    std::array<uint8_t, numberOfEnumerators<CellTestKind>> order { };
    uint8_t next = 0;
    for (CellTestKind kind : { CellTestKind::CellTypeIs, CellTestKind::IsObject, CellTestKind::ShapeIn, CellTestKind::IsPristine, CellTestKind::IsBuiltinCell, CellTestKind::IsSymbol, CellTestKind::IsClosureOf, CellTestKind::SharingModeIs, CellTestKind::IsCallable, CellTestKind::InheritsClassInfo, CellTestKind::IsNamespaceOf })
        order[static_cast<size_t>(kind)] = next++;
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(next == order.size());
    return order;
}();

static uint32_t subjectOfCellTest(const CellTest& test)
{
    switch (test.kind) {
    case CellTestKind::CellTypeIs:
        return test.cellTypes.first;
    case CellTestKind::InheritsClassInfo:
        return test.builtinClass.index();
    case CellTestKind::IsBuiltinCell:
        return test.builtin.index();
    case CellTestKind::IsClosureOf:
        return test.function.index();
    case CellTestKind::SharingModeIs:
        return static_cast<uint32_t>(test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return test.module.index();
    case CellTestKind::ShapeIn:
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
    case CellTestKind::IsPristine:
        return 0;
    case CellTestKind::IsSymbol:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool symbolComesEarlierInTypeTest(TypeReader reader, PropertyKeyID a, PropertyKeyID b)
{
    bool firstHasStableNumber = reader.table().hasStableNumber(a);
    bool secondHasStableNumber = reader.table().hasStableNumber(b);
    if (firstHasStableNumber || secondHasStableNumber)
        return firstHasStableNumber && (!secondHasStableNumber || a < b);
    auto contentOf = [](const PropertyKey& key) {
        return std::tie(key.kind, key.text, key.owner, key.symbol, key.module, key.callNumber);
    };
    return contentOf(reader.at(a)) < contentOf(reader.at(b));
}

static bool cellTestComesEarlier(TypeReader reader, const CellTest& a, const CellTest& b)
{
    if (a.kind != b.kind)
        return orderOfCellTestKinds[static_cast<size_t>(a.kind)] < orderOfCellTestKinds[static_cast<size_t>(b.kind)];
    if (a.kind == CellTestKind::IsSymbol)
        return symbolComesEarlierInTypeTest(reader, a.symbol, b.symbol);
    return subjectOfCellTest(a) < subjectOfCellTest(b);
}

static ALWAYS_INLINE void settleVerdictOfTypeTest(TypeTestPlan& plan)
{
    bool everyObjectPasses = plan.objects == ObjectsVerdict::DoNotOccur || plan.objects == ObjectsVerdict::AllPass;
    if (!plan.passingPrimitives && plan.cellTests.isEmpty() && plan.objects != ObjectsVerdict::AllPass)
        plan = { .verdict = TypeTestVerdict::NeverPasses, .from = plan.from, .to = plan.to, .representation = plan.representation };
    else if (!plan.failingPrimitives && !plan.integers && plan.stringLiterals.isEmpty() && everyObjectPasses)
        plan = { .verdict = TypeTestVerdict::AlwaysPasses, .from = plan.from, .to = plan.to, .representation = plan.representation };
}

static void fillTypeTestPlan(TypeTestPlan& plan, TypeReader reader, const TypeTestContext* context, const ObjectsUnderTypeTest& tested, const Type& from, const Type& to)
{
    classifyPrimitivesForTypeTest(plan, reader, from, to);
    plan.objects = objectsVerdictOfTypeTest(tested, from, to);
    appendObjectTestsOfTypeTest(plan, reader, context, tested, from, to);
    AOT::stableSortInLine(plan.cellTests, [&](const CellTest& a, const CellTest& b) {
        return cellTestComesEarlier(reader, a, b);
    });
    settleVerdictOfTypeTest(plan);
}

static void restrictTypeTestToRepresentation(TypeTestPlan& plan, TypeReader reader)
{
    if (plan.verdict != TypeTestVerdict::NeedsTest)
        return;
    Type widest = reader.at(widestTypeOf(plan.representation));
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
    settleVerdictOfTypeTest(plan);
}

TypeTestPlan computeTypeTestPlan(TypeArena& arena, const TypeTestContext& context, TypeID fromID, Representation representation, TypeID toID)
{
    RELEASE_ASSERT(fromID && toID);
    TypeTestPlan plan { .from = fromID, .to = toID, .representation = representation };
    if (arena.isSubset(fromID, toID))
        plan.verdict = TypeTestVerdict::AlwaysPasses;
    else if (!arena.overlap(fromID, toID))
        plan.verdict = TypeTestVerdict::NeverPasses;
    else {
        Type from = arena.at(fromID);
        Type to = arena.at(toID);
        fillTypeTestPlan(plan, arena, &context, ObjectsUnderTypeTest(arena, fromID, toID), from, to);
        restrictTypeTestToRepresentation(plan, arena);
    }
    return plan;
}

bool recordInTablesTellsType(TypeArena& types, TypeID type)
{
    if (!type || liesInThread(type))
        return false;
    if ((holdsOnlyPrimitives(type) ? primitivesInID(type) : types.recordOf(type).primitives) & Primitive::Unspecified)
        return false;
    if (holdsOnlyPrimitives(type))
        return true;
    return types.recordOf(type).isJavaScriptValue() && types.testablePartOf(type) == type;
}

TypeTestPlan computeTypeTestPlanFromAnything(TypeReader reader, const Type& to)
{
    TypeTestPlan plan { .from = TypeTable::anything };
    fillTypeTestPlan(plan, reader, nullptr, { }, reader.at(TypeTable::anything), to);
    return plan;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

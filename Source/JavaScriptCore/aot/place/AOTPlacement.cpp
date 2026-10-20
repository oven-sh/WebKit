/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacement.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTPhaseTimes.h"
#include "AOTPlacementSteps.h"
#include "AOTRegExpRequests.h"
#include <algorithm>
#include <functional>
#include <tuple>
#include <type_traits>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

Placement::Placement(PlacementParts&& parts)
    : m_slots(parts.slots)
    , m_shapes(WTF::move(parts.shapes))
    , m_fieldAccesses(WTF::move(parts.fieldAccesses))
    , m_allocations(WTF::move(parts.allocations))
    , m_closureLayout(parts.closureLayout)
    , m_keyOrders(WTF::move(parts.keyOrders))
    , m_statics(WTF::move(parts.statics))
    , m_guardBits(WTF::move(parts.guardBits))
    , m_constants(WTF::move(parts.constants))
    , m_functions(WTF::move(parts.functions))
    , m_typesNeededAtRunTime(WTF::move(parts.typesNeededAtRunTime))
    , m_numberOfRegExpRecords(parts.numberOfRegExpRecords)
{
}

void Placement::setResolvedSites(ResolvedSites&& sites)
{
    m_requests = WTF::move(sites.requests);
    m_propertyRuns = WTF::move(sites.propertyRuns);
    m_stringConstantsInTables = WTF::move(sites.stringConstantsInTables);
}

template<typename Element, typename Key>
static std::optional<uint32_t> findPosition(const Vector<Element>& ascending, Key key, const auto& keyOf)
{
    auto* found = tryBinarySearch<const Element, Key>(ascending, ascending.size(), key, [&](const Element* element) {
        return std::invoke(keyOf, *element);
    });
    if (!found)
        return std::nullopt;
    return static_cast<uint32_t>(found - ascending.begin());
}

FieldAccessID Placement::findFieldAccess(PropertyKeyID key, LayoutSetID receivers, bool isStore) const
{
    auto position = findPosition(m_fieldAccesses, SlotAccessKey { key, receivers, isStore }, [](const FieldAccess& access) {
        return SlotAccessKey { access.key, access.receivers, access.isStore };
    });
    return position ? FieldAccessID(*position) : FieldAccessID();
}

std::optional<uint32_t> Placement::moduleBuiltinCellOffset(BuiltinID builtin, ModuleID module) const
{
    auto& cells = m_statics.moduleBuiltinCells;
    auto position = findPosition(cells, ModuleBuiltinUse { builtin, module }, [](const ModuleBuiltinCell& cell) {
        return ModuleBuiltinUse { cell.builtin, cell.module };
    });
    if (!position)
        return std::nullopt;
    return cells[*position].offset;
}

std::optional<uint32_t> Placement::builtinCell(BuiltinCellKey key) const
{
    return findPosition(m_constants.builtinCells, key.encoded(), &BuiltinCellKey::encoded);
}

std::optional<uint32_t> Placement::bigIntConstant(StringID digits) const
{
    return findPosition(m_constants.bigIntConstants, digits, std::identity { });
}

std::optional<uint32_t> Placement::requireEdge(ModuleID referrer, ModuleID target) const
{
    return findPosition(m_constants.requireEdges, RequireEdge { referrer, target }, std::identity { });
}

AllocationID AllocationTable::find(LayoutID layout, uint32_t shape, std::span<const FieldIndex> initialFields) const
{
    for (AllocationID id : ofLayout(layout)) {
        const PlacedAllocation& allocation = allocations[id.index()];
        if (allocation.shape == shape && equalSpans(allocation.initialFields.span(), initialFields))
            return id;
    }
    return { };
}

std::optional<uint32_t> KeyOrders::findOrderWord(LayoutID layout, std::span<const FieldIndex> fields) const
{
    using FieldList = std::span<const FieldIndex>;
    if (fields.empty())
        return orderWordOf(0, 0);
    auto orders = of(layout);
    auto isLexicographicallyLess = [](FieldList a, FieldList b) {
        return std::ranges::lexicographical_compare(a, b);
    };
    auto order = std::ranges::lower_bound(orders, fields, isLexicographicallyLess, &Vector<FieldIndex>::span);
    if (order == orders.end() || !spanHasPrefix(order->span(), fields))
        return std::nullopt;
    return orderWordOf(static_cast<uint32_t>(order - orders.begin()), static_cast<uint32_t>(fields.size()));
}

std::span<const KeyOrderContinuation> KeyOrders::continuationsCreating(LayoutID layout, std::span<const FieldIndex> fields) const
{
    auto isBefore = [&](const PlacedFieldCreation& creation) {
        if (creation.layout != layout)
            return creation.layout < layout;
        return std::ranges::lexicographical_compare(creation.fields, fields);
    };
    auto found = std::ranges::partition_point(fieldCreations, isBefore);
    if (found == fieldCreations.end() || found->layout != layout || !std::ranges::equal(found->fields, fields))
        return { };
    return found->continuations.span();
}

GuardBitID GuardBitTable::find(LayoutID classLayout, ClassSide side, MemberGuardKind kind, PropertyKeyID key) const
{
    using GuardBitKey = std::tuple<LayoutID, ClassSide, MemberGuardKind, PropertyKeyID>;
    auto position = findPosition(bits, GuardBitKey { classLayout, side, kind, key }, [](const PlacedGuardBit& bit) {
        return GuardBitKey { bit.classLayout, bit.side, bit.kind, bit.key };
    });
    return position ? GuardBitID(*position) : GuardBitID();
}

std::span<const PlacedGuardBit> GuardBitTable::ofClass(LayoutID classLayout) const
{
    return std::span<const PlacedGuardBit>(std::ranges::equal_range(bits, classLayout, { }, &PlacedGuardBit::classLayout));
}

namespace {

struct ShapeTestCounts {
    void add(const ShapeTest& test)
    {
        switch (test.form) {
        case ShapeTestForm::NeverPasses:
            ++neverPasses;
            return;
        case ShapeTestForm::AlwaysPasses:
        case ShapeTestForm::OneShape:
        case ShapeTestForm::UpTo:
        case ShapeTestForm::From:
        case ShapeTestForm::Range:
        case ShapeTestForm::Block:
        case ShapeTestForm::BlockOrEarlier:
            ++ranges;
            return;
        case ShapeTestForm::ImmediateBits:
        case ShapeTestForm::BitSet:
            ++bitSets;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    uint64_t neverPasses { 0 };
    uint64_t ranges { 0 };
    uint64_t bitSets { 0 };
};

class StepRunner {
public:
    explicit StepRunner(BuildContext& build)
        : m_build(build)
        , m_phaseTimes(build, PassID::NumberAndEncode)
    {
    }

    bool hasFailed() const { return m_build.diagnostics().hasErrors(); }

    template<typename Step>
    auto runStep(ASCIILiteral name, const Step& step)
    {
        using Result = std::invoke_result_t<const Step&>;
        if (hasFailed())
            return Result();
        auto didFinish = makeScopeExit([&] {
            m_phaseTimes.didFinish(name);
        });
        return step();
    }

private:
    BuildContext& m_build;
    PhaseTimes m_phaseTimes;
};

} // anonymous namespace

static void countStatistics(const Placement& placement, const PlacedSites& sites, Statistics& statistics)
{
    const ShapeTable& shapes = placement.shapes();
    auto& tests = sites.shapeTests;
    ShapeTestCounts shapeTests;
    for (size_t index = 0; index < tests.size(); ++index) {
        if (index && tests[index].tested == tests[index - 1].tested)
            continue;
        shapeTests.add(shapes.testOf(tests[index].tested));
    }

    statistics.count(StatisticID::Shapes, shapes.shapeCountInRange(1, shapes.numberOfShapes()));
    statistics.count(StatisticID::ShapeTestNeverPasses, shapeTests.neverPasses);
    statistics.count(StatisticID::ShapeTestRange, shapeTests.ranges);
    statistics.count(StatisticID::ShapeTestBitSet, shapeTests.bitSets);
    statistics.count(StatisticID::GuardBitsPlaced, placement.guardBits().bits.size());
    statistics.count(StatisticID::RequestsShared, placement.requests().numberOfSharedRequests());
}

static LayoutID ensureClosureLayout(ProgramEditor& editor)
{
    auto& layouts = editor.program().layouts;
    for (unsigned index = 0; index < layouts.size(); ++index) {
        if (layouts[index].kind == LayoutKind::Closure)
            return LayoutID(index);
    }
    Layout closure;
    closure.kind = LayoutKind::Closure;
    return editor.addLayout(WTF::move(closure));
}

Placement numberAndEncode(ProgramEditor& editor, const PlacementInputs& inputs, BuildContext& build)
{
    StepRunner steps(build);
    LayoutID closureLayout = ensureClosureLayout(editor);
    const Program& program = editor.program();
    CellSizeClasses sizeClasses(build.options().sizeClassProgression);
    PlacementContext context { program, inputs, sizeClasses, build, build.recordedOptions().useWideReferences(), closureLayout };
    TypeArena arena(editor.types());

    steps.runStep("checkPlacementInputs"_s, [&] { checkPlacementInputs(context); });
    auto sites = steps.runStep("collectPlacedSites"_s, [&] { return collectPlacedSites(context); });
    auto variants = steps.runStep("chooseShapeVariants"_s, [&] { return chooseShapeVariants(context, sites); });
    auto fieldAccesses = steps.runStep("createFieldAccesses"_s, [&] { return createFieldAccesses(context, sites, arena); });
    auto allocations = steps.runStep("numberAllocations"_s, [&] { return numberAllocations(context, sites); });
    auto [keyOrders, orderWords] = steps.runStep("collectKeyOrders"_s, [&] { return collectKeyOrders(context, sites, allocations); });
    auto typesNeeded = steps.runStep("computeTypesNeededAtRunTime"_s, [&] { return computeTypesNeededAtRunTime(context, sites); });
    auto constants = steps.runStep("numberConstants"_s, [&] { return numberConstants(context, sites, typesNeeded); });
    auto functions = steps.runStep("numberFunctions"_s, [&] { return numberFunctions(context, sites); });
    auto candidates = steps.runStep("collectShapeTestCandidates"_s, [&] { return collectShapeTestCandidates(context, sites, fieldAccesses.span(), variants, typesNeeded); });
    auto numbering = steps.runStep("numberShapes"_s, [&] { return numberShapes(context, candidates, variants); });
    auto shapes = steps.runStep("deriveShapeTests"_s, [&] { return deriveShapeTests(context, sites, numbering, variants, typesNeeded, fieldAccesses); });
    steps.runStep("finishAllocations"_s, [&] { finishAllocations(context, shapes, orderWords, sites.allocations.span(), allocations); });
    auto guardBits = steps.runStep("placeGuardBits"_s, [&] { return placeGuardBits(context, sites); });
    auto [statics, literals] = steps.runStep("placeStatics"_s, [&] { return placeStatics(context, sites, guardBits); });
    auto placement = steps.runStep("createPlacement"_s, [&] {
        return Placement(PlacementParts {
            .slots = &inputs.slots,
            .shapes = WTF::move(shapes),
            .fieldAccesses = WTF::move(fieldAccesses),
            .allocations = WTF::move(allocations),
            .closureLayout = closureLayout,
            .keyOrders = WTF::move(keyOrders),
            .statics = WTF::move(statics),
            .guardBits = WTF::move(guardBits),
            .constants = WTF::move(constants),
            .functions = WTF::move(functions),
            .typesNeededAtRunTime = WTF::move(typesNeeded),
            .numberOfRegExpRecords = static_cast<unsigned>(inputs.regExpRequests.all().size()),
        });
    });
    auto resolvedSites = steps.runStep("resolveSites"_s, [&] { return resolveSites(editor, context, { sites, literals, orderWords, placement }); });
    if (steps.hasFailed())
        return { };
    placement.setResolvedSites(WTF::move(resolvedSites));
    countStatistics(placement, sites, build.statistics());
    build.destroyLater(WTF::move(sites));
    return placement;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

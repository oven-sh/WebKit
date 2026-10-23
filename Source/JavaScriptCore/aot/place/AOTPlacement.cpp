/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacement.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTInterningPhases.h"
#include "AOTPhaseTimes.h"
#include "AOTPlacementSteps.h"
#include "AOTRegExpRequests.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <optional>
#include <tuple>
#include <type_traits>
#include <wtf/Scope.h>
#include <wtf/ScopedLambda.h>
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
    , m_methodTable(WTF::move(parts.methodTable))
    , m_globalBindings(WTF::move(parts.globalBindings))
    , m_constants(WTF::move(parts.constants))
    , m_functions(WTF::move(parts.functions))
    , m_classRanks(WTF::move(parts.classRanks))
    , m_layoutsOutOfUse(WTF::move(parts.layoutsOutOfUse))
    , m_typesNeededAtRunTime(WTF::move(parts.typesNeededAtRunTime))
    , m_typesWithCells(WTF::move(parts.typesWithCells))
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

AllocationID AllocationTable::find(LayoutID layout, uint32_t shape, std::span<const FieldIndex> initialFields, bool holdsInnerObjects) const
{
    for (AllocationID id : ofLayout(layout)) {
        const PlacedAllocation& allocation = allocations[id.index()];
        if (allocation.shape == shape && allocation.holdsInnerObjects == holdsInnerObjects && equalSpans(allocation.initialFields.span(), initialFields))
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

std::optional<uint32_t> KeyOrders::numberOfStoreThatMayAdd(FunctionID function, uint32_t storesInFront) const
{
    if (function.index() + 1 >= firstStoreThatMayAdd.size())
        return std::nullopt;
    uint32_t first = firstStoreThatMayAdd[function.index()];
    if (storesInFront >= firstStoreThatMayAdd[function.index() + 1] - first)
        return std::nullopt;
    return first + storesInFront;
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

// Steps run beside each other, so each has a clock of its own.
class StepRunner {
public:
    explicit StepRunner(BuildContext& build)
        : m_build(build)
    {
    }

    bool hasFailed() const { return m_build.diagnostics().hasErrors(); }

    template<typename Step>
    auto runStep(ASCIILiteral name, const Step& step) const
    {
        using Result = std::invoke_result_t<const Step&>;
        if (hasFailed())
            return Result();
        PhaseTimes times(m_build, PassID::NumberAndEncode);
        auto didFinish = makeScopeExit([&] {
            times.didFinish(name);
        });
        return step();
    }

    // Each chain is a sequence of steps that reads nothing another chain writes.
    void runBeside(std::initializer_list<ScopedLambda<void()>> chains) const
    {
        if (!hasFailed())
            runTasksInParallel(m_build.parallelLoops(), chains);
    }

private:
    BuildContext& m_build;
};

struct ShapeInputs {
    Vector<FieldAccess> fieldAccesses;
    ShapeVariants variants;
    BitVector typesNeededAtRunTime;
    BitVector typesWithCells;
    // The numbers of the shapes do not depend on which layouts are out of use.
    BitVector typesThatOrderShapes;
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

// encodeLayoutRecords gives every layout of kind Instance that is in use a class record, in the order of the layouts.
static Vector<std::optional<uint32_t>> rankClasses(const Program& program, const BitVector& layoutsOutOfUse)
{
    Vector<std::optional<uint32_t>> ranks(program.layouts.size());
    uint32_t numberOfClasses = 0;
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Instance && !layoutsOutOfUse.get(index))
            ranks[index] = numberOfClasses++;
    }
    return ranks;
}

// A task is the same rows at any number of threads, and the phase gives out the final ids in the order of the tasks.
static constexpr size_t fieldAccessesPerTask = 256;

static unsigned numberOfFieldAccessTasks(size_t numberOfFieldAccesses)
{
    return static_cast<unsigned>((numberOfFieldAccesses + fieldAccessesPerTask - 1) / fieldAccessesPerTask);
}

template<typename T>
static std::span<T> fieldAccessesOfTask(std::span<T> all, unsigned task)
{
    size_t first = task * fieldAccessesPerTask;
    return all.subspan(first, std::min(fieldAccessesPerTask, all.size() - first));
}

static Vector<FieldAccess> createFieldAccessesInTasks(const PlacementContext& context, const PlacedSites& sites, TypeInterningPhase& phase)
{
    Vector<FieldAccess> rows(sites.fieldAccesses.size());
    forEachIndexInParallel(context.build.parallelLoops(), numberOfFieldAccessTasks(rows.size()), [&](unsigned task) {
        TypeArena arena(phase, task);
        PlacedSites sitesOfTask;
        sitesOfTask.fieldAccesses = Vector<FieldAccessUse>(fieldAccessesOfTask(sites.fieldAccesses.span(), task));
        Vector<FieldAccess> rowsOfTask = createFieldAccesses(context, sitesOfTask, arena);
        std::ranges::move(rowsOfTask, fieldAccessesOfTask(rows.mutableSpan(), task).begin());
    });
    return rows;
}

static void remapFieldAccesses(const ParallelLoops& loops, const TypeRemap& types, std::span<FieldAccess> rows)
{
    if (types.isIdentity())
        return;
    forEachIndexInParallel(loops, numberOfFieldAccessTasks(rows.size()), [&](unsigned task) {
        if (types.isIdentityFor(task))
            return;
        for (FieldAccess& row : fieldAccessesOfTask(rows, task)) {
            types.apply(row.membersWithField);
            types.apply(row.contents);
        }
    });
}

// The rows of the field access table need new types and layout sets.
static ShapeInputs prepareShapeInputs(const StepRunner& steps, ProgramEditor& editor, const PlacementContext& context, const PlacedSites& sites, NOESCAPE const ScopedLambda<void()>& chainBeside)
{
    ShapeInputs result;
    if (steps.hasFailed())
        return result;

    auto phase = makeUniqueWithoutFastMallocCheck<TypeInterningPhase>(editor.types(), numberOfFieldAccessTasks(sites.fieldAccesses.size()));
    steps.runBeside({
        [&] { result.fieldAccesses = steps.runStep("createFieldAccesses"_s, [&] { return createFieldAccessesInTasks(context, sites, *phase); }); },
        [&] {
            steps.runStep("computeTypesNeededAtRunTime"_s, [&] {
                result.typesNeededAtRunTime = computeTypesNeededAtRunTime(context, sites, TypesTestedByCode::Exclude);
                result.typesWithCells = computeTypesNeededAtRunTime(context, sites, TypesTestedByCode::Include);
                result.typesThatOrderShapes = sites.layoutsOutOfUse.isEmpty() ? result.typesNeededAtRunTime : computeTypesNeededAtRunTime(context, sites, TypesTestedByCode::Exclude, LayoutsOutOfUse::Include);
            });
        },
        [&] { result.variants = steps.runStep("chooseShapeVariants"_s, [&] { return chooseShapeVariants(context, sites); }); },
        [&] { chainBeside(); },
    });
    TypeRemap types = phase->finish(StringRemap(), &context.build.parallelLoops());
    context.build.destroyLater(WTF::move(phase));
    remapFieldAccesses(context.build.parallelLoops(), types, result.fieldAccesses.mutableSpan());
    return result;
}

Placement numberAndEncode(ProgramEditor& editor, const PlacementInputs& inputs, BuildContext& build)
{
    return numberAndEncode(editor, inputs, build, [] { });
}

struct PlacementInSteps::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(const PlacementInputs& inputs, BuildContext& build, BodyScansForPlacement* scans)
        : steps(build)
        , inputs(inputs)
        , build(build)
        , scans(scans)
        , sizeClasses(build.options().sizeClassProgression)
    {
    }

    StepRunner steps;
    const PlacementInputs& inputs;
    BuildContext& build;
    BodyScansForPlacement* scans;
    CellSizeClasses sizeClasses;
    std::optional<PlacementContext> context;
    GlobalBindingTable globalBindings;
    PlacedSites sites;
    ShapeVariants variants;
    BitVector typesNeeded;
    BitVector typesWithCells;
    BitVector typesThatOrderShapes;
    LiteralSites literals;
    MethodTableFunctions methodTableFunctions;
    PlacedOrderWords orderWords;
    // By function: see resolveFieldAccesses().
    Vector<Vector<ValueID>> valuesWithOtherWords;
};

PlacementInSteps::PlacementInSteps(const PlacementInputs& inputs, BuildContext& build, BodyScansForPlacement* scans)
    : m_state(makeUnique<State>(inputs, build, scans))
{
}

PlacementInSteps::~PlacementInSteps() = default;

Placement PlacementInSteps::numberWhatRepresentationsAsk(ProgramEditor& editor, NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites)
{
    State& state = *m_state;
    const StepRunner& steps = state.steps;
    BodyScansForPlacement* scans = state.scans;
    LayoutID closureLayout = ensureClosureLayout(editor);
    const Program& program = editor.program();
    state.context.emplace(PlacementContext { program, state.inputs, state.sizeClasses, state.build, state.build.recordedOptions().useWideReferences(), closureLayout });
    const PlacementContext& context = *state.context;

    state.globalBindings = steps.runStep("placeGlobalBindings"_s, [&] {
        TypeArena arena(editor.types());
        if (!scans)
            return placeGlobalBindings(editor, context, arena);
        auto sitesOf = [&](FunctionID id) -> const Vector<GlobalBindingSite>& {
            return scans->globalBindingSitesOf(id);
        };
        GlobalBindingSitesOfBodies sitesOfBodies(sitesOf);
        Vector<FunctionID> functionsWithOtherOperations;
        GlobalBindingTable table = placeGlobalBindings(editor, context, arena, &sitesOfBodies, &functionsWithOtherOperations);
        for (FunctionID id : functionsWithOtherOperations)
            scans->scan(id);
        return table;
    });

    // The other readers of the bodies end on one thread, long behind the sites. Nothing here waits for what they find.
    PlacedSites& sites = state.sites;
    GuardBitTable guardBits;
    StaticsPlan statics;
    ShapeInputs shapeInputs;
    steps.runBeside({
        [&] { readBodiesBesideCollectingSites(); },
        [&] {
            steps.runBeside({
                [&] { sites = steps.runStep("collectPlacedSites"_s, [&] { return scans ? scans->takeSites(context) : collectPlacedSites(context); }); },
                [&] { state.methodTableFunctions = steps.runStep("collectMethodTableFunctions"_s, [&] { return collectMethodTableFunctions(context); }); },
            });
            // The statics wait for nothing that the rows of the field accesses are made from.
            shapeInputs = prepareShapeInputs(steps, editor, context, sites, [&] {
                guardBits = steps.runStep("placeGuardBits"_s, [&] { return placeGuardBits(context, sites); });
                steps.runStep("placeGuardSets"_s, [&] { placeGuardSets(context, sites, guardBits); });
                std::tie(statics, state.literals) = steps.runStep("placeStatics"_s, [&] { return placeStatics(context, sites, guardBits); });
            });
        },
    });
    if (steps.hasFailed())
        return { };
    state.variants = WTF::move(shapeInputs.variants);
    state.typesNeeded = WTF::move(shapeInputs.typesNeededAtRunTime);
    state.typesWithCells = WTF::move(shapeInputs.typesWithCells);
    state.typesThatOrderShapes = WTF::move(shapeInputs.typesThatOrderShapes);
    return Placement(PlacementParts {
        .slots = &state.inputs.slots,
        .fieldAccesses = WTF::move(shapeInputs.fieldAccesses),
        .closureLayout = closureLayout,
        .statics = WTF::move(statics),
        .guardBits = WTF::move(guardBits),
    });
}

void PlacementInSteps::numberRest(Placement& placement)
{
    State& state = *m_state;
    const StepRunner& steps = state.steps;
    BodyScansForPlacement* scans = state.scans;
    if (steps.hasFailed())
        return;
    const PlacementContext& context = *state.context;
    const Program& program = context.program;
    PlacedSites& sites = state.sites;
    const BitVector& typesNeeded = state.typesThatOrderShapes;
    Vector<FieldAccess>& fieldAccesses = placement.m_fieldAccesses;
    const GuardBitTable& guardBits = placement.m_guardBits;

    // Numbering the shapes is the longest chain and mostly one thread. Nothing else waits for it but the allocations.
    ShapeTable shapes;
    AllocationTable allocations;
    KeyOrders keyOrders;
    ConstantNumbering constants;
    FunctionNumbering functions;
    Vector<std::optional<uint32_t>> classRanks;
    steps.runBeside({
        [&] {
            auto candidates = steps.runStep("collectShapeTestCandidates"_s, [&] { return collectShapeTestCandidates(context, sites, fieldAccesses.span(), state.variants, typesNeeded); });
            auto numbering = steps.runStep("numberShapes"_s, [&] { return numberShapes(context, candidates, state.variants); });
            shapes = steps.runStep("deriveShapeTests"_s, [&] { return deriveShapeTests(context, sites, numbering, state.variants, typesNeeded, fieldAccesses); });
        },
        [&] {
            allocations = steps.runStep("numberAllocations"_s, [&] { return numberAllocations(context, sites); });
            std::tie(keyOrders, state.orderWords) = steps.runStep("collectKeyOrders"_s, [&] { return collectKeyOrders(context, sites, allocations); });
        },
        [&] {
            constants = steps.runStep("numberConstants"_s, [&] {
                if (!scans)
                    return numberConstants(context, sites, state.typesWithCells);
                auto usesOf = [&](FunctionID id) -> const Vector<KeyCellUse>& {
                    return scans->keyCellUsesOf(id);
                };
                KeyCellUsesOfBodies usesOfBodies(usesOf);
                return numberConstants(context, sites, state.typesWithCells, &usesOfBodies);
            });
        },
        [&] {
            functions = steps.runStep("numberFunctions"_s, [&] { return numberFunctions(context, sites); });
            classRanks = steps.runStep("rankClasses"_s, [&] { return rankClasses(program, sites.layoutsOutOfUse); });
        },
    });
    steps.runStep("finishAllocations"_s, [&] { finishAllocations(context, shapes, state.orderWords, sites.allocations.span(), allocations); });
    auto methodTable = steps.runStep("placeMethodTable"_s, [&] { return placeMethodTable(context, sites, shapes, guardBits, state.methodTableFunctions); });

    placement.m_shapes = WTF::move(shapes);
    placement.m_allocations = WTF::move(allocations);
    placement.m_keyOrders = WTF::move(keyOrders);
    placement.m_methodTable = WTF::move(methodTable);
    placement.m_globalBindings = WTF::move(state.globalBindings);
    placement.m_constants = WTF::move(constants);
    placement.m_functions = WTF::move(functions);
    placement.m_classRanks = WTF::move(classRanks);
    placement.m_layoutsOutOfUse = sites.layoutsOutOfUse;
    placement.m_typesNeededAtRunTime = WTF::move(state.typesNeeded);
    placement.m_typesWithCells = WTF::move(state.typesWithCells);
    placement.m_numberOfRegExpRecords = static_cast<unsigned>(state.inputs.regExpRequests.all().size());
}

void PlacementInSteps::prepareToResolveFieldAccesses()
{
    m_state->valuesWithOtherWords = Vector<Vector<ValueID>>(m_state->context->program.functions.size());
}

void PlacementInSteps::resolveFieldAccessesOf(const Placement& placement, FunctionID id, Function& function)
{
    State& state = *m_state;
    resolveFieldAccesses(*state.context, { state.sites, state.literals, state.orderWords, placement }, id, function, state.valuesWithOtherWords[id.index()]);
}

void PlacementInSteps::resolveSites(ProgramEditor& editor, Placement& placement)
{
    State& state = *m_state;
    const StepRunner& steps = state.steps;
    if (steps.hasFailed()) {
        placement = { };
        return;
    }
    const PlacementContext& context = *state.context;
    SiteResolutionInputs inputs { state.sites, state.literals, state.orderWords, placement };
    auto resolvedSites = steps.runStep("resolveSites"_s, [&] {
        if (state.valuesWithOtherWords.isEmpty())
            return Typed::resolveSites(editor, context, inputs);
        return resolveOtherWords(editor, context, inputs, state.valuesWithOtherWords.span());
    });
    if (steps.hasFailed()) {
        placement = { };
        return;
    }
    placement.setResolvedSites(WTF::move(resolvedSites));
    countStatistics(placement, state.sites, state.build.statistics());
    state.build.destroyLater(WTF::move(state.sites));
    state.build.destroyLater(WTF::move(state.valuesWithOtherWords));
    if (state.scans)
        state.scans->destroyLater();
}

Placement numberAndEncode(ProgramEditor& editor, const PlacementInputs& inputs, BuildContext& build, NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites, BodyScansForPlacement* scans)
{
    PlacementInSteps steps(inputs, build, scans);
    Placement placement = steps.numberWhatRepresentationsAsk(editor, readBodiesBesideCollectingSites);
    steps.numberRest(placement);
    steps.resolveSites(editor, placement);
    return placement;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementTestCommands.h"

#if ENABLE(AOT)

#include "AOTCallGraph.h"
#include "AOTChooseCallingConventions.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTHeat.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPlacementCheck.h"
#include "AOTPlacementSteps.h"
#include "AOTPlacementTestSupport.h"
#include "AOTRegExpRequests.h"
#include "AOTReuseEnvironments.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTTestRunner.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <inttypes.h>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlacementView {
    ASCIILiteral name;
    void (*print)(PrintStream&, const PlacedProgram&);
};

struct SiteCell {
    uint32_t offset { 0 };
    Opcode opcode { Opcode::Undefined };

    friend auto operator<=>(const SiteCell&, const SiteCell&) = default;
};

enum class PlacementMutation : uint8_t {
    FieldAccess,
    Allocation,
    GuardBit,
    Request,
    RegExpCell,
    SiteCache,
    DeferredForm,
    DeferredFormWithoutStore,
    SingletonForm,
    OptionalField,
    FieldType,
    RunTimeAllocation,
    PrivateDefinition,
};

struct NamedMutation {
    ASCIILiteral name;
    PlacementMutation mutation;
};

enum class MutatedValues : uint8_t {
    First,
    All,
};

struct MutationContext {
    IRPrinter& printer;
    ProgramEditor& editor;
    const Placement& placement;
    const SiteCaches& siteCaches;
};

} // anonymous namespace

static constexpr uint32_t numberOfSwappedSiteCaches = 2;

std::unique_ptr<PlacedProgram> runPipelineForPlacementTest(TestCommandContext& context, PassID last)
{
    if (context.form != IRForm::AsRead) {
        context.out.println("error: this command starts from AsRead"_s);
        return nullptr;
    }

    auto placed = makeUnique<PlacedProgram>();
    placed->output = runPipelineUntil(last, WTF::move(context.program), context.build);
    if (!placed->output) {
        reopenDiagnostics(context.build);
        return nullptr;
    }
    placed->callGraph = buildCallGraph(placed->program(), *placed->output->flow, context.build);
    return placed;
}

PlacementInputs PlacedProgram::inputs() const
{
    return {
        .flow = *output->flow,
        .callGraph = callGraph,
        .heat = *output->heat,
        .callingConventions = *output->callingConventions,
        .frameAllocation = *output->frameAllocation,
        .environmentReuse = *output->environmentReuse,
        .literalStrings = *output->literalStrings,
        .constantCollections = *output->constantCollections,
        .slots = *output->slots,
        .regExpRequests = *output->regExpRequests,
    };
}

static void returnProgram(TestCommandContext& context, PlacedProgram& placed)
{
    context.program = WTF::move(placed.output->program);
    context.form = context.program->form();
}

static void forEachValue(const Program& program, NOESCAPE const Invocable<void(const Value&)> auto& functor)
{
    for (const Function& function : program.functions) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                functor(function.at(id));
        }
    }
}

static void printNumbers(PrintStream& out, const auto& numbers)
{
    printList(out, numbers, [&](uint32_t number) {
        out.print(number);
    });
}

static bool isCompiledObject(const Placement& placement, LayoutID id)
{
    return placement.layout(id).storage == LayoutStorage::CompiledObject;
}

static void printLayoutShapes(IRPrinter& printer, const ShapeTable& shapes, LayoutID id)
{
    ShapeRange range = shapes.shapesOf(id);
    if (range.isEmpty())
        return;

    PrintStream& out = printer.out();
    printer.print(id);
    out.println(" shapes "_s, range.first, ".."_s, range.end() - 1);
    for (uint32_t number = range.first; number < range.end(); ++number) {
        const PlacedShape& shape = shapes.shape(number);
        out.print("  shape "_s, number, shape.keysAreNeverEnumerated ? " unordered"_s : " ordered"_s, " impossible="_s);
        printer.print(shape.impossibleMutations);
        out.print("\n"_s);
    }
}

static void printTestWithCost(PrintStream& out, const ShapeTest& test)
{
    out.print(" "_s);
    printShapeTest(out, test);
    out.println(" cost="_s, instructionsFor(test));
}

static void printShapes(PrintStream& out, const PlacedProgram& placed)
{
    const Program& program = placed.program();
    const ShapeTable& shapes = placed.placement().shapes();
    IRPrinter printer(out, program);
    for (uint32_t index = 0; index < program.layouts.size(); ++index)
        printLayoutShapes(printer, shapes, LayoutID(index));
    out.println("environments from "_s, shapes.firstEnvironmentShape());

    for (size_t index : collectSetsNamedBySites(program)) {
        LayoutSetID set(index);
        out.print("set "_s);
        printer.print(set);
        printTestWithCost(out, shapes.testOf(set));
    }
    for (const ShapeTestWithinKnownSet& pair : shapes.testsWithinKnownSets()) {
        out.print("set "_s);
        printer.print(pair.tested);
        out.print(" within "_s);
        printer.print(pair.known);
        printTestWithCost(out, pair.test);
    }
}

static void printAccesses(PrintStream& out, const PlacedProgram& placed)
{
    const Placement& placement = placed.placement();
    IRPrinter printer(out, placed.program());
    for (uint32_t index = 0; index < placement.numberOfFieldAccesses(); ++index) {
        const FieldAccess& access = placement.fieldAccess(FieldAccessID(index));
        out.print("access "_s, index, " "_s);
        printer.print(access.key);
        out.print(" "_s);
        printer.print(access.receivers);
        out.print(access.isStore ? " store "_s : " read "_s, nameOf(access.coverage), " "_s);
        printSlot(out, access.slot);
        if (access.findsOffsetByShape)
            out.print(" offsetByShape"_s);
        if (access.slot.needsExtension()) {
            out.print(" extension="_s);
            printSlot(out, access.extensionWord);
        }
        out.print(" withField="_s);
        printer.print(access.membersWithField);
        out.print(" test="_s);
        printShapeTest(out, access.membersWithFieldTest);
        out.print(" contents="_s);
        printer.print(access.contents);
        out.print("\n"_s);
    }
}

static void printKeyOrders(PrintStream& out, const PlacedProgram& placed)
{
    const Program& program = placed.program();
    const Placement& placement = placed.placement();
    IRPrinter printer(out, program);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!isCompiledObject(placement, id))
            continue;
        std::span<const Vector<FieldIndex>> orders = placement.keyOrders().of(id);
        printer.print(id);
        out.print(" orders="_s, orders.size(), ":"_s);
        for (const Vector<FieldIndex>& order : orders) {
            out.print(" "_s);
            printNumbers(out, order);
        }
        out.print("\n"_s);
    }
    for (uint32_t index = 0; index < placement.numberOfAllocations(); ++index) {
        const PlacedAllocation& allocation = placement.allocation(AllocationID(index));
        if (!isCompiledObject(placement, allocation.layout))
            continue;
        out.print("allocation "_s, index, " "_s);
        printer.print(allocation.layout);
        out.print(" fields="_s);
        printNumbers(out, allocation.initialFields);
        out.println(" order="_s, orderOf(allocation.orderWord), " count="_s, countOf(allocation.orderWord));
    }
}

static void printKeyOrderContinuations(PrintStream& out, const PlacedProgram& placed)
{
    IRPrinter printer(out, placed.program());
    for (const PlacedFieldCreation& creation : placed.placement().keyOrders().fieldCreations) {
        printer.print(creation.layout);
        out.print(" creating "_s);
        printNumbers(out, creation.fields);
        out.print(":"_s);
        if (creation.continuations.isEmpty())
            out.print(" none"_s);
        for (KeyOrderContinuation continuation : creation.continuations) {
            out.print(" order="_s, orderOf(continuation.orderWordBefore), " count="_s, countOf(continuation.orderWordBefore));
            out.print(" -> order="_s, orderOf(continuation.orderWordAfter), " count="_s, countOf(continuation.orderWordAfter), ";"_s);
        }
        out.print("\n"_s);
    }
}

static void printAllocations(PrintStream& out, const PlacedProgram& placed)
{
    const Placement& placement = placed.placement();
    IRPrinter printer(out, placed.program());
    for (uint32_t index = 0; index < placement.numberOfAllocations(); ++index) {
        const PlacedAllocation& allocation = placement.allocation(AllocationID(index));
        out.print("allocation "_s, index, " "_s);
        printer.print(allocation.layout);
        out.print(" shape="_s, allocation.shape, " fields="_s);
        printNumbers(out, allocation.initialFields);
        out.print(" size="_s, allocation.size);
        out.printf(" order=0x%x", allocation.orderWord);
        if (allocation.layout == placement.closureLayout())
            out.print(" captures="_s, allocation.numberOfCaptures);
        if (allocation.constructedBy) {
            out.print(" constructedBy="_s);
            printer.print(allocation.constructedBy);
        }
        out.print("\n"_s);
    }
}

static void printGuards(PrintStream& out, const PlacedProgram& placed)
{
    IRPrinter printer(out, placed.program());
    const GuardBitTable& table = placed.placement().guardBits();
    std::span<const PlacedGuardBit> guards = table.bits.span();
    for (size_t index = 0; index < guards.size(); ++index) {
        const PlacedGuardBit& guard = guards[index];
        out.print("guard "_s, index, " "_s);
        if (guard.classLayout) {
            printer.print(guard.classLayout);
            out.print(" "_s, nameOf(guard.side), " "_s, nameOf(guard.kind), " "_s);
            printer.print(guard.key);
        } else {
            out.print("set "_s);
            printList(out, table.membersOf(GuardBitID(index)), [&](GuardBitID member) {
                out.print(member.index());
            });
        }
        out.println(" bit="_s, guard.bit, " word@"_s, guard.wordOffset, guard.holdsWhileClear ? " holdsWhileClear"_s : ""_s);
    }
}

static void printVariables(IRPrinter& printer, const Placement& placement)
{
    const Program& program = printer.program();
    PrintStream& out = printer.out();
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        VariableID id(index);
        const PlacedVariable& variable = placement.variable(id);
        if (!variable.hasWordInStatics())
            continue;
        printer.print(id);
        out.print(" offset="_s, variable.offset, " representation="_s, nameOf(variable.representation), " encoding="_s, nameOf(variable.encoding), " region="_s, nameOf(variable.region), " initial="_s);
        out.printf("0x%016" PRIx64, variable.initialWord);
        out.print(" type="_s);
        printer.print(program.at(id).declared.type);
        out.print("\n"_s);
    }
}

static void printModuleValues(IRPrinter& printer, const Placement& placement)
{
    for (uint32_t index = 0; index < printer.program().modules.size(); ++index) {
        ModuleID id(index);
        if (!placement.hasModuleValue(id))
            continue;
        printer.out().print("module-value "_s);
        printer.print(id);
        printer.out().println(" @"_s, placement.moduleValueOffset(id));
    }
}

static void printGuardWords(IRPrinter& printer, const GuardBitTable& guards)
{
    const PlacedGuardBit* previous = nullptr;
    for (const PlacedGuardBit& guard : guards.bits) {
        bool isFirstOfWord = !previous || previous->wordOffset != guard.wordOffset;
        previous = &guard;
        if (!isFirstOfWord)
            continue;
        if (!guard.classLayout) {
            printer.out().println("guard-word sets @"_s, guard.wordOffset);
            continue;
        }
        printer.out().print("guard-word "_s);
        printer.print(guard.classLayout);
        printer.out().println(" "_s, nameOf(guard.side), " @"_s, guard.wordOffset);
    }
}

static void printModuleBuiltinCells(IRPrinter& printer, const StaticsPlan& statics)
{
    for (const ModuleBuiltinCell& cell : statics.moduleBuiltinCells) {
        printer.out().print("module-builtin "_s);
        printer.print(cell.builtin);
        printer.out().print(" "_s);
        printer.print(cell.module);
        printer.out().println(" @"_s, cell.offset);
    }
}

static void printSharedStorage(IRPrinter& printer, const RequestTable& requests)
{
    for (const RequestRecord& record : requests.records()) {
        auto* request = std::get_if<DataRecord>(&record);
        if (!request || !(request->sharedStorageOffset || request->node))
            continue;
        printer.out().print(request->node ? "storage-in-image "_s : "shared-storage "_s);
        printer.print(request->data);
        printer.out().println(" @"_s, request->node ? *request->node : *request->sharedStorageOffset);
    }
}

static void printSiteCells(PrintStream& out, const Program& program, const StaticsPlan& statics)
{
    Vector<SiteCell> cells;
    forEachValue(program, [&](const Value& value) {
        if (!Value::opcodesWithStaticsOffset.contains(value.opcode))
            return;
        if (std::optional<StaticsOffset> offset = value.staticsOffset())
            cells.append(SiteCell { offset->value(), value.opcode });
    });
    AOT::sort(cells);
    removeRepeatedElements(cells);

    for (const SiteCell& cell : cells) {
        out.print("site-cell "_s, nameOf(cell.opcode), " @"_s, cell.offset);
        if (cell.opcode == Opcode::NewArray)
            out.print(" lastArray@"_s, cell.offset + statics.distanceFromArrayProfileToLastArray);
        out.print("\n"_s);
    }
}

static void printFreeCells(IRPrinter& printer, const Placement& placement)
{
    for (uint32_t index = 0; index < printer.program().layouts.size(); ++index) {
        LayoutID id(index);
        std::optional<uint32_t> offset = placement.freeCellOffset(id);
        if (!offset)
            continue;
        printer.out().print("free-cell "_s);
        printer.print(id);
        printer.out().println(" @"_s, *offset);
    }
}

static void printStatics(PrintStream& out, const PlacedProgram& placed)
{
    const Placement& placement = placed.placement();
    const StaticsPlan& statics = placement.statics();
    IRPrinter printer(out, placed.program());
    out.println("statics size="_s, statics.size, " filledAtEntry="_s, statics.sizeFilledAtProgramEntry);
    printVariables(printer, placement);
    printModuleValues(printer, placement);
    printGuardWords(printer, placement.guardBits());
    printModuleBuiltinCells(printer, statics);
    printSharedStorage(printer, placement.requests());
    printSiteCells(out, placed.program(), statics);
    printFreeCells(printer, placement);

    out.print("roots="_s);
    printNumbers(out, statics.roots);
    out.print("\ncleared="_s);
    printList(out, statics.clearedEachCollection, [&](const StaticsRange& range) {
        out.print(range.offset, "+"_s, range.size);
    });
    out.print("\n"_s);
}

static void printRequests(PrintStream& out, const PlacedProgram& placed)
{
    const RequestTable& requests = placed.placement().requests();
    IRPrinter printer(out, placed.program());
    std::span<const RequestRecord> records = requests.records();
    for (unsigned index = 0; index < records.size(); ++index) {
        out.print("request @"_s, requests.offsetOf(index), " "_s);
        printRequestRecord(printer, records[index]);
        out.print("\n"_s);
    }
    out.println("requests size="_s, requests.sizeInBytes(), " shared="_s, requests.numberOfSharedRequests());

    std::span<const PlacedPropertyRun> runs = placed.placement().propertyRuns().runs();
    for (size_t index = 0; index < runs.size(); ++index) {
        out.print("run "_s, index, " "_s);
        printList(out, runs[index].keys, [&](PropertyKeyID key) {
            printer.print(key);
        });
        out.print("\n"_s);
    }
}

static void printSiteCaches(PrintStream& out, const PlacedProgram& placed)
{
    placed.siteCaches().dump(out, placed.program());
}

static void printPlacedProgram(PrintStream& out, const PlacedProgram& placed)
{
    printProgram(out, placed.program());
}

static std::span<const PlacementView> placementViews()
{
    static constexpr PlacementView views[] {
        { "shapes"_s, printShapes },
        { "accesses"_s, printAccesses },
        { "key-orders"_s, printKeyOrders },
        { "key-order-continuations"_s, printKeyOrderContinuations },
        { "allocations"_s, printAllocations },
        { "guards"_s, printGuards },
        { "statics"_s, printStatics },
        { "requests"_s, printRequests },
        { "site-caches"_s, printSiteCaches },
        { "program"_s, printPlacedProgram },
    };
    return views;
}

static void printViews(PrintStream& out, std::span<const String> names, const PlacedProgram& placed)
{
    for (const String& name : names) {
        if (!findView(placementViews(), name)) {
            printBadArgument(out, name);
            return;
        }
    }
    if (names.empty()) {
        placed.placement().dump(out, placed.program());
        return;
    }
    for (const String& name : names)
        findView(placementViews(), name)->print(out, placed);
}

static void runPlace(TestCommandContext& context)
{
    std::unique_ptr<PlacedProgram> placed = runPipelineForPlacementTest(context, PassID::CheckPlacement);
    if (!placed)
        return;
    printViews(context.out, context.arguments, *placed);
    returnProgram(context, *placed);
}

static std::optional<PlacementMutation> parseMutation(StringView word)
{
    static constexpr NamedMutation mutations[] {
        { "field-access"_s, PlacementMutation::FieldAccess },
        { "allocation"_s, PlacementMutation::Allocation },
        { "guard-bit"_s, PlacementMutation::GuardBit },
        { "request"_s, PlacementMutation::Request },
        { "regexp-cell"_s, PlacementMutation::RegExpCell },
        { "site-cache"_s, PlacementMutation::SiteCache },
        { "deferred-form"_s, PlacementMutation::DeferredForm },
        { "deferred-form-without-store"_s, PlacementMutation::DeferredFormWithoutStore },
        { "singleton-form"_s, PlacementMutation::SingletonForm },
        { "optional-field"_s, PlacementMutation::OptionalField },
        { "field-type"_s, PlacementMutation::FieldType },
        { "run-time-allocation"_s, PlacementMutation::RunTimeAllocation },
        { "private-definition"_s, PlacementMutation::PrivateDefinition },
    };
    const NamedMutation* found = findView(std::span<const NamedMutation> { mutations }, word);
    if (!found)
        return std::nullopt;
    return found->mutation;
}

template<typename IDType>
static void forEachReferenceOfType(Value& value, NOESCAPE const Invocable<void(IDType&)> auto& functor)
{
    forEachReference(value, [&](auto& reference) {
        if constexpr (std::is_same_v<decltype(reference), IDType&>)
            functor(reference);
    });
}

template<typename IDType>
static bool advancePayload(Value& value, uint32_t numberOfIDs)
{
    bool didAdvance = false;
    forEachReferenceOfType<IDType>(value, [&](IDType& id) {
        if (numberOfIDs < 2 || didAdvance)
            return;
        id = IDType((id.index() + 1) % numberOfIDs);
        didAdvance = true;
    });
    return didAdvance;
}

static bool advanceRequest(Value& value, const RequestTable& requests)
{
    if (!Value::opcodesWithRequest.contains(value.opcode) || namesPropertyRun(value.opcode))
        return false;
    std::optional<Request> request = value.request();
    auto numberOfRecords = static_cast<unsigned>(requests.records().size());
    if (!request || numberOfRecords < 2)
        return false;

    for (unsigned index = 0; index < numberOfRecords; ++index) {
        if (requests.offsetOf(index) != request->value())
            continue;
        value.setRequest(Request(requests.offsetOf((index + 1) % numberOfRecords)));
        return true;
    }
    return false;
}

static bool changeRegExpCell(const Program& program, Value& value)
{
    if (value.opcode != Opcode::SharedRegExpLiteral)
        return false;
    std::optional<StaticsOffset> cell = value.staticsOffset();
    if (!cell)
        return false;

    std::optional<StaticsOffset> otherCell;
    forEachValue(program, [&](const Value& other) {
        if (otherCell || (other.opcode != Opcode::NewRegExp && other.opcode != Opcode::SharedRegExpLiteral))
            return;
        std::optional<StaticsOffset> candidate = other.staticsOffset();
        if (candidate && candidate != cell)
            otherCell = candidate;
    });
    if (!otherCell)
        return false;
    value.setStaticsOffset(otherCell);
    return true;
}

static bool swapSiteCaches(Value& value, uint32_t numberOfSiteCaches)
{
    bool didSwap = false;
    forEachReferenceOfType<SiteCacheID>(value, [&](SiteCacheID& cache) {
        uint32_t mirrored = numberOfSiteCaches - 1 - cache.index();
        if (mirrored == cache.index() || std::min(mirrored, cache.index()) >= numberOfSwappedSiteCaches)
            return;
        cache = SiteCacheID(mirrored);
        didSwap = true;
    });
    return didSwap;
}

static bool makeClosureFull(Value& value, ClosureForm form)
{
    if (value.opcode != Opcode::NewClosure || value.closureForm() != form)
        return false;
    value.setClosureForm(ClosureForm::Full);
    return true;
}

static bool makeCreatedClosureDeferred(Value& value)
{
    if (value.opcode != Opcode::NewClosure || (value.closureForm() != ClosureForm::Full && value.closureForm() != ClosureForm::Slim))
        return false;
    value.setClosureForm(ClosureForm::Deferred);
    return true;
}

static bool mutateValues(const MutationContext& context, MutatedValues mutatedValues, NOESCAPE const Invocable<bool(Value&)> auto& mutateValue)
{
    const Program& program = context.editor.program();
    PrintStream& out = context.printer.out();
    bool didMutate = false;
    for (uint32_t functionIndex = 0; functionIndex < program.functions.size(); ++functionIndex) {
        FunctionID id(functionIndex);
        if (!program.at(id).hasBody())
            continue;
        Function& function = context.editor.at(id);
        for (const Block& block : function.blocks) {
            for (ValueID value : block.values) {
                if (!mutateValue(function.edit(value)))
                    continue;
                out.print("mutated "_s);
                context.printer.print(id);
                out.print(" "_s);
                context.printer.print(value);
                out.print("\n"_s);
                if (mutatedValues == MutatedValues::First)
                    return true;
                didMutate = true;
            }
        }
    }
    return didMutate;
}

static bool mutateFirstField(const MutationContext& context, NOESCAPE const Invocable<bool(const Layout&, const PlacedSlot&)> auto& isEligible, NOESCAPE const Invocable<void(Field&)> auto& mutateField)
{
    const Program& program = context.editor.program();
    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        LayoutID id(layoutIndex);
        const Layout& layout = program.at(id);
        for (FieldIndex index = 0; index < layout.fields.size(); ++index) {
            const PlacedSlot& slot = context.placement.slot(id, index);
            if (!slot.hasStorage() || !isEligible(layout, slot))
                continue;
            FieldReference declaration = declarationOf(program, { id, index });
            mutateField(context.editor.at(declaration.layout).fields[declaration.field]);
            context.printer.out().print("mutated "_s);
            context.printer.print(id);
            context.printer.out().println("."_s, index);
            return true;
        }
    }
    return false;
}

static bool hasOrderedKeysAndSlotWithoutAbsentState(const PlannedLayout& planned)
{
    if (planned.storage != LayoutStorage::CompiledObject || planned.keysAreNeverEnumerated)
        return false;
    return std::ranges::any_of(planned.slots.span().first(planned.numberOfFields()), [](const PlacedSlot& slot) {
        return slot.hasStorage() && !canLeaveSlot(slot.representation);
    });
}

static bool makeFirstLiteralLayoutConstructedBy(const MutationContext& context, FunctionID constructor)
{
    const Program& program = context.editor.program();
    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        LayoutID id(layoutIndex);
        if (program.at(id).constructor || !hasOrderedKeysAndSlotWithoutAbsentState(context.placement.layout(id)))
            continue;
        context.editor.at(id).constructor = constructor;
        context.printer.out().print("mutated "_s);
        context.printer.print(id);
        context.printer.out().println();
        return true;
    }
    return false;
}

static bool defineFieldWithoutAbsentState(const Placement& placement, Value& value)
{
    if (value.opcode != Opcode::PrivateDefine)
        return false;
    const PlannedLayout& planned = placement.layout(value.layout());
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (!planned.slots[field].hasStorage() || canLeaveSlot(planned.slots[field].representation))
            continue;
        value.setField(field);
        return true;
    }
    return false;
}

static bool mutate(const MutationContext& context, PlacementMutation mutation)
{
    const Placement& placement = context.placement;
    switch (mutation) {
    case PlacementMutation::FieldAccess:
        return mutateValues(context, MutatedValues::First, [&](Value& value) {
            return advancePayload<FieldAccessID>(value, placement.numberOfFieldAccesses());
        });
    case PlacementMutation::Allocation:
        // A closure fits every closure allocation with room for its captures, so the next one need not be wrong.
        return mutateValues(context, MutatedValues::First, [&](Value& value) {
            return value.opcode != Opcode::NewClosure && advancePayload<AllocationID>(value, placement.numberOfAllocations());
        });
    case PlacementMutation::GuardBit:
        return mutateValues(context, MutatedValues::First, [&](Value& value) {
            return advancePayload<GuardBitID>(value, placement.tableSizes().guardBits);
        });
    case PlacementMutation::Request:
        return mutateValues(context, MutatedValues::First, [&](Value& value) {
            return advanceRequest(value, placement.requests());
        });
    case PlacementMutation::RegExpCell:
        return mutateValues(context, MutatedValues::First, [&](Value& value) {
            return changeRegExpCell(context.editor.program(), value);
        });
    case PlacementMutation::SiteCache:
        return mutateValues(context, MutatedValues::All, [&](Value& value) {
            return swapSiteCaches(value, context.siteCaches.count());
        });
    case PlacementMutation::DeferredForm:
        return mutateValues(context, MutatedValues::First, [](Value& value) {
            return makeClosureFull(value, ClosureForm::Deferred);
        });
    case PlacementMutation::DeferredFormWithoutStore:
        return mutateValues(context, MutatedValues::First, makeCreatedClosureDeferred);
    case PlacementMutation::SingletonForm:
        return mutateValues(context, MutatedValues::First, [](Value& value) {
            return makeClosureFull(value, ClosureForm::Singleton);
        });
    case PlacementMutation::OptionalField:
        return mutateFirstField(context, [](const Layout& layout, const PlacedSlot& slot) {
            return layout.kind != LayoutKind::CoroutineFrame && !canLeaveSlot(slot.representation);
        }, [](Field& field) {
            field.flags.remove(FieldFlags::IsRequired);
            field.flow.occupancy = Occupancy::Unknown;
        });
    case PlacementMutation::FieldType:
        return mutateFirstField(context, [](const Layout&, const PlacedSlot& slot) {
            return slot.representation == SlotRepresentation::Int32;
        }, [](Field& field) {
            field.declared.type = TypeTable::number;
        });
    case PlacementMutation::RunTimeAllocation:
        return makeFirstLiteralLayoutConstructedBy(context, context.editor.program().at(ModuleID(0u)).body);
    case PlacementMutation::PrivateDefinition:
        return mutateValues(context, MutatedValues::First, [&](Value& value) {
            return defineFieldWithoutAbsentState(placement, value);
        });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printFailedRules(TestCommandContext& context, const Program& program)
{
    Diagnostics reported = context.build.takeDiagnostics();
    reported.finish(&program);
    for (const InternalError& error : reported.internalErrors()) {
        if (error.kind == InternalErrorKind::PlacementCheckFailed)
            context.out.println("rule "_s, nameOf(static_cast<PlacementRule>(error.count)));
    }
    context.build.diagnostics().append(Vector<UserDiagnostic>(reported.userDiagnostics()));
    context.build.diagnostics().append(Vector<InternalError>(reported.internalErrors()));
}

static void checkMutatedPlacement(TestCommandContext& context, PlacedProgram& placed)
{
    if (context.build.diagnostics().hasErrors()) {
        context.out.println("error: checkPlacement failed before the mutation"_s);
        return;
    }

    std::optional<PlacementMutation> mutation;
    if (context.arguments.size() == 1)
        mutation = parseMutation(context.arguments.front());
    if (!mutation) {
        printBadArgument(context.out, context.arguments.empty() ? StringView() : StringView(context.arguments.back()));
        return;
    }

    IRPrinter printer(context.out, placed.program());
    ProgramEditor editor(placed.program());
    if (!mutate({ printer, editor, placed.placement(), placed.siteCaches() }, *mutation)) {
        context.out.println("error: nothing to mutate"_s);
        return;
    }
    checkPlacement(placed.program(), placed.inputs(), placed.placement(), placed.siteCaches(), context.build);
    printFailedRules(context, placed.program());
}

static void runCheckPlacement(TestCommandContext& context)
{
    std::unique_ptr<PlacedProgram> placed = runPipelineForPlacementTest(context, PassID::CheckPlacement);
    if (!placed)
        return;
    checkMutatedPlacement(context, *placed);
    returnProgram(context, *placed);
}

std::span<const TestCommand> placementTestCommands()
{
    static constexpr TestCommand commands[] {
        { "plan-slots"_s, IRForm::AsRead, runPlanSlots, RawWordCheck::Run, ProgramAsRead::WithLibrary },
        { "place"_s, IRForm::AsRead, runPlace, RawWordCheck::Run, ProgramAsRead::WithLibrary },
        { "check-placement"_s, IRForm::AsRead, runCheckPlacement, RawWordCheck::Run, ProgramAsRead::WithLibrary },
        { "tables"_s, IRForm::AsRead, runTables, RawWordCheck::Run, ProgramAsRead::WithLibrary },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

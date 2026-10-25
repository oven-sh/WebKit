/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTHeat.h"
#include "AOTPlacementTestSupport.h"
#include "AOTReuseEnvironments.h"
#include "AOTSlotPlanSteps.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <span>
#include <tuple>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SlotPlanViewContext {
    const Program& program() const { return placed.program(); }
    const SlotPlan& slots() const { return placed.slots(); }

    PrintStream& out;
    const PlacedProgram& placed;
    BuildContext& build;
};

struct SlotPlanView {
    ASCIILiteral name;
    void (*print)(const SlotPlanViewContext&);
};

struct AccessRequest {
    PropertyKeyID key;
    LayoutSetID receivers;
    bool isRead { false };
    bool isStore { false };
    uint32_t sites { 0 };
    SiteWeight weight { 0 };
};

} // anonymous namespace

static ASCIILiteral nameOf(LayoutStorage storage)
{
    switch (storage) {
    case LayoutStorage::None:
        return "None"_s;
    case LayoutStorage::CompiledObject:
        return "CompiledObject"_s;
    case LayoutStorage::OrdinaryObject:
        return "OrdinaryObject"_s;
    case LayoutStorage::Environment:
        return "Environment"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(VariableStorage storage)
{
    switch (storage) {
    case VariableStorage::Statics:
        return "Statics"_s;
    case VariableStorage::None:
        return "None"_s;
    case VariableStorage::ClosureTable:
        return "ClosureTable"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(MemberCreation creation)
{
    switch (creation) {
    case MemberCreation::AtDefinition:
        return "AtDefinition"_s;
    case MemberCreation::ByClassRecord:
        return "ByClassRecord"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printFlag(PrintStream& out, bool isSet, ASCIILiteral name)
{
    if (isSet)
        out.print(" "_s, name);
}

static void printLayoutIDs(IRPrinter& printer, std::span<const LayoutID> layouts)
{
    PrintStream& out = printer.out();
    out.print("{"_s);
    CommaPrinter space(" "_s);
    for (LayoutID layout : layouts) {
        out.print(space);
        printer.print(layout);
    }
    out.print("}"_s);
}

static void finishLineWithSlot(PrintStream& out, const PlacedSlot& slot)
{
    out.print(" "_s);
    printSlot(out, slot);
    out.print("\n"_s);
}

static SlotUses collectSlotUsesFor(const SlotPlanViewContext& view)
{
    const Program& program = view.program();
    const PipelineOutput& output = *view.placed.output;
    SlotPlanInputs inputs {
        .flow = *output.flow,
        .callGraph = view.placed.callGraph,
        .heat = *output.heat,
        .frameAllocation = *output.frameAllocation,
        .environmentReuse = *output.environmentReuse,
    };
    CellSizeClasses sizeClasses(view.build.options().sizeClassProgression);
    SlotRows rows(program);
    Vector<LayoutStorage> layoutStorage(program.layouts.size(), [&](size_t index) {
        return view.slots().storageOf(LayoutID(index));
    });
    InheritedRows inheritedRows(program, rows, layoutStorage.span());
    SlotPlanContext context { program, inputs, view.build, sizeClasses, rows, layoutStorage.span(), view.build.recordedOptions().useWideReferences(), inheritedRows };
    return collectSlotUses(context);
}

static void printRowsOf(IRPrinter& printer, LayoutID id, const PlannedLayout& layout)
{
    PrintStream& out = printer.out();
    const Vector<Field>& fields = printer.program().at(id).fields;
    for (FieldIndex index = 0; index < layout.numberOfFields(); ++index) {
        if (!layout.slots[index].hasStorage())
            continue;
        out.print("  ."_s, index, " "_s);
        printer.print(index < fields.size() ? fields[index].key : PropertyKeyID());
        finishLineWithSlot(out, layout.slots[index]);
    }
    if (layout.hasExtension()) {
        out.print("  extension"_s);
        finishLineWithSlot(out, layout.slots[layout.extensionWordIndex()]);
    }
    if (layout.hasExtraPropertiesWord()) {
        out.print("  extra-properties"_s);
        finishLineWithSlot(out, layout.slots[layout.extraPropertiesWordIndex()]);
    }
}

static void printLayouts(const SlotPlanViewContext& view)
{
    PrintStream& out = view.out;
    IRPrinter printer(out, view.program());
    for (unsigned index = 0; index < view.slots().numberOfLayouts(); ++index) {
        LayoutID id(index);
        const PlannedLayout& layout = view.slots().layout(id);
        if (layout.storage == LayoutStorage::None)
            continue;
        printer.print(id);
        out.print(" "_s, nameOf(layout.storage), " size="_s, layout.size, " extension="_s, layout.extensionSize);
        printFlag(out, layout.holdsWideReferences, "wide"_s);
        printFlag(out, layout.keysAreNeverEnumerated, "unordered"_s);
        out.print("\n"_s);
        printRowsOf(printer, id, layout);
    }
}

static Vector<AccessRequest> mergeDirections(std::span<const SlotAccessUse> accessesByKey)
{
    Vector<AccessRequest> requests;
    for (const SlotAccessUse& use : accessesByKey) {
        if (requests.isEmpty() || requests.last().key != use.access.key || requests.last().receivers != use.access.receivers)
            requests.append(AccessRequest { .key = use.access.key, .receivers = use.access.receivers });
        AccessRequest& request = requests.last();
        request.isRead |= !use.access.isStore;
        request.isStore |= use.access.isStore;
        request.sites += use.sites;
        request.weight = saturatedSum(request.weight, use.weight);
    }
    return requests;
}

static ASCIILiteral directionOf(const AccessRequest& request)
{
    if (request.isRead && request.isStore)
        return "read+store"_s;
    return request.isStore ? "store"_s : "read"_s;
}

static PlacedSlot locationOf(const SlotGroup& group)
{
    const PlacedSlot& slot = group.slot;
    return { .representation = slot.representation, .storage = slot.storage, .offset = slot.offset };
}

static void printAgreement(IRPrinter& printer, const SlotAgreement& agreement)
{
    PrintStream& out = printer.out();
    if (agreement.hasOneLocation()) {
        out.print("  one location"_s);
        finishLineWithSlot(out, locationOf(agreement.groups[0]));
    } else if (!agreement.groups.isEmpty()) {
        out.print("  "_s, agreement.groups.size(), " groups: "_s);
        CommaPrinter comma;
        for (const SlotGroup& group : agreement.groups) {
            out.print(comma);
            printLayoutIDs(printer, group.members.span());
            out.print(" "_s);
            printSlot(out, locationOf(group));
        }
        out.print("\n"_s);
    }
    if (agreement.coversEveryMember())
        return;
    out.print("  without slot "_s);
    printLayoutIDs(printer, agreement.membersWithoutSlot.span());
    printFlag(out, agreement.membersWithoutSlotKeepBytesZero, "zero"_s);
    out.print("\n"_s);
}

static void printAccesses(const SlotPlanViewContext& view)
{
    PrintStream& out = view.out;
    const Program& program = view.program();
    IRPrinter printer(out, program);
    SlotUses uses = collectSlotUsesFor(view);
    Vector<AccessRequest> requests = mergeDirections(uses.accesses.span());
    AOT::sort(requests, [](const AccessRequest& a, const AccessRequest& b) {
        return std::tuple(b.weight, a.key, a.receivers) < std::tuple(a.weight, b.key, b.receivers);
    });
    for (const AccessRequest& request : requests) {
        std::span<const LayoutID> members;
        if (request.receivers)
            members = program.at(request.receivers).members.span();
        printer.print(request.key);
        out.print(" "_s);
        printLayoutIDs(printer, members);
        out.print(" "_s, directionOf(request), " weight="_s, request.weight, " sites="_s, request.sites, "\n"_s);
        printAgreement(printer, view.slots().agreementOf(program, request.key, members));
    }
}

static void printVariables(const SlotPlanViewContext& view)
{
    PrintStream& out = view.out;
    IRPrinter printer(out, view.program());
    for (unsigned index = 0; index < view.slots().numberOfVariables(); ++index) {
        VariableID id(index);
        PlannedVariable variable = view.slots().variable(id);
        printer.print(id);
        out.print(" "_s, nameOf(variable.storage), " "_s, nameOf(variable.encoding), " "_s, nameOf(variable.representation));
        printFlag(out, variable.isUsedBeforeProgramEntry, "before-entry"_s);
        out.print("\n"_s);
    }
}

static void printClosures(const SlotPlanViewContext& view)
{
    PrintStream& out = view.out;
    const Program& program = view.program();
    IRPrinter printer(out, program);
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;
        FunctionID id(index);
        printer.print(id);
        out.print(" "_s, nameOf(view.slots().closureFormOf(id)), "\n"_s);
    }
}

static void printClass(IRPrinter& printer, LayoutID id, const ClassCreationPlan& plan)
{
    PrintStream& out = printer.out();
    out.print("class "_s);
    printer.print(id);
    printFlag(out, plan.createsMembersLazily, "lazy"_s);
    printFlag(out, plan.createsPrototypeBeforeClassObject, "prototypeFirst"_s);
    printFlag(out, plan.createsClassObjectOnMutation, "onMutation"_s);
    printFlag(out, plan.guardsHoldWhileClear, "guardsHoldWhileClear"_s);
    out.print("\n"_s);

    const Vector<ClassMember>& members = printer.program().at(id).classFlow.members;
    for (size_t index = 0; index < members.size(); ++index) {
        MemberCreation creation = index < plan.members.size() ? plan.members[index] : MemberCreation::AtDefinition;
        out.print("  "_s, nameOf(members[index].side), " "_s);
        if (members[index].key)
            printer.print(members[index].key);
        else
            out.print("computed"_s);
        out.print(" "_s, nameOf(creation), "\n"_s);
    }
}

static void printClasses(const SlotPlanViewContext& view)
{
    const Program& program = view.program();
    IRPrinter printer(view.out, program);
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor)
            continue;
        LayoutID id(index);
        printClass(printer, id, view.slots().classCreation(id));
    }
}

static void printPadding(const SlotPlanViewContext& view)
{
    PrintStream& out = view.out;
    IRPrinter printer(out, view.program());
    for (unsigned index = 0; index < view.slots().numberOfLayouts(); ++index) {
        LayoutID id(index);
        const PlannedLayout& layout = view.slots().layout(id);
        if (!layout.hasSlots())
            continue;
        printer.print(id);
        out.print(" zero "_s);
        printList(out, layout.bytesKeptZero, [&](ReservedBytes bytes) {
            out.print("@"_s, bytes.offset, "+"_s, bytes.width);
        });
        out.print("\n"_s);
    }
}

static void printColumns(const SlotPlanViewContext& view)
{
    PrintStream& out = view.out;
    IRPrinter printer(out, view.program());
    SlotUses uses = collectSlotUsesFor(view);
    const Vector<SiteWeight>& weights = uses.keyWeights;
    Vector<uint32_t> keys;
    for (uint32_t key = 0; key < weights.size(); ++key) {
        if (weights[key])
            keys.append(key);
    }
    AOT::sort(keys, [&](uint32_t a, uint32_t b) {
        return weights[a] != weights[b] ? weights[a] > weights[b] : a < b;
    });
    for (uint32_t key : keys) {
        PropertyKeyID id(key);
        printer.print(id);
        out.print(" weight="_s, weights[key], view.slots().hasColumn(id) ? " column\n"_s : " no column\n"_s);
    }
}

static void printFill(const SlotPlanViewContext& view)
{
    dumpFillEstimates(view.out, view.program(), collectSlotUsesFor(view));
}

static std::span<const SlotPlanView> slotPlanViews()
{
    static constexpr SlotPlanView views[] {
        { "layouts"_s, printLayouts },
        { "accesses"_s, printAccesses },
        { "variables"_s, printVariables },
        { "closures"_s, printClosures },
        { "classes"_s, printClasses },
        { "padding"_s, printPadding },
        { "columns"_s, printColumns },
        { "fill"_s, printFill },
    };
    return views;
}

static void printViews(const SlotPlanViewContext& view, std::span<const String> names)
{
    for (const String& name : names) {
        if (!findView(slotPlanViews(), name)) {
            printBadArgument(view.out, name);
            return;
        }
    }
    if (names.empty()) {
        view.slots().dump(view.out, view.program());
        return;
    }
    for (const String& name : names)
        findView(slotPlanViews(), name)->print(view);
}

void runPlanSlots(TestCommandContext& context)
{
    std::unique_ptr<PlacedProgram> placed = runPipelineForPlacementTest(context, PassID::PlanSlots);
    if (!placed)
        return;
    printViews({ context.out, *placed, context.build }, context.arguments);
    context.program = WTF::move(placed->output->program);
    context.form = context.program->form();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

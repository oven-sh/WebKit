/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTStatistics.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static bool isDeferredSlot(const PlacedSlot& slot, FunctionID function)
{
    return slot.representation == SlotRepresentation::DeferredClosure && slot.deferredFunction == function;
}

static bool initializesDeferredSlot(SelectionContext& context, const Value& allocation, ValueID stored, FunctionID function)
{
    std::span<const ValueID> values = allocation.valuesOperands();
    auto value = std::ranges::find(values, stored);
    if (value == values.end())
        return false;
    uint32_t field = allocation.fields()[value - values.begin()];
    return isDeferredSlot(context.slots().slot(allocation.layout(), field), function);
}

static bool storesIntoDeferredSlot(SelectionContext& context, const Value& store, ValueID stored, FunctionID function)
{
    std::optional<uint8_t> storedOperand = storedOf(store.opcode);
    if (accessClassOf(store.opcode) != AccessClass::Slot || !storedOperand || store.operand(*storedOperand) != stored)
        return false;

    const Program& program = context.program();
    const SlotPlan& slots = context.slots();
    PropertyKeyID key = store.key();
    SlotAgreement agreement = slots.agreementOf(program, key, store.receivers());
    if (!agreement.hasOneLocation() || !agreement.coversEveryMember())
        return false;
    return std::ranges::all_of(agreement.groups[0].members, [&](LayoutID member) {
        std::optional<uint32_t> field = program.at(member).findField(key);
        return field && isDeferredSlot(slots.slot(member, *field), function);
    });
}

static bool isDeferredStore(SelectionContext& context, ValueID id)
{
    const Function& function = context.function();
    ValueID stored = id;
    std::optional<ValueID> user = context.singleUser(stored);
    while (user && function.at(*user).opcode == Opcode::Upcast) {
        stored = *user;
        user = context.singleUser(stored);
    }
    if (!user)
        return false;

    FunctionID closureFunction = function.at(id).function();
    const Value& store = function.at(*user);
    if (store.opcode == Opcode::NewObject)
        return initializesDeferredSlot(context, store, stored, closureFunction);
    return storesIntoDeferredSlot(context, store, stored, closureFunction);
}

static ClosureForm cellFormOf(SelectionContext& context, const Value& closure)
{
    ClosureForm form = closure.closureForm();
    return form == ClosureForm::Singleton ? context.slots().closureFormOf(closure.function()) : form;
}

static void countClosures(SelectionContext& context, std::span<const ValueID> closures)
{
    const Function& function = context.function();
    uint64_t full = 0;
    uint64_t slim = 0;
    uint64_t deferred = 0;
    for (ValueID id : closures) {
        const Value& closure = function.at(id);
        // AllocateInFrames counted it when it took the closure off the heap.
        if (closure.storage() != AllocationStorage::Heap)
            continue;
        switch (cellFormOf(context, closure)) {
        case ClosureForm::Full:
            ++full;
            break;
        case ClosureForm::Slim:
            ++slim;
            break;
        case ClosureForm::Deferred:
            ++deferred;
            break;
        case ClosureForm::Singleton:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    Statistics& statistics = context.build().statistics();
    statistics.count(StatisticID::ClosuresFull, full);
    statistics.count(StatisticID::ClosuresSlim, slim);
    statistics.count(StatisticID::ClosuresDeferred, deferred);
}

static std::optional<AllocationOutcome> outcomeOf(ClosureForm form)
{
    switch (form) {
    case ClosureForm::Full:
    case ClosureForm::Singleton:
        return std::nullopt;
    case ClosureForm::Slim:
        return AllocationOutcome::Slim;
    case ClosureForm::Deferred:
        return AllocationOutcome::Deferred;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addAllocationRemarks(SelectionContext& context, std::span<const ValueID> closures)
{
    BuildContext& build = context.build();
    if (!build.options().collectsAllocationRemarks())
        return;

    const Function& function = context.function();
    for (ValueID id : closures) {
        const Value& closure = function.at(id);
        std::optional<AllocationOutcome> outcome = outcomeOf(closure.closureForm());
        if (!outcome || closure.storage() != AllocationStorage::Heap || !closure.allocationSite())
            continue;
        // AllocateInFrames counted this copy when it left the closure on the heap.
        build.addAllocationRemark({ .site = closure.allocationSite(), .location = function.locationOf(id), .outcome = *outcome, .copies = 0 });
    }
}

static ClosureForm chooseForm(SelectionContext& context, ValueID id)
{
    const Value& closure = context.function().at(id);
    if (closure.isMadeOnDemand() || isDeferredStore(context, id))
        return ClosureForm::Deferred;
    if (hasSingletonClosure(context.program(), closure.function()))
        return ClosureForm::Singleton;
    return context.slots().closureFormOf(closure.function());
}

void selectClosureForm(SelectionContext& context)
{
    std::span<const ValueID> closures = context.valuesWith(Opcode::NewClosure);
    if (closures.empty())
        return;

    Function& function = context.pass().function;
    uint32_t numberOfChangedForms = 0;
    for (ValueID id : closures) {
        Value& closure = function.at(id);
        ClosureForm form = chooseForm(context, id);
        if (closure.closureForm() == form)
            continue;
        closure.setClosureForm(form);
        ++numberOfChangedForms;
    }

    countClosures(context, closures);
    addAllocationRemarks(context, closures);
    context.didSelectForms(SelectionRuleID::ClosureForm, numberOfChangedForms);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

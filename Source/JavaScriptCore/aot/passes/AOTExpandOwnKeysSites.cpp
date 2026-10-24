/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTJSONSupport.h"
#include "AOTKnownValue.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeTestCoverage.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// A field that is redefined, or stored with a value its slot cannot hold, keeps its place in the key order and leaves its slot for the extra properties.
static constexpr OptionSet<ObjectMutation> mutationsChangingOwnKeys {
    ObjectMutation::DeleteField,
    ObjectMutation::AddProperty,
    ObjectMutation::RedefineProperty,
    ObjectMutation::StoreOfOtherType,
    ObjectMutation::ReachNativeCode,
    ObjectMutation::ReachUnknownJavaScript,
};

static BuiltinID calledBuiltinOf(const BuiltinTable& builtins, const Value& call)
{
    return builtins.canonicalID(builtins.at(call.overload()).builtin);
}

static bool mayContainSites(const Function& function, BlockID block)
{
    return function.at(block).frequency != BlockFrequency::Rare;
}

static bool hasOrdinaryOwnKeys(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
        return true;
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<OwnKeyWalk> ownKeyWalkOf(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::CopyDataProperties) {
        if (!value.excludedOperands().empty())
            return std::nullopt;
        return OwnKeyWalk::Spread;
    }
    if (value.opcode == Opcode::CopyObject) {
        if (!value.excludedOperands().empty() || !value.excludedKeys().empty())
            return std::nullopt;
        return OwnKeyWalk::Copy;
    }
    if (value.opcode != Opcode::CallBuiltin || value.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;

    const BuiltinTable& builtins = program.builtins();
    if (builtins.at(value.overload()).mode != BuiltinCallMode::Call)
        return std::nullopt;

    BuiltinID builtin = calledBuiltinOf(builtins, value);
    if (builtin == Builtins::ObjectKeys)
        return OwnKeyWalk::Keys;
    if (builtin == Builtins::ObjectValues)
        return OwnKeyWalk::Values;
    if (builtin == Builtins::ObjectEntries)
        return OwnKeyWalk::Entries;
    if (builtin == Builtins::ObjectAssign)
        return OwnKeyWalk::Assign;
    if (builtin == Builtins::JSONStringify)
        return OwnKeyWalk::Stringify;

    const BuiltinForms* forms = builtins.formsOf(builtin);
    if (forms && forms->jsonTextForm)
        return OwnKeyWalk::StringifyIntoText;
    return std::nullopt;
}

static std::optional<OwnKeyWalkSite> findSiteAt(const Program& program, const Function& function, ProgramPoint point)
{
    ValueID id = function.at(point.block).values[point.index];
    const Value& value = function.at(id);
    std::optional<OwnKeyWalk> walk = ownKeyWalkOf(program, value);
    if (!walk)
        return std::nullopt;

    OwnKeyWalkSite site { .walk = *walk, .block = point.block, .index = point.index, .value = id };
    if (*walk == OwnKeyWalk::Spread) {
        site.source = value.sourceOperand();
        site.target = value.targetOperand();
        return site;
    }
    if (*walk == OwnKeyWalk::Copy) {
        site.source = value.sourceOperand();
        return site;
    }

    std::span<const ValueID> arguments = builtinCallOf(program, value).arguments;
    switch (*walk) {
    case OwnKeyWalk::Keys:
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
        break;
    case OwnKeyWalk::Assign:
        if (arguments.size() < 2)
            return std::nullopt;
        site.target = arguments[0];
        site.sourceArgument = 1;
        site.laterSources.append(arguments.subspan(2));
        break;
    case OwnKeyWalk::Stringify: {
        bool hasReplacerOrIndent = std::ranges::any_of(arguments.subspan(std::min<size_t>(arguments.size(), 1)), [&](ValueID argument) {
            return !knownValueOf(program, function, argument).isNullish();
        });
        if (hasReplacerOrIndent)
            return std::nullopt;
        break;
    }
    case OwnKeyWalk::StringifyIntoText: {
        const BuiltinForms* forms = program.builtins().formsOf(calledBuiltinOf(program.builtins(), value));
        std::optional<BuiltinOperand> textArgument = forms->jsonTextArgument;
        if (!textArgument || textArgument->kind() != BuiltinOperand::Kind::Argument)
            return std::nullopt;
        site.sourceArgument = textArgument->argumentIndex();
        site.textForm = forms->jsonTextForm;
        break;
    }
    case OwnKeyWalk::Spread:
    case OwnKeyWalk::Copy:
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (site.sourceArgument >= arguments.size())
        return std::nullopt;
    site.source = arguments[site.sourceArgument];
    return site;
}

Vector<OwnKeyWalkSite, 4> findOwnKeyWalkSites(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<OwnKeyWalkSite, 4> sites;
    for (BlockID block : context.analyses.cfg().reversePostOrder() | std::views::reverse) {
        if (!mayContainSites(function, block))
            continue;
        for (uint32_t index = function.at(block).values.size(); index--;) {
            if (std::optional<OwnKeyWalkSite> site = findSiteAt(context.program, function, { block, index }))
                sites.append(*site);
        }
    }
    return sites;
}

UnchangedAllocations::UnchangedAllocations(FunctionPassContext& context, std::span<const OwnKeyWalkSite> sites)
    : m_context(context)
{
    auto addUse = [&](const OwnKeyWalkSite& site, ValueID object) {
        ValueID origin = context.analyses.sameObjectThroughCasts(object);
        if (context.function.at(origin).opcode == Opcode::NewObject)
            m_uses.append(AllocationAtPoint { origin, { site.block, site.index } });
    };
    for (const OwnKeyWalkSite& site : sites) {
        addUse(site, site.source);
        if (site.walk == OwnKeyWalk::Assign || site.walk == OwnKeyWalk::Spread)
            addUse(site, site.target);
    }

    const CFG& cfg = context.analyses.cfg();
    AOT::sort(m_uses, { }, [&](const AllocationAtPoint& use) {
        return std::pair { cfg.reversePostOrderIndex(use.point.block), use.point.index };
    });
}

bool UnchangedAllocations::contains(ValueID allocation, ProgramPoint point) const
{
    size_t index = m_uses.findIf([&](const AllocationAtPoint& use) {
        return use.allocation == allocation && use.point.block == point.block && use.point.index == point.index;
    });
    if (index == notFound)
        return false;
    if (!m_unchanged)
        m_unchanged = findUnchangedAllocations(m_context, m_uses.span());
    return m_unchanged->get(index);
}

static bool isUnchangedLiteral(FunctionPassContext& context, const UnchangedAllocations& unchanged, ValueID value, ProgramPoint point)
{
    ValueID origin = context.analyses.sameObjectThroughCasts(value);
    const Value& allocation = context.function.at(origin);
    if (allocation.opcode != Opcode::NewObject || allocation.prototypeOperand())
        return false;
    return context.program.at(allocation.layout()).kind == LayoutKind::Record && unchanged.contains(origin, point);
}

static std::optional<OwnKeyWalkPlan> planUnchangedAllocationWalk(FunctionPassContext& context, const UnchangedAllocations& unchanged, const OwnKeyWalkSite& site)
{
    std::optional<KeyOrderFields> order = keyOrderAtCreation(context.program, context.function, context.analyses, site.source);
    if (!order)
        return std::nullopt;

    ValueID allocation = context.analyses.sameObjectThroughCasts(site.source);
    LayoutID layout = context.function.at(allocation).layout();
    if (!hasOrdinaryOwnKeys(context.program.at(layout).kind) || (!unchanged.contains(allocation, { site.block, site.index }) && !context.build.isSabotaged(Sabotage::WalkedLiteralsAreAsTheyWereMade)))
        return std::nullopt;

    OwnKeyWalkPlan plan { .needsShapeTest = false, .needsKeyOrderTest = false, .needsOriginalCall = false, .unchangedAllocation = allocation };
    plan.arms.append(OwnKeyWalkArm { layout, WTF::move(*order) });
    return plan;
}

static bool hasOnlyFilledFields(const Program& program, const OwnKeyWalkArm& arm)
{
    return std::ranges::all_of(arm.order, [&](uint32_t field) {
        return flowOf(program, { arm.layout, field }).occupancy == Occupancy::AlwaysFilled;
    });
}

static bool isUnderPrivateName(const Program& program, const Field& field)
{
    if (!field.key)
        return false;
    PropertyKeyKind kind = program.at(field.key).kind;
    return kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::EnginePrivateName;
}

static bool hasFieldsUnderPrivateNames(const Program& program, LayoutID layout)
{
    return std::ranges::any_of(program.at(layout).fields, [&](const Field& field) {
        return isUnderPrivateName(program, field);
    });
}

static bool listsOrderThatNoObjectHas(const Program& program, const OwnKeyWalkArm& arm)
{
    const Vector<Field>& fields = program.at(arm.layout).fields;
    for (uint32_t field = 0; field < fields.size(); ++field) {
        if (isUnderPrivateName(program, fields[field]) && !arm.order.contains(field))
            return true;
    }
    return false;
}

static bool isOrdinaryDataField(const Program& program, FieldReference reference)
{
    constexpr OptionSet<FieldFlags> privateNameFlags { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    const Field& field = fieldAt(program, reference);
    return flowOf(program, reference).location.deferral == ClosureDeferral::Unknown && !field.flags.containsAny(privateNameFlags) && field.form.isEmpty();
}

// The bits of such a slot are all there is to its field.
static bool hasPlainSlot(const Program& program, FieldReference reference)
{
    return flowOf(program, reference).location.stores != StoreCount::AtMostOnce && isOrdinaryDataField(program, reference);
}

struct WalkedLayouts {
    bool areProven { false };
    std::span<const LayoutID> layouts;
};

static WalkedLayouts walkedLayoutsOf(FunctionPassContext& context, const OwnKeyWalkSite& site)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const Type& type = program.typeOf(function, site.source);
    bool areProven = type.hasOnlyLayouts(program) && (copiesNothingFromNullish(site.walk) || !type.mayBeUndefinedOrNull());
    return { areProven, membersOf(program, areProven ? type : program.expectedOrTypeOf(function, site.source)) };
}

// The layout of a literal that has no property yet when a spread without excluded keys fills it.
static LayoutID layoutOfEmptyTarget(FunctionPassContext& context, const UnchangedAllocations& unchanged, const OwnKeyWalkSite& site)
{
    if (site.walk != OwnKeyWalk::Spread || !context.function.at(site.value).excludedKeys().empty())
        return { };
    const Value& allocation = context.function.at(context.analyses.sameObjectThroughCasts(site.target));
    if (allocation.opcode != Opcode::NewObject || !allocation.fields().empty())
        return { };
    LayoutID layout = allocation.layout();
    if (!std::ranges::contains(walkedLayoutsOf(context, site).layouts, layout))
        return { };
    for (uint32_t field = 0; field < context.program.at(layout).fields.size(); ++field) {
        if (!hasPlainSlot(context.program, { layout, field }))
            return { };
    }
    if (!isUnchangedLiteral(context, unchanged, site.target, { site.block, site.index }))
        return { };
    return layout;
}

static std::optional<OwnKeyWalkPlan> planWalkByLayouts(FunctionPassContext& context, const OwnKeyWalkSite& site, LayoutID emptyTarget = { })
{
    const Program& program = context.program;
    auto [isProven, layouts] = walkedLayoutsOf(context, site);
    // Behind arms the list of the built-in meets the lists of the arms, and is made even where only its length is asked for. Objects that have none of the expected layouts may be all that comes.
    if (site.walk == OwnKeyWalk::Keys && !isProven)
        return std::nullopt;

    // HasKeyOrder compares a word that is numbered per layout, so the arms of several proven layouts test the shape too.
    OwnKeyWalkPlan plan { .needsShapeTest = !isProven || layouts.size() > 1 };
    KeyOrderLimits limits { context.limit(LimitID::MaxExpandedKeyOrders), context.limit(LimitID::MaxExpandedKeyFields) };
    uint64_t numberOfOrdersLeftOut = 0;
    bool coversEveryObject = !plan.needsShapeTest;
    for (LayoutID layout : layouts) {
        const Layout& record = program.at(layout);
        if (record.flow.allocation == AllocationState::NeverAllocated)
            continue;
        if (!hasOrdinaryOwnKeys(record.kind)) {
            coversEveryObject = false;
            continue;
        }
        if (layout == emptyTarget && plan.arms.size() < limits.maxOrders && record.fields.size() <= limits.maxFields) {
            coversEveryObject = false;
            plan.arms.append(OwnKeyWalkArm { layout, { }, true });
            continue;
        }

        ExpectedKeyOrders expected = expectedKeyOrdersOf(program, layout, limits);
        numberOfOrdersLeftOut += expected.numberOfOrdersLeftOut;
        coversEveryObject &= expected.coverage == KeyOrderCoverage::EveryObject && receiverStateOf(program, layout).impossibleMutations.containsAll(mutationsChangingOwnKeys);
        for (KeyOrderFields& order : expected.orders) {
            if (plan.arms.size() == limits.maxOrders) {
                ++numberOfOrdersLeftOut;
                continue;
            }
            plan.arms.append(OwnKeyWalkArm { layout, WTF::move(order) });
        }
    }

    if (numberOfOrdersLeftOut)
        context.exceeds(LimitID::MaxExpandedKeyOrders, limits.maxOrders + numberOfOrdersLeftOut, site.value);
    if (plan.arms.isEmpty())
        return std::nullopt;

    if (coversEveryObject && !numberOfOrdersLeftOut) {
        // The orders of flow list own keys. The word that HasKeyOrder compares counts the fields under private names too, so no object that has one passes the test of such an order.
        bool canTestOrders = std::ranges::none_of(plan.arms, [&](const OwnKeyWalkArm& arm) {
            return hasFieldsUnderPrivateNames(program, arm.layout);
        });
        // A validating build tests the one order that flow has proven: an object with another order reaches the Unreachable behind the arm.
        plan.needsKeyOrderTest = plan.arms.size() > 1 || !hasOnlyFilledFields(program, plan.arms[0]) || (canTestOrders && context.build.validates());
        plan.needsOriginalCall = plan.needsKeyOrderTest && !canTestOrders;
    }

    if (plan.needsKeyOrderTest) {
        plan.arms.removeAllMatching([&](const OwnKeyWalkArm& arm) {
            return !arm.copiesFieldsAtOnce && listsOrderThatNoObjectHas(program, arm);
        });
        if (plan.arms.isEmpty())
            return std::nullopt;
    }
    return plan;
}

static std::optional<OwnKeyWalkPlan> planSourceWalk(FunctionPassContext& context, const UnchangedAllocations& unchanged, const OwnKeyWalkSite& site)
{
    if (std::optional<OwnKeyWalkPlan> plan = planUnchangedAllocationWalk(context, unchanged, site))
        return plan;
    return planWalkByLayouts(context, site, layoutOfEmptyTarget(context, unchanged, site));
}

// Set(target, "__proto__", value) gives the target a prototype, whose setters would then run between the reads of the source. The expansion reads every field first.
static bool assignsPrototype(const Program& program, const OwnKeyWalkPlan& plan)
{
    return std::ranges::any_of(plan.arms, [&](const OwnKeyWalkArm& arm) {
        const Vector<Field>& fields = program.at(arm.layout).fields;
        return std::ranges::any_of(arm.order, [&](uint32_t field) {
            return fields[field].key && program.nameClassOf(fields[field].key) == NameClass::Proto;
        });
    });
}

// A copy is an ordinary object with ordinary properties under the same names in the same order.
AOT_THRESHOLD(size_t, maxFieldsCopiedByAllocation, 8);

static bool isCopiedByAllocation(const Program& program, const OwnKeyWalkArm& arm)
{
    const Layout& layout = program.at(arm.layout);
    if (layout.kind != LayoutKind::Record || arm.order.size() > maxFieldsCopiedByAllocation)
        return false;
    return std::ranges::all_of(arm.order, [&](uint32_t field) {
        return isEnumerableStringField(program, layout.fields[field]) && isOrdinaryDataField(program, { arm.layout, field });
    });
}

std::optional<OwnKeyWalkPlan> planOwnKeyWalk(FunctionPassContext& context, const UnchangedAllocations& unchanged, const OwnKeyWalkSite& site)
{
    RELEASE_ASSERT(site.walk != OwnKeyWalk::Assign);
    std::optional<OwnKeyWalkPlan> plan = planSourceWalk(context, unchanged, site);
    if (!plan || site.walk != OwnKeyWalk::Copy)
        return plan;
    bool leavesArmsOut = plan->arms.removeAllMatching([&](const OwnKeyWalkArm& arm) {
        return !isCopiedByAllocation(context.program, arm);
    });
    if (plan->arms.isEmpty())
        return std::nullopt;
    if (leavesArmsOut) {
        if (!plan->needsShapeTest)
            return std::nullopt;
        plan->needsOriginalCall = true;
    }
    return plan;
}

std::optional<OwnKeyWalkPlan> planWalkOfEnumeratedObject(FunctionPassContext& context, ValueID forInOpen)
{
    return planWalkByLayouts(context, OwnKeyWalkSite { .value = forInOpen, .source = context.function.at(forInOpen).objectOperand() });
}

static std::optional<Value> makeAssignCall(FunctionPassContext& context, const Value& original, std::span<const ValueID> arguments)
{
    const Program& program = context.program;
    TypingContext typing { program, context.function, context.types };
    BuiltinOperandTypes operands = operandTypesOf(typing, original.receiverOperand(), arguments);
    OverloadMatch match = matchOverload({ typing, program.builtins().at(original.overload()).builtin, BuiltinCallMode::Call, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
        return std::nullopt;

    Value call = original;
    call.setOverload(match.overload);
    call.setArgumentsOperands(arguments);
    if (!overloadTakesOperands(program, context.function, call))
        return std::nullopt;
    return call;
}

static std::optional<Vector<Value, 2>> makeAssignCallsFromEachSource(FunctionPassContext& context, const OwnKeyWalkSite& site)
{
    const Value& original = context.function.at(site.value);
    Vector<Value, 2> calls { original };
    for (size_t first = 0; first < site.laterSources.size(); ++first) {
        Vector<ValueID, 4> arguments { site.target };
        arguments.append(site.laterSources.subspan(first));
        std::optional<Value> call = makeAssignCall(context, original, arguments.span());
        if (!call)
            return std::nullopt;
        calls.append(WTF::move(*call));
    }
    return calls;
}

// The stores of a walked source add data properties to a literal that has no setter, so the next source finds it without one.
std::optional<AssignPlan> planAssign(FunctionPassContext& context, const UnchangedAllocations& unchanged, const OwnKeyWalkSite& site)
{
    RELEASE_ASSERT(site.walk == OwnKeyWalk::Assign);
    if (!isUnchangedLiteral(context, unchanged, site.target, { site.block, site.index }))
        return std::nullopt;
    std::optional<Vector<Value, 2>> calls = makeAssignCallsFromEachSource(context, site);
    if (!calls)
        return std::nullopt;

    AssignPlan result;
    OwnKeyWalkSite walked = site;
    for (Value& call : *calls) {
        size_t index = result.walkedSources.size();
        if (index)
            walked.source = site.laterSources[index - 1];
        // What is known of an allocation at the call holds up to the stores of the first source.
        std::optional<OwnKeyWalkPlan> plan = index ? planWalkByLayouts(context, walked) : planSourceWalk(context, unchanged, walked);
        if (!plan || assignsPrototype(context.program, *plan))
            break;
        result.walkedSources.append(AssignedSource { walked.source, WTF::move(*plan), WTF::move(call) });
    }

    size_t numberOfWalkedSources = result.walkedSources.size();
    if (!numberOfWalkedSources)
        return std::nullopt;
    if (numberOfWalkedSources < calls->size())
        result.remainingSourcesCall = WTF::move(calls->at(numberOfWalkedSources));
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

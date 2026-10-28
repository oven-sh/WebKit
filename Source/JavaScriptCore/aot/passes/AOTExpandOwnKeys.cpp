/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysInternal.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTJSONSerializerBuilder.h"
#include "AOTJSONSupport.h"
#include "AOTKnownValue.h"
#include "AOTNaturalLoops.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRegionCloner.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTRewrite.h"
#include "AOTSort.h"
#include "AOTSpecifiedValues.h"
#include "AOTThreshold.h"
#include "AOTTypeTestCoverage.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <ranges>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

bool isEnumerableStringField(const Program& program, const Field& field)
{
    return isJSONSerializedKey(program, field.key) && !field.computedKey && !field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden })
        && !field.form.contains(FieldForm::IsNotEnumerable);
}

ExpectedKeyOrders expectedKeyOrdersOf(const Program& program, LayoutID id, KeyOrderLimits limits)
{
    // The instances of a class below a built-in class are cells of that class. They have no slots and no word for the order of their keys.
    if (builtinBaseClassOf(program, id))
        return { .layout = id };

    Vector<LayoutID, 4> layoutsFromBaseDown;
    for (LayoutID current = id; current; current = program.at(current).flow.base) {
        RELEASE_ASSERT(layoutsFromBaseDown.size() < program.layouts.size());
        layoutsFromBaseDown.append(current);
    }
    layoutsFromBaseDown.reverse();

    const Layout& layout = program.at(id);
    auto saturateToUInt32 = [](uint64_t count) {
        return std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max());
    };
    // The slot of an accessor holds a GetterSetter, which a walk over the slots would take for the value.
    auto listsAccessor = [&](const KeyOrderFields& order) {
        return std::ranges::any_of(order, [&](uint32_t field) {
            return layout.fields[field].form.contains(FieldForm::IsAccessor);
        });
    };
    // No object is seen without a field that is always filled, so an order that leaves one out is the order of none.
    BitVector isInEveryOrder(layout.fields.size());
    for (uint32_t field = 0; field < layout.fields.size(); ++field) {
        if (isEnumerableStringField(program, layout.fields[field]) && flowOf(program, { id, field }).occupancy == Occupancy::AlwaysFilled)
            isInEveryOrder.quickSet(field);
    }
    auto isOrderOfNoObject = [&, numberOfFieldsInEveryOrder = isInEveryOrder.bitCount()](const KeyOrderFields& order) {
        return static_cast<size_t>(std::ranges::count_if(order, [&](uint32_t field) { return isInEveryOrder.quickGet(field); })) < numberOfFieldsInEveryOrder;
    };
    uint64_t numberOfOrdersLeftOut = 0;
    bool isComplete = true;
    Vector<KeyOrderFields, 4> orders { KeyOrderFields() };
    for (LayoutID declaringLayout : layoutsFromBaseDown) {
        const Layout& declaring = program.at(declaringLayout);
        Vector<KeyOrderFields, 4> createdOrders;
        for (const FieldOrder& order : declaring.flow.constructorOrders) {
            KeyOrderFields created;
            created.reserveInitialCapacity(order.fields.size());
            for (uint32_t declaredField : order.fields) {
                std::optional<uint32_t> field = layout.findField(fieldAt(program, { declaringLayout, declaredField }).key);
                if (!field)
                    break;
                created.append(*field);
            }
            if (created.size() == order.fields.size())
                createdOrders.append(WTF::move(created));
            else
                isComplete = false;
        }
        // Flow lists orders for a layout that is allocated itself. A base that declares no field adds nothing to the order of a subclass.
        if (declaring.flow.constructorOrders.isEmpty() && declaringLayout != id && std::ranges::all_of(declaring.fields, [](const Field& field) {
            return field.flags.contains(FieldFlags::IsInherited);
        }))
            createdOrders.append(KeyOrderFields());
        numberOfOrdersLeftOut = saturateToUInt32(numberOfOrdersLeftOut) * createdOrders.size();

        Vector<KeyOrderFields, 4> extendedOrders;
        for (const KeyOrderFields& order : orders) {
            for (const KeyOrderFields& created : createdOrders) {
                KeyOrderFields extended = order;
                for (uint32_t field : created) {
                    if (!order.contains(field))
                        extended.append(field);
                }
                if (extendedOrders.contains(extended) || (declaringLayout == id && isOrderOfNoObject(extended)))
                    continue;
                if (extended.size() > limits.maxFields || extendedOrders.size() == limits.maxOrders || listsAccessor(extended))
                    ++numberOfOrdersLeftOut;
                else
                    extendedOrders.append(WTF::move(extended));
            }
        }
        orders = WTF::move(extendedOrders);
    }

    // The orders of a base are those of objects allocated as the base, so they are no proof about what its constructor does for a subclass.
    bool coversEveryObject = isComplete && !numberOfOrdersLeftOut && !orders.isEmpty()
        && layout.flow.baseState == BaseState::None && layout.flow.fieldCreation != FieldCreation::Unknown;
    return { id, WTF::move(orders), coversEveryObject, static_cast<uint32_t>(saturateToUInt32(numberOfOrdersLeftOut)) };
}

static constexpr size_t conditionsPerAllocation = 3;

// Bit i is set when nothing between the NewObject uses[i].allocation and uses[i].point can have changed its shape, its key order or its extra properties.
static BitVector findUnchangedAllocations(FunctionPassContext& context, std::span<const AllocationAtPoint> uses)
{
    BitVector unchanged(uses.size());
    if (uses.empty())
        return unchanged;

    TestConditions conditions(context.analyses);
    Vector<TestRequirement> requirements;
    Vector<ExistingProof> proofs;
    Vector<size_t, 8> askedUses;
    for (size_t index = 0; index < uses.size(); ++index) {
        const AllocationAtPoint& use = uses[index];
        if (!allocatesOwnLayout(context.program, context.function.at(use.allocation)))
            continue;
        LayoutID layout = context.function.at(use.allocation).layout();
        std::array<Value, conditionsPerAllocation> wantedConditions {
            makeShapeTest(use.allocation, context.types.completeLayoutSet(singleElementSpan(layout))),
            Emission::make<&Value::setLayout, &Value::setFields>(Opcode::HasKeyOrder, { use.allocation }, layout, context.function.at(use.allocation).fields()),
            IRBuilder::make(Opcode::HasNoExtraProperties, { use.allocation }),
        };
        size_t numberOfNewConditions = std::ranges::count_if(wantedConditions, [&](const Value& condition) {
            return !conditions.find(condition);
        });
        if (conditions.size() + numberOfNewConditions > context.limit(LimitID::MaxTestedTypesPerFunction)) {
            context.gaveUp(LimitID::MaxTestedTypesPerFunction, use.allocation);
            continue;
        }

        for (const Value& condition : wantedConditions) {
            unsigned numberOfKnownConditions = conditions.size();
            TestConditionID id = conditions.add(condition);
            if (conditions.size() > numberOfKnownConditions)
                proofs.append(ExistingProof { ProofSource::Allocation, id, use.allocation, { } });
            requirements.append(TestRequirement { 0, 0, id, use.point, RequirementKind::AsksOnly });
        }
        askedUses.append(index);
    }
    if (askedUses.isEmpty())
        return unchanged;

    TestEvents events(TestEventInputs { context.analyses, conditions, { }, requirements.span(), proofs.span(), { }, false });
    TestCoverage coverage(TestCoverageInputs { events, GapBudget::unlimited(), nullptr });
    for (size_t asked = 0; asked < askedUses.size(); ++asked) {
        bool isUnchanged = true;
        for (size_t condition = 0; condition < conditionsPerAllocation; ++condition)
            isUnchanged &= coverage.of(static_cast<uint32_t>(asked * conditionsPerAllocation + condition)) == RequirementCoverage::Covered;
        if (isUnchanged)
            unchanged.set(askedUses[asked]);
    }
    return unchanged;
}

static PassOutcome foldKeyOrderTests(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<AllocationAtPoint, 4> tests;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (!opcodesOfKeyOrderTests.contains(value.opcode))
                continue;
            ValueID allocation = context.analyses.identityOf(value.objectOperand());
            if (function.at(allocation).opcode != Opcode::NewObject)
                continue;
            bool hasBranchElsewhere = std::ranges::any_of(context.analyses.useLists().usesOf(values[index]), [&](const Use& use) {
                return use.kind == Use::Kind::TerminatorOperand && use.block != block && function.at(use.block).terminator.isConditionalBranch();
            });
            if (!hasBranchElsewhere)
                tests.append(AllocationAtPoint { allocation, { block, index } });
        }
    }
    BitVector decidableTests = findUnchangedAllocations(context, tests.span());
    if (decidableTests.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (size_t index : decidableTests) {
        auto [allocation, point] = tests[index];
        ValueID test = function.at(point.block).values[point.index];
        bool result = function.at(test).opcode == Opcode::HasNoExtraProperties
            || (function.at(test).layout() == function.at(allocation).layout() && std::ranges::equal(function.at(test).fields(), function.at(allocation).fields()));

        Terminator& terminator = function.edit(point.block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        if (terminator.isConditionalBranch() && terminator.conditionOperand() == test) {
            if (!result)
                decideTerminator(builder, point.block, static_cast<uint32_t>(&terminator.notTaken() - terminator.targets.begin()));
            else {
                // The taken edge passes a guard, which only a test or an allocation can stand for.
                Target taken = terminator.taken();
                builder.setOrigin(terminator.origin());
                terminator = Terminator { };
                builder.moveToEndOf(point.block);
                Arguments arguments = WTF::move(taken.arguments);
                for (size_t proof = 0; proof < taken.proofs.size(); ++proof)
                    arguments.append(builder.append(Opcode::AllGuards, { allocation }));
                builder.emitJump(taken.block, WTF::move(arguments));
            }
        }

        becomeConstant(context.program, function, context.types, test, KnownValue::boolean(result));
    }
    context.analyses.didChangeValues();
    builder.emptyUnreachableBlocks();
    return PassOutcome::Changed;
}

static bool isExpandedByPlan(OwnKeyWalk walk)
{
    switch (walk) {
    case OwnKeyWalk::Keys:
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread:
    case OwnKeyWalk::Copy:
        return true;
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

UnchangedAllocations::UnchangedAllocations(FunctionPassContext& context, std::span<const OwnKeyWalkSite> sites)
    : m_context(context)
{
    auto addUse = [&](const OwnKeyWalkSite& site, ValueID object) {
        ValueID origin = context.analyses.sameObjectThroughCasts(object).valueInFrontOfFacts();
        if (context.function.at(origin).opcode == Opcode::NewObject)
            m_uses.append(AllocationAtPoint { origin, { site.block, site.index } });
    };
    for (const OwnKeyWalkSite& site : sites) {
        if (!isExpandedByPlan(site.walk))
            continue;
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

static BuiltinID calledBuiltinOf(const BuiltinTable& builtins, const Value& call)
{
    return builtins.canonicalID(builtins.at(call.overload()).builtin);
}

static std::optional<OwnKeyWalk> ownKeyWalkOf(const Program& program, const Value& value)
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
    for (ValueID later : function.at(point.block).values.span().subspan(point.index + 1)) {
        if (function.at(later).opcode == Opcode::AssumeType && function.at(later).operand(0) == id) {
            site.provenTypeOfList = function.at(later).type;
            break;
        }
    }
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

// A field that is redefined, or stored with a value its slot cannot hold, keeps its place in the key order and leaves its slot for the extra properties.
static constexpr OptionSet<ObjectMutation> mutationsChangingOwnKeys {
    ObjectMutation::DeleteField,
    ObjectMutation::AddProperty,
    ObjectMutation::RedefineProperty,
    ObjectMutation::StoreOfOtherType,
    ObjectMutation::ReachNativeCode,
    ObjectMutation::ReachUnknownJavaScript,
};

static constexpr bool copiesNothingFromNullish(OwnKeyWalk walk)
{
    switch (walk) {
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread:
        return true;
    // It makes an object of undefined and null too, which the original operation does.
    case OwnKeyWalk::Copy:
    case OwnKeyWalk::Keys:
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static bool hasOrdinaryOwnKeys(LayoutKind kind) { return hasOrdinaryOwnKeysOf(kind); }

static bool isUnchangedLiteral(FunctionPassContext& context, const UnchangedAllocations& unchanged, ValueID value, ProgramPoint point)
{
    ValueID origin = context.analyses.sameObjectThroughCasts(value).valueInFrontOfFacts();
    const Value& allocation = context.function.at(origin);
    if (allocation.opcode != Opcode::NewObject || allocation.prototypeOperand())
        return false;
    return context.program.at(allocation.layout()).kind == LayoutKind::Record && unchanged.contains(origin, point);
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
    return kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::EnginePrivateName || kind == PropertyKeyKind::ModulePrivateName;
}

static bool isOrdinaryDataField(const Program& program, FieldReference reference)
{
    constexpr OptionSet<FieldFlags> privateNameFlags { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    const Field& field = fieldAt(program, reference);
    return flowOf(program, reference).location.deferral == ClosureDeferral::Unknown && !field.flags.containsAny(privateNameFlags) && field.form.isEmpty();
}

// Whether the layouts are proven, and the layouts.
static std::pair<bool, std::span<const LayoutID>> walkedLayoutsOf(FunctionPassContext& context, const OwnKeyWalkSite& site)
{
    const Program& program = context.program;
    const Type& type = program.typeOf(context.function, site.source);
    bool areProven = type.hasOnlyLayouts() && (copiesNothingFromNullish(site.walk) || !type.mayBeUndefinedOrNull());
    return { areProven, membersOf(program, areProven ? type : program.expectedOrTypeOf(context.function, site.source).predicted()) };
}

static bool expectsOneLayout(FunctionPassContext& context, const OwnKeyWalkSite& site)
{
    return std::ranges::count_if(walkedLayoutsOf(context, site).second, [&](LayoutID layout) {
        return context.program.at(layout).flow.allocation != AllocationState::NeverAllocated;
    }) == 1;
}

// Once its fields are stored one by one, such an object need not be made.
static bool isKeptByNobody(FunctionPassContext& context, ValueID object)
{
    const EscapeAnalysis& escape = context.analyses.escape();
    std::optional<ObjectID> tracked = escape.gaveUp() ? std::nullopt : escape.objectDefinedBy(object);
    return tracked && std::ranges::none_of(escape.usesOf(*tracked), &ObjectUse::isEscape);
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
    if (!std::ranges::contains(walkedLayoutsOf(context, site).second, layout))
        return { };
    for (uint32_t field = 0; field < context.program.at(layout).fields.size(); ++field) {
        // The bits of such a slot are all there is to its field.
        bool hasPlainSlot = flowOf(context.program, { layout, field }).location.stores != StoreCount::AtMostOnce && isOrdinaryDataField(context.program, { layout, field });
        if (!hasPlainSlot)
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
        coversEveryObject &= expected.coversEveryObject && receiverStateOf(program, layout).impossibleMutations.containsAll(mutationsChangingOwnKeys);
        for (KeyOrderFields& order : expected.orders) {
            if (plan.arms.size() == limits.maxOrders)
                ++numberOfOrdersLeftOut;
            else
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
            return std::ranges::any_of(program.at(arm.layout).fields, [&](const Field& field) {
                return isUnderPrivateName(program, field);
            });
        });
        // A validating build tests the one order that flow has proven: an object with another order reaches the Unreachable behind the arm.
        plan.needsKeyOrderTest = plan.arms.size() > 1 || !hasOnlyFilledFields(program, plan.arms[0]) || (canTestOrders && context.build.validates());
        plan.needsOriginalCall = plan.needsKeyOrderTest && !canTestOrders;
    }

    if (plan.needsKeyOrderTest) {
        plan.arms.removeAllMatching([&](const OwnKeyWalkArm& arm) {
            const Vector<Field>& fields = program.at(arm.layout).fields;
            for (uint32_t field = 0; field < fields.size() && !arm.copiesFieldsAtOnce; ++field) {
                if (isUnderPrivateName(program, fields[field]) && !arm.order.contains(field))
                    return true;
            }
            return false;
        });
        if (plan.arms.isEmpty())
            return std::nullopt;
    }
    return plan;
}

static std::optional<OwnKeyWalkPlan> planSourceWalk(FunctionPassContext& context, const UnchangedAllocations& unchanged, const OwnKeyWalkSite& site)
{
    ValueID id = context.analyses.sameObjectThroughCasts(site.source).valueInFrontOfFacts();
    const Value& allocation = context.function.at(id);
    if (allocation.opcode == Opcode::NewObject) {
        const Layout& layout = context.program.at(allocation.layout());
        bool hasOnlyConstantKeys = std::ranges::all_of(allocation.fields(), [&](uint32_t field) {
            return !!layout.fields[field].key;
        });
        if (hasOnlyConstantKeys && hasOrdinaryOwnKeys(layout.kind) && (unchanged.contains(id, { site.block, site.index }) || context.build.isSabotaged(Sabotage::WalkedLiteralsAreAsTheyWereMade))) {
            OwnKeyWalkPlan plan { .needsShapeTest = false, .needsKeyOrderTest = false, .needsOriginalCall = false, .unchangedAllocation = id };
            plan.arms.append(OwnKeyWalkArm { allocation.layout(), KeyOrderFields(allocation.fields()) });
            return plan;
        }
    }
    LayoutID emptyTarget = layoutOfEmptyTarget(context, unchanged, site);
    if (emptyTarget && isKeptByNobody(context, site.target)) {
        // The operation behind the arms takes the object as it is, so it is made there. A hot loop leaves for a copy of itself on that way. Other code goes on behind the arms, where an object that is made on one way is made on all.
        const NaturalLoop* loop = context.analyses.naturalLoops().innerMostLoopOf(site.block);
        std::optional<OwnKeyWalkPlan> plan = planWalkByLayouts(context, site);
        if (plan && (!plan->needsOriginalCall || (loop && isHot(heatClassByOwnLoops(context, loop->header())).predicted())))
            return plan;
    }
    return planWalkByLayouts(context, site, emptyTarget);
}

// A copy is an ordinary object with ordinary properties under the same names in the same order.
AOT_THRESHOLD(size_t, maxFieldsCopiedByAllocation, 8);

std::optional<OwnKeyWalkPlan> planOwnKeyWalk(FunctionPassContext& context, const UnchangedAllocations& unchanged, const OwnKeyWalkSite& site)
{
    RELEASE_ASSERT(site.walk != OwnKeyWalk::Assign);
    std::optional<OwnKeyWalkPlan> plan = planSourceWalk(context, unchanged, site);
    if (!plan || site.walk != OwnKeyWalk::Copy)
        return plan;
    bool leavesArmsOut = plan->arms.removeAllMatching([&](const OwnKeyWalkArm& arm) {
        const Layout& layout = context.program.at(arm.layout);
        if (layout.kind != LayoutKind::Record || arm.order.size() > maxFieldsCopiedByAllocation)
            return true;
        return !std::ranges::all_of(arm.order, [&](uint32_t field) {
            return isEnumerableStringField(context.program, layout.fields[field]) && isOrdinaryDataField(context.program, { arm.layout, field });
        });
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

namespace {

struct WalkedSource {
    ValueID source;
    OwnKeyWalkPlan plan;
    // It is the last arm of the walk. That of Object.assign assigns this source and every later one.
    Value slowPathCall;
};

// JSON.stringify has no walked source: what it writes is planned when it is expanded.
struct PlannedWalk {
    OwnKeyWalkSite site;
    Vector<WalkedSource, 1> walkedSources;
    std::optional<Value> remainingSourcesCall;
};

} // anonymous namespace

// The stores of a walked source add data properties to a literal that has no setter, so the next source finds it without one.
static bool planAssign(FunctionPassContext& context, const UnchangedAllocations& unchanged, PlannedWalk& walk)
{
    const Program& program = context.program;
    const OwnKeyWalkSite& site = walk.site;
    RELEASE_ASSERT(site.walk == OwnKeyWalk::Assign);
    if (!isUnchangedLiteral(context, unchanged, site.target, { site.block, site.index }))
        return false;

    const Value& original = context.function.at(site.value);
    Vector<Value, 2> calls { original };
    for (size_t first = 0; first < site.laterSources.size(); ++first) {
        Vector<ValueID, 4> arguments { site.target };
        arguments.append(site.laterSources.subspan(first));
        TypingContext typing { program, context.function, context.types };
        BuiltinOperandTypes operands = operandTypesOf(typing, original.receiverOperand(), arguments.span());
        OverloadMatch match = matchOverload({ typing, program.builtins().at(original.overload()).builtin, BuiltinCallMode::Call, operands, operands });
        if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
            return false;

        Value call = original;
        call.setOverload(match.overload);
        call.setArgumentsOperands(arguments.span());
        if (!overloadTakesOperands(program, context.function, call))
            return false;
        calls.append(WTF::move(call));
    }

    OwnKeyWalkSite walked = site;
    for (Value& call : calls) {
        size_t index = walk.walkedSources.size();
        if (index)
            walked.source = site.laterSources[index - 1];
        // What is known of an allocation at the call holds up to the stores of the first source.
        std::optional<OwnKeyWalkPlan> plan = index ? planWalkByLayouts(context, walked) : planSourceWalk(context, unchanged, walked);
        // Set(target, "__proto__", value) gives the target a prototype, whose setters would then run between the reads of the source. The expansion reads every field first.
        bool assignsPrototype = plan && std::ranges::any_of(plan->arms, [&](const OwnKeyWalkArm& arm) {
            const Vector<Field>& fields = program.at(arm.layout).fields;
            return std::ranges::any_of(arm.order, [&](uint32_t field) {
                return fields[field].key && program.nameClassOf(fields[field].key) == NameClass::Proto;
            });
        });
        if (!plan || assignsPrototype)
            break;
        walk.walkedSources.append(WalkedSource { walked.source, WTF::move(*plan), WTF::move(call) });
    }

    size_t numberOfWalkedSources = walk.walkedSources.size();
    if (!numberOfWalkedSources)
        return false;
    if (numberOfWalkedSources < calls.size())
        walk.remainingSourcesCall = WTF::move(calls[numberOfWalkedSources]);
    return true;
}

// The built-in copies the list that the run time keeps for the order, which is the cheaper way to a list that lives on.
static bool isListThatLivesOn(FunctionPassContext& context, const OwnKeyWalkSite& site)
{
    if (site.walk != OwnKeyWalk::Keys)
        return false;
    const EscapeAnalysis& escape = context.analyses.escape();
    std::optional<ObjectID> list = escape.gaveUp() ? std::nullopt : escape.objectDefinedBy(site.value);
    return !list || std::ranges::any_of(escape.usesOf(*list), &ObjectUse::isEscape);
}

// `{ ...source, ..`: the first thing that a new object without a property gets.
static bool fillsEmptyLiteral(const Function& function, const OwnKeyWalkSite& site)
{
    if (site.walk != OwnKeyWalk::Spread)
        return false;
    const Value& target = function.at(site.target);
    if (target.opcode != Opcode::NewObject || !target.fields().empty())
        return false;
    const BlockValues& values = function.at(site.block).values;
    for (uint32_t index = site.index; index--;) {
        if (values[index] == site.target)
            return true;
        if (std::ranges::contains(function.at(values[index]).operands(), site.target))
            return false;
    }
    return false;
}

static bool walksWithoutTests(const OwnKeyWalkPlan& plan)
{
    return plan.arms.size() == 1 && !plan.needsShapeTest && !plan.needsKeyOrderTest && !plan.needsOriginalCall;
}

// The one test is that of a validating build, which holds the proven order against the object.
static bool walksOneProvenOrder(FunctionPassContext& context, const OwnKeyWalkPlan& plan)
{
    if (plan.arms.size() != 1 || plan.needsShapeTest || plan.needsOriginalCall || plan.arms[0].copiesFieldsAtOnce)
        return false;
    return !plan.needsKeyOrderTest || (context.build.validates() && hasOnlyFilledFields(context.program, plan.arms[0]));
}

// The sites are listed last first: an expansion moves only the values after its point.
static Vector<PlannedWalk, 4> planOwnKeyWalks(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<OwnKeyWalkSite, 4> sites;
    Vector<ValueID, 4> fillingLiteralsOutsideHotCode;
    // The operation finds out at run time which names the source holds, and what it has found out serves the next run. Code that runs once, or seldom, pays for that and gets nothing back.
    BitVector copiesThatSeldomRun;
    for (BlockID block : context.analyses.cfg().reversePostOrder() | std::views::reverse) {
        if (function.at(block).frequency.predicted() == BlockFrequency::Rare)
            continue;
        for (uint32_t index = function.at(block).values.size(); index--;) {
            std::optional<OwnKeyWalkSite> site = findSiteAt(context.program, function, { block, index });
            if (!site)
                continue;
            Expected<HeatClass> heat = heatClassByOwnLoops(context, block);
            if (context.runsAtMostOnce(block) || isCold(heat).predicted()) {
                if (site->walk == OwnKeyWalk::Spread || site->walk == OwnKeyWalk::Assign) {
                    copiesThatSeldomRun.set(site->value.index());
                    sites.append(WTF::move(*site));
                }
                continue;
            }
            // The operation copies a record by its words. What the arms add to that, the layout of the copy and no call, is worth their bytes in hot code.
            if (!isCold(heat).predicted() && (site->walk != OwnKeyWalk::Copy || isHot(heat).predicted()) && !isListThatLivesOn(context, *site)) {
                if (!isHot(heat).predicted() && fillsEmptyLiteral(function, *site))
                    fillingLiteralsOutsideHotCode.append(site->value);
                sites.append(WTF::move(*site));
            }
        }
    }
    UnchangedAllocations unchanged(context, sites.span());

    Vector<PlannedWalk, 4> walks;
    for (OwnKeyWalkSite& site : sites) {
        PlannedWalk walk { WTF::move(site), { }, { } };
        bool seldomRuns = copiesThatSeldomRun.get(walk.site.value.index());
        if (walk.site.walk == OwnKeyWalk::Assign) {
            if (!planAssign(context, unchanged, walk))
                continue;
            if (seldomRuns && (walk.remainingSourcesCall || !std::ranges::all_of(walk.walkedSources, [&](const WalkedSource& source) { return walksOneProvenOrder(context, source.plan); })))
                continue;
        } else if (isExpandedByPlan(walk.site.walk)) {
            std::optional<OwnKeyWalkPlan> plan = planOwnKeyWalk(context, unchanged, walk.site);
            if (!plan || (seldomRuns && !walksOneProvenOrder(context, *plan)))
                continue;
            // A load and a store for each field take the place of the call. Arms behind tests, with the call behind them, are worth their bytes in hot code, or where they leave no object to make.
            if (fillingLiteralsOutsideHotCode.contains(walk.site.value) && !walksWithoutTests(*plan) && !isKeptByNobody(context, walk.site.target)) {
                // The copy of all slots at once is a few words of code in front of the call, which is then laid out as the rare way: for a source that is expected to have the layout of the literal and no other.
                if (!expectsOneLayout(context, walk.site) || !plan->arms[0].copiesFieldsAtOnce)
                    continue;
            }
            walk.walkedSources.append(WalkedSource { walk.site.source, WTF::move(*plan), function.at(walk.site.value) });
        }
        walks.append(WTF::move(walk));
    }
    return walks;
}

// The list a walk makes is new, so an earlier pass may have named the call as the guard for what holds of a new array, and a value that was named as a guard has to stay one.
static void keepResultAsGuard(IRBuilder& builder, IRBuilder::Expansion& expansion)
{
    if (expansion.results.size() != 1)
        return;

    Function& function = builder.function();
    ValueID guard;
    // Puts the guard, once there is one, in the place of the result.
    auto isNamedAsGuard = [&, result = expansion.results[0]] {
        bool isNamed = false;
        auto visit = [&](auto& user) {
            user.forEachOperand([&](ValueID& operand, uint32_t operandIndex) {
                if (user.operandKind(operandIndex) != OperandKind::Guard || operand != result)
                    return;
                isNamed = true;
                if (guard)
                    operand = guard;
            });
        };
        for (Block& block : function.editAllBlocks()) {
            for (ValueID value : block.values)
                visit(function.edit(value));
            visit(block.terminator);
        }
        return isNamed;
    };
    if (!isNamedAsGuard())
        return;
    guard = builder.addParameter(expansion.continuation, TypeTable::guard);
    isNamedAsGuard();
    expansion.results.append(guard);
}

static ValueID appendOriginalOperation(IRBuilder& builder, const Value& original)
{
    if (resultRuleOf(original.opcode) == ResultRule::Declared)
        return builder.appendWithType(Value(original), original.type);
    return builder.append(Value(original));
}

static Emission::TestedObject emitArmTests(Emission& emission, ValueID source, const OwnKeyWalkPlan& plan, const OwnKeyWalkArm& arm, BlockID nextArm)
{
    Emission::TestedObject object { source, { } };
    if (plan.needsShapeTest)
        object = emission.emitShapeTest(source, singleElementSpan(arm.layout), nextArm);
    if (plan.needsKeyOrderTest && !emission.inputs().build.isSabotaged(Sabotage::WalkedKeysStandInTheFirstOrder)) {
        object.guard = emission.emitKeyOrderTest(object, arm.layout, arm.order.span(), nextArm);
        return object;
    }
    if (object.guard || !plan.unchangedAllocation)
        return object;
    const Type& type = emission.typeOf(object.object);
    bool needsGuardForReads = type.mayBeUndefinedOrNull() || type.singleLayout(emission.program()) != arm.layout || !hasOnlyFilledFields(emission.program(), arm);
    if (!needsGuardForReads)
        return object;
    return { plan.unchangedAllocation, plan.unchangedAllocation };
}

static bool visitsSymbolKeys(OwnKeyWalk walk)
{
    switch (walk) {
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread:
    case OwnKeyWalk::Copy:
        return true;
    case OwnKeyWalk::Keys:
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID emitWalk(Emission& emission, const OwnKeyWalkSite& site, const Value& original, const Emission::TestedObject& object, const OwnKeyWalkArm& arm)
{
    using Values = Vector<ValueID, 8>;
    IRBuilder& builder = emission.builder();
    const Program& program = emission.program();
    const Vector<Field>& fields = program.at(arm.layout).fields;
    bool isAssign = site.walk == OwnKeyWalk::Assign;
    std::span<const PropertyKeyID> excludedKeys = site.walk == OwnKeyWalk::Spread ? original.excludedKeys() : std::span<const PropertyKeyID>();

    // A copy gets its properties in the order in which the source got them, names and symbols alike.
    Vector<uint32_t, 8> walkedFields;
    for (uint32_t index : arm.order) {
        const Field& field = fields[index];
        RELEASE_ASSERT(!field.computedKey);
        if (std::ranges::contains(excludedKeys, field.key))
            continue;
        if (isEnumerableStringField(program, field) || (visitsSymbolKeys(site.walk) && field.key && isSymbolKind(program.at(field.key).kind)))
            walkedFields.append(index);
    }

    auto appendName = [&](uint32_t field) {
        return emission.appendString(program.at(fields[field].key).text);
    };
    auto appendRead = [&](uint32_t field) {
        if (flowOf(program, { arm.layout, field }).location.deferral == ClosureDeferral::Unknown)
            return emission.appendFieldRead(object, arm.layout, field);
        return builder.append(Emission::make<&Value::setKey, &Value::setReceivers, &Value::setLevel>(Opcode::GetDeferredClosureSlot, { object.object, object.guard },
            fields[field].key, emission.layoutSetOf(singleElementSpan(arm.layout)), object.guard ? KnowledgeLevel::Expected : KnowledgeLevel::Proven));
    };
    auto elementTypeOf = [&](TypeID arrayType) {
        const ClassPart* part = program.at(arrayType).findClassPart(BuiltinClasses::Array);
        return part ? part->writtenContentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything;
    };
    auto appendArray = [&](TypeID contents, std::span<const ValueID> elements) {
        return builder.append(Emission::make<&Value::setContents, &Value::setElementsOperands>(Opcode::NewArray, { }, contents, elements));
    };

    switch (site.walk) {
    case OwnKeyWalk::Keys:
        return appendArray(elementTypeOf(original.type), walkedFields.map<Values>(appendName).span());
    case OwnKeyWalk::Values:
        return appendArray(elementTypeOf(site.provenTypeOfList ? site.provenTypeOfList : original.type), walkedFields.map<Values>(appendRead).span());
    case OwnKeyWalk::Entries: {
        TypeID entryType = elementTypeOf(original.type);
        TypeID entryContents = elementTypeOf(entryType);
        Values entries = walkedFields.map<Values>([&](uint32_t field) {
            std::array<ValueID, 2> entry { appendName(field), appendRead(field) };
            return appendArray(entryContents, entry);
        });
        return appendArray(entryType, entries.span());
    }
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread: {
        Values values = walkedFields.map<Values>(appendRead);
        // A read by layout knows what any object of the layout holds. What flow has proven of the fields of the target it has proven of this copy too.
        std::span<const LayoutID> layoutsOfTarget = membersOf(program, emission.typeOf(site.target));
        if (!isAssign && emission.typeOf(site.target).hasOnlyLayouts() && layoutsOfTarget.size() == 1 && program.at(layoutsOfTarget[0]).flow.impossibleMutations.contains(ObjectMutation::StoreOfOtherType)) {
            const Layout& layoutOfTarget = program.at(layoutsOfTarget[0]);
            for (size_t index = 0; index < values.size(); ++index) {
                std::optional<uint32_t> field = layoutOfTarget.findField(fields[walkedFields[index]].key);
                std::optional<ProvenInEveryVersion> established = field ? layoutOfTarget.fields[*field].declared.established() : std::nullopt;
                if (!established)
                    continue;
                TypeID read = builder.function().at(values[index]).type;
                TypeID held = emission.inputs().types.intersectionOf(read, *established);
                if (held != read && held != TypeTable::never)
                    values[index] = builder.append(makeAssumeType(values[index], Proven::byFlowOfPosition(held)));
            }
        }
        for (size_t index = 0; index < values.size(); ++index) {
            Value store = IRBuilder::make(isAssign ? Opcode::PropSet : Opcode::PropDefine, { site.target, values[index] });
            store.setKey(fields[walkedFields[index]].key);
            if (isAssign)
                store.setMode(ECMAMode::Strict);
            builder.append(WTF::move(store));
        }
        return isAssign ? site.target : ValueID();
    }
    // The copy has the layout of its source, and gets its fields in the order in which the source got them.
    case OwnKeyWalk::Copy:
        return builder.append(Emission::make<&Value::setLayout, &Value::setFields, &Value::setValuesOperands>(Opcode::NewObject, { }, arm.layout, walkedFields.span(), walkedFields.map<Values>(appendRead).span()));
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A walk that is done goes on at afterWalk. Without one it ends the expansion, as the original operation always does.
static void emitArms(Emission& emission, const OwnKeyWalkSite& site, const WalkedSource& walked, const IRBuilder::Expansion& expansion, BlockID afterWalk)
{
    const OwnKeyWalkPlan& plan = walked.plan;
    RELEASE_ASSERT(!plan.arms.isEmpty());
    IRBuilder& builder = emission.builder();
    const Function& function = builder.function();
    Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;

    auto endExpansion = [&](ValueID result) {
        Vector<ValueID, 2> listAndGuard;
        if (!expansion.results.isEmpty())
            listAndGuard.append(result);
        if (expansion.results.size() > 1)
            listAndGuard.append(builder.append(Opcode::AllGuards, { result }));
        builder.endExpansion(expansion, listAndGuard.span());
    };
    auto endArm = [&](ValueID result) {
        if (afterWalk)
            builder.emitJump(afterWalk);
        else
            endExpansion(result);
    };

    ValueID source = walked.source;
    if (copiesNothingFromNullish(site.walk) && !plan.unchangedAllocation && emission.typeOf(source).mayBeUndefinedOrNull()) {
        BlockID firstArm;
        if (emission.typeOf(source).isUndefinedOrNull()) {
            firstArm = builder.addBlock(frequency);
            emission.emitTypeTest(source, TypeTable::nullish, firstArm);
        } else {
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(source, TypeTable::nullish), 0, frequency, frequency);
            firstArm = branch.notTaken;
            source = branch.provenOnNotTaken;
        }
        endArm(site.walk == OwnKeyWalk::Assign ? site.target : ValueID());
        builder.moveToEndOf(firstArm);
    }

    bool hasFailingEdges = plan.needsShapeTest || plan.needsKeyOrderTest;
    BlockID afterLastArm = hasFailingEdges || plan.needsOriginalCall ? builder.addBlock(BlockFrequency::Rare) : BlockID();
    for (const OwnKeyWalkArm& arm : plan.arms) {
        BlockID nextArm = &arm == &plan.arms.last() ? afterLastArm : builder.addBlock(frequency);
        if (arm.copiesFieldsAtOnce) {
            Value copy = Emission::make<&Value::setLayout>(Opcode::TryCopyFields, { site.target, emission.emitTypeTest(source, TypeTable::object, nextArm) }, arm.layout);
            IRBuilder::BranchBlocks blocks = builder.emitBranch(builder.append(WTF::move(copy)), function.at(builder.currentBlock()).frequency, function.at(nextArm).frequency);
            builder.moveToEndOf(blocks.notTaken);
            builder.emitJump(nextArm);
            builder.moveToEndOf(blocks.taken);
            endArm({ });
        } else
            endArm(emitWalk(emission, site, walked.slowPathCall, emitArmTests(emission, source, plan, arm, nextArm), arm));
        if (nextArm)
            builder.moveToEndOf(nextArm);
    }

    // Without the original operation, flow has proven that every object passes the tests of one arm.
    if (plan.needsOriginalCall)
        endExpansion(appendOriginalOperation(builder, walked.slowPathCall));
    else if (afterLastArm)
        builder.emitUnreachable();
}

static void expandWalk(Emission& emission, const PlannedWalk& walk)
{
    IRBuilder& builder = emission.builder();
    const OwnKeyWalkSite& site = walk.site;
    Expected<BlockFrequency> frequency = builder.function().at(site.block).frequency;
    IRBuilder::Expansion expansion = builder.beginExpansion({ site.block, site.index });
    if (site.walk != OwnKeyWalk::Assign)
        keepResultAsGuard(builder, expansion);

    for (const WalkedSource& walked : walk.walkedSources) {
        bool isLastOperation = &walked == &walk.walkedSources.last() && !walk.remainingSourcesCall;
        BlockID afterWalk = isLastOperation ? BlockID() : builder.addBlock(frequency);
        emitArms(emission, site, walked, expansion, afterWalk);
        if (afterWalk)
            builder.moveToEndOf(afterWalk);
    }
    if (walk.remainingSourcesCall)
        builder.endExpansion(expansion, appendOriginalOperation(builder, *walk.remainingSourcesCall));
}

static PassOutcome expandStringify(FunctionPassContext& context, Emission& emission, const OwnKeyWalkSite& site)
{
    IRBuilder& builder = emission.builder();
    JSONSerializerBuilder planner(builder, emission.inputs(), ValueID());
    if (!planner.hasPlanFor(site.source))
        return PassOutcome::Unchanged;
    // A string builder for one string, number, boolean or null costs three times what the stub of the call needs to write it.
    if (site.walk == OwnKeyWalk::Stringify && (!emission.typeOf(site.source).hasObjects() || planner.writesOnePrimitive(site.source)))
        return PassOutcome::Unchanged;

    std::optional<BuiltinCallPlan> textFormCall;
    if (site.walk == OwnKeyWalk::StringifyIntoText) {
        TypingContext typing { context.program, context.function, context.types };
        BuiltinOperandTypes operands = operandTypesOf(typing, builtinCallOf(context.program, context.function.at(site.value)));
        operands.arguments[site.sourceArgument] = TypeTable::string;
        textFormCall = planBuiltinCall({ { typing, site.textForm, BuiltinCallMode::Call, operands, operands }, context.analyses.effectContext() });
        bool passesItsOperandsUnchanged = std::ranges::all_of(textFormCall->steps, [](const OperandStep& step) {
            switch (step.kind) {
            case OperandStepKind::Pass:
            case OperandStepKind::PassUndefined:
                return true;
            case OperandStepKind::Convert:
            case OperandStepKind::RequireObjectCoercible:
                return false;
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
        if (!textFormCall->isProven() || !textFormCall->requirements.isEmpty() || !passesItsOperandsUnchanged || site.sourceArgument >= textFormCall->numberOfArguments)
            return PassOutcome::Unchanged;
    }

    const Value original = context.function.at(site.value);
    IRBuilder::Expansion expansion = builder.beginExpansion({ site.block, site.index });
    BlockID generic = builder.addBlock(BlockFrequency::Rare);
    BlockID noText = builder.addBlock(BlockFrequency::Rare);

    ValueID stringBuilder = builder.append(Opcode::StringBuilderBegin, { });
    JSONSerializerBuilder serializer(builder, emission.inputs(), stringBuilder, calledBuiltinOf(context.program.builtins(), original));

    bool listsHolders = serializer.mayListHolders(site.source);
    bool isCovered = listsHolders || (!serializer.writingRunsCode(site.source) && serializer.mayHoldItself(site.source));
    Expected<BlockFrequency> frequency = builder.function().at(builder.currentBlock()).frequency;
    std::optional<Target> outerHandler = builder.handler();
    BlockID threw;
    if (isCovered) {
        threw = builder.addHandlerBlock(listsHolders ? BlockRole::CleanupHandler : BlockRole::CatchHandler, ValueID());
        builder.setHandler(Target { threw, { }, { } });
        BlockID covered = builder.addBlock(frequency);
        builder.emitJump(covered);
        builder.moveToEndOf(covered);
    }

    serializer.appendRoot(site.source, noText);
    serializer.commit();

    ValueID result = builder.append(Opcode::StringBuilderFinish, { stringBuilder });
    if (isCovered) {
        builder.setHandler(WTF::move(outerHandler));
        BlockID finished = builder.addBlock(frequency);
        builder.emitJump(finished);

        builder.moveToEndOf(threw);
        if (listsHolders) {
            ValueID exception = builder.append(Opcode::CatchException, { });
            builder.append(Opcode::JSONResetHolders, { stringBuilder });
            builder.emitRethrow(exception);
        } else {
            // No code of the program has run, so the call as written starts from the beginning. It finds the cycle, or that the text is too long.
            builder.append(Opcode::CatchValue, { });
            builder.emitJump(noText);
        }
        builder.moveToEndOf(finished);
    }
    if (textFormCall) {
        BuiltinCallOperands operands { original.receiverOperand(), Vector<ValueID, 4>(original.argumentsOperands()) };
        operands.arguments[site.sourceArgument] = result;
        BuiltinCallOperands passedOperands = insertOperandSteps(*textFormCall, operands, { }, [&](auto&& value) {
            return builder.append(WTF::move(value));
        });
        BuiltinCallStamps stamps { original.expectedResult(), original.accessSite(), original.allocationSite() };
        result = builder.append(makeBuiltinCall(textFormCall->overload, BuiltinCallForm::Whole, passedOperands, stamps));
    }
    builder.endExpansion(expansion, convertTo(builder, result, DeclaredType(Proven::byPosition(original.type))));

    builder.moveToEndOf(noText);
    builder.append(Opcode::StringBuilderDiscard, { stringBuilder });
    builder.emitJump(generic);

    builder.moveToEndOf(generic);
    builder.endExpansion(expansion, appendOriginalOperation(builder, original));
    return PassOutcome::Changed;
}

static bool hasNoEffect(FunctionPassContext& context, ValueID id)
{
    return context.function.at(id).opcode == Opcode::LoopHint || context.analyses.isUnobservable(id);
}

// A turn that finds the object changed starts over at the header of the loop as written, so nothing in front of the step may have happened twice.
static ValueID findStepAtHeadOf(FunctionPassContext& context, BlockID header)
{
    for (ValueID id : context.function.at(header).values) {
        if (context.function.at(id).opcode == Opcode::ForInStep)
            return id;
        if (!hasNoEffect(context, id))
            return { };
    }
    return { };
}

static BlockID findOnlyEntryOf(FunctionPassContext& context, const NaturalLoop& loop)
{
    BlockID entry;
    for (BlockID predecessor : context.analyses.cfg().predecessors(loop.header())) {
        if (context.analyses.naturalLoops().belongsTo(predecessor, loop))
            continue;
        if (entry)
            return { };
        entry = predecessor;
    }
    if (!entry || context.function.at(entry).terminator.kind != TerminatorKind::Jump)
        return { };
    return entry;
}

// Between the open and the entry of the loop nothing may change the object: the tests of an arm say what the enumerator lists. Blocks that only jump on may lie between the two.
static bool leadsToEntryWithoutEffect(FunctionPassContext& context, ValueID open, BlockID entry)
{
    const Function& function = context.function;
    for (BlockID block = entry;;) {
        const BlockValues& values = function.at(block).values;
        size_t first = values.contains(open) ? values.find(open) + 1 : 0;
        for (size_t index = first; index < values.size(); ++index) {
            if (!hasNoEffect(context, values[index]))
                return false;
        }
        if (first)
            return true;
        auto predecessors = context.analyses.cfg().predecessors(block);
        if (predecessors.size() != 1 || function.at(predecessors[0]).terminator.kind != TerminatorKind::Jump)
            return false;
        block = predecessors[0];
    }
}

static constexpr OptionSet<BlockEdit> editsOfTargets { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments };

// `out[name] = record[name]` under a type: the value is tested, and the arm of the failing test is unlikely.
static PassOutcome rejoinSeparatedLoop(FunctionPassContext& context, const NaturalLoop& loop)
{
    Function& function = context.function;
    if (!findStepAtHeadOf(context, loop.header()))
        return PassOutcome::Unchanged;
    BlockID second = findOnlyEntryOf(context, loop);
    if (!second || !function.at(second).values.isEmpty() || function.at(second).handler != function.at(loop.header()).handler)
        return PassOutcome::Unchanged;
    const Target& onward = function.at(second).terminator.targets[0];
    if (!std::ranges::equal(onward.arguments, function.at(second).parameters))
        return PassOutcome::Unchanged;

    Vector<BlockID, 4> ends;
    for (BlockID predecessor : context.analyses.cfg().predecessors(second)) {
        if (context.analyses.dominators().dominates(loop.header(), predecessor))
            ends.append(predecessor);
    }
    if (ends.isEmpty())
        return PassOutcome::Unchanged;
    for (BlockID end : ends) {
        for (Target& target : function.edit(end, editsOfTargets).terminator.targets) {
            if (target.block == second)
                target.block = loop.header();
        }
    }
    context.analyses.didChangeControlFlow();
    return PassOutcome::Changed;
}

// One loop at a time: an edit changes the blocks that the loops are made of.
static PassOutcome editLoopsOneAtATime(FunctionPassContext& context, NOESCAPE const Invocable<PassOutcome(const NaturalLoop&)> auto& edit)
{
    for (PassOutcome outcome = PassOutcome::Unchanged;; outcome = PassOutcome::Changed) {
        NaturalLoops& loops = context.analyses.naturalLoops();
        PassOutcome outcomeOfRound = PassOutcome::Unchanged;
        for (unsigned index = 0; outcomeOfRound == PassOutcome::Unchanged && index < loops.numLoops(); ++index)
            outcomeOfRound = edit(loops.loop(index));
        if (outcomeOfRound == PassOutcome::Unchanged)
            return outcome;
    }
}

namespace {

struct ForInLoop {
    BlockID header;
    BlockID entry;
    ValueID step;
    ValueID open;
    ValueID source;
    std::span<const BlockID> region;
};

} // anonymous namespace

static bool isExpectedToBeList(const Program& program, const Function& function, ValueID source)
{
    return program.expectedOrTypeOf(function, source).predicted().findClassPart(BuiltinClasses::Array);
}

// `list[key]` under the text of an index is `list[index]`, whatever the object is.
static void readUnderIndexInPlaceOfItsText(Function& function, std::span<const BlockID> blocks, ValueID text, ValueID index)
{
    auto isText = [&](ValueID key) {
        while (key != text && isCastWithoutTest(function.at(key).opcode))
            key = function.at(key).operand(0);
        return key == text;
    };
    for (BlockID block : blocks) {
        for (ValueID id : function.at(block).values) {
            const Value& access = function.at(id);
            if (elementAccessOf(access.opcode) && isText(access.propertyOperand()))
                function.edit(id).setPropertyOperand(index);
        }
    }
}

// A for-in loop over a list gets a copy of its body for the indexed part of the enumerator. There the key is an integer, a read under it is a read of an element, and its text is made only for
// whoever else asks for it. A turn that finds no element in the storage, and the turn behind the last index, go on in the loop as written, from the same position.
static PassOutcome expandForInLoopOverList(FunctionPassContext& context, Emission& emission, const ForInLoop& loop)
{
    const Program& program = context.program;
    Function& function = context.function;
    IRBuilder& builder = emission.builder();
    // The copy is as much code again as the loop.
    if (!isHot(heatClassOf(context, loop.header)).predicted())
        return PassOutcome::Unchanged;
    uint64_t bytesOfRegion = 0;
    for (BlockID block : loop.region)
        bytesOfRegion += estimatedCodeBytes(program, function, function.at(block));
    if (context.exceeds(LimitID::MaxVersionedIterationBytes, bytesOfRegion, loop.open))
        return PassOutcome::Unchanged;

    builder.setOrigin(function.at(loop.open).origin());
    Vector<std::unique_ptr<RegionCloner>, 1> cloners;
    cloners.append(makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, function));
    RegionCloner& cloner = *cloners.first();
    cloner.cloneBlocks(loop.region, [](ValueID, Value&) { });
    BlockID header = cloner.cloneOf(loop.header);
    ValueID step = cloner.cloneOf(loop.step);

    // The region may hold a cycle with a step of its own, which goes on to any position. The loop as written takes them all.
    size_t positionIndex = function.at(header).parameters.find(function.at(step).positionOperand());
    auto isPositionBehindStep = [&](ValueID position) {
        const Value& next = function.at(definitionThroughUpcasts(function, position));
        if (next.opcode != Opcode::Add)
            return false;
        const Value& found = function.at(next.operand(0));
        const Value& distance = function.at(next.operand(1));
        return found.opcode == Opcode::ResultAt && found.operand(OperandIndex::ResultAt::results) == step && found.index() == static_cast<uint32_t>(ForInStepResult::Position)
            && distance.opcode == Opcode::Integer && distance.integer() == 1;
    };
    for (BlockID block : cloner.clonedBlocks()) {
        for (Target& target : function.edit(block, editsOfTargets).terminator.targets) {
            if (target.block == header && !(positionIndex < target.arguments.size() && isPositionBehindStep(target.arguments[positionIndex])))
                target.block = loop.header;
        }
    }
    context.analyses.didChangeControlFlow();

    // An enumerator without an indexed part goes straight to the loop as written.
    Target toLoop = function.at(loop.entry).terminator.targets[0];
    RELEASE_ASSERT(positionIndex < toLoop.arguments.size());
    function.edit(loop.entry, editsOfTargets).terminator = Terminator();
    context.analyses.didChangeControlFlow();
    builder.setHandler(function.at(loop.entry).handler);
    builder.moveToEndOf(loop.entry);
    ValueID indexedLength = builder.append(Opcode::GetEnumeratorIndexedLength, { loop.open });
    ValueID hasIndexedPart = builder.append(Opcode::Less, { toLoop.arguments[positionIndex], indexedLength });
    builder.emitBranch(hasIndexedPart, Target { header, Arguments(toLoop.arguments), { } }, Target { loop.header, Arguments(toLoop.arguments), { } });
    RegionCloner::repairSSA(context.analyses, cloners.span());

    // The repair splits a block that has a handler after each value that other blocks read, so the step may have left the header.
    ValueLocation placeOfStep = context.analyses.useLists().locationOf(step);
    RELEASE_ASSERT(placeOfStep.placement == ValuePlacement::InBlock);

    Arguments carriedValues(function.at(header).parameters);
    ValueID object = function.at(step).objectOperand();
    ValueID position = function.at(step).positionOperand();
    Expected<BlockFrequency> frequency = function.at(header).frequency;
    Expected<BlockFrequency> unlikely = rarerOf(frequency, BlockFrequency::Unlikely);
    builder.setOrigin(function.at(step).origin());
    IRBuilder::Expansion expansion = builder.beginExpansion({ placeOfStep.block, placeOfStep.index });
    // A later copy of these blocks without their proofs takes the position for any number. What goes on to a step is an integer by the operation that makes it.
    auto goOnInLoopAsWritten = [&] {
        Arguments arguments(carriedValues);
        arguments[positionIndex] = builder.append(Opcode::ToEnumeratorPosition, { position });
        builder.emitJump(loop.header, WTF::move(arguments));
    };
    if (BlockID failedUnasked = builder.beginPrediction()) {
        BlockID tests = builder.currentBlock();
        builder.moveToEndOf(failedUnasked);
        goOnInLoopAsWritten();
        builder.moveToEndOf(tests);
    }
    IRBuilder::ProvenBranch isIndex = builder.emitBranchWithProof(builder.append(Opcode::Less, { position, indexedLength }), 0, frequency, unlikely);
    builder.moveToEndOf(isIndex.notTaken);
    goOnInLoopAsWritten();
    builder.moveToEndOf(isIndex.taken);
    ValueID index = isIndex.provenOnTaken;
    IRBuilder::BranchBlocks isThere = builder.emitBranch(builder.append(Opcode::IsElementInStorage, { object, index }), frequency, unlikely);
    builder.moveToEndOf(isThere.notTaken);
    goOnInLoopAsWritten();
    builder.moveToEndOf(isThere.taken);
    ValueID key = builder.append(Opcode::ToString, { index });
    std::array<ValueID, 3> results { builder.appendBoolean(false), key, index };
    builder.endExpansion(expansion, results);

    Vector<BlockID> blocksOfCopy(cloner.clonedBlocks());
    blocksOfCopy.append(expansion.continuation);
    readUnderIndexInPlaceOfItsText(function, blocksOfCopy.span(), expansion.results[static_cast<size_t>(ForInStepResult::Key)], index);
    context.analyses.didChangeValues();
    context.build.statistics().count(StatisticID::OwnKeyWalksExpanded);
    return PassOutcome::Changed;
}

// A for-in loop over an object of a few expected layouts gets, per layout and key order, one copy of its body for each name.
static PassOutcome expandForInLoop(FunctionPassContext& context, Emission& emission, const NaturalLoop& naturalLoop)
{
    const Program& program = context.program;
    Function& function = context.function;
    IRBuilder& builder = emission.builder();
    BlockID header = naturalLoop.header();
    if (function.at(header).frequency.predicted() == BlockFrequency::Rare || context.runsAtMostOnce(header))
        return PassOutcome::Unchanged;

    ValueID step = findStepAtHeadOf(context, header);
    if (!step || !function.at(step).positionOperand())
        return PassOutcome::Unchanged;
    ValueID open = function.at(step).enumeratorOperand();
    ValueID source = function.at(step).objectOperand();
    if (function.at(open).opcode != Opcode::ForInOpen || function.at(open).objectOperand() != source)
        return PassOutcome::Unchanged;

    BlockID entry = findOnlyEntryOf(context, naturalLoop);
    if (!entry || !leadsToEntryWithoutEffect(context, open, entry))
        return PassOutcome::Unchanged;

    Vector<BlockID> region;
    for (unsigned index = 0; index < naturalLoop.size(); ++index)
        region.append(naturalLoop.at(index));
    if (RegionCloner::checkCanDuplicate(context.analyses, region.span(), singleElementSpan(header)) != CloneCheck::CanClone)
        return PassOutcome::Unchanged;

    std::optional<OwnKeyWalkPlan> plan = planWalkByLayouts(context, OwnKeyWalkSite { .value = open, .source = source });
    if (!plan && isExpectedToBeList(program, function, source))
        return expandForInLoopOverList(context, emission, { header, entry, step, open, source, region.span() });
    if (!plan)
        return PassOutcome::Unchanged;

    struct Arm {
        const OwnKeyWalkArm& tested;
        Vector<PropertyKeyID, 8> names;
    };
    Vector<Arm, 4> arms;
    uint64_t numberOfCopies = 0;
    for (const OwnKeyWalkArm& tested : plan->arms) {
        Vector<PropertyKeyID, 8> names;
        for (uint32_t index : tested.order) {
            const Field& field = program.at(tested.layout).fields[index];
            if (isEnumerableStringField(program, field))
                names.append(field.key);
        }
        // An object without names is done at the first step of the loop as written.
        if (names.isEmpty())
            continue;
        numberOfCopies += names.size();
        arms.append(Arm { tested, WTF::move(names) });
    }

    // `"key" !== name`: in a copy for one name the answer is known, and the comparison is gone.
    auto comparesNameWithText = [&](const Value& value) {
        auto comparesInThisOrder = [&](ValueID name, ValueID text) {
            const Value& result = function.at(name);
            return result.opcode == Opcode::ResultAt && result.index() == 1 && result.operand(OperandIndex::ResultAt::results) == step && function.at(text).opcode == Opcode::String;
        };
        return value.opcode == Opcode::StrictEq && (comparesInThisOrder(value.operand(0), value.operand(1)) || comparesInThisOrder(value.operand(1), value.operand(0)));
    };
    uint64_t bytesOfRegion = 0;
    for (BlockID block : region) {
        bytesOfRegion += estimatedCodeBytes(program, function, function.at(block));
        for (ValueID id : function.at(block).values) {
            if (comparesNameWithText(function.at(id)))
                bytesOfRegion -= estimatedCodeBytes(program, function, function.at(id));
        }
    }
    // The arms that fit are expanded, and the objects of the others take the loop as written.
    if (context.exceeds(LimitID::MaxVersionedIterationBytes, bytesOfRegion * numberOfCopies, open)) {
        uint64_t maxCopies = context.limit(LimitID::MaxVersionedIterationBytes) / std::max<uint64_t>(bytesOfRegion, 1);
        size_t numberOfArms = 0;
        for (uint64_t copies = 0; numberOfArms < arms.size(); ++numberOfArms) {
            copies += arms[numberOfArms].names.size();
            if (copies > maxCopies)
                break;
        }
        arms.shrink(numberOfArms);
    }
    if (arms.isEmpty())
        return PassOutcome::Unchanged;

    struct Turn {
        BlockID header;
        ValueID step;
        PropertyKeyID name;
        size_t position;
    };
    Vector<Turn, 8> turns;
    Vector<std::unique_ptr<RegionCloner>, 8> cloners;
    auto isPositionBehind = [&](ValueID stepOfTurn, ValueID position) {
        const Value& next = function.at(definitionThroughUpcasts(function, position));
        if (next.opcode != Opcode::Add)
            return false;
        const Value& found = function.at(next.operand(0));
        const Value& distance = function.at(next.operand(1));
        return found.opcode == Opcode::ResultAt && found.operand(OperandIndex::ResultAt::results) == stepOfTurn && found.index() == static_cast<uint32_t>(ForInStepResult::Position)
            && distance.opcode == Opcode::Integer && distance.integer() == 1;
    };

    // The turns of an arm stand one behind the other, each with its name as a constant. The one behind the last goes on in the loop as written, which finds what the prototypes list, or the end.
    builder.setOrigin(function.at(open).origin());
    Vector<BlockID, 4> firstTurnOfArm;
    for (const Arm& arm : arms) {
        BlockID next = header;
        for (size_t position = arm.names.size(); position--;) {
            auto cloner = makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, function);
            cloner->cloneBlocks(region.span(), [](ValueID, Value&) { });
            Turn turn { cloner->cloneOf(header), cloner->cloneOf(step), arm.names[position], position };

            // A turn is on no cycle of its own.
            BitVector loopHints;
            const BlockValues& values = function.at(turn.header).values;
            for (uint32_t index = 0; values[index] != turn.step; ++index) {
                if (function.at(values[index]).opcode == Opcode::LoopHint)
                    loopHints.set(index);
            }
            builder.eraseValues(turn.header, loopHints);

            size_t positionIndex = function.at(turn.header).parameters.find(function.at(turn.step).positionOperand());
            for (BlockID block : cloner->clonedBlocks()) {
                for (Target& target : function.edit(block, editsOfTargets).terminator.targets) {
                    if (target.block != turn.header)
                        continue;
                    // The region may hold a cycle with a step of its own, which goes on to any position. The loop as written takes them all.
                    bool isAtNextPosition = positionIndex < target.arguments.size() && isPositionBehind(turn.step, target.arguments[positionIndex]);
                    target.block = isAtNextPosition ? next : header;
                }
            }
            turns.append(turn);
            cloners.append(WTF::move(cloner));
            next = turn.header;
        }
        firstTurnOfArm.append(next);
    }
    context.analyses.didChangeControlFlow();

    Target toLoop = function.at(entry).terminator.targets[0];
    function.edit(entry, editsOfTargets).terminator = Terminator();
    context.analyses.didChangeControlFlow();
    builder.setHandler(function.at(entry).handler);
    builder.moveToEndOf(entry);

    Expected<BlockFrequency> frequencyOfEntry = function.at(entry).frequency;
    for (size_t index = 0; index < arms.size(); ++index) {
        BlockID nextArm = builder.addBlock(frequencyOfEntry);
        const OwnKeyWalkArm& arm = arms[index].tested;
        emission.emitKeyOrderTest(emission.emitShapeTest(source, singleElementSpan(arm.layout), nextArm), arm.layout, arm.order.span(), nextArm);
        builder.emitJump(firstTurnOfArm[index], Arguments(toLoop.arguments));
        builder.moveToEndOf(nextArm);
    }
    builder.emitJump(toLoop.block, Arguments(toLoop.arguments));
    RegionCloner::repairSSA(context.analyses, cloners.span());

    // While the object is as the enumerator found it, the step answers with the name at its position. Otherwise the turn is taken by the loop as written, from the same position on.
    for (const Turn& turn : turns) {
        // The repair splits a block that has a handler after each value that other blocks read, so the step may have left the header. A turn that no edge enters has no place.
        ValueLocation placeOfStep = context.analyses.useLists().locationOf(turn.step);
        if (placeOfStep.placement != ValuePlacement::InBlock)
            continue;

        Arguments carriedValues(function.at(turn.header).parameters);
        ValueID enumerator = function.at(turn.step).enumeratorOperand();
        ValueID object = function.at(turn.step).objectOperand();
        Expected<BlockFrequency> frequency = function.at(turn.header).frequency;
        builder.setOrigin(function.at(turn.step).origin());
        IRBuilder::Expansion expansion = builder.beginExpansion({ placeOfStep.block, placeOfStep.index });
        builder.emitFailureOfPredictions(Target { header, Arguments(carriedValues), { } });
        ValueID position = builder.appendInteger(turn.position);
        ValueID isCurrent = builder.append(Opcode::IsEnumeratorCurrent, { enumerator, object, position });
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isCurrent, 0, frequency, rarerOf(frequency, BlockFrequency::Unlikely));
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(header, WTF::move(carriedValues));
        builder.moveToEndOf(branch.taken);
        std::array<ValueID, 3> results { builder.appendBoolean(false), emission.appendString(program.at(turn.name).text), position };
        builder.endExpansion(expansion, results);
    }
    context.build.statistics().count(StatisticID::OwnKeyWalksExpanded, arms.size());
    return PassOutcome::Changed;
}

static PassOutcome expandForInLoops(FunctionPassContext& context, Emission& emission)
{
    bool hasStep = context.function.values.containsIf([](const Value& value) {
        return value.opcode == Opcode::ForInStep;
    });
    if (!hasStep)
        return PassOutcome::Unchanged;

    PassOutcome outcome = editLoopsOneAtATime(context, [&](const NaturalLoop& loop) {
        return rejoinSeparatedLoop(context, loop);
    });
    return outcome | editLoopsOneAtATime(context, [&](const NaturalLoop& loop) {
        return expandForInLoop(context, emission, loop);
    });
}

namespace {

struct SpreadValueOnEdge {
    std::optional<Edge> edge;
    Vector<std::pair<PropertyKeyID, ValueID>, 4> properties;
    Vector<ValueID, 2> definitions;
    ValueID other;
};

} // anonymous namespace

AOT_THRESHOLD(unsigned, maxFieldsOfMergedLiterals, 8);

static constexpr uint16_t primitivesWithoutProperties = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnySymbol | Primitive::BigInt;

static bool isCastOrFinishedObject(Opcode opcode)
{
    return isCastWithoutTest(opcode) || opcode == Opcode::FinishObject;
}

static Value makeDefinitionOfProperty(ValueID object, PropertyKeyID key, ValueID value)
{
    return Emission::make<&Value::setKey>(Opcode::PropDefine, { object, value }, key);
}

static bool collectLiteralMadeForSpread(FunctionPassContext& context, ValueID allocation, ValueID copy, SpreadValueOnEdge& value)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    while (isCastOrFinishedObject(function.at(allocation).opcode))
        allocation = function.at(allocation).operand(0);
    const Value& literal = function.at(allocation);
    if (literal.opcode != Opcode::NewObject || literal.prototypeOperand())
        return false;
    const Layout& layout = context.program.at(literal.layout());
    if (layout.kind != LayoutKind::Record)
        return false;
    std::span<const ValueID> values = literal.valuesOperands();
    for (size_t index = 0; index < values.size(); ++index) {
        const Field& field = layout.fields[literal.fields()[index]];
        if (!field.form.isEmpty())
            return false;
        value.properties.append({ field.key, values[index] });
    }

    BlockID block = useLists.locationOf(allocation).block;
    for (ValueID current = allocation;;) {
        std::optional<Use> next;
        for (const Use& use : useLists.usesOf(current)) {
            bool isDefinition = use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::PropDefine && use.operandIndex == OperandIndex::PropDefine::object;
            if (isDefinition) {
                if (use.block != block)
                    return false;
                value.definitions.append(use.user);
                continue;
            }
            if (next)
                return false;
            next = use;
        }
        if (!next)
            return false;
        if (next->kind == Use::Kind::EdgeArgument) {
            if (!value.edge || next->edge() != *value.edge)
                return false;
            break;
        }
        if (next->kind != Use::Kind::Operand || next->block != block)
            return false;
        if (next->user == copy) {
            if (value.edge || next->operandIndex != OperandIndex::CopyDataProperties::source)
                return false;
            break;
        }
        if (!isCastOrFinishedObject(function.at(next->user).opcode))
            return false;
        current = next->user;
    }
    AOT::sort(value.definitions, { }, [&](ValueID definition) {
        return useLists.locationOf(definition).index;
    });
    for (ValueID definition : value.definitions)
        value.properties.append({ function.at(definition).key(), function.at(definition).valueOperand() });
    return true;
}

static PassOutcome defineFieldsOfMergedLiterals(FunctionPassContext& context, IRBuilder& builder, ProgramPoint point)
{
    Function& function = context.function;
    ValueID copy = function.at(point.block).values[point.index];
    if (!function.at(copy).excludedOperands().empty() || !function.at(copy).excludedKeys().empty())
        return PassOutcome::Unchanged;
    ValueID target = function.at(copy).targetOperand();
    Origin origin = function.at(copy).origin();

    Vector<ValueID, 2> castsOfParameter;
    ValueID source = function.at(copy).sourceOperand();
    // A cast on the way may name the fact that the word has been filled. What takes the place of the word here is looked at behind the same fact.
    ValueID factOfSource;
    while (isCastWithoutTest(function.at(source).opcode)) {
        castsOfParameter.append(source);
        if (function.at(source).opcode == Opcode::AssumeType && function.at(source).factOperand())
            factOfSource = function.at(source).factOperand();
        source = function.at(source).operand(0);
    }
    const UseLists& useLists = context.analyses.useLists();
    ValueLocation location = useLists.locationOf(source);
    if (location.placement == ValuePlacement::InBlock) {
        SpreadValueOnEdge literal;
        if (!collectLiteralMadeForSpread(context, source, copy, literal) || literal.properties.size() > maxFieldsOfMergedLiterals)
            return PassOutcome::Unchanged;
        builder.setOrigin(origin);
        for (auto [key, held] : literal.properties)
            builder.insertBefore(point, makeDefinitionOfProperty(target, key, held));
        becomeUndefined(function, copy);
        for (ValueID definition : literal.definitions)
            becomeUndefined(function, definition);
        builder.applyInsertions();
        return PassOutcome::Changed;
    }
    if (location.placement != ValuePlacement::Parameter || function.at(location.block).role != BlockRole::Ordinary)
        return PassOutcome::Unchanged;
    BlockID merge = location.block;
    uint32_t parameterIndex = location.index;

    std::optional<Use> onlyUse = useLists.singleUse(source);
    while (onlyUse && onlyUse->kind == Use::Kind::Operand && isCastOrFinishedObject(function.at(onlyUse->user).opcode))
        onlyUse = useLists.singleUse(onlyUse->user);
    if (!onlyUse || onlyUse->kind != Use::Kind::Operand || onlyUse->user != copy || onlyUse->operandIndex != OperandIndex::CopyDataProperties::source)
        return PassOutcome::Unchanged;

    Vector<SpreadValueOnEdge, 2> edges;
    for (const Edge& edge : context.analyses.edgesFromUnreachableBlocks()) {
        if (targetOf(function, edge).block != merge)
            continue;
        if (edge.isHandlerEdge() || parameterIndex >= targetOf(function, edge).arguments.size())
            return PassOutcome::Unchanged;
        edges.append(SpreadValueOnEdge { .edge = edge });
    }

    unsigned numberOfFields = 0;
    bool hasLiteral = false;
    for (const Edge& edge : context.analyses.cfg().incomingEdges(merge)) {
        if (edge.isHandlerEdge())
            return PassOutcome::Unchanged;
        const Target& incoming = function.at(edge.from).terminator.targets[edge.targetIndex];
        if (parameterIndex >= incoming.arguments.size())
            return PassOutcome::Unchanged;
        ValueID argument = incoming.arguments[parameterIndex];
        SpreadValueOnEdge value { .edge = edge };
        if (context.program.typeOf(function, argument).hasOnlyPrimitives(primitivesWithoutProperties))
            edges.append(WTF::move(value));
        else if (!collectLiteralMadeForSpread(context, argument, ValueID(), value))
            edges.append(SpreadValueOnEdge { .edge = edge, .other = argument });
        else {
            numberOfFields += value.properties.size();
            hasLiteral = true;
            edges.append(WTF::move(value));
        }
    }
    if (!hasLiteral || numberOfFields > maxFieldsOfMergedLiterals)
        return PassOutcome::Unchanged;

    builder.setOrigin(origin);
    Vector<const SpreadValueOnEdge*, 2> literals;
    Vector<std::pair<ValueID, TypeID>, 8> parametersOfLiterals;
    ValueID which = builder.addParameter(merge, TypeTable::int32);
    for (const SpreadValueOnEdge& value : edges) {
        if (value.properties.isEmpty())
            continue;
        for (auto [key, held] : value.properties) {
            TypeID type = function.at(held).type;
            parametersOfLiterals.append({ builder.addParameter(merge, context.types.unionOf(type, TypeTable::undefined)), type });
        }
        literals.append(&value);
    }
    // The literals are numbered from one on. Zero stands for a value without properties.
    const int64_t numberOfOther = literals.size() + 1;
    TypeID typeOfOther = TypeTable::undefined;
    bool hasOther = false;
    for (const SpreadValueOnEdge& value : edges) {
        if (!value.other)
            continue;
        hasOther = true;
        typeOfOther = context.types.unionOf(typeOfOther, function.at(value.other).type);
    }
    ValueID other = hasOther ? builder.addParameter(merge, typeOfOther) : ValueID();

    size_t nextLiteral = 0;
    for (const SpreadValueOnEdge& value : edges) {
        size_t ownLiteral = value.properties.isEmpty() ? notFound : nextLiteral++;
        ProgramPoint end = pointBeforeTerminatorOf(function, value.edge->from);
        Value number = Emission::make<&Value::setInteger>(Opcode::Integer, { }, ownLiteral != notFound ? static_cast<int64_t>(ownLiteral + 1) : value.other ? numberOfOther : 0);
        builder.appendArgument(*value.edge, builder.insertBefore(end, WTF::move(number)));
        ValueID nothing;
        auto ensureNothing = [&] {
            if (!nothing)
                nothing = builder.insertBefore(end, IRBuilder::make(Opcode::Undefined));
            return nothing;
        };
        for (size_t index = 0; index < literals.size(); ++index) {
            for (size_t field = 0; field < literals[index]->properties.size(); ++field)
                builder.appendArgument(*value.edge, index == ownLiteral ? value.properties[field].second : ensureNothing());
        }
        if (hasOther)
            builder.appendArgument(*value.edge, value.other ? value.other : ensureNothing());
    }
    builder.applyInsertions();

    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    auto endArm = [&] {
        builder.endExpansion(expansion, std::span<const ValueID> { });
    };
    Vector<std::pair<int64_t, Target>> cases;
    Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
    for (int64_t number = 1; number <= static_cast<int64_t>(literals.size() + hasOther); ++number)
        cases.append({ number, Target { builder.addBlock(frequency), { }, { } } });
    Vector<BlockID, 4> arms = cases.map<Vector<BlockID, 4>>([](const auto& entry) {
        return entry.second.block;
    });
    BlockID nothingToCopy = builder.addBlock(frequency);
    builder.emitSwitch(which, WTF::move(cases), Target { nothingToCopy, { }, { } });
    const std::pair<ValueID, TypeID>* passed = parametersOfLiterals.begin();
    for (size_t index = 0; index < literals.size(); ++index) {
        builder.moveToEndOf(arms[index]);
        for (size_t field = 0; field < literals[index]->properties.size(); ++field, ++passed) {
            ValueID held = builder.append(makeAssumeType(passed->first, Proven::byConstruction(passed->second)));
            builder.append(makeDefinitionOfProperty(target, literals[index]->properties[field].first, held));
        }
        endArm();
    }
    if (hasOther) {
        builder.moveToEndOf(arms.last());
        if (factOfSource && context.program.typeOf(function, other).mayBeUnspecified())
            other = builder.append(makeCastBehindFact(function, context.types, other, factOfSource));
        IRBuilder::BranchBlocks hasNothing = builder.emitBranch(builder.appendTypeTest(other, Type::fromPrimitives(primitivesWithoutProperties)), frequency, frequency);
        endArm();
        builder.moveToEndOf(hasNothing.notTaken);
        builder.append(Opcode::CopyDataProperties, { target, other });
        endArm();
    }
    builder.moveToEndOf(nothingToCopy);
    endArm();
    for (ValueID cast : castsOfParameter)
        becomeUndefined(function, cast);
    for (const SpreadValueOnEdge& value : edges) {
        for (ValueID definition : value.definitions)
            becomeUndefined(function, definition);
    }

    BitVector parameter(function.at(merge).parameters.size());
    parameter.set(parameterIndex);
    builder.removeParameters(merge, parameter);
    return PassOutcome::Changed;
}

static PassOutcome defineFieldsOfMergedLiterals(FunctionPassContext& context, IRBuilder& builder)
{
    const Function& function = context.function;
    auto defineAtFirstSpread = [&] {
        for (uint32_t block = 0; block < function.blocks.size(); ++block) {
            const BlockValues& values = function.at(BlockID(block)).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (function.at(values[index]).opcode == Opcode::CopyDataProperties && defineFieldsOfMergedLiterals(context, builder, { BlockID(block), index }) == PassOutcome::Changed)
                    return true;
            }
        }
        return false;
    };
    PassOutcome outcome = PassOutcome::Unchanged;
    while (defineAtFirstSpread()) {
        context.analyses.didChangeValues();
        context.build.statistics().count(StatisticID::OwnKeyWalksExpanded, 1);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

PassOutcome expandOwnKeys(FunctionPassContext& context)
{
    const OpcodeSet opcodes = context.analyses.opcodesOfValues();
    bool mayHaveWalks = opcodes.intersects(opcodesOfWalks);
    bool mayHaveForInLoops = opcodes.contains(Opcode::ForInStep);
    bool mayHaveKeyOrderTests = opcodes.intersects(opcodesOfKeyOrderTests) && opcodes.contains(Opcode::NewObject);
    if (!mayHaveWalks && !mayHaveForInLoops && !mayHaveKeyOrderTests)
        return PassOutcome::Unchanged;

    EmissionInputs inputs(context);
    IRBuilder builder(inputs.analysisContext(), context.function, context.types, &context.analyses);
    Emission emission(builder, inputs);
    PassOutcome outcome = PassOutcome::Unchanged;
    if (opcodes.contains(Opcode::CopyDataProperties))
        outcome = defineFieldsOfMergedLiterals(context, builder);

    Vector<PlannedWalk, 4> walks;
    if (mayHaveWalks)
        walks = planOwnKeyWalks(context);
    for (const PlannedWalk& walk : walks) {
        if (!walk.walkedSources.isEmpty())
            expandWalk(emission, walk);
        else if (expandStringify(context, emission, walk.site) == PassOutcome::Unchanged)
            continue;
        context.build.statistics().count(StatisticID::OwnKeyWalksExpanded, std::max<uint64_t>(walk.walkedSources.size(), 1));
        outcome = PassOutcome::Changed;
    }

    if (mayHaveForInLoops)
        outcome |= expandForInLoops(context, emission);
    if (outcome == PassOutcome::Changed || mayHaveKeyOrderTests)
        outcome |= foldKeyOrderTests(context);
    return outcome;
}

namespace {

struct OpenDataNode {
    const DataNode* node;
    uint32_t ordinal;
    size_t numberOfChildren;
    Vector<ValueID, 8> values;
    Vector<uint32_t, 2> holes;
    Vector<uint32_t, 8> fieldsOfHead { };
    Vector<ValueID, 8> valuesOfHead { };
    Vector<size_t, 2> membersDefinedAfterHead { };
    bool canAddToHead { true };
    ValueID head { };

    size_t numberOfFinishedChildren() const { return values.size() + holes.size(); }
};

} // anonymous namespace

static bool shouldExpand(const Program& program, const DataNode& root, uint64_t maxNodes, bool runsAtMostOnce)
{
    uint64_t numberOfNodes = 0;
    bool hasMemberWithoutField = false;
    bool fitsEncoding = true;
    bool hasNeverAllocatedLayout = false;
    forEachDataNodeInPreorder(root, [&](uint32_t, const DataNode& node) {
        ++numberOfNodes;
        const auto* object = node.tryAs<DataNode::Object>();
        if (!object)
            return;
        const Layout& layout = program.at(object->layout);
        hasNeverAllocatedLayout |= layout.flow.allocation == AllocationState::NeverAllocated;
        for (const DataMember& member : object->members)
            hasMemberWithoutField |= !layout.findField(member.key);
        fitsEncoding &= fitsReadOnlyEncoding(program, *object);
    });
    if (hasNeverAllocatedLayout)
        return false;
    if (!fitsEncoding)
        return true;
    // A definition by name that runs once finds nothing in any cache, so there the members that are no fields are cheaper from the read-only encoding whatever the size of the tree.
    if (hasMemberWithoutField && runsAtMostOnce)
        return false;
    return numberOfNodes <= maxNodes;
}

static std::optional<size_t> numberOfChildrenOf(const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Array:
        return node.as<DataNode::Array>().elements.size();
    case DataNode::Kind::Object:
        return node.as<DataNode::Object>().members.size();
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
    case DataNode::Kind::BigInt:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makeConstant(const DataNode& leaf)
{
    switch (leaf.kind()) {
    case DataNode::Kind::Undefined:
        return IRBuilder::make(Opcode::Undefined);
    case DataNode::Kind::Null:
        return IRBuilder::make(Opcode::Null);
    case DataNode::Kind::Boolean:
        return Emission::make<&Value::setBoolean>(Opcode::Boolean, { }, leaf.as<DataNode::Boolean>().value);
    case DataNode::Kind::Number: {
        double number = leaf.as<DataNode::Number>().value;
        if (isTrackedInteger(number))
            return Emission::make<&Value::setInteger>(Opcode::Integer, { }, static_cast<int64_t>(number));
        return Emission::make<&Value::setNumber>(Opcode::Double, { }, number);
    }
    case DataNode::Kind::String:
        return Emission::make<&Value::setString>(Opcode::String, { }, leaf.as<DataNode::String>().value);
    case DataNode::Kind::BigInt:
        return Emission::make<&Value::setDigits>(Opcode::BigInt, { }, leaf.as<DataNode::BigInt>().digits);
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makeHead(const OpenDataNode& open, const FlowDataNodeStamp& stamp)
{
    return Emission::make<&Value::setLayout, &Value::setFields, &Value::setValuesOperands, &Value::setAllocationSite, &Value::setAllocationClass>(Opcode::NewObject, { },
        open.node->as<DataNode::Object>().layout, open.fieldsOfHead.span(), open.valuesOfHead.span(), stamp.site, stamp.allocationClass);
}

static void allocateHead(OpenDataNode& open, const FlowDataNodeStamp& stamp, const auto& insert)
{
    const Vector<DataMember>& members = open.node->as<DataNode::Object>().members;
    open.head = insert(makeHead(open, stamp));
    for (size_t index : std::exchange(open.membersDefinedAfterHead, { }))
        insert(makeDefinitionOfProperty(open.head, members[index].key, open.values[index]));
}

// The values of this many members may all be made before the object is allocated.
AOT_THRESHOLD(size_t, maxMembersMadeBeforeAllocation, 4095);

// Array indexes are listed before the other own keys, which are listed in the order of their creation. A long object is allocated at the first
// member with another name that the allocation cannot fill, and each later member is defined as soon as its value is made.
static void addChild(Rewrite& rewrite, OpenDataNode& open, ValueID value, const auto& stampOf, const auto& insert)
{
    const size_t index = open.values.size();
    open.values.append(value);
    const auto* object = open.node->tryAs<DataNode::Object>();
    if (!object)
        return;

    const Program& program = rewrite.program;
    const PropertyKeyID key = object->members[index].key;
    if (open.head) {
        insert(makeDefinitionOfProperty(open.head, key, value));
        return;
    }
    const Layout& layout = program.at(object->layout);
    std::optional<uint32_t> field = layout.findField(key);
    // An allocation fills a slot without a test.
    auto fitsSlot = [&] {
        TypingContext typing { program, rewrite.function, rewrite.context.types };
        return typing.typeOf(value).isSubsetOf(typing.typeOf(layout.fields[*field].declared), program);
    };
    if (field && open.canAddToHead && fitsSlot()) {
        open.fieldsOfHead.append(*field);
        open.valuesOfHead.append(value);
        return;
    }
    open.membersDefinedAfterHead.append(index);
    open.canAddToHead &= program.nameClassOf(key) == NameClass::Index;
    if (!open.canAddToHead && object->members.size() > maxMembersMadeBeforeAllocation)
        allocateHead(open, stampOf(open), insert);
}

static Value makeAggregate(OpenDataNode& closed, const FlowDataNodeStamp& stamp, const auto& insert)
{
    if (const auto* array = closed.node->tryAs<DataNode::Array>()) {
        // Flow proves storage only of contents that it has proven.
        return Emission::make<&Value::setContents, &Value::setContentsAreProven, &Value::setArrayStorage, &Value::setHoles, &Value::setElementsOperands, &Value::setAllocationSite>(Opcode::NewArray, { },
            stamp.elements ? stamp.elements : array->contents, stamp.arrayStorage != ArrayStorage::Other, stamp.arrayStorage, closed.holes.span(), closed.values.span(), stamp.site);
    }

    if (!closed.head && closed.membersDefinedAfterHead.isEmpty())
        return makeHead(closed, stamp);
    if (!closed.head)
        allocateHead(closed, stamp, insert);
    return IRBuilder::make(Opcode::FinishObject, { closed.head });
}

static void expandDataAt(Rewrite& rewrite, ProgramPoint point)
{
    ValueID id = rewrite.at(point.block).values[point.index];
    Origin origin = rewrite.at(id).origin();
    AllocationSiteID rootSite = rewrite.at(id).allocationSite();
    const DataNode& root = rewrite.program.at(rewrite.at(id).data()).root;
    auto insert = [&](Value&& value) {
        return rewrite.insertBefore(point, origin, WTF::move(value));
    };
    auto stampOf = [&](const OpenDataNode& open) {
        return rewrite.context.inputs.flow.dataNodeStamp(rootSite, open.ordinal);
    };

    Value replacement;
    Vector<OpenDataNode, 4> openNodes;
    if (!numberOfChildrenOf(root))
        replacement = makeConstant(root);
    else {
        forEachDataNodeInPreorder(root, [&](uint32_t ordinal, const DataNode& node) {
            if (std::optional<size_t> numberOfChildren = numberOfChildrenOf(node))
                openNodes.append(OpenDataNode { &node, ordinal, *numberOfChildren, { }, { } });
            else if (node.is<DataNode::Hole>())
                openNodes.last().holes.append(static_cast<uint32_t>(openNodes.last().numberOfFinishedChildren()));
            else
                addChild(rewrite, openNodes.last(), insert(makeConstant(node)), stampOf, insert);

            while (!openNodes.isEmpty() && openNodes.last().numberOfFinishedChildren() == openNodes.last().numberOfChildren) {
                OpenDataNode closed = openNodes.takeLast();
                FlowDataNodeStamp stamp = stampOf(closed);
                Value aggregate = makeAggregate(closed, stamp, insert);
                if (openNodes.isEmpty()) {
                    replacement = WTF::move(aggregate);
                    return;
                }
                // The field that takes an array may have been narrowed to what flow has proven of that very array.
                ValueID made = insert(WTF::move(aggregate));
                addChild(rewrite, openNodes.last(), stamp.type ? insert(makeAssumeType(made, Proven::byConstruction(stamp.type))) : made, stampOf, insert);
            }
        });
    }
    RELEASE_ASSERT(openNodes.isEmpty());

    replacement.setOrigin(origin);
    replacement.type = resultTypeOf(rewrite.program, rewrite.function, replacement, rewrite.context.types);
    replacement.setExpected(expectedTypeOf(rewrite.program, rewrite.function, replacement, rewrite.context.types));
    rewrite.edit(id) = WTF::move(replacement);
}

PassOutcome expandData(FunctionPassContext& context)
{
    Rewrite rewrite(context);
    const Program& program = context.program;
    uint64_t maxNodes = context.limit(LimitID::MaxExpandedDataNodes);
    for (uint32_t blockIndex = 0; blockIndex < rewrite.function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (uint32_t index = static_cast<uint32_t>(rewrite.at(block).values.size()); index--;) {
            const Value& value = rewrite.at(rewrite.at(block).values[index]);
            if (value.opcode != Opcode::Data)
                continue;
            const DataNode& root = program.at(value.data()).root;
            if (shouldExpand(program, root, maxNodes, context.runsAtMostOnce(block)))
                expandDataAt(rewrite, { block, index });
            else {
                // Flow has proven the storage of the arrays of such a tree on the word that each becomes a literal, which allocates what its stamp says.
                RELEASE_ASSERT(!isExpandedForItsSizeAlone(root, maxNodes));
            }
        }
    }
    return rewrite.commit();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

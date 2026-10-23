/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTHeat.h"
#include "AOTKnownClosures.h"
#include "AOTPropertyAccessCache.h"
#include "AOTResolveAccesses.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestedAccess {
    PropertyAccessPlan plan;
    SlotStamp stamp;
    AccessTests tests;
};

} // anonymous namespace

static PropertyAccessPlan planWithoutCalls(FunctionPassContext& context, PropertyAccessQuery& query)
{
    query.isOnlyCalled = false;
    query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    return planPropertyAccess({ context.program, &context.inputs.flow }, query);
}

static const Value* findFilledSlotTest(std::span<const Value> tests)
{
    auto found = std::ranges::find(tests, Opcode::IsSlotFilled, &Value::opcode);
    return found == tests.end() ? nullptr : &*found;
}

// What the access is once its tests have passed: the receiver has one of the layouts that its test lists.
static PropertyAccessPlan planBehindTests(FunctionPassContext& context, PropertyAccessQuery& query, std::span<const Value> receiverTests)
{
    if (std::optional<ReceiverCandidates> established = receiversEstablishedBy(context.program, receiverTests))
        query.receivers = WTF::move(*established);
    query.expectsEmptySlot = (query.access == Access::Store || query.access == Access::Read) && !findFilledSlotTest(receiverTests);
    return planWithoutCalls(context, query);
}

// The rewrite plans the store again behind the tests. Whether what passes the test of the stored value fits the fields is settled here, by that plan, for the layouts that the test of the receiver lists.
static bool storedValueFitsBehindTests(FunctionPassContext& context, const PropertyAccessQuery& query, ValueID stored, const TestedAccess& tested)
{
    if (!tested.plan.stores() || tested.plan.storedValueFits)
        return true;

    std::span<const Value> tests = tested.tests.tests.span();
    const Value& storedValueTest = tests.back();
    RELEASE_ASSERT(storedValueTest.opcode == Opcode::TypeTest && storedValueTest.operand(testedOperandIndex) == stored);

    Type passed = context.program.typeOf(context.function, stored).castTo(context.types.at(storedValueTest.testedType()), context.types);
    PropertyAccessQuery queryBehindTests = query;
    queryBehindTests.stored = &passed;
    PropertyAccessPlan plan = planBehindTests(context, queryBehindTests, tests.first(tests.size() - 1));
    return plan.isResolved() && plan.receiverTest == ReceiverTest::None && plan.stores() && plan.storedValueFits;
}

enum class ReceiversOfQuery : bool { AsExpected, Chosen };

static std::optional<TestedAccess> planAccessUnderTests(TestSiteReporter& reporter, const Value& access, PropertyAccessQuery& query, ReceiversOfQuery receivers = ReceiversOfQuery::AsExpected)
{
    FunctionPassContext& context = reporter.context();
    AccessOperands operands = operandsOfNamedAccess(access);
    // The rule of calls has asked the same of a read or a store that it left alone.
    std::optional<PlannedAccess> planned;
    if (receivers == ReceiversOfQuery::AsExpected && !query.expectsEmptySlot && !query.mayTestSlotFirst)
        planned = reporter.takePlanOfUse(false);
    while (true) {
        TestedAccess tested;
        if (planned) {
            query.isOnlyCalled = false;
            query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
            tested = { WTF::move(planned->plan), planned->stamp, WTF::move(planned->tests) };
            planned = std::nullopt;
            if (tested.plan.needsNoTest())
                return std::nullopt;
        } else {
            tested.plan = planWithoutCalls(context, query);
            if (!tested.plan.isResolved() || tested.plan.needsNoTest())
                return std::nullopt;

            tested.stamp = createSlotStamp(context, access, tested.plan);
            tested.tests = createAccessTests(context, tested.plan, { operands.receiver, operands.stored, tested.stamp });
        }
        if (tested.tests.tests.isEmpty())
            return std::nullopt;

        const Value* receiverTest = tested.tests.findReceiverLayoutTest();
        if (receiverTest && reporter.findEarlierTest(*receiverTest) == EarlierTest::Failed) {
            size_t numberOfExpectedMembers = query.receivers.predicted.size();
            query.receivers.predicted.subtract(tested.plan.receivers);
            RELEASE_ASSERT(query.receivers.predicted.size() < numberOfExpectedMembers);
            continue;
        }

        const Value* filledSlotTest = findFilledSlotTest(tested.tests.tests.span());
        if (filledSlotTest && query.access == Access::Store && !query.expectsEmptySlot && reporter.findEarlierTest(*filledSlotTest) == EarlierTest::Failed) {
            query.expectsEmptySlot = true;
            continue;
        }

        if (tested.plan.form == AccessForm::Call || !storedValueFitsBehindTests(context, query, operands.stored, tested))
            return std::nullopt;
        return tested;
    }
}

// One test of a slot is cheaper than the three under which an empty slot reads as undefined.
static constexpr unsigned minOptionalFieldsReadToExpectEmptySlots = 4;

static LayoutSetID layoutsOfReceiver(TestSiteReporter& reporter, const Value& access)
{
    return reporter.program().expectedOrTypeOf(reporter.function(), access.operand(*receiverOf(access.opcode))).layouts;
}

static PropertyAccessCache::Optionality computeOptionality(const Program& program, std::span<const LayoutID> layouts, PropertyAccessCache::DeclaredTypesKey key, PropertyKeyID name)
{
    PropertyAccessCache::Optionality result { key, false, true };
    for (LayoutID layout : layouts) {
        std::optional<FieldReference> reference = findField(program, layout, name, BaseChain::Proven);
        bool isOptional = reference && !fieldAt(program, *reference).flags.contains(FieldFlags::IsRequired);
        result.isOptionalInSomeLayout |= isOptional;
        result.isOptionOfRecordInEveryLayout &= isOptional && !program.at(declaredLayoutOf(program, layout)).constructor;
    }
    return result;
}

// `node.kind`, asked in every function of a program and in every round, of a family of some hundred layouts that all require it. The answer hangs on the set and the name alone.
static PropertyAccessCache::Optionality optionalityOf(const Program& program, LayoutSetID set, PropertyKeyID name)
{
    std::span<const LayoutID> layouts = program.at(set).members.span();
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache || layouts.size() < PropertyAccessCache::fewestLayouts || TypeTable::isProvisional(name))
        return computeOptionality(program, layouts, { }, name);
    PropertyAccessCache::DeclaredTypesKey key { cache->lists.idOf(program.types, set), name };
    return findOrCompute(cache->optionalities, key, computeHash(key.layouts, key.key), [&] {
        return computeOptionality(program, layouts, key, name);
    });
}

static bool readsOptionalField(const Program& program, LayoutSetID layouts, PropertyKeyID key)
{
    return optionalityOf(program, layouts, key).isOptionalInSomeLayout;
}

static bool storesOptionOfRecord(const Program& program, LayoutSetID layouts, PropertyKeyID key)
{
    return optionalityOf(program, layouts, key).isOptionOfRecordInEveryLayout;
}

// `const P = (F.prototype = {}); P.emit = ..`: the members of such a class are fields of a record, and the class lists none of them.
static bool claimsAbsenceOfNameOnInstalledPrototype(TestSiteReporter& reporter, const PropertyAccessPlan& plan, PropertyKeyID key)
{
    if (plan.member.kind != MemberKind::Absent && plan.member.kind != MemberKind::Field)
        return false;
    bool restsOnAbsence = std::ranges::any_of(plan.conditions, [&](const MemberCondition& condition) {
        return condition.kind == MemberCondition::Kind::MemberUnchanged && condition.key == key;
    });
    if (!restsOnAbsence)
        return false;
    const Program& program = reporter.program();
    const Heat& heat = reporter.context().inputs.heat;
    PropertyKeyID prototypeKey = program.keyOf(BuiltinNames::prototype);
    return std::ranges::any_of(program.layoutsWithName(key), [&](LayoutID layout) {
        return heat.isStoredUnder(layout, prototypeKey);
    });
}

// `Object.setPrototypeOf(make(), prototype).method()`: the object has just been handed a prototype, so a test that it still has the one of its layout fails every time and sends the rest of the function into its copy.
static bool testsPrototypeThatWasJustReplaced(TestSiteReporter& reporter, const Value& access, const TestedAccess& tested)
{
    if (!std::ranges::contains(tested.tests.tests, Opcode::HasLayoutPrototype, &Value::opcode))
        return false;
    const Function& function = reporter.function();
    const Value& receiver = function.at(definitionThroughSameValues(function, access.operand(*receiverOf(access.opcode))));
    return receiver.opcode == Opcode::CallBuiltin && reporter.program().builtins().at(receiver.overload()).code.implementation.inlineBuiltin == InlineBuiltin::SetPrototypeOf;
}

// A test buys a load in place of a read by name and costs the test, the arm with the read by name and the join.
static bool isReadOfUntypedFieldInColdCode(TestSiteReporter& reporter, const Value& access, const TestedAccess& tested)
{
    FunctionPassContext& context = reporter.context();
    BlockID block = reporter.point().block;
    if (!context.isBehindRounds || access.opcode != Opcode::PropGet || tested.plan.operation != Opcode::GetSlot || reporter.function().at(block).handler || heatClassOf(context, block) > HeatClass::Cold)
        return false;
    // What is entered most is small: accessors, selectors, callbacks. Their tests are few bytes. A body that was pasted in is as small as it was where it was written.
    const Function& function = reporter.function();
    if (std::min(function.values.size(), context.program.sourceFunctionOf(function, access.origin()).values.size()) <= HeatModel::maxSizeOfSmallFunction)
        return false;
    // However seldom the function is entered: a few calls may be hundreds of turns.
    if (context.analyses.naturalLoops().innerMostLoopOf(block))
        return false;
    const Value* receiverTest = tested.tests.findReceiverLayoutTest();
    return receiverTest && std::ranges::all_of(tested.tests.tests, [&](const Value& test) {
        return &test == receiverTest || (test.opcode == Opcode::IsSlotFilled && !test.holdsDeclaredType());
    });
}

// `this._events = ..` in EventEmitter, for every emitter of the program.
static std::optional<TestedAccess> planForReceiversThatClaimNoAbsence(TestSiteReporter& reporter, const Value& access, PropertyAccessQuery& query, const PropertyAccessPlan& refused)
{
    if (query.start != MemberStart::Instance || refused.receivers.size() < 2)
        return std::nullopt;
    LayoutList others;
    for (LayoutID layout : refused.receivers) {
        PropertyAccessQuery queryOfOne = query;
        queryOfOne.receivers = ReceiverCandidates::underGuard(singleElementSpan(layout));
        PropertyAccessPlan plan = planWithoutCalls(reporter.context(), queryOfOne);
        if (plan.isResolved() && !claimsAbsenceOfNameOnInstalledPrototype(reporter, plan, query.key))
            others.add(layout);
    }
    if (others.isEmpty() || others.size() == refused.receivers.size())
        return std::nullopt;

    ReceiverCandidates expected;
    expected.level = KnowledgeLevel::Expected;
    expected.predicted = WTF::move(others);
    expected.mayBeNullish = query.receivers.mayBeNullish;
    query.receivers = WTF::move(expected);
    std::optional<TestedAccess> tested = planAccessUnderTests(reporter, access, query, ReceiversOfQuery::Chosen);
    if (!tested || claimsAbsenceOfNameOnInstalledPrototype(reporter, tested->plan, query.key))
        return std::nullopt;
    return tested;
}

// `const context = { cwd, depth }; context.file = () => load(context);`: two steps, so that the function can name the object. The literal stands in this function and does not list the field.
static bool storesFieldThatAllocationLeftOut(TestSiteReporter& reporter, const Value& store, PropertyKeyID key)
{
    const Program& program = reporter.program();
    const Value& allocation = reporter.function().at(reporter.analyses().identityOf(store.operand(*receiverOf(store.opcode))));
    if (allocation.opcode != Opcode::NewObject)
        return false;
    std::optional<uint32_t> field = program.at(declaredLayoutOf(program, allocation.layout())).findField(key);
    return field && !std::ranges::contains(allocation.fields(), *field);
}

// `{ wrap: "nowrap", ...style }`, `const { children, ...rest } = props`: what is copied key by key, whichever keys it has. An arm for one order of its keys asks for that order.
static ValueID objectWhoseKeysAreWalked(const Value& value)
{
    if (value.opcode == Opcode::CopyDataProperties)
        return value.operand(OperandIndex::CopyDataProperties::source);
    return value.opcode == Opcode::HasKeyOrder ? value.operand(OperandIndex::HasKeyOrder::object) : ValueID();
}

// For each set of layouts the optional fields that the likely code of the function reads off it, each name once. A function that walks the keys of such an object reads them all.
static UncheckedKeyHashMap<uint32_t, unsigned> countOptionalFieldsRead(TestSiteReporter& reporter)
{
    const Program& program = reporter.program();
    const Function& function = reporter.function();
    UncheckedKeyHashSet<uint64_t> seen;
    UncheckedKeyHashMap<uint32_t, unsigned> counts;
    for (BlockID block : reporter.analyses().cfg().reversePostOrder()) {
        if (function.at(block).frequency != BlockFrequency::Normal)
            continue;
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (ValueID walked = objectWhoseKeysAreWalked(value)) {
                if (LayoutSetID layouts = program.expectedOrTypeOf(function, walked).layouts)
                    counts.add(layouts.index() + 1, 0).iterator->value += minOptionalFieldsReadToExpectEmptySlots;
                continue;
            }
            bool readsField = value.opcode == Opcode::PropGet || value.opcode == Opcode::GetSlot || value.opcode == Opcode::GetSlotOrUndefined;
            if (!readsField || !value.key())
                continue;
            LayoutSetID layouts = layoutsOfReceiver(reporter, value);
            if (!layouts || !seen.add((static_cast<uint64_t>(layouts.index()) + 1) << 32 | value.key().index()).isNewEntry)
                continue;
            if (readsOptionalField(program, layouts, value.key()))
                counts.add(layouts.index() + 1, 0).iterator->value++;
        }
    }
    return counts;
}

// In a checked function of the program an option is what the checker says it is. The records of the library hold what callers hand in, and their empty slots mostly fail one of the tests as well.
static bool readsOptionsLeftOutAsUndefined(const FunctionPassContext& context)
{
    return context.function.has(FunctionFlags::IsChecked) && !isLibraryCode(context.program, context.function);
}

// Whoever reports the tests of a read and whoever rewrites the read behind them ask the same.
static bool mayTestSlotFirst(const FunctionPassContext& context, const PropertyAccessQuery& query)
{
    return query.access == Access::Read && readsOptionsLeftOutAsUndefined(context) && !isCold(context.inputs.heat.of(context.program, context.id));
}

void reportPropertyAccessTests(TestSiteReporter& reporter)
{
    std::optional<UncheckedKeyHashMap<uint32_t, unsigned>> optionalFieldsRead;
    reporter.forEachCandidate([&](const Value& access) {
        std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(reporter.program(), reporter.function(), access, ExpectedClassObjects::Tested);
        if (!query)
            return;
        bool mayMeetOptionLeftOut = query->access == Access::Read || query->access == Access::Store;
        if (LayoutSetID layouts = mayMeetOptionLeftOut ? layoutsOfReceiver(reporter, access) : LayoutSetID()) {
            if (!optionalFieldsRead)
                optionalFieldsRead = countOptionalFieldsRead(reporter);
            bool handlesOptions = optionalFieldsRead->get(layouts.index() + 1) >= minOptionalFieldsReadToExpectEmptySlots;
            // `style.wrap ??= "nowrap"`: the read has taken the empty slot for undefined, so no test of the slot has failed in front of the store that fills it.
            query->expectsEmptySlot = handlesOptions && (query->access == Access::Read || storesOptionOfRecord(reporter.program(), layouts, query->key));
        }
        if (query->access == Access::Store && storesFieldThatAllocationLeftOut(reporter, access, query->key))
            query->expectsEmptySlot = true;
        query->mayTestSlotFirst = mayTestSlotFirst(reporter.context(), *query);
        std::optional<TestedAccess> tested = planAccessUnderTests(reporter, access, *query);
        if (tested && claimsAbsenceOfNameOnInstalledPrototype(reporter, tested->plan, query->key)) {
            PropertyAccessPlan refused = WTF::move(tested->plan);
            tested = planForReceiversThatClaimNoAbsence(reporter, access, *query, refused);
        }
        if (!tested || testsPrototypeThatWasJustReplaced(reporter, access, *tested) || isReadOfUntypedFieldInColdCode(reporter, access, *tested))
            return;
        reporter.report(tested->tests.tests.span(), effectsAfterRewrite(reporter.context(), tested->plan, tested->stamp));
    });
}

namespace {

struct AccessBehindSlotTest {
    BlockID test;
    ValueSite access;
};

} // anonymous namespace

static bool isDefinedInBlockBefore(const Block& block, uint32_t end, ValueID value)
{
    for (uint32_t index = 0; index < end; ++index) {
        if (block.values[index] == value)
            return true;
    }
    return false;
}

static bool isGuardValidation(const Function& function, const UseLists& useLists, const Block& block, uint32_t index)
{
    if (index + 2 >= block.values.size())
        return false;
    ValueID test = block.values[index];
    ValueID expected = block.values[index + 1];
    const Value& validation = function.at(block.values[index + 2]);
    if (provesOf(function.at(test).opcode) == Proof::None || function.at(expected).opcode != Opcode::Boolean || validation.opcode != Opcode::ValidateSameValue)
        return false;
    return validation.operand(0) == test && validation.operand(1) == expected && useLists.useCount(test) == 1 && useLists.useCount(expected) == 1;
}

static std::optional<ValueSite> findAccessBehindSlotTest(FunctionPassContext& context, const Value& test, BlockID filled)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    if (function.at(filled).parameters.size() != 1 || useLists.useCount(function.at(filled).parameters[0]) != 1)
        return std::nullopt;
    const Terminator& terminator = function.at(filled).terminator;
    if (function.at(filled).values.isEmpty() && terminator.kind == TerminatorKind::Jump && terminator.targets[0].arguments.isEmpty()) {
        filled = terminator.targets[0].block;
        if (!function.at(filled).parameters.isEmpty() || !context.analyses.cfg().singleIncomingEdge(filled))
            return std::nullopt;
    }
    const Block& block = function.at(filled);
    for (uint32_t index = 0; index < block.values.size(); ++index) {
        ValueID id = block.values[index];
        const Value& value = function.at(id);
        if (value.opcode == Opcode::AllGuards && useLists.useCount(id) == 1)
            continue;
        if (isGuardValidation(function, useLists, block, index)) {
            index += 2;
            continue;
        }
        if (value.opcode != Opcode::GetSlot && value.opcode != Opcode::PutSlot)
            return std::nullopt;
        if (value.objectOperand() != test.objectOperand() || value.key() != test.key())
            return std::nullopt;
        if (value.opcode == Opcode::PutSlot && isDefinedInBlockBefore(block, index, value.valueOperand()))
            return std::nullopt;
        if (value.opcode == Opcode::GetSlot && (value.holdsDeclaredType() || !(context.program.typeOf(function, id).primitives & Primitive::Undefined)))
            return std::nullopt;
        return ValueSite { filled, index, id };
    }
    return std::nullopt;
}

static Vector<AccessBehindSlotTest> findAccessesBehindSlotTests(FunctionPassContext& context)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    Vector<AccessBehindSlotTest> sites;
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        if (block.frequency != BlockFrequency::Normal || block.values.isEmpty() || !block.terminator.isConditionalBranch())
            continue;
        const Value& test = function.at(block.values.last());
        if (test.opcode != Opcode::IsSlotFilled || test.holdsDeclaredType() || block.terminator.conditionOperand() != block.values.last())
            continue;
        const Target& filled = block.terminator.taken();
        bool leavesWhereEmpty = isRarerThan(function.at(block.terminator.notTaken().block).frequency, block.frequency);
        if (!leavesWhereEmpty || !filled.arguments.isEmpty() || !cfg.singleIncomingEdge(filled.block))
            continue;
        if (std::optional<ValueSite> access = findAccessBehindSlotTest(context, test, filled.block))
            sites.append({ id, *access });
    }
    return sites;
}

static std::optional<PropertyAccessPlan> planEmptySlotCase(FunctionPassContext& context, const Value& slotTest, bool isStore, bool functionStoresField)
{
    PropertyAccessContext accessContext { context.program, &context.inputs.flow };
    std::span<const LayoutID> receivers = context.program.at(slotTest.receivers()).members;
    bool handlesLazyFields = !isLibraryCode(context.program, context.function);
    PropertyAccessPlan plan;
    if (isStore) {
        std::optional<LazyFieldCreation> creation = handlesLazyFields ? planLazyFieldCreation(accessContext, receivers, slotTest.key()) : std::nullopt;
        if (!creation)
            return std::nullopt;
        plan.operation = creation->operation;
        plan.storedValueFits = true;
        plan.requiredProofs = creation->proofs;
        plan.conditions = WTF::move(creation->conditions);
        return plan;
    }
    OptionSet<OptionKind> kinds;
    if (readsOptionsLeftOutAsUndefined(context))
        kinds.add(OptionKind::RecordOption);
    if (handlesLazyFields && functionStoresField)
        kinds.add(OptionKind::LazilyInitializedField);
    std::optional<TestsOfEmptySlot> needed = kinds.isEmpty() ? std::nullopt : testsThatOptionLeftOutReadsAsUndefined(accessContext, receivers, slotTest.key(), kinds);
    if (!needed)
        return std::nullopt;
    plan.requiredProofs = needed->proofs;
    plan.conditions = WTF::move(needed->conditions);
    return plan;
}

static void addEmptySlotCase(FunctionPassContext& context, IRBuilder& builder, SiteExpansion& expansion, const AccessBehindSlotTest& site, const Value& slotTest, const PropertyAccessPlan& plan, bool isStore)
{
    Function& function = context.function;
    AccessTests tests = createAccessTests(context, plan, { slotTest.objectOperand(), { }, { slotTest.key(), slotTest.receivers(), slotTest.level(), { } } });

    BlockFrequency frequency = function.at(site.test).frequency;
    Target leaves = function.at(site.test).terminator.notTaken();
    expansion.begin(site.access);
    Value original = expansion.original();
    ValueID copy = expansion.appendOriginal();
    if (isStore)
        expansion.finishArmWithoutResult();
    else
        expansion.finishArm(copy);

    BlockID empty = builder.addBlock(frequency);
    builder.moveToEndOf(empty);
    builder.setOrigin(slotTest.origin());
    Vector<ValueID, 4> guards;
    if (slotTest.guardOperand())
        guards.append(slotTest.guardOperand());
    for (Value& test : tests.tests) {
        if (slotTest.guardOperand() && OperandPosition::guard(test.opcode))
            test.setGuardOperand(slotTest.guardOperand());
        ValueID passed = builder.append(WTF::move(test));
        if (isStore) {
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(passed, 0, frequency, frequency, { { }, Target(leaves) });
            guards.append(branch.provenOnTaken);
            builder.moveToEndOf(branch.taken);
            continue;
        }
        BlockID next = builder.addBlock(frequency);
        builder.emitBranch(passed, Target { next, { }, { } }, Target(leaves));
        builder.moveToEndOf(next);
    }
    if (isStore) {
        Value store(plan.operation);
        store.setObjectOperand(original.objectOperand());
        if (!guards.isEmpty())
            store.setGuardOperand(guards.size() == 1 ? guards[0] : builder.appendAllGuards(guards.span()));
        store.setValueOperand(original.valueOperand());
        setSlotStamp(store, { original.key(), original.receivers(), original.level(), original.accessSite() });
        builder.append(WTF::move(store));
        expansion.finishArmWithoutResult();
    } else
        expansion.finishArm(expansion.append(IRBuilder::make(Opcode::Undefined)));
    expansion.end();
    builder.redirectEdge(Edge { site.test, 1 }, Target { empty, { }, { } });
}

PassOutcome addEmptySlotCases(FunctionPassContext& context)
{
    Vector<AccessBehindSlotTest> sites = findAccessesBehindSlotTests(context);
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    Function& function = context.function;
    UncheckedKeyHashSet<uint32_t> storedKeys;
    for (const AccessBehindSlotTest& site : sites) {
        const Value& access = function.at(site.access.value);
        if (access.opcode == Opcode::PutSlot)
            storedKeys.add(access.key().index() + 1);
    }

    IRBuilder builder({ context.build, context.program, &context.inputs.flow, &context.inputs.effects, context.id }, function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    PassOutcome outcome = PassOutcome::Unchanged;
    // An expansion moves what stands behind the access into a block of its own, the next slot test with it. So the last comes first.
    for (const AccessBehindSlotTest& site : sites | std::views::reverse) {
        Value slotTest = function.at(function.at(site.test).values.last());
        bool isStore = function.at(site.access.value).opcode == Opcode::PutSlot;
        std::optional<PropertyAccessPlan> plan = planEmptySlotCase(context, slotTest, isStore, storedKeys.contains(slotTest.key().index() + 1));
        if (!plan)
            continue;
        addEmptySlotCase(context, builder, expansion, site, slotTest, *plan, isStore);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

// The operand of the site is what passed the test, or a cast of it to the type the operand had.
static bool isBehindCastsOf(const Function& function, ValueID proof, ValueID operand)
{
    for (ValueID id : CastChain(function, operand)) {
        if (id == proof)
            return true;
    }
    return false;
}

// A test of the stored value may name layouts too, and it says nothing about the receiver.
static std::span<const Value> testsWithoutStoredValueTest(const TestSiteRewriter& rewriter, ValueID stored)
{
    std::span<const Value> tests = rewriter.tests();
    if (!stored || tests.empty())
        return tests;
    uint32_t last = static_cast<uint32_t>(tests.size() - 1);
    bool lastTestsStoredValue = tests[last].opcode == Opcode::TypeTest && isBehindCastsOf(rewriter.function(), rewriter.proofOf(last), stored);
    return lastTestsStoredValue ? tests.first(last) : tests;
}

#if ASSERT_ENABLED
static bool isObjectTestOf(const Value& test, Proof proof, PropertyKeyID key)
{
    if (provesOf(test.opcode) != proof || !OperandPosition::object(test.opcode))
        return false;
    return proof != Proof::SlotFilled || test.key() == key;
}

static bool testsCoverPlan(std::span<const Value> tests, const PropertyAccessPlan& plan, PropertyKeyID key)
{
    bool coverProofs = std::ranges::all_of(allMembersOf<Proof>, [&](Proof proof) {
        return !plan.requiredProofs.contains(proof) || std::ranges::any_of(tests, [&](const Value& test) {
            return isObjectTestOf(test, proof, key);
        });
    });
    return coverProofs && testsCoverConditions(tests, plan.conditions.span());
}
#endif

// One value stands for everything that has the identity of the tested value, so its type may be wider than that of the operand the site was planned with.
static ValueID narrowToFields(TestSiteRewriter& rewriter, ValueID stored, const PropertyAccessPlan& plan, PropertyKeyID key)
{
    FunctionPassContext& context = rewriter.context();
    TypeArena& types = context.types;
    Type fieldType = storedTypeOf(context.program, plan.receivers.span(), key, types);
    Type narrowed = context.program.typeOf(context.function, stored).intersectWith(fieldType, types);
    if (narrowed.isNever())
        return { };

    TypeID castType = types.intern(narrowed);
    if (context.build.validates())
        rewriter.insertBeforeUse(makeValidateType(stored, castType));
    return rewriter.insertBeforeUse(makeAssumeType(stored, castType));
}

void rewriteTestedPropertyAccess(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    const Program& program = rewriter.program();
    const Value& access = rewriter.useValue();

    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(program, rewriter.function(), access, ExpectedClassObjects::Tested);
    RELEASE_ASSERT(query);
    query->mayTestSlotFirst = mayTestSlotFirst(context, *query);

    AccessOperands operands = operandsOfNamedAccess(access);
    operands.guard = rewriter.guard();

    PropertyAccessPlan plan = planBehindTests(context, *query, testsWithoutStoredValueTest(rewriter, operands.stored));
    RELEASE_ASSERT(plan.isResolved());
    RELEASE_ASSERT(plan.receiverTest == ReceiverTest::None);
    ASSERT(testsCoverPlan(rewriter.tests(), plan, query->key));
    plan.level = KnowledgeLevel::Expected;

    if (plan.stores() && !plan.storedValueFits) {
        operands.stored = narrowToFields(rewriter, operands.stored, plan, query->key);
        // No value is both what the operand may be and what the fields hold, so nothing gets here.
        if (!operands.stored)
            return;
        plan.storedValueFits = true;
    }

    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeUse = [&](auto&& value) -> ValueID {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertValue insert(insertBeforeUse);
    ProgramPoint point = rewriter.point();
    NamedAccessSite site { { point.block, point.index, rewriter.use() }, WTF::move(*query), WTF::move(plan) };
    AccessRewriter(context, insert).rewrite(site, operands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

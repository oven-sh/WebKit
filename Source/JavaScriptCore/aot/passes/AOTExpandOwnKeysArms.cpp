/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysInternal.h"

#if ENABLE(AOT)

#include "AOTDecidedBranches.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTTypeTestCoverage.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestOfAllocation {
    ProgramPoint point;
    ValueID test;
    ValueID allocation;
};

} // anonymous namespace

static constexpr OpcodeSet testsOfOwnKeys { Opcode::HasKeyOrder, Opcode::HasNoExtraProperties };
static constexpr size_t conditionsPerAllocation = 3;

static bool hasSymbolKey(const Program& program, const Field& field)
{
    return field.key && isSymbolKind(program.at(field.key).kind);
}

static bool visitsSymbolKeys(OwnKeyWalk walk)
{
    switch (walk) {
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread:
    case OwnKeyWalk::Copy:
        return true;
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool needsGuardForReads(const Program& program, const Type& source, const OwnKeyWalkArm& arm)
{
    if (source.mayBeUndefinedOrNull() || source.singleLayout(program) != arm.layout)
        return true;
    return std::ranges::any_of(arm.order, [&](uint32_t field) {
        return flowOf(program, { arm.layout, field }).occupancy != Occupancy::AlwaysFilled;
    });
}

static ValueID appendWalkedFieldRead(Emission& emission, const Emission::TestedObject& object, LayoutID layout, uint32_t field)
{
    const Program& program = emission.program();
    if (flowOf(program, { layout, field }).location.deferral == ClosureDeferral::Unknown)
        return emission.appendFieldRead(object, layout, field);

    Value read = IRBuilder::make(Opcode::GetDeferredClosureSlot, { object.object, object.guard });
    read.setKey(fieldAt(program, { layout, field }).key);
    read.setReceivers(emission.layoutSetOf(singleElementSpan(layout)));
    read.setLevel(object.guard ? KnowledgeLevel::Expected : KnowledgeLevel::Proven);
    return emission.builder().append(WTF::move(read));
}

static TypeID elementTypeOfArray(const Program& program, TypeID arrayType)
{
    const ClassPart* part = program.at(arrayType).findClassPart(BuiltinClasses::Array);
    return part ? part->contentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything;
}

static ValueID appendArray(IRBuilder& builder, TypeID contents, std::span<const ValueID> elements)
{
    Value array = IRBuilder::make(Opcode::NewArray);
    array.setContents(contents);
    array.setElementsOperands(elements);
    return builder.append(WTF::move(array));
}

template<typename Functor>
static void forEachGuardOperand(Function& function, const Functor& functor)
{
    auto visit = [&](auto& user) {
        user.forEachOperand([&](ValueID& operand, uint32_t operandIndex) {
            if (user.operandKind(operandIndex) == OperandKind::Guard)
                functor(operand);
        });
    };
    for (Block& block : function.editAllBlocks()) {
        for (ValueID value : block.values)
            visit(function.edit(value));
        visit(block.terminator);
    }
}

// The list a walk makes is new, so an earlier pass may have named the call as the guard for what holds of a new array, and a value that was named as a guard has to stay one.
// The expansion has made the call a parameter of the block behind the arms. That block takes a guard beside the list, for which every arm passes its own allocation.
static void keepResultAsGuard(IRBuilder& builder, IRBuilder::Expansion& expansion)
{
    if (expansion.results.size() != 1)
        return;

    ValueID result = expansion.results[0];
    bool isNamedAsGuard = false;
    forEachGuardOperand(builder.function(), [&](ValueID& operand) {
        isNamedAsGuard |= operand == result;
    });
    if (!isNamedAsGuard)
        return;

    ValueID guard = builder.addParameter(expansion.continuation, TypeTable::guard);
    forEachGuardOperand(builder.function(), [&](ValueID& operand) {
        if (operand == result)
            operand = guard;
    });
    expansion.results.append(guard);
}

void OwnKeyWalkExpander::expand(const OwnKeyWalkSite& site, const OwnKeyWalkPlan& plan)
{
    m_originalOperation = builder().function().at(site.value);
    IRBuilder::Expansion expansion = builder().beginExpansion({ site.block, site.index });
    keepResultAsGuard(builder(), expansion);
    emitArms(site, plan, expansion, { });
}

void OwnKeyWalkExpander::expandAssign(const OwnKeyWalkSite& site, const AssignPlan& plan)
{
    IRBuilder& builder = this->builder();
    BlockFrequency frequency = builder.function().at(site.block).frequency;
    IRBuilder::Expansion expansion = builder.beginExpansion({ site.block, site.index });

    OwnKeyWalkSite walked = site;
    for (const AssignedSource& assigned : plan.walkedSources) {
        bool isLastOperation = &assigned == &plan.walkedSources.last() && !plan.remainingSourcesCall;
        BlockID afterWalk = isLastOperation ? BlockID() : builder.addBlock(frequency);
        walked.source = assigned.source;
        m_originalOperation = assigned.slowPathCall;
        emitArms(walked, assigned.plan, expansion, afterWalk);
        if (afterWalk)
            builder.moveToEndOf(afterWalk);
    }
    if (!plan.remainingSourcesCall)
        return;
    m_originalOperation = *plan.remainingSourcesCall;
    builder.endExpansion(expansion, emitOriginalOperation(site));
}

void OwnKeyWalkExpander::emitArms(const OwnKeyWalkSite& site, const OwnKeyWalkPlan& plan, const IRBuilder::Expansion& expansion, BlockID afterWalk)
{
    RELEASE_ASSERT(!plan.arms.isEmpty());
    IRBuilder& builder = this->builder();
    BlockFrequency frequency = builder.function().at(builder.currentBlock()).frequency;

    auto endExpansion = [&](ValueID result) {
        if (expansion.results.isEmpty())
            builder.endExpansion(expansion, std::span<const ValueID> { });
        else if (expansion.results.size() == 1)
            builder.endExpansion(expansion, result);
        else {
            std::array<ValueID, 2> listAndGuard { result, builder.append(Opcode::AllGuards, { result }) };
            builder.endExpansion(expansion, listAndGuard);
        }
    };
    auto endArm = [&](ValueID result) {
        if (afterWalk)
            builder.emitJump(afterWalk);
        else
            endExpansion(result);
    };
    auto emitWalk = [&](const TestedObject& object, const OwnKeyWalkArm& arm) -> ValueID {
        Vector<WalkedField, 8> fields = fieldsWalkedBy(site.walk, arm);
        switch (site.walk) {
        case OwnKeyWalk::Values:
            return emitValues(object, arm, fields.span());
        case OwnKeyWalk::Entries:
            return emitEntries(object, arm, fields.span());
        case OwnKeyWalk::Assign:
            emitAssign(site, object, arm, fields.span());
            return site.target;
        case OwnKeyWalk::Spread:
            emitSpread(site, object, arm, fields.span());
            return { };
        case OwnKeyWalk::Copy:
            return emitCopy(object, arm, fields.span());
        case OwnKeyWalk::Stringify:
        case OwnKeyWalk::StringifyIntoText:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    ValueID source = site.source;
    if (copiesNothingFromNullish(site.walk) && !plan.unchangedAllocation && typeOf(site.source).mayBeUndefinedOrNull()) {
        BlockID firstArm;
        if (typeOf(site.source).isUndefinedOrNull()) {
            firstArm = builder.addBlock(frequency);
            emitTypeTest(site.source, TypeTable::nullish, firstArm);
        } else {
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(site.source, TypeTable::nullish), 0, frequency, frequency);
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
        TestedObject object = emitArmTests(source, plan, arm, nextArm);
        if (arm.copiesFieldsAtOnce) {
            emitCopyOfFields(site, object, arm, nextArm);
            endArm({ });
        } else
            endArm(emitWalk(object, arm));
        if (nextArm)
            builder.moveToEndOf(nextArm);
    }

    // Without the original operation, flow has proven that every object passes the tests of one arm.
    if (plan.needsOriginalCall)
        endExpansion(emitOriginalOperation(site));
    else if (afterLastArm)
        builder.emitUnreachable();
}

auto OwnKeyWalkExpander::fieldsWalkedBy(OwnKeyWalk walk, const OwnKeyWalkArm& arm) const -> Vector<WalkedField, 8>
{
    const Vector<Field>& fields = program().at(arm.layout).fields;
    std::span<const PropertyKeyID> excludedKeys;
    if (walk == OwnKeyWalk::Spread)
        excludedKeys = m_originalOperation.excludedKeys();

    Vector<WalkedField, 8> walkedFields;
    Vector<WalkedField, 8> fieldsWithSymbolKeys;
    for (uint32_t index : arm.order) {
        const Field& field = fields[index];
        RELEASE_ASSERT(!field.computedKey);
        if (std::ranges::contains(excludedKeys, field.key))
            continue;
        if (isEnumerableStringField(program(), field))
            walkedFields.append(WalkedField { field.key, index });
        else if (hasSymbolKey(program(), field))
            fieldsWithSymbolKeys.append(WalkedField { field.key, index });
    }
    if (visitsSymbolKeys(walk))
        walkedFields.appendVector(fieldsWithSymbolKeys);
    return walkedFields;
}

auto OwnKeyWalkExpander::emitArmTests(ValueID source, const OwnKeyWalkPlan& plan, const OwnKeyWalkArm& arm, BlockID nextArm) -> TestedObject
{
    if (arm.copiesFieldsAtOnce)
        return { emitTypeTest(source, TypeTable::object, nextArm), { } };
    TestedObject object { source, { } };
    if (plan.needsShapeTest)
        object = emitShapeTest(source, singleElementSpan(arm.layout), nextArm);
    if (plan.needsKeyOrderTest) {
        object.guard = emitKeyOrderTest(object, arm.layout, arm.order.span(), nextArm);
        return object;
    }
    if (object.guard || !plan.unchangedAllocation || !needsGuardForReads(program(), typeOf(object.object), arm))
        return object;
    return { plan.unchangedAllocation, plan.unchangedAllocation };
}

ValueID OwnKeyWalkExpander::emitValues(const TestedObject& object, const OwnKeyWalkArm& arm, std::span<const WalkedField> fields)
{
    Vector<ValueID, 8> values(fields.size(), [&](size_t index) {
        return appendWalkedFieldRead(*this, object, arm.layout, fields[index].field);
    });
    return appendArray(builder(), elementTypeOfArray(program(), m_originalOperation.type), values.span());
}

ValueID OwnKeyWalkExpander::emitEntries(const TestedObject& object, const OwnKeyWalkArm& arm, std::span<const WalkedField> fields)
{
    TypeID entryType = elementTypeOfArray(program(), m_originalOperation.type);
    TypeID entryContents = elementTypeOfArray(program(), entryType);
    Vector<ValueID, 8> entries(fields.size(), [&](size_t index) {
        std::array<ValueID, 2> entry {
            appendString(program().at(fields[index].key).text),
            appendWalkedFieldRead(*this, object, arm.layout, fields[index].field),
        };
        return appendArray(builder(), entryContents, entry);
    });
    return appendArray(builder(), entryType, entries.span());
}

void OwnKeyWalkExpander::emitAssign(const OwnKeyWalkSite& site, const TestedObject& object, const OwnKeyWalkArm& arm, std::span<const WalkedField> fields)
{
    Vector<ValueID, 8> values(fields.size(), [&](size_t index) {
        return appendWalkedFieldRead(*this, object, arm.layout, fields[index].field);
    });
    for (size_t index = 0; index < fields.size(); ++index) {
        Value store = IRBuilder::make(Opcode::PropSet, { site.target, values[index] });
        store.setKey(fields[index].key);
        store.setMode(ECMAMode::Strict);
        builder().append(WTF::move(store));
    }
}

void OwnKeyWalkExpander::emitSpread(const OwnKeyWalkSite& site, const TestedObject& object, const OwnKeyWalkArm& arm, std::span<const WalkedField> fields)
{
    Vector<ValueID, 8> values(fields.size(), [&](size_t index) {
        return appendWalkedFieldRead(*this, object, arm.layout, fields[index].field);
    });
    for (size_t index = 0; index < fields.size(); ++index) {
        Value definition = IRBuilder::make(Opcode::PropDefine, { site.target, values[index] });
        definition.setKey(fields[index].key);
        builder().append(WTF::move(definition));
    }
}

// The copy has the layout of its source, and gets its fields in the order in which the source got them.
ValueID OwnKeyWalkExpander::emitCopy(const TestedObject& object, const OwnKeyWalkArm& arm, std::span<const WalkedField> fields)
{
    Vector<ValueID, 8> values(fields.size(), [&](size_t index) {
        return appendWalkedFieldRead(*this, object, arm.layout, fields[index].field);
    });
    Vector<uint32_t, 8> listedFields(fields.size(), [&](size_t index) {
        return fields[index].field;
    });
    Value allocation = IRBuilder::make(Opcode::NewObject);
    allocation.setLayout(arm.layout);
    allocation.setFields(listedFields.span());
    allocation.setValuesOperands(values.span());
    return builder().append(WTF::move(allocation));
}

void OwnKeyWalkExpander::emitCopyOfFields(const OwnKeyWalkSite& site, const TestedObject& object, const OwnKeyWalkArm& arm, BlockID nextArm)
{
    Value copy = IRBuilder::make(Opcode::TryCopyFields, { site.target, object.object });
    copy.setLayout(arm.layout);
    IRBuilder::BranchBlocks blocks = builder().emitBranch(builder().append(WTF::move(copy)), builder().function().at(builder().currentBlock()).frequency, builder().function().at(nextArm).frequency);
    builder().moveToEndOf(blocks.notTaken);
    builder().emitJump(nextArm);
    builder().moveToEndOf(blocks.taken);
}

ValueID OwnKeyWalkExpander::emitOriginalOperation(const OwnKeyWalkSite&)
{
    Value original = m_originalOperation;
    if (resultRuleOf(original.opcode) == ResultRule::Declared)
        return builder().appendWithType(WTF::move(original), m_originalOperation.type);
    return builder().append(WTF::move(original));
}

static bool hasBranchElsewhere(const Function& function, const UseLists& useLists, ValueID test, BlockID block)
{
    return std::ranges::any_of(useLists.usesOf(test), [&](const Use& use) {
        return use.kind == Use::Kind::TerminatorOperand && use.block != block && function.at(use.block).terminator.isConditionalBranch();
    });
}

static Vector<TestOfAllocation, 4> findCandidateTests(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<TestOfAllocation, 4> tests;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (!testsOfOwnKeys.contains(value.opcode))
                continue;
            ValueID allocation = context.analyses.identityOf(value.objectOperand());
            if (function.at(allocation).opcode != Opcode::NewObject)
                continue;
            if (hasBranchElsewhere(function, context.analyses.useLists(), values[index], block))
                continue;
            tests.append(TestOfAllocation { { block, index }, values[index], allocation });
        }
    }
    return tests;
}

static std::array<Value, conditionsPerAllocation> conditionsAtCreation(FunctionPassContext& context, ValueID allocationID)
{
    const Value& allocation = context.function.at(allocationID);
    LayoutID layout = allocation.layout();
    Value keyOrderTest = IRBuilder::make(Opcode::HasKeyOrder, { allocationID });
    keyOrderTest.setLayout(layout);
    keyOrderTest.setFields(allocation.fields());
    return {
        makeShapeTest(allocationID, context.types.completeLayoutSet(singleElementSpan(layout))),
        WTF::move(keyOrderTest),
        IRBuilder::make(Opcode::HasNoExtraProperties, { allocationID }),
    };
}

BitVector findUnchangedAllocations(FunctionPassContext& context, std::span<const AllocationUse> uses)
{
    BitVector unchanged(uses.size());
    if (uses.empty())
        return unchanged;

    TestConditions conditions(context.analyses);
    Vector<TestRequirement> requirements;
    Vector<ExistingProof> proofs;
    Vector<size_t, 8> askedUses;
    for (size_t index = 0; index < uses.size(); ++index) {
        const AllocationUse& use = uses[index];
        if (!allocatesOwnLayout(context.program, context.function.at(use.allocation)))
            continue;
        std::array<Value, conditionsPerAllocation> wantedConditions = conditionsAtCreation(context, use.allocation);
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

static Vector<TestOfAllocation, 4> findDecidableTests(FunctionPassContext& context)
{
    Vector<TestOfAllocation, 4> candidateTests = findCandidateTests(context);
    Vector<AllocationUse, 4> uses = candidateTests.map<Vector<AllocationUse, 4>>([](const TestOfAllocation& test) {
        return AllocationUse { test.allocation, test.point };
    });

    Vector<TestOfAllocation, 4> tests;
    for (size_t index : findUnchangedAllocations(context, uses.span()))
        tests.append(candidateTests[index]);
    return tests;
}

static bool evaluateAtCreation(const Value& test, const Value& allocation)
{
    if (test.opcode == Opcode::HasNoExtraProperties)
        return true;
    return test.layout() == allocation.layout() && std::ranges::equal(test.fields(), allocation.fields());
}

static void decideBranchOn(IRBuilder& builder, const TestOfAllocation& test, bool result)
{
    BlockID block = test.point.block;
    Terminator& terminator = builder.function().edit(block).terminator;
    if (!terminator.isConditionalBranch() || terminator.conditionOperand() != test.test)
        return;
    if (!result) {
        decideTerminator(builder, block, static_cast<uint32_t>(&terminator.notTaken() - terminator.targets.begin()));
        return;
    }

    // The taken edge passes a guard, which only a test or an allocation can stand for.
    Target taken = terminator.taken();
    builder.setOrigin(terminator.origin());
    terminator = Terminator { };
    builder.moveToEndOf(block);
    Arguments arguments = WTF::move(taken.arguments);
    for (size_t index = 0; index < taken.proofs.size(); ++index)
        arguments.append(builder.append(Opcode::AllGuards, { test.allocation }));
    builder.emitJump(taken.block, WTF::move(arguments));
}

static void becomeBoolean(FunctionPassContext& context, ValueID id, bool boolean)
{
    Value& value = context.function.edit(id);
    value.become(Opcode::Boolean);
    value.setBoolean(boolean);
    value.type = resultTypeOf(context.program, context.function, value, context.types);
    value.setExpected({ });
}

bool foldKeyOrderTests(FunctionPassContext& context)
{
    Vector<TestOfAllocation, 4> tests = findDecidableTests(context);
    if (tests.isEmpty())
        return false;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    for (const TestOfAllocation& test : tests) {
        bool result = evaluateAtCreation(context.function.at(test.test), context.function.at(test.allocation));
        decideBranchOn(builder, test, result);
        becomeBoolean(context, test.test, result);
    }
    context.analyses.didChangeValues();
    builder.emptyUnreachableBlocks();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t takenTargetOfTest = 0;

static bool hasLastingAnswer(const Value& test)
{
    return provesForever(provesOf(test.opcode));
}

static uint32_t tagMaskOf(const ObjectVariant& variant)
{
    return variant.tagBit ? 1u << *variant.tagBit : 0;
}

static bool hasUntaggedVariant(const ObjectState& state)
{
    return state.variants.containsIf([](const ObjectVariant& variant) {
        return !variant.tagBit;
    });
}

static std::optional<TestEntry> evaluateAtOperand(const AllocationSinking& sinking, const ObjectState& state, TestEntry entry, uint32_t operandIndex)
{
    const Value& test = sinking.function().at(entry.test);
    ShapeContext context = sinking.shapeContext(entry.object);
    uint32_t tagBitsWhenTrue = 0;
    uint32_t tagBitsWhenFalse = 0;
    unsigned numberOfTrueAnswers = 0;
    for (const ObjectVariant& variant : state.variants) {
        switch (evaluateTest(context, context.family.at(variant.shape), test, operandIndex)) {
        case KnownBoolean::Unknown:
            return std::nullopt;
        case KnownBoolean::True:
            ++numberOfTrueAnswers;
            tagBitsWhenTrue |= tagMaskOf(variant);
            break;
        case KnownBoolean::False:
            tagBitsWhenFalse |= tagMaskOf(variant);
            break;
        }
    }

    if (!numberOfTrueAnswers || numberOfTrueAnswers == state.variants.size()) {
        entry.result = !!numberOfTrueAnswers;
        return entry;
    }
    if (hasUntaggedVariant(state) || (tagBitsWhenTrue & tagBitsWhenFalse))
        return std::nullopt;
    entry.tagBitsWhenTrue = tagBitsWhenTrue;
    return entry;
}

static std::optional<TestEntry> answerAtBranch(const SinkingState& state, const Function& function, ValueID condition, NOESCAPE const Invocable<std::optional<TestEntry>(ValueID)> auto& evaluate)
{
    const TestEntry* recorded = state.findTest(condition);
    if (!recorded)
        return std::nullopt;
    if (recorded->result)
        return *recorded;
    if (hasLastingAnswer(function.at(condition)))
        return evaluate(condition);
    if (!recorded->tagBitsWhenTrue)
        return std::nullopt;
    return *recorded;
}

static std::optional<uint32_t> tagBitsOnEdge(const SinkingState& state, const TestEntry& answer, uint32_t targetIndex)
{
    const ObjectState* objectState = state.find(answer.object);
    if (!objectState || hasUntaggedVariant(*objectState))
        return std::nullopt;
    uint32_t tagBits = objectState->tagBits();
    return targetIndex == takenTargetOfTest ? tagBits & answer.tagBitsWhenTrue : tagBits & ~answer.tagBitsWhenTrue;
}

// A merge assigns new tag bits and drops a test whose entries differ, so an entry leaves its block without its answer.
static void forgetAnswers(SinkingState& state, const Function& function)
{
    for (TestEntry& entry : state.tests)
        entry = TestEntry { entry.test, hasLastingAnswer(function.at(entry.test)) ? VirtualObjectID() : entry.object, std::nullopt };
}

static bool mayTakeShapeCase(const ShapeContext& context, const VirtualShape& shape, const Terminator& switchOnShape, uint32_t targetIndex)
{
    std::optional<uint32_t> shapeCase = findShapeCase(context, shape, switchOnShape);
    return !shapeCase || *shapeCase == targetIndex;
}

static bool hasOperands(const Value& value)
{
    bool hasOperand = false;
    value.forEachOperand([&](ValueID, uint32_t) {
        hasOperand = true;
    });
    return hasOperand;
}

static bool canBeBuiltByCaller(const Function& function, const VirtualObjectDescription& description)
{
    const Value& allocation = function.at(description.allocation);
    // An opaque object is built as a copy of its allocation, whose operands are values of this function.
    if (description.kind == VirtualObjectKind::Opaque && hasOperands(allocation))
        return false;
    return description.canBeBuilt && !description.staysInPlace && boundToOf(allocation.opcode) == BoundTo::Nothing;
}

Execution ObjectInterpreter::executeTest(ValueID id)
{
    if (testsLeftToRun.contains(function().at(id).opcode))
        return executeOther(id);

    std::optional<TestEntry> entry = evaluateTestOn(id);
    if (!entry) {
        noteListsThatMayNotFit(id);
        return executeOther(id);
    }

    m_state.setTest(*entry);
    if (isRewriting())
        replaceTest(id, *entry);
    return Execution::Interpreted;
}

void ObjectInterpreter::noteListsThatMayNotFit(ValueID test)
{
    const Value& value = function().at(test);
    VirtualObjectID object = value.opcode == Opcode::HasArrayStorage ? objectOf(value.arrayOperand()) : VirtualObjectID();
    if (!object)
        return;
    for (const ObjectVariant& variant : stateOf(object)->variants) {
        const VirtualShape& shape = shapeOf(object, variant);
        if (shape.kind != VirtualShapeKind::Fields)
            continue;
        const VirtualObjectDescription& description = m_sinking.familyOf(object).descriptionOf(shape);
        if (classifyListOfNumbers(program(), description) == ListOfNumbers::MayNotFit)
            m_sinking.didTestListThatMayNotFit(description.allocation);
    }
}

std::optional<TestEntry> ObjectInterpreter::evaluateTestOn(ValueID test) const
{
    const Value& value = function().at(test);
    if (value.opcode == Opcode::StrictEq && objectOf(value.leftOperand()) && objectOf(value.rightOperand()))
        return evaluateComparison(test);

    VirtualObjectID object;
    std::optional<uint32_t> objectOperand;
    std::optional<uint32_t> guardOperand;
    bool hasOneObject = true;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        bool isGuard = value.operandKind(index) == OperandKind::Guard;
        VirtualObjectID tracked = isGuard ? guardedObjectOf(operand) : objectOf(operand);
        if (!tracked)
            return;
        std::optional<uint32_t>& position = isGuard ? guardOperand : objectOperand;
        if (position || (object && object != tracked))
            hasOneObject = false;
        position = index;
        object = tracked;
    });
    if (!object || !hasOneObject)
        return std::nullopt;

    uint32_t operandIndex = objectOperand ? *objectOperand : *guardOperand;
    return evaluateAtOperand(m_sinking, *stateOf(object), TestEntry { test, object, std::nullopt }, operandIndex);
}

std::optional<TestEntry> ObjectInterpreter::evaluateComparison(ValueID test) const
{
    const Value& value = function().at(test);
    VirtualObjectID left = objectOf(value.leftOperand());
    VirtualObjectID right = objectOf(value.rightOperand());
    const ObjectState& leftState = *stateOf(left);
    const ObjectState& rightState = *stateOf(right);
    bool areSameObject = left == right;

    if (!areSameObject) {
        if (std::optional<TestEntry> entry = evaluateAtOperand(m_sinking, leftState, TestEntry { test, left, std::nullopt }, OperandIndex::StrictEq::left))
            return entry;
        if (std::optional<TestEntry> entry = evaluateAtOperand(m_sinking, rightState, TestEntry { test, right, std::nullopt }, OperandIndex::StrictEq::right))
            return entry;
        if (m_sinking.at(left).family != m_sinking.at(right).family)
            return std::nullopt;
    }

    ShapeContext context = m_sinking.shapeContext(left);
    std::optional<bool> result;
    for (const ObjectVariant& leftVariant : leftState.variants) {
        for (const ObjectVariant& rightVariant : rightState.variants) {
            if (areSameObject && leftVariant != rightVariant)
                continue;
            KnownBoolean answer = evaluateTestOfTwoShapes(context, shapeOf(left, leftVariant), shapeOf(right, rightVariant), areSameObject, value);
            if (answer == KnownBoolean::Unknown)
                return std::nullopt;
            bool isTrue = answer == KnownBoolean::True;
            if (result && *result != isTrue)
                return std::nullopt;
            result = isTrue;
        }
    }
    return TestEntry { test, left, result };
}

void ObjectInterpreter::replaceTest(ValueID test, const TestEntry& entry)
{
    if (entry.result) {
        replaceRead(test, cursor().builder().appendBoolean(*entry.result));
        return;
    }
    replaceRead(test, emitTagTest(entry.object, entry.tagBitsWhenTrue));
}

ValueID ObjectInterpreter::emitTagTest(VirtualObjectID object, uint32_t tagBits)
{
    ValueID tag = values().find(object)->tag;
    RELEASE_ASSERT(tag);
    IRBuilder& builder = cursor().builder();
    ValueID mask = builder.appendInteger(tagBits);
    ValueID masked = builder.append(Opcode::BitAnd, { tag, mask });
    return builder.append(Opcode::ToBoolean, { masked });
}

ValueID ObjectInterpreter::tagOf(VirtualObjectID object, BlockID target)
{
    const ObjectState& state = *stateOf(object);
    if (state.needsTag()) {
        ValueID tag = values().find(object)->tag;
        RELEASE_ASSERT(tag);
        return tag;
    }

    std::optional<uint8_t> tagBit = m_sinking.familyOf(object).findTagBit(target, state.variants[0].shape);
    RELEASE_ASSERT(tagBit);
    return cursor().builder().appendInteger(int64_t(1) << *tagBit);
}

void ObjectInterpreter::buildBeforeBranch(const Terminator& terminator)
{
    unsigned numberOfLiveTargets = 0;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex)
        numberOfLiveTargets += !isDeadEdge(terminator, targetIndex);
    if (numberOfLiveTargets < 2)
        return;

    Vector<VirtualObjectID, 4> objects;
    for (const ObjectEntry& entry : m_state.objects) {
        if (!entry.state.isCell() && m_sinking.at(entry.object).isBuiltBeforeBranches)
            objects.append(entry.object);
    }
    for (VirtualObjectID object : objects)
        build(object, BuildCause { .limit = LimitID::MaxMaterializations });
}

void ObjectInterpreter::executeTerminator(const Terminator& terminator)
{
    buildBeforeBranch(terminator);
    switch (terminator.kind) {
    case TerminatorKind::Return:
        executeReturn(terminator);
        return;
    case TerminatorKind::Branch:
        // A test may have narrowed a join of objects and booleans to its booleans, and what is left is a condition.
        requireCell(terminator.conditionOperand(), causeOf(terminator, OperandIndex::Branch::condition));
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::Unreachable:
        return;
    case TerminatorKind::SwitchOnShape: {
        ValueID subject = terminator.subjectOperand();
        VirtualObjectID object = objectOf(subject);
        if (!object)
            return;
        ShapeContext context = m_sinking.shapeContext(object);
        bool hasUnknownCase = stateOf(object)->variants.containsIf([&](const ObjectVariant& variant) {
            return !findShapeCase(context, shapeOf(object, variant), terminator);
        });
        if (hasUnknownCase)
            requireCell(subject, causeOf(terminator, OperandIndex::SwitchOnShape::subject));
        return;
    }
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::TryCallInline: {
        Vector<std::pair<ValueID, uint32_t>, 4> operands;
        terminator.forEachOperand([&](ValueID operand, uint32_t index) {
            operands.append({ operand, index });
        });
        for (auto [operand, index] : operands) {
            if (objectOf(operand))
                requireCell(operand, causeOf(terminator, index));
        }
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ObjectInterpreter::executeRelevantValue(const BlockValues& values, uint32_t entry)
{
    ValueID id = values[entry & ~AllocationSinking::onlyWritesOrThrows];
    if (!(entry & AllocationSinking::onlyWritesOrThrows)) {
        execute(id);
        return;
    }
    if (m_sinking.checksShortcuts()) [[unlikely]] {
        SinkingState state = m_state;
        ObjectInterpreter(m_sinking, m_mode, state).execute(id);
        forgetFieldsWrittenBy(id);
        RELEASE_ASSERT(m_state == state);
        return;
    }
    forgetFieldsWrittenBy(id);
}

void ObjectInterpreter::executeTerminatorOf(BlockID block)
{
    const Terminator& terminator = function().at(block).terminator;
    if (m_sinking.mentionsName(terminator, block)) {
        executeTerminator(terminator);
        return;
    }
    if (!m_sinking.checksShortcuts()) [[likely]]
        return;
    SinkingState before = m_state;
    executeTerminator(terminator);
    RELEASE_ASSERT(m_state == before);
}

// In the state behind the block. A value that mentions no name and writes nothing does the same in every state.
void ObjectInterpreter::checkThatOtherValuesChangeNothing(BlockID block)
{
    const BlockValues& values = function().at(block).values;
    std::span<const uint32_t> relevantValues = m_sinking.relevantValuesOf(block);
    SinkingState before = m_state;
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (!relevantValues.empty() && (relevantValues.front() & ~AllocationSinking::onlyWritesOrThrows) == index) {
            relevantValues = relevantValues.subspan(1);
            continue;
        }
        execute(values[index]);
        RELEASE_ASSERT(m_state == before);
    }
}

void ObjectInterpreter::executeReturn(const Terminator& terminator)
{
    ValueID value = terminator.valueOperand();
    if (!value)
        return;

    BuildCause cause = causeOf(terminator, OperandIndex::Return::value);
    if (m_sinking.policy().returnedObjects == ReturnedObjects::AreBuilt) {
        requireCell(value, cause);
        return;
    }

    VirtualObjectID object = objectOf(value);
    if (object) {
        const VirtualFamily& family = m_sinking.familyOf(object);
        bool canStayVirtual = std::ranges::all_of(stateOf(object)->variants, [&](const ObjectVariant& variant) {
            const VirtualShape& shape = family.at(variant.shape);
            return shape.kind != VirtualShapeKind::Fields || canBeBuiltByCaller(function(), family.descriptionOf(shape));
        });
        if (canStayVirtual) {
            Vector<VirtualObjectID, 4> objectsInFields;
            for (const HeldObject& entry : stateOf(object)->heldObjects)
                objectsInFields.append(entry.object);
            for (VirtualObjectID objectInField : objectsInFields)
                build(objectInField, cause);
        } else
            requireCell(value, cause);
    }

    if (m_mode != SinkingMode::Count)
        return;
    if (object && !stateOf(object)->isCell())
        m_sinking.at(object).diesVirtual = true;
    m_sinking.addVirtualReturn(VirtualReturn { m_state.block, value, object, object ? *stateOf(object) : ObjectState { } });
}

bool ObjectInterpreter::isDeadEdge(const Terminator& terminator, uint32_t targetIndex) const
{
    switch (terminator.kind) {
    case TerminatorKind::Branch: {
        std::optional<TestEntry> answer = answerAtBranch(m_state, function(), terminator.conditionOperand(), [&](ValueID test) {
            return evaluateTestOn(test);
        });
        if (!answer)
            return false;
        if (answer->result)
            return (targetIndex == takenTargetOfTest) != *answer->result;
        std::optional<uint32_t> tagBits = tagBitsOnEdge(m_state, *answer, targetIndex);
        return tagBits && !*tagBits;
    }
    case TerminatorKind::SwitchOnShape: {
        VirtualObjectID object = objectOf(terminator.subjectOperand());
        if (!object || !stateOf(object)->isVirtual())
            return false;
        ShapeContext context = m_sinking.shapeContext(object);
        return !stateOf(object)->variants.containsIf([&](const ObjectVariant& variant) {
            return mayTakeShapeCase(context, shapeOf(object, variant), terminator, targetIndex);
        });
    }
    case TerminatorKind::Jump:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ObjectInterpreter::hasInterpretedTest(const Terminator& terminator) const
{
    switch (terminator.kind) {
    case TerminatorKind::Branch:
        return !!m_state.findTest(terminator.conditionOperand());
    case TerminatorKind::SwitchOnShape: {
        VirtualObjectID object = objectOf(terminator.subjectOperand());
        return object && stateOf(object)->isVirtual();
    }
    case TerminatorKind::Jump:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ObjectInterpreter::narrowOnEdge(const Terminator& terminator, uint32_t targetIndex)
{
    switch (terminator.kind) {
    case TerminatorKind::Branch: {
        std::optional<TestEntry> answer = answerAtBranch(m_state, function(), terminator.conditionOperand(), [&](ValueID test) {
            return evaluateTestOn(test);
        });
        if (!answer || answer->result)
            break;
        if (std::optional<uint32_t> tagBits = tagBitsOnEdge(m_state, *answer, targetIndex))
            narrow(answer->object, *tagBits);
        break;
    }
    case TerminatorKind::SwitchOnShape: {
        VirtualObjectID object = objectOf(terminator.subjectOperand());
        if (!object || !stateOf(object)->isVirtual())
            break;
        if (std::optional<uint32_t> tagBits = tagBitsOfShapeCase(terminator, object, targetIndex))
            narrow(object, *tagBits);
        break;
    }
    case TerminatorKind::Jump:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        break;
    }
    forgetAnswers(m_state, function());
}

void ObjectInterpreter::forgetAnswersOfTests()
{
    forgetAnswers(m_state, function());
}

void ObjectInterpreter::narrow(VirtualObjectID object, uint32_t keptTagBits)
{
    ObjectState& state = *stateOf(object);
    state.variants.removeAllMatching([&](const ObjectVariant& variant) {
        return variant.tagBit && !(keptTagBits & tagMaskOf(variant));
    });
    RELEASE_ASSERT(!state.variants.isEmpty());
    state.lacksFieldsOnSomeWay |= std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        return shapeOf(object, variant).kind == VirtualShapeKind::Constant;
    });

    state.heldObjects.removeAllMatching([&](const HeldObject& held) {
        return !state.variants.containsIf([&](const ObjectVariant& variant) {
            return hasStored(shapeOf(object, variant), held.field);
        });
    });
}

std::optional<uint32_t> ObjectInterpreter::tagBitsOfShapeCase(const Terminator& terminator, VirtualObjectID object, uint32_t targetIndex) const
{
    const ObjectState& state = *stateOf(object);
    if (hasUntaggedVariant(state))
        return std::nullopt;

    ShapeContext context = m_sinking.shapeContext(object);
    uint32_t tagBits = 0;
    for (const ObjectVariant& variant : state.variants) {
        if (mayTakeShapeCase(context, shapeOf(object, variant), terminator, targetIndex))
            tagBits |= tagMaskOf(variant);
    }
    return tagBits;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

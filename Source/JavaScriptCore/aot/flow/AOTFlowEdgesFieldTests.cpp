/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

#include "AOTValueIdentity.h"
#include <algorithm>
#include <cstdlib>

namespace JSC { namespace AOT { namespace Typed {

// `x.k`, seen through the casts of what was read.
const Value* FlowSummaryBuilder::findFieldRead(ValueID value) const
{
    const Value& read = m_function.at(sameObjectThroughCasts(m_function, value));
    return read.opcode == Opcode::PropGet && !read.isFreeName() ? &read : nullptr;
}

std::optional<FlowSummaryBuilder::FieldTest> FlowSummaryBuilder::fieldTestOfBranch(const Terminator& branch, bool isTaken) const
{
    const Value& test = m_function.at(branch.conditionOperand());
    if (test.opcode != Opcode::StrictEq)
        return std::nullopt;
    for (uint32_t index = 0; index < 2; ++index) {
        const Value* read = findFieldRead(test.operand(index));
        ValueID other = test.operand(1 - index);
        if (!read || !typeOf(other).isSingleValue() || typeOf(other).hasObjects())
            continue;
        FieldTest fieldTest { m_identities->identityOf(read->objectOperand()), read->key(), { }, { }, isTaken ? FlowFilterKind::Inside : FlowFilterKind::Outside };
        if (typeOf(other).hasOnlyPrimitives(Primitive::Integer))
            fieldTest.ranges.append(typeOf(other).integers);
        else
            fieldTest.types.append(m_function.at(other).type);
        return fieldTest;
    }
    return std::nullopt;
}

std::optional<FlowSummaryBuilder::FieldTest> FlowSummaryBuilder::fieldTestOfSwitch(const Terminator& terminator, uint32_t targetIndex) const
{
    const Value* read = findFieldRead(terminator.subjectOperand());
    if (!read || targetIndex >= terminator.cases().size() || !Range::full().contains(terminator.cases()[targetIndex]))
        return std::nullopt;
    return FieldTest { m_identities->identityOf(read->objectOperand()), read->key(), { }, { Range::one(terminator.cases()[targetIndex]) }, FlowFilterKind::Inside };
}

std::optional<FlowSummaryBuilder::FieldTest> FlowSummaryBuilder::fieldTestOn(Edge edge) const
{
    if (edge.isHandlerEdge())
        return std::nullopt;
    const Terminator& terminator = m_function.at(edge.from).terminator;
    switch (terminator.kind) {
    case TerminatorKind::Branch:
        return fieldTestOfBranch(terminator, !edge.targetIndex);
    case TerminatorKind::Switch:
        return fieldTestOfSwitch(terminator, edge.targetIndex);
    default:
        return std::nullopt;
    }
}

static void uniteAdjacentRanges(Vector<Range, 1>& ranges)
{
    std::ranges::sort(ranges, { }, &Range::minimum);
    size_t count = 0;
    for (Range range : ranges) {
        if (count && ranges[count - 1].maximum + 1 >= range.minimum)
            ranges[count - 1].maximum = std::max(ranges[count - 1].maximum, range.maximum);
        else
            ranges[count++] = range;
    }
    ranges.shrink(count);
}

// A block runs only after one of the tests on the edges into it came out that way. Several edges say something together where each finds one of several values in the same field.
std::optional<FlowSummaryBuilder::FieldTest> FlowSummaryBuilder::fieldTestOnEntryOf(BlockID block) const
{
    std::optional<FieldTest> result;
    for (Edge edge : m_cfg.incomingEdges(block)) {
        std::optional<FieldTest> test = fieldTestOn(edge);
        if (!test)
            return std::nullopt;
        if (!result) {
            result = WTF::move(test);
            continue;
        }
        if (result->object != test->object || result->key != test->key || result->kind != FlowFilterKind::Inside || test->kind != FlowFilterKind::Inside)
            return std::nullopt;
        result->ranges.appendVector(test->ranges);
        for (TypeID type : test->types) {
            if (!result->types.contains(type))
                result->types.append(type);
        }
    }
    if (result)
        uniteAdjacentRanges(result->ranges);
    return result;
}

void FlowSummaryBuilder::collectFieldTests()
{
    // FIXME: Off until it is measured.
    static const bool isSwitchedOn = !!getenv("BUN_AOT_FIELD_FILTERS");
    m_hasCollectedFieldTests = true;
    if (!isSwitchedOn)
        return;

    bool testsField = std::ranges::any_of(m_cfg.reversePostOrder(), [&](BlockID block) {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind == TerminatorKind::Switch)
            return !!findFieldRead(terminator.subjectOperand());
        if (terminator.kind != TerminatorKind::Branch)
            return false;
        const Value& test = m_function.at(terminator.conditionOperand());
        return test.opcode == Opcode::StrictEq && (findFieldRead(test.operand(0)) || findFieldRead(test.operand(1)));
    });
    if (!testsField)
        return;

    // A value that a test has narrowed on an edge is still the same object.
    m_identities = makeUnique<ValueIdentities>(m_function, m_cfg);
    for (BlockID block : m_cfg.reversePostOrder()) {
        if (std::optional<FieldTest> test = fieldTestOnEntryOf(block))
            m_fieldTests.add(block, WTF::move(*test));
    }
    if (m_fieldTests.isEmpty())
        return;

    m_dominators = makeUnique<Dominators>(m_cfg, false);
    m_nearestBlockWithFieldTests.fill(BlockID(), m_function.blocks.size());
    for (BlockID block : m_cfg.reversePostOrder()) {
        if (m_fieldTests.contains(block))
            m_nearestBlockWithFieldTests[block.index()] = block;
        else if (block != entryBlock)
            m_nearestBlockWithFieldTests[block.index()] = m_nearestBlockWithFieldTests[m_dominators->idom(block).index()];
    }
}

FlowLocalID FlowSummaryBuilder::addFieldFilter(FlowLocalID from, const FieldTest& test)
{
    RELEASE_ASSERT(test.kind == FlowFilterKind::Inside || test.ranges.size() + test.types.size() == 1);
    FlowLocalID to = m_assembler.addTemporary();
    for (TypeID type : test.types)
        m_assembler.add(FlowFilter { from, to, type, test.kind, test.key, std::nullopt });
    for (Range range : test.ranges)
        m_assembler.add(FlowFilter { from, to, { }, test.kind, test.key, m_assembler.addRange(range) });
    return to;
}

// What the value is at this place in the function, given the tests of its fields that lead here. The tests above the definition of a value that was narrowed here hold for it already.
FlowLocalID FlowSummaryBuilder::narrowByFieldTests(ValueID value)
{
    FlowLocalID local = localOf(value);
    if (!m_hasCollectedFieldTests)
        collectFieldTests();
    if (!local || m_fieldTests.isEmpty())
        return local;

    ValueID object = m_identities->identityOf(value);
    BlockID narrowedIn = m_blocksOfNarrowedValues.get(value);
    for (BlockID block = m_nearestBlockWithFieldTests[m_block.index()]; block && block != narrowedIn; ) {
        if (const FieldTest& test = m_fieldTests.find(block)->value; test.object == object)
            local = addFieldFilter(local, test);
        block = block == entryBlock ? BlockID() : m_nearestBlockWithFieldTests[m_dominators->idom(block).index()];
    }
    return local;
}

// The receiver of an access and what a call is handed are used where the operation stands, with or without a mark of the front end.
FlowLocalID FlowSummaryBuilder::localAtUse(ValueID value)
{
    if (!value)
        return { };
    if (!m_hasCollectedFieldTests)
        collectFieldTests();
    BlockID nearest = m_fieldTests.isEmpty() ? BlockID() : m_nearestBlockWithFieldTests[m_block.index()];
    if (!nearest)
        return localOf(value);

    auto known = m_localsAtUses.find({ value, nearest });
    if (known != m_localsAtUses.end())
        return known->value;
    FlowLocalID local = narrowByFieldTests(value);
    m_localsAtUses.add({ value, nearest }, local);
    return local;
}

void FlowSummaryBuilder::addPredict(ValueID id, const Value& value)
{
    m_assembler.copy(localAtUse(value.valueOperand()), localOf(id));
    if (!m_fieldTests.isEmpty()) {
        if (BlockID block = m_nearestBlockWithFieldTests[m_block.index()])
            m_blocksOfNarrowedValues.add(id, block);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

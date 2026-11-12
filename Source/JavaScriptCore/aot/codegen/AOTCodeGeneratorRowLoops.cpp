/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTElementAccessEmitter.h"
#include "AOTOperationContext.h"
#include "AOTThrowEmitter.h"
#include "AOTVectorOutput.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

struct RowLoop {
    enum class Form : uint8_t { Sum, ColumnSum, Map, Gather, Nest };
    struct Step {
        enum class Kind : uint8_t { Value, Name, OwnTest, Test, Row };
        Kind kind { Kind::Value };
        BlockID block;
        ValueID value;
        ValueID source;
        bool expected { true };
        unsigned guard { 0 };
    };
    struct ElementRead {
        ValueID access;
        BlockID block;
        ValueID list;
        bool isColumn { false };
    };
    struct ElementStore {
        ValueID access;
        BlockID block;
        ValueID list;
        ValueID number;
    };
    struct Body {
        Vector<BlockID, 16> blocks;
        Vector<Step, 16> decisions;
        Vector<Step, 16> operations;
        Vector<ElementRead, 6> reads;
        Vector<ElementStore, 2> stores;
        Vector<ValueID, 8> impliedTests;
        Vector<ValueID, 4> checkedReads;
        bool isVector { true };
    };
    Form form { Form::Sum };
    BlockID head;
    ValueID counter;
    ValueID bound;
    ValueID poll;
    BlockID innerHead;
    ValueID innerCounter;
    ValueID innerBound;
    ValueID firstSum;
    ValueID sum;
    ValueID nextSum;
    std::optional<unsigned> innerGuard;
    bool storesAtCounter { false };
    Vector<Body, 1> bodies;
    Vector<Step, 24> stepsInFront;
    Vector<Step, 24> stepsBefore;
    Vector<Step, 24> stepsBesideSum;
    Vector<Step, 24> stepsAfter;
    ValueID store;
    std::unique_ptr<RowLoop> nested;
    Vector<Step, 8> stepsAtEnd;
    Vector<Step, 24> stepsOfNest;
};

static constexpr int32_t longestInnerLoop = 1 << 20;
static constexpr int32_t mostNumbersKept = 64;

class RowLoopFinder {
public:
    using Body = RowLoop::Body;
    using Step = RowLoop::Step;
    using ElementRead = RowLoop::ElementRead;
    using ElementStore = RowLoop::ElementStore;
    using RowGuard = CodeGenerator::RowGuard;
    enum class UsesCounts : bool { No, Yes };

    RowLoopFinder(CodeGenerator& generator, BlockID head, UsesCounts usesCounts, std::span<const BlockID> innerHeadsTaken, const RowLoopFinder* outer = nullptr)
        : m_generator(generator)
        , m_code(generator.code())
        , m_usesCounts(usesCounts == UsesCounts::Yes)
        , m_innerHeadsTaken(innerHeadsTaken)
        , m_outer(outer)
    {
        m_found.head = head;
    }

    std::optional<RowLoop> find();
    std::optional<RowLoop> findGather();
    std::optional<RowLoop> findNest(std::span<const std::pair<BlockID, BlockID>> headsTaken);

private:
    const Operation& at(ValueID value) const { return m_code.at(value); }
    Opcode opcodeOf(ValueID value) const { return m_generator.opcodeOf(value); }
    bool isConstant(ValueID value) const { return m_code.isConstant(value); }
    ValueID passed(ValueID value) const
    {
        static constexpr OpcodeSet passingOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
        while (!isConstant(value) && passingOn.contains(opcodeOf(value)))
            value = at(value).valueOperand();
        return value;
    }
    ValueID representative(ValueID value) const
    {
        static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Unbox, Opcode::Box, Opcode::WidenInteger, Opcode::NarrowInteger };
        for (bool isChanged = true; isChanged && value && !isConstant(value);) {
            isChanged = false;
            if (names.contains(opcodeOf(value))) {
                value = at(value).valueOperand();
                isChanged = true;
            }
            for (const auto& [name, source] : m_aliases) {
                if (name == value) {
                    value = source;
                    isChanged = true;
                }
            }
        }
        return value;
    }
    bool isCongruent(ValueID first, ValueID second) const;
    bool isInLoop(ValueID value) const
    {
        BlockID home = isConstant(value) ? BlockID() : m_code.blockOf(value);
        return home && (m_path.contains(home) || isInnerBlock(home) || m_nestedBlocks.contains(home));
    }
    bool isOfInnerLoop(ValueID value) const
    {
        BlockID home = isConstant(value) ? BlockID() : m_code.blockOf(value);
        return home && isInnerBlock(home);
    }
    bool isInnerBlock(BlockID block) const
    {
        return block == m_found.innerHead || std::ranges::any_of(m_found.bodies, [&](const Body& body) { return body.blocks.contains(block); });
    }
    bool isAvailableBefore(ValueID value) const { return isConstant(value) || (!isInLoop(value) && (m_generator.m_values[value.index()] || (m_outer && m_outer->isInLoop(value)))); }
    bool isPlainBlock(BlockID id) const
    {
        const Block& block = m_code.at(id);
        return m_code.isReached(id) && block.frequency == BlockFrequency::Normal && !m_code.handlerOf(id) && !block.subject && !block.isEntry && !block.isHandlerBlock();
    }
    bool stepsByOne(ValueID passedOn, ValueID counter) const;
    const RowGuard* findGuard(BlockID presenceBlock, unsigned& place) const;
    auto& steps() { return m_found.innerHead ? m_found.stepsAfter : m_found.stepsBefore; }
    bool isBehindNested() const { return !!m_found.nested; }
    bool hasStored() const { return m_found.store || m_hasMapped; }
    bool visit(BlockID);
    bool follow(const Edge&);
    bool followInnerLoop(const Edge& entry);
    bool followNested(const Edge&);
    bool findOrderOfNest();
    bool isColumnIndex(ValueID) const;
    bool isInvariant(ValueID, unsigned depth = 0) const;
    bool isImpliedInnerTest(const Body&, ValueID) const;
    bool hasImpliedTests(Body&) const;
    bool exploreBody(const Edge&, const Body& before);
    bool isPartOf(ValueID access, ValueID term) const;
    bool addOperation(BlockID, ValueID);
    bool addInnerOperation(Body&, BlockID, ValueID);
    bool addTest(BlockID, ValueID test, bool isOwnTest, bool expected);
    bool isTerm(const Body&, ValueID, unsigned depth = 0) const;
    bool dependsOnCounter(ValueID, unsigned depth = 0) const;
    bool dependsOnSum(ValueID, unsigned depth = 0) const;
    bool findOrderOfSteps();
    bool finish(const Edge& back);
    bool readsStoredColumn();
    void dropImpliedTests();
    void findBound();
    void dropUnusedSteps();

    struct Place {
        size_t path;
        size_t aliases;
        size_t stepsBefore;
        size_t stepsAfter;
        size_t names;
        size_t converted;
        size_t stepsAtEnd;
    };
    Place place() const { return { m_path.size(), m_aliases.size(), m_found.stepsBefore.size(), m_found.stepsAfter.size(), m_namesOfRows.size(), m_converted.size(), m_found.stepsAtEnd.size() }; }
    void restorePlace(const Place&, bool hadInnerLoop);

    static constexpr unsigned mostBlocks = 64;
    static constexpr unsigned mostVisits = 400;
    static constexpr size_t mostBodies = 16;
    static constexpr unsigned mostDepth = 12;
    static constexpr size_t mostReads = 6;
    CodeGenerator& m_generator;
    const Code& m_code;
    bool m_usesCounts { false };
    unsigned m_visits { 0 };
    bool m_hasMapped { false };
    bool m_isNest { false };
    std::span<const std::pair<BlockID, BlockID>> m_headsTaken;
    Vector<BlockID, 32> m_nestedBlocks;
    std::span<const BlockID> m_innerHeadsTaken;
    const RowLoopFinder* m_outer { nullptr };
    Vector<BlockID, 64> m_path;
    Vector<std::pair<ValueID, ValueID>, 16> m_aliases;
    Vector<ValueID, 16> m_namesOfRows;
    Vector<ValueID, 4> m_converted;
    RowLoop m_found;
};

bool RowLoopFinder::isCongruent(ValueID first, ValueID second) const
{
    if (first == second)
        return true;
    const Operation& a = at(first);
    const Operation& b = at(second);
    if (a.opcode != b.opcode || a.representation != b.representation || a.flags != b.flags || a.opcode == Opcode::Parameter || !std::ranges::equal(a.payload(), b.payload()) || a.operands().size() != b.operands().size())
        return false;
    for (size_t index = 0; index < a.operands().size(); ++index) {
        if (representative(a.operands()[index]) != representative(b.operands()[index]))
            return false;
    }
    return true;
}

bool RowLoopFinder::stepsByOne(ValueID passedOn, ValueID counter) const
{
    static constexpr OpcodeSet conversions { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::NumberToInteger };
    ValueID stepped = passedOn;
    while (!isConstant(stepped) && conversions.contains(opcodeOf(stepped)))
        stepped = at(stepped).valueOperand();
    if (isConstant(stepped) || opcodeOf(stepped) != Opcode::Increment || at(passedOn).representation != at(counter).representation)
        return false;
    OperatorForm form = at(stepped).form();
    return (form == OperatorForm::Integers || form == OperatorForm::CheckedIntegers) && passed(at(stepped).valueOperand()) == counter;
}

const CodeGenerator::RowGuard* RowLoopFinder::findGuard(BlockID presenceBlock, unsigned& place) const
{
    if (!m_generator.m_rows)
        return nullptr;
    const auto& guards = m_generator.m_rows->guards;
    for (place = 0; place < guards.size(); ++place) {
        if (guards[place].presenceBlock == presenceBlock)
            return &guards[place];
    }
    return nullptr;
}

void RowLoopFinder::restorePlace(const Place& place, bool hadInnerLoop)
{
    m_path.shrink(place.path);
    m_aliases.shrink(place.aliases);
    m_found.stepsBefore.shrink(place.stepsBefore);
    m_found.stepsAfter.shrink(place.stepsAfter);
    m_namesOfRows.shrink(place.names);
    m_converted.shrink(place.converted);
    m_found.stepsAtEnd.shrink(place.stepsAtEnd);
    if (hadInnerLoop)
        return;
    m_found.innerHead = { };
    m_found.bodies.shrink(0);
    m_found.innerGuard = std::nullopt;
    m_hasMapped = false;
    m_found.sum = { };
    m_found.nextSum = { };
    m_found.firstSum = { };
    m_found.store = { };
    m_found.nested = nullptr;
    m_nestedBlocks.shrink(0);
}

bool RowLoopFinder::addTest(BlockID block, ValueID test, bool isOwnTest, bool expected)
{
    if (!isOwnTest && !isInLoop(test) && !isAvailableBefore(test))
        return false;
    const Operation& operation = at(test);
    static constexpr OpcodeSet testsOfClass { Opcode::HasStructureOfClass, Opcode::ShapeTest };
    Vector<Step, 48> earlierSteps;
    earlierSteps.appendRange(m_found.stepsBefore.begin(), m_found.stepsBefore.end());
    earlierSteps.appendRange(m_found.stepsAfter.begin(), m_found.stepsAfter.end());
    for (const Step& step : earlierSteps) {
        if (step.kind != Step::Kind::OwnTest && step.kind != Step::Kind::Test)
            continue;
        const Operation& earlier = at(step.value);
        if (isCongruent(test, step.value))
            return step.expected == expected;
        bool bothHold = expected && step.expected;
        if (bothHold && testsOfClass.contains(operation.opcode) && testsOfClass.contains(earlier.opcode) && representative(operation.operand(0)) == representative(earlier.operand(0)))
            return false;
        if (bothHold && operation.opcode == Opcode::HasArrayStorage && earlier.opcode == Opcode::HasArrayStorage && operation.arrayStorage() != earlier.arrayStorage() && representative(operation.arrayOperand()) == representative(earlier.arrayOperand()) && !operation.admitsInt32Storage() && !earlier.admitsInt32Storage())
            return false;
    }
    Step step { isOwnTest ? Step::Kind::OwnTest : Step::Kind::Test, block, test, { }, expected, 0 };
    if (isBehindNested())
        m_found.stepsAtEnd.append(step);
    else
        steps().append(step);
    return true;
}

void RowLoopFinder::dropImpliedTests()
{
    auto isImplied = [&](const Step& step) {
        if (step.kind != Step::Kind::OwnTest || step.expected || opcodeOf(step.value) != Opcode::HasArrayStorage)
            return false;
        return std::ranges::any_of(m_found.stepsBefore, [&](const Step& other) {
            return other.kind == Step::Kind::OwnTest && other.expected && opcodeOf(other.value) == Opcode::HasArrayStorage && at(other.value).arrayStorage() != at(step.value).arrayStorage() && !at(other.value).admitsInt32Storage() && representative(at(other.value).arrayOperand()) == representative(at(step.value).arrayOperand());
        });
    };
    Vector<Step, 24> kept;
    for (const Step& step : m_found.stepsBefore) {
        if (!isImplied(step))
            kept.append(step);
    }
    m_found.stepsBefore = WTF::move(kept);
}

void RowLoopFinder::findBound()
{
    for (size_t place = 0; place < m_found.stepsBefore.size(); ++place) {
        const Step& step = m_found.stepsBefore[place];
        if ((step.kind != Step::Kind::OwnTest && step.kind != Step::Kind::Test) || !step.expected)
            continue;
        ValueID test = representative(step.value);
        if (isConstant(test) || opcodeOf(test) != Opcode::Less || at(test).form() != OperatorForm::Integers || passed(at(test).leftOperand()) != m_found.counter)
            continue;
        ValueID bound = at(test).rightOperand();
        if (at(bound).representation != at(m_found.counter).representation || dependsOnCounter(bound))
            continue;
        m_found.bound = bound;
        m_found.stepsBefore.removeAt(place);
        return;
    }
}

void RowLoopFinder::dropUnusedSteps()
{
    Vector<ValueID, 64> used;
    auto use = [&](ValueID value) {
        if (value && !isConstant(value) && isInLoop(value) && !used.contains(value))
            used.append(value);
    };
    auto useOperands = [&](ValueID value) {
        for (ValueID operand : at(value).operands())
            use(operand);
    };
    use(m_found.bound);
    use(m_found.innerBound);
    use(m_found.firstSum);
    if (m_found.innerGuard)
        use(m_generator.m_rows->guards[*m_found.innerGuard].array);
    for (const Body& body : m_found.bodies) {
        for (BlockID block : body.blocks) {
            for (ValueID value : m_code.valuesOf(block))
                useOperands(value);
            for (ValueID operand : m_code.at(block).terminator.operands())
                use(operand);
            m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
                for (ValueID argument : edge.arguments())
                    use(argument);
            });
        }
    }
    auto drop = [&](Vector<Step, 24>& steps) {
        Vector<Step, 24> kept;
        for (size_t place = steps.size(); place--;) {
            const Step& step = steps[place];
            bool isNeeded = (step.kind != Step::Kind::Value && step.kind != Step::Kind::Name) || step.value == m_found.store || used.contains(step.value);
            if (!isNeeded)
                continue;
            if (step.kind == Step::Kind::Name)
                use(step.source);
            else if (step.kind == Step::Kind::Test)
                use(step.value);
            else if (step.kind != Step::Kind::Row)
                useOperands(step.value);
            kept.append(step);
        }
        kept.reverse();
        steps = WTF::move(kept);
    };
    drop(m_found.stepsAfter);
    drop(m_found.stepsBefore);
}

static bool onlyReads(const Operation& operation)
{
    static constexpr OpcodeSet reads {
        Opcode::HasStructureOfClass, Opcode::ShapeTest, Opcode::HasLayoutPrototype, Opcode::AreMembersUnchanged, Opcode::IsSlotFilled, Opcode::IsPristine, Opcode::IsElementPresent, Opcode::HasArrayStorage, Opcode::TypeTest,
        Opcode::GetSlot, Opcode::GetPropertyOfHeir, Opcode::GetCapture, Opcode::GetArrayStorage, Opcode::GetArrayLength, Opcode::GetArrayElement, Opcode::Narrowed, Opcode::Unbox, Opcode::Box,
    };
    return operation.traits().isPure() || (reads.contains(operation.opcode) && operation.traits().isUnobservable() && !operation.traits().mayCollect());
}

bool RowLoopFinder::addOperation(BlockID block, ValueID value)
{
    const Operation& operation = at(value);
    if (operation.opcode == Opcode::LoopHint) {
        if (block != m_found.head || operation.throwsOwedError() || (m_found.poll && m_found.poll != value))
            return false;
        m_found.poll = value;
        return true;
    }
    if (operation.opcode == Opcode::PutDoubleIndex) {
        if (!m_found.innerHead || hasStored() || !operation.isInBounds())
            return false;
        m_found.store = value;
        m_found.stepsAfter.append({ Step::Kind::Value, block, value, { }, true, 0 });
        return true;
    }
    if (operation.opcode == Opcode::ConvertToDoubleStorage && !m_found.innerHead) {
        m_converted.append(operation.arrayOperand());
        return true;
    }
    if (!onlyReads(operation) || isNeverType(operation.type))
        return false;
    if (m_found.store)
        return true;
    if (!m_found.innerHead && operation.opcode != Opcode::Parameter && !operation.operands().empty()) {
        for (const Step& step : m_found.stepsBefore) {
            if (step.kind == Step::Kind::Value && isCongruent(value, step.value)) {
                m_aliases.append({ value, step.value });
                m_found.stepsBefore.append({ Step::Kind::Name, block, value, step.value, true, 0 });
                return true;
            }
        }
    }
    Step step { Step::Kind::Value, block, value, { }, true, 0 };
    if (isBehindNested())
        m_found.stepsAtEnd.append(step);
    else
        steps().append(step);
    return true;
}

bool RowLoopFinder::addInnerOperation(Body& body, BlockID block, ValueID value)
{
    static constexpr OpcodeSet looksAtRows { Opcode::GetArrayStorage, Opcode::GetArrayLength, Opcode::HasArrayStorage, Opcode::Narrowed, Opcode::Unbox, Opcode::WidenInteger, Opcode::NarrowInteger };
    const Operation& operation = at(value);
    if (operation.opcode == Opcode::PutDoubleIndex) {
        if (m_found.sum || !operation.isInBounds() || passed(operation.positionOperand()) != m_found.innerCounter || !isInvariant(operation.arrayOperand()) || at(operation.arrayOperand()).representation != Representation::Cell || body.stores.size() == 2)
            return false;
        body.stores.append({ value, block, operation.arrayOperand(), operation.valueOperand() });
        return true;
    }
    if (operation.opcode != Opcode::GetArrayElement || (!m_found.sum && isInvariant(value))) {
        if (m_found.sum)
            return looksAtRows.contains(operation.opcode) || operation.traits().isPure();
        if (!onlyReads(operation) || isNeverType(operation.type))
            return false;
        if (body.stores.isEmpty())
            (isInvariant(value) ? body.decisions : body.operations).append({ Step::Kind::Value, block, value, { }, true, 0 });
        return true;
    }
    bool isNumber = operation.representation == Representation::Double || (operation.representation == Representation::OptionalDouble && !m_found.sum);
    if (operation.level() != KnowledgeLevel::Proven || operation.arrayStorage() != ArrayStorage::Double || !isNumber || body.reads.size() == mostReads || !body.stores.isEmpty())
        return false;
    ElementRead read { value, block, operation.arrayOperand(), false };
    ValueID position = passed(operation.positionOperand());
    bool isOfRow = m_found.innerGuard && std::ranges::contains(m_generator.m_rows->guards[*m_found.innerGuard].names, read.list);
    bool isAtColumn = m_found.form == RowLoop::Form::Gather ? isColumnIndex(operation.positionOperand()) : position == m_found.counter;
    if (isOfRow && isAtColumn)
        read.isColumn = true;
    else if (position != m_found.innerCounter || !isInvariant(read.list) || at(read.list).representation != Representation::Cell)
        return false;
    body.reads.append(read);
    return true;
}

bool RowLoopFinder::isColumnIndex(ValueID position) const
{
    Representation representation = at(position).representation;
    return (isAvailableBefore(position) || (m_isNest && passed(position) == m_found.counter)) && (representation == Representation::Int32 || representation == Representation::Int64) && m_generator.typeOf(position).integers.minimum >= 0;
}

bool RowLoopFinder::isInvariant(ValueID value, unsigned depth) const
{
    if (!value || !isOfInnerLoop(value))
        return true;
    const Operation& operation = at(value);
    bool mayChange = (operation.opcode == Opcode::GetArrayElement || operation.opcode == Opcode::IsElementPresent) && operation.arrayStorage() != ArrayStorage::Contiguous;
    if (depth > mostDepth || operation.opcode == Opcode::Parameter || mayChange || !onlyReads(operation))
        return false;
    return std::ranges::all_of(operation.operands(), [&](ValueID operand) { return isInvariant(operand, depth + 1); });
}

bool RowLoopFinder::isImpliedInnerTest(const Body& body, ValueID test) const
{
    if (m_found.sum || isConstant(test))
        return false;
    const Operation& operation = at(test);
    switch (operation.opcode) {
    case Opcode::Less: {
        ValueID length = passed(operation.rightOperand());
        return operation.form() == OperatorForm::Integers && passed(operation.leftOperand()) == m_found.innerCounter && !isConstant(length) && opcodeOf(length) == Opcode::GetArrayLength;
    }
    case Opcode::StrictEq:
        return operation.form() == OperatorForm::Numbers && operation.leftOperand() == operation.rightOperand() && !isInvariant(operation.leftOperand());
    case Opcode::IsElementPresent:
        return (passed(operation.positionOperand()) == m_found.innerCounter || (m_found.form == RowLoop::Form::Gather && isColumnIndex(operation.positionOperand()))) && operation.arrayStorage() == ArrayStorage::Double;
    case Opcode::TypeTest:
        return std::ranges::contains(body.reads, operation.valueOperand(), &ElementRead::access) && (at(operation.valueOperand()).representation == Representation::OptionalDouble || at(operation.valueOperand()).representation == Representation::Double) && m_generator.inputs().types.isSubset(TypeTable::number, operation.testedType());
    default:
        return false;
    }
}

bool RowLoopFinder::hasImpliedTests(Body& body) const
{
    auto isOfLists = [&](ValueID list) {
        return std::ranges::any_of(body.reads, [&](const ElementRead& read) { return representative(read.list) == representative(list); })
            || std::ranges::any_of(body.stores, [&](const ElementStore& store) { return representative(store.list) == representative(list); });
    };
    return std::ranges::all_of(body.impliedTests, [&](ValueID condition) {
        const Operation& operation = at(condition);
        if (operation.opcode == Opcode::Less)
            return isOfLists(at(passed(operation.rightOperand())).arrayOperand());
        if (operation.opcode == Opcode::StrictEq)
            return std::ranges::any_of(body.stores, [&](const ElementStore& store) { return passed(store.number) == passed(operation.leftOperand()); });
        size_t read = operation.opcode == Opcode::TypeTest ? body.reads.findIf([&](const ElementRead& found) { return found.access == operation.valueOperand(); })
            : body.reads.findIf([&](const ElementRead& found) { return representative(found.list) == representative(operation.arrayOperand()); });
        if (read != notFound)
            body.checkedReads.append(body.reads[read].access);
        return read != notFound;
    });
}

bool RowLoopFinder::exploreBody(const Edge& edge, const Body& before)
{
    BlockID head = m_found.innerHead;
    size_t placeOfBody = m_found.bodies.size() - 1;
    if (edge.to == head) {
        Body body = before;
        if (edge.arguments().size() != 1 || !stepsByOne(edge.arguments()[0], m_found.innerCounter) || body.stores.isEmpty() || !hasImpliedTests(body))
            return false;
        body.isVector = std::ranges::all_of(body.stores, [&](const ElementStore& store) { return isTerm(body, store.number); })
            && std::ranges::all_of(body.checkedReads, [&](ValueID read) { return std::ranges::any_of(body.stores, [&](const ElementStore& store) { return isPartOf(read, store.number); }); });
        if (!body.isVector && body.stores.size() > 1)
            return false;
        m_found.bodies.insert(placeOfBody, WTF::move(body));
        return true;
    }
    BlockID id = edge.to;
    const Block& block = m_code.at(id);
    if (m_found.bodies.size() > mostBodies || ++m_visits > mostVisits || !isPlainBlock(id) || block.isLoopHead || m_path.contains(id) || before.blocks.contains(id) || edge.arguments().size() != block.parameters().size())
        return false;
    Body body = before;
    body.blocks.append(id);
    m_found.bodies.last() = body;
    for (size_t index = 0; index < block.parameters().size(); ++index) {
        ValueID parameter = block.parameters()[index];
        ValueID argument = edge.arguments()[index];
        if (m_generator.isCarriedBoxed(parameter) || at(parameter).representation != at(argument).representation || isNeverType(at(argument).type))
            return false;
        body.operations.append({ Step::Kind::Name, id, parameter, argument, true, 0 });
    }
    ValueID test = block.terminator.kind == TerminatorKind::Branch ? block.terminator.conditionOperand() : ValueID();
    unsigned placeOfGuard = 0;
    const RowGuard* guard = m_found.form == RowLoop::Form::Gather ? findGuard(id, placeOfGuard) : nullptr;
    bool isOwnTest = test && (guard || m_generator.testReadByBranchAlone(id, block.terminator) == test);
    for (ValueID value : m_code.valuesOf(block)) {
        if (!(value == test && isOwnTest) && !addInnerOperation(body, id, value))
            return false;
    }
    if (block.terminator.kind == TerminatorKind::Jump)
        return exploreBody(m_code.at(block.terminator.next()), body);
    if (guard) {
        if ((m_found.innerGuard && *m_found.innerGuard != placeOfGuard) || passed(guard->position) != m_found.innerCounter || (!isAvailableBefore(guard->array) && !(m_isNest && isInLoop(guard->array))))
            return false;
        m_found.innerGuard = placeOfGuard;
        body.blocks.appendRange(guard->checkBlocks.begin(), guard->checkBlocks.end());
        m_found.bodies.last() = body;
        return exploreBody(m_code.at(m_code.at(guard->checkBlocks.last()).terminator.taken()), body);
    }
    if (!test || isConstant(test) || !body.stores.isEmpty())
        return false;
    if (isImpliedInnerTest(body, test)) {
        body.impliedTests.append(test);
        return exploreBody(m_code.at(block.terminator.taken()), body);
    }
    if (!isInvariant(test))
        return false;
    bool isFound = false;
    for (bool expected : { true, false }) {
        Body side = body;
        side.decisions.append({ isOwnTest ? Step::Kind::OwnTest : Step::Kind::Test, id, test, { }, expected, 0 });
        isFound |= exploreBody(m_code.at(expected ? block.terminator.taken() : block.terminator.notTaken()), side);
    }
    return isFound;
}

bool RowLoopFinder::followInnerLoop(const Edge& entry)
{
    BlockID head = entry.to;
    const Block& block = m_code.at(head);
    if (!isPlainBlock(head) || block.terminator.kind != TerminatorKind::Branch || entry.arguments().size() != block.parameters().size() || block.parameters().empty() || block.parameters().size() > 2)
        return false;
    ValueID test = block.terminator.conditionOperand();
    if (isConstant(test) || m_code.blockOf(test) != head)
        return false;
    for (ValueID value : m_code.valuesOf(block)) {
        if (value != test && (opcodeOf(value) != Opcode::LoopHint || at(value).throwsOwedError()))
            return false;
    }
    const Operation& comparison = at(test);
    if (comparison.opcode != Opcode::Less || comparison.form() != OperatorForm::Integers || !m_code.at(block.terminator.taken()).arguments().empty())
        return false;
    m_found.innerHead = head;
    m_found.bodies.append({ });
    m_found.bodies.last().blocks.append(head);
    m_found.innerCounter = comparison.leftOperand();
    m_found.innerBound = comparison.rightOperand();
    if (!std::ranges::contains(block.parameters(), m_found.innerCounter) || at(m_found.innerCounter).representation != Representation::Int32 || at(m_found.innerBound).representation != Representation::Int32)
        return false;
    size_t placeOfCounter = block.parameters()[0] == m_found.innerCounter ? 0 : 1;
    ValueID first = entry.arguments()[placeOfCounter];
    if (!isConstant(first) || opcodeOf(first) != Opcode::Integer || at(first).integer() || m_generator.isCarriedBoxed(m_found.innerCounter))
        return false;
    if (block.parameters().size() == 1) {
        if (m_isNest)
            m_found.form = RowLoop::Form::Gather;
        Body body = m_found.bodies.last();
        bool isFound = exploreBody(m_code.at(block.terminator.taken()), body);
        m_found.bodies.removeLast();
        if (m_isNest) {
            m_found.form = RowLoop::Form::Nest;
            return isFound;
        }
        m_hasMapped = isFound;
        auto isDecidedByRows = [&](const Body& found) {
            return std::ranges::any_of(found.decisions, [&](const Step& step) { return dependsOnCounter(step.value); });
        };
        if (isFound && std::ranges::any_of(m_found.bodies, isDecidedByRows)) {
            m_found.bodies.shrink(1);
            m_found.stepsBefore.appendRange(m_found.bodies[0].decisions.begin(), m_found.bodies[0].decisions.end());
            m_found.bodies[0].decisions.shrink(0);
        }
        return isFound;
    }

    m_found.sum = block.parameters()[!placeOfCounter];
    m_found.firstSum = entry.arguments()[!placeOfCounter];
    if (at(m_found.sum).representation != Representation::Double || at(m_found.firstSum).representation != Representation::Double || m_generator.isCarriedBoxed(m_found.sum) || !m_generator.m_values[m_found.sum.index()] || !isAvailableBefore(m_found.firstSum))
        return false;
    Body& body = m_found.bodies.last();
    for (const Edge* edge = &m_code.at(block.terminator.taken()); edge->to != head;) {
        BlockID id = edge->to;
        const Block& next = m_code.at(id);
        if (body.blocks.size() >= mostBlocks || !isPlainBlock(id) || next.isLoopHead || !next.parameters().empty() || !edge->arguments().empty() || m_path.contains(id) || body.blocks.contains(id))
            return false;
        body.blocks.append(id);
        unsigned place = 0;
        const RowGuard* guard = findGuard(id, place);
        ValueID condition = next.terminator.kind == TerminatorKind::Branch ? next.terminator.conditionOperand() : ValueID();
        for (ValueID value : m_code.valuesOf(next)) {
            if (value != condition && !addInnerOperation(body, id, value))
                return false;
        }
        if (guard) {
            if (m_found.innerGuard || passed(guard->position) != m_found.innerCounter)
                return false;
            m_found.innerGuard = place;
            body.blocks.appendRange(guard->checkBlocks.begin(), guard->checkBlocks.end());
            edge = &m_code.at(m_code.at(guard->checkBlocks.last()).terminator.taken());
            continue;
        }
        if (next.terminator.kind != TerminatorKind::Jump)
            return false;
        edge = &m_code.at(next.terminator.next());
        if (edge->to != head)
            continue;
        if (edge->arguments().size() != block.parameters().size() || !stepsByOne(edge->arguments()[placeOfCounter], m_found.innerCounter))
            return false;
        m_found.nextSum = edge->arguments()[!placeOfCounter];
    }
    return m_found.nextSum && isOfInnerLoop(m_found.nextSum) && isTerm(body, m_found.nextSum);
}

bool RowLoopFinder::isPartOf(ValueID access, ValueID term) const
{
    if (access == term)
        return true;
    if (!isOfInnerLoop(term) || opcodeOf(term) == Opcode::Parameter || opcodeOf(term) == Opcode::GetArrayElement)
        return false;
    if (opcodeOf(term) == Opcode::Narrowed)
        return isPartOf(access, at(term).valueOperand());
    return isPartOf(access, at(term).leftOperand()) || isPartOf(access, at(term).rightOperand());
}

bool RowLoopFinder::isTerm(const Body& body, ValueID value, unsigned depth) const
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    const Operation& operation = at(value);
    if (std::ranges::contains(body.reads, value, &ElementRead::access))
        return true;
    if (depth > mostDepth || operation.representation != Representation::Double)
        return false;
    if (value == m_found.sum)
        return true;
    if (!isOfInnerLoop(value))
        return isAvailableBefore(value) || (m_found.form == RowLoop::Form::Map && std::ranges::contains(m_found.stepsBefore, value, &Step::value));
    if (operation.opcode == Opcode::Narrowed)
        return isTerm(body, operation.valueOperand(), depth + 1);
    return arithmetic.contains(operation.opcode) && operation.form() == OperatorForm::Numbers && isTerm(body, operation.leftOperand(), depth + 1) && isTerm(body, operation.rightOperand(), depth + 1);
}

bool RowLoopFinder::follow(const Edge& edge)
{
    BlockID id = edge.to;
    if (id == m_found.head)
        return finish(edge);
    const Block& block = m_code.at(id);
    if (block.isLoopHead) {
        if (m_isNest && m_found.innerHead)
            return followNested(edge);
        if (m_found.innerHead || std::ranges::contains(m_innerHeadsTaken, id))
            return false;
        Place place = this->place();
        if (followInnerLoop(edge) && follow(m_code.at(block.terminator.notTaken())))
            return true;
        restorePlace(place, false);
        return false;
    }
    if (!isPlainBlock(id) || m_path.contains(id) || edge.arguments().size() != block.parameters().size())
        return false;
    Place place = this->place();
    bool hadInnerLoop = !!m_found.innerHead;
    for (size_t index = 0; index < block.parameters().size(); ++index) {
        ValueID parameter = block.parameters()[index];
        ValueID argument = edge.arguments()[index];
        if (m_found.store)
            break;
        if (isBehindNested() || m_generator.isCarriedBoxed(parameter) || at(parameter).representation != at(argument).representation || isNeverType(at(argument).type) || (!isConstant(argument) && !isInLoop(argument) && !m_generator.m_values[argument.index()])) {
            restorePlace(place, hadInnerLoop);
            return false;
        }
        m_aliases.append({ parameter, argument });
        steps().append({ Step::Kind::Name, id, parameter, argument, true, 0 });
    }
    if (visit(id))
        return true;
    restorePlace(place, hadInnerLoop);
    return false;
}

bool RowLoopFinder::followNested(const Edge& edge)
{
    BlockID head = edge.to;
    const Terminator& terminator = m_code.at(head).terminator;
    if (m_found.nested || edge.arguments().size() != 1 || !isConstant(edge.arguments()[0]) || opcodeOf(edge.arguments()[0]) != Opcode::Integer || at(edge.arguments()[0]).integer() || std::ranges::contains(m_headsTaken, std::pair { m_found.innerHead, head }))
        return false;
    RowLoopFinder finder(m_generator, head, UsesCounts::Yes, { }, this);
    std::optional<RowLoop> nested = finder.find();
    if (!nested || (nested->form != RowLoop::Form::Sum && nested->form != RowLoop::Form::ColumnSum) || nested->storesAtCounter || !nested->bound || at(nested->counter).representation != Representation::Int32 || terminator.kind != TerminatorKind::Branch || isConstant(terminator.conditionOperand()) || m_code.blockOf(terminator.conditionOperand()) != head || opcodeOf(terminator.conditionOperand()) != Opcode::Less || at(terminator.conditionOperand()).rightOperand() != nested->bound)
        return false;
    m_found.nested = std::make_unique<RowLoop>(WTF::move(*nested));
    m_nestedBlocks.shrink(0);
    m_nestedBlocks.appendRange(finder.m_path.begin(), finder.m_path.end());
    for (const Body& body : m_found.nested->bodies)
        m_nestedBlocks.appendRange(body.blocks.begin(), body.blocks.end());
    Place place = this->place();
    if (follow(m_code.at(terminator.notTaken())))
        return true;
    restorePlace(place, true);
    m_found.nested = nullptr;
    m_nestedBlocks.shrink(0);
    return false;
}

bool RowLoopFinder::visit(BlockID id)
{
    if (m_path.size() == mostBlocks || ++m_visits > mostVisits)
        return false;
    m_path.append(id);
    const Block& block = m_code.at(id);
    unsigned placeOfGuard = 0;
    const RowGuard* guard = m_usesCounts ? findGuard(id, placeOfGuard) : nullptr;
    if (guard && (passed(guard->position) != m_found.counter || !isAvailableBefore(guard->array)))
        guard = nullptr;
    ValueID test = block.terminator.kind == TerminatorKind::Branch ? block.terminator.conditionOperand() : ValueID();
    bool isOwnTest = test && m_generator.testReadByBranchAlone(id, block.terminator) == test;
    for (ValueID value : m_code.valuesOf(block)) {
        if (!(value == test && (guard || isOwnTest)) && !addOperation(id, value))
            return false;
    }
    if (block.terminator.kind == TerminatorKind::Jump)
        return follow(m_code.at(block.terminator.next()));
    if (!test || m_found.store || ((m_hasMapped || isBehindNested()) && (guard || isConstant(test))))
        return false;
    if (guard) {
        steps().append({ Step::Kind::Row, id, guard->row, { }, true, placeOfGuard });
        m_namesOfRows.appendRange(guard->names.begin(), guard->names.end());
        m_path.appendRange(guard->checkBlocks.begin(), guard->checkBlocks.end());
        return follow(m_code.at(m_code.at(guard->checkBlocks.last()).terminator.taken()));
    }
    if (isConstant(test))
        return false;
    bool hadInnerLoop = !!m_found.innerHead;
    bool prefersNotTaken = opcodeOf(test) == Opcode::HasArrayStorage && at(test).arrayStorage() == ArrayStorage::Int32;
    for (bool expected : { !prefersNotTaken, prefersNotTaken }) {
        Place place = this->place();
        if (addTest(id, test, isOwnTest, expected) && follow(m_code.at(expected ? block.terminator.taken() : block.terminator.notTaken())))
            return true;
        restorePlace(place, hadInnerLoop);
    }
    return false;
}

bool RowLoopFinder::dependsOnCounter(ValueID value, unsigned depth) const
{
    if (!value || isConstant(value))
        return false;
    if (value == m_found.counter || value == m_found.sum || m_namesOfRows.contains(value) || depth > mostBlocks)
        return true;
    if (!isInLoop(value) || value == m_found.innerCounter || (m_found.nested && (value == m_found.nested->counter || value == m_found.nested->innerCounter)))
        return false;
    if (opcodeOf(value) == Opcode::Parameter) {
        for (std::span<const Step> steps : { m_found.stepsBefore.span(), m_found.stepsAfter.span() }) {
            for (const Step& named : steps) {
                if (named.kind == Step::Kind::Name && named.value == value)
                    return dependsOnCounter(named.source, depth + 1);
            }
        }
        return true;
    }
    return std::ranges::any_of(at(value).operands(), [&](ValueID operand) { return dependsOnCounter(operand, depth + 1); });
}

bool RowLoopFinder::dependsOnSum(ValueID value, unsigned depth) const
{
    if (!value || isConstant(value) || !isInLoop(value))
        return false;
    if (value == m_found.sum || depth > mostBlocks)
        return true;
    if (opcodeOf(value) == Opcode::Parameter) {
        size_t step = m_found.stepsAfter.findIf([&](const Step& named) { return named.kind == Step::Kind::Name && named.value == value; });
        return step != notFound && dependsOnSum(m_found.stepsAfter[step].source, depth + 1);
    }
    return std::ranges::any_of(at(value).operands(), [&](ValueID operand) { return dependsOnSum(operand, depth + 1); });
}

bool RowLoopFinder::readsStoredColumn()
{
    const Operation& store = at(m_found.store);
    ValueID position = store.positionOperand();
    Representation representation = at(position).representation;
    if (passed(position) == m_found.counter) {
        m_found.storesAtCounter = !dependsOnCounter(store.arrayOperand()) && at(store.arrayOperand()).representation == Representation::Cell;
        return !m_found.storesAtCounter;
    }
    if (!isAvailableBefore(position) || (representation != Representation::Int32 && representation != Representation::Int64))
        return true;
    return std::ranges::any_of(m_found.bodies[0].reads, [&](const ElementRead& read) { return !read.isColumn && !m_generator.isArrayWithoutRows(read.list); });
}

bool RowLoopFinder::findOrderOfSteps()
{
    static constexpr OpcodeSet elementReads { Opcode::GetArrayElement, Opcode::IsElementPresent };
    static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
    for (ValueID array : m_converted) {
        bool isChecked = std::ranges::any_of(m_found.stepsBefore, [&](const Step& step) {
            return step.kind == Step::Kind::OwnTest && step.expected && opcodeOf(step.value) == Opcode::HasArrayStorage && at(step.value).arrayStorage() == ArrayStorage::Double && !at(step.value).admitsInt32Storage() && representative(at(step.value).arrayOperand()) == representative(array);
        });
        if (!isChecked)
            return false;
    }
    dropImpliedTests();
    findBound();
    dropUnusedSteps();
    Vector<Step, 24> independent;
    Vector<Step, 24> dependent;
    bool hasRow = false;
    for (const Step& step : m_found.stepsBefore) {
        bool isDependent = step.kind == Step::Kind::Row || dependsOnCounter(step.kind == Step::Kind::Name ? step.source : step.value);
        bool readsElement = step.kind != Step::Kind::Row && step.kind != Step::Kind::Name && !isConstant(step.value) && isInLoop(step.value) && elementReads.contains(opcodeOf(step.value));
        hasRow |= step.kind == Step::Kind::Row || (readsElement && opcodeOf(step.value) == Opcode::IsElementPresent && step.expected && passed(at(step.value).positionOperand()) == m_found.counter);
        bool mayChange = readsElement && !(at(step.value).arrayStorage() == ArrayStorage::Contiguous);
        if (mayChange)
            return false;
        (isDependent ? dependent : independent).append(step);
    }
    hasRow |= std::ranges::any_of(m_found.stepsAfter, [&](const Step& step) {
        return step.kind == Step::Kind::Row || (step.kind == Step::Kind::OwnTest && step.expected && opcodeOf(step.value) == Opcode::IsElementPresent && passed(at(step.value).positionOperand()) == m_found.counter);
    });
    bool hasGuards = std::ranges::contains(m_found.stepsBefore, Step::Kind::Row, &Step::kind) || std::ranges::contains(m_found.stepsAfter, Step::Kind::Row, &Step::kind);
    if (hasGuards && !m_found.bound)
        return false;
    if (m_found.bound && !isAvailableBefore(m_found.bound) && !std::ranges::contains(independent, m_found.bound, &Step::value))
        return false;
    if ((!hasRow && !m_found.bound) || dependsOnCounter(m_found.innerBound) || (!isAvailableBefore(m_found.innerBound) && !std::ranges::contains(independent, m_found.innerBound, &Step::value)))
        return false;
    bool readsColumns = m_found.sum && std::ranges::contains(m_found.bodies[0].reads, true, &ElementRead::isColumn);
    if (readsColumns != !!m_found.innerGuard)
        return false;
    if (readsColumns) {
        const RowGuard& guard = m_generator.m_rows->guards[*m_found.innerGuard];
        bool onlyNames = std::ranges::all_of(dependent, [&](const Step& step) { return step.kind == Step::Kind::Value && names.contains(opcodeOf(step.value)); });
        bool readsRows = std::ranges::any_of(m_found.bodies[0].reads, [&](const ElementRead& read) { return !read.isColumn && dependsOnCounter(read.list); });
        bool isLimited = m_found.bound || std::ranges::contains(m_found.stepsAfter, Step::Kind::Row, &Step::kind);
        if (!onlyNames || readsRows || !isLimited || readsStoredColumn() || dependsOnCounter(guard.array) || (!isAvailableBefore(guard.array) && !std::ranges::contains(independent, guard.array, &Step::value)))
            return false;
        m_found.form = RowLoop::Form::ColumnSum;
    } else
        m_found.form = m_found.sum ? RowLoop::Form::Sum : RowLoop::Form::Map;
    auto isNamed = [&](ValueID list) {
        return isAvailableBefore(list) || m_namesOfRows.contains(list) || std::ranges::contains(m_found.stepsBefore, list, &Step::value);
    };
    for (const Body& body : m_found.bodies) {
        auto isDecided = [&](ValueID list) { return isNamed(list) || std::ranges::contains(body.decisions, list, &Step::value); };
        if (!std::ranges::all_of(body.reads, [&](const ElementRead& read) { return read.isColumn || isDecided(read.list); }) || !std::ranges::all_of(body.stores, [&](const ElementStore& store) { return isDecided(store.list); }))
            return false;
    }
    if (m_found.sum) {
        Vector<Step, 24> afterSum;
        for (const Step& step : m_found.stepsAfter) {
            bool readsSum = step.value == m_found.store || (step.kind != Step::Kind::Row && dependsOnSum(step.kind == Step::Kind::Name ? step.source : step.value));
            (readsSum ? afterSum : m_found.stepsBesideSum).append(step);
        }
        m_found.stepsAfter = WTF::move(afterSum);
    }
    if (m_found.form == RowLoop::Form::Map) {
        for (const Step& step : m_found.stepsAfter) {
            if (dependsOnCounter(step.kind == Step::Kind::Name ? step.source : step.value))
                return false;
        }
    }
    m_found.stepsInFront.appendRange(independent.begin(), independent.end());
    m_found.stepsBefore.shrink(0);
    m_found.stepsBefore.appendRange(dependent.begin(), dependent.end());
    return true;
}

bool RowLoopFinder::finish(const Edge& back)
{
    return m_found.innerHead && (m_isNest ? !!m_found.nested : hasStored()) && back.arguments().size() == 1 && stepsByOne(back.arguments()[0], m_found.counter);
}

bool RowLoopFinder::findOrderOfNest()
{
    RowLoop& nested = *m_found.nested;
    findBound();
    auto isKnown = [&](ValueID value) { return !dependsOnCounter(value); };
    m_found.bodies.shrink(1);
    Body& gather = m_found.bodies[0];
    if (!m_found.bound || !isKnown(m_found.bound) || !isKnown(m_found.innerBound) || !gather.isVector || gather.stores.size() != 1 || !m_generator.isArrayWithoutRows(gather.stores[0].list) || !isKnown(gather.stores[0].list))
        return false;
    m_found.stepsBefore.appendRange(gather.decisions.begin(), gather.decisions.end());
    gather.decisions.shrink(0);
    for (const ElementRead& read : gather.reads) {
        if (!read.isColumn && isKnown(read.list) && !m_generator.isArrayWithoutRows(read.list))
            return false;
    }
    if (m_found.innerGuard && !isKnown(m_generator.m_rows->guards[*m_found.innerGuard].array))
        return false;
    for (const ElementRead& read : nested.bodies[0].reads) {
        bool isOfRow = read.isColumn || std::ranges::any_of(nested.stepsBefore, [&](const Step& step) { return step.kind == Step::Kind::Row && m_generator.m_rows->guards[step.guard].names.contains(read.list); });
        if (!isOfRow && (!isKnown(read.list) || !m_generator.isArrayWithoutRows(read.list)))
            return false;
    }
    size_t storedRows = nested.stepsBesideSum.findIf([&](const Step& step) { return step.kind == Step::Kind::Row && m_generator.m_rows->guards[step.guard].names.contains(at(nested.store).arrayOperand()); });
    if (storedRows == notFound || !isKnown(nested.bound) || !isKnown(nested.innerBound) || !isKnown(nested.firstSum))
        return false;
    for (std::span<const Step> steps : { nested.stepsBefore.span(), nested.stepsBesideSum.span(), nested.stepsAfter.span() }) {
        for (const Step& step : steps) {
            if (step.kind == Step::Kind::Row && !isKnown(m_generator.m_rows->guards[step.guard].array))
                return false;
        }
    }
    if (nested.innerGuard && !isKnown(m_generator.m_rows->guards[*nested.innerGuard].array))
        return false;
    for (const Step& step : m_found.stepsAtEnd) {
        if (step.kind != Step::Kind::Value && !isKnown(step.value))
            return false;
    }
    m_found.stepsAtEnd.removeAllMatching([&](const Step& step) { return !isKnown(step.value); });

    auto split = [&](Vector<Step, 24>& steps, Vector<Step, 24>& known) {
        Vector<Step, 24> dependent;
        for (const Step& step : steps)
            (isKnown(step.kind == Step::Kind::Name ? step.source : step.value) ? known : dependent).append(step);
        steps = WTF::move(dependent);
    };
    split(m_found.stepsBefore, m_found.stepsInFront);
    split(m_found.stepsAfter, m_found.stepsBesideSum);
    split(nested.stepsInFront, m_found.stepsOfNest);
    return true;
}

std::optional<RowLoop> RowLoopFinder::findNest(std::span<const std::pair<BlockID, BlockID>> headsTaken)
{
    const Block& block = m_code.at(m_found.head);
    if (!isPlainBlock(m_found.head) || block.parameters().size() != 1)
        return std::nullopt;
    m_isNest = true;
    m_headsTaken = headsTaken;
    m_found.form = RowLoop::Form::Nest;
    m_found.counter = block.parameters()[0];
    if (at(m_found.counter).representation != Representation::Int32 || m_generator.isCarriedBoxed(m_found.counter) || !m_generator.m_values[m_found.counter.index()] || m_generator.typeOf(m_found.counter).integers.minimum < 0)
        return std::nullopt;
    if (!visit(m_found.head) || !m_found.nested || !findOrderOfNest())
        return std::nullopt;
    return WTF::move(m_found);
}

std::optional<RowLoop> RowLoopFinder::findGather()
{
    BlockID head = m_found.head;
    const Block& block = m_code.at(head);
    if (!isPlainBlock(head) || block.parameters().size() != 1 || block.terminator.kind != TerminatorKind::Branch || !m_code.at(block.terminator.taken()).arguments().empty())
        return std::nullopt;
    ValueID test = block.terminator.conditionOperand();
    if (isConstant(test) || m_code.blockOf(test) != head)
        return std::nullopt;
    for (ValueID value : m_code.valuesOf(block)) {
        if (value == test)
            continue;
        if (opcodeOf(value) != Opcode::LoopHint || at(value).throwsOwedError() || m_found.poll)
            return std::nullopt;
        m_found.poll = value;
    }
    const Operation& comparison = at(test);
    m_found.form = RowLoop::Form::Gather;
    m_found.innerHead = head;
    m_found.counter = block.parameters()[0];
    m_found.innerCounter = m_found.counter;
    if (comparison.opcode != Opcode::Less || comparison.form() != OperatorForm::Integers || comparison.leftOperand() != m_found.counter || at(m_found.counter).representation != Representation::Int32 || m_generator.isCarriedBoxed(m_found.counter) || !m_generator.m_values[m_found.counter.index()] || m_generator.typeOf(m_found.counter).integers.minimum < 0)
        return std::nullopt;
    m_found.innerBound = comparison.rightOperand();
    if (at(m_found.innerBound).representation != Representation::Int32 || !isAvailableBefore(m_found.innerBound))
        return std::nullopt;
    Body body;
    body.blocks.append(head);
    m_found.bodies.append(body);
    if (!exploreBody(m_code.at(block.terminator.taken()), body) || !m_found.innerGuard)
        return std::nullopt;
    m_found.bodies.removeLast();
    for (const Body& found : m_found.bodies) {
        bool readsColumn = std::ranges::contains(found.reads, true, &ElementRead::isColumn);
        bool areListsKnown = std::ranges::all_of(found.reads, [&](const ElementRead& read) { return read.isColumn || isAvailableBefore(read.list); }) && std::ranges::all_of(found.stores, [&](const ElementStore& store) { return isAvailableBefore(store.list); });
        if (!found.isVector || !readsColumn || !areListsKnown || found.stores.size() != 1)
            return std::nullopt;
    }
    return WTF::move(m_found);
}

std::optional<RowLoop> RowLoopFinder::find()
{
    const Block& block = m_code.at(m_found.head);
    if (!isPlainBlock(m_found.head) || block.parameters().size() != 1)
        return std::nullopt;
    m_found.counter = block.parameters()[0];
    m_found.form = RowLoop::Form::Map;
    Representation representation = at(m_found.counter).representation;
    if ((representation != Representation::Int32 && representation != Representation::Int64) || m_generator.isCarriedBoxed(m_found.counter) || !m_generator.m_values[m_found.counter.index()] || m_generator.typeOf(m_found.counter).integers.minimum < 0)
        return std::nullopt;
    if (!visit(m_found.head) || !findOrderOfSteps())
        return std::nullopt;
    return WTF::move(m_found);
}

class RowLoopEmitter {
public:
    using Body = RowLoop::Body;
    using Step = RowLoop::Step;
    using ElementRead = RowLoop::ElementRead;
    using ElementStore = RowLoop::ElementStore;
    using RowGuard = CodeGenerator::RowGuard;
    using ContextOfEnd = TerminatorContext (*)(const CodeGenerator&, BlockID);

    RowLoopEmitter(CodeGenerator& generator, const RowLoop& loop, ContextOfEnd contextOfEnd)
        : m_generator(generator)
        , m_code(generator.code())
        , m_out(generator.m_out)
        , m_elements(generator.emitter<ElementAccessEmitter>())
        , m_loop(loop)
        , m_contextOfEnd(contextOfEnd)
    {
    }

    struct Exit {
        LBasicBlock target { nullptr };
        LValue phi { nullptr };
        LValue counter { nullptr };
    };
    void emit(LValue first, const Exit& front, const Exit& written);

private:
    struct Base {
        LValue storage { nullptr };
        LValue cell { nullptr };
    };
    bool isNamedInRows(ValueID list) const;
    void emitBasesInFront();
    void emitHolders(std::span<const Step>);
    void emitColumnHolder();
    void emitSums();
    void emitBodies();
    void emitBody();
    void emitNest();
    void emitFrontOfNested(const RowLoop& nest, const Exit& front);
    std::pair<LValue, LValue> emitRange(LValue holder, LValue count);
    void leaveUnlessDisjoint(std::pair<LValue, LValue>, std::pair<LValue, LValue>);
    void emitGatherOfNest(std::span<const Base> stores, std::pair<LValue, LValue> storedRows, const Exit&);
    void emitGather();
    void emitRowsByLength(ArrayStorage kind, LValue counter, const Exit& otherKind);
    void emitInnerMapKept(std::span<const Base> reads, std::span<const Base> stores, const Exit&);
    LValue valueOf(ValueID, BlockID user);
    LValue counterIn64Bits(LValue counter) { return counter->type() == Machine::Int32 ? m_out.zeroExt(counter, Machine::Int64) : counter; }
    void name(ValueID, LValue);
    void continueIn(LBasicBlock holds, LBasicBlock fails, const Exit&);
    void leaveUnless(LValue condition, const Exit&);
    void leave(const Exit&);
    void emitExits();
    void emitSteps(std::span<const Step>, const Exit&);
    void emitPoll();
    LValue emitRowCell(const RowGuard&, LValue storage, LValue position);
    LValue emitCountedRows(const RowGuard&, LValue needed, LValue array);
    void emitCountedArray(const RowGuard&, LValue array);
    Base requireKindAndLength(const ElementRead&, LValue cell, ArrayStorage kind, LValue length, const Exit& otherKind, const Exit& tooShort);
    void checkAccess(BlockID, ValueID access, LValue cell, ArrayStorage kind, LValue index, unsigned lanes);
    LValue emitElement(const ElementRead&, ArrayStorage kind, const Base&, LValue index, ptrdiff_t offset, const Exit&, bool isVector = false);
    LValue emitTerm(ValueID, LValue sum, std::span<const LValue> elements, bool isVector);
    Vector<LValue, 6> emitScalarElements(ArrayStorage kind, std::span<const Base>, LValue index, unsigned term, LValue column, ptrdiff_t offsetOfColumn, const Exit&);
    LValue emitInnerSum(ArrayStorage kind, std::span<const Base>, bool isPair, const Exit&);
    Vector<LValue, 2> emitNumbers(std::span<const Base>, LValue index, unsigned offset, bool isVector, const Exit&);
    void emitStores(std::span<const LValue> numbers, std::span<const Base>, LValue index, unsigned offset, bool isVector);
    void emitInnerMap(std::span<const Base> reads, std::span<const Base> stores, bool isPair, const Exit&);
    void addSplats(ValueID);
    void emitRows(ArrayStorage kind, LBasicBlock entry, LValue counter, bool isPair, const Exit& otherKind);
    void emitColumns(ArrayStorage kind, LBasicBlock entry, LValue counter, const Exit& otherKind);
    void emitNextIteration(LBasicBlock entry, LValue counter, LValue next);
    Exit exitTo(const Exit& exit, LValue counter) const { return m_columnEnd && exit.target == m_written.target ? m_written : Exit { exit.target, exit.phi, counter }; }
    bool admitsInt32s() const;
    bool isBaseInFront(size_t place) const { return !!m_bases[place].cell; }
    bool isInnerBlock(BlockID block) const
    {
        return block == m_loop.innerHead || std::ranges::any_of(m_loop.bodies, [&](const Body& body) { return body.blocks.contains(block); });
    }

    static constexpr int32_t termsOfIteration = 4;
    static constexpr int32_t lanes = 2;
    CodeGenerator& m_generator;
    const Code& m_code;
    Output& m_out;
    ElementAccessEmitter& m_elements;
    const RowLoop& m_loop;
    const Body* m_body { nullptr };
    ContextOfEnd m_contextOfEnd;
    Vector<std::pair<ValueID, LValue>, 64> m_saved;
    Vector<std::pair<LBasicBlock, Exit>, 32> m_exits;
    Exit m_written { };
    Exit m_front { };
    LValue m_first { nullptr };
    LValue m_limit { nullptr };
    LValue m_length { nullptr };
    LValue m_lengthAsWord { nullptr };
    LValue m_firstSum { nullptr };
    Vector<Base, 6> m_bases;
    Vector<Base, 2> m_basesOfStores;
    Vector<std::pair<unsigned, LValue>, 4> m_storagesOfHolders;
    Vector<std::pair<ValueID, LValue>, 4> m_splats;
    LValue m_column { nullptr };
    Machine::StackSlot* m_sums { nullptr };
    Machine::StackSlot* m_keptNumbers { nullptr };
    bool m_isNested { false };
    LBasicBlock m_columnEnd { nullptr };
};

LValue RowLoopEmitter::valueOf(ValueID value, BlockID user)
{
    if (std::ranges::contains(m_saved, value, &std::pair<ValueID, LValue>::first))
        return m_generator.m_values[value.index()];
    TerminatorContext context = m_contextOfEnd(m_generator, user);
    SetForScope<const OperationContext*> whileReading(m_generator.m_operation, &context);
    LValue lowered = m_generator.get(value).value;
    m_out.clearDeclaredAccesses();
    return lowered;
}

void RowLoopEmitter::name(ValueID value, LValue lowered)
{
    LValue& place = m_generator.m_values[value.index()];
    if (!std::ranges::contains(m_saved, value, &std::pair<ValueID, LValue>::first))
        m_saved.append({ value, place });
    place = lowered;
}

void RowLoopEmitter::leave(const Exit& exit)
{
    if (exit.phi)
        m_out.upsilon(exit.counter, exit.phi);
    m_out.jump(normalEdge(exit.target));
}

void RowLoopEmitter::continueIn(LBasicBlock holds, LBasicBlock fails, const Exit& exit)
{
    m_exits.append({ fails, exit });
    ScopeFork fork = m_out.forkScope();
    m_out.appendToSuccessor(holds, fork);
}

void RowLoopEmitter::leaveUnless(LValue condition, const Exit& exit)
{
    LBasicBlock holds = m_out.newBlock();
    LBasicBlock fails = m_out.newBlock();
    m_out.branch(condition, normalEdge(holds), unlikelyEdge(fails));
    continueIn(holds, fails, exit);
}

void RowLoopEmitter::emitExits()
{
    for (const auto& [block, exit] : std::exchange(m_exits, { })) {
        m_out.appendTo(block);
        leave(exit);
    }
}

LValue RowLoopEmitter::emitRowCell(const RowGuard& guard, LValue storage, LValue position)
{
    ValueContext context = m_generator.contextOf(m_code.blockOf(guard.row), guard.row);
    SetForScope<const OperationContext*> whileReading(m_generator.m_operation, &context);
    m_generator.declareAccessesOf(context);
    LValue row = m_out.load64(m_elements.elementOfStorage(ArrayStorage::Contiguous, storage, position));
    if (m_elements.checksBounds()) [[unlikely]]
        m_elements.stopUnlessInside(context, m_elements.isPristineArray(CellPointer(row), { PristineAspects::Storage, PristineAspects::Names }));
    m_out.clearDeclaredAccesses();
    return row;
}

void RowLoopEmitter::emitSteps(std::span<const Step> steps, const Exit& exit)
{
    for (const Step& step : steps) {
        switch (step.kind) {
        case Step::Kind::Value:
            name(step.value, nullptr);
            m_generator.generateValue(step.block, step.value, m_code.at(step.value), false, { });
            break;
        case Step::Kind::Name:
            name(step.value, valueOf(step.source, step.block));
            break;
        case Step::Kind::OwnTest:
        case Step::Kind::Test: {
            LBasicBlock holds = m_out.newBlock();
            LBasicBlock fails = m_out.newBlock();
            BranchTargets targets { normalEdge(holds), unlikelyEdge(fails) };
            if (!step.expected)
                targets = { unlikelyEdge(fails), normalEdge(holds) };
            if (step.kind == Step::Kind::OwnTest) {
                name(step.value, nullptr);
                m_generator.generateBranchCondition(m_contextOfEnd(m_generator, step.block), step.value, targets);
            } else
                m_out.branch(valueOf(step.value, step.block), targets.taken, targets.notTaken);
            continueIn(holds, fails, exit);
            break;
        }
        case Step::Kind::Row: {
            const RowGuard& guard = m_generator.m_rows->guards[step.guard];
            auto holder = std::ranges::find(m_storagesOfHolders, step.guard, &std::pair<unsigned, LValue>::first);
            LValue row = emitRowCell(guard, holder->second, counterIn64Bits(valueOf(m_loop.counter, m_loop.head)));
            for (ValueID rowName : guard.names)
                name(rowName, row);
            break;
        }
        }
        m_out.clearDeclaredAccesses();
        if (m_generator.m_hasFailed)
            return;
    }
}

void RowLoopEmitter::emitPoll()
{
    if (!m_loop.poll)
        return;
    name(m_loop.poll, nullptr);
    m_generator.generateValue(m_loop.head, m_loop.poll, m_code.at(m_loop.poll), false, { });
    m_out.clearDeclaredAccesses();
}

bool RowLoopEmitter::admitsInt32s() const
{
    if (m_loop.form == RowLoop::Form::Map)
        return false;
    bool someIsRow = false;
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        if (isBaseInFront(place))
            continue;
        someIsRow = true;
        if (!m_code.at(m_body->reads[place].access).admitsInt32Storage())
            return false;
    }
    return someIsRow;
}

RowLoopEmitter::Base RowLoopEmitter::requireKindAndLength(const ElementRead& read, LValue cell, ArrayStorage kind, LValue length, const Exit& otherKind, const Exit& tooShort)
{
    ValueContext context = m_generator.contextOf(read.block, read.access);
    SetForScope<const OperationContext*> whileAsking(m_generator.m_operation, &context);
    CellPointer list(cell);
    if (m_code.at(read.access).admitsInt32Storage())
        leaveUnless((kind == ArrayStorage::Double ? m_elements.numericListHoldsDoubles(list) : m_elements.numericListHoldsInt32s(list)).value(), otherKind);
    ArrayElements elements = m_elements.elementsOf(list);
    leaveUnless(m_out.aboveOrEqual(m_elements.publicLengthAsWord(elements).value(), length), tooShort);
    return { elements.butterfly.value(), cell };
}

void RowLoopEmitter::checkAccess(BlockID block, ValueID access, LValue cell, ArrayStorage kind, LValue index, unsigned numberOfElements)
{
    if (!m_elements.checksBounds()) [[likely]]
        return;
    ValueContext context = m_generator.contextOf(block, access);
    SetForScope<const OperationContext*> whileChecking(m_generator.m_operation, &context);
    ArrayElements elements = m_elements.elementsOf(m_elements.askedAnew(CellPointer(cell)));
    m_elements.stopUnlessInside(context, m_elements.hasArrayStorage(elements.array, kind));
    m_elements.stopUnlessInside(context, Condition(m_out.below(m_out.add(index, m_out.constInt64(numberOfElements - 1)), m_elements.publicLength(elements).value())));
}

LValue RowLoopEmitter::emitElement(const ElementRead& read, ArrayStorage kind, const Base& base, LValue index, ptrdiff_t offset, const Exit& exit, bool isVector)
{
    ValueContext context = m_generator.contextOf(read.block, read.access);
    SetForScope<const OperationContext*> whileReading(m_generator.m_operation, &context);
    m_out.setOrigin(machineOriginOf(read.access));
    checkAccess(read.block, read.access, base.cell, kind, offset ? m_out.add(index, m_out.constInt64(offset)) : index, isVector ? lanes : 1);
    m_generator.declareAccessesOf(context);
    TypedPointer first = m_elements.elementOfStorage(kind, base.storage, index);
    TypedPointer element(*first.heap(), first.base(), first.offset() + offset * static_cast<ptrdiff_t>(sizeof(double)));
    LValue result = nullptr;
    if (isVector)
        result = m_out.load(element, Machine::Vector128);
    else if (kind == ArrayStorage::Double)
        result = m_out.loadDouble(element);
    else {
        LValue boxed = m_out.load64(element);
        leaveUnless(m_elements.areInt32Elements(boxed).value(), exit);
        result = m_out.intToDouble(m_out.castToInt32(boxed));
    }
    m_out.clearDeclaredAccesses();
    return result;
}

LValue RowLoopEmitter::emitTerm(ValueID value, LValue sum, std::span<const LValue> elements, bool isVector)
{
    if (value == m_loop.sum)
        return sum;
    VectorOutput vectors(m_out);
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        if (m_body->reads[place].access == value)
            return elements[place];
    }
    const Operation& operation = m_code.at(value);
    BlockID home = m_code.isConstant(value) ? BlockID() : m_code.blockOf(value);
    if (!home || !isInnerBlock(home)) {
        if (isVector) {
            auto known = std::ranges::find(m_splats, value, &std::pair<ValueID, LValue>::first);
            if (known != m_splats.end())
                return known->second;
        }
        LValue number = valueOf(value, m_loop.innerHead);
        return isVector ? vectors.splat(number) : number;
    }
    if (operation.opcode == Opcode::Narrowed)
        return emitTerm(operation.valueOperand(), sum, elements, isVector);
    LValue left = emitTerm(operation.leftOperand(), sum, elements, isVector);
    LValue right = emitTerm(operation.rightOperand(), sum, elements, isVector);
    m_out.setOrigin(machineOriginOf(value));
    switch (operation.opcode) {
    case Opcode::Add:
        return isVector ? vectors.compute(VectorOutput::Arithmetic::Add, left, right) : m_out.doubleAdd(left, right);
    case Opcode::Sub:
        return isVector ? vectors.compute(VectorOutput::Arithmetic::Sub, left, right) : m_out.doubleSub(left, right);
    case Opcode::Mul:
        return isVector ? vectors.compute(VectorOutput::Arithmetic::Mul, left, right) : m_out.doubleMul(left, right);
    case Opcode::Div:
        return isVector ? vectors.compute(VectorOutput::Arithmetic::Div, left, right) : m_out.doubleDiv(left, right);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Vector<LValue, 6> RowLoopEmitter::emitScalarElements(ArrayStorage kind, std::span<const Base> bases, LValue index, unsigned term, LValue column, ptrdiff_t offsetOfColumn, const Exit& exit)
{
    Vector<LValue, 6> elements;
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        const ElementRead& read = m_body->reads[place];
        ArrayStorage kindOfRead = isBaseInFront(place) ? ArrayStorage::Double : kind;
        elements.append(emitElement(read, kindOfRead, bases[place], read.isColumn ? column : index, read.isColumn ? offsetOfColumn : term, exit));
    }
    return elements;
}

LValue RowLoopEmitter::emitCountedRows(const RowGuard& guard, LValue needed, LValue array)
{
    LBasicBlock check = m_out.newBlock();
    LBasicBlock begins = m_out.newBlock();
    LBasicBlock fails = m_out.newBlock();
    LBasicBlock ends = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    LValue position = m_out.phiIn(check, Machine::Int32);
    LValue counted = m_out.phiIn(ends, Machine::Int32);
    LValue result = m_out.phiIn(done, Machine::Int32);
    LValue known = m_out.load32(m_generator.rowCountAddress(guard.count));
    if (needed->type() == Machine::Int64)
        needed = m_out.castToInt32(needed);
    ArrayElements holder = m_elements.elementsOf(CellPointer(array));
    LBasicBlock isKnown = m_out.newBlock();
    m_out.branch(m_out.lessThan(known, needed), normalEdge(begins), normalEdge(isKnown));
    m_out.appendTo(isKnown);
    m_out.upsilon(known, result);
    m_out.jump(normalEdge(done));
    m_out.appendTo(begins);
    LValue length = m_elements.publicLengthAsWord(holder).value();
    LValue end = m_out.select(m_out.below(length, needed), length, needed);
    LBasicBlock hasSome = m_out.newBlock();
    LBasicBlock hasNone = m_out.newBlock();
    m_out.branch(m_out.lessThan(known, end), normalEdge(hasSome), normalEdge(hasNone));
    m_out.appendTo(hasNone);
    m_out.upsilon(known, counted);
    m_out.jump(normalEdge(ends));
    m_out.appendTo(hasSome);
    m_out.upsilon(known, position);
    m_out.jump(normalEdge(check));

    m_out.appendTo(check);
    LValue row = m_out.load64(m_elements.elementOfStorage(ArrayStorage::Contiguous, holder.butterfly.value(), m_out.zeroExt(position, Machine::Int64)));
    for (ValueID rowName : guard.names)
        name(rowName, row);
    LBasicBlock isPresent = m_out.newBlock();
    m_out.branch(m_out.notZero64(row), normalEdge(isPresent), unlikelyEdge(fails));
    m_out.appendTo(isPresent);
    for (BlockID block : guard.checkBlocks) {
        ValueID test = m_generator.testReadByBranchAlone(block, m_code.at(block).terminator);
        LBasicBlock passes = m_out.newBlock();
        name(test, nullptr);
        m_generator.generateBranchCondition(m_contextOfEnd(m_generator, block), test, { normalEdge(passes), unlikelyEdge(fails) });
        m_out.clearDeclaredAccesses();
        if (m_generator.m_hasFailed)
            return known;
        m_out.appendToSuccessor(passes, m_out.forkScope());
    }
    LValue next = m_out.add(position, m_out.int32One);
    LBasicBlock again = m_out.newBlock();
    LBasicBlock all = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, end), normalEdge(again), normalEdge(all));
    m_out.appendTo(again);
    m_out.upsilon(next, position);
    m_out.jump(normalEdge(check));
    m_out.appendTo(all);
    m_out.upsilon(next, counted);
    m_out.jump(normalEdge(ends));
    m_out.appendTo(fails);
    m_out.upsilon(position, counted);
    m_out.jump(normalEdge(ends));
    m_out.appendTo(ends);
    m_out.store32(counted, m_generator.rowCountAddress(guard.count));
    m_out.upsilon(counted, result);
    m_out.jump(normalEdge(done));
    m_out.appendTo(done);
    return result;
}

LValue RowLoopEmitter::emitInnerSum(ArrayStorage kind, std::span<const Base> bases, bool isPair, const Exit& exit)
{
    if (isPair) {
        LValue added = m_firstSum;
        for (unsigned term = 0; term < lanes; ++term)
            added = emitTerm(m_loop.nextSum, added, emitScalarElements(kind, bases, m_out.constInt64(0), term, nullptr, 0, exit).span(), false);
        return added;
    }
    LBasicBlock several = m_out.newBlock();
    LBasicBlock rest = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    LValue index = m_out.phiIn(several, Machine::Int64);
    LValue sum = m_out.phiIn(several, Machine::Double);
    LValue indexOfRest = m_out.phiIn(rest, Machine::Int64);
    LValue sumOfRest = m_out.phiIn(rest, Machine::Double);
    LValue result = m_out.phiIn(done, Machine::Double);
    LBasicBlock begins = m_out.newBlock();
    LBasicBlock isShort = m_out.newBlock();
    m_out.branch(m_out.lessThan(m_length, m_out.constInt64(termsOfIteration)), normalEdge(isShort), normalEdge(begins));
    m_out.appendTo(isShort);
    m_out.upsilon(m_out.constInt64(0), indexOfRest);
    m_out.upsilon(m_firstSum, sumOfRest);
    m_out.jump(normalEdge(rest));
    m_out.appendTo(begins);
    m_out.upsilon(m_out.constInt64(0), index);
    m_out.upsilon(m_firstSum, sum);
    m_out.jump(normalEdge(several));

    auto emitTerms = [&](LValue first, unsigned begin, unsigned end, LValue added) {
        for (unsigned term = begin; term < end; ++term)
            added = emitTerm(m_loop.nextSum, added, emitScalarElements(kind, bases, first, term, nullptr, 0, exit).span(), false);
        return added;
    };
    m_out.appendTo(several);
    LValue sumBehind = emitTerms(index, 0, termsOfIteration, sum);
    LValue indexBehind = m_out.add(index, m_out.constInt64(termsOfIteration));
    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    m_out.branch(m_out.lessThanOrEqual(indexBehind, m_out.sub(m_length, m_out.constInt64(termsOfIteration))), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    m_out.upsilon(indexBehind, index);
    m_out.upsilon(sumBehind, sum);
    m_out.jump(normalEdge(several));
    m_out.appendTo(ended);
    m_out.upsilon(indexBehind, indexOfRest);
    m_out.upsilon(sumBehind, sumOfRest);
    m_out.jump(normalEdge(rest));

    m_out.appendTo(rest);
    LValue remaining = m_out.bitAnd(m_lengthAsWord, m_out.constInt32(termsOfIteration - 1));
    LBasicBlock two = m_out.newBlock();
    LBasicBlock notTwo = m_out.newBlock();
    LBasicBlock some = m_out.newBlock();
    LBasicBlock none = m_out.newBlock();
    LBasicBlock three = m_out.newBlock();
    LBasicBlock one = m_out.newBlock();
    auto end = [&](LValue value) {
        m_out.upsilon(value, result);
        m_out.jump(normalEdge(done));
    };
    m_out.branch(m_out.equal(remaining, m_out.constInt32(2)), normalEdge(two), normalEdge(notTwo));
    m_out.appendTo(two);
    end(emitTerms(indexOfRest, 0, 2, sumOfRest));
    m_out.appendTo(notTwo);
    m_out.branch(m_out.equal(remaining, m_out.int32Zero), normalEdge(none), normalEdge(some));
    m_out.appendTo(none);
    end(sumOfRest);
    m_out.appendTo(some);
    LValue sumOfOne = emitTerms(indexOfRest, 0, 1, sumOfRest);
    m_out.branch(m_out.equal(remaining, m_out.int32One), normalEdge(one), normalEdge(three));
    m_out.appendTo(one);
    end(sumOfOne);
    m_out.appendTo(three);
    end(emitTerms(indexOfRest, 1, 3, sumOfOne));
    m_out.appendTo(done);
    return result;
}

Vector<LValue, 2> RowLoopEmitter::emitNumbers(std::span<const Base> bases, LValue index, unsigned offset, bool isVector, const Exit& exit)
{
    Vector<LValue, 6> elements;
    for (size_t place = 0; place < m_body->reads.size(); ++place)
        elements.append(emitElement(m_body->reads[place], ArrayStorage::Double, bases[place], index, offset, exit, isVector));
    Vector<LValue, 2> numbers;
    for (const ElementStore& store : m_body->stores)
        numbers.append(emitTerm(store.number, nullptr, elements.span(), isVector));
    return numbers;
}

void RowLoopEmitter::emitStores(std::span<const LValue> numbers, std::span<const Base> bases, LValue index, unsigned offset, bool isVector)
{
    for (size_t place = 0; place < m_body->stores.size(); ++place) {
        const ElementStore& store = m_body->stores[place];
        ValueContext context = m_generator.contextOf(store.block, store.access);
        SetForScope<const OperationContext*> whileStoring(m_generator.m_operation, &context);
        m_out.setOrigin(machineOriginOf(store.access));
        checkAccess(store.block, store.access, bases[place].cell, ArrayStorage::Double, offset ? m_out.add(index, m_out.constInt64(offset)) : index, isVector ? lanes : 1);
        m_generator.declareAccessesOf(context);
        TypedPointer first = m_elements.elementOfStorage(ArrayStorage::Double, bases[place].storage, index);
        TypedPointer element(*first.heap(), first.base(), first.offset() + offset * static_cast<ptrdiff_t>(sizeof(double)));
        if (isVector)
            m_out.store(numbers[place], element);
        else
            m_out.storeDouble(numbers[place], element);
        m_out.clearDeclaredAccesses();
    }
}

void RowLoopEmitter::addSplats(ValueID value)
{
    BlockID home = m_code.isConstant(value) ? BlockID() : m_code.blockOf(value);
    if (home && isInnerBlock(home)) {
        if (std::ranges::contains(m_body->reads, value, &ElementRead::access))
            return;
        if (m_generator.opcodeOf(value) == Opcode::Narrowed) {
            addSplats(m_code.at(value).valueOperand());
            return;
        }
        addSplats(m_code.at(value).leftOperand());
        addSplats(m_code.at(value).rightOperand());
        return;
    }
    if (!std::ranges::contains(m_splats, value, &std::pair<ValueID, LValue>::first))
        m_splats.append({ value, VectorOutput(m_out).splat(valueOf(value, m_loop.innerHead)) });
}

void RowLoopEmitter::emitInnerMap(std::span<const Base> reads, std::span<const Base> stores, bool isPair, const Exit& exit)
{
    VectorOutput vectors(m_out);
    LValue zero = m_out.constInt64(0);
    auto sumOfLanes = [&](LValue sum) { return m_out.doubleAdd(vectors.lane(sum, 0), vectors.lane(sum, 1)); };
    auto sumOf = [&](std::span<const LValue> numbers) {
        LValue sum = numbers[0];
        for (size_t place = 1; place < numbers.size(); ++place)
            sum = vectors.compute(VectorOutput::Arithmetic::Add, sum, numbers[place]);
        return sum;
    };
    if (isPair) {
        Vector<LValue, 2> numbers = emitNumbers(reads, zero, 0, true, exit);
        m_out.setOrigin(Machine::Origin());
        LValue sum = sumOfLanes(sumOf(numbers.span()));
        leaveUnless(m_out.doubleEqual(sum, sum), exit);
        emitStores(numbers.span(), stores, zero, 0, true);
        m_out.setOrigin(Machine::Origin());
        return;
    }

    static constexpr unsigned pairsOfIteration = 2;
    static constexpr int64_t numbersOfIteration = pairsOfIteration * lanes;
    LBasicBlock isFour = m_out.newBlock();
    LBasicBlock isOther = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.equal(m_lengthAsWord, m_out.constInt32(numbersOfIteration)), normalEdge(isFour), normalEdge(isOther));
    m_out.appendTo(isFour);
    {
        Vector<LValue, 2> first = emitNumbers(reads, zero, 0, true, exit);
        Vector<LValue, 2> second = emitNumbers(reads, zero, lanes, true, exit);
        m_out.setOrigin(Machine::Origin());
        LValue sum = sumOfLanes(vectors.compute(VectorOutput::Arithmetic::Add, sumOf(first.span()), sumOf(second.span())));
        leaveUnless(m_out.doubleEqual(sum, sum), exit);
        emitStores(first.span(), stores, zero, 0, true);
        emitStores(second.span(), stores, zero, lanes, true);
        m_out.setOrigin(Machine::Origin());
        m_out.jump(normalEdge(done));
    }

    m_out.appendTo(isOther);
    LValue zeros = vectors.splat(m_out.constDouble(0));
    LValue lastOfSeveral = m_out.sub(m_length, m_out.constInt64(numbersOfIteration));
    auto emitLoop = [&](bool isStoring) {
        LBasicBlock several = m_out.newBlock();
        LBasicBlock rest = m_out.newBlock();
        LBasicBlock single = m_out.newBlock();
        LBasicBlock ended = m_out.newBlock();
        LBasicBlock begins = m_out.newBlock();
        LBasicBlock isShort = m_out.newBlock();
        LValue index = m_out.phiIn(several, Machine::Int64);
        LValue sum = isStoring ? nullptr : m_out.phiIn(several, Machine::Vector128);
        LValue indexOfRest = m_out.phiIn(rest, Machine::Int64);
        LValue sumOfRest = isStoring ? nullptr : m_out.phiIn(rest, Machine::Vector128);
        LValue indexOfSingle = m_out.phiIn(single, Machine::Int64);
        LValue sumOfSingles = isStoring ? nullptr : m_out.phiIn(single, Machine::Double);
        LValue result = isStoring ? nullptr : m_out.phiIn(ended, Machine::Double);
        auto pass = [&](LValue passedIndex, LValue passedSum, LValue toIndex, LValue toSum, LBasicBlock to) {
            m_out.upsilon(passedIndex, toIndex);
            if (!isStoring)
                m_out.upsilon(passedSum, toSum);
            m_out.jump(normalEdge(to));
        };
        m_out.branch(m_out.lessThan(lastOfSeveral, zero), normalEdge(isShort), normalEdge(begins));
        m_out.appendTo(isShort);
        pass(zero, zeros, indexOfRest, sumOfRest, rest);
        m_out.appendTo(begins);
        pass(zero, zeros, index, sum, several);
        m_out.appendTo(several);
        LValue added = sum;
        for (unsigned pair = 0; pair < pairsOfIteration; ++pair) {
            Vector<LValue, 2> numbers = emitNumbers(reads, index, pair * lanes, true, exit);
            if (isStoring)
                emitStores(numbers.span(), stores, index, pair * lanes, true);
            else
                added = vectors.compute(VectorOutput::Arithmetic::Add, added, sumOf(numbers.span()));
        }
        m_out.setOrigin(Machine::Origin());
        LValue next = m_out.add(index, m_out.constInt64(numbersOfIteration));
        LBasicBlock again = m_out.newBlock();
        LBasicBlock continues = m_out.newBlock();
        m_out.branch(m_out.lessThanOrEqual(next, lastOfSeveral), normalEdge(again), normalEdge(continues));
        m_out.appendTo(again);
        pass(next, added, index, sum, several);
        m_out.appendTo(continues);
        pass(next, added, indexOfRest, sumOfRest, rest);

        m_out.appendTo(rest);
        LValue sumOfSeveral = isStoring ? nullptr : sumOfLanes(sumOfRest);
        LBasicBlock hasSingle = m_out.newBlock();
        LBasicBlock hasNone = m_out.newBlock();
        m_out.branch(m_out.lessThan(indexOfRest, m_length), normalEdge(hasSingle), normalEdge(hasNone));
        m_out.appendTo(hasNone);
        if (!isStoring)
            m_out.upsilon(sumOfSeveral, result);
        m_out.jump(normalEdge(ended));
        m_out.appendTo(hasSingle);
        pass(indexOfRest, sumOfSeveral, indexOfSingle, sumOfSingles, single);
        m_out.appendTo(single);
        Vector<LValue, 2> last = emitNumbers(reads, indexOfSingle, 0, false, exit);
        LValue addedSingle = sumOfSingles;
        if (isStoring)
            emitStores(last.span(), stores, indexOfSingle, 0, false);
        else {
            for (LValue number : last)
                addedSingle = m_out.doubleAdd(addedSingle, number);
        }
        m_out.setOrigin(Machine::Origin());
        LValue nextSingle = m_out.add(indexOfSingle, m_out.constInt64(1));
        LBasicBlock anotherSingle = m_out.newBlock();
        LBasicBlock noSingle = m_out.newBlock();
        m_out.branch(m_out.lessThan(nextSingle, m_length), normalEdge(anotherSingle), normalEdge(noSingle));
        m_out.appendTo(anotherSingle);
        pass(nextSingle, addedSingle, indexOfSingle, sumOfSingles, single);
        m_out.appendTo(noSingle);
        if (!isStoring)
            m_out.upsilon(addedSingle, result);
        m_out.jump(normalEdge(ended));
        m_out.appendTo(ended);
        return result;
    };
    LValue sum = emitLoop(false);
    leaveUnless(m_out.doubleEqual(sum, sum), exit);
    emitLoop(true);
    m_out.jump(normalEdge(done));
    m_out.appendTo(done);
}

void RowLoopEmitter::emitInnerMapKept(std::span<const Base> reads, std::span<const Base> stores, const Exit& exit)
{
    const ElementStore& store = m_body->stores[0];
    LBasicBlock numbers = m_out.newBlock();
    LValue index = m_out.phiIn(numbers, Machine::Int64);
    LValue sum = m_out.phiIn(numbers, Machine::Double);
    auto keptNumber = [&](LValue place) { return m_out.baseIndex(m_generator.heaps().frameSlots, m_out.slotBase(m_keptNumbers), place, ScaleEight); };
    m_out.upsilon(m_out.constInt64(0), index);
    m_out.upsilon(m_out.constDouble(0), sum);
    m_out.jump(normalEdge(numbers));

    m_out.appendTo(numbers);
    name(m_loop.innerCounter, m_out.castToInt32(index));
    LValue added = sum;
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        const ElementRead& read = m_body->reads[place];
        LValue element = emitElement(read, ArrayStorage::Double, reads[place], index, 0, exit);
        name(read.access, element);
        if (m_body->checkedReads.contains(read.access))
            added = m_out.doubleAdd(added, element);
    }
    emitSteps(m_body->operations.span(), exit);
    if (m_generator.m_hasFailed)
        return;
    m_out.setOrigin(Machine::Origin());
    LValue number = valueOf(store.number, store.block);
    m_out.storeDouble(number, keptNumber(index));
    added = m_out.doubleAdd(added, number);
    LValue next = m_out.add(index, m_out.constInt64(1));
    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, m_length), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    m_out.upsilon(next, index);
    m_out.upsilon(added, sum);
    m_out.jump(normalEdge(numbers));
    m_out.appendTo(ended);
    leaveUnless(m_out.doubleEqual(added, added), exit);

    LBasicBlock copies = m_out.newBlock();
    LValue copied = m_out.phiIn(copies, Machine::Int64);
    m_out.upsilon(m_out.constInt64(0), copied);
    m_out.jump(normalEdge(copies));
    m_out.appendTo(copies);
    LValue kept = m_out.loadDouble(keptNumber(copied));
    emitStores(std::span(&kept, 1), stores, copied, 0, false);
    m_out.setOrigin(Machine::Origin());
    LValue behind = m_out.add(copied, m_out.constInt64(1));
    LBasicBlock another = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.lessThan(behind, m_length), normalEdge(another), normalEdge(done));
    m_out.appendTo(another);
    m_out.upsilon(behind, copied);
    m_out.jump(normalEdge(copies));
    m_out.appendTo(done);
}

void RowLoopEmitter::emitNextIteration(LBasicBlock entry, LValue counter, LValue next)
{
    m_out.setOrigin(Machine::Origin());
    if (!m_limit) {
        m_out.upsilon(next, counter);
        m_out.jump(normalEdge(entry));
        return;
    }
    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, m_limit), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    m_out.upsilon(next, counter);
    m_out.jump(normalEdge(entry));
    m_out.appendTo(ended);
    if (m_columnEnd)
        m_out.jump(normalEdge(m_columnEnd));
    else
        leave(exitTo(m_written, next));
}

void RowLoopEmitter::emitRows(ArrayStorage kind, LBasicBlock entry, LValue counter, bool isPair, const Exit& otherKind)
{
    Exit written = exitTo(m_written, counter);
    m_out.appendTo(entry);
    m_out.setOrigin(Machine::Origin());
    emitPoll();
    name(m_loop.counter, counter);
    emitSteps(m_loop.stepsBefore.span(), written);
    if (m_generator.m_hasFailed)
        return;
    m_out.setOrigin(Machine::Origin());
    Vector<Base, 6> bases = m_bases;
    Vector<std::pair<LValue, Base>, 6> known;
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        if (isBaseInFront(place))
            continue;
        const ElementRead& read = m_body->reads[place];
        LValue cell = valueOf(read.list, m_loop.innerHead);
        size_t earlier = known.findIf([&](const auto& other) { return other.first == cell; });
        bases[place] = earlier != notFound ? known[earlier].second : requireKindAndLength(read, cell, kind, m_lengthAsWord, exitTo(otherKind, counter), written);
        known.append({ cell, bases[place] });
    }
    if (m_loop.form == RowLoop::Form::Map) {
        m_splats.shrink(0);
        for (size_t place = 0; place < m_body->stores.size() && m_body->isVector; ++place)
            addSplats(m_body->stores[place].number);
        Vector<Base, 2> stores = m_basesOfStores;
        for (size_t place = 0; place < m_body->stores.size(); ++place) {
            if (stores[place].cell)
                continue;
            LValue cell = valueOf(m_body->stores[place].list, m_loop.innerHead);
            size_t earlier = known.findIf([&](const auto& other) { return other.first == cell; });
            if (earlier != notFound) {
                stores[place] = known[earlier].second;
                continue;
            }
            ArrayElements elements = m_elements.elementsOf(CellPointer(cell));
            leaveUnless(m_out.aboveOrEqual(m_elements.publicLengthAsWord(elements).value(), m_lengthAsWord), written);
            stores[place] = { elements.butterfly.value(), cell };
        }
        if (m_body->isVector)
            emitInnerMap(bases.span(), stores.span(), isPair, written);
        else {
            emitInnerMapKept(bases.span(), stores.span(), written);
            if (m_generator.m_hasFailed)
                return;
        }
    } else {
        emitSteps(m_loop.stepsBesideSum.span(), written);
        if (m_generator.m_hasFailed)
            return;
        name(m_loop.sum, emitInnerSum(kind, bases.span(), isPair, written));
        emitSteps(m_loop.stepsAfter.span(), written);
        if (m_generator.m_hasFailed)
            return;
    }
    emitNextIteration(entry, counter, m_out.add(counter, m_out.constantOfType(counter->type(), 1)));
}

void RowLoopEmitter::emitColumns(ArrayStorage kind, LBasicBlock entry, LValue counter, const Exit& otherKind)
{
    const RowGuard& guard = m_generator.m_rows->guards[*m_loop.innerGuard];
    auto holder = std::ranges::find(m_storagesOfHolders, *m_loop.innerGuard, &std::pair<unsigned, LValue>::first);
    VectorOutput vectors(m_out);
    bool isVector = kind == ArrayStorage::Double;
    static constexpr std::array<int32_t, 3> widthsOfDoubles { 8, 2, 1 };
    static constexpr std::array<int32_t, 2> widthsOfInt32s { 2, 1 };
    std::span<const int32_t> widths = isVector ? std::span<const int32_t>(widthsOfDoubles) : std::span<const int32_t>(widthsOfInt32s);
    Exit written = exitTo(m_written, counter);
    LType type = counter->type();
    m_out.appendTo(entry);
    m_out.setOrigin(Machine::Origin());
    emitPoll();
    LBasicBlock stores = m_out.newBlock();
    LValue stored = m_out.phiIn(stores, type);
    LValue place = m_out.phiIn(stores, Machine::Int64);
    LValue end = m_out.phiIn(stores, type);
    LValue remaining = m_out.sub(m_limit, counter);
    LValue distance = m_column ? m_out.sub(m_out.sub(m_column, counter), m_out.constantOfType(type, 1)) : nullptr;
    LValue first = counterIn64Bits(counter);
    for (int32_t width : widths) {
        LBasicBlock isWide = m_out.newBlock();
        LBasicBlock isNarrow = width > 1 ? m_out.newBlock() : nullptr;
        if (width > 1) {
            LBasicBlock fits = m_out.newBlock();
            m_out.branch(m_out.lessThan(remaining, m_out.constantOfType(type, width)), normalEdge(isNarrow), normalEdge(fits));
            m_out.appendTo(fits);
            if (distance)
                m_out.branch(m_out.below(distance, m_out.constantOfType(type, width - 1)), normalEdge(isNarrow), normalEdge(isWide));
            else
                m_out.jump(normalEdge(isWide));
        } else
            m_out.jump(normalEdge(isWide));
        m_out.appendTo(isWide);
        bool inVectors = isVector && width > 1;
        unsigned numberOfSums = inVectors ? width / lanes : width;
        LType typeOfSums = inVectors ? Machine::Vector128 : Machine::Double;
        LBasicBlock terms = m_out.newBlock();
        LValue index = m_out.phiIn(terms, Machine::Int64);
        Vector<LValue, 4> sums;
        LValue firstSum = inVectors ? vectors.splat(m_firstSum) : m_firstSum;
        for (unsigned sum = 0; sum < numberOfSums; ++sum) {
            sums.append(m_out.phiIn(terms, typeOfSums));
            m_out.upsilon(firstSum, sums.last());
        }
        LValue endOfColumns = m_out.add(counter, m_out.constantOfType(type, width));
        LValue lengthNeeded = m_out.castToInt32(endOfColumns);
        m_out.upsilon(m_out.constInt64(0), index);
        m_out.jump(normalEdge(terms));

        m_out.appendTo(terms);
        LValue cell = emitRowCell(guard, holder->second, index);
        Vector<Base, 6> bases = m_bases;
        size_t firstOfColumn = m_body->reads.findIf([](const ElementRead& read) { return read.isColumn; });
        Base row = requireKindAndLength(m_body->reads[firstOfColumn], cell, kind, lengthNeeded, exitTo(otherKind, counter), written);
        for (size_t read = 0; read < m_body->reads.size(); ++read) {
            if (m_body->reads[read].isColumn)
                bases[read] = row;
        }
        Vector<LValue, 4> next;
        Vector<LValue, 6> shared;
        for (size_t read = 0; read < m_body->reads.size(); ++read) {
            LValue number = nullptr;
            if (!m_body->reads[read].isColumn) {
                number = emitElement(m_body->reads[read], ArrayStorage::Double, bases[read], index, 0, written);
                if (inVectors)
                    number = vectors.splat(number);
            }
            shared.append(number);
        }
        for (unsigned sum = 0; sum < numberOfSums; ++sum) {
            Vector<LValue, 6> elements;
            for (size_t read = 0; read < m_body->reads.size(); ++read)
                elements.append(shared[read] ? shared[read] : emitElement(m_body->reads[read], kind, bases[read], first, sum * (inVectors ? lanes : 1), written, inVectors));
            next.append(emitTerm(m_loop.nextSum, sums[sum], elements.span(), inVectors));
        }
        m_out.setOrigin(Machine::Origin());
        LValue indexBehind = m_out.add(index, m_out.constInt64(1));
        LBasicBlock again = m_out.newBlock();
        LBasicBlock ended = m_out.newBlock();
        m_out.branch(m_out.lessThan(indexBehind, m_length), normalEdge(again), normalEdge(ended));
        m_out.appendTo(again);
        for (unsigned sum = 0; sum < numberOfSums; ++sum)
            m_out.upsilon(next[sum], sums[sum]);
        m_out.upsilon(indexBehind, index);
        m_out.jump(normalEdge(terms));
        m_out.appendTo(ended);
        for (unsigned sum = 0; sum < numberOfSums; ++sum)
            m_out.store(next[sum], m_out.address(m_generator.heaps().frameSlots, m_out.slotBase(m_sums), sum * (inVectors ? lanes : 1) * sizeof(double)));
        m_out.upsilon(counter, stored);
        m_out.upsilon(m_out.constInt64(0), place);
        m_out.upsilon(endOfColumns, end);
        m_out.jump(normalEdge(stores));
        if (isNarrow)
            m_out.appendTo(isNarrow);
    }

    m_out.appendTo(stores);
    Exit writtenAtStore = exitTo(m_written, stored);
    name(m_loop.counter, stored);
    emitSteps(m_loop.stepsBefore.span(), writtenAtStore);
    emitSteps(m_loop.stepsBesideSum.span(), writtenAtStore);
    if (m_generator.m_hasFailed)
        return;
    name(m_loop.sum, m_out.loadDouble(m_out.baseIndex(m_generator.heaps().frameSlots, m_out.slotBase(m_sums), place, ScaleEight)));
    emitSteps(m_loop.stepsAfter.span(), writtenAtStore);
    if (m_generator.m_hasFailed)
        return;
    m_out.setOrigin(Machine::Origin());
    LValue next = m_out.add(stored, m_out.constantOfType(type, 1));
    LBasicBlock again = m_out.newBlock();
    LBasicBlock hasStored = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, end), normalEdge(again), normalEdge(hasStored));
    m_out.appendTo(again);
    m_out.upsilon(next, stored);
    m_out.upsilon(m_out.add(place, m_out.constInt64(1)), place);
    m_out.upsilon(end, end);
    m_out.jump(normalEdge(stores));
    m_out.appendTo(hasStored);
    emitNextIteration(entry, counter, next);
}

bool RowLoopEmitter::isNamedInRows(ValueID list) const
{
    return std::ranges::any_of(m_loop.stepsBefore, [&](const Step& step) {
        return step.value == list || (step.kind == Step::Kind::Row && m_generator.m_rows->guards[step.guard].names.contains(list));
    });
}

void RowLoopEmitter::emitBasesInFront()
{
    m_bases.shrink(0);
    m_basesOfStores.shrink(0);
    for (const ElementRead& read : m_body->reads) {
        Base base;
        if (!read.isColumn && !isNamedInRows(read.list))
            base = requireKindAndLength(read, valueOf(read.list, m_loop.innerHead), ArrayStorage::Double, m_lengthAsWord, m_front, m_front);
        m_bases.append(base);
    }
    for (const ElementStore& store : m_body->stores) {
        Base base;
        if (!isNamedInRows(store.list)) {
            LValue cell = valueOf(store.list, m_loop.innerHead);
            ArrayElements elements = m_elements.elementsOf(CellPointer(cell));
            leaveUnless(m_out.aboveOrEqual(m_elements.publicLengthAsWord(elements).value(), m_lengthAsWord), m_front);
            base = { elements.butterfly.value(), cell };
        }
        m_basesOfStores.append(base);
    }
}

void RowLoopEmitter::emitHolders(std::span<const Step> steps)
{
    for (const Step& step : steps) {
        if (step.kind != Step::Kind::Row)
            continue;
        const RowGuard& guard = m_generator.m_rows->guards[step.guard];
        LValue array = valueOf(guard.array, m_loop.head);
        if (m_isNested)
            emitCountedArray(guard, array);
        LValue counted = emitCountedRows(guard, m_limit, array);
        if (m_first->type() == Machine::Int64)
            counted = m_out.zeroExt(counted, Machine::Int64);
        m_limit = m_out.select(m_out.lessThan(counted, m_limit), counted, m_limit);
        m_storagesOfHolders.append({ step.guard, m_elements.elementsOf(CellPointer(array)).butterfly.value() });
    }
}

void RowLoopEmitter::emitCountedArray(const RowGuard& guard, LValue array)
{
    LBasicBlock isOtherArray = m_out.newBlock();
    LBasicBlock isSameArray = m_out.newBlock();
    m_out.branch(m_out.equal(m_out.load64(m_generator.rowCountAddress(guard.count, CodeGenerator::RowCount::offsetOfArray)), array), normalEdge(isSameArray), normalEdge(isOtherArray));
    m_out.appendTo(isOtherArray);
    m_out.store64(array, m_generator.rowCountAddress(guard.count, CodeGenerator::RowCount::offsetOfArray));
    m_out.store32(m_out.int32Zero, m_generator.rowCountAddress(guard.count));
    m_out.jump(normalEdge(isSameArray));
    m_out.appendTo(isSameArray);
}

void RowLoopEmitter::emitColumnHolder()
{
    const RowGuard& guard = m_generator.m_rows->guards[*m_loop.innerGuard];
    LValue array = valueOf(guard.array, m_loop.innerHead);
    emitCountedArray(guard, array);
    leaveUnless(m_out.lessThanOrEqual(m_lengthAsWord, emitCountedRows(guard, m_lengthAsWord, array)), m_front);
    m_storagesOfHolders.append({ *m_loop.innerGuard, m_elements.elementsOf(CellPointer(array)).butterfly.value() });
    LType type = m_first->type();
    const Operation& store = m_code.at(m_loop.store);
    if (m_loop.storesAtCounter) {
        LValue stored = valueOf(store.arrayOperand(), m_loop.innerHead);
        for (const ElementRead& read : m_body->reads) {
            if (!read.isColumn)
                leaveUnless(m_out.notEqual(valueOf(read.list, m_loop.innerHead), stored), m_front);
        }
    } else {
        LValue column = valueOf(store.positionOperand(), m_loop.head);
        m_column = column->type() == type ? column : type == Machine::Int32 ? m_out.castToInt32(column) : m_out.signExt32To64(column);
    }
    m_sums = m_generator.m_proc.addStackSlot(8 * sizeof(double));
    name(m_loop.counter, m_first);
    emitSteps(m_loop.stepsBefore.span(), m_front);
    emitSteps(m_loop.stepsBesideSum.span(), m_front);
    m_out.setOrigin(Machine::Origin());
}

void RowLoopEmitter::emitSums()
{
    LType type = m_first->type();
    bool readsColumns = m_loop.form == RowLoop::Form::ColumnSum;
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock int32s = admitsInt32s() ? m_out.newBlock() : nullptr;
    LValue counterOfDoubles = m_out.phiIn(doubles, type);
    LValue counterOfInt32s = int32s ? m_out.phiIn(int32s, type) : nullptr;
    m_out.upsilon(m_first, counterOfDoubles);
    m_out.jump(normalEdge(doubles));
    auto emitKind = [&](ArrayStorage kind, LBasicBlock entry, LValue counter, const Exit& otherKind) {
        if (readsColumns)
            emitColumns(kind, entry, counter, otherKind);
        else {
            m_out.appendTo(entry);
            emitRowsByLength(kind, counter, otherKind);
        }
    };
    emitKind(ArrayStorage::Double, doubles, counterOfDoubles, int32s ? Exit { int32s, counterOfInt32s, nullptr } : m_written);
    if (int32s && !m_generator.m_hasFailed)
        emitKind(ArrayStorage::Int32, int32s, counterOfInt32s, m_written);
}

void RowLoopEmitter::emitRowsByLength(ArrayStorage kind, LValue counter, const Exit& otherKind)
{
    LType type = counter->type();
    LBasicBlock others = m_out.newBlock();
    LValue counterOfOthers = m_out.phiIn(others, type);
    if (!m_body->isVector) {
        m_out.upsilon(counter, counterOfOthers);
        m_out.jump(normalEdge(others));
        emitRows(kind, others, counterOfOthers, false, otherKind);
        return;
    }
    LBasicBlock pairs = m_out.newBlock();
    LBasicBlock isPair = m_out.newBlock();
    LBasicBlock isOther = m_out.newBlock();
    LValue counterOfPairs = m_out.phiIn(pairs, type);
    m_out.branch(m_out.equal(m_lengthAsWord, m_out.constInt32(lanes)), normalEdge(isPair), normalEdge(isOther));
    m_out.appendTo(isPair);
    m_out.upsilon(counter, counterOfPairs);
    m_out.jump(normalEdge(pairs));
    m_out.appendTo(isOther);
    m_out.upsilon(counter, counterOfOthers);
    m_out.jump(normalEdge(others));
    emitRows(kind, pairs, counterOfPairs, true, otherKind);
    if (!m_generator.m_hasFailed)
        emitRows(kind, others, counterOfOthers, false, otherKind);
}

void RowLoopEmitter::emitBodies()
{
    struct Entry {
        LBasicBlock block { nullptr };
        size_t place { 0 };
    };
    Vector<Entry, 16> entries;
    entries.grow(m_loop.bodies.size());
    auto isSame = [](const Step& first, const Step& second) {
        return first.kind == second.kind && first.value == second.value && first.expected == second.expected;
    };
    for (size_t index = 0; index < m_loop.bodies.size(); ++index) {
        const Body& body = m_loop.bodies[index];
        if (index) {
            if (!entries[index].block)
                continue;
            m_out.appendTo(entries[index].block);
        }
        for (size_t place = entries[index].place; place < body.decisions.size(); ++place) {
            const Step& step = body.decisions[place];
            if (step.kind == Step::Kind::Value) {
                emitSteps(std::span(&step, 1), m_front);
                if (m_generator.m_hasFailed)
                    return;
                continue;
            }
            size_t other = notFound;
            for (size_t later = index + 1; later < m_loop.bodies.size() && other == notFound; ++later) {
                const auto& decisions = m_loop.bodies[later].decisions;
                bool sharesPrefix = decisions.size() > place && !entries[later].block && decisions[place].value == step.value && decisions[place].kind == step.kind && decisions[place].expected != step.expected;
                for (size_t before = 0; before < place && sharesPrefix; ++before)
                    sharesPrefix = isSame(decisions[before], body.decisions[before]);
                if (sharesPrefix)
                    other = later;
            }
            if (other == notFound) {
                emitSteps(std::span(&step, 1), m_front);
                if (m_generator.m_hasFailed)
                    return;
                continue;
            }
            LBasicBlock holds = m_out.newBlock();
            entries[other] = { m_out.newBlock(), place + 1 };
            BranchTargets targets { normalEdge(holds), normalEdge(entries[other].block) };
            if (!step.expected)
                targets = targets.swapped();
            if (step.kind == Step::Kind::OwnTest) {
                name(step.value, nullptr);
                m_generator.generateBranchCondition(m_contextOfEnd(m_generator, step.block), step.value, targets);
            } else
                m_out.branch(valueOf(step.value, step.block), targets.taken, targets.notTaken);
            m_out.clearDeclaredAccesses();
            if (m_generator.m_hasFailed)
                return;
            m_out.appendTo(holds);
        }
        m_body = &body;
        m_out.setOrigin(Machine::Origin());
        emitBody();
        if (m_generator.m_hasFailed)
            return;
    }
}

void RowLoopEmitter::emitBody()
{
    if (m_loop.form == RowLoop::Form::Gather) {
        emitGather();
        return;
    }
    if (!m_body->isVector) {
        leaveUnless(m_out.lessThanOrEqual(m_lengthAsWord, m_out.constInt32(mostNumbersKept)), m_front);
        if (!m_keptNumbers)
            m_keptNumbers = m_generator.m_proc.addStackSlot(mostNumbersKept * sizeof(double));
    }
    emitBasesInFront();
    emitRowsByLength(ArrayStorage::Double, m_first, m_written);
}

void RowLoopEmitter::emitGather()
{
    const RowGuard& guard = m_generator.m_rows->guards[*m_loop.innerGuard];
    const ElementStore& store = m_body->stores[0];
    LValue holder = m_elements.elementsOf(CellPointer(valueOf(guard.array, m_loop.head))).butterfly.value();
    LValue cellOfStore = valueOf(store.list, m_loop.head);
    ArrayElements elementsOfStore = m_elements.elementsOf(CellPointer(cellOfStore));
    LValue room = m_elements.publicLengthAsWord(elementsOfStore).value();
    LValue limit = m_out.select(m_out.below(room, m_limit), room, m_limit);
    leaveUnless(m_out.lessThan(m_first, limit), m_front);
    Base baseOfStore { elementsOfStore.butterfly.value(), cellOfStore };
    Vector<Base, 6> bases;
    Vector<LValue, 6> columns;
    for (const ElementRead& read : m_body->reads) {
        LValue column = nullptr;
        Base base;
        if (read.isColumn) {
            column = valueOf(m_code.at(read.access).positionOperand(), m_loop.head);
            if (column->type() == Machine::Int32)
                column = m_out.zeroExt(column, Machine::Int64);
        } else
            base = requireKindAndLength(read, valueOf(read.list, m_loop.head), ArrayStorage::Double, limit, m_front, m_front);
        columns.append(column);
        bases.append(base);
    }
    LBasicBlock iteration = m_out.newBlock();
    LValue counter = m_out.phiIn(iteration, Machine::Int32);
    m_out.upsilon(m_first, counter);
    m_out.jump(normalEdge(iteration));
    m_out.appendTo(iteration);
    m_out.setOrigin(Machine::Origin());
    emitPoll();
    Exit written = exitTo(m_written, counter);
    LValue index = m_out.zeroExt(counter, Machine::Int64);
    LValue cell = emitRowCell(guard, holder, index);
    Vector<LValue, 6> elements;
    LValue checked = nullptr;
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        const ElementRead& read = m_body->reads[place];
        if (read.isColumn) {
            ValueContext context = m_generator.contextOf(read.block, read.access);
            SetForScope<const OperationContext*> whileAsking(m_generator.m_operation, &context);
            CellPointer row(cell);
            if (m_code.at(read.access).admitsInt32Storage())
                leaveUnless(m_elements.numericListHoldsDoubles(row).value(), written);
            ArrayElements elementsOfRow = m_elements.elementsOf(row);
            leaveUnless(m_out.below(columns[place], m_elements.publicLength(elementsOfRow).value()), written);
            bases[place] = { elementsOfRow.butterfly.value(), cell };
        }
        LValue element = emitElement(read, ArrayStorage::Double, bases[place], read.isColumn ? columns[place] : index, 0, written);
        elements.append(element);
        if (m_body->checkedReads.contains(read.access) && !m_body->stores.isEmpty() && store.number != read.access)
            checked = checked ? m_out.doubleAdd(checked, element) : element;
    }
    LValue number = emitTerm(store.number, nullptr, elements.span(), false);
    if (checked)
        leaveUnless(m_out.doubleEqual(checked, checked), written);
    leaveUnless(m_out.doubleEqual(number, number), written);
    emitStores(std::span(&number, 1), std::span(&baseOfStore, 1), index, 0, false);
    m_out.setOrigin(Machine::Origin());
    LValue next = m_out.add(counter, m_out.int32One);
    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, limit), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    m_out.upsilon(next, counter);
    m_out.jump(normalEdge(iteration));
    m_out.appendTo(ended);
    leave(exitTo(m_written, next));
}

std::pair<LValue, LValue> RowLoopEmitter::emitRange(LValue holder, LValue count)
{
    LBasicBlock rows = m_out.newBlock();
    LValue index = m_out.phiIn(rows, Machine::Int64);
    LValue lowest = m_out.phiIn(rows, Machine::Int64);
    LValue highest = m_out.phiIn(rows, Machine::Int64);
    LValue first = m_out.load64(m_elements.elementOfStorage(ArrayStorage::Contiguous, holder, m_out.constInt64(0)));
    m_out.upsilon(m_out.constInt64(0), index);
    m_out.upsilon(first, lowest);
    m_out.upsilon(first, highest);
    m_out.jump(normalEdge(rows));
    m_out.appendTo(rows);
    LValue row = m_out.load64(m_elements.elementOfStorage(ArrayStorage::Contiguous, holder, index));
    LValue lower = m_out.select(m_out.below(row, lowest), row, lowest);
    LValue higher = m_out.select(m_out.below(highest, row), row, highest);
    LValue next = m_out.add(index, m_out.constInt64(1));
    LBasicBlock again = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, m_out.zeroExt(count, Machine::Int64)), normalEdge(again), normalEdge(done));
    m_out.appendTo(again);
    m_out.upsilon(next, index);
    m_out.upsilon(lower, lowest);
    m_out.upsilon(higher, highest);
    m_out.jump(normalEdge(rows));
    m_out.appendTo(done);
    return { lower, higher };
}

void RowLoopEmitter::leaveUnlessDisjoint(std::pair<LValue, LValue> first, std::pair<LValue, LValue> second)
{
    leaveUnless(m_out.bitOr(m_out.below(first.second, second.first), m_out.below(second.second, first.first)), m_front);
}

void RowLoopEmitter::emitFrontOfNested(const RowLoop& nest, const Exit& front)
{
    m_isNested = true;
    m_front = front;
    m_first = m_out.int32Zero;
    emitSteps(nest.stepsOfNest.span(), m_front);
    if (m_generator.m_hasFailed)
        return;
    m_out.setOrigin(Machine::Origin());
    m_limit = valueOf(m_loop.bound, m_loop.head);
    LValue bound = m_limit;
    leaveUnless(m_out.greaterThan(bound, m_out.int32Zero), m_front);
    emitHolders(m_loop.stepsBefore.span());
    emitHolders(m_loop.stepsBesideSum.span());
    leaveUnless(m_out.equal(m_limit, bound), m_front);
    m_lengthAsWord = valueOf(m_loop.innerBound, m_loop.head);
    leaveUnless(m_out.below(m_out.sub(m_lengthAsWord, m_out.int32One), m_out.constInt32(longestInnerLoop)), m_front);
    m_length = m_out.zeroExt(m_lengthAsWord, Machine::Int64);
    m_firstSum = valueOf(m_loop.firstSum, m_loop.head);
    m_body = &m_loop.bodies[0];
    if (m_loop.form == RowLoop::Form::ColumnSum) {
        const RowGuard& guard = m_generator.m_rows->guards[*m_loop.innerGuard];
        LValue array = valueOf(guard.array, m_loop.head);
        emitCountedArray(guard, array);
        leaveUnless(m_out.lessThanOrEqual(m_lengthAsWord, emitCountedRows(guard, m_lengthAsWord, array)), m_front);
        m_storagesOfHolders.append({ *m_loop.innerGuard, m_elements.elementsOf(CellPointer(array)).butterfly.value() });
        m_sums = m_generator.m_proc.addStackSlot(8 * sizeof(double));
    }
    emitBasesInFront();
    name(m_loop.counter, bound);
    name(m_loop.innerCounter, m_lengthAsWord);
    emitSteps(nest.stepsAtEnd.span(), m_front);
    m_out.setOrigin(Machine::Origin());
}

void RowLoopEmitter::emitGatherOfNest(std::span<const Base> stores, std::pair<LValue, LValue> storedRows, const Exit& exit)
{
    LValue column = counterIn64Bits(exit.counter);
    Vector<Base, 6> bases = m_bases;
    bool readsColumn = false;
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        const ElementRead& read = m_body->reads[place];
        readsColumn |= read.isColumn;
        if (read.isColumn || bases[place].cell)
            continue;
        LValue cell = valueOf(read.list, m_loop.innerHead);
        bases[place] = requireKindAndLength(read, cell, ArrayStorage::Double, m_lengthAsWord, exit, exit);
        leaveUnless(m_out.bitOr(m_out.below(cell, storedRows.first), m_out.below(storedRows.second, cell)), exit);
    }
    if (!readsColumn) {
        m_splats.shrink(0);
        addSplats(m_body->stores[0].number);
        emitInnerMap(bases.span(), stores, false, exit);
        return;
    }
    const RowGuard& guard = m_generator.m_rows->guards[*m_loop.innerGuard];
    auto holder = std::ranges::find(m_storagesOfHolders, *m_loop.innerGuard, &std::pair<unsigned, LValue>::first);
    LBasicBlock rows = m_out.newBlock();
    LValue index = m_out.phiIn(rows, Machine::Int64);
    m_out.upsilon(m_out.constInt64(0), index);
    m_out.jump(normalEdge(rows));
    m_out.appendTo(rows);
    LValue cell = emitRowCell(guard, holder->second, index);
    Vector<LValue, 6> elements;
    Base row;
    for (size_t place = 0; place < m_body->reads.size(); ++place) {
        const ElementRead& read = m_body->reads[place];
        if (read.isColumn && !row.cell)
            row = requireKindAndLength(read, cell, ArrayStorage::Double, m_out.castToInt32(m_out.add(column, m_out.constInt64(1))), exit, exit);
        elements.append(emitElement(read, ArrayStorage::Double, read.isColumn ? row : bases[place], read.isColumn ? column : index, 0, exit));
    }
    LValue number = emitTerm(m_body->stores[0].number, nullptr, elements.span(), false);
    leaveUnless(m_out.doubleEqual(number, number), exit);
    emitStores(std::span(&number, 1), stores, index, 0, false);
    m_out.setOrigin(Machine::Origin());
    LValue next = m_out.add(index, m_out.constInt64(1));
    LBasicBlock again = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, m_length), normalEdge(again), normalEdge(done));
    m_out.appendTo(again);
    m_out.upsilon(next, index);
    m_out.jump(normalEdge(rows));
    m_out.appendTo(done);
}

void RowLoopEmitter::emitNest()
{
    const RowLoop& nested = *m_loop.nested;
    RowLoopEmitter sums(m_generator, nested, m_contextOfEnd);
    m_body = &m_loop.bodies[0];
    emitSteps(m_loop.stepsBesideSum.span(), m_front);
    if (m_generator.m_hasFailed)
        return;
    sums.emitFrontOfNested(m_loop, m_front);
    if (m_generator.m_hasFailed)
        return;
    emitBasesInFront();
    if (m_loop.innerGuard) {
        const RowGuard& guard = m_generator.m_rows->guards[*m_loop.innerGuard];
        LValue array = valueOf(guard.array, m_loop.innerHead);
        emitCountedArray(guard, array);
        leaveUnless(m_out.lessThanOrEqual(m_lengthAsWord, emitCountedRows(guard, m_lengthAsWord, array)), m_front);
        m_storagesOfHolders.append({ *m_loop.innerGuard, m_elements.elementsOf(CellPointer(array)).butterfly.value() });
    }
    auto holderOf = [&](const RowLoopEmitter& emitter, unsigned guard) { return std::ranges::find(emitter.m_storagesOfHolders, guard, &std::pair<unsigned, LValue>::first)->second; };
    size_t storedStep = nested.stepsBesideSum.findIf([&](const Step& step) { return step.kind == Step::Kind::Row && m_generator.m_rows->guards[step.guard].names.contains(m_code.at(nested.store).arrayOperand()); });
    std::pair<LValue, LValue> storedRows = emitRange(holderOf(sums, nested.stepsBesideSum[storedStep].guard), sums.m_limit);
    for (const Step& step : nested.stepsBefore) {
        if (step.kind == Step::Kind::Row)
            leaveUnlessDisjoint(storedRows, emitRange(holderOf(sums, step.guard), sums.m_limit));
    }
    if (nested.innerGuard)
        leaveUnlessDisjoint(storedRows, emitRange(holderOf(sums, *nested.innerGuard), sums.m_lengthAsWord));
    if (m_loop.innerGuard)
        leaveUnlessDisjoint(storedRows, emitRange(holderOf(*this, *m_loop.innerGuard), m_lengthAsWord));

    LBasicBlock columns = m_out.newBlock();
    LValue column = m_out.phiIn(columns, Machine::Int32);
    m_out.upsilon(m_first, column);
    m_out.jump(normalEdge(columns));
    m_out.appendTo(columns);
    Exit exit = exitTo(m_written, column);
    emitPoll();
    name(m_loop.counter, column);
    emitSteps(m_loop.stepsBefore.span(), exit);
    if (m_generator.m_hasFailed)
        return;
    m_out.setOrigin(Machine::Origin());
    emitGatherOfNest(m_basesOfStores.span(), storedRows, exit);
    name(m_loop.innerCounter, m_lengthAsWord);
    emitSteps(m_loop.stepsAfter.span(), exit);
    if (m_generator.m_hasFailed)
        return;
    sums.m_written = exit;
    sums.m_columnEnd = m_out.newBlock();
    sums.emitSteps(nested.stepsInFront.span(), exit);
    if (m_generator.m_hasFailed)
        return;
    m_out.setOrigin(Machine::Origin());
    if (nested.form == RowLoop::Form::ColumnSum) {
        LValue stored = sums.valueOf(m_code.at(nested.store).positionOperand(), nested.head);
        sums.m_column = stored->type() == Machine::Int64 ? m_out.castToInt32(stored) : stored;
    }
    sums.emitSums();
    if (m_generator.m_hasFailed)
        return;
    sums.emitExits();
    m_out.appendTo(sums.m_columnEnd);
    m_out.setOrigin(Machine::Origin());
    emitNextIteration(columns, column, m_out.add(column, m_out.int32One));
    for (const auto& [value, lowered] : sums.m_saved)
        m_generator.m_values[value.index()] = lowered;
}

void RowLoopEmitter::emit(LValue first, const Exit& front, const Exit& written)
{
    m_first = first;
    m_front = front;
    m_written = written;
    m_out.clearDeclaredAccesses();
    switch (m_loop.form) {
    case RowLoop::Form::Sum:
        m_generator.build().statistics().count(StatisticID::RowSumLoops);
        break;
    case RowLoop::Form::ColumnSum:
        m_generator.build().statistics().count(StatisticID::ColumnSumLoops);
        break;
    case RowLoop::Form::Map:
        m_generator.build().statistics().count(StatisticID::RowMapLoops);
        break;
    case RowLoop::Form::Gather:
        m_generator.build().statistics().count(StatisticID::GatherLoops);
        break;
    case RowLoop::Form::Nest:
        m_generator.build().statistics().count(StatisticID::RowSumNests);
        break;
    }
    m_out.setOrigin(Machine::Origin());
    SetForScope<BlockID> whileEmitting(m_generator.m_headOfTurnsLaidOut, m_loop.head);
    auto finish = makeScopeExit([&] {
        if (m_generator.m_hasFailed)
            return;
        emitExits();
        for (const auto& [value, lowered] : m_saved)
            m_generator.m_values[value.index()] = lowered;
    });
    if (m_loop.form == RowLoop::Form::Gather) {
        const RowGuard& guard = m_generator.m_rows->guards[*m_loop.innerGuard];
        LValue bound = valueOf(m_loop.innerBound, m_loop.head);
        leaveUnless(m_out.lessThan(first, bound), m_front);
        LValue counted = emitCountedRows(guard, bound, valueOf(guard.array, m_loop.head));
        m_limit = m_out.select(m_out.lessThan(counted, bound), counted, bound);
        leaveUnless(m_out.lessThan(first, m_limit), m_front);
        emitBodies();
        return;
    }
    name(m_loop.counter, first);
    emitSteps(m_loop.stepsInFront.span(), m_front);
    if (m_generator.m_hasFailed)
        return;
    m_out.setOrigin(Machine::Origin());
    if (m_loop.bound) {
        m_limit = valueOf(m_loop.bound, m_loop.innerHead);
        leaveUnless(m_out.lessThan(first, m_limit), m_front);
    }
    emitHolders(m_loop.stepsBefore.span());
    emitHolders(m_loop.stepsBesideSum.span());
    if (!m_storagesOfHolders.isEmpty())
        leaveUnless(m_out.lessThan(first, m_limit), m_front);
    m_lengthAsWord = valueOf(m_loop.innerBound, m_loop.innerHead);
    leaveUnless(m_out.below(m_out.sub(m_lengthAsWord, m_out.int32One), m_out.constInt32(longestInnerLoop)), m_front);
    m_length = m_out.zeroExt(m_lengthAsWord, Machine::Int64);
    name(m_loop.innerCounter, m_lengthAsWord);
    if (m_loop.form == RowLoop::Form::Nest) {
        emitNest();
        return;
    }
    if (m_loop.form == RowLoop::Form::Map) {
        emitSteps(m_loop.stepsAfter.span(), m_front);
        if (m_generator.m_hasFailed)
            return;
        emitBodies();
    } else {
        m_body = &m_loop.bodies[0];
        m_firstSum = valueOf(m_loop.firstSum, m_loop.innerHead);
        if (m_loop.form == RowLoop::Form::ColumnSum)
            emitColumnHolder();
        if (m_generator.m_hasFailed)
            return;
        emitBasesInFront();
        emitSums();
    }
}

void CodeGenerator::tryEmitRowLoop(BlockID head)
{
    if (m_keepsValuesInHeapFrame || m_followsNumbersOutOfBytes || m_interleaving || m_headOfTurnsLaidOut || code().at(head).parameters().size() != 1)
        return;
    static constexpr size_t mostLoops = 6;
    Vector<RowLoop, 2> loops;
    Vector<BlockID, mostLoops> innerHeads;
    Vector<std::pair<BlockID, BlockID>, mostLoops> nestedHeads;
    while (loops.size() < mostLoops) {
        std::optional<RowLoop> loop = m_rows ? RowLoopFinder(*this, head, RowLoopFinder::UsesCounts::Yes, innerHeads.span()).find() : std::nullopt;
        if (!loop)
            loop = RowLoopFinder(*this, head, RowLoopFinder::UsesCounts::No, innerHeads.span()).find();
        if (!loop && loops.isEmpty() && m_rows)
            loop = RowLoopFinder(*this, head, RowLoopFinder::UsesCounts::No, { }).findGather();
        if (!loop && m_rows && (loops.isEmpty() || loops.last().form == RowLoop::Form::Nest))
            loop = RowLoopFinder(*this, head, RowLoopFinder::UsesCounts::No, { }).findNest(nestedHeads.span());
        if (!loop)
            break;
        if (loop->form == RowLoop::Form::Nest)
            nestedHeads.append({ loop->innerHead, loop->nested->head });
        else
            innerHeads.append(loop->innerHead);
        loops.append(WTF::move(*loop));
        if (loops.last().form == RowLoop::Form::Gather)
            break;
    }
    if (loops.isEmpty())
        return;

    ValueID counter = loops[0].counter;
    LValue first = m_values[counter.index()];
    LBasicBlock writtenHead = m_out.newBlock();
    LBasicBlock written = m_out.newBlock();
    LValue counterOfWrittenHead = m_out.phiIn(writtenHead, first->type());
    LValue counterOfWritten = m_out.phiIn(written, first->type());
    RowLoopEmitter::Exit exit { written, counterOfWritten, first };
    LBasicBlock entry = blockFor(head);
    for (size_t place = 0; place < loops.size(); ++place) {
        LBasicBlock next = place + 1 < loops.size() ? m_out.newBlock() : nullptr;
        m_out.appendTo(entry);
        RowLoopEmitter(*this, loops[place], [](const CodeGenerator& generator, BlockID block) {
            return TerminatorContext(block, generator.code().at(block).terminator, nullptr);
        }).emit(first, next ? RowLoopEmitter::Exit { next, nullptr, nullptr } : exit, exit);
        if (m_hasFailed)
            return;
        entry = next;
    }
    m_out.appendTo(written);
    m_out.upsilon(counterOfWritten, counterOfWrittenHead);
    m_out.jump(normalEdge(writtenHead));
    m_blocks[head.index()] = writtenHead;
    m_values[counter.index()] = counterOfWrittenHead;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

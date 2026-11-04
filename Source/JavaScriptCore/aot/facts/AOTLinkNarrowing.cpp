/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkUnions.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

// ---- What a test lets through

// A failing side keeps whatever is not sure to pass, the mark of a variable that is not initialized too.
static constexpr uint16_t primitivesThatTestsMeetInNarrowing = Primitive::All | Primitive::Absent;
// 0 === -0, and NaN, which is falsy, is a whole number here.
static constexpr TypeID zeroesInNarrowing = *tryMakeTypeOfPrimitives(Primitive::Integer | Primitive::WholeNumber, Range::one(0));
static constexpr TypeID primitivesMaybeFalsyInNarrowing = *tryMakeTypeOfPrimitives(Primitive::UndefinedOrNull | Primitive::False | Primitive::Integer | Primitive::WholeNumber | Primitive::String | Primitive::BigInt | Primitive::Absent, Range::one(0));
static constexpr TypeID primitivesMaybeTruthyInNarrowing = typeOfPrimitives(primitivesThatTestsMeetInNarrowing & ~(Primitive::UndefinedOrNull | Primitive::False));

static TypesPassedByTest typesOutsideInNarrowing(uint16_t primitivesSurelyInside, bool everyObjectIsInside)
{
    if (!primitivesSurelyInside && !everyObjectIsInside)
        return { };
    TypeID primitives = typeOfPrimitives(static_cast<uint16_t>(primitivesThatTestsMeetInNarrowing & ~primitivesSurelyInside));
    if (everyObjectIsInside)
        return { primitives, { } };
    if (isNeverType(primitives))
        return { TypeTable::object, { } };
    return { primitives, TypeTable::object };
}

// The failing side loses a kind only where the tested type holds every value of the kind, and where the test answers the same for ever:
// an object that is of no tested layout today may be what Link knows under that layout all the same, so a test for layouts, classes or named objects cuts nothing where it fails.
static TypesPassedByTest typesPassedByTypeTestInNarrowing(const Program& program, TypeID tested, bool isOnTakenEdge)
{
    if (!tested)
        return { };
    if (isOnTakenEdge)
        return { tested, { } };

    uint16_t primitives = 0;
    Range integers;
    bool fractionsAreBounded = false;
    bool holdsEveryObject = false;
    if (holdsOnlyPrimitives(tested)) {
        primitives = primitivesInID(tested);
        integers = integersInID(tested);
    } else {
        Type type = program.at(tested);
        if (!type.isJavaScriptValue() || type.hasNamedObjects() || type.unnamedLayouts || type.unnamedClasses)
            return { };
        primitives = type.primitives;
        integers = type.integers;
        fractionsAreBounded = !type.boundsOfFractions.isEmpty();
        holdsEveryObject = type.anyObject;
    }
    uint16_t surelyInside = primitives & primitivesThatTestsMeetInNarrowing;
    if (integers != Range::full())
        surelyInside &= ~Primitive::Integer;
    if (fractionsAreBounded)
        surelyInside &= ~Primitive::FractionalNumber;
    return typesOutsideInNarrowing(surelyInside, holdsEveryObject);
}

static TypeID kindOfIntegerInNarrowing(int64_t value)
{
    if (!Range::full().contains(value))
        return typeOfPrimitives(Primitive::WholeNumber);
    if (!value)
        return zeroesInNarrowing;
    return Range::int32().contains(value) ? TypeTable::int32 : TypeTable::integer;
}

// The kind of a primitive that is written. Null: the value is none.
static TypeID kindOfConstantInNarrowing(const BodyValue& constant)
{
    if (!constant || constant.isParameter())
        return { };
    switch (constant.opcode()) {
    case Opcode::Undefined:
        return TypeTable::undefined;
    case Opcode::Null:
        return TypeTable::null;
    case Opcode::Boolean:
        return typeOfPrimitives(constant.as<Opcode::Boolean>().boolean() ? Primitive::True : Primitive::False);
    case Opcode::Integer:
        return kindOfIntegerInNarrowing(constant.as<Opcode::Integer>().integer());
    case Opcode::Double: {
        double number = constant.as<Opcode::Double>().number();
        PrimitiveSet kind = primitiveOfNumber(number);
        if (kind == PrimitiveSet::Integer)
            return kindOfIntegerInNarrowing(static_cast<int64_t>(number));
        return kind == PrimitiveSet::WholeNumber ? zeroesInNarrowing : typeOfPrimitives(Primitive::FractionalNumber);
    }
    case Opcode::String:
        return TypeTable::string;
    case Opcode::BigInt:
        return TypeTable::bigInt;
    default:
        return { };
    }
}

// `x === "a"` leaves texts, `x === 0` both zeroes. Where it fails only a value that is a kind by itself is gone. `x == null` is the test for both, and no other loose comparison says anything: it converts.
static TypesPassedByTest typesPassedByComparisonInNarrowing(Opcode opcode, bool isOnTakenEdge, const BodyValue& other)
{
    TypeID kind = kindOfConstantInNarrowing(other);
    if (!kind)
        return { };
    if (opcode == Opcode::Eq) {
        if (!isNullishType(kind))
            return { };
        return isOnTakenEdge ? TypesPassedByTest { TypeTable::nullish, { } } : typesOutsideInNarrowing(Primitive::UndefinedOrNull, false);
    }
    if (opcode != Opcode::StrictEq)
        return { };
    if (isOnTakenEdge)
        return { kind, { } };
    uint16_t primitives = primitivesInID(kind);
    if (primitives & ~(Primitive::UndefinedOrNull | Primitive::Boolean))
        return { };
    return typesOutsideInNarrowing(primitives, false);
}

TypesPassedByTest typesPassedByTest(const Program& program, const BodyValue& test, uint32_t operandIndex, bool isOnTakenEdge, const BodyValue& other)
{
    Proof proof = provesOf(test.opcode());
    if (proof == Proof::TestedType) {
        if (test.opcode() != Opcode::TypeTest || operandIndex)
            return { };
        return typesPassedByTypeTestInNarrowing(program, test.as<Opcode::TypeTest>().testedType(), isOnTakenEdge);
    }
    if (proof == Proof::Truthiness) {
        if (operandIndex)
            return { };
        return isOnTakenEdge ? TypesPassedByTest { primitivesMaybeTruthyInNarrowing, TypeTable::object } : TypesPassedByTest { primitivesMaybeFalsyInNarrowing, { } };
    }
    if (proof == Proof::Equality)
        return typesPassedByComparisonInNarrowing(test.opcode(), isOnTakenEdge, other);
    return { };
}

TypesPassedByTest typesPassedByCase(const BodyTerminator& terminator, uint32_t targetIndex)
{
    if (targetIndex >= terminator.numberOfCases())
        return { };
    if (terminator.kind() == TerminatorKind::Switch)
        return { kindOfIntegerInNarrowing(terminator.as<TerminatorKind::Switch>().cases()[targetIndex]), { } };
    if (terminator.kind() == TerminatorKind::SwitchString)
        return { TypeTable::string, { } };
    return { };
}

TypesPassedByTest typesPassedByCheck(const Program&, const BodyValue& check)
{
    if (check.opcode() == Opcode::RequireObjectCoercible)
        return typesOutsideInNarrowing(Primitive::UndefinedOrNull, false);
    if (check.opcode() == Opcode::RequireObject)
        return { TypeTable::object, { } };
    return { };
}

static BodyValue definitionBehindCastsInNarrowing(ScanAskedByNames& scan, ValueID value)
{
    BodyValue definition = value ? scan.definitionOf(value) : BodyValue();
    while (definition && inputOperationsThatHandOn.contains(definition.opcode()) && definition.operand(0))
        definition = scan.definitionOf(definition.operand(0));
    return definition;
}

TypeID typeThatAlonePassesOnEdge(ScanAskedByNames& scan, const Program& program, const BodyTerminator& terminator, const BodyValue& test, uint32_t operandIndex, uint32_t targetIndex)
{
    TypesPassedByTest passes;
    if (terminator.hasCases()) {
        if (!operandIndex)
            passes = typesPassedByCase(terminator, targetIndex);
    } else if (test && terminator.isConditionalBranch()) {
        bool comparesTwo = operandIndex < 2 && numberOfNamedOperandsOf(test.opcode()) >= 2;
        passes = typesPassedByTest(program, test, operandIndex, !targetIndex, comparesTwo ? definitionBehindCastsInNarrowing(scan, test.operand(1 - operandIndex)) : BodyValue());
    }
    return passes.second ? TypeID() : passes.first;
}

// ---- The names

void NamesBehindTestsInScan::begin(uint32_t numberOfBlocks, uint32_t numberOfValues, bool follows)
{
    m_follows = follows;
    m_flowIsNotWhatNamesWentBy = false;
    m_numberOfValues = numberOfValues;
    m_inForce = none;
    m_headOnEdge = std::nullopt;
    m_valueAskedAsAtRead = none;
    m_nameAsAtRead = none;
    m_names.shrink(0);
    m_parts.shrink(0);
    m_nameOfValue.shrink(0);
    m_edges.shrink(0);
    m_joins.shrink(0);
    m_namesAtReads.clear();
    m_blocks.shrink(0);
    if (follows)
        m_blocks.grow(numberOfBlocks);
}

uint32_t NamesBehindTestsInScan::add(uint32_t parent, Name&& name)
{
    if (m_names.isEmpty())
        m_nameOfValue.fill(none, m_numberOfValues);
    name.parent = parent;
    name.depth = depthOf(parent) + 1;
    uint32_t jump = parent == none ? none : m_names[parent].jump;
    uint32_t jumpOfJump = jump == none ? none : m_names[jump].jump;
    bool jumpsAreAlike = depthOf(parent) - depthOf(jump) == depthOf(jump) - depthOf(jumpOfJump);
    name.jump = jumpsAreAlike ? jumpOfJump : parent;
    m_names.append(WTF::move(name));
    return static_cast<uint32_t>(m_names.size() - 1);
}

uint32_t NamesBehindTestsInScan::addOtherName(uint32_t parent, uint32_t value, uint32_t group, uint32_t sameAs)
{
    Name name;
    name.value = value;
    name.group = group;
    name.firstPart = static_cast<uint32_t>(m_parts.size());
    name.numberOfParts = 1;
    m_parts.append(sameAs);
    return add(parent, WTF::move(name));
}

// A cast hands its operand on, so what has passed under one name has passed under each.
uint32_t NamesBehindTestsInScan::nameValueAndWhatItWasCastFrom(ScanAskedByNames& scan, uint32_t parent, ValueID subject, TypesPassedByTest passes, uint32_t group, uint32_t positionOfProof)
{
    if (!scan.mayGoUnderName(subject))
        return parent;
    Name name;
    name.value = subject.index();
    name.group = group;
    name.passes = passes;
    name.nameInFrontOfTest = nameOf(subject);
    name.positionOfProof = positionOfProof;
    uint32_t first = add(parent, WTF::move(name));
    uint32_t head = first;
    for (BodyValue cast = scan.definitionOf(subject); cast && inputOperationsThatHandOn.contains(cast.opcode()) && cast.operand(0); cast = scan.definitionOf(cast.operand(0)))
        head = addOtherName(head, cast.operand(0).index(), group, first);
    return head;
}

uint32_t NamesBehindTestsInScan::climbTo(uint32_t name, uint32_t depth) const
{
    while (depthOf(name) > depth) {
        uint32_t jump = m_names[name].jump;
        name = depthOf(jump) >= depth ? jump : m_names[name].parent;
    }
    return name;
}

uint32_t NamesBehindTestsInScan::commonPartOf(uint32_t first, uint32_t second) const
{
    uint32_t depth = std::min(depthOf(first), depthOf(second));
    first = climbTo(first, depth);
    second = climbTo(second, depth);
    while (first != second) {
        bool jumpsDiffer = m_names[first].jump != m_names[second].jump;
        first = jumpsDiffer ? m_names[first].jump : m_names[first].parent;
        second = jumpsDiffer ? m_names[second].jump : m_names[second].parent;
    }
    return first;
}

void NamesBehindTestsInScan::standIn(uint32_t head)
{
    uint32_t leaving = m_inForce;
    uint32_t entering = head;
    m_path.shrink(0);
    while (leaving != entering) {
        if (depthOf(leaving) >= depthOf(entering)) {
            const Name& name = m_names[leaving];
            m_nameOfValue[name.value] = name.hidden;
            leaving = name.parent;
        } else {
            m_path.append(entering);
            entering = m_names[entering].parent;
        }
    }
    for (size_t index = m_path.size(); index--;) {
        Name& name = m_names[m_path[index]];
        name.hidden = std::exchange(m_nameOfValue[name.value], m_path[index]);
    }
    m_inForce = head;
}

void NamesBehindTestsInScan::beginEdge(ScanAskedByNames& scan, const Program& program, const BodyTerminator& terminator, uint32_t targetIndex, const BodyTarget& target)
{
    m_headOnEdge = m_inForce;
    if (!m_follows)
        return;
    uint32_t to = target.block().index();
    auto positionOfProofOf = [&](uint32_t operandIndex) {
        std::span<const uint32_t> proofs = target.proofs();
        for (size_t index = 0; index < proofs.size(); ++index) {
            if (proofs[index] == operandIndex)
                return static_cast<uint32_t>(target.arguments().size() + index);
        }
        return none;
    };

    if (terminator.hasCases()) {
        ValueID subject = terminator.operand(0);
        TypesPassedByTest passes = subject ? typesPassedByCase(terminator, targetIndex) : TypesPassedByTest();
        if (passes)
            m_headOnEdge = nameValueAndWhatItWasCastFrom(scan, *m_headOnEdge, subject, passes, to, positionOfProofOf(0));
        return;
    }
    if (!terminator.isConditionalBranch())
        return;

    ValueID condition = terminator.as<TerminatorKind::Branch>().conditionOperand();
    BodyValue asked = condition ? scan.definitionOf(condition) : BodyValue();
    BodyValue test = asked;
    bool isOnTakenEdge = !targetIndex;
    while (test && (test.opcode() == Opcode::LogicalNot || inputOperationsThatHandOn.contains(test.opcode())) && test.operand(0)) {
        isOnTakenEdge ^= test.opcode() == Opcode::LogicalNot;
        test = scan.definitionOf(test.operand(0));
    }
    if (!test || provesOf(test.opcode()) == Proof::None)
        return;
    bool edgeSpeaksOfTest = test.id() == asked.id();
    uint32_t numberOfOperands = std::min<uint32_t>(numberOfNamedOperandsOf(test.opcode()), 2);
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        ValueID subject = test.operand(operandIndex);
        if (!subject)
            continue;
        BodyValue other = numberOfOperands == 2 ? definitionBehindCastsInNarrowing(scan, test.operand(1 - operandIndex)) : BodyValue();
        if (TypesPassedByTest passes = typesPassedByTest(program, test, operandIndex, isOnTakenEdge, other))
            m_headOnEdge = nameValueAndWhatItWasCastFrom(scan, *m_headOnEdge, subject, passes, to, edgeSpeaksOfTest ? positionOfProofOf(operandIndex) : none);
    }
}

void NamesBehindTestsInScan::leadEdge(uint32_t from, uint32_t to)
{
    uint32_t head = m_headOnEdge.value_or(m_inForce);
    m_headOnEdge = std::nullopt;
    if (!m_follows)
        return;
    AtBlock& at = m_blocks[to];
    if (to <= from) {
        if (at.isEntered && (at.namesItsParameters || climbTo(head, depthOf(at.names)) != at.names))
            m_flowIsNotWhatNamesWentBy = true;
        return;
    }
    // An edge that brings no name is not written down: enterBlock() hears how many there are.
    if (head == none)
        return;
    at.common = at.numberOfEdges ? commonPartOf(at.common, head) : head;
    ++at.numberOfEdges;
    m_edges.append({ head, at.firstEdge });
    at.firstEdge = static_cast<uint32_t>(m_edges.size() - 1);
}

uint32_t NamesBehindTestsInScan::findInGroupAtHead(uint32_t head, uint32_t value, uint32_t depthOfCommonPart) const
{
    if (head == none)
        return none;
    uint32_t group = m_names[head].group;
    for (uint32_t name = head; depthOf(name) > depthOfCommonPart && m_names[name].group == group; name = m_names[name].parent) {
        if (m_names[name].value == value)
            return name;
    }
    return none;
}

uint32_t NamesBehindTestsInScan::uniteWhatEveryEdgeHasCut(const AtBlock& at, uint32_t block)
{
    uint32_t head = at.common;
    uint32_t depthOfCommonPart = depthOf(at.common);
    uint32_t headOfFirstEdge = m_edges[at.firstEdge].head;
    uint32_t group = m_names[headOfFirstEdge].group;
    Vector<uint32_t, 4> values;
    for (uint32_t name = headOfFirstEdge; depthOf(name) > depthOfCommonPart && m_names[name].group == group; name = m_names[name].parent)
        values.appendIfNotContains(m_names[name].value);

    for (uint32_t value : values) {
        uint32_t firstPart = static_cast<uint32_t>(m_parts.size());
        bool everyEdgeHasCutIt = true;
        for (uint32_t edge = at.firstEdge; edge != none && everyEdgeHasCutIt; edge = m_edges[edge].next) {
            uint32_t part = findInGroupAtHead(m_edges[edge].head, value, depthOfCommonPart);
            everyEdgeHasCutIt = part != none;
            m_parts.append(part);
        }
        if (!everyEdgeHasCutIt) {
            m_parts.shrink(firstPart);
            continue;
        }
        Name name;
        name.value = value;
        name.group = block;
        name.firstPart = firstPart;
        name.numberOfParts = static_cast<uint32_t>(m_parts.size()) - firstPart;
        head = add(head, WTF::move(name));
    }
    return head;
}

void NamesBehindTestsInScan::enterBlock(const BodyBlock& block, uint32_t numberOfEdgesRead)
{
    if (!m_follows)
        return;
    uint32_t number = block.id().index();
    AtBlock at = m_blocks[number];
    at.isEntered = true;
    if (!at.numberOfEdges || at.numberOfEdges != numberOfEdgesRead)
        at.names = none;
    else if (at.numberOfEdges > 1)
        at.names = uniteWhatEveryEdgeHasCut(at, number);
    else {
        // The parameter that takes the proof of the one edge is what has passed.
        at.names = at.common;
        for (uint32_t name = at.common; name != none && m_names[name].group == number; name = m_names[name].parent) {
            uint32_t position = m_names[name].positionOfProof;
            if (position == none || position >= block.numberOfParameters())
                continue;
            at.names = addOtherName(at.names, block.parameter(position).index(), number, name);
            at.namesItsParameters = true;
        }
    }
    m_blocks[number] = at;
    standIn(at.names);
}

void NamesBehindTestsInScan::noteCheck(ScanAskedByNames& scan, const Program& program, const BodyValue& check)
{
    if (!m_follows || !check.operand(0))
        return;
    TypesPassedByTest passes = typesPassedByCheck(program, check);
    if (!passes)
        return;
    // A group of its own: it is no test in front of an edge.
    uint32_t group = none;
    uint32_t before = m_inForce;
    uint32_t head = nameValueAndWhatItWasCastFrom(scan, before, check.operand(0), passes, group, none);
    if (head == before)
        return;
    uint32_t first = climbTo(head, depthOf(before) + 1);
    standIn(addOtherName(head, check.id().index(), group, first));
}

// The list of a value is the same wherever it is asked for. Only the name hangs on the place.
WordsOfListInScan NamesBehindTestsInScan::listWithoutName(ScanAskedByNames& scan, uint32_t value)
{
    SetForScope valueAsked(m_valueAskedAsAtRead, value);
    SetForScope nameAnswered(m_nameAsAtRead, none);
    WordsOfListInScan list = scan.listOf(ValueID(value));
    // A value that comes from nowhere is one that the scan knows nothing of.
    return list.count ? list : scan.listOfNode(0);
}

uint32_t NamesBehindTestsInScan::behindOtherNames(uint32_t name) const
{
    while (m_names[name].numberOfParts == 1)
        name = m_parts[m_names[name].firstPart];
    return name;
}

void NamesBehindTestsInScan::makeJoin(ScanAskedByNames& scan, uint32_t name, std::span<const WordsOfListInScan> lists)
{
    uint32_t join = scan.newJoin();
    m_names[name].join = join;
    m_joins.append(join);
    TypesPassedByTest passes = m_names[name].passes;
    for (WordsOfListInScan list : lists) {
        scan.flowInto(list, join, passes.first);
        if (passes.second)
            scan.flowInto(list, join, passes.second);
    }
}

// What flows into a Join is asked for in front of the Join: it may make nodes, and m_joins ascends.
// A row of guards on one value is a row of names, each behind the one in front: it is gone along, and not down into.
WordsOfListInScan NamesBehindTestsInScan::listUnder(ScanAskedByNames& scan, uint32_t name)
{
    Vector<uint32_t, 8> row;
    uint32_t cursor = behindOtherNames(name);
    while (cursor != none && m_names[cursor].join == noSummaryNode && !m_names[cursor].numberOfParts) {
        row.append(cursor);
        cursor = m_names[cursor].nameInFrontOfTest == none ? none : behindOtherNames(m_names[cursor].nameInFrontOfTest);
    }

    WordsOfListInScan list;
    if (cursor == none)
        list = listWithoutName(scan, m_names[row.last()].value);
    else {
        if (m_names[cursor].join == noSummaryNode) {
            Vector<WordsOfListInScan, 4> lists;
            for (uint32_t index = 0; index < m_names[cursor].numberOfParts; ++index)
                lists.append(listUnder(scan, m_parts[m_names[cursor].firstPart + index]));
            makeJoin(scan, cursor, lists.span());
        }
        list = scan.listOfNode(m_names[cursor].join);
    }
    for (size_t index = row.size(); index--;) {
        makeJoin(scan, row[index], singleElementSpan(list));
        list = scan.listOfNode(m_names[row[index]].join);
    }
    return list;
}

uint32_t NamesBehindTestsInScan::joinBehindTest(ScanAskedByNames& scan, WordsOfListInScan list) const
{
    if (m_joins.isEmpty())
        return noSummaryNode;
    uint32_t node = scan.onlyNodeOf(list);
    return node != noSummaryNode && std::ranges::binary_search(m_joins, node) ? node : noSummaryNode;
}

void NamesBehindTestsInScan::noteRead(uint32_t read, ValueID object)
{
    if (uint32_t name = object ? nameOf(object) : none; name != none)
        m_namesAtReads.set(read, name);
}

NamesBehindTestsInScan::NameAsAtRead::NameAsAtRead(NamesBehindTestsInScan& names, uint32_t read, ValueID object)
    : m_names(names)
    , m_valueBefore(names.m_valueAskedAsAtRead)
    , m_nameBefore(names.m_nameAsAtRead)
{
    if (!object || names.m_names.isEmpty())
        return;
    auto found = names.m_namesAtReads.find(read);
    names.m_valueAskedAsAtRead = object.index();
    names.m_nameAsAtRead = found == names.m_namesAtReads.end() ? none : found->value;
}

NamesBehindTestsInScan::NameAsAtRead::~NameAsAtRead()
{
    m_names.m_valueAskedAsAtRead = m_valueBefore;
    m_names.m_nameAsAtRead = m_nameBefore;
}

// ---- Calls that wait

bool callWaitsForArgument(LinkFlow& flow, FunctionID caller, uint32_t indexOfCall)
{
    const SummaryCall& call = flow.summaries.at(caller).calls[indexOfCall];
    if (call.waitsFor == noSummaryNode)
        return false;
    flow.observe(flow.nodeOf(caller, call.waitsFor), FlowObserver { FlowObserver::Kind::ArgumentOfWaitingCall, caller, indexOfCall });
    return true;
}

// It decides on the type that it is told, and entering a function twice is entering it once.
void LinkFlow::waitingCallObserved(const FlowObserver& observer, const LinkType& now)
{
    if (now.isNothing())
        return;
    enter(observer.function, observer.record, summaries.at(observer.function).calls[observer.record].function);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

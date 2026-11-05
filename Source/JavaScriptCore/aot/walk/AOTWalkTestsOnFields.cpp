/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTPrimitives.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTypeNarrowing.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t leastIntegersInTestsOnFields = 2;
static constexpr uint32_t leastTextsInTestsOnFields = 3;

static constexpr OpcodeSet equalitiesThatTestsOnFieldsStandFor { Opcode::StrictEq, Opcode::StringEqualsLiteral, Opcode::ValueEqualsLiteral };

namespace {

struct ConstantInTestsOnFields {
    bool isText { false };
    int64_t number { 0 };
    StringID string;
};

struct CaseInTestsOnFields {
    int64_t number { 0 };
    StringID string;
    BlockID asks;
    BlockID equal;
    ValueID nameOfHolder;
};

struct LinkInTestsOnFields {
    BlockID onEdge;
    BlockID asks;
    ValueID read;
};

} // anonymous namespace

static ValueID valueBehindNamesInTestsOnFields(const Code& code, ValueID value)
{
    while (value && opcodesThatGiveSameValue.contains(code.at(value).opcode)) {
        ValueID source = code.at(value).valueOperand();
        if (source.index() >= value.index())
            return { };
        value = source;
    }
    return value;
}

static bool isOperationOfBlockInTestsOnFields(const BodyIndex& index, BodyBlock block, ValueID value)
{
    return value && block.holds(value) && !index.isParameter(value);
}

static bool isConstantBehindCastsInTestsOnFields(const BodyIndex& index, ValueID value)
{
    ValueID root = index.valueBehindCasts(value);
    return root && !index.isParameter(root) && primitiveConstants.contains(index.opcodeOf(root));
}

static std::optional<ConstantInTestsOnFields> caseBehindCastsInTestsOnFields(const BodyIndex& index, ValueID value)
{
    ValueID root = index.valueBehindCasts(value);
    if (!root || index.isParameter(root))
        return std::nullopt;
    BodyValue operation = index.value(root);
    if (operation.opcode() == Opcode::String)
        return ConstantInTestsOnFields { true, 0, operation.as<Opcode::String>().string() };
    if (operation.opcode() != Opcode::Integer)
        return std::nullopt;
    int64_t number = operation.as<Opcode::Integer>().integer();
    if (!Range::full().contains(number))
        return std::nullopt;
    return ConstantInTestsOnFields { false, number, { } };
}

static bool standsUnderHandlerOfHeadInTestsOnFields(BodyBlock head, BodyBlock passed)
{
    if (head.hasHandler() != passed.hasHandler())
        return false;
    if (!head.hasHandler())
        return true;
    return head.handler().block() == passed.handler().block() && std::ranges::equal(head.handler().arguments(), passed.handler().arguments());
}

static bool mayBePassedByInTestsOnFields(const BodyIndex& index, BodyBlock head, BodyBlock passed)
{
    BlockID id = passed.id();
    if (index.numberOfIncomingEdges(id) != 1 || index.isLoopHead(id) || index.isEnteredByHandlerEdge(id) || passed.isHandlerBlock() || passed.subject())
        return false;
    return id != head.id() && index.loopOf(id) == index.loopOf(head.id()) && standsUnderHandlerOfHeadInTestsOnFields(head, passed);
}

static bool comparesSubjectItselfInTestsOnFields(const Code& code, Reference comparison, Reference subject)
{
    if (!comparison || !comparison.isValue())
        return false;
    ValueID built = valueBehindNamesInTestsOnFields(code, comparison.id());
    if (!built || !equalitiesThatTestsOnFieldsStandFor.contains(code.at(built).opcode))
        return false;
    ValueID producer = valueBehindNamesInTestsOnFields(code, subject.id());
    return producer && std::ranges::any_of(code.at(built).operands(), [&](ValueID operand) {
        return operand && !code.isConstant(operand) && valueBehindNamesInTestsOnFields(code, operand) == producer;
    });
}

namespace {

class TestsOfOneFieldInTestsOnFields {
public:
    TestsOfOneFieldInTestsOnFields(Walker& walker, BodyBlock head)
        : m_walker(walker)
        , m_index(walker.currentBodyIndex())
        , m_head(head)
    {
    }

    bool find()
    {
        Question first;
        if (!ask(m_head, first))
            return false;
        m_comparesTexts = first.constant.isText;
        m_subject = m_walker.referenceOf(first.subject);
        if (!m_subject || !m_subject.isValue())
            return false;
        if (!comparesSubjectItselfInTestsOnFields(m_walker.code(), m_walker.referenceOf(first.comparison), m_subject) || !findReadThatMayBeRepeated(first.subject, first.comparison))
            return false;
        TypeID typeOfSubject = m_walker.typeOf(m_subject);
        Representation form = m_walker.representationOf(m_subject);
        if (!typeOfSubject || isNeverType(typeOfSubject) || (!m_comparesTexts && form != Representation::JSValue && form != Representation::Int32 && form != Representation::Int64))
            return false;
        take(first, m_head.id());

        while (tryAddLink()) { }
        if (m_links.isEmpty())
            return false;
        removeRepeatedCases();
        return true;
    }

    bool comparesTexts() const { return m_comparesTexts; }
    Reference subject() const { return m_subject; }
    Vector<CaseInTestsOnFields, 16>& cases() { return m_cases; }
    BlockID blockOfLastComparison() const { return m_blockOfLastComparison; }
    BlockID blockWhereNothingMatches() const { return m_differs; }

    bool comesToTwoPlaces() const
    {
        if (!narrowsHolderAlone(m_index.block(m_differs), m_nameAtLastComparison))
            return false;
        std::optional<BodyTarget> first;
        ValueID nameGivenByFirst;
        for (const CaseInTestsOnFields& listed : m_cases) {
            BodyBlock onEdge = m_index.block(listed.equal);
            if (!narrowsHolderAlone(onEdge, listed.nameOfHolder))
                return false;
            BodyTarget arrival = onEdge.terminator().next();
            if (!first) {
                first = arrival;
                nameGivenByFirst = onEdge.firstValue();
                continue;
            }
            std::span<const ValueID> brought = arrival.arguments();
            std::span<const ValueID> broughtByFirst = first->arguments();
            if (arrival.block() != first->block() || brought.size() != broughtByFirst.size())
                return false;
            for (size_t position = 0; position < brought.size(); ++position) {
                bool bothBringHolder = brought[position] == onEdge.firstValue() && broughtByFirst[position] == nameGivenByFirst;
                if (bothBringHolder || brought[position] == broughtByFirst[position])
                    continue;
                if (m_index.valueBehindCasts(brought[position]) != m_index.valueBehindCasts(broughtByFirst[position]))
                    return false;
            }
        }
        return true;
    }

    void appendBlocksBetweenComparisons(Vector<BlockID, 16>& blocks) const
    {
        for (const LinkInTestsOnFields& link : m_links) {
            blocks.append(link.onEdge);
            blocks.append(link.asks);
        }
    }

    void bindValuesBetweenComparisons()
    {
        for (const LinkInTestsOnFields& link : m_links) {
            bindHolderNarrowedIn(link.onEdge);
            m_walker.bind(link.read, m_subject);
        }
    }

    void bindHolderNarrowedIn(BlockID id)
    {
        BodyBlock onEdge = m_index.block(id);
        BodyValue narrowed = m_index.value(onEdge.firstValue());
        m_walker.bind(onEdge.parameter(0), m_subject);
        m_walker.bind(narrowed.id(), m_walker.referenceOf(narrowed.operand(OperandIndex::NarrowByField::value)));
    }

private:
    struct Question {
        ValueID comparison;
        ValueID subject;
        ConstantInTestsOnFields constant;
        BlockID equal;
        BlockID differs;
    };

    bool ask(BodyBlock block, Question& question) const
    {
        BodyTerminator terminator = block.terminator();
        if (terminator.kind() != TerminatorKind::Branch || terminator.numberOfTargets() != 2)
            return false;
        ValueID comparison = terminator.as<TerminatorKind::Branch>().conditionOperand();
        if (!isOperationOfBlockInTestsOnFields(m_index, block, comparison) || m_index.opcodeOf(comparison) != Opcode::StrictEq || !m_index.hasOneUse(comparison))
            return false;
        BodyTarget taken = terminator.taken();
        BodyTarget notTaken = terminator.notTaken();
        if (taken.proofs().size() != 1 || notTaken.proofs().size() != 1 || taken.proofs()[0] != notTaken.proofs()[0] || !taken.arguments().empty() || !notTaken.arguments().empty())
            return false;
        uint32_t subjectIndex = taken.proofs()[0];
        if (subjectIndex > OperandIndex::StrictEq::right)
            return false;

        BodyValue equality = m_index.value(comparison);
        std::optional<ConstantInTestsOnFields> constant = caseBehindCastsInTestsOnFields(m_index, equality.operand(1 - subjectIndex));
        if (!constant || isConstantBehindCastsInTestsOnFields(m_index, equality.operand(subjectIndex)))
            return false;
        question = { comparison, equality.operand(subjectIndex), *constant, taken.block(), notTaken.block() };
        return true;
    }

    void take(const Question& question, BlockID asks)
    {
        m_cases.append(CaseInTestsOnFields { question.constant.number, question.constant.string, asks, question.equal, m_nameOfHolder });
        m_blockOfLastComparison = asks;
        m_nameAtLastComparison = m_nameOfHolder;
        m_differs = question.differs;
    }

    bool findReadThatMayBeRepeated(ValueID subject, ValueID comparison)
    {
        ValueID root = m_index.valueBehindCasts(subject);
        if (!isOperationOfBlockInTestsOnFields(m_index, m_head, root) || m_index.opcodeOf(root) != Opcode::PropGet)
            return false;
        BodyOperation<Opcode::PropGet> written = m_index.value(root).as<Opcode::PropGet>();
        if (written.isFreeName())
            return false;
        for (uint32_t behind = root.index() + 1; behind < m_head.endOfValues().index(); ++behind) {
            BodyValue operation = m_index.value(ValueID(behind));
            if (operation.id() != comparison && !primitiveConstants.contains(operation.opcode()) && operation.opcode() != Opcode::Upcast)
                return false;
        }

        const Code& code = m_walker.code();
        ValueID slotRead = valueBehindNamesInTestsOnFields(code, m_subject.id());
        if (!slotRead || code.at(slotRead).opcode != Opcode::GetSlot || testsItsOwnGuess(code.at(slotRead)) || code.at(slotRead).key() != written.key() || !code.at(slotRead).traits().isRemovableWhenUnused())
            return false;
        m_nameOfHolder = written.objectOperand();
        m_key = written.key();
        return !!m_nameOfHolder;
    }

    bool repeatsRead(BodyBlock block, ValueID value) const
    {
        if (!isOperationOfBlockInTestsOnFields(m_index, block, value) || m_index.opcodeOf(value) != Opcode::PropGet || m_index.useCount(value) != 3)
            return false;
        BodyOperation<Opcode::PropGet> read = m_index.value(value).as<Opcode::PropGet>();
        return !read.isFreeName() && read.key() == m_key && isHolder(read.objectOperand(), m_nameOfHolder);
    }

    bool narrowsHolderAlone(BodyBlock onEdge, ValueID nameOfHolder) const
    {
        if (onEdge.numberOfValues() != 1)
            return false;
        BodyTerminator end = onEdge.terminator();
        if (end.kind() != TerminatorKind::Jump || !end.next().proofs().empty())
            return false;
        return narrowsHolderAtHead(onEdge, nameOfHolder);
    }

    bool isHolder(ValueID value, ValueID nameOfHolder) const
    {
        if (value == nameOfHolder)
            return true;
        if (!m_holder)
            m_holder = m_index.valueBehindCasts(m_nameOfHolder);
        return m_index.valueBehindCasts(value) == m_holder;
    }

    bool narrowsHolderAtHead(BodyBlock onEdge, ValueID nameOfHolder) const
    {
        if (!mayBePassedByInTestsOnFields(m_index, m_head, onEdge) || onEdge.numberOfParameters() != 1 || !onEdge.numberOfValues())
            return false;
        BodyValue operation = m_index.value(onEdge.firstValue());
        if (operation.opcode() != Opcode::NarrowByField || !m_index.hasOneUse(onEdge.parameter(0)))
            return false;
        BodyOperation<Opcode::NarrowByField> narrowed = operation.as<Opcode::NarrowByField>();
        return narrowed.readOperand() == onEdge.parameter(0) && narrowed.key() == m_key && isHolder(narrowed.valueOperand(), nameOfHolder);
    }

    bool tryAddLink()
    {
        BodyBlock onEdge = m_index.block(m_differs);
        BodyBlock asks = onEdge;
        bool blockOnEdgeAsks = !narrowsHolderAlone(onEdge, m_nameOfHolder);
        if (blockOnEdgeAsks && !narrowsHolderAtHead(onEdge, m_nameOfHolder))
            return false;
        m_nameOfHolder = onEdge.firstValue();
        if (!blockOnEdgeAsks) {
            BodyTarget onward = onEdge.terminator().next();
            asks = m_index.block(onward.block());
            if (onward.numberOfIncomingValues() || asks.numberOfParameters() || !mayBePassedByInTestsOnFields(m_index, m_head, asks))
                return false;
        }

        Question question;
        if (!ask(asks, question) || question.constant.isText != m_comparesTexts || !repeatsRead(asks, question.subject))
            return false;
        if (question.equal == m_head.id() || question.differs == m_head.id())
            return false;
        for (BodyValue operation : asks.values()) {
            if (operation.id() == question.comparison || operation.id() == question.subject || (blockOnEdgeAsks && operation.id() == onEdge.firstValue()))
                continue;
            bool onlyFeedsComparison = primitiveConstants.contains(operation.opcode()) || operation.opcode() == Opcode::Upcast;
            if (!onlyFeedsComparison || !m_index.hasOneUse(operation.id()))
                return false;
        }

        m_links.append(LinkInTestsOnFields { onEdge.id(), asks.id(), question.subject });
        take(question, asks.id());
        return true;
    }

    void removeRepeatedCases()
    {
        Vector<uint32_t, 16> order;
        for (uint32_t place = 0; place < m_cases.size(); ++place)
            order.append(place);
        AOT::stableSort(order, { }, [&](uint32_t place) {
            return std::pair { m_cases[place].number, m_cases[place].string };
        });
        Vector<bool, 16> isRepeated;
        isRepeated.fill(false, m_cases.size());
        for (size_t place = 1; place < order.size(); ++place) {
            const CaseInTestsOnFields& earlier = m_cases[order[place - 1]];
            const CaseInTestsOnFields& later = m_cases[order[place]];
            isRepeated[order[place]] = earlier.number == later.number && earlier.string == later.string;
        }
        uint32_t place = 0;
        m_cases.removeAllMatching([&](const CaseInTestsOnFields&) {
            return isRepeated[place++];
        });
    }

    Walker& m_walker;
    const BodyIndex& m_index;
    BodyBlock m_head;
    bool m_comparesTexts { false };
    Reference m_subject;
    mutable ValueID m_holder;
    ValueID m_nameOfHolder;
    ValueID m_nameAtLastComparison;
    PropertyKeyID m_key;
    Vector<CaseInTestsOnFields, 16> m_cases;
    Vector<LinkInTestsOnFields, 16> m_links;
    BlockID m_blockOfLastComparison;
    BlockID m_differs;
};

} // anonymous namespace

static TerminatorDraft draftOfSwitchInTestsOnFields(Reference subject, bool comparesTexts, std::span<const CaseInTestsOnFields> cases, Origin origin)
{
    TerminatorDraft terminator(comparesTexts ? TerminatorKind::SwitchString : TerminatorKind::Switch, origin);
    terminator.setSubjectOperand(subject);
    if (comparesTexts) {
        Vector<StringID, 16> strings;
        for (const CaseInTestsOnFields& listed : cases)
            strings.append(listed.string);
        terminator.setStrings(strings.span());
        return terminator;
    }
    Vector<int64_t, 16> numbers;
    for (const CaseInTestsOnFields& listed : cases)
        numbers.append(listed.number);
    terminator.setCases(numbers.span());
    return terminator;
}

bool Walker::endBlockWithSwitchOverTestsOfField(const BodyTerminator& terminator)
{
    if (terminator.kind() != TerminatorKind::Branch || !has(Mechanism::TypedOperations) || isUnreachable())
        return false;
    const BodyIndex& index = currentBodyIndex();
    if (index.hasIrreducibleFlow() || isWalkingBlockForEachWay(m_frames.size() - 1))
        return false;

    TestsOfOneFieldInTestsOnFields tests(*this, index.block(terminator.block()));
    if (!tests.find())
        return false;
    Reference subject = tests.subject();
    bool comparesTexts = tests.comparesTexts();
    TypeID typeOfSubject = typeOf(subject);
    if (!typeOfSubject || isNeverType(typeOfSubject))
        return false;
    Representation form = representationOf(subject);
    if (!comparesTexts && form != Representation::JSValue && form != Representation::Int32 && form != Representation::Int64)
        return false;
    TypingContext context = typing();
    tests.cases().removeAllMatching([&](const CaseInTestsOnFields& listed) {
        return isNeverType(comparesTexts ? narrowByStringCase(context, typeOfSubject, listed.string) : narrowByIntegerCase(context, typeOfSubject, listed.number));
    });
    if (tests.cases().size() < (comparesTexts ? leastTextsInTestsOnFields : leastIntegersInTestsOnFields))
        return false;

    Vector<BlockID, 16> blocksPassedBy;
    tests.appendBlocksBetweenComparisons(blocksPassedBy);
    TerminatorDraft switchOverConstants = draftOfSwitchInTestsOnFields(subject, comparesTexts, tests.cases().span(), m_origin);

    if (tests.comesToTwoPlaces()) {
        BlockID equalOfFirstCase = tests.cases().first().equal;
        for (const CaseInTestsOnFields& listed : tests.cases())
            blocksPassedBy.append(listed.equal);
        blocksPassedBy.append(tests.blockWhereNothingMatches());
        tests.bindValuesBetweenComparisons();
        tests.bindHolderNarrowedIn(equalOfFirstCase);
        tests.bindHolderNarrowedIn(tests.blockWhereNothingMatches());
        takeNarrowerNamesPast(blocksPassedBy.span());

        BlockID equalsSomeConstant = addBlock();
        BlockID equalsNoConstant = addBlock();
        Vector<BlockID, 16> targets;
        targets.fill(equalsSomeConstant, tests.cases().size());
        endWithSwitchOverOwnBlocks(switchOverConstants, targets.span(), equalsNoConstant);

        auto goOnBehind = [&](BlockID block, BlockID passedBy) {
            continueIn(block);
            if (isUnreachable() || m_hasFailed)
                return;
            EdgeToTargetOfBody jumpOfBody { passedBy, 0 };
            endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(jumpOfBody));
        };
        goOnBehind(equalsSomeConstant, equalOfFirstCase);
        goOnBehind(equalsNoConstant, tests.blockWhereNothingMatches());
        m_block = { };
        m_state.clear();
        return true;
    }

    tests.bindValuesBetweenComparisons();
    takeNarrowerNamesPast(blocksPassedBy.span());
    Vector<EdgeToTargetOfBody, 16> edges;
    Vector<uint32_t, 16> casesLeft;
    if (std::optional<TerminatorDraft> switchOverShapes = draftOfSwitchOverShapes(switchOverConstants, casesLeft)) {
        for (uint32_t place : casesLeft)
            edges.append(EdgeToTargetOfBody { tests.cases()[place].asks, 0 });
        switchOverConstants = WTF::move(*switchOverShapes);
    } else {
        for (const CaseInTestsOnFields& listed : tests.cases())
            edges.append(EdgeToTargetOfBody { listed.asks, 0 });
    }
    edges.append(EdgeToTargetOfBody { tests.blockOfLastComparison(), 1 });
    endBlockLeadingTo(WTF::move(switchOverConstants), edges.span());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

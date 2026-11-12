/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTResultRules.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static bool holdsObjectsForLinkOperators(const LinkType& type)
{
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.hasUnknownObjects;
}

class StateOfLinkOperators {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkOperators);
    WTF_MAKE_NONCOPYABLE(StateOfLinkOperators);
public:
    explicit StateOfLinkOperators(Program&);

    uint16_t resultOf(Opcode, std::span<const uint16_t> operands);

private:
    TypeArena m_types;
    TypingContext m_typing;
    HashMap<uint64_t, uint16_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_results;
};

StateOfLinkOperators::StateOfLinkOperators(Program& program)
    : m_types(program.types, ArenaKeeps::UntilReset)
    , m_typing { program, m_types, PositionTypes::knowingNothing() }
{
}

uint16_t StateOfLinkOperators::resultOf(Opcode opcode, std::span<const uint16_t> operands)
{
    uint64_t question = static_cast<uint64_t>(opcode);
    for (uint16_t primitives : operands)
        question = question << 16 | primitives;
    auto known = m_results.find(question);
    if (known != m_results.end())
        return known->value;

    Vector<TypeID, SummaryOperator::mostOperands> types;
    for (uint16_t primitives : operands) {
        LinkType operand;
        operand.primitives = primitives;
        types.append(operand.primitivesAsType());
    }
    Type result = m_types.at(resultTypeOfOperator(m_typing, opcode, types.span()));
    RELEASE_ASSERT(result.isJavaScriptValue() && !result.hasObjects());
    uint16_t answer = LinkType::primitivesOf(result.primitives, result.integers);
    m_results.add(question, answer);
    return answer;
}

void DeleterOfStateOfLinkOperators::operator()(StateOfLinkOperators* state) const
{
    delete state;
}

void LinkFlow::prepareOperatorsForSeveralThreads()
{
    if (!m_stateOfLinkOperators)
        m_stateOfLinkOperators.reset(new StateOfLinkOperators(program));
}

std::optional<uint16_t> LinkFlow::primitivesThatOperatorGivesWhateverArrives(FunctionID function, uint32_t record)
{
    const ThinSummary& summary = summaries.at(function);
    const SummaryOperator& operation = summary.operators[record];
    if (flowRuleOf(operation.opcode) == FlowRule::ToPropertyKey)
        return std::nullopt;
    Vector<uint16_t, SummaryOperator::mostOperands> operands;
    for (uint32_t index = 0; index < SummaryOperator::mostOperands && operation.operands[index] != noSummaryNode; ++index) {
        const SummaryNode& operand = summary.nodes[operation.operands[index]];
        LinkType written = operand.kind == SummaryNode::Kind::Constant ? fromTypeID(TypeID::fromBits(operand.first)) : LinkType::anything();
        uint16_t primitives = (holdsObjectsForLinkOperators(written) ? LinkType::anything().primitives : written.primitives) & LinkType::closed(Primitive::All);
        if (!primitives)
            return std::nullopt;
        operands.append(primitives);
    }
    if (operands.isEmpty())
        return std::nullopt;

    prepareOperatorsForSeveralThreads();
    uint64_t words = static_cast<uint64_t>(operation.opcode) << 40 | static_cast<uint64_t>(operands.size()) << 32 | static_cast<uint64_t>(operands[0]) << 16 | (operands.size() > 1 ? operands[1] : 0);
    return static_cast<uint16_t>(askedOnceByLane(MadeOnDemand::ResultOfOperator, words, [&]() -> uint32_t {
        return m_stateOfLinkOperators->resultOf(operation.opcode, operands.span());
    }));
}

static bool mayGiveCountsInLinkOperators(const SummaryOperator& operation)
{
    static constexpr OpcodeSet countingOperators { Opcode::ToNumeric, Opcode::Mul, Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::RShift, Opcode::URShift };
    return countingOperators.contains(operation.opcode) || (operation.opcode == Opcode::Mod && operation.writtenDivisor != SummaryOperator::WrittenDivisor::None);
}

static LinkType::Counts countsThatOperatorGivesInLinkOperators(const SummaryOperator& operation, std::span<const LinkType::Counts, SummaryOperator::mostOperands> counts)
{
    using Counts = LinkType::Counts;
    if (operation.opcode == Opcode::Sub && counts[0] == Counts::BelowTwoTo31 && counts[1] == Counts::BelowTwoTo31)
        return Counts::Int32;
    if (operation.step < 0 && counts[operation.steppedOperand] == Counts::BelowTwoTo31)
        return Counts::Int32;
    if (operation.step && LinkType::areExact(counts[operation.steppedOperand]))
        return operation.step > 0 && LinkType::startsAtZero(counts[operation.steppedOperand]) ? Counts::ExactFromZeroUp : Counts::Exact;
    switch (operation.opcode) {
    case Opcode::ToNumeric:
        return counts[0];
    case Opcode::URShift:
        return operation.shiftsRightByConstant || counts[0] == Counts::BelowTwoTo31 ? Counts::BelowTwoTo31 : Counts::BelowTwoTo32;
    case Opcode::BitAnd:
        return std::min(counts[0], counts[1]) == Counts::BelowTwoTo31 ? Counts::BelowTwoTo31 : Counts::Unknown;
    case Opcode::BitOr:
    case Opcode::BitXor:
        return std::max(counts[0], counts[1]) == Counts::BelowTwoTo31 ? Counts::BelowTwoTo31 : Counts::Unknown;
    case Opcode::RShift:
        return counts[0] == Counts::BelowTwoTo31 ? Counts::BelowTwoTo31 : Counts::Unknown;
    default:
        break;
    }
    if (operation.opcode == Opcode::Add || operation.opcode == Opcode::Sub) {
        bool giveFromZeroUp = operation.opcode == Opcode::Add && LinkType::startsAtZero(counts[0]) && LinkType::startsAtZero(counts[1]);
        if (LinkType::areBelowTwoTo32(counts[0]) && LinkType::areBelowTwoTo32(counts[1]))
            return giveFromZeroUp ? Counts::ExactFromZeroUp : Counts::Exact;
        return giveFromZeroUp && std::min(counts[0], counts[1]) <= Counts::BelowTwoTo32 ? Counts::WholeFromZeroUp : Counts::Unknown;
    }
    if (!LinkType::startsAtZero(counts[0]))
        return Counts::Unknown;
    switch (operation.opcode) {
    case Opcode::Mul:
        return std::max(counts[0], counts[1]) <= Counts::BelowTwoTo32 ? Counts::WholeFromZeroUp : Counts::Unknown;
    case Opcode::Mod:
        return operation.writtenDivisor == SummaryOperator::WrittenDivisor::FromOneBelowTwoTo31 ? Counts::BelowTwoTo31 : Counts::BelowTwoTo32;
    default:
        return Counts::Unknown;
    }
}

static bool makesEqualityConvertObjects(const LinkType& type)
{
    constexpr uint16_t primitives = LinkType::closed(Primitive::All) | Primitive::LiteralSymbol;
    return type.primitives & primitives & ~Primitive::UndefinedOrNull;
}

void LinkFlow::operatorObserved(const FlowObserver& observer, const LinkType& now)
{
    if (const SummaryOperator& compared = summaries.at(observer.function).operators[observer.record]; compared.isConversionByEquality()) {
        if (!makesEqualityConvertObjects(now) || makesEqualityConvertObjects(typeObservedBefore()))
            return;
        const ThinSummary& summary = summaries.at(observer.function);
        for (const SummaryEdge& edge : std::ranges::equal_range(summary.edges.span(), compared.result, { }, &SummaryEdge::to))
            addEdge(nodeOf(observer.function, edge.from), nodeOf(observer.function, edge.to), edge.passes);
        return;
    }
    bool isKeyConversion = flowRuleOf(summaries.at(observer.function).operators[observer.record].opcode) == FlowRule::ToPropertyKey;
    auto widenLiterals = [&](uint16_t primitives) -> uint16_t {
        return primitives & LinkType::literalString && !isKeyConversion ? primitives | Primitive::String : primitives;
    };
    if (!typeObservedBefore().isNever() && widenLiterals(typeObservedBefore().primitives) == widenLiterals(now.primitives) && holdsObjectsForLinkOperators(typeObservedBefore()) == holdsObjectsForLinkOperators(now) && typeObservedBefore().hasSameShadowsAs(now))
        return;

    const SummaryOperator& operation = summaries.at(observer.function).operators[observer.record];
    bool someCameByEscapeAlone = false;
    auto answer = [&](bool ofWhatWasSeen) -> LinkType {
        auto held = [&](uint32_t node) {
            const LinkType& type = typeAt(nodeOf(observer.function, node));
            someCameByEscapeAlone |= type.escapeOnlyPrimitives || type.isUnknownEscapeOnly;
            return ofWhatWasSeen ? type.directPart() : type;
        };
        if (flowRuleOf(operation.opcode) == FlowRule::ToPropertyKey) {
            constexpr uint16_t handedOn = LinkType::closed(Primitive::AnyString | Primitive::AnySymbol);
            uint16_t primitives = held(operation.operands[0]).primitives;
            if (operation.converted[0] != noSummaryNode) {
                uint16_t answered = held(operation.converted[0]).primitives;
                if (answered & LinkType::oneSymbol)
                    answered |= Primitive::Symbol;
                primitives |= answered;
            } else if (holdsObjectsForLinkOperators(held(operation.operands[0])))
                primitives |= LinkType::anything().primitives;
            LinkType key;
            key.primitives = primitives & handedOn;
            if (primitives & LinkType::closed(Primitive::All) & ~handedOn)
                key.primitives |= LinkType::literalString;
            return key;
        }
        if (operation.opcode == Opcode::ToThis || operation.opcode == Opcode::ToObject) {
            uint16_t given = held(operation.operands[0]).primitives & LinkType::closed(Primitive::All);
            LinkType converted;
            if (operation.opcode == Opcode::ToThis && given & Primitive::UndefinedOrNull)
                converted.builtin = program.builtins().canonicalID(Builtins::GlobalThis);
            converted.hasUnknownObjects = given & ~Primitive::UndefinedOrNull;
            return converted;
        }
        Vector<uint16_t, SummaryOperator::mostOperands> operands;
        std::array<LinkType::Counts, SummaryOperator::mostOperands> counts { LinkType::Counts::Unknown, LinkType::Counts::Unknown };
        static_assert(SummaryOperator::mostOperands == 2);
        bool asksForCounts = !ofWhatWasSeen && mayGiveCountsInLinkOperators(operation);
        for (uint32_t index = 0; index < SummaryOperator::mostOperands && operation.operands[index] != noSummaryNode; ++index) {
            LinkType operand = held(operation.operands[index]);
            uint16_t primitives = operand.primitives;
            if (operation.converted[index] != noSummaryNode)
                primitives |= held(operation.converted[index]).primitives;
            else if (holdsObjectsForLinkOperators(operand))
                primitives |= LinkType::anything().primitives;
            primitives &= LinkType::closed(Primitive::All);
            if (!primitives)
                return { };
            operands.append(primitives);
            if (asksForCounts && !(primitives & ~operand.primitives) && !(primitives & ~LinkType::closed(Primitive::Number)) && !holdsObjectsForLinkOperators(operand))
                counts[index] = primitives & Primitive::Number ? operand.counts : LinkType::startsAtZero(operand.counts) ? LinkType::Counts::BelowTwoTo31 : LinkType::Counts::Int32;
        }

        prepareOperatorsForSeveralThreads();
        LinkType result;
        uint64_t words = static_cast<uint64_t>(operation.opcode) << 40 | static_cast<uint64_t>(operands.size()) << 32 | static_cast<uint64_t>(operands[0]) << 16 | (operands.size() > 1 ? operands[1] : 0);
        result.primitives = static_cast<uint16_t>(askedOnceByLane(MadeOnDemand::ResultOfOperator, words, [&]() -> uint32_t {
            return m_stateOfLinkOperators->resultOf(operation.opcode, operands.span());
        }));
        if (asksForCounts) {
            result.counts = countsThatOperatorGivesInLinkOperators(operation, counts);
            if (result.counts == LinkType::Counts::BelowTwoTo31 || result.counts == LinkType::Counts::Int32)
                result.primitives = LinkType::int32;
            else if (result.counts == LinkType::Counts::BelowTwoTo32)
                result.primitives = LinkType::closed(Primitive::Integer);
        }
        if (result.counts == LinkType::Counts::Unknown && result.hasNumbers() && !(result.primitives & Primitive::Number))
            result.counts = LinkType::Counts::Int32;
        return result;
    };
    LinkType result = answer(false);
    if (result.isNever())
        return;
    LinkType seen = someCameByEscapeAlone ? answer(true) : result;
    result.escapeOnlyPrimitives = result.primitives & ~seen.primitives;
    result.isUnknownEscapeOnly = result.hasUnknownObjects && !seen.hasUnknownObjects;
    arrive(nodeOf(observer.function, operation.result), result, flowRuleOf(operation.opcode) == FlowRule::ToPropertyKey ? symbolSeenAt(nodeOf(observer.function, operation.operands[0])) : PropertyKeyID());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

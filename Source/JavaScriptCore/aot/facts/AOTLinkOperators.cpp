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
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.holdsUnseen;
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

void LinkFlow::operatorObserved(const FlowObserver& observer, const LinkType& now)
{
    if (!typeObservedBefore().isNothing() && typeObservedBefore().primitives == now.primitives && holdsObjectsForLinkOperators(typeObservedBefore()) == holdsObjectsForLinkOperators(now) && typeObservedBefore().hasSameShadowsAs(now))
        return;

    const SummaryOperator& operation = summaries.at(observer.function).operators[observer.record];
    auto answer = [&](bool ofWhatWasSeen) -> LinkType {
        auto held = [&](uint32_t node) {
            const LinkType& type = typeAt(nodeOf(observer.function, node));
            return ofWhatWasSeen ? type.asSeen() : type;
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
                key.primitives |= Primitive::String;
            return key;
        }
        if (operation.opcode == Opcode::ToThis || operation.opcode == Opcode::ToObject) {
            uint16_t given = held(operation.operands[0]).primitives & LinkType::closed(Primitive::All);
            LinkType converted;
            if (operation.opcode == Opcode::ToThis && given & Primitive::UndefinedOrNull)
                converted.builtin = program.builtins().canonicalID(Builtins::GlobalThis);
            converted.holdsUnseen = given & ~Primitive::UndefinedOrNull;
            return converted;
        }
        Vector<uint16_t, SummaryOperator::mostOperands> operands;
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
        }

        prepareOperatorsForSeveralThreads();
        LinkType result;
        uint64_t words = static_cast<uint64_t>(operation.opcode) << 40 | static_cast<uint64_t>(operands.size()) << 32 | static_cast<uint64_t>(operands[0]) << 16 | (operands.size() > 1 ? operands[1] : 0);
        result.primitives = static_cast<uint16_t>(askedOnceByLane(MadeOnDemand::ResultOfOperator, words, [&]() -> uint32_t {
            return m_stateOfLinkOperators->resultOf(operation.opcode, operands.span());
        }));
        return result;
    };
    LinkType result = answer(false);
    if (result.isNothing())
        return;
    LinkType seen = answer(true);
    result.primitivesByEscapeAlone = result.primitives & ~seen.primitives;
    result.unseenIsByEscapeAlone = result.holdsUnseen && !seen.holdsUnseen;
    arrive(nodeOf(observer.function, operation.result), result, flowRuleOf(operation.opcode) == FlowRule::ToPropertyKey ? symbolSeenAt(nodeOf(observer.function, operation.operands[0])) : PropertyKeyID());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

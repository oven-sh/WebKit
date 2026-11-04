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

// WHAT AN OPERATOR GIVES HANGS ON WHAT ARRIVES AT ITS OPERANDS, and one function says how: resultTypeOfOperator(), which types the operation for the walk too.
// It is asked with the primitives that have arrived, and what it answers arrives at the result. More at an operand is another question, and its answer is joined in: nothing is taken back.
// An object among an operand is converted first. That is a call by name for each key of the protocol, which the scan has left, and the operator goes on with the primitives that those calls answer.

static bool holdsObjectsForLinkOperators(const LinkType& type)
{
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.holdsUnseen;
}

class StateOfLinkOperators {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkOperators);
    WTF_MAKE_NONCOPYABLE(StateOfLinkOperators);
public:
    explicit StateOfLinkOperators(Program&);

    // Both in the bits of LinkType::primitives.
    uint16_t resultOf(Opcode, std::span<const uint16_t> operands);

private:
    TypeArena m_types;
    TypingContext m_typing;
    // By the opcode above the primitives of the operands. A program has a few hundred of those, however many operators it has.
    HashMap<uint64_t, uint16_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_results;
};

StateOfLinkOperators::StateOfLinkOperators(Program& program)
    // The flow adds nothing to the program's table of types. A range that the table lacks lies here, and of an answer the primitives alone are taken.
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
    // Which objects they are is for the calls of the conversion.
    if (!typeObservedBefore().isNothing() && typeObservedBefore().primitives == now.primitives && holdsObjectsForLinkOperators(typeObservedBefore()) == holdsObjectsForLinkOperators(now) && typeObservedBefore().hasSameShadowsAs(now))
        return;

    const SummaryOperator& operation = summaries.at(observer.function).operators[observer.record];
    // Of what the operands hold, or of what was seen of them. Nothing: there is no answer yet.
    auto answer = [&](bool ofWhatWasSeen) -> LinkType {
        auto held = [&](uint32_t node) {
            const LinkType& type = typeAt(nodeOf(observer.function, node));
            return ofWhatWasSeen ? type.asSeen() : type;
        };
        // A conversion to a key hands a text and a symbol on as they are, the one symbol under its name, and makes a text of any other primitive.
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
        // In the place of undefined and null stands the global object, or the conversion throws. In that of any other primitive stands a wrapper that is made there.
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
            // An operand is a value of JavaScript: the mark of an empty cell never comes this far.
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

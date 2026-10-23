/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnspecifiedValuePaths.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLiveness.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Fact {
    ValueID value;
    bool isUnspecified { false };
    int64_t integer { 0 };

    friend bool operator==(const Fact&, const Fact&) = default;
};

using Facts = Vector<Fact, 8>;

struct State {
    BlockID block;
    Facts facts;
    std::optional<size_t> previous;
    uint32_t firstValue { 0 };
};

// What a branch tells about its condition multiplies the states, so it is remembered only to refute what was found without it.
enum class BranchesTeach : bool { No, Yes };

class PathWalker {
public:
    PathWalker(FunctionAnalyses& analyses, size_t maxUses, BranchesTeach branchesTeach)
        : m_analyses(analyses)
        , m_function(analyses.function())
        , m_visited(m_function.blocks.size())
        , m_maxUses(maxUses)
        , m_branchesTeach(branchesTeach == BranchesTeach::Yes)
    {
    }

    UsesOfUnspecifiedValues run();

private:
    static constexpr size_t maxStates = 1 << 16;

    std::optional<Fact> factOf(const Facts&, ValueID) const;
    std::optional<int64_t> integerOf(const Facts&, ValueID, unsigned depth = 0) const;
    std::optional<int64_t> numberOf(const Facts&, ValueID, unsigned depth = 0) const;
    bool isNumber(ValueID) const;
    bool isUnspecified(const Facts&, ValueID) const;
    static void set(Facts&, Fact);

    template<typename User> void listUsesBy(const Facts&, BlockID, ValueID id, const User&);
    void evaluate(Facts&, ValueID);
    std::optional<int64_t> fold(const Facts&, const Value&, unsigned depth = 0) const;
    void walk(size_t stateIndex);
    void follow(const Facts&, Edge, std::optional<Fact> learned = std::nullopt);
    void enter(BlockID target, Facts&&);

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    Vector<Vector<Facts>> m_visited;
    Vector<State> m_states;
    Vector<size_t> m_worklist;
    size_t m_current { 0 };
    size_t m_maxUses;
    bool m_branchesTeach;
    bool m_gaveUp { false };
    Vector<UseOfUnspecifiedValue> m_uses;
};

std::optional<Fact> PathWalker::factOf(const Facts& facts, ValueID id) const
{
    const Value& value = m_function.at(id);
    if (value.opcode == Opcode::Unspecified)
        return Fact { id, true, 0 };
    if (value.opcode == Opcode::Integer)
        return Fact { id, false, value.integer() };
    if (value.opcode == Opcode::Boolean)
        return Fact { id, false, value.boolean() };
    for (const Fact& fact : facts) {
        if (fact.value == id)
            return fact;
    }
    return std::nullopt;
}

// What a branch has told about an operand since the value was computed counts too, so a value without a fact is computed again from its operands.
std::optional<int64_t> PathWalker::integerOf(const Facts& facts, ValueID id, unsigned depth) const
{
    static constexpr unsigned maxDepth = 8;
    if (std::optional<Fact> fact = factOf(facts, id))
        return fact->isUnspecified ? std::nullopt : std::optional<int64_t>(fact->integer);
    if (depth == maxDepth)
        return std::nullopt;
    return fold(facts, m_function.at(id), depth + 1);
}

// A known boolean is kept as 0 or 1, and is no number to a comparison or a type test.
bool PathWalker::isNumber(ValueID id) const
{
    const Program& program = m_analyses.context().program;
    return program.typeOf(m_function, id).isSubsetOf(Type::fromPrimitives(Primitive::Number), program);
}

std::optional<int64_t> PathWalker::numberOf(const Facts& facts, ValueID id, unsigned depth) const
{
    return isNumber(id) ? integerOf(facts, id, depth) : std::nullopt;
}

bool PathWalker::isUnspecified(const Facts& facts, ValueID id) const
{
    std::optional<Fact> fact = factOf(facts, id);
    return fact && fact->isUnspecified;
}

void PathWalker::set(Facts& facts, Fact added)
{
    for (Fact& fact : facts) {
        if (fact.value == added.value) {
            fact = added;
            return;
        }
    }
    facts.append(added);
}

template<typename User>
void PathWalker::listUsesBy(const Facts& facts, BlockID block, ValueID id, const User& user)
{
    static constexpr OperandUses usesThatLookAtNothing { OperandUse::Returns, OperandUse::Retains };
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (user.operandKind(operandIndex) == OperandKind::Guard || !isUnspecified(facts, operand))
            return;
        if (user.operandUses(operandIndex).containsOnly(usesThatLookAtNothing))
            return;
        bool isListed = std::ranges::any_of(m_uses, [&](const UseOfUnspecifiedValue& use) {
            return use.block == block && use.user == id && use.operand == operand;
        });
        if (isListed)
            return;
        UseOfUnspecifiedValue use { block, id, operand, { } };
        for (std::optional<size_t> index = m_current; index; index = m_states[*index].previous)
            use.path.append(m_states[*index].block);
        use.path.reverse();
        m_uses.append(WTF::move(use));
    });
}

std::optional<int64_t> PathWalker::fold(const Facts& facts, const Value& value, unsigned depth) const
{
    auto both = [&](auto&& operation) -> std::optional<int64_t> {
        std::optional<int64_t> left = numberOf(facts, value.operand(0), depth);
        std::optional<int64_t> right = numberOf(facts, value.operand(1), depth);
        if (!left || !right)
            return std::nullopt;
        return operation(*left, *right);
    };
    switch (value.opcode) {
    case Opcode::BitAnd:
        return both([](int64_t left, int64_t right) { return left & right; });
    case Opcode::BitOr:
        return both([](int64_t left, int64_t right) { return left | right; });
    case Opcode::LShift:
        return both([](int64_t left, int64_t right) -> std::optional<int64_t> {
            if (left < 0 || left > INT32_MAX || right < 0 || right > 30 || left << right > INT32_MAX)
                return std::nullopt;
            return left << right;
        });
    case Opcode::ToNumber:
    case Opcode::Upcast:
    case Opcode::AssumeType:
        return integerOf(facts, value.operand(0), depth);
    case Opcode::StrictEq:
    case Opcode::Eq:
        return both([](int64_t left, int64_t right) { return left == right; });
    case Opcode::Less:
        return both([](int64_t left, int64_t right) { return left < right; });
    case Opcode::LessEq:
        return both([](int64_t left, int64_t right) { return left <= right; });
    case Opcode::Greater:
        return both([](int64_t left, int64_t right) { return left > right; });
    case Opcode::GreaterEq:
        return both([](int64_t left, int64_t right) { return left >= right; });
    case Opcode::ToBoolean:
        if (std::optional<int64_t> operand = integerOf(facts, value.operand(0), depth))
            return !!*operand;
        return std::nullopt;
    case Opcode::LogicalNot:
        if (std::optional<int64_t> operand = integerOf(facts, value.operand(0), depth))
            return !*operand;
        return std::nullopt;
    case Opcode::TypeTest:
        if (std::optional<int64_t> operand = numberOf(facts, value.operand(0), depth)) {
            const Program& program = m_analyses.context().program;
            return Type::integer({ *operand, *operand }).isSubsetOf(program.at(value.testedType()), program);
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

void PathWalker::evaluate(Facts& facts, ValueID id)
{
    const Value& value = m_function.at(id);
    if (value.opcode == Opcode::Unspecified) {
        set(facts, Fact { id, true, 0 });
        return;
    }
    if (std::optional<int64_t> integer = fold(facts, value)) {
        set(facts, Fact { id, false, *integer });
        return;
    }
    std::optional<Fact> returned;
    forEachReturnedOperand(m_function, value, [&](ValueID operand) {
        if (std::optional<Fact> fact = factOf(facts, operand))
            returned = fact;
    });
    if (!returned) {
        facts.removeAllMatching([&](const Fact& fact) {
            return fact.value == id;
        });
        return;
    }
    returned->value = id;
    set(facts, *returned);
}

void PathWalker::enter(BlockID target, Facts&& facts)
{
    const Liveness& liveness = m_analyses.liveness();
    const Block& block = m_function.at(target);
    facts.removeAllMatching([&](const Fact& fact) {
        return !block.parameters.contains(fact.value) && !liveness.isLiveAtHead(fact.value, target);
    });
    std::ranges::sort(facts, { }, [](const Fact& fact) {
        return fact.value.index();
    });
    Vector<Facts>& visited = m_visited[target.index()];
    if (visited.contains(facts))
        return;
    if (m_states.size() == maxStates) {
        m_gaveUp = true;
        return;
    }
    visited.append(facts);
    m_worklist.append(m_states.size());
    m_states.append(State { target, WTF::move(facts), m_current, 0 });
}

void PathWalker::follow(const Facts& facts, Edge edge, std::optional<Fact> learned)
{
    const Target& target = targetOf(m_function, edge);
    Facts atTarget = facts;
    if (learned)
        set(atTarget, *learned);
    const Block& block = m_function.at(target.block);
    Vector<std::optional<Fact>, 8> passed;
    for (ValueID argument : target.arguments)
        passed.append(factOf(atTarget, argument));
    if (!edge.isHandlerEdge()) {
        uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
        for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
            std::optional<EdgeResult> result = tryGetEdgeResult(m_function, { edge.from, edge.targetIndex, resultIndex });
            bool isSameValue = result && result->kind == EdgeResultKind::NarrowedOperand && result->subject;
            passed.append(isSameValue ? factOf(atTarget, result->subject) : std::nullopt);
        }
    }
    for (ValueID parameter : block.parameters) {
        atTarget.removeAllMatching([&](const Fact& fact) {
            return fact.value == parameter;
        });
    }
    for (size_t index = 0; index < passed.size() && index < block.parameters.size(); ++index) {
        if (!passed[index])
            continue;
        passed[index]->value = block.parameters[index];
        atTarget.append(*passed[index]);
    }
    enter(target.block, WTF::move(atTarget));
}

void PathWalker::walk(size_t stateIndex)
{
    m_current = stateIndex;
    State state { m_states[stateIndex].block, m_states[stateIndex].facts, { }, m_states[stateIndex].firstValue };
    const Block& block = m_function.at(state.block);
    Facts facts = state.facts;
    if (block.handler && !state.firstValue)
        follow(facts, Edge::toHandlerOf(state.block));
    for (uint32_t index = state.firstValue; index < block.values.size(); ++index) {
        ValueID id = block.values[index];
        const Value& value = m_function.at(id);
        // A tag may be computed from a boolean that no branch tells: the rest is followed once for each of its values.
        if (m_branchesTeach && value.opcode == Opcode::ToNumber && !isNumber(value.operand(0)) && !factOf(facts, value.operand(0)) && !integerOf(facts, value.operand(0))) {
            if (m_states.size() == maxStates) {
                m_gaveUp = true;
                return;
            }
            Facts whenFalse = facts;
            set(whenFalse, Fact { value.operand(0), false, 0 });
            m_worklist.append(m_states.size());
            m_states.append(State { state.block, WTF::move(whenFalse), stateIndex, index });
            set(facts, Fact { value.operand(0), false, 1 });
        }
        listUsesBy(facts, state.block, id, value);
        evaluate(facts, id);
    }

    const Terminator& terminator = block.terminator;
    listUsesBy(facts, state.block, { }, terminator);
    switch (terminator.kind) {
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
        if (std::optional<int64_t> condition = integerOf(facts, terminator.conditionOperand())) {
            follow(facts, Edge { state.block, *condition ? 0u : 1u });
            return;
        }
        if (!m_branchesTeach)
            break;
        if (const Value& test = m_function.at(terminator.conditionOperand()); test.opcode == Opcode::StrictEq) {
            std::optional<int64_t> left = numberOf(facts, test.operand(0));
            std::optional<int64_t> right = numberOf(facts, test.operand(1));
            if (!!left != !!right && isNumber(test.operand(left ? 1 : 0))) {
                Facts whenEqual = facts;
                set(whenEqual, Fact { test.operand(left ? 1 : 0), false, left ? *left : *right });
                follow(whenEqual, Edge { state.block, 0 }, Fact { terminator.conditionOperand(), false, 1 });
                follow(facts, Edge { state.block, 1 }, Fact { terminator.conditionOperand(), false, 0 });
                return;
            }
        }
        follow(facts, Edge { state.block, 0 }, Fact { terminator.conditionOperand(), false, 1 });
        follow(facts, Edge { state.block, 1 }, Fact { terminator.conditionOperand(), false, 0 });
        return;
    case TerminatorKind::Switch: {
        ValueID subject = terminator.subjectOperand();
        std::optional<int64_t> known = integerOf(facts, subject);
        uint32_t numberOfCases = terminator.numberOfCases();
        bool isCase = false;
        for (uint32_t index = 0; index < numberOfCases; ++index) {
            int64_t caseValue = terminator.cases()[index];
            if (known && *known != caseValue)
                continue;
            isCase = true;
            follow(facts, Edge { state.block, index }, Fact { subject, false, caseValue });
        }
        if (!known || !isCase)
            follow(facts, Edge { state.block, numberOfCases });
        return;
    }
    default:
        break;
    }
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex)
        follow(facts, Edge { state.block, targetIndex });
}

UsesOfUnspecifiedValues PathWalker::run()
{
    m_worklist.append(0);
    m_states.append(State { entryBlock, { }, std::nullopt, 0 });
    while (!m_worklist.isEmpty() && !m_gaveUp && m_uses.size() < m_maxUses)
        walk(m_worklist.takeLast());
    return { WTF::move(m_uses), m_worklist.isEmpty() && !m_gaveUp };
}

} // anonymous namespace

UsesOfUnspecifiedValues findUsesOfUnspecifiedValues(FunctionAnalyses& analyses, size_t maxUses)
{
    if (!analyses.opcodesOfValues().contains(Opcode::Unspecified))
        return { };
    UsesOfUnspecifiedValues found = PathWalker(analyses, maxUses, BranchesTeach::No).run();
    if (found.uses.isEmpty())
        return found;
    UsesOfUnspecifiedValues left = PathWalker(analyses, maxUses, BranchesTeach::Yes).run();
    return left.wasEveryPathFollowed || !left.uses.isEmpty() ? left : found;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

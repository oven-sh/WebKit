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
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class Integers {
public:
    static constexpr size_t capacity = 8;

    Integers() = default;
    Integers(int64_t only)
        : m_size(1)
    {
        m_sorted[0] = only;
    }

    static Integers bothBooleans()
    {
        Integers both(0);
        both.add(1);
        return both;
    }

    std::span<const int64_t> span() const { return std::span(m_sorted).first(m_size); }
    size_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    bool contains(int64_t integer) const { return std::ranges::binary_search(span(), integer); }
    bool isOnly(int64_t integer) const { return m_size == 1 && m_sorted[0] == integer; }

    bool add(int64_t integer)
    {
        std::span<const int64_t> sorted = span();
        size_t index = std::ranges::lower_bound(sorted, integer) - sorted.begin();
        if (index < m_size && m_sorted[index] == integer)
            return true;
        if (m_size == capacity)
            return false;
        std::copy_backward(m_sorted.begin() + index, m_sorted.begin() + m_size, m_sorted.begin() + m_size + 1);
        m_sorted[index] = integer;
        ++m_size;
        return true;
    }

    Integers intersectedWith(const Integers& other) const
    {
        return filtered([&](int64_t integer) { return other.contains(integer); });
    }

    Integers filtered(NOESCAPE const Invocable<bool(int64_t)> auto& keeps) const
    {
        Integers kept;
        for (int64_t integer : span()) {
            if (keeps(integer))
                kept.m_sorted[kept.m_size++] = integer;
        }
        return kept;
    }

    std::optional<Integers> unitedWith(const Integers& other) const
    {
        Integers united = *this;
        for (int64_t integer : other.span()) {
            if (!united.add(integer))
                return std::nullopt;
        }
        return united;
    }

    friend bool operator==(const Integers& left, const Integers& right) { return std::ranges::equal(left.span(), right.span()); }

private:
    std::array<int64_t, capacity> m_sorted { };
    uint8_t m_size { 0 };
};

struct Fact {
    ValueID value;
    TypeID testedType;
    Integers integers;

    bool isBefore(ValueID otherValue, TypeID otherType) const
    {
        if (value != otherValue)
            return value.index() < otherValue.index();
        return rankOf(testedType) < rankOf(otherType);
    }

    static uint64_t rankOf(TypeID type) { return type ? static_cast<uint64_t>(type.index()) + 1 : 0; }
};

class Facts {
public:
    const Integers* find(ValueID value, TypeID testedType = { }) const
    {
        size_t index = indexOf(value, testedType);
        return isAt(index, value, testedType) ? &m_sorted[index].integers : nullptr;
    }

    void set(ValueID value, TypeID testedType, const Integers& integers)
    {
        size_t index = indexOf(value, testedType);
        if (isAt(index, value, testedType))
            m_sorted[index].integers = integers;
        else
            m_sorted.insert(index, Fact { value, testedType, integers });
    }

    void remove(ValueID value)
    {
        size_t index = indexOf(value, { });
        if (isAt(index, value, { }))
            m_sorted.removeAt(index);
    }

    void removeAllMatching(NOESCAPE const Invocable<bool(const Fact&)> auto& matches) { m_sorted.removeAllMatching(matches); }

    void forEachTestOf(ValueID value, NOESCAPE const Invocable<void(TypeID, const Integers& outcome)> auto& functor) const
    {
        for (size_t index = indexOf(value, { }); index < m_sorted.size() && m_sorted[index].value == value; ++index) {
            if (m_sorted[index].testedType)
                functor(m_sorted[index].testedType, m_sorted[index].integers);
        }
    }

    bool mergeWith(const Facts& other)
    {
        bool changed = false;
        size_t otherIndex = 0;
        m_sorted.removeAllMatching([&](Fact& fact) {
            while (otherIndex < other.m_sorted.size() && other.m_sorted[otherIndex].isBefore(fact.value, fact.testedType))
                ++otherIndex;
            std::optional<Integers> united;
            if (other.isAt(otherIndex, fact.value, fact.testedType))
                united = fact.integers.unitedWith(other.m_sorted[otherIndex].integers);
            if (united && *united == fact.integers)
                return false;
            changed = true;
            if (!united || fact.testedType)
                return true;
            fact.integers = *united;
            return false;
        });
        return changed;
    }

    size_t size() const { return m_sorted.size(); }
    const Fact& operator[](size_t index) const { return m_sorted[index]; }

private:
    size_t indexOf(ValueID value, TypeID testedType) const
    {
        return std::ranges::partition_point(m_sorted, [&](const Fact& fact) { return fact.isBefore(value, testedType); }) - m_sorted.begin();
    }

    bool isAt(size_t index, ValueID value, TypeID testedType) const
    {
        return index < m_sorted.size() && m_sorted[index].value == value && m_sorted[index].testedType == testedType;
    }

    Vector<Fact, 8> m_sorted;
};

struct RemainingKinds {
    uint16_t primitives { 0 };
    bool mayBeObject { false };
    bool isObjectOfBuiltinClassesOnly { false };
    const Type* type { nullptr };

    bool mayPass(const Type& tested) const { return (mayBeObject && tested.hasObjects()) || primitives & primitivesThatMayPass(tested); }
    bool mayFail(const Type& tested, const Program& program) const
    {
        if (primitives & ~primitivesThatAlwaysPass(tested))
            return true;
        return mayBeObject && !(isObjectOfBuiltinClassesOnly && type->withOnlyObjects().isSubsetOf(tested, program));
    }
    bool isUndefinedOrNull() const { return !mayBeObject && !(primitives & ~Primitive::UndefinedOrNull); }

    static uint16_t primitivesThatMayPass(const Type& tested)
    {
        uint16_t primitives = tested.primitives;
        for (uint16_t kinds : { Primitive::Number, Primitive::AnyString, Primitive::AnySymbol }) {
            if (primitives & kinds)
                primitives |= kinds;
        }
        return primitives;
    }

    static uint16_t primitivesThatAlwaysPass(const Type& tested)
    {
        uint16_t primitives = tested.primitives & ~(Primitive::LiteralString | Primitive::LiteralSymbol);
        if ((primitives & Primitive::Number) != Primitive::Number || tested.integers != Range::full())
            primitives &= ~Primitive::Number;
        if (primitives & Primitive::String)
            primitives |= Primitive::LiteralString;
        if (primitives & Primitive::Symbol)
            primitives |= Primitive::LiteralSymbol;
        return primitives;
    }
};

class Folder {
public:
    Folder(const Program& program, const Function& function, FunctionAnalyses* analyses)
        : m_program(program)
        , m_function(function)
        , m_analyses(analyses)
    {
    }

    bool isNumber(ValueID id) const { return m_program.typeOf(m_function, id).isNumber(); }
    static bool canFold(Opcode);

    std::optional<Integers> integersOf(const Facts& facts, ValueID id, unsigned depth = 0) const
    {
        if (!id)
            return std::nullopt;
        const Value& value = m_function.at(id);
        if (value.opcode == Opcode::Integer)
            return Integers(value.integer());
        if (value.opcode == Opcode::Boolean)
            return Integers(value.boolean());
        if (value.opcode == Opcode::Unspecified)
            return Integers();
        if (const Integers* known = facts.find(id))
            return *known;
        if (depth < maxDepth) {
            if (std::optional<Integers> folded = fold(facts, value, depth + 1))
                return folded;
        }
        if (m_program.typeOf(m_function, id).isBoolean())
            return Integers::bothBooleans();
        return std::nullopt;
    }

    std::optional<RemainingKinds> remainingKindsOf(const Facts& facts, ValueID id) const
    {
        if (!m_analyses)
            return std::nullopt;
        ValueID identity = m_analyses->identityOf(id);
        const Type& type = m_program.typeOf(m_function, id);
        if (!type.isJavaScriptValue())
            return std::nullopt;
        bool hasBuiltinClassesOnly = !type.anyObject && !type.anyFunction && !type.layouts && type.functions.isEmpty() && type.builtins.isEmpty();
        RemainingKinds remaining { type.primitives, type.hasObjects(), hasBuiltinClassesOnly, &type };
        bool isTested = false;
        facts.forEachTestOf(identity, [&](TypeID testedType, const Integers& outcome) {
            const Type& tested = m_program.at(testedType);
            isTested = true;
            if (outcome.isOnly(0)) {
                remaining.primitives &= ~RemainingKinds::primitivesThatAlwaysPass(tested);
                return;
            }
            remaining.primitives &= RemainingKinds::primitivesThatMayPass(tested);
            remaining.mayBeObject = false;
        });
        return isTested ? std::optional(remaining) : std::nullopt;
    }

    std::optional<Integers> fold(const Facts& facts, const Value& value, unsigned depth = 0) const
    {
        auto both = [&](auto&& operation) -> std::optional<Integers> {
            std::optional<Integers> left = numbersOf(facts, value.operand(0), depth);
            std::optional<Integers> right = numbersOf(facts, value.operand(1), depth);
            if (!left || !right)
                return std::nullopt;
            Integers results;
            for (int64_t leftInteger : left->span()) {
                for (int64_t rightInteger : right->span()) {
                    std::optional<int64_t> result = operation(leftInteger, rightInteger);
                    if (!result || !results.add(*result))
                        return std::nullopt;
                }
            }
            return results;
        };
        auto each = [&](auto&& operation) -> std::optional<Integers> {
            std::optional<Integers> operand = integersOf(facts, value.operand(0), depth);
            if (!operand)
                return std::nullopt;
            Integers results;
            for (int64_t integer : operand->span())
                results.add(operation(integer));
            return results;
        };
        using Result = std::optional<int64_t>;
        switch (value.opcode) {
        case Opcode::BitAnd:
            return both([](int64_t left, int64_t right) -> Result { return left & right; });
        case Opcode::BitOr:
            return both([](int64_t left, int64_t right) -> Result { return left | right; });
        case Opcode::LShift:
            return both([](int64_t left, int64_t right) -> Result {
                if (left < 0 || left > INT32_MAX || right < 0 || right > 30 || left << right > INT32_MAX)
                    return std::nullopt;
                return left << right;
            });
        case Opcode::ToNumber:
            return integersOf(facts, value.operand(0), depth);
        case Opcode::StrictEq:
        case Opcode::Eq:
            return both([](int64_t left, int64_t right) -> Result { return left == right; });
        case Opcode::Less:
            return both([](int64_t left, int64_t right) -> Result { return left < right; });
        case Opcode::LessEq:
            return both([](int64_t left, int64_t right) -> Result { return left <= right; });
        case Opcode::Greater:
            return both([](int64_t left, int64_t right) -> Result { return left > right; });
        case Opcode::GreaterEq:
            return both([](int64_t left, int64_t right) -> Result { return left >= right; });
        case Opcode::ToBoolean:
            return each([](int64_t integer) { return !!integer; });
        case Opcode::LogicalNot:
            return each([](int64_t integer) { return !integer; });
        case Opcode::TypeTest:
            if (std::optional<Integers> operand = numbersOf(facts, value.operand(0), depth)) {
                Integers results;
                for (int64_t integer : operand->span())
                    results.add(Type::integer({ integer, integer }).isSubsetOf(m_program.at(value.testedType()), m_program));
                return results;
            }
            if (m_analyses) {
                if (const Integers* outcome = facts.find(m_analyses->identityOf(value.operand(0)), value.testedType()))
                    return *outcome;
            }
            if (std::optional<RemainingKinds> remaining = remainingKindsOf(facts, value.operand(0))) {
                const Type& tested = m_program.at(value.testedType());
                if (remaining->mayPass(tested) != remaining->mayFail(tested, m_program))
                    return Integers(remaining->mayPass(tested));
            }
            return std::nullopt;
        case Opcode::CallDirect: {
            const Function& callee = m_program.at(value.function());
            if (callee.role != FunctionRole::OutlinedBody || callee.blocks.size() != 1)
                return std::nullopt;
            const Block& body = callee.at(entryBlock);
            if (body.terminator.kind != TerminatorKind::Return || !body.terminator.numberOfOperands() || body.parameters.size() != value.numberOfOperands())
                return std::nullopt;
            Facts inCallee;
            for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
                if (std::optional<Integers> argument = integersOf(facts, value.operand(index), depth))
                    inCallee.set(body.parameters[index], { }, *argument);
            }
            return Folder(m_program, callee, nullptr).integersOf(inCallee, body.terminator.operand(0));
        }
        default: {
            std::optional<Integers> returned;
            forEachReturnedOperand(m_function, value, [&](ValueID operand) { returned = integersOf(facts, operand, depth); });
            return returned;
        }
        }
    }

    AOT_CLASS_THRESHOLD(unsigned, maxDepth, 8);

    std::optional<Integers> numbersOf(const Facts& facts, ValueID id, unsigned depth) const { return isNumber(id) ? integersOf(facts, id, depth) : std::nullopt; }

    const Program& m_program;
    const Function& m_function;
    FunctionAnalyses* m_analyses;
};

bool Folder::canFold(Opcode opcode)
{
    switch (opcode) {
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::LShift:
    case Opcode::ToNumber:
    case Opcode::StrictEq:
    case Opcode::Eq:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
    case Opcode::ToBoolean:
    case Opcode::LogicalNot:
    case Opcode::TypeTest:
    case Opcode::CallDirect:
        return true;
    default:
        return mayReturnOperand(opcode);
    }
}

using MarkedValues = Vector<ValueID, 4>;

struct WalkState {
    BlockID block;
    MarkedValues marked;
    Facts facts;
    std::optional<uint32_t> previous;
    bool isQueued { false };
};

class MergingWalker {
public:
    MergingWalker(FunctionAnalyses& analyses, size_t maxUses, size_t maxBlockVisits, const BitVector* countedAsLooking)
        : m_analyses(analyses)
        , m_function(analyses.function())
        , m_program(analyses.context().program)
        , m_folder(m_program, m_function, &analyses)
        , m_statesOfBlock(m_function.blocks.size())
        , m_maxUses(maxUses)
        , m_blockVisitsLeft(maxBlockVisits)
        , m_countedAsLooking(countedAsLooking)
    {
    }

    AOT_CLASS_THRESHOLD(unsigned, maxNarrowingDepth, 6);

    bool isAlwaysUnspecified(ValueID id) const { return m_isAlwaysUnspecified.get(id.index()); }
    bool narrow(Facts&, ValueID, Integers allowed, unsigned depth = 0) const;
    bool narrowOnEdge(Facts& facts, ValueID id, const Integers& allowed) const { return narrow(facts, id, allowed) && refoldNarrowedValues(facts); }

    bool refoldNarrowedValues(Facts& facts) const
    {
        AOT_THRESHOLD(unsigned, maxRounds, 4);
        for (unsigned round = 0; round < maxRounds; ++round) {
            bool changed = false;
            for (size_t index = 0; index < facts.size(); ++index) {
                Fact fact = facts[index];
                const Value& value = m_function.at(fact.value);
                if (fact.testedType || fact.integers.size() < 2 || !Folder::canFold(value.opcode))
                    continue;
                facts.remove(fact.value);
                std::optional<Integers> computed = m_folder.fold(facts, value);
                Integers narrowed = computed ? computed->intersectedWith(fact.integers) : fact.integers;
                if (narrowed.isEmpty())
                    return false;
                facts.set(fact.value, { }, narrowed);
                changed |= narrowed.size() < fact.integers.size();
            }
            if (!changed)
                break;
        }
        return true;
    }

    bool endsPath(const Facts& facts, const Value& value) const
    {
        if (value.opcode != Opcode::RequireObjectCoercible && value.opcode != Opcode::AssumeType)
            return false;
        std::optional<RemainingKinds> remaining = m_folder.remainingKindsOf(facts, value.operand(0));
        if (!remaining)
            return false;
        if (value.opcode == Opcode::RequireObjectCoercible)
            return remaining->isUndefinedOrNull();
        return !remaining->mayPass(m_program.at(value.castType()));
    }

    Vector<BlockID> pathTo(const Vector<WalkState>& states, std::optional<uint32_t> stateIndex) const
    {
        Vector<BlockID> path;
        for (std::optional<uint32_t> index = stateIndex; index; index = states[*index].previous)
            path.append(states[*index].block);
        path.reverse();
        return path;
    }

    void listUsesBy(const MarkedValues& marked, BlockID block, ValueID id, const auto& user)
    {
        static constexpr OperandUses usesThatLookAtNothing { OperandUse::Returns, OperandUse::Retains };
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            bool isUnspecified = m_origin ? marked.contains(operand) : isAlwaysUnspecified(operand);
            if (!isUnspecified || user.operandKind(operandIndex) == OperandKind::Guard)
                return;
            bool isCountedAsLooking = id && m_countedAsLooking && m_countedAsLooking->get(id.index());
            if (user.operandUses(operandIndex).containsOnly(usesThatLookAtNothing) && !isCountedAsLooking)
                return;
            bool isListed = std::ranges::any_of(m_uses, [&](const UseOfUnspecifiedValue& use) {
                return use.block == block && use.user == id && use.operand == operand;
            });
            if (isListed)
                return;
            UseOfUnspecifiedValue use { block, id, operand, pathTo(m_unmarkedStates, m_start) };
            use.path.appendVector(pathTo(m_states, m_current));
            m_uses.append(WTF::move(use));
        });
    }

    void walkBlock(BlockID blockID, Facts&& facts, MarkedValues&& marked, NOESCAPE const Invocable<void(Edge, Facts&&, const MarkedValues&)> auto& follow)
    {
        const Block& block = m_function.at(blockID);
        if (block.handler)
            follow(Edge::toHandlerOf(blockID), Facts(facts), marked);
        bool listsUses = m_origin || m_unmarkedStates.size();
        for (ValueID id : block.values) {
            const Value& value = m_function.at(id);
            if (listsUses)
                listUsesBy(marked, blockID, id, value);
            if (endsPath(facts, value))
                return;
            if (value.opcode == Opcode::RequireObjectCoercible)
                facts.set(m_analyses.identityOf(id), TypeTable::nullish, Integers(0));
            facts.remove(id);
            if (Folder::canFold(value.opcode)) {
                if (std::optional<Integers> integers = m_folder.fold(facts, value))
                    facts.set(id, { }, *integers);
            }
            if (marked.isEmpty() || !mayReturnOperand(value.opcode))
                continue;
            forEachReturnedOperand(m_function, value, [&](ValueID operand) {
                if (marked.contains(operand))
                    marked.appendIfNotContains(id);
            });
        }

        const Terminator& terminator = block.terminator;
        if (listsUses)
            listUsesBy(marked, blockID, { }, terminator);
        switch (terminator.kind) {
        case TerminatorKind::Branch:
        case TerminatorKind::TestAndBranch: {
            ValueID condition = terminator.conditionOperand();
            Integers outcomes = m_folder.integersOf(facts, condition).value_or(Integers::bothBooleans());
            for (uint32_t targetIndex = 0; targetIndex < 2; ++targetIndex) {
                Integers allowed = outcomes.filtered([&](int64_t outcome) { return !outcome == !!targetIndex; });
                Facts onEdge = facts;
                if (!allowed.isEmpty() && narrowOnEdge(onEdge, condition, allowed))
                    follow(Edge { blockID, targetIndex }, WTF::move(onEdge), marked);
            }
            return;
        }
        case TerminatorKind::Switch: {
            ValueID subject = terminator.subjectOperand();
            std::optional<Integers> known = m_folder.isNumber(subject) ? m_folder.integersOf(facts, subject) : std::nullopt;
            uint32_t numberOfCases = terminator.numberOfCases();
            for (uint32_t index = 0; index < numberOfCases; ++index) {
                int64_t caseValue = terminator.cases()[index];
                Facts onEdge = facts;
                if ((!known || known->contains(caseValue)) && narrowOnEdge(onEdge, subject, Integers(caseValue)))
                    follow(Edge { blockID, index }, WTF::move(onEdge), marked);
            }
            if (!known) {
                follow(Edge { blockID, numberOfCases }, WTF::move(facts), marked);
                return;
            }
            Integers others = known->filtered([&](int64_t integer) { return !std::ranges::contains(terminator.cases(), integer); });
            if (!others.isEmpty() && narrowOnEdge(facts, subject, others))
                follow(Edge { blockID, numberOfCases }, WTF::move(facts), marked);
            return;
        }
        case TerminatorKind::Jump:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
            break;
        }
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex)
            follow(Edge { blockID, targetIndex }, Facts(facts), marked);
    }

    void follow(Edge edge, const Facts& facts, const MarkedValues& marked)
    {
        const Target& target = targetOf(m_function, edge);
        const Block& block = m_function.at(target.block);
        bool isOrigin = m_origin && *m_origin == edge;

        struct Passed {
            std::optional<Integers> integers;
            bool isMarked { false };
        };
        Vector<Passed, 8> passed;
        auto pass = [&](ValueID id) {
            passed.append(Passed { m_folder.integersOf(facts, id), marked.contains(id) || (isOrigin && isAlwaysUnspecified(id)) });
        };
        for (ValueID argument : target.arguments)
            pass(argument);
        if (!edge.isHandlerEdge()) {
            uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
            for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
                std::optional<EdgeResult> result = tryGetEdgeResult(m_function, { edge.from, edge.targetIndex, resultIndex });
                if (result && result->kind == EdgeResultKind::NarrowedOperand && result->subject)
                    pass(result->subject);
                else
                    passed.append(Passed { });
            }
        }

        const Liveness& liveness = m_analyses.liveness();
        const UseLists& useLists = m_analyses.useLists();
        auto survives = [&](ValueID id) { return useLists.blockOf(id) != target.block; };
        Facts atTarget = facts;
        atTarget.removeAllMatching([&](const Fact& fact) {
            return !survives(fact.value) || (!fact.testedType && !liveness.isLiveAtHead(fact.value, target.block));
        });
        MarkedValues markedAtTarget;
        for (ValueID id : marked) {
            if (survives(id) && liveness.isLiveAtHead(id, target.block))
                markedAtTarget.append(id);
        }
        for (size_t index = 0; index < passed.size() && index < block.parameters.size(); ++index) {
            ValueID parameter = block.parameters[index];
            bool tellsNothing = !passed[index].integers || (*passed[index].integers == Integers::bothBooleans() && m_program.typeOf(m_function, parameter).isBoolean());
            if (!tellsNothing)
                atTarget.set(parameter, { }, *passed[index].integers);
            if (passed[index].isMarked)
                markedAtTarget.append(parameter);
        }
        if (m_origin && markedAtTarget.isEmpty())
            return;
        AOT::sort(markedAtTarget, [](ValueID left, ValueID right) { return left.index() < right.index(); });
        enter(target.block, WTF::move(atTarget), WTF::move(markedAtTarget));
    }

    void enter(BlockID block, Facts&& facts, MarkedValues&& marked)
    {
        auto queue = [&](uint32_t stateIndex) {
            m_states[stateIndex].isQueued = true;
            m_queue.append({ m_analyses.cfg().reversePostOrderIndex(block), stateIndex });
            std::ranges::push_heap(m_queue, std::greater { });
        };
        for (uint32_t stateIndex : m_statesOfBlock[block.index()]) {
            WalkState& state = m_states[stateIndex];
            if (state.marked != marked)
                continue;
            if (state.facts.mergeWith(facts) && !state.isQueued)
                queue(stateIndex);
            return;
        }
        uint32_t stateIndex = m_states.size();
        m_statesOfBlock[block.index()].append(stateIndex);
        m_states.append(WalkState { block, WTF::move(marked), WTF::move(facts), m_current, false });
        queue(stateIndex);
    }

    void walkAll()
    {
        while (!m_queue.isEmpty() && m_uses.size() < m_maxUses) {
            if (!m_blockVisitsLeft) {
                m_gaveUp = true;
                return;
            }
            --m_blockVisitsLeft;
            std::ranges::pop_heap(m_queue, std::greater { });
            uint32_t stateIndex = m_queue.takeLast().second;
            m_current = stateIndex;
            WalkState& state = m_states[stateIndex];
            state.isQueued = false;
            walkBlock(state.block, Facts(state.facts), MarkedValues(state.marked), [&](Edge edge, Facts&& facts, const MarkedValues& marked) {
                follow(edge, facts, marked);
            });
        }
    }

    UsesOfUnspecifiedValues run()
    {
        m_isAlwaysUnspecified.ensureSize(m_function.values.size());
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                bool isUnspecified = value.opcode == Opcode::Unspecified;
                if (!isUnspecified && mayReturnOperand(value.opcode))
                    forEachReturnedOperand(m_function, value, [&](ValueID operand) { isUnspecified |= isAlwaysUnspecified(operand); });
                if (isUnspecified)
                    m_isAlwaysUnspecified.quickSet(id.index());
            }
        }

        enter(entryBlock, { }, { });
        walkAll();
        m_unmarkedStates = std::exchange(m_states, { });
        for (Vector<uint32_t, 1>& states : m_statesOfBlock)
            states.clear();

        for (uint32_t index = 0; index < m_unmarkedStates.size() && !m_gaveUp && m_uses.size() < m_maxUses; ++index) {
            const WalkState& reached = m_unmarkedStates[index];
            struct Start {
                Edge edge;
                Facts facts;
            };
            Vector<Start, 2> starts;
            m_start = index;
            m_origin = std::nullopt;
            m_current = std::nullopt;
            walkBlock(reached.block, Facts(reached.facts), { }, [&](Edge edge, Facts&& facts, const MarkedValues&) {
                bool passesUnspecifiedValue = std::ranges::any_of(targetOf(m_function, edge).arguments, [&](ValueID argument) {
                    return isAlwaysUnspecified(argument);
                });
                if (passesUnspecifiedValue)
                    starts.append(Start { edge, WTF::move(facts) });
            });
            for (Start& start : starts) {
                m_origin = start.edge;
                follow(start.edge, start.facts, { });
                walkAll();
                for (const WalkState& state : m_states)
                    m_statesOfBlock[state.block.index()].clear();
                m_states.clear();
                m_queue.clear();
                m_current = std::nullopt;
            }
        }
        return { WTF::move(m_uses), !m_gaveUp && m_uses.size() < m_maxUses };
    }

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    const Program& m_program;
    Folder m_folder;
    BitVector m_isAlwaysUnspecified;
    Vector<WalkState> m_states;
    Vector<Vector<uint32_t, 1>> m_statesOfBlock;
    Vector<std::pair<uint32_t, uint32_t>> m_queue;
    Vector<WalkState> m_unmarkedStates;
    std::optional<uint32_t> m_start;
    std::optional<Edge> m_origin;
    std::optional<uint32_t> m_current;
    size_t m_maxUses;
    size_t m_blockVisitsLeft;
    const BitVector* m_countedAsLooking;
    bool m_gaveUp { false };
    Vector<UseOfUnspecifiedValue> m_uses;
};

bool MergingWalker::narrow(Facts& facts, ValueID id, Integers allowed, unsigned depth) const
{
    if (std::optional<Integers> known = m_folder.integersOf(facts, id)) {
        allowed = allowed.intersectedWith(*known);
        if (allowed.isEmpty())
            return false;
    }
    const Value& value = m_function.at(id);
    if (value.opcode == Opcode::Integer || value.opcode == Opcode::Boolean)
        return true;
    facts.set(id, { }, allowed);
    if (depth == maxNarrowingDepth || !Folder::canFold(value.opcode))
        return true;
    if (value.opcode == Opcode::TypeTest && !m_folder.isNumber(value.operand(0))) {
        if (allowed.size() == 1 && !m_program.at(value.testedType()).hasObjects())
            facts.set(m_analyses.identityOf(value.operand(0)), value.testedType(), allowed);
        return true;
    }
    if (value.opcode == Opcode::StrictEq && allowed.isOnly(1)) {
        for (uint32_t index = 0; index < 2; ++index) {
            ValueID unknown = value.operand(index);
            ValueID other = value.operand(1 - index);
            if (!m_folder.isNumber(unknown) || !m_folder.isNumber(other) || m_folder.integersOf(facts, unknown))
                continue;
            if (std::optional<Integers> known = m_folder.integersOf(facts, other))
                return narrow(facts, unknown, *known, depth + 1);
        }
    }
    bool isPossible = true;
    value.forEachOperand([&](ValueID operand, uint32_t) {
        if (!isPossible)
            return;
        Opcode opcode = m_function.at(operand).opcode;
        if (opcode == Opcode::Integer || opcode == Opcode::Boolean)
            return;
        std::optional<Integers> candidates = m_folder.integersOf(facts, operand);
        if (!candidates || candidates->size() < 2)
            return;
        Facts trial = facts;
        trial.remove(id);
        Integers kept = candidates->filtered([&](int64_t candidate) {
            trial.set(operand, { }, Integers(candidate));
            std::optional<Integers> result = m_folder.fold(trial, value, 1);
            return !result || !result->intersectedWith(allowed).isEmpty();
        });
        if (kept.isEmpty())
            isPossible = false;
        else if (kept.size() < candidates->size())
            isPossible = narrow(facts, operand, kept, depth + 1);
    });
    return isPossible;
}

} // anonymous namespace

UsesOfUnspecifiedValues findUsesOfUnspecifiedValues(FunctionAnalyses& analyses, size_t maxUses, OtherFunctions otherFunctions, size_t maxBlockVisits, const BitVector* countedAsLooking)
{
    if (!analyses.opcodesOfValues().contains(Opcode::Unspecified))
        return { };
    bool callsOutlinedBody = otherFunctions == OtherFunctions::MayChange && std::ranges::any_of(analyses.function().values, [&](const Value& value) {
        return value.opcode == Opcode::CallDirect && analyses.context().program.at(value.function()).role == FunctionRole::OutlinedBody;
    });
    if (callsOutlinedBody)
        return { };
    return MergingWalker(analyses, maxUses, maxBlockVisits, countedAsLooking).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

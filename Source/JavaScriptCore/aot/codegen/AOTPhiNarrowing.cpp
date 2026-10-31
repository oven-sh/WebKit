/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPhiNarrowing.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include "B3BasicBlockInlines.h"
#include "B3BlockRarity.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhiChildren.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <cmath>
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

static bool isExtension(B3::Value* value)
{
    return value->opcode() == B3::SExt32 || value->opcode() == B3::ZExt32;
}

bool narrowTruncatedPhis(B3::Procedure& proc)
{
    BitVector isCandidate;
    for (B3::Value* value : proc.values()) {
        if (auto* upsilon = value->as<B3::UpsilonValue>(); upsilon && isExtension(upsilon->child(0)))
            isCandidate.set(upsilon->phi()->index());
    }
    if (isCandidate.isEmpty())
        return false;

    BitVector hasTruncation;
    for (B3::Value* value : proc.values()) {
        for (B3::Value* child : value->children()) {
            if (!isCandidate.get(child->index()))
                continue;
            if (value->opcode() == B3::Trunc)
                hasTruncation.set(child->index());
            else
                isCandidate.clear(child->index());
        }
    }
    isCandidate.filter(hasTruncation);
    if (isCandidate.isEmpty())
        return false;

    Vector<B3::Value*> narrowPhis;
    narrowPhis.fill(nullptr, proc.values().size());
    B3::InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            if (value->opcode() == B3::Phi && isCandidate.get(value->index()))
                narrowPhis[value->index()] = insertionSet.insert<B3::Value>(index, B3::Phi, B3::Int32, value->origin());
        }
        insertionSet.execute(block);
    }

    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            if (auto* upsilon = value->as<B3::UpsilonValue>()) {
                B3::Value* narrowPhi = upsilon->phi()->index() < narrowPhis.size() ? narrowPhis[upsilon->phi()->index()] : nullptr;
                if (!narrowPhi)
                    continue;
                B3::Value* input = upsilon->child(0);
                B3::Value* narrowInput = nullptr;
                if (isExtension(input))
                    narrowInput = input->child(0);
                else if (input->hasInt64())
                    narrowInput = insertionSet.insert<B3::Const32Value>(index, upsilon->origin(), static_cast<int32_t>(input->asInt64()));
                else
                    narrowInput = insertionSet.insert<B3::Value>(index, B3::Trunc, upsilon->origin(), input);
                insertionSet.insert<B3::UpsilonValue>(index, upsilon->origin(), narrowInput, narrowPhi);
                upsilon->replaceWithNop();
                continue;
            }
            if (value->opcode() != B3::Trunc || value->child(0)->index() >= narrowPhis.size())
                continue;
            if (B3::Value* narrowPhi = narrowPhis[value->child(0)->index()])
                value->replaceWithIdentity(narrowPhi);
        }
        insertionSet.execute(block);
    }
    for (B3::Value* value : proc.values()) {
        value->performSubstitution();
        if (value->opcode() == B3::Phi && value->index() < narrowPhis.size() && narrowPhis[value->index()])
            value->replaceWithNopIgnoringType();
    }
    return true;
}

// value = (value * 31 + 7) % 1000003, where nothing proves that `value` is whole.
// The remainder tests its dividend for a whole number, takes the remainder of the integer and makes it a number again. Round the loop that is
// integer > number > product > sum > integer, all of it one chain. Here the integer goes round the loop beside the number, in a Phi of its own that holds
// `unknownInteger` wherever the number did not come from an integer. While it is not negative, and so small that each step is exact, the product and the sum are those of integers.
// Otherwise the number is tested as before, which is also how the integer gets known again.
class WholeNumbersCarriedAsIntegers {
public:
    explicit WholeNumbersCarriedAsIntegers(B3::Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        findTests();
        if (m_tests.isEmpty() || !m_proc.hasCycle())
            return false;

        m_proc.resetValueOwners();
        m_numberOfOldBlocks = m_proc.size();
        m_phiChildren.emplace(m_proc);
        dropTestsWhoseValuesEscape();
        bool hasPlan = false;
        for (Test& test : m_tests) {
            test.isPlanned = test.isPlanned && plan(test);
            hasPlan |= test.isPlanned;
        }
        if (!hasPlan)
            return false;

        for (Test& test : m_tests) {
            if (!test.isPlanned)
                continue;
            for (Node& node : test.nodes) {
                if (node.kind == NodeKind::CarriedPhi)
                    node.integer = integerPhiFor(node.number);
            }
        }
        executeInsertions();
        for (Test& test : m_tests) {
            if (test.isPlanned)
                apply(test);
        }
        replaceTestedIntegers();
        moveNumbersToRareUsers();
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr int64_t unknownInteger = -1;
    static constexpr unsigned bitsOfExactIntegers = 53;

    enum class NodeKind : uint8_t {
        Constant,
        Integer,
        BoundedInteger,
        CarriedPhi,
        Sum,
        Difference,
        Product,
        Negation,
    };

    struct Node {
        B3::Value* number { nullptr };
        NodeKind kind { NodeKind::Constant };
        unsigned left { 0 };
        unsigned right { 0 };
        int64_t min { 0 };
        int64_t max { 0 };
        bool mayBeNegativeZero { false };
        B3::Value* integer { nullptr };
    };

    struct Test {
        B3::BasicBlock* block { nullptr };
        B3::PatchpointValue* truncated { nullptr };
        B3::Value* narrowed { nullptr };
        B3::Value* number { nullptr };
        Vector<B3::Value*, 8> values;
        Vector<Node, 8> nodes;
        // Blocks behind the test that only compare the integer with a constant, each the one way into the next.
        Vector<B3::BasicBlock*, 2> laterTests;
        Vector<B3::BasicBlock*, 4> blocksOfUsers;
        unsigned bitsOfBoundedLeaves { 0 };
        bool isPlanned { true };
    };

    struct Insertion {
        B3::BasicBlock* block;
        B3::Value* before;
        B3::Value* value;
    };

    static bool isOperation(NodeKind kind)
    {
        switch (kind) {
        case NodeKind::Sum:
        case NodeKind::Difference:
        case NodeKind::Product:
        case NodeKind::Negation:
            return true;
        case NodeKind::Constant:
        case NodeKind::Integer:
        case NodeKind::BoundedInteger:
        case NodeKind::CarriedPhi:
            return false;
        }
        return false;
    }

    // Branch(Equal(BitwiseCast(IToD(truncated)), BitwiseCast(number))), or the same with IToD(Trunc(truncated)), at the end of a block that holds nothing else from `truncated` on.
    void findTests()
    {
        for (B3::BasicBlock* block : m_proc) {
            B3::Value* branch = block->last();
            if (branch->opcode() != B3::Branch || branch->child(0)->opcode() != B3::Equal)
                continue;
            if (block->successor(1).frequency() != B3::FrequencyClass::Rare || block->successorBlock(0) == block->successorBlock(1))
                continue;
            B3::Value* equal = branch->child(0);
            for (unsigned side = 0; side < 2; ++side) {
                B3::Value* bitsOfConverted = equal->child(side);
                B3::Value* bitsOfNumber = equal->child(1 - side);
                if (bitsOfConverted->opcode() != B3::BitwiseCast || bitsOfNumber->opcode() != B3::BitwiseCast || bitsOfConverted->child(0)->opcode() != B3::IToD)
                    continue;
                B3::Value* converted = bitsOfConverted->child(0);
                B3::Value* integer = converted->child(0);
                B3::Value* narrowed = integer->opcode() == B3::Trunc ? integer : nullptr;
                auto* truncated = (narrowed ? narrowed->child(0) : integer)->as<B3::PatchpointValue>();
                if (!truncated || !truncated->pureFunction || !truncated->pureFunction->truncatesDoubleToInt64 || truncated->child(0) != bitsOfNumber->child(0))
                    continue;
                Test test;
                test.block = block;
                test.truncated = truncated;
                test.narrowed = narrowed;
                test.number = truncated->child(0);
                test.values = { truncated, converted, bitsOfConverted, bitsOfNumber, equal, branch };
                if (narrowed)
                    test.values.append(narrowed);
                if (block->size() < test.values.size() || block->at(block->size() - test.values.size()) != truncated)
                    break;
                bool holdsOnlyTest = true;
                for (unsigned index = block->size() - test.values.size(); index < block->size(); ++index)
                    holdsOnlyTest &= test.values.contains(block->at(index));
                if (holdsOnlyTest)
                    m_tests.append(WTF::move(test));
                break;
            }
        }
    }

    void dropTestsWhoseValuesEscape()
    {
        B3::Dominators& dominators = m_proc.dominators();
        UncheckedKeyHashMap<B3::Value*, unsigned> testOfValue;
        for (unsigned index = 0; index < m_tests.size(); ++index) {
            Test& test = m_tests[index];
            if (test.block->successorBlock(0)->numPredecessors() != 1)
                test.isPlanned = false;
            for (B3::Value* value : test.values)
                testOfValue.add(value, index);
            for (B3::BasicBlock* block = test.block->successorBlock(0); isLaterTest(test, block); block = block->successorBlock(0)) {
                test.laterTests.append(block);
                m_usesOfLaterTests.add(block->at(0), 0);
            }
        }
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* user : *block) {
                for (B3::Value* child : user->children()) {
                    if (auto uses = m_usesOfLaterTests.find(child); uses != m_usesOfLaterTests.end())
                        ++uses->value;
                    auto iterator = testOfValue.find(child);
                    if (iterator == testOfValue.end())
                        continue;
                    Test& test = m_tests[iterator->value];
                    if (test.values.contains(user))
                        continue;
                    bool isResult = child == test.truncated || child == test.narrowed;
                    if (!isResult || !dominators.dominates(test.block->successorBlock(0), block))
                        test.isPlanned = false;
                    if (test.blocksOfUsers.isEmpty() || test.blocksOfUsers.last() != block)
                        test.blocksOfUsers.append(block);
                }
            }
        }
    }

    static bool isLaterTest(const Test& test, B3::BasicBlock* block)
    {
        if (block->numPredecessors() != 1 || block->size() != 2 || block->last()->opcode() != B3::Branch || block->last()->child(0) != block->at(0))
            return false;
        B3::Value* comparison = block->at(0);
        if (!B3::isIntegerComparison(comparison->opcode()) || !comparison->child(1)->hasInt())
            return false;
        return comparison->child(0) == test.truncated || (test.narrowed && comparison->child(0) == test.narrowed);
    }

    // The integer made of integers passes by the later tests that its range decides.
    void keepLaterTestsThatRangeDecides(Test& test)
    {
        const Node& result = test.nodes.last();
        B3::IntRange range(result.min, result.max);
        if (test.narrowed)
            range = B3::IntRange(std::max<int64_t>(result.min, std::numeric_limits<int32_t>::min()), std::min<int64_t>(result.max, std::numeric_limits<int32_t>::max()));
        unsigned decided = 0;
        while (decided < test.laterTests.size()) {
            B3::Value* comparison = test.laterTests[decided]->at(0);
            if (m_usesOfLaterTests.get(comparison) != 1 || B3::evaluateComparison(comparison->opcode(), range, comparison->child(1)->asInt()) != TriState::True)
                break;
            ++decided;
        }
        B3::Dominators& dominators = m_proc.dominators();
        for (; decided; --decided) {
            B3::BasicBlock* join = test.laterTests[decided - 1]->successorBlock(0);
            bool reachesAllUsers = join->numPredecessors() == 1;
            for (B3::BasicBlock* block : test.blocksOfUsers)
                reachesAllUsers = reachesAllUsers && (dominators.dominates(join, block) || test.laterTests.find(block) < decided);
            if (reachesAllUsers)
                break;
        }
        test.laterTests.shrink(decided);
    }

    static std::optional<int64_t> wholeConstant(B3::Value* number)
    {
        double constant = number->asDouble();
        if (std::trunc(constant) != constant || std::abs(constant) > static_cast<double>(1ll << bitsOfExactIntegers) || (!constant && std::signbit(constant)))
            return std::nullopt;
        return static_cast<int64_t>(constant);
    }

    static bool comesFromInteger(B3::Value* number)
    {
        return number->opcode() == B3::IToD || (number->opcode() == B3::ConstDouble && wholeConstant(number));
    }

    // Whether an integer reaches the Phi through Phis alone.
    bool mayComeFromInteger(B3::Value* phi)
    {
        if (auto iterator = m_mayComeFromInteger.find(phi); iterator != m_mayComeFromInteger.end())
            return iterator->value;

        Vector<B3::Value*, 8> web;
        Vector<std::pair<B3::Value*, B3::Value*>, 8> edges;
        Vector<B3::Value*, 8> reached;
        web.append(phi);
        m_mayComeFromInteger.add(phi, false);
        for (unsigned index = 0; index < web.size(); ++index) {
            B3::Value* current = web[index];
            bool isReached = false;
            for (B3::Value* input : m_phiChildren->at(current).values()) {
                if (comesFromInteger(input)) {
                    isReached = true;
                    continue;
                }
                if (input->opcode() != B3::Phi)
                    continue;
                auto result = m_mayComeFromInteger.add(input, false);
                if (result.isNewEntry)
                    web.append(input);
                else if (result.iterator->value)
                    isReached = true;
                edges.append({ input, current });
            }
            if (isReached)
                reached.append(current);
        }
        UncheckedKeyHashMap<B3::Value*, Vector<B3::Value*, 2>> usersOf;
        for (auto [input, user] : edges)
            usersOf.add(input, Vector<B3::Value*, 2>()).iterator->value.append(user);
        for (B3::Value* current : reached)
            m_mayComeFromInteger.set(current, true);
        while (!reached.isEmpty()) {
            B3::Value* current = reached.takeLast();
            auto users = usersOf.find(current);
            if (users == usersOf.end())
                continue;
            for (B3::Value* user : users->value) {
                auto iterator = m_mayComeFromInteger.find(user);
                if (iterator->value)
                    continue;
                iterator->value = true;
                reached.append(user);
            }
        }
        return m_mayComeFromInteger.get(phi);
    }

    // Whether a number that is made from an integer behind the test, in the loop of the test, comes round to the Phi.
    bool isCarriedAroundLoopOf(const Test& test, B3::Value* phi)
    {
        if (!mayComeFromInteger(phi))
            return false;
        const B3::NaturalLoop* loop = m_proc.naturalLoops().innerMostLoopOf(test.block);
        if (!loop)
            return false;
        B3::Dominators& dominators = m_proc.dominators();
        B3::BlockRarity& rarity = m_proc.blockRarity();
        B3::BasicBlock* behindTest = test.block->successorBlock(0);
        Vector<B3::Value*, 8> web;
        UncheckedKeyHashSet<B3::Value*> seen;
        web.append(phi);
        seen.add(phi);
        for (unsigned index = 0; index < web.size(); ++index) {
            for (B3::UpsilonValue* upsilon : m_phiChildren->at(web[index])) {
                B3::Value* input = upsilon->child(0);
                if (input->opcode() == B3::IToD && !rarity.isRare(upsilon->owner) && m_proc.naturalLoops().belongsTo(upsilon->owner, *loop) && dominators.dominates(behindTest, upsilon->owner))
                    return true;
                if (input->opcode() == B3::Phi && mayComeFromInteger(input) && seen.add(input).isNewEntry)
                    web.append(input);
            }
        }
        return false;
    }

    bool collectNodes(Test& test)
    {
        struct Visit {
            B3::Value* number;
            bool hasVisitedChildren;
        };
        UncheckedKeyHashMap<B3::Value*, unsigned> indexOf;
        Vector<Visit, 16> stack;
        stack.append({ test.number, false });
        bool hasCarriedPhi = false;
        while (!stack.isEmpty()) {
            Visit visit = stack.takeLast();
            B3::Value* number = visit.number;
            if (indexOf.contains(number))
                continue;
            Node node;
            node.number = number;
            switch (number->opcode()) {
            case B3::Add:
            case B3::Sub:
            case B3::Mul:
            case B3::Neg:
                if (!visit.hasVisitedChildren) {
                    stack.append({ number, true });
                    for (B3::Value* child : number->children())
                        stack.append({ child, false });
                    continue;
                }
                node.kind = number->opcode() == B3::Add ? NodeKind::Sum : number->opcode() == B3::Sub ? NodeKind::Difference : number->opcode() == B3::Mul ? NodeKind::Product : NodeKind::Negation;
                node.left = indexOf.get(number->child(0));
                node.right = indexOf.get(number->child(number->numChildren() - 1));
                break;
            case B3::ConstDouble:
                if (!wholeConstant(number))
                    return false;
                node.kind = NodeKind::Constant;
                break;
            case B3::IToD: {
                B3::IntRange range = B3::rangeFor(m_proc, number->child(0));
                constexpr int64_t limit = 1ll << bitsOfExactIntegers;
                node.kind = range.min() >= -limit && range.max() <= limit ? NodeKind::Integer : NodeKind::BoundedInteger;
                node.min = range.min();
                node.max = range.max();
                break;
            }
            case B3::Phi:
                if (!isCarriedAroundLoopOf(test, number))
                    return false;
                node.kind = NodeKind::CarriedPhi;
                hasCarriedPhi = true;
                break;
            default:
                return false;
            }
            indexOf.add(number, test.nodes.size());
            test.nodes.append(node);
        }
        return hasCarriedPhi;
    }

    // Each step is exact while no result is beyond 2^53. Only the sign of a zero can then differ from the integer's.
    static bool computeBounds(Test& test, unsigned bitsOfBoundedLeaves)
    {
        constexpr int64_t limit = 1ll << bitsOfExactIntegers;
        for (Node& node : test.nodes) {
            const Node& left = test.nodes[node.left];
            const Node& right = test.nodes[node.right];
            auto mayBeZero = [](const Node& operand) { return operand.min <= 0 && operand.max >= 0; };
            switch (node.kind) {
            case NodeKind::Constant:
                node.min = node.max = *wholeConstant(node.number);
                break;
            case NodeKind::Integer:
                break;
            case NodeKind::BoundedInteger:
                node.max = 1ll << bitsOfBoundedLeaves;
                node.min = -node.max;
                break;
            case NodeKind::CarriedPhi:
                node.max = 1ll << bitsOfBoundedLeaves;
                node.min = 0;
                break;
            case NodeKind::Sum:
                node.min = left.min + right.min;
                node.max = left.max + right.max;
                node.mayBeNegativeZero = left.mayBeNegativeZero && right.mayBeNegativeZero;
                break;
            case NodeKind::Difference:
                node.min = left.min - right.max;
                node.max = left.max - right.min;
                node.mayBeNegativeZero = left.mayBeNegativeZero && mayBeZero(right);
                break;
            case NodeKind::Product: {
                node.min = std::numeric_limits<int64_t>::max();
                node.max = std::numeric_limits<int64_t>::min();
                for (int64_t x : { left.min, left.max }) {
                    for (int64_t y : { right.min, right.max }) {
                        CheckedInt64 product = CheckedInt64(x) * y;
                        if (product.hasOverflowed())
                            return false;
                        node.min = std::min(node.min, product.value());
                        node.max = std::max(node.max, product.value());
                    }
                }
                node.mayBeNegativeZero = left.mayBeNegativeZero || right.mayBeNegativeZero || (mayBeZero(left) && right.min < 0) || (mayBeZero(right) && left.min < 0);
                break;
            }
            case NodeKind::Negation:
                node.min = -left.max;
                node.max = -left.min;
                node.mayBeNegativeZero = mayBeZero(left);
                break;
            }
            if (node.min < -limit || node.max > limit)
                return false;
        }
        return true;
    }

    bool plan(Test& test)
    {
        if (!collectNodes(test) || !computeBounds(test, 0))
            return false;
        unsigned fits = 0;
        unsigned tooMany = bitsOfExactIntegers;
        while (fits + 1 < tooMany) {
            unsigned middle = (fits + tooMany) / 2;
            if (computeBounds(test, middle))
                fits = middle;
            else
                tooMany = middle;
        }
        test.bitsOfBoundedLeaves = fits;
        if (!computeBounds(test, fits))
            return false;
        keepLaterTestsThatRangeDecides(test);
        return true;
    }

    B3::Value* integerPhiFor(B3::Value* firstPhi)
    {
        if (B3::Value* known = m_integerPhis.get(firstPhi))
            return known;
        Vector<B3::Value*, 8> web;
        auto phiFor = [&](B3::Value* phi) {
            auto result = m_integerPhis.add(phi, nullptr);
            if (result.isNewEntry) {
                result.iterator->value = m_proc.add<B3::Value>(B3::Phi, B3::Int64, phi->origin());
                m_insertions.append({ phi->owner, phi, result.iterator->value });
                web.append(phi);
            }
            return result.iterator->value;
        };
        phiFor(firstPhi);
        while (!web.isEmpty()) {
            B3::Value* phi = web.takeLast();
            B3::Value* integerPhi = m_integerPhis.get(phi);
            for (B3::UpsilonValue* upsilon : m_phiChildren->at(phi)) {
                B3::Value* input = upsilon->child(0);
                B3::Value* integer = nullptr;
                auto insert = [&](B3::Value* value) {
                    m_insertions.append({ upsilon->owner, upsilon, value });
                    return value;
                };
                if (input->opcode() == B3::IToD)
                    integer = input->child(0)->type() == B3::Int64 ? input->child(0) : insert(m_proc.add<B3::Value>(B3::SExt32, upsilon->origin(), input->child(0)));
                else if (input->opcode() == B3::Phi && mayComeFromInteger(input))
                    integer = phiFor(input);
                else {
                    std::optional<int64_t> constant = input->opcode() == B3::ConstDouble ? wholeConstant(input) : std::nullopt;
                    integer = insert(m_proc.add<B3::Const64Value>(upsilon->origin(), constant.value_or(unknownInteger)));
                }
                insert(m_proc.add<B3::UpsilonValue>(upsilon->origin(), integer, integerPhi));
            }
        }
        return m_integerPhis.get(firstPhi);
    }

    void executeInsertions()
    {
        UncheckedKeyHashMap<B3::Value*, Vector<B3::Value*, 2>> insertedBefore;
        BitVector changedBlocks;
        for (const Insertion& insertion : m_insertions) {
            insertedBefore.add(insertion.before, Vector<B3::Value*, 2>()).iterator->value.append(insertion.value);
            changedBlocks.set(insertion.block->index());
        }
        B3::InsertionSet insertionSet(m_proc);
        for (B3::BasicBlock* block : m_proc) {
            if (!changedBlocks.get(block->index()))
                continue;
            for (unsigned index = 0; index < block->size(); ++index) {
                auto iterator = insertedBefore.find(block->at(index));
                if (iterator == insertedBefore.end())
                    continue;
                for (B3::Value* value : iterator->value)
                    insertionSet.insertValue(index, value);
            }
            insertionSet.execute(block);
        }
        m_insertions.clear();
    }

    void apply(Test& test)
    {
        B3::BasicBlock* block = test.block;
        B3::Origin origin = test.truncated->origin();
        B3::FrequentedBlock behindTest = block->successor(0);
        B3::FrequentedBlock notWhole = block->successor(1);

        B3::BasicBlock* testOfNumber = m_proc.addBlock(block->frequency());
        B3::BasicBlock* numberIsWhole = m_proc.addBlock(block->frequency());
        m_rareNewBlocks.set(testOfNumber->index());
        m_rareNewBlocks.set(numberIsWhole->index());
        m_blocksOfTestedNumbers.set(testOfNumber->index());
        m_blocksOfTestedNumbers.set(numberIsWhole->index());
        for (B3::BasicBlock* laterTest : test.laterTests)
            m_blocksOfTestedNumbers.set(laterTest->index());
        B3::BasicBlock* join = test.laterTests.isEmpty() ? behindTest.block() : test.laterTests.last()->successorBlock(0);
        unsigned start = block->size() - test.values.size();
        RELEASE_ASSERT(block->at(start) == test.truncated);
        for (unsigned index = start; index < block->size(); ++index)
            testOfNumber->append(block->at(index));
        block->values().shrink(start);
        testOfNumber->setSuccessors(B3::FrequentedBlock(numberIsWhole), notWhole);

        B3::BasicBlock* current = block;
        auto append = [&]<typename... Arguments>(B3::Opcode opcode, Arguments... arguments) {
            return current->appendNew<B3::Value>(m_proc, opcode, origin, arguments...);
        };
        auto constant = [&](int64_t value) -> B3::Value* {
            return current->appendNew<B3::Const64Value>(m_proc, origin, value);
        };
        auto goOnIf = [&](B3::Value* condition) {
            append(B3::Branch, condition);
            B3::BasicBlock* next = m_proc.addBlock(block->frequency());
            current->setSuccessors(B3::FrequentedBlock(next), B3::FrequentedBlock(testOfNumber, B3::FrequencyClass::Rare));
            current = next;
        };
        // A Phi behind the test is what may state the range that holds there.
        auto goOnWithBounded = [&](B3::Value* condition, const Node& node) {
            B3::Value* bounded = m_proc.add<B3::Value>(B3::Phi, B3::Int64, origin);
            current->appendNew<B3::UpsilonValue>(m_proc, origin, node.integer, bounded);
            goOnIf(condition);
            current->append(bounded);
            m_proc.setKnownRange(bounded, B3::IntRange(node.min, node.max));
            return bounded;
        };
        int64_t bound = 1ll << test.bitsOfBoundedLeaves;

        for (NodeKind tested : { NodeKind::CarriedPhi, NodeKind::BoundedInteger }) {
            for (Node& node : test.nodes) {
                if (node.kind != tested)
                    continue;
                switch (node.kind) {
                case NodeKind::CarriedPhi:
                    node.integer = goOnWithBounded(append(B3::BelowEqual, node.integer, constant(bound)), node);
                    break;
                case NodeKind::BoundedInteger:
                    node.integer = node.number->child(0);
                    node.integer = goOnWithBounded(append(B3::BelowEqual, append(B3::Add, node.integer, constant(bound)), constant(2 * bound)), node);
                    break;
                case NodeKind::Constant:
                case NodeKind::Integer:
                case NodeKind::Sum:
                case NodeKind::Difference:
                case NodeKind::Product:
                case NodeKind::Negation:
                    break;
                }
            }
        }
        for (Node& node : test.nodes) {
            B3::Value* left = test.nodes[node.left].integer;
            B3::Value* right = test.nodes[node.right].integer;
            switch (node.kind) {
            case NodeKind::Constant:
                node.integer = constant(*wholeConstant(node.number));
                break;
            case NodeKind::Integer:
                node.integer = node.number->child(0);
                if (node.integer->type() == B3::Int32)
                    node.integer = append(node.min >= 0 ? B3::ZExt32 : B3::SExt32, node.integer);
                break;
            case NodeKind::Sum:
                node.integer = append(B3::Add, left, right);
                break;
            case NodeKind::Difference:
                node.integer = append(B3::Sub, left, right);
                break;
            case NodeKind::Product:
                node.integer = append(B3::Mul, left, right);
                break;
            case NodeKind::Negation:
                node.integer = append(B3::Neg, left);
                break;
            case NodeKind::BoundedInteger:
            case NodeKind::CarriedPhi:
                break;
            }
            if (isOperation(node.kind))
                m_operations.add(node.number);
        }
        const Node& result = test.nodes.last();
        RELEASE_ASSERT(result.number == test.number);
        if (result.mayBeNegativeZero)
            goOnIf(append(B3::NotEqual, result.integer, constant(0)));
        if (test.narrowed && (result.min < std::numeric_limits<int32_t>::min() || result.max > std::numeric_limits<int32_t>::max()))
            goOnIf(append(B3::Equal, append(B3::SExt32, append(B3::Trunc, result.integer)), result.integer));

        B3::Value* phi = m_proc.add<B3::Value>(B3::Phi, B3::Int64, origin);
        current->appendNew<B3::UpsilonValue>(m_proc, origin, result.integer, phi);
        append(B3::Jump);
        current->setSuccessors(B3::FrequentedBlock(join));
        B3::InsertionSet insertionSet(m_proc);
        if (test.laterTests.isEmpty())
            numberIsWhole->appendNew<B3::UpsilonValue>(m_proc, origin, test.truncated, phi);
        else {
            insertionSet.insert<B3::UpsilonValue>(test.laterTests.last()->size() - 1, origin, test.truncated, phi);
            insertionSet.execute(test.laterTests.last());
        }
        numberIsWhole->appendNew<B3::Value>(m_proc, B3::Jump, origin);
        numberIsWhole->setSuccessors(B3::FrequentedBlock(behindTest.block()));

        insertionSet.insertValue(0, phi);
        m_replacements.add(test.truncated, phi);
        if (test.narrowed)
            m_replacements.add(test.narrowed, insertionSet.insert<B3::Value>(0, B3::Trunc, origin, phi));
        insertionSet.execute(join);
    }

    bool isRare(B3::BasicBlock* block)
    {
        if (block->index() >= m_numberOfOldBlocks)
            return m_rareNewBlocks.get(block->index());
        return m_proc.blockRarity().isRare(block);
    }

    void replaceTestedIntegers()
    {
        for (B3::BasicBlock* block : m_proc) {
            if (m_blocksOfTestedNumbers.get(block->index()))
                continue;
            for (B3::Value* value : *block) {
                for (B3::Value*& child : value->children()) {
                    if (B3::Value* replacement = m_replacements.get(child))
                        child = replacement;
                }
            }
        }
    }

    // The products and sums of numbers are needed only where the integers are not known.
    void moveNumbersToRareUsers()
    {
        UncheckedKeyHashSet<B3::Value*> stays;
        Vector<B3::Value*, 16> worklist;
        auto keep = [&](B3::Value* operation) {
            if (m_operations.contains(operation) && stays.add(operation).isNewEntry)
                worklist.append(operation);
        };
        for (B3::BasicBlock* block : m_proc) {
            if (isRare(block))
                continue;
            for (B3::Value* value : *block) {
                if (m_operations.contains(value))
                    continue;
                for (B3::Value* child : value->children())
                    keep(child);
            }
        }
        while (!worklist.isEmpty()) {
            for (B3::Value* child : worklist.takeLast()->children())
                keep(child);
        }

        auto moves = [&](B3::Value* value) {
            return m_operations.contains(value) && !stays.contains(value);
        };
        B3::InsertionSet insertionSet(m_proc);
        UncheckedKeyHashMap<B3::Value*, B3::Value*> copies;
        Vector<B3::Value*, 16> stack;
        for (B3::BasicBlock* block : m_proc) {
            if (!isRare(block))
                continue;
            copies.clear();
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* value = block->at(index);
                if (moves(value))
                    continue;
                for (B3::Value*& child : value->children()) {
                    if (!moves(child))
                        continue;
                    stack.append(child);
                    while (!stack.isEmpty()) {
                        B3::Value* operation = stack.last();
                        if (copies.contains(operation)) {
                            stack.removeLast();
                            continue;
                        }
                        bool hasOperands = true;
                        for (B3::Value* operand : operation->children()) {
                            if (moves(operand) && !copies.contains(operand)) {
                                stack.append(operand);
                                hasOperands = false;
                            }
                        }
                        if (!hasOperands)
                            continue;
                        B3::Value* copy = m_proc.clone(operation);
                        for (B3::Value*& operand : copy->children()) {
                            if (moves(operand))
                                operand = copies.get(operand);
                        }
                        insertionSet.insertValue(index, copy);
                        copies.add(operation, copy);
                        stack.removeLast();
                    }
                    child = copies.get(child);
                }
            }
            insertionSet.execute(block);
        }
    }

    B3::Procedure& m_proc;
    Vector<Test> m_tests;
    std::optional<B3::PhiChildren> m_phiChildren;
    UncheckedKeyHashMap<B3::Value*, bool> m_mayComeFromInteger;
    UncheckedKeyHashMap<B3::Value*, B3::Value*> m_integerPhis;
    UncheckedKeyHashMap<B3::Value*, B3::Value*> m_replacements;
    UncheckedKeyHashSet<B3::Value*> m_operations;
    Vector<Insertion> m_insertions;
    UncheckedKeyHashMap<B3::Value*, unsigned> m_usesOfLaterTests;
    BitVector m_rareNewBlocks;
    BitVector m_blocksOfTestedNumbers;
    unsigned m_numberOfOldBlocks { 0 };
};

bool carryWholeNumbersAsIntegers(B3::Procedure& proc)
{
    return WholeNumbersCarriedAsIntegers(proc).run();
}

B3::ClientPhase carryWholeNumbersAsIntegersBeforeOptimization()
{
    return {
        B3::ClientPhase::Point::BeforeOptimization,
        "carryWholeNumbersAsIntegers"_s,
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) {
            return carryWholeNumbersAsIntegers(proc);
        })
    };
}

B3::ClientPhase narrowTruncatedPhisAfterOptimization()
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "narrowTruncatedPhis"_s,
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) {
            return narrowTruncatedPhis(proc);
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

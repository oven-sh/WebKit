/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTControlEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTOperationContext.h"
#include <algorithm>
#include <bit>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t mostBlocksInRotatingIteration = 32;
static constexpr size_t mostPositionsSearchedForIteration = 256;
static constexpr unsigned mostOperationsOfRotatingLoop = 224;
static constexpr unsigned fewestParametersRotated = 3;
static constexpr unsigned fewestBitOperationsInRotatingIteration = 6;
static constexpr int64_t mostIterationsOfRotatingLoop = 256;
static constexpr int64_t fewestIterationsOfRotatingLoop = 16;
static constexpr int64_t mostIterationsEmittedOfRotatingLoop = 64;
static constexpr int64_t mostIterationsInRow = 8;
static constexpr int64_t mostIterationsInRowOfRing = 16;
static constexpr int64_t fewestIterationsInRow = 4;
static constexpr int64_t mostBasesOfRing = 4;

static constexpr OpcodeSet readsInRotatingIterations {
    Opcode::GetArrayElement, Opcode::IsElementPresent, Opcode::GetTypedArrayElement, Opcode::GetArrayLength, Opcode::HasArrayStorage, Opcode::GetArrayStorage, Opcode::TypeTest, Opcode::CheckProof,
};
static constexpr OpcodeSet storesInRotatingIterations { Opcode::PutContiguousIndex, Opcode::PutTypedArrayElement };
static constexpr OpcodeSet conversionsOfPassedWords { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
static constexpr OpcodeSet bitOperationsOfRotatingIterations { Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
static constexpr OpcodeSet relationsOfRotatingCounters { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

ValueID CodeGenerator::wordPassedOn(ValueID value) const
{
    while (!code().isConstant(value) && conversionsOfPassedWords.contains(opcodeOf(value)))
        value = code().at(value).valueOperand();
    return value;
}

std::optional<int64_t> CodeGenerator::constantIntegerOf(ValueID value) const
{
    if (!code().isConstant(value) || opcodeOf(value) != Opcode::Integer)
        return std::nullopt;
    return code().at(value).integer();
}

std::optional<int64_t> CodeGenerator::distanceFromCounter(const RotatingLoop& loop, ValueID value) const
{
    static constexpr unsigned mostSteps = 4;
    int64_t distance = 0;
    for (unsigned step = 0; step < mostSteps; ++step) {
        while (!code().isConstant(value) && opcodeOf(value) == Opcode::Narrowed)
            value = code().at(value).valueOperand();
        if (value == loop.counter)
            return distance;
        if (code().isConstant(value))
            return std::nullopt;
        const Operation& operation = code().at(value);
        if (operation.opcode == Opcode::Increment && operation.form() == OperatorForm::Integers) {
            ++distance;
            value = operation.valueOperand();
            continue;
        }
        if ((operation.opcode != Opcode::Add && operation.opcode != Opcode::Sub) || operation.form() != OperatorForm::Integers)
            return std::nullopt;
        std::optional<int64_t> right = constantIntegerOf(operation.rightOperand());
        std::optional<int64_t> left = right || operation.opcode == Opcode::Sub ? std::nullopt : constantIntegerOf(operation.leftOperand());
        if (!right && !left)
            return std::nullopt;
        int64_t added = operation.opcode == Opcode::Sub ? -*right : right ? *right : *left;
        if (std::abs(added) > mostIterationsOfRotatingLoop)
            return std::nullopt;
        distance += added;
        value = right ? operation.leftOperand() : operation.rightOperand();
    }
    return std::nullopt;
}

std::optional<int64_t> CodeGenerator::boundaryOfRelation(const RotatingLoop& loop, const Operation& relation, bool& holdsBelow) const
{
    if (!relationsOfRotatingCounters.contains(relation.opcode) || relation.form() != OperatorForm::Integers)
        return std::nullopt;
    bool comparesLess = relation.opcode == Opcode::Less || relation.opcode == Opcode::LessEq;
    bool admitsEqual = relation.opcode == Opcode::LessEq || relation.opcode == Opcode::GreaterEq;
    if (std::optional<int64_t> bound = constantIntegerOf(relation.rightOperand())) {
        std::optional<int64_t> distance = distanceFromCounter(loop, relation.leftOperand());
        if (!distance)
            return std::nullopt;
        holdsBelow = comparesLess;
        return *bound - *distance + (comparesLess == admitsEqual);
    }
    std::optional<int64_t> bound = constantIntegerOf(relation.leftOperand());
    std::optional<int64_t> distance = bound ? distanceFromCounter(loop, relation.rightOperand()) : std::nullopt;
    if (!distance)
        return std::nullopt;
    holdsBelow = !comparesLess;
    return *bound - *distance + (comparesLess != admitsEqual);
}

std::optional<CodeGenerator::RotatingLoop> CodeGenerator::findRotatingLoop(size_t positionOfHead) const
{
    BlockID head = m_order[positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    if (parameters.size() <= fewestParametersRotated || m_keepsValuesInHeapFrame || m_headOfTurnsLaidOut || m_interleaving || m_rows)
        return std::nullopt;
    if (blockOfHead.frequency != BlockFrequency::Normal || code().handlerOf(head) || blockOfHead.subject || blockOfHead.isEntry || blockOfHead.isHandlerBlock() || blockOfHead.terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    RotatingLoop loop { .positionOfHead = positionOfHead };
    ValueID test = blockOfHead.terminator.conditionOperand();
    if (code().isConstant(test) || code().blockOf(test) != head)
        return std::nullopt;
    for (ValueID value : code().valuesOf(head)) {
        if (value == test)
            continue;
        if (opcodeOf(value) != Opcode::LoopHint || loop.poll)
            return std::nullopt;
        loop.poll = value;
    }
    const Operation& comparison = code().at(test);
    if (!relationsOfRotatingCounters.contains(comparison.opcode) || comparison.form() != OperatorForm::Integers)
        return std::nullopt;
    bool comparesLess = comparison.opcode == Opcode::Less || comparison.opcode == Opcode::LessEq;
    loop.counter = comparesLess ? comparison.leftOperand() : comparison.rightOperand();
    ValueID bound = comparesLess ? comparison.rightOperand() : comparison.leftOperand();
    loop.indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    if (loop.indexOfCounter == parameters.size() || representationOf(loop.counter) != Representation::Int32)
        return std::nullopt;
    bool admitsEqual = comparison.opcode == Opcode::LessEq || comparison.opcode == Opcode::GreaterEq;
    int64_t end = 0;
    if (std::optional<int64_t> constant = constantIntegerOf(bound))
        end = *constant + admitsEqual;
    else {
        if (admitsEqual || code().isConstant(bound) || code().blockOf(bound) == head || representationOf(bound) != Representation::Int32 || typeOf(bound).integers.isEmpty() || typeOf(bound).integers.minimum < 0)
            return std::nullopt;
        end = typeOf(bound).integers.maximum;
        loop.bound = bound;
    }
    auto isPlainParameter = [&](ValueID parameter) {
        static constexpr std::array plain { Representation::Int32, Representation::Int64, Representation::Double, Representation::Boolean, Representation::JSValue, Representation::Cell, Representation::RawWord };
        return !isCarriedBoxed(parameter) && !isCarriedUnboxed(parameter) && !isCarriedSum(parameter) && std::ranges::contains(plain, representationOf(parameter));
    };
    if (!std::ranges::all_of(parameters, isPlainParameter) || carriesBesideParameters(head))
        return std::nullopt;

    auto canBePart = [&](BlockID block) {
        const Block& part = code().at(block);
        return part.isInLoop && !part.isLoopHead && part.frequency == BlockFrequency::Normal && !code().handlerOf(block) && !part.subject && !part.isEntry && !part.isHandlerBlock() && std::ranges::all_of(part.parameters(), isPlainParameter);
    };
    const Edge& intoIteration = code().at(blockOfHead.terminator.taken());
    if (!intoIteration.arguments().empty() || !canBePart(intoIteration.to) || m_singlePredecessors[intoIteration.to.index()] != head)
        return std::nullopt;
    Vector<BlockID, mostBlocksInRotatingIteration> reached { intoIteration.to };
    const Edge* back = nullptr;
    auto forEachEdge = [&](BlockID block, auto&& function) {
        const Terminator& terminator = code().at(block).terminator;
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge)
            function(code().at(terminator.edge(edge)));
    };
    bool isSimple = true;
    for (size_t place = 0; place < reached.size() && isSimple; ++place) {
        const Terminator& terminator = code().at(reached[place]).terminator;
        isSimple = terminator.kind == TerminatorKind::Jump || terminator.kind == TerminatorKind::Branch;
        forEachEdge(reached[place], [&](const Edge& edge) {
            if (edge.to == head) {
                isSimple &= !back && terminator.kind == TerminatorKind::Jump;
                back = &edge;
            } else if (canBePart(edge.to) && !reached.contains(edge.to))
                reached.append(edge.to);
        });
        isSimple &= reached.size() <= mostBlocksInRotatingIteration;
    }
    if (!isSimple || !back || back->arguments().size() != parameters.size())
        return std::nullopt;
    Vector<BlockID, mostBlocksInRotatingIteration> members;
    for (bool grows = true; grows;) {
        grows = false;
        for (BlockID block : reached) {
            if (members.contains(block))
                continue;
            bool leadsBack = false;
            forEachEdge(block, [&](const Edge& edge) { leadsBack |= edge.to == head || members.contains(edge.to); });
            if (leadsBack) {
                members.append(block);
                grows = true;
            }
        }
    }
    for (size_t position = positionOfHead + 1; position < m_order.size() && position < positionOfHead + mostPositionsSearchedForIteration && loop.blocks.size() < members.size(); ++position) {
        if (members.contains(m_order[position]))
            loop.blocks.append(m_order[position]);
    }
    if (loop.blocks.size() != members.size() || loop.blocks.first() != intoIteration.to || (loop.bound && loop.blocks.contains(code().blockOf(loop.bound))))
        return std::nullopt;
    auto placeOf = [&](BlockID block) -> size_t { return std::ranges::find(loop.blocks, block) - loop.blocks.begin(); };

    std::optional<int64_t> start;
    bool hasOneStart = true;
    code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        if (edge.to != head || loop.blocks.contains(edge.from))
            return;
        std::optional<int64_t> entering = edge.arguments().size() == parameters.size() ? constantIntegerOf(edge.arguments()[loop.indexOfCounter]) : std::nullopt;
        hasOneStart &= entering && (!start || *start == *entering);
        start = entering;
    });
    if (!hasOneStart || !start || *start < 0 || end - *start > mostIterationsOfRotatingLoop || end - *start < fewestIterationsOfRotatingLoop)
        return std::nullopt;
    std::optional<int64_t> step = distanceFromCounter(loop, back->arguments()[loop.indexOfCounter]);
    if (!step || *step != 1)
        return std::nullopt;
    unsigned numberRotated = 0;
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID passed = wordPassedOn(back->arguments()[index]);
        numberRotated += passed != parameters[index] && std::ranges::contains(parameters, passed) && passed != loop.counter;
    }
    if (numberRotated < fewestParametersRotated)
        return std::nullopt;

    unsigned numberOfOperations = 0;
    unsigned numberOfBitOperations = 0;
    int64_t iterationsInRow = mostIterationsInRow;
    Vector<int64_t, 4> boundaries { *start, end };
    BitVector storesBefore;
    storesBefore.ensureSize(loop.blocks.size());
    for (size_t place = 0; place < loop.blocks.size(); ++place) {
        BlockID block = loop.blocks[place];
        bool hasStored = storesBefore.quickGet(place);
        for (ValueID value : code().valuesOf(block)) {
            const Operation& operation = code().at(value);
            numberOfBitOperations += bitOperationsOfRotatingIterations.contains(operation.opcode);
            if (++numberOfOperations > mostOperationsOfRotatingLoop || isNeverType(operation.type))
                return std::nullopt;
            bool isViewOfAnyKind = (operation.opcode == Opcode::GetTypedArrayElement || operation.opcode == Opcode::PutTypedArrayElement) && operation.typedArrayType() == NotTypedArray;
            if (isViewOfAnyKind)
                return std::nullopt;
            bool isElement = operation.opcode == Opcode::GetArrayElement || operation.opcode == Opcode::GetTypedArrayElement || storesInRotatingIterations.contains(operation.opcode);
            if (std::optional<PositionInRing> position = isElement ? positionInRing(loop, operation.positionOperand(), 0, 0) : std::nullopt)
                iterationsInRow = std::max(iterationsInRow, std::min(position->mask + 1, mostIterationsInRowOfRing));
            if (storesInRotatingIterations.contains(operation.opcode)) {
                const Type& stored = typeOf(operation.valueOperand());
                if (stored.hasObjects() || stored.primitives & cellPrimitives || (operation.opcode == Opcode::PutContiguousIndex && !operation.isInBounds()))
                    return std::nullopt;
                hasStored = true;
                continue;
            }
            if (readsInRotatingIterations.contains(operation.opcode))
                continue;
            if (!operation.traits().isPure())
                return std::nullopt;
        }
        const Terminator& terminator = code().at(block).terminator;
        ValueID condition = terminator.kind == TerminatorKind::Branch ? terminator.conditionOperand() : ValueID();
        if (condition && !code().isConstant(condition)) {
            BlockID home = code().blockOf(condition);
            bool holdsBelow = false;
            if (home != head && !loop.blocks.contains(home)) {
                if (loop.blocks.contains(code().at(terminator.taken()).to) && !loop.invariantConditions.contains(condition))
                    loop.invariantConditions.append(condition);
            } else if (std::optional<int64_t> boundary = boundaryOfRelation(loop, code().at(condition), holdsBelow); boundary && *boundary > *start && *boundary < end && !boundaries.contains(*boundary))
                boundaries.append(*boundary);
        }
        bool isOrdered = true;
        forEachEdge(block, [&](const Edge& edge) {
            if (edge.to == head)
                return;
            size_t target = placeOf(edge.to);
            isOrdered &= target == loop.blocks.size() ? !hasStored : target > place;
            if (target < loop.blocks.size() && hasStored)
                storesBefore.quickSet(target);
        });
        if (!isOrdered)
            return std::nullopt;
    }
    if (numberOfBitOperations < fewestBitOperationsInRotatingIteration)
        return std::nullopt;
    if (loop.bound) {
        if (boundaries.size() != 2 || end - *start <= mostIterationsInRow)
            return std::nullopt;
        loop.iterations = mostIterationsInRow;
    }
    std::ranges::sort(boundaries);
    for (int64_t iterations = iterationsInRow; iterations >= fewestIterationsInRow && !loop.iterations; iterations /= 2) {
        bool divides = static_cast<int64_t>(boundaries.size() - 1) * iterations <= mostIterationsEmittedOfRotatingLoop;
        for (size_t index = 1; index < boundaries.size(); ++index)
            divides &= !((boundaries[index] - boundaries[index - 1]) % iterations);
        if (divides)
            loop.iterations = iterations;
    }
    if (!loop.iterations)
        return std::nullopt;
    for (size_t index = 1; index < boundaries.size(); ++index) {
        loop.stages.append({ boundaries[index - 1], boundaries[index] });
        if (findRotatingIteration(loop, loop.stages.last()).blocks.last() != back->from)
            return std::nullopt;
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (representationOf(parameters[index]) == Representation::Int64)
            loop.indexesOfInt32s.append(index);
    }
    for (bool changes = true; changes;) {
        changes = loop.indexesOfInt32s.removeAllMatching([&](size_t index) {
            ValueID next = back->arguments()[index];
            if (code().isConstant(next))
                return true;
            if (opcodeOf(next) == Opcode::WidenInteger)
                return representationOf(code().at(next).valueOperand()) != Representation::Int32;
            size_t passed = std::ranges::find(parameters, next) - parameters.begin();
            return passed == parameters.size() || !loop.indexesOfInt32s.contains(passed);
        });
    }
    for (BlockID block : loop.blocks) {
        for (ValueID value : code().valuesOf(block)) {
            const Operation& access = code().at(value);
            if (storesInRotatingIterations.contains(access.opcode) || access.opcode == Opcode::GetArrayElement || access.opcode == Opcode::GetTypedArrayElement)
                loop.accesses.append({ access.opcode, access.representation, access.opcode == Opcode::GetArrayElement ? access.arrayStorage() : ArrayStorage(), access.opcode == Opcode::GetTypedArrayElement ? access.typedArrayType() : NotTypedArray });
        }
    }
    loop.position = code().at(test).origin().position;
    if (std::ranges::any_of(m_rotatingLoopsUnrolled, [&](const auto& unrolled) { return unrolled.first == loop.position && unrolled.second == loop.accesses; }))
        return std::nullopt;
    findRingReads(loop);
    findWordSums(loop);
    findMajorities(loop);
    return loop;
}

bool CodeGenerator::isReadForLowWordAlone(const RotatingLoop& loop, ValueID value, unsigned depth) const
{
    static constexpr unsigned mostDepth = 6;
    static constexpr OpcodeSet readersOfLowWord { Opcode::ToInt32, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    if (depth > mostDepth)
        return false;
    uint32_t uses = 0;
    for (BlockID block : loop.blocks) {
        for (ValueID user : code().valuesOf(block)) {
            const Operation& operation = code().at(user);
            uint32_t usesByUser = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t) { usesByUser += operand == value; });
            if (!usesByUser)
                continue;
            uses += usesByUser;
            if (readersOfLowWord.contains(operation.opcode) && (operation.opcode == Opcode::ToInt32 || operation.form() == OperatorForm::Integers || operation.form() == OperatorForm::Numbers))
                continue;
            if (!isSumOfWideWords(user) || !isReadForLowWordAlone(loop, user, depth + 1))
                return false;
        }
        const Terminator& terminator = code().at(block).terminator;
        for (uint32_t index = 0; index < terminator.numberOfEdges; ++index) {
            const Edge& edge = code().at(terminator.edge(index));
            for (size_t place = 0; place < edge.arguments().size(); ++place) {
                if (edge.arguments()[place] != value)
                    continue;
                ++uses;
                if (!loop.blocks.contains(edge.to) || !isReadForLowWordAlone(loop, code().at(edge.to).parameters()[place], depth + 1))
                    return false;
            }
        }
    }
    return uses == code().useCountOf(value);
}

bool CodeGenerator::isSumOfWideWords(ValueID value) const
{
    if (code().isConstant(value))
        return false;
    const Operation& operation = code().at(value);
    return operation.opcode == Opcode::Add && operation.form() == OperatorForm::Integers && operation.representation == Representation::Int64;
}

void CodeGenerator::findRingReads(RotatingLoop& loop) const
{
    for (BlockID block : loop.blocks) {
        const Terminator& terminator = code().at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        ValueID test = testReadByBranchAlone(block, terminator);
        if (!test || opcodeOf(test) != Opcode::TypeTest || representationOf(code().at(test).valueOperand()) != Representation::JSValue)
            continue;
        ValueID read = code().at(test).valueOperand();
        if (code().isConstant(read) || opcodeOf(read) != Opcode::GetArrayElement || code().blockOf(read) != block || code().useCountOf(read) != 2)
            continue;
        const Operation& element = code().at(read);
        if (element.arrayStorage() != ArrayStorage::Contiguous || element.level() != KnowledgeLevel::Proven || !element.isInBounds() || !element.storageOperand() || !positionInRing(loop, element.positionOperand(), 0, 0))
            continue;
        const Edge& passes = code().at(terminator.taken());
        if (!passes.arguments().empty() || !loop.blocks.contains(passes.to) || m_singlePredecessors[passes.to.index()] != block)
            continue;
        for (ValueID narrowed : code().valuesOf(passes.to)) {
            const Operation& operation = code().at(narrowed);
            if (operation.opcode != Opcode::Narrowed || operation.valueOperand() != read || operation.testOperand() != test || !operation.isOnTakenEdge())
                continue;
            ValueID unboxed = code().useCountOf(narrowed) == 1 ? code().onlyUserOf(narrowed) : ValueID();
            if (unboxed && opcodeOf(unboxed) == Opcode::Unbox && representationOf(unboxed) == Representation::Int64 && code().blockOf(unboxed) == passes.to && isReadForLowWordAlone(loop, unboxed, 0))
                loop.ringReads.append({ read, narrowed, unboxed, block, passes.to });
            break;
        }
    }
}

void CodeGenerator::findWordSums(RotatingLoop& loop) const
{
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> depths;
    auto depthOf = [&](ValueID value) -> unsigned { return code().isConstant(value) ? 0 : depths.get(value.index()); };
    for (BlockID block : loop.blocks) {
        for (ValueID parameter : code().at(block).parameters())
            depths.set(parameter.index(), 1);
        for (ValueID value : code().valuesOf(block)) {
            const Operation& operation = code().at(value);
            unsigned depth = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t) { depth = std::max(depth, depthOf(operand)); });
            depths.set(value.index(), operation.opcode == Opcode::GetArrayElement || operation.opcode == Opcode::Unbox ? 1 : depth + !conversionsOfPassedWords.contains(operation.opcode));
        }
    }
    auto isMember = [&](ValueID value) {
        if (!isSumOfWideWords(value) || !loop.blocks.contains(code().blockOf(value)) || code().useCountOf(value) != 1)
            return false;
        ValueID user = code().onlyUserOf(value);
        return user && isSumOfWideWords(user) && loop.blocks.contains(code().blockOf(user));
    };
    static constexpr size_t mostTerms = 12;
    for (BlockID block : loop.blocks) {
        for (ValueID root : code().valuesOf(block)) {
            if (!isSumOfWideWords(root) || isMember(root))
                continue;
            RotatingLoop::WordSum sum { root, { }, { } };
            Vector<ValueID, 12> pending { root };
            while (!pending.isEmpty() && sum.terms.size() <= mostTerms) {
                ValueID value = pending.takeLast();
                for (ValueID term : { code().at(value).rightOperand(), code().at(value).leftOperand() }) {
                    if (isMember(term)) {
                        pending.append(term);
                        sum.members.append(term);
                    } else
                        sum.terms.append(term);
                }
            }
            bool hasWords = std::ranges::all_of(sum.terms, [&](ValueID term) { return representationOf(term) == Representation::Int32 || representationOf(term) == Representation::Int64; });
            if (!hasWords || sum.terms.size() > mostTerms || !isReadForLowWordAlone(loop, root, 0))
                continue;
            std::ranges::reverse(sum.terms);
            std::ranges::stable_sort(sum.terms, { }, [&](ValueID term) {
                size_t place = code().isConstant(term) ? loop.blocks.size() : std::ranges::find(loop.blocks, code().blockOf(term)) - loop.blocks.begin();
                return std::pair { place == loop.blocks.size() ? 0 : place + 1, depthOf(term) };
            });
            loop.wordSums.append(WTF::move(sum));
        }
    }
}

void CodeGenerator::findMajorities(RotatingLoop& loop) const
{
    auto isBitOperation = [&](ValueID value, Opcode opcode, BlockID block) {
        return !code().isConstant(value) && opcodeOf(value) == opcode && code().blockOf(value) == block && code().at(value).representation == Representation::Int32 && (code().at(value).form() == OperatorForm::Integers || code().at(value).form() == OperatorForm::Numbers);
    };
    auto isWord = [&](ValueID value) { return representationOf(value) == Representation::Int32 || representationOf(value) == Representation::Int64; };
    auto isSingleBitAnd = [&](ValueID value, BlockID block) { return isBitOperation(value, Opcode::BitAnd, block) && code().useCountOf(value) == 1; };
    auto areOperands = [&](ValueID value, ValueID a, ValueID b) {
        const Operation& operation = code().at(value);
        return (operation.leftOperand() == a && operation.rightOperand() == b) || (operation.leftOperand() == b && operation.rightOperand() == a);
    };
    for (BlockID block : loop.blocks) {
        for (ValueID root : code().valuesOf(block)) {
            if (!isBitOperation(root, Opcode::BitXor, block))
                continue;
            ValueID left = code().at(root).leftOperand();
            ValueID right = code().at(root).rightOperand();
            RotatingLoop::Majority found { .root = root };
            for (auto [both, rest] : { std::pair { left, right }, std::pair { right, left } }) {
                if (found.a || !isSingleBitAnd(both, block))
                    continue;
                ValueID a = code().at(both).leftOperand();
                ValueID b = code().at(both).rightOperand();
                if (isSingleBitAnd(rest, block)) {
                    for (auto [c, either] : { std::pair { code().at(rest).leftOperand(), code().at(rest).rightOperand() }, std::pair { code().at(rest).rightOperand(), code().at(rest).leftOperand() } }) {
                        if (!found.a && isBitOperation(either, Opcode::BitXor, block) && areOperands(either, a, b)) {
                            found.a = a;
                            found.b = b;
                            found.c = c;
                            found.members = { both, rest };
                        }
                    }
                } else if (isBitOperation(rest, Opcode::BitXor, block) && code().useCountOf(rest) == 1 && isSingleBitAnd(code().at(rest).leftOperand(), block) && isSingleBitAnd(code().at(rest).rightOperand(), block)) {
                    ValueID first = code().at(rest).leftOperand();
                    ValueID second = code().at(rest).rightOperand();
                    for (ValueID c : { code().at(first).leftOperand(), code().at(first).rightOperand() }) {
                        if (!found.a && c != a && c != b && ((areOperands(first, a, c) && areOperands(second, b, c)) || (areOperands(first, b, c) && areOperands(second, a, c)))) {
                            found.a = a;
                            found.b = b;
                            found.c = c;
                            found.members = { both, rest, first, second };
                        }
                    }
                }
            }
            if (found.a && isWord(found.a) && isWord(found.b) && isWord(found.c))
                loop.majorities.append(WTF::move(found));
        }
    }
}

std::optional<CodeGenerator::PositionInRing> CodeGenerator::positionInRing(const RotatingLoop& loop, ValueID position, int64_t first, int64_t iteration) const
{
    if (code().isConstant(position) || opcodeOf(position) != Opcode::BitAnd || code().at(position).form() != OperatorForm::Integers)
        return std::nullopt;
    const Operation& operation = code().at(position);
    std::optional<int64_t> mask = constantIntegerOf(operation.rightOperand());
    std::optional<int64_t> distance = mask ? distanceFromCounter(loop, operation.leftOperand()) : std::nullopt;
    if (!distance || *mask <= 0 || *mask > INT32_MAX || !std::has_single_bit(static_cast<uint64_t>(*mask) + 1) || *distance < -*mask)
        return std::nullopt;
    if (loop.iterations) {
        int64_t size = *mask + 1;
        if (loop.stages.first().first % loop.iterations || (size > loop.iterations ? size % loop.iterations || size / loop.iterations > mostBasesOfRing : loop.iterations % size))
            return std::nullopt;
    }
    return PositionInRing { *mask, (first + iteration + *distance) & *mask };
}

std::optional<int64_t> CodeGenerator::valueInRotatingIteration(const RotatingLoop& loop, const RotatingLoop::Stage& stage, int64_t iteration, ValueID value) const
{
    if (code().isConstant(value))
        return std::nullopt;
    bool isOneRow = !loop.bound && stage.end - stage.first == loop.iterations;
    if (std::optional<int64_t> distance = distanceFromCounter(loop, value))
        return isOneRow ? std::optional<int64_t>(stage.first + iteration + *distance) : std::nullopt;
    std::optional<PositionInRing> position = positionInRing(loop, value, isOneRow ? stage.first : 0, iteration);
    return position && (isOneRow || position->mask < loop.iterations) ? std::optional<int64_t>(position->place) : std::nullopt;
}

CodeGenerator::RotatingIteration CodeGenerator::findRotatingIteration(const RotatingLoop& loop, const RotatingLoop::Stage& stage) const
{
    RotatingIteration found;
    BlockID head = m_order[loop.positionOfHead];
    Vector<std::pair<BlockID, unsigned>, 32> waysInto { { loop.blocks.first(), 1 } };
    Vector<BlockID, 8> pending { loop.blocks.first() };
    auto numberOfWaysInto = [&](BlockID block) -> unsigned {
        auto ways = std::ranges::find(waysInto, block, &std::pair<BlockID, unsigned>::first);
        return ways == waysInto.end() ? 0 : ways->second;
    };
    for (BlockID block : loop.blocks) {
        if (!numberOfWaysInto(block))
            continue;
        const Terminator& terminator = code().at(block).terminator;
        std::optional<uint32_t> known;
        ValueID condition = terminator.kind == TerminatorKind::Branch ? terminator.conditionOperand() : ValueID();
        bool holdsBelow = false;
        if (condition && loop.invariantConditions.contains(condition) && loop.blocks.contains(code().at(terminator.taken()).to))
            known = 0;
        else if (condition && !code().isConstant(condition)) {
            if (std::optional<int64_t> boundary = boundaryOfRelation(loop, code().at(condition), holdsBelow)) {
                if (stage.end <= *boundary)
                    known = !holdsBelow;
                else if (stage.first >= *boundary)
                    known = holdsBelow;
            }
        }
        if (known)
            found.knownEdges.append({ block, *known });
        BlockID last = found.blocks.isEmpty() ? BlockID() : found.blocks.last();
        found.blocks.append(block);
        pending.removeFirst(block);
        if (pending.isEmpty())
            found.blocksOfEveryIteration.append(block);
        bool continues = false;
        if (last && numberOfWaysInto(block) == 1) {
            const Terminator& before = code().at(last).terminator;
            auto knownBefore = std::ranges::find(found.knownEdges, last, &std::pair<BlockID, uint32_t>::first);
            if (before.kind == TerminatorKind::Jump)
                continues = code().at(before.next()).to == block;
            else if (knownBefore != found.knownEdges.end())
                continues = code().at(before.edge(knownBefore->second)).to == block;
        }
        if (continues)
            found.continuations.append(block);
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
            if (known && *known != edge)
                continue;
            BlockID target = code().at(terminator.edge(edge)).to;
            if (target == head || !loop.blocks.contains(target))
                continue;
            auto ways = std::ranges::find(waysInto, target, &std::pair<BlockID, unsigned>::first);
            if (ways == waysInto.end()) {
                waysInto.append({ target, 1 });
                pending.append(target);
            } else
                ++ways->second;
        }
    }
    return found;
}

NEVER_INLINE bool CodeGenerator::takesKnownEdge(const TerminatorContext& terminator)
{
    auto known = std::ranges::find(m_rotatingIteration->knownEdges, terminator.block(), &std::pair<BlockID, uint32_t>::first);
    if (known == m_rotatingIteration->knownEdges.end())
        return false;
    jumpTo(terminator, known->second, { });
    return true;
}

NEVER_INLINE void CodeGenerator::passToContinuation(const Edge& edge)
{
    std::span<const ValueID> parameters = code().at(edge.to).parameters();
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (!m_rotatingIteration->parametersOfContinuations.contains(parameter))
            continue;
        ValueID argument = edge.arguments()[index];
        m_values[parameter.index()] = (holdsAnyNaN(argument) && letsAnyNaNInto(parameter) ? getWithAnyNaN(argument) : getIn(argument, representationOf(parameter))).value;
    }
}

LValue CodeGenerator::wordInRotatingIteration(ValueID value)
{
    LValue word = getIn(value, representationOf(value)).value;
    return word->type() == Machine::Int64 ? m_out.castToInt32(word) : word;
}

void CodeGenerator::addWordSums()
{
    RotatingIteration& iteration = *m_rotatingIteration;
    auto isReady = [&](ValueID term) {
        if (code().isConstant(term))
            return true;
        BlockID home = code().blockOf(term);
        bool isOfIteration = iteration.loop->blocks.contains(home);
        return m_values[term.index()] && (!isOfIteration || (iteration.blocksBegun.contains(home) && iteration.blocksOfEveryIteration.contains(home)));
    };
    for (size_t index = 0; index < iteration.loop->wordSums.size(); ++index) {
        const RotatingLoop::WordSum& sum = iteration.loop->wordSums[index];
        RotatingIteration::PartialSum& partial = iteration.partialSums[index];
        if (partial.numberOfTerms == sum.terms.size() || (!partial.sum && !(isReady(sum.terms[0]) && isReady(sum.terms[1]))))
            continue;
        ValueContext context(code().blockOf(sum.root), sum.root, code().at(sum.root), nullptr);
        SetForScope<const OperationContext*> whileAdding(m_operation, &context);
        for (; partial.numberOfTerms < sum.terms.size() && isReady(sum.terms[partial.numberOfTerms]); ++partial.numberOfTerms) {
            LValue word = wordInRotatingIteration(sum.terms[partial.numberOfTerms]);
            partial.sum = partial.sum ? m_out.add(partial.sum, word) : word;
        }
    }
}

void CodeGenerator::noteStoredWord(ValueID store)
{
    ValueID stored = code().at(store).valueOperand();
    ValueID word = !code().isConstant(stored) && opcodeOf(stored) == Opcode::Box ? code().at(stored).valueOperand() : ValueID();
    if (!word || code().isConstant(word) || representationOf(word) != Representation::Int32 || m_valuesDefinedBoxed.get(word.index()))
        return;
    ValueContext context(code().blockOf(store), store, code().at(store), nullptr);
    SetForScope<const OperationContext*> whileExtending(m_operation, &context);
    m_rotatingIteration->storedWords.append({ store, m_out.signExt32To64(getIn(word, Representation::Int32).value) });
}

void CodeGenerator::beginAccessInRow(ValueID access)
{
    RotatingIteration& iteration = *m_rotatingIteration;
    auto address = std::ranges::find(iteration.addresses, access, &RotatingIteration::Address::access);
    iteration.address = address == iteration.addresses.end() ? nullptr : &*address;
}

void CodeGenerator::endAccessInRow()
{
    RotatingIteration& iteration = *m_rotatingIteration;
    iteration.address = nullptr;
    RELEASE_ASSERT(std::exchange(iteration.takesStart, false) == std::exchange(iteration.takesPlace, false));
}

LValue CodeGenerator::startOfRowSlow(ValueID access)
{
    RotatingIteration& iteration = *m_rotatingIteration;
    if (!iteration.address || iteration.address->access != access || !m_operation || m_operation->valueID() != access)
        return nullptr;
    iteration.takesStart = true;
    return iteration.address->start;
}

LValue CodeGenerator::placeInRowSlow(ValueID position)
{
    RotatingIteration& iteration = *m_rotatingIteration;
    if (!iteration.address || iteration.address->position != position || !m_operation || m_operation->valueID() != iteration.address->access)
        return nullptr;
    iteration.takesPlace = true;
    return iteration.address->place;
}

NEVER_INLINE bool CodeGenerator::generatesInRotatingIteration(ValueID id)
{
    RotatingIteration& iteration = *m_rotatingIteration;
    const bool isInOrder = !m_operation;
    if (isInOrder) {
        endAccessInRow();
        static constexpr OpcodeSet followsTest { Opcode::Narrowed, Opcode::Unbox };
        if (iteration.isInEveryIteration && !followsTest.contains(opcodeOf(id)))
            addWordSums();
        if (opcodeOf(id) == Opcode::PutContiguousIndex)
            noteStoredWord(id);
    }
    const auto& sums = iteration.loop->wordSums;
    if (auto sum = std::ranges::find(sums, id, &RotatingLoop::WordSum::root); sum != sums.end()) {
        RotatingIteration::PartialSum& partial = iteration.partialSums[sum - sums.begin()];
        ValueContext context(code().blockOf(id), id, code().at(id), nullptr);
        SetForScope<const OperationContext*> whileAdding(m_operation, &context);
        for (; partial.numberOfTerms < sum->terms.size(); ++partial.numberOfTerms) {
            LValue word = wordInRotatingIteration(sum->terms[partial.numberOfTerms]);
            partial.sum = partial.sum ? m_out.add(partial.sum, word) : word;
        }
        m_values[id.index()] = m_out.signExt32To64(partial.sum);
        return true;
    }
    if (auto majority = std::ranges::find(iteration.loop->majorities, id, &RotatingLoop::Majority::root); majority != iteration.loop->majorities.end()) {
        ValueContext context(code().blockOf(id), id, code().at(id), nullptr);
        SetForScope<const OperationContext*> whileChoosing(m_operation, &context);
        LValue a = wordInRotatingIteration(majority->a);
        LValue b = wordInRotatingIteration(majority->b);
        LValue c = wordInRotatingIteration(majority->c);
        auto differencesOf = [&](LValue x, LValue y) -> LValue {
            for (auto& [left, right, difference] : iteration.differences) {
                if ((left == x && right == y) || (left == y && right == x))
                    return difference;
            }
            return nullptr;
        };
        LValue known = differencesOf(b, c);
        LValue middle = known ? b : a;
        if (!known)
            known = differencesOf(a, c);
        LValue differences = m_out.bitXor(a, b);
        if (iteration.isInEveryIteration)
            iteration.differences.append({ a, b, differences });
        if (!known) {
            middle = b;
            known = m_out.bitXor(b, c);
            if (iteration.isInEveryIteration)
                iteration.differences.append({ b, c, known });
        }
        m_values[id.index()] = m_out.bitXor(middle, m_out.bitAnd(differences, known));
        return true;
    }
    if (isInOrder)
        beginAccessInRow(id);
    return false;
}

NEVER_INLINE void CodeGenerator::generateTerminatorOfRotatingIteration(const TerminatorContext& terminator)
{
    endAccessInRow();
    if (takesKnownEdge(terminator))
        return;
    if (terminator.kind() == TerminatorKind::Branch)
        beginAccessInRow(terminator->conditionOperand());
    dispatch(terminator);
    endAccessInRow();
}

void CodeGenerator::unrollRotatingLoop(RotatingLoop& loop)
{
    const bool checksBounds = emitter<ElementAccessEmitter>().checksBounds();
    if (checksBounds)
        loop.wordSums.shrink(0);
    build().statistics().count(StatisticID::RotatingLoopsUnrolled);
    m_rotatingLoopsUnrolled.append({ loop.position, loop.accesses });
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    using Values = Vector<LValue, 16>;
    LBasicBlock written = m_out.newBlock();
    Values phisOfWritten;
    Values current;
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        current.append(phi);
        phisOfWritten.append(phi ? m_out.phiIn(written, phi->type()) : nullptr);
    }
    Vector<LBasicBlock, 32> blocksOfWritten;
    Vector<std::pair<ValueID, LValue>, 8> phisOfBlocks;
    for (BlockID block : loop.blocks) {
        blocksOfWritten.append(m_blocks[block.index()]);
        for (ValueID parameter : code().at(block).parameters())
            phisOfBlocks.append({ parameter, m_values[parameter.index()] });
    }
    loop.indexesOfInt32s.removeAllMatching([&](size_t index) {
        return !current[index];
    });
    auto passOn = [&](const Values& values, const Values& phis, LBasicBlock target) {
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phis[index])
                m_out.upsilon(phis[index]->type() == values[index]->type() ? values[index] : m_out.castToInt32(values[index]), phis[index]);
        }
        m_out.jump(normalEdge(target));
    };

    m_out.appendTo(blockFor(head));
    m_out.setOrigin(Machine::Origin());
    TerminatorContext context(head, blockOfHead.terminator, nullptr);
    Vector<LValue, 2> conditions;
    LValue bound = nullptr;
    {
        SetForScope<const OperationContext*> whileDefining(m_operation, &context);
        for (ValueID condition : loop.invariantConditions)
            conditions.append(get(condition).value);
        if (loop.bound)
            bound = get(loop.bound).value;
        for (BlockID block : loop.blocks) {
            for (ValueID value : code().valuesOf(block)) {
                code().at(value).forEachOperand([&](ValueID operand, uint32_t) {
                    static constexpr OpcodeSet definedAtFirstUse { Opcode::GetArrayStorage, Opcode::Narrowed, Opcode::Box, Opcode::Unbox, Opcode::IntegerToNumber, Opcode::NumberToInteger, Opcode::WidenInteger, Opcode::NarrowInteger };
                    if (code().isConstant(operand) || m_values[operand.index()] || !definedAtFirstUse.contains(opcodeOf(operand)) || !hasRegister(representationOf(operand)))
                        return;
                    if (BlockID home = code().blockOf(operand); home != head && !loop.blocks.contains(home))
                        get(operand);
                });
            }
        }
    }
    for (LValue condition : conditions) {
        LBasicBlock here = m_out.m_block;
        LBasicBlock fails = m_out.newBlock();
        LBasicBlock holds = m_out.newBlock();
        m_out.appendTo(fails);
        passOn(current, phisOfWritten, written);
        m_out.appendTo(here);
        m_out.branch(condition, normalEdge(holds), normalEdge(fails));
        m_out.appendTo(holds);
    }
    if (!loop.indexesOfInt32s.isEmpty()) {
        LBasicBlock here = m_out.m_block;
        LBasicBlock fails = m_out.newBlock();
        m_out.appendTo(fails);
        passOn(current, phisOfWritten, written);
        m_out.appendTo(here);
        for (size_t index : loop.indexesOfInt32s) {
            LBasicBlock isInt32 = m_out.newBlock();
            LValue integer = m_out.signExt32To64(m_out.castToInt32(current[index]));
            m_out.branch(m_out.equal(integer, current[index]), normalEdge(isInt32), rareEdge(fails));
            m_out.appendTo(isInt32);
            current[index] = m_out.signExt32To64(m_out.castToInt32(current[index]));
        }
    }
    bool comesRound = true;
    for (const RotatingLoop::Stage& stage : loop.stages) {
        RotatingIteration iterationOfStage = findRotatingIteration(loop, stage);
        iterationOfStage.loop = &loop;
        for (auto [parameter, phi] : phisOfBlocks) {
            if (phi && iterationOfStage.continuations.contains(code().blockOf(parameter)))
                iterationOfStage.parametersOfContinuations.append(parameter);
        }
        bool isOneRow = !loop.bound && stage.end - stage.first == loop.iterations;
        LValue lastStart = nullptr;
        if (loop.bound) {
            LBasicBlock here = m_out.m_block;
            LBasicBlock tooFew = m_out.newBlock();
            LBasicBlock enough = m_out.newBlock();
            m_out.appendTo(tooFew);
            passOn(current, phisOfWritten, written);
            m_out.appendTo(here);
            lastStart = m_out.sub(bound, m_out.constInt32(loop.iterations));
            m_out.branch(m_out.lessThan(lastStart, m_out.constInt32(stage.first)), normalEdge(tooFew), normalEdge(enough));
            m_out.appendTo(enough);
        }
        Values phis;
        LBasicBlock row = nullptr;
        if (!isOneRow) {
            row = m_out.newBlock();
            for (size_t index = 0; index < parameters.size(); ++index)
                phis.append(current[index] ? m_out.phiIn(row, loop.indexesOfInt32s.contains(index) ? Machine::Int32 : current[index]->type()) : nullptr);
            passOn(current, phis, row);
            m_out.appendTo(row);
            for (size_t index = 0; index < parameters.size(); ++index)
                current[index] = loop.indexesOfInt32s.contains(index) ? m_out.signExt32To64(phis[index]) : phis[index];
        }
        LValue firstCounter = isOneRow ? m_out.constInt32(stage.first) : phis[loop.indexOfCounter];
        auto counterAt = [&](int64_t iteration) -> LValue {
            if (isOneRow)
                return m_out.constInt32(stage.first + iteration);
            if (!iteration)
                return firstCounter;
            return m_out.castToInt32(m_out.add(m_out.extendWordThatIsNotNegative(firstCounter), m_out.constInt64(iteration)));
        };
        Vector<std::tuple<int64_t, int64_t, LValue>, 4> bases;
        auto baseOf = [&](const PositionInRing& position) -> LValue {
            int64_t start = position.place / loop.iterations * loop.iterations;
            for (auto& [mask, placeOfStart, base] : bases) {
                if (mask == position.mask && placeOfStart == start)
                    return base;
            }
            LValue base = m_out.extendWordThatIsNotNegative(m_out.bitAnd(start ? m_out.add(firstCounter, m_out.constInt32(start)) : firstCounter, m_out.constInt32(position.mask)));
            bases.append({ position.mask, start, base });
            return base;
        };
        struct StartOfRow {
            ValueID storage;
            LValue base { nullptr };
            LValue address { nullptr };
        };
        Vector<StartOfRow, 4> starts;
        auto startOf = [&](ValueID storage, LValue base) -> LValue {
            for (const StartOfRow& start : starts) {
                if (start.storage == storage && start.base == base)
                    return start.address;
            }
            SetForScope<const OperationContext*> whileAddressing(m_operation, &context);
            LValue address = m_out.opaque(m_out.add(get(storage).value, m_out.shl(base, m_out.constInt32(3))));
            starts.append({ storage, base, address });
            return address;
        };
        auto addressOf = [&](ValueID access, int64_t iteration) -> std::optional<RotatingIteration::Address> {
            const Operation& operation = code().at(access);
            static constexpr OpcodeSet accesses { Opcode::GetArrayElement, Opcode::IsElementPresent, Opcode::PutContiguousIndex };
            if (isOneRow || checksBounds || !accesses.contains(operation.opcode) || !operation.isInBounds() || (operation.opcode == Opcode::GetArrayElement && (operation.level() != KnowledgeLevel::Proven || operation.asksCellWhereNothingIsFound())))
                return std::nullopt;
            ValueID storage = operation.storageOperand();
            ValueID position = operation.positionOperand();
            if (!storage || code().isConstant(storage) || code().isConstant(position) || representationOf(storage) != Representation::RawWord || code().blockOf(storage) == head || loop.blocks.contains(code().blockOf(storage)))
                return std::nullopt;
            if (std::optional<PositionInRing> inRing = positionInRing(loop, position, 0, iteration); inRing && inRing->mask >= loop.iterations)
                return RotatingIteration::Address { access, storage, position, startOf(storage, baseOf(*inRing)), m_out.constInt64(inRing->place % loop.iterations) };
            if (std::optional<int64_t> distance = distanceFromCounter(loop, position); distance && *distance >= 0)
                return RotatingIteration::Address { access, storage, position, startOf(storage, m_out.extendWordThatIsNotNegative(firstCounter)), m_out.constInt64(iteration + *distance) };
            return std::nullopt;
        };
        struct KnownElement {
            ValueID storage;
            int64_t mask { 0 };
            int64_t place { 0 };
            LValue word { nullptr };
        };
        Vector<KnownElement, 16> knownElements;
        for (int64_t iteration = 0; iteration < loop.iterations && !isOneRow; ++iteration) {
            for (BlockID block : iterationOfStage.blocks) {
                for (ValueID value : code().valuesOf(block)) {
                    if (std::optional<PositionInRing> position = positionInRing(loop, value, 0, iteration); position && position->mask >= loop.iterations)
                        baseOf(*position);
                    addressOf(value, iteration);
                }
            }
        }
        for (int64_t iteration = 0; iteration < loop.iterations; ++iteration) {
            auto placeInRow = [&](ValueID position) { return positionInRing(loop, position, isOneRow ? stage.first : 0, iteration); };
            for (BlockID block : loop.blocks) {
                for (ValueID parameter : code().at(block).parameters())
                    m_values[parameter.index()] = nullptr;
                for (ValueID value : code().valuesOf(block))
                    m_values[value.index()] = nullptr;
            }
            current[loop.indexOfCounter] = counterAt(iteration);
            for (size_t index = 0; index < parameters.size(); ++index)
                m_values[parameters[index].index()] = phisOfWritten[index] ? current[index] : nullptr;
            if (loop.poll && !iteration) {
                generateValue(head, loop.poll, code().at(loop.poll), false, { });
                m_out.setOrigin(Machine::Origin());
            }
            iterationOfStage.addresses.shrink(0);
            iterationOfStage.storedWords.shrink(0);
            iterationOfStage.blocksBegun.shrink(0);
            iterationOfStage.partialSums.fill({ }, loop.wordSums.size());
            for (BlockID block : iterationOfStage.blocks) {
                for (ValueID value : code().valuesOf(block)) {
                    if (std::optional<RotatingIteration::Address> address = addressOf(value, iteration))
                        iterationOfStage.addresses.append(*address);
                    ValueID user = code().useCountOf(value) == 1 ? code().onlyUserOf(value) : ValueID();
                    if (user && loop.blocks.contains(code().blockOf(user)) && placeInRow(user) && (representationOf(user) == Representation::Int32 || representationOf(user) == Representation::Int64)) {
                        m_valuesLeftOutOfTurnsLaidOut.append(value);
                        continue;
                    }
                    if (representationOf(value) != Representation::Int32 && representationOf(value) != Representation::Int64)
                        continue;
                    if (std::optional<int64_t> known = valueInRotatingIteration(loop, stage, iteration, value)) {
                        m_values[value.index()] = m_out.constantOfType(machineTypeOf(representationOf(value)), *known);
                        m_valuesLeftOutOfTurnsLaidOut.append(value);
                    } else if (std::optional<PositionInRing> position = isOneRow ? std::nullopt : placeInRow(value)) {
                        LValue place = m_out.add(baseOf(*position), m_out.constInt64(position->place % loop.iterations));
                        m_values[value.index()] = representationOf(value) == Representation::Int64 ? place : m_out.castToInt32(place);
                        m_valuesLeftOutOfTurnsLaidOut.append(value);
                    }
                }
            }
            for (const RotatingLoop::WordSum& sum : loop.wordSums)
                m_valuesLeftOutOfTurnsLaidOut.appendVector(sum.members);
            for (const RotatingLoop::Majority& majority : loop.majorities)
                m_valuesLeftOutOfTurnsLaidOut.appendVector(majority.members);
            m_valuesLeftOutOfTurnsLaidOut.append(loop.counter);
            size_t numberOfKnownEdges = iterationOfStage.knownEdges.size();
            m_headOfTurnsLaidOut = head;
            m_turnLaidOut = loop.blocks.span();
            m_rotatingIteration = &iterationOfStage;
            Values ahead = current;
            LBasicBlock leaving = nullptr;
            m_blockLeavingTurnLaidOut = [&] {
                if (leaving)
                    return leaving;
                LBasicBlock here = m_out.m_block;
                leaving = m_out.newBlock();
                m_out.appendTo(leaving);
                Values values;
                for (size_t index = 0; index < parameters.size(); ++index) {
                    LValue value = ahead[index];
                    if (index == loop.indexOfCounter)
                        value = isOneRow ? m_out.constInt32(stage.first + iteration) : iteration ? m_out.add(firstCounter, m_out.constInt32(iteration)) : firstCounter;
                    else if (value && value->opcode() == Machine::SExt32)
                        value = m_out.signExt32To64(value->child(0));
                    values.append(value);
                }
                passOn(values, phisOfWritten, written);
                m_out.appendTo(here);
                return leaving;
            };
            for (BlockID block : loop.blocks)
                m_blocks[block.index()] = nullptr;
            m_blocks[loop.blocks.first().index()] = m_out.m_block;
            m_nextBlockGoesOnHere = true;
            for (BlockID block : iterationOfStage.blocks.span().subspan(1)) {
                if (!iterationOfStage.continuations.contains(block))
                    m_blocks[block.index()] = m_out.newBlock();
            }
            for (auto [parameter, phi] : phisOfBlocks) {
                if (LBasicBlock block = m_blocks[code().blockOf(parameter).index()]; phi && block)
                    m_values[parameter.index()] = m_out.phiIn(block, phi->type());
            }
            for (BlockID block : iterationOfStage.blocks) {
                for (const RotatingLoop::RingRead& read : loop.ringReads) {
                    if (read.block != block)
                        continue;
                    std::optional<PositionInRing> position = placeInRow(code().at(read.element).positionOperand());
                    auto known = std::ranges::find_if(knownElements, [&](const KnownElement& element) { return position && element.storage == code().at(read.element).storageOperand() && element.mask == position->mask && element.place == position->place; });
                    if (known == knownElements.end())
                        continue;
                    m_values[read.unboxed.index()] = known->word;
                    m_valuesLeftOutOfTurnsLaidOut.appendList({ read.element, read.narrowed, read.unboxed });
                    iterationOfStage.knownEdges.append({ block, 0 });
                }
                if (LBasicBlock next = m_blocks[block.index()]; !m_nextBlockGoesOnHere && (!next || !next->numberOfIncomingEdges())) {
                    if (next)
                        m_blocksLeftOut.append(next);
                    continue;
                }
                if (!std::exchange(m_nextBlockGoesOnHere, true)) {
                    if (BlockID predecessor = m_singlePredecessors[block.index()])
                        m_out.appendToSuccessor(blockFor(block), m_scopeAtEndOfBlock[predecessor.index()]);
                    else
                        m_out.appendTo(blockFor(block));
                    m_out.setOrigin(Machine::Origin());
                }
                iterationOfStage.blocksBegun.append(block);
                iterationOfStage.isInEveryIteration = iterationOfStage.blocksOfEveryIteration.contains(block);
                generateBlock(block);
                if (m_hasFailed)
                    return;
                m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
                for (const RotatingLoop::RingRead& read : loop.ringReads) {
                    std::optional<PositionInRing> position = read.blockOfConversion == block && !m_valuesLeftOutOfTurnsLaidOut.contains(read.unboxed) ? placeInRow(code().at(read.element).positionOperand()) : std::nullopt;
                    if (position && iterationOfStage.isInEveryIteration && m_values[read.unboxed.index()])
                        knownElements.append({ code().at(read.element).storageOperand(), position->mask, position->place, m_values[read.unboxed.index()] });
                }
                for (ValueID store : code().valuesOf(block)) {
                    const Operation& operation = code().at(store);
                    if (!storesInRotatingIterations.contains(operation.opcode))
                        continue;
                    std::optional<PositionInRing> position = operation.opcode == Opcode::PutContiguousIndex ? placeInRow(operation.positionOperand()) : std::nullopt;
                    knownElements.removeAllMatching([&](const KnownElement& element) {
                        return !position || element.storage != operation.storageOperand() || element.mask != position->mask || element.place == position->place;
                    });
                    auto stored = std::ranges::find(iterationOfStage.storedWords, store, &std::pair<ValueID, LValue>::first);
                    if (position && iterationOfStage.isInEveryIteration && stored != iterationOfStage.storedWords.end())
                        knownElements.append({ operation.storageOperand(), position->mask, position->place, stored->second });
                }
            }
            iterationOfStage.knownEdges.shrink(numberOfKnownEdges);
            comesRound = std::exchange(m_nextBlockGoesOnHere, false);
            RELEASE_ASSERT(comesRound == currentBlockIsOpen() && (!comesRound || m_argumentsForNextTurn.size() == parameters.size()));
            m_headOfTurnsLaidOut = BlockID();
            m_turnLaidOut = { };
            m_rotatingIteration = nullptr;
            m_blockLeavingTurnLaidOut = nullptr;
            m_valuesLeftOutOfTurnsLaidOut.shrink(0);
            m_out.setOrigin(Machine::Origin());
            if (!comesRound)
                break;
            for (size_t index = 0; index < parameters.size(); ++index) {
                if (phisOfWritten[index])
                    current[index] = m_argumentsForNextTurn[index].value;
            }
        }
        if (!comesRound)
            break;
        Values next = current;
        next[loop.indexOfCounter] = m_out.add(firstCounter, m_out.constInt32(loop.iterations));
        current[loop.indexOfCounter] = loop.bound ? next[loop.indexOfCounter] : m_out.constInt32(stage.end);
        if (isOneRow)
            continue;
        LBasicBlock here = m_out.m_block;
        LBasicBlock again = m_out.newBlock();
        LBasicBlock behind = m_out.newBlock();
        m_out.appendTo(again);
        passOn(next, phis, row);
        m_out.appendTo(here);
        LValue goesOn = loop.bound ? m_out.lessThanOrEqual(next[loop.indexOfCounter], lastStart) : m_out.lessThan(next[loop.indexOfCounter], m_out.constInt32(stage.end));
        m_out.branch(goesOn, normalEdge(again), normalEdge(behind));
        m_out.appendTo(behind);
    }
    if (comesRound)
        passOn(current, phisOfWritten, written);

    for (size_t place = 0; place < loop.blocks.size(); ++place) {
        BlockID block = loop.blocks[place];
        for (ValueID value : code().valuesOf(block))
            m_values[value.index()] = nullptr;
        m_blocks[block.index()] = blocksOfWritten[place];
        m_scopeAtEndOfBlock[block.index()] = ScopeFork();
    }
    for (auto [parameter, phi] : phisOfBlocks)
        m_values[parameter.index()] = phi;
    m_blocks[head.index()] = written;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfWritten[index];
    if (loop.poll)
        m_values[loop.poll.index()] = nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AirCode.h"
#include "AirGenerate.h"
#include "B3BasicBlockInlines.h"
#include "B3Generate.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "LinkBuffer.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

Lowering::Lowering(Procedure& proc, const Program& program, FunctionID id, const Function& function, LoweringContext& context)
    : Emitter(proc)
    , m_program(program)
    , m_function(function)
    , m_id(id)
    , m_isEntry(program.isEntry(id))
    , m_context(context)
    , m_builtins(Builtins::singleton())
    , m_length(program.find("length"_s.span8()).value_or(StringID()))
{
}

LType Lowering::b3TypeOf(Rep rep)
{
    switch (rep) {
    case Rep::Void:
        return Void;
    case Rep::Int32:
        return Int32;
    case Rep::Int64:
    case Rep::OptionalInt64:
    case Rep::Cell:
    case Rep::JSValue:
        return Int64;
    case Rep::Double:
    case Rep::OptionalDouble:
        return Double;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Less likely than its sibling, and not rare: it comes behind that in the code, among the blocks that are run.
LBasicBlock Lowering::newUnlikelyBlock()
{
    LBasicBlock block = m_out.newBlock();
    block->setFrequency(std::min(block->frequency(), 0.25));
    return block;
}

LBasicBlock Lowering::newColdBlock()
{
    LBasicBlock block = m_out.newBlock();
    block->setFrequency(0);
    return block;
}

Integer Lowering::integer(int64_t value)
{
    m_context.wideIntegerConstants.add(value);
    return Integer(m_out.constInt64(value));
}

// Where control goes from there. A condition whose type leaves one outcome is not tested.
std::span<const Target> Lowering::successorsOf(const Terminator& terminator) const
{
    std::span<const Target> targets = terminator.targets.span();
    if (terminator.kind != TerminatorKind::Branch)
        return targets;
    uint16_t primitives = typeOf(terminator.value).primitives;
    bool mayBeTrue = primitives & Primitive::True;
    bool mayBeFalse = primitives & Primitive::False;
    return targets.subspan(mayBeTrue ? 0 : 1, mayBeTrue + mayBeFalse);
}

Vector<BlockID> Lowering::reversePostorder() const
{
    Vector<BlockID> order;
    BitVector seen(m_function.blocks.size());
    Vector<std::pair<BlockID, unsigned>> stack;
    seen.set(0);
    stack.append({ BlockID(0u), 0 });
    while (!stack.isEmpty()) {
        auto& [block, next] = stack.last();
        auto targets = successorsOf(m_function.at(block).terminator);
        // From the last target to the first, so that the first comes right after the block.
        if (next < targets.size()) {
            BlockID successor = targets[targets.size() - 1 - next++].block;
            if (!seen.set(successor.index()))
                stack.append({ successor, 0 });
            continue;
        }
        order.append(block);
        stack.removeLast();
    }
    order.reverse();
    return order;
}

LBasicBlock Lowering::edgeTo(const Target& target, bool isBranch)
{
    auto& parameters = m_function.at(target.block).parameters;
    Vector<LValue, 4> arguments;
    Vector<std::pair<LValue, LValue>, 2> cells;
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        Operand argument = in(target.arguments[i], m_function.at(parameters[i]).rep);
        if (LValue phi = m_cellPhis.get(parameters[i].index())) {
            Parts parts = partsOf(argument.as<Boxed>());
            arguments.append(parts.word);
            cells.append({ phi, parts.cell });
            continue;
        }
        arguments.append(argument.value);
    }
    Vector<std::pair<LValue, LValue>, 2> truthiness;
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (LValue phi = m_truthinessPhis.get(parameters[i].index()))
            truthiness.append({ phi, lowerToBoolean(target.arguments[i]).value() });
    }
    Vector<std::pair<LValue, LValue>, 2> integers;
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (LValue phi = m_integerPhis.get(parameters[i].index()))
            integers.append({ phi, integerOrMarker(target.arguments[i]).value() });
    }
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (LValue phi = m_int32Phis.get(parameters[i].index()))
            integers.append({ phi, toInt32(target.arguments[i]).value() });
    }
    auto& phis = m_phis[target.block.index()];
    LBasicBlock block = m_blocks[target.block.index()];
    auto anchorAll = [&] {
        for (unsigned i = 0; i < arguments.size(); ++i) {
            // The back end takes a phi for what comes in where that is one value, so it is a cell no earlier than that value is.
            if (!m_typeOfCell.contains(arguments[i]))
                m_typeOfCell.remove(phis[i]);
            m_out.addIncomingToPhi(phis[i], m_out.anchor(arguments[i]));
        }
        for (auto [phi, isTruthy] : truthiness)
            m_out.addIncomingToPhi(phi, m_out.anchor(isTruthy));
        for (auto [phi, kept] : integers)
            m_out.addIncomingToPhi(phi, m_out.anchor(kept));
        for (auto [phi, cell] : cells)
            m_out.addIncomingToPhi(phi, m_out.anchor(cell));
    };
    if (!isBranch || arguments.isEmpty()) {
        anchorAll();
        return block;
    }
    LBasicBlock here = m_out.m_block;
    LBasicBlock edge = m_out.newBlock();
    edge->setFrequency(block->frequency());
    m_out.appendTo(edge);
    anchorAll();
    m_out.jump(block);
    m_out.appendTo(here);
    return edge;
}

std::optional<unsigned> Lowering::nilSuccessorOf(const Terminator& terminator) const
{
    bool isNegated = false;
    ValueID id = terminator.value;
    for (; m_function.at(id).opcode == Opcode::LogicalNot; id = m_function.at(id).child(0))
        isNegated = !isNegated;
    auto& condition = m_function.at(id);
    constexpr uint16_t cells = Primitive::String | Primitive::Symbol | Primitive::BigInt;
    auto isReference = [&](ValueID operand, uint16_t others) {
        auto& type = m_program.typeOf(m_function, operand);
        return type.primitives & Primitive::UndefinedOrNull && type.hasCells() && !(type.primitives & ~(Primitive::UndefinedOrNull | others));
    };
    auto isNil = [&](ValueID operand) {
        return m_function.at(operand).opcode == Opcode::Undefined || m_function.at(operand).opcode == Opcode::Null;
    };
    bool isTrueOfNil;
    switch (condition.opcode) {
    case Opcode::IsUndefinedOrNull:
        if (!isReference(condition.child(0), cells))
            return std::nullopt;
        isTrueOfNil = true;
        break;
    case Opcode::StrictEq:
    case Opcode::Eq:
        if (!(isNil(condition.child(1)) && isReference(condition.child(0), cells)) && !(isNil(condition.child(0)) && isReference(condition.child(1), cells)))
            return std::nullopt;
        isTrueOfNil = true;
        break;
    case Opcode::ToBoolean:
        // An empty string is no nil.
        if (!isReference(condition.child(0), 0))
            return std::nullopt;
        isTrueOfNil = false;
        break;
    default:
        return std::nullopt;
    }
    return isTrueOfNil != isNegated ? 0 : 1;
}

std::optional<std::pair<ValueID, unsigned>> Lowering::nilTestOf(const Terminator& terminator) const
{
    bool isNegated = false;
    ValueID id = terminator.value;
    for (; m_function.at(id).opcode == Opcode::LogicalNot; id = m_function.at(id).child(0))
        isNegated = !isNegated;
    auto& condition = m_function.at(id);
    ValueID tested;
    bool isTrueOfNil = true;
    switch (condition.opcode) {
    case Opcode::IsUndefinedOrNull:
        tested = condition.child(0);
        break;
    case Opcode::StrictEq:
    case Opcode::Eq:
        for (auto [operand, constant] : { std::pair { condition.child(0), condition.child(1) }, std::pair { condition.child(1), condition.child(0) } }) {
            Opcode opcode = m_function.at(constant).opcode;
            if (opcode != Opcode::Undefined && opcode != Opcode::Null)
                continue;
            // `===` tells one of the two from everything else.
            if (condition.opcode == Opcode::StrictEq && m_program.typeOf(m_function, operand).primitives & (opcode == Opcode::Undefined ? Primitive::Null : Primitive::Undefined))
                continue;
            tested = operand;
        }
        break;
    // What is true is neither.
    case Opcode::ToBoolean:
        tested = condition.child(0);
        isTrueOfNil = false;
        break;
    default:
        break;
    }
    if (!tested)
        return std::nullopt;
    return std::pair { tested, isTrueOfNil != isNegated ? 1u : 0u };
}

static DFG::Node* originForMap(const Function&, const Value&);

void Lowering::lowerTerminator(const Terminator& terminator)
{
    openStringsInFrontOfLoopsEnteredBy(terminator);
    switch (terminator.kind) {
    case TerminatorKind::Unreachable:
        // What is said never to come back may, where what it is said of is not what is there.
        if (Options::auditAOTTypedAssertions()) [[unlikely]]
            AOT_TYPED_OPERATION(ThrowReachedUnreachable);
        m_out.unreachable();
        return;
    case TerminatorKind::Jump:
        m_out.jump(edgeTo(terminator.targets[0], false));
        return;
    case TerminatorKind::Branch: {
        auto successors = successorsOf(terminator);
        if (successors.empty()) {
            m_out.unreachable();
            return;
        }
        if (successors.size() == 1) {
            m_out.jump(edgeTo(successors[0], false));
            return;
        }
        if (isFetchedWhereNeeded(terminator.value)) {
            lowerBranchOnText();
            return;
        }
        if (m_isCarriedOutByBranch.get(terminator.value.index())) {
            LBasicBlock isOne = edgeTo(terminator.targets[0], true);
            LBasicBlock isNone = edgeTo(terminator.targets[1], true);
            if (Options::aotMapFilePath()) [[unlikely]]
                m_out.setOrigin(originForMap(m_function, m_function.at(terminator.value)));
            branchOnClosureOf(m_function.at(terminator.value), isOne, isNone);
            return;
        }
        Word32 condition = get(terminator.value).as<Word32>();
        LBasicBlock taken = edgeTo(terminator.targets[0], true);
        LBasicBlock notTaken = edgeTo(terminator.targets[1], true);
        // The program's own test holds wherever only that edge leads: see hasPassedNilTest().
        if (auto test = nilTestOf(terminator); test && terminator.targets[test->second].block.index() && numberOfPredecessorsOf(terminator.targets[test->second].block) == 1) {
            ValueID tested = test->first;
            while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
                tested = m_function.at(tested).child(0);
            m_passedNilTestIn.add(tested.index(), Vector<BlockID, 1> { }).iterator->value.append(terminator.targets[test->second].block);
        }
        if (auto& test = m_function.at(terminator.value); test.opcode == Opcode::IsUndefinedOrNull && terminator.targets[0].block.index() && terminator.targets[0].block != terminator.targets[1].block && numberOfPredecessorsOf(terminator.targets[0].block) == 1) {
            ValueID tested = test.child(0);
            while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
                tested = m_function.at(tested).child(0);
            m_foundNilIn.set(terminator.targets[0].block.index(), tested);
        }
        // For the order of the code only: a reference is expected to refer to something.
        if (auto nilSuccessor = nilSuccessorOf(terminator)) {
            LBasicBlock& ifNil = *nilSuccessor ? notTaken : taken;
            if (ifNil->frequency() >= 1) {
                LBasicBlock here = m_out.m_block;
                LBasicBlock edge = newUnlikelyBlock();
                m_out.appendTo(edge);
                m_out.jump(ifNil);
                m_out.appendTo(here);
                ifNil = edge;
            }
        }
        if (m_blocksThatEvaluateModules.get(terminator.targets[0].block.index()) && notTaken->frequency())
            m_out.branch(condition.value(), rarely(taken), usually(notTaken));
        else if (m_blocksThatEvaluateModules.get(terminator.targets[1].block.index()) && taken->frequency())
            m_out.branch(condition.value(), usually(taken), rarely(notTaken));
        else
            m_out.branch(condition.value(), unsure(taken), unsure(notTaken));
        return;
    }
    case TerminatorKind::Return: {
        if (m_isEntry) {
            m_out.ret(undefined().value());
            return;
        }
        if (!m_function.resultTypes.isEmpty()) {
            // The part of a split function that is entered jumps to a continuation, which returns: nothing comes back here.
            if (m_function.at(terminator.value).isJumpToContinuation())
                m_out.unreachable();
            else
                returnResults(m_function.at(terminator.value));
            return;
        }
        Rep rep = resultRepOf(m_program, m_id);
        if (rep == Rep::Void) {
            m_out.m_block->clearSuccessors();
            m_out.m_block->appendNew<B3::Value>(m_proc, B3::Return, m_out.origin());
            return;
        }
        if (terminator.value)
            m_out.ret(in(terminator.value, rep).value);
        else
            m_out.ret(convert(undefined(), Type::ofPrimitives(Primitive::Undefined), rep).value);
        return;
    }
    case TerminatorKind::Throw: {
        Boxed thrown = in<Boxed>(terminator.value);
        if (!terminator.targets.isEmpty())
            spillForCatch(terminator.targets[0]);
        AOT_TYPED_OPERATION(Throw, thrown);
        m_currentCatchEntrypoint = std::nullopt;
        m_out.unreachable();
        return;
    }
    case TerminatorKind::Invoke:
        // The other edge is taken by whoever unwinds the stack.
        m_out.jump(edgeTo(terminator.targets[0], false));
        return;
    case TerminatorKind::Switch: {
        Integer value = in<Integer>(terminator.value);
        Vector<FTL::SwitchCase> cases;
        for (unsigned i = 0; i < terminator.cases.size(); ++i)
            cases.append(FTL::SwitchCase(m_out.constInt64(terminator.cases[i]), edgeTo(terminator.targets[i], true), FTL::Weight()));
        m_out.switchInstruction(value.value(), cases, edgeTo(terminator.targets.last(), true), FTL::Weight());
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The blocks from which an edge that goes back can be reached without passing the block it goes back to.
void Lowering::computeLoopBlocks(const Vector<BlockID>& order)
{
    unsigned numberOfBlocks = m_function.blocks.size();
    Vector<unsigned> place(numberOfBlocks);
    for (unsigned i = 0; i < order.size(); ++i)
        place[order[i].index()] = i;
    Vector<Vector<BlockID, 2>> predecessors(numberOfBlocks);
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator))
            predecessors[target.block.index()].append(block);
    }
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator)) {
            if (place[target.block.index()] > place[block.index()])
                continue;
            BitVector seen(numberOfBlocks);
            seen.set(target.block.index());
            Vector<BlockID, 16> worklist;
            if (!seen.set(block.index()))
                worklist.append(block);
            while (!worklist.isEmpty()) {
                for (BlockID predecessor : predecessors[worklist.takeLast().index()]) {
                    if (!seen.set(predecessor.index()))
                        worklist.append(predecessor);
                }
            }
            m_loopBlocks.merge(seen);
        }
    }
}

// Of the blocks of the function, not of what they are lowered to.
void Lowering::computeDominators(const Vector<BlockID>& order)
{
    unsigned numberOfBlocks = m_function.blocks.size();
    static constexpr unsigned unknown = UINT_MAX;
    Vector<unsigned> place(numberOfBlocks);
    for (unsigned i = 0; i < order.size(); ++i)
        place[order[i].index()] = i;
    Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator))
            predecessors[target.block.index()].append(block.index());
    }
    Vector<unsigned> dominator;
    dominator.fill(unknown, numberOfBlocks);
    dominator[order[0].index()] = order[0].index();
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i = 1; i < order.size(); ++i) {
            unsigned found = unknown;
            for (unsigned other : predecessors[order[i].index()]) {
                if (dominator[other] == unknown)
                    continue;
                if (found == unknown) {
                    found = other;
                    continue;
                }
                while (found != other) {
                    while (place[other] > place[found])
                        other = dominator[other];
                    while (place[found] > place[other])
                        found = dominator[found];
                }
            }
            if (dominator[order[i].index()] != found) {
                dominator[order[i].index()] = found;
                changed = true;
            }
        }
    }
    Vector<Vector<unsigned, 2>> dominated(numberOfBlocks);
    for (unsigned i = 1; i < order.size(); ++i)
        dominated[dominator[order[i].index()]].append(order[i].index());
    m_dominatorTreeEnter.fill(0, numberOfBlocks);
    m_dominatorTreeLeave.fill(0, numberOfBlocks);
    unsigned clock = 0;
    Vector<std::pair<unsigned, unsigned>> stack { { order[0].index(), 0u } };
    m_dominatorTreeEnter[order[0].index()] = clock++;
    while (!stack.isEmpty()) {
        auto& [block, next] = stack.last();
        if (next == dominated[block].size()) {
            m_dominatorTreeLeave[block] = clock++;
            stack.removeLast();
            continue;
        }
        unsigned child = dominated[block][next++];
        m_dominatorTreeEnter[child] = clock++;
        stack.append({ child, 0u });
    }
}

// Of the value, or of what it is a cast of, but for a Coerce, which may make something else of its operand. The value that is being lowered is kept as its declared type says.
bool Lowering::hasPassedNilTest(ValueID id) const
{
    if (m_passedNilTestIn.isEmpty() || !m_currentBlock || (m_currentIndexInBlock < m_currentBlock->values.size() && m_currentBlock->values[m_currentIndexInBlock] == id))
        return false;
    for (ValueID value = id;; value = m_function.at(value).child(0)) {
        if (auto found = m_passedNilTestIn.find(value.index()); found != m_passedNilTestIn.end()) {
            for (BlockID block : found->value) {
                if (dominates(block, m_currentBlockID))
                    return true;
            }
        }
        if (m_function.at(value).opcode != Opcode::Cast || m_function.at(value).castReason() == CastReason::Coerce)
            return false;
    }
}

const Type& Lowering::withoutNil(TypeID type) const
{
    return *m_typesWithoutNil.ensure(type.index(), [&] {
        return makeUniqueWithoutFastMallocCheck<Type>(m_program.at(type).withoutPrimitives(Primitive::UndefinedOrNull));
    }).iterator->value;
}

CellPointer Lowering::nonNil(ValueID receiver, StringID name)
{
    Operand operand = get(receiver);
    if (operand.rep == Rep::Cell || !typeOf(receiver).mayBeUndefinedOrNull())
        return nonNil(operand, typeOf(receiver), name);
    bool isUnconditional = m_out.m_block == m_unconditionalBlock;
    CellPointer cell = nonNil(operand, typeOf(receiver), name);
    bool isWatched = m_currentBlock->terminator.kind == TerminatorKind::Invoke && m_currentIndexInBlock + 1 == m_currentBlock->values.size();
    if (isUnconditional) {
        m_unconditionalBlock = m_out.m_block;
        if (!isWatched) {
            ValueID tested = receiver;
            while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
                tested = m_function.at(tested).child(0);
            m_passedNilTestIn.add(tested.index(), Vector<BlockID, 1> { }).iterator->value.append(m_currentBlockID);
        }
    }
    return cell;
}

// For the map of the image. A number in the place of a pointer, which nothing follows: the kind of operation, from 1, and whether it came along with a copied body.
static constexpr unsigned firstCastForMap = 256;
static constexpr unsigned firstTerminatorForMap = 288;
static constexpr unsigned firstBuiltinForMap = 320;

static DFG::Node* originForMap(unsigned kind, bool isCopied)
{
    return std::bit_cast<DFG::Node*>(static_cast<uintptr_t>((kind + 1) << 1 | isCopied) << 8);
}

static DFG::Node* originForMap(const Function& function, const Value& value)
{
    unsigned kind = static_cast<unsigned>(value.opcode);
    if (value.opcode == Opcode::Cast)
        kind = firstCastForMap + static_cast<unsigned>(value.castReason());
    else if (value.opcode == Opcode::CallBuiltin)
        kind = firstBuiltinForMap + value.builtin().index();
    uint32_t outermost = value.inlineFrame;
    while (outermost && function.inlineFrames[outermost].parent)
        outermost = function.inlineFrames[outermost].parent;
    uintptr_t licences = value.inlineFrame ? function.inlineFrames[outermost].licence | function.inlineFrames[value.inlineFrame].licence << 4 : 0;
    return std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(kind, !!value.inlineFrame)) | licences << 24);
}

static String bytesByOperation(const B3::PCToOriginMap& map, LinkBuffer& linkBuffer, size_t codeSize)
{
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bytes;
    auto* start = static_cast<const uint8_t*>(linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr());
    auto& ranges = map.ranges();
    auto offsetOf = [&](size_t i) {
        return i < ranges.size() ? std::min<size_t>(static_cast<const uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(ranges[i].label).untaggedPtr()) - start, codeSize) : codeSize;
    };
    std::array<uint32_t, 256> bytesByLicences { };
    bytes.add(0, 0).iterator->value += offsetOf(0);
    bytesByLicences[0] += offsetOf(0);
    for (size_t i = 0; i < ranges.size(); ++i) {
        uintptr_t origin = std::bit_cast<uintptr_t>(ranges[i].origin.dfgOrigin());
        bytes.add(static_cast<uint32_t>(origin >> 8 & 0xffff), 0).iterator->value += offsetOf(i + 1) - offsetOf(i);
        bytesByLicences[origin >> 24 & 0xff] += offsetOf(i + 1) - offsetOf(i);
    }
    Vector<std::pair<uint32_t, uint32_t>> sorted;
    for (auto& entry : bytes)
        sorted.append({ entry.key, entry.value });
    std::ranges::sort(sorted);
    StringPrintStream out;
    for (auto& [origin, size] : sorted) {
        if (!size)
            continue;
        out.print("\t");
        unsigned kind = (origin >> 1) - 1;
        if (!origin)
            out.print("-");
        else if (kind >= firstBuiltinForMap) {
            auto& builtin = Builtins::singleton().at(BuiltinID(kind - firstBuiltinForMap));
            out.print("CallBuiltin.", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name);
        } else if (kind >= firstTerminatorForMap)
            out.print("->", kind - firstTerminatorForMap);
        else if (kind >= firstCastForMap)
            out.print("Cast.", kind - firstCastForMap);
        else
            out.print(nameOf(static_cast<Opcode>(kind)));
        out.print(origin & 1 ? "+" : "", "\t", size);
    }
    out.print("\nL");
    for (unsigned licences = 0; licences < bytesByLicences.size(); ++licences) {
        if (bytesByLicences[licences])
            out.print("\t", licencesForMap[licences & 15], licencesForMap[licences >> 4], "\t", bytesByLicences[licences]);
    }
    return out.toString();
}

void Lowering::lowerBlock(BlockID id)
{
    m_out.appendTo(m_blocks[id.index()]);
    m_currentBlockID = id;
    m_isInLoop = m_loopBlocks.get(id.index());
    bool didEmitCalls = std::exchange(m_context.emitsCalls, false);
    auto& block = m_function.at(id);
    if (!m_catchSpills.isEmpty()) {
        for (ValueID parameter : block.parameters)
            define(parameter, m_values[parameter.index()]);
    }
    if (!id.index() && m_argumentCountVariable) {
        m_out.store32(m_argumentCount.value(), catchSpillSlot(m_firstArgumentWord));
        m_out.store64(m_argumentBuffer.value(), catchSpillSlot(m_firstArgumentWord + 1));
        m_out.m_block->appendNew<VariableValue>(m_proc, Set, m_out.origin(), m_argumentCountVariable, m_argumentCount.value());
        m_out.m_block->appendNew<VariableValue>(m_proc, Set, m_out.origin(), m_argumentBufferVariable, m_argumentBuffer.value());
    }
    m_currentBlock = &block;
    for (unsigned i = 0; i < block.values.size(); ++i) {
        ValueID valueID = block.values[i];
        auto& value = m_function.at(valueID);
        m_currentIndexInBlock = i;
        if (block.terminator.kind == TerminatorKind::Invoke && i + 1 == block.values.size())
            spillForCatch(block.terminator.targets[1]);
        m_unconditionalBlock = m_out.m_block;
        if (Options::aotMapFilePath()) [[unlikely]]
            m_out.setOrigin(originForMap(m_function, value));
        double frequency = m_out.m_block->frequency();
        Operand result = lower(valueID, value);
        m_currentCatchEntrypoint = std::nullopt;
        if (!m_whyFailed.isNull())
            return;
        // A join that was made in a cold arm would be born cold, and all that follows with it: see AOT::Output::newBlock().
        RELEASE_ASSERT_WITH_MESSAGE(m_out.m_block->frequency() >= frequency, "AOT: the lowering of an operation ends off the usual path");
        // Then the type of what an operation makes is anything, like every type the front end gives: see Reader::typeReliedOn(). What was
        // made is a cell all the same.
        if (result.rep == Rep::Cell && value.rep == Rep::JSValue && Options::auditAOTTypedAssertions()) [[unlikely]]
            result = box(result.as<CellPointer>());
        if (result.rep != value.rep) [[unlikely]] {
            dataLogLn("AOT: f", m_id.index(), ": v", valueID.index(), " = ", nameOf(value.opcode), " is kept as ", nameOf(value.rep), " and was lowered as ", nameOf(result.rep));
            CRASH();
        }
        define(valueID, result);
        RELEASE_ASSERT_WITH_MESSAGE(!m_environmentAwaitingBarrier || i + 1 < block.values.size(), "AOT: a block ends between a store to an environment and its barrier");
        if (isFetchedWhereNeeded(valueID))
            continue;
        if (result.rep == Rep::Double)
            didDefineNumber(valueID, value, result.as<Number>());
        if (value.assertedType && value.opcode != Opcode::Cast) [[unlikely]]
            auditResult(result, value);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateFact(result, valueID, typeOf(valueID));
        // What a test has shown to be a cell, an operand a cast hands on, and what a variable is read back as were defined before that: a load from it may trap there.
        bool isDefinedHere = result.rep == Rep::Cell && result.value->opcode() != B3::Get && !m_takenForCells.contains(result.value);
        value.forEachChild([&](ValueID child) {
            isDefinedHere &= m_values[child.index()].value != result.value;
        });
        if (isDefinedHere)
            m_typeOfCell.add(result.value, value.type);
    }
    if (Options::aotMapFilePath()) [[unlikely]]
        m_out.setOrigin(originForMap(firstTerminatorForMap + static_cast<unsigned>(block.terminator.kind), false));
    lowerTerminator(block.terminator);
    if (m_context.emitsCalls)
        m_context.blocksThatEmitCalls.set(id.index());
    m_context.emitsCalls |= didEmitCalls;
}

bool Lowering::run()
{
    if (m_function.blocks.isEmpty()) {
        fail("a function without a body"_s);
        return false;
    }
    static_assert(maxInlineFrames == PackedSite::maxInlineFrames);
    for (auto& frame : m_function.inlineFrames) {
        bool isFunctionItself = m_context.inlineCallSites.isEmpty();
        m_context.inlineCallSites.append(isFunctionItself ? 0 : CallSiteIndex(BytecodeIndex(m_nextSite++)).bits());
        if (!isFunctionItself)
            m_context.sitePositions.append({ frame.callPosition, frame.parent });
    }
    m_context.numberOfSitesWithoutCode = m_nextSite - 1;
    unsigned numberOfBlocks = m_function.blocks.size();
    m_values.grow(m_function.values.size());
    m_blocks.fill(nullptr, numberOfBlocks);
    m_phis.grow(numberOfBlocks);
    m_out.initialize(m_heaps);
    m_out.setFrequency(1);
    LBasicBlock prologue = m_out.newBlock();
    Vector<BlockID> order = reversePostorder();
    computeLoopBlocks(order);
    computeDominators(order);
    computeLoops(order);
    computeCatchEntrypoints(order);
    m_context.numberOfCatchEntrypoints = m_catchEntrypoints.size();
    m_proc.setNumEntrypoints(1 + m_catchEntrypoints.size());
    for (BlockID block : order) {
        m_blocks[block.index()] = m_out.newBlock();
        if (m_function.at(block).terminator.kind == TerminatorKind::Unreachable || m_function.at(block).terminator.kind == TerminatorKind::Throw)
            m_blocks[block.index()]->setFrequency(0);
    }
    // A module is evaluated once in the life of the program. What only leads there, or to a throw, is run no more often.
    for (unsigned i = order.size(); m_function.kind != FunctionKind::Module && i--;) {
        auto& block = m_function.at(order[i]);
        bool evaluatesModule = block.values.containsIf([&](ValueID id) {
            return m_function.at(id).opcode == Opcode::CallDirect && m_program.at(m_function.at(id).function()).kind == FunctionKind::Module;
        });
        if (!evaluatesModule && (block.terminator.kind == TerminatorKind::Jump || block.terminator.kind == TerminatorKind::Branch)) {
            bool onlyLeadsToColdBlocks = true;
            for (auto& target : block.terminator.targets) {
                evaluatesModule |= m_blocksThatEvaluateModules.get(target.block.index());
                onlyLeadsToColdBlocks &= m_blocks[target.block.index()] && !m_blocks[target.block.index()]->frequency();
            }
            evaluatesModule &= onlyLeadsToColdBlocks;
        }
        if (evaluatesModule) {
            m_blocks[order[i].index()]->setFrequency(0);
            m_blocksThatEvaluateModules.set(order[i].index());
        }
    }
    for (auto& handler : m_catchEntrypoints)
        handler.entry = newColdBlock();
    m_out.appendTo(prologue);
    m_out.initializeConstants(m_proc, prologue);
    findPinnedRegisters();
    if (m_catchSpillSlotCount)
        m_catchSpillArea = m_out.lockedStackSlot(m_catchSpillSlotCount * sizeof(Register));
    if (!lowerEntryParameters())
        return false;
    if (!m_function.isTyped() && m_catchEntrypoints.isEmpty()) {
        m_icDataAsOfLastHint = m_proc.addVariable(Int64);
        findICData();
    }
    findTruthinessPhis();
    findClosureTestsCarriedOutByBranches();
    findLeavesFetchedLate();
    findConcatenationsWithNumbers();
    findStringsReadInLoops();
    findIterationsOfStrings();
    findEntriesOfArrays();
    findParametersJoinedInParts();
    findNumbersJoinedWithIntegers(order);
    findNumbersJoinedWithInt32(order);
    for (BlockID block : order) {
        if (!block.index())
            continue;
        m_out.appendTo(m_blocks[block.index()]);
        for (ValueID id : m_function.at(block).parameters) {
            Rep rep = m_function.at(id).rep;
            LValue phi = m_out.phi(b3TypeOf(rep));
            m_phis[block.index()].append(phi);
            m_values[id.index()] = Operand(rep, phi);
            if (m_isJoinedInParts.get(id.index())) {
                LValue cell = m_out.phi(Int64);
                m_cellPhis.add(id.index(), cell);
                // An edge may pass the word of a slot that reads `undefined` where it holds nothing.
                m_values[id.index()] = fromParts(phi, cell, true);
            }
            if (m_hasTruthinessPhi.get(id.index()))
                m_truthinessPhis.add(id.index(), m_out.phi(Int32));
            if (auto least = m_leastOfNumbersJoinedWithIntegers.find(id.index()); least != m_leastOfNumbersJoinedWithIntegers.end()) {
                LValue integerPhi = m_out.phi(Int64);
                m_integerPhis.add(id.index(), integerPhi);
                m_integersOfNumbers.add(id.index(), IntegerOfNumber { phi, integerPhi, least->value });
            }
            if (m_hasInt32Phi.get(id.index()))
                m_int32Phis.add(id.index(), m_out.phi(Int32));
            if (rep == Rep::Cell)
                m_typeOfCell.add(phi, m_function.at(id).type);
        }
    }
    // What comes before the switch is run on every entry, also on that of a handler.
    LBasicBlock start = m_blocks[0];
    m_out.appendTo(prologue);
    if (m_catchEntrypoints.isEmpty())
        m_out.jump(start);
    else {
        Vector<LBasicBlock> entries { start };
        for (auto& handler : m_catchEntrypoints)
            entries.append(handler.entry);
        m_out.entrySwitch(entries);
    }
    for (BlockID block : order) {
        // Its comparison was carried out with those before it: see lowerBranchOnText().
        if (m_blocksTakenIntoBranchOnText.get(block.index())) {
            m_out.appendTo(m_blocks[block.index()]);
            m_out.unreachable();
            continue;
        }
        lowerBlock(block);
        if (!m_whyFailed.isNull())
            return false;
    }
    for (auto& handler : m_catchEntrypoints)
        lowerCatchEntrypoint(handler);
    m_heaps.computeRangesAndDecorateInstructions();
    m_proc.deleteOrphans();
    m_out.applyBlockOrder();
    return true;
}

// Whoever calls by name passes the environment, `this` and the arguments as the function declares them. The body may take them for more.
bool Lowering::lowerEntryParameters()
{
    auto& parameters = m_function.blocks[0].parameters;
    unsigned nextGPR = 0;
    if (m_function.has(FunctionUsesArgumentBuffer)) {
        m_argumentCount = Word32(m_out.castToInt32(registerOnEntry(argumentGPR(0))));
        m_argumentBuffer = Pointer(registerOnEntry(argumentGPR(1)));
    }
    if (m_function.isGenericEntrypoint()) {
        // The function value, `this` and every argument, each in its own register. All but the first may be anything.
        for (unsigned i = 0; i < parameters.size(); ++i) {
            Rep rep = m_function.at(parameters[i]).rep;
            RELEASE_ASSERT(i ? rep == Rep::JSValue : holdsCells(rep));
            GPRReg reg = i ? thisGPR : calleeGPR;
            if (i >= 2) {
                ++m_genericEntrypointParameterCount;
                if (nextGPR >= numberOfArgumentGPRs) {
                    fail("more parameters than there are registers"_s);
                    return false;
                }
                reg = argumentGPR(nextGPR++);
            }
            LValue value = registerOnEntry(reg);
            m_values[parameters[i].index()] = Operand(rep, value);
            if (rep == Rep::Cell)
                m_typeOfCell.add(value, m_function.at(parameters[i]).type);
        }
        return true;
    }

    unsigned nextFPR = 0;
    unsigned onStack = 0;
    for (unsigned i = 0; i < parameters.size(); ++i) {
        auto& parameter = m_function.at(parameters[i]);
        auto& type = typePassedAs(m_function, i);
        Rep rep = repOf(type);
        bool isDouble = b3TypeOf(rep) == Double;
        LValue value;
        if (isDouble && nextFPR < FPRInfo::numberOfArgumentRegisters)
            value = registerOnEntry(FPRInfo::toArgumentRegister(nextFPR++));
        else if (!isDouble && nextGPR < numberOfArgumentGPRs) {
            value = registerOnEntry(argumentGPR(nextGPR++));
            if (rep == Rep::Int32)
                value = m_out.castToInt32(value);
        } else {
            m_context.readsCallerFrame = true;
            TypedPointer pointer = m_out.address(m_heaps.root, m_out.framePointer(), sizeof(CallerFrameAndPC) + onStack++ * sizeof(Register));
            value = isDouble ? m_out.loadDouble(pointer) : rep == Rep::Int32 ? m_out.load32(pointer) : m_out.load64(pointer);
        }
        m_values[parameters[i].index()] = convert(Operand(rep, value), type, parameter.rep);
        if (rep == Rep::Cell && parameter.rep == Rep::Cell)
            m_typeOfCell.add(value, parameter.type);
    }
    return true;
}

TypedPointer Lowering::addressOf(VariableID id)
{
    return m_out.address(m_heaps.AOTTyped_variables[id.index()], fixedPointer(Instance::offsetOfTypedStatics()), m_program.variables[id.index()].offset);
}

// What was read from a variable that may hold nothing yet.
void Lowering::checkInitialized(Operand raw, StringID name)
{
    require(logicalNot(isAbsent(raw)), [&] {
        throwAt(ThrowKind::TDZ, std::nullopt, name.index());
    });
}

// The module variables and the variables in environments that this function stores to. One that is stored to once, and is never seen
// uninitialized, never changes while any other function runs: none that reads it is entered before the store.
void Lowering::findStoresToVariables()
{
    if (std::exchange(m_didFindStoresToVariables, true))
        return;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode == Opcode::PutModuleVariable)
                m_variablesStoredTo.set(value.variable().index());
            else if (value.opcode == Opcode::PutField)
                m_fieldsStoredTo.add(static_cast<uint64_t>(value.layout().index()) << 32 | value.field());
        }
    }
}

Operand Lowering::lowerGetModuleVariable(ValueID result, const Value& value)
{
    auto& variable = m_program.variables[value.variable().index()];
    unsigned flags = IsSafe;
    if (variable.isStoredOnce) {
        findStoresToVariables();
        if (!m_variablesStoredTo.get(value.variable().index()))
            flags |= IsFixed;
    }
    Operand raw = load(variable.store, addressOf(value.variable()), flags);
    if (variable.mayBeSeenUninitialized())
        checkInitialized(raw, variable.name);
    return ensure(raw, m_program.at(variable.type ? variable.type : m_program.types.anything), typeOf(result));
}

LValue Lowering::wordOfGlobalPropertyCache(uint32_t cacheNumber, size_t offset)
{
    return marked(m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, fixedPointer(Instance::offsetOfTypedStatics()), m_program.globalPropertyCachesOffset + (cacheNumber - 1) * sizeof(GlobalPropertyCache) + offset)), IsSafe);
}

Boxed Lowering::valueOfGlobalPropertyOrElse(uint32_t cacheNumber, LBasicBlock miss)
{
    LBasicBlock mayChange = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue value = wordOfGlobalPropertyCache(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, value));
    ValueFromBlock forGood = m_out.anchor(value);
    m_out.branch(m_out.notZero64(value), unsure(continuation), unsure(mayChange));
    m_out.appendTo(mayChange);
    ValueFromBlock current = m_out.anchor(m_out.load64(globalPropertyOrElse(cacheNumber, miss)));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, forGood, current));
}

TypedPointer Lowering::globalPropertyOrElse(uint32_t cacheNumber, LBasicBlock miss)
{
    auto inCache = [&](size_t offset) {
        return wordOfGlobalPropertyCache(cacheNumber, offset);
    };
    static_assert(!OBJECT_OFFSETOF(GlobalPropertyCache, structureID) && OBJECT_OFFSETOF(GlobalPropertyCache, globalLexicalBindingEpoch) == sizeof(uint32_t));
    CellPointer globalObject(fixedPointer(Instance::offsetOfGlobalObject()));
    Integer structureID = zeroExtended(loadField<Word32>(globalObject, m_heaps.JSCell_structureID, IsSafe));
    Integer epoch = zeroExtended(loadField<Word32>(globalObject, m_heaps.JSGlobalObject_globalLexicalBindingEpoch, IsSafe));
    orElse(m_out.equal(m_out.bitOr(structureID.value(), m_out.shl(epoch.value(), m_out.constInt32(32))), inCache(0)), miss);
    LValue location = inCache(OBJECT_OFFSETOF(GlobalPropertyCache, location));
    LValue butterflyIfNegative = m_out.bitAnd(loadField<Pointer>(globalObject, m_heaps.JSObject_butterfly, IsSafe).value(), m_out.aShr(location, m_out.constInt32(63)));
    return TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(butterflyIfNegative, location));
}

Boxed Lowering::lowerGetGlobal(const Value& value)
{
    Word32 name = word32(value.string().index());
    uint32_t cacheNumber = m_program.globalPropertyCacheNumber(value.string(), false);
    auto generally = [&] {
        if (value.opcode == Opcode::GetGlobal)
            return AOT_TYPED_OPERATION(GetFree, name, word32(cacheNumber));
        return AOT_TYPED_OPERATION(GetFreeForTypeOf, name, word32(cacheNumber));
    };
    if (!cacheNumber)
        return generally();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fast = m_out.anchor(valueOfGlobalPropertyOrElse(cacheNumber, miss).value());
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(miss, [&] {
        slow = m_out.anchor(generally().value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow));
}

std::optional<Boxed> Lowering::getPropertyOfGlobalThis(ValueID base, StringID name)
{
    uint32_t cacheNumber = m_program.propertyOfGlobalThisCacheNumber(name);
    if (auto& value = m_function.at(base); !cacheNumber || value.opcode != Opcode::GetGlobal || !m_program.isNameOfGlobalThis(value.string()))
        return std::nullopt;
    Boxed object = in<Boxed>(base);
    LBasicBlock isAnotherObject = newColdBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    orElse(m_out.equal(object.value(), marked(m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, fixedPointer(Instance::offsetOfGlobalObject()), JSGlobalObject::offsetOfGlobalThis())), IsSafe)), isAnotherObject);
    ValueFromBlock fast = m_out.anchor(valueOfGlobalPropertyOrElse(cacheNumber, miss).value());
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(miss, [&] {
        slow = m_out.anchor(AOT_TYPED_OPERATION(GetPropertyOfGlobalThis, word32(name.index()), word32(cacheNumber)).value());
        m_out.jump(continuation);
    });
    ValueFromBlock general;
    inColdBlock(isAnotherObject, [&] {
        general = m_out.anchor(emitGetById(object, name).value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow, general));
}

void Lowering::lowerPutGlobal(const Value& value)
{
    Word32 name = word32(value.string().index());
    uint32_t cacheNumber = m_program.globalPropertyCacheNumber(value.string(), true);
    Boxed stored = in<Boxed>(value.child(0));
    if (!cacheNumber) {
        AOT_TYPED_OPERATION(SetFree, name, word32(cacheNumber), stored);
        return;
    }
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    TypedPointer property = globalPropertyOrElse(cacheNumber, miss);
    didEmitWrite(Writes::Heap);
    m_out.store64(stored.value(), property);
    if (typeOf(value.child(0)).hasCells())
        barrier(CellPointer(fixedPointer(Instance::offsetOfGlobalObject())));
    m_out.jump(continuation);
    inColdBlock(miss, [&] {
        AOT_TYPED_OPERATION(SetFree, name, word32(cacheNumber), stored);
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

void Lowering::lowerPutModuleVariable(const Value& value)
{
    auto& variable = m_program.variables[value.variable().index()];
    didEmitWrite(Writes::Fields);
    store(variable.store, in(value.child(0), repOf(variable.store)), addressOf(value.variable()));
}

// Only compiled code gets to see an environment or the frame of a coroutine.
Operand Lowering::lowerGetEnvironmentField(ValueID result, const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    unsigned flags = safeIfReaches(cell, field.offset + sizeOf(field.store));
    if (field.flags & fieldIsImmutable)
        flags |= IsFixed;
    else if (field.flags & fieldIsStoredOnce && field.flags & fieldIsNeverSeenUninitialized) {
        findStoresToVariables();
        if (!m_fieldsStoredTo.contains(static_cast<uint64_t>(value.layout().index()) << 32 | value.field()))
            flags |= IsFixed;
    }
    Operand raw = load(field.store, fieldAt(cell, field), flags, cell.value());
    auto& stored = m_program.at(field.stored);
    if (stored.primitives & Primitive::Absent)
        checkInitialized(raw, field.name);
    return ensure(raw, stored.withoutPrimitives(Primitive::Absent), typeOf(result));
}

// Stores to one environment that stand side by side, as those to the frame of a coroutine in front of a suspension. Nothing between them allocates, so the barrier behind the last is that of all.
bool Lowering::isFollowedByStoreWithBarrier(ValueID environment) const
{
    for (size_t i = m_currentIndexInBlock + 1; i < m_currentBlock->values.size(); ++i) {
        auto& next = m_function.at(m_currentBlock->values[i]);
        if (next.opcode == Opcode::Integer)
            continue;
        if (next.opcode != Opcode::PutField || next.child(0) != environment || isObjectLayout(m_program.at(next.layout()).kind))
            return false;
        if (isLeftToItsFirstUse(next.child(1)) || isFetchedWhereNeeded(next.child(1)) || isInitializingStore(m_program, m_function, *m_currentBlock, i))
            return false;
        if (holdsCells(m_program.at(next.layout()).fields[next.field()].store) && typeOf(next.child(1)).hasCells())
            return true;
    }
    return false;
}

void Lowering::lowerPutEnvironmentField(const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    ValueID stored = value.child(1);
    didEmitWrite(isLocalAllocation(m_function, value.child(0)) ? Writes::RuntimeState : Writes::Fields);
    store(field.store, in(stored, repOf(field.store)), fieldAt(cell, field));
    if (isLoweringInitializingStore())
        didLowerInitializingStore();
    else if (holdsCells(field.store) && typeOf(stored).hasCells() && !m_program.at(value.layout()).isInFrame) {
        m_environmentAwaitingBarrier = isFollowedByStoreWithBarrier(value.child(0)) ? value.child(0) : ValueID();
        if (!m_environmentAwaitingBarrier)
            barrier(cell);
    }
}

CellPointer Lowering::lowerNewPromise()
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue promise = newPromise(slowPath);
    applyEffects(promise->as<PatchpointValue>(), { Writes::RuntimeState, false });
    ValueFromBlock fast = m_out.anchor(promise);
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewPromise).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

// The promise of an async function that returns before anyone has looked at the promise: it is pending, nobody waits for it, no resolving
// function was called, which is all bits of its word clear. What is no object has no `then`. Then to resolve it is to store the value.
void Lowering::lowerResolvePromise(const Value& value)
{
    CellPointer promise = in<CellPointer>(value.arguments[0]);
    Boxed resolution = in<Boxed>(value.arguments[1]);
    OperationEffects effects = effectsOfBuiltinCall(value);
    // Read out of the frame: the function has been suspended, its caller has the promise and waits for it as a rule.
    auto& source = m_function.at(value.arguments[0]);
    if (source.opcode == Opcode::GetField && m_program.at(source.layout()).isCoroutineFrame) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(ResolvePromise, effects, promise, resolution);
        return;
    }
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock inPlace = m_out.newBlock();
    LBasicBlock outOfLine = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isZero(loadField<Integer>(promise, m_heaps.JSPromise_packed)).value(), unsure(isCellCase), unsure(outOfLine));
    m_out.appendTo(isCellCase);
    LBasicBlock isCellOfSomeKind = m_out.newBlock();
    m_out.branch(isCell(resolution).value(), unsure(isCellOfSomeKind), unsure(inPlace));
    m_out.appendTo(isCellOfSomeKind);
    m_out.branch(below(typeOfCell(asCell(resolution)), word32(ObjectType)).value(), unsure(inPlace), unsure(outOfLine));
    m_out.appendTo(inPlace);
    didEmitWrite(effects.writes);
    static_assert(CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer == 48);
    storeField(promise, m_heaps.JSPromise_slot, resolution);
    storeField(promise, m_heaps.JSPromise_packed, integer(static_cast<int64_t>(static_cast<uint64_t>(JSPromise::Status::Fulfilled) | JSPromise::isFirstResolvingFunctionCalledFlag) << 48));
    barrier(promise);
    m_out.jump(continuation);
    m_out.appendTo(outOfLine);
    AOT_TYPED_OPERATION_WITH_EFFECTS(ResolvePromise, effects, promise, resolution);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// What the language does and no function of the engine: the runtime carries it out.
Operand Lowering::lowerLanguageOperation(ValueID id, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    RELEASE_ASSERT(builtin.implementation.kind == BuiltinImplementation::Operation);
    if (builtin.implementation.operation == LanguageOperation::ConstructWith)
        return lowerConstructWith(id, value);
    if (builtin.implementation.operation == LanguageOperation::Apply || builtin.implementation.operation == LanguageOperation::CallWithSpread)
        return lowerApply(value.arguments[0], value.arguments[1], value.arguments[2], typeOf(id), builtin.implementation.operation);
    auto& type = typeOf(id);
    switch (builtin.implementation.operation) {
    case LanguageOperation::DefineField:
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (m_currentIndexInBlock == literal->fields.last())
                lowerObjectLiteral(*literal);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        if (auto* run = propertyRunAt(m_currentIndexInBlock)) {
            if (m_currentIndexInBlock == run->stores.last().index)
                lowerPropertyRun(*run);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        if (auto& name = m_function.at(value.arguments[1]); name.opcode == Opcode::String && emitDefineByName(in<Boxed>(value.arguments[0]), name.string(), in<Boxed>(value.arguments[2])))
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        break;
    case LanguageOperation::DefineGetter:
    case LanguageOperation::DefineSetter:
        if (auto* group = accessorGroupAt(m_currentIndexInBlock)) {
            lowerMemberOfAccessorGroup(*group, value);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        break;
    case LanguageOperation::GetNewTarget:
        return ensure(lowerGetNewTarget(), type);
    case LanguageOperation::PrivateGet:
    case LanguageOperation::PrivateHas: {
        Stub stub = builtin.implementation.operation == LanguageOperation::PrivateGet ? Stub::PrivateGet : Stub::PrivateHas;
        return ensure(Boxed(callICStub(stub, Int64, { { in<Boxed>(value.arguments[0]).value(), firstStubOperandGPR }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR1 } }, std::nullopt, effectsOfBuiltinCall(value))), type);
    }
    case LanguageOperation::PrivateSet:
    case LanguageOperation::PrivateFieldAdd: {
        Stub stub = builtin.implementation.operation == LanguageOperation::PrivateSet ? Stub::PrivateSet : Stub::PrivateFieldAdd;
        callICStub(stub, Void, { { in<Boxed>(value.arguments[0]).value(), firstStubOperandGPR }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[2]).value(), GPRInfo::argumentGPR2 } }, std::nullopt, effectsOfBuiltinCall(value));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case LanguageOperation::CloneObject:
        if (auto group = findCloneGroup(m_currentIndexInBlock)) {
            m_cloneGroup = WTF::move(group);
            if (m_cloneGroup->isDecidedAtClone)
                decideCloneGroup(*m_cloneGroup);
            return placeholder(value.rep);
        }
        return ensure(countPlainAllocation(PlainAllocationKind::Clone, id, cloneObject(value.arguments[0], true)), m_program.at(m_program.types.object), type);
    case LanguageOperation::CopyDataProperties:
        if (isMadeHere(value.arguments[0])) {
            // In the place of a NewObject: what follows counts on an ordinary object.
            CellPointer object = countPlainAllocation(PlainAllocationKind::Clone, value.arguments[0], cloneObject(value.arguments[1], false));
            defineObjectMadeLater(value.arguments[0], object);
            return ensure(object, m_program.at(m_program.types.object), type);
        }
        break;
    case LanguageOperation::NewPromise:
        return ensure(lowerNewPromise(), Type::engineClass(m_builtins.promiseClass()), type);
    case LanguageOperation::ResolvePromise:
        lowerResolvePromise(value);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case LanguageOperation::Await: {
        // The closure is read from the frame, whose layout says what drives the coroutine: a promise, or the object of a generator.
        auto& closure = m_function.at(value.arguments[0]);
        bool isAsyncFunction = closure.opcode == Opcode::GetField && m_program.at(closure.layout()).isCoroutineFrame
            && m_program.at(m_program.at(closure.layout()).fields[CoroutineFrame::promiseField].type).onlyEngineClass() == &m_builtins.promiseClass();
        AOT_TYPED_OPERATION_WITH_EFFECTS(Await, effectsOfBuiltinCall(value), in<CellPointer>(value.arguments[0]), in<Boxed>(value.arguments[1]), boolean(isAsyncFunction));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    default:
        break;
    }
    auto outOfLine = [&] {
        auto [count, list] = argumentBuffer(value.arguments);
        Boxed result = AOT_TYPED_OPERATION_WITH_EFFECTS(LanguageOperation, effectsOfBuiltinCall(value), word32(static_cast<int32_t>(builtin.implementation.operation)), count, list);
        return vouchesForItsType(m_program, m_function, value) ? convert(result, type, value.rep) : ensure(result, type);
    };
    if (builtin.implementation.operation == LanguageOperation::CopyDataPropertiesExcluding) {
        auto expected = predictionOf(id);
        LayoutID view = expected ? expected->onlyView() : LayoutID();
        if (view && m_program.at(view).kind == LayoutKind::Record && m_program.at(view).shape && !Options::auditAOTTypedAssertions()) {
            auto [count, list] = argumentBuffer(value.arguments);
            CellPointer made = AOT_TYPED_OPERATION(TryCopyRecordAs, word32(m_program.at(view).shape), count, list);
            LBasicBlock isMade = m_out.newBlock();
            LBasicBlock isNotMade = newColdBlock();
            Join join(m_out, value.rep, b3TypeOf(value.rep));
            m_out.branch(m_out.notZero64(made.value()), usually(isMade), rarely(isNotMade));
            m_out.appendTo(isMade);
            join.add(ensure(made, m_program.at(m_program.types.object), type));
            m_out.appendTo(isNotMade);
            join.add(outOfLine());
            return join.finish();
        }
    }
    if (builtin.implementation.operation == LanguageOperation::AppendSpread) {
        if (LBasicBlock slowPath = appendSpread(value)) {
            Join join(m_out, value.rep, b3TypeOf(value.rep));
            join.add(ensure(get(value.arguments[0]), typeOf(value.arguments[0]), type));
            m_out.appendTo(slowPath);
            join.add(outOfLine());
            return join.finish();
        }
    }
    if (builtin.implementation.operation != LanguageOperation::GetFromArguments || !mayBeIndex(value.arguments[1]))
        return outOfLine();
    // The list is an array that compiled code has made. An element that is there is loaded from it.
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, value.rep, b3TypeOf(value.rep));
    join.add(getArrayElement(asCell(in<Boxed>(value.arguments[0])), value.arguments[1], type, slowPath));
    m_out.appendTo(slowPath);
    join.add(outOfLine());
    return join.finish();
}

Operand Lowering::lower(ValueID id, const Value& value)
{
    auto& type = typeOf(id);
    if (auto character = castOfIteratedCharacter(value))
        return *character;
    if (auto part = partOfEntriesOfArray(id, value))
        return *part;
    if (auto deferred = deferToBranchOnText(id, value))
        return *deferred;
    if (auto concatenated = lowerAsPartOfConcatenation(id, value))
        return *concatenated;
    switch (value.opcode) {
    case Opcode::Parameter:
        RELEASE_ASSERT_NOT_REACHED();
    // A pass that puts a constant in the place of a value leaves it the type of that value.
    case Opcode::Undefined:
        return convert(undefined(), type, value.rep);
    case Opcode::Null:
        return convert(boxedConstant(jsNull()), type, value.rep);
    case Opcode::Boolean:
        return convert(boolean(value.boolean()), type, value.rep);
    case Opcode::Integer:
        return convert(integer(value.integer()), type, value.rep);
    case Opcode::Double:
        return convert(number(std::bit_cast<double>(value.bits())), type, value.rep);
    case Opcode::String:
        if (isFetchedWhereNeeded(id))
            return Operand(value.rep, nullptr);
        return convert(stringConstant(value.string()), type, value.rep);
    case Opcode::BigInt:
        return convert(AOT_TYPED_OPERATION(BigIntConstant, word32(value.string().index())), type, value.rep);
    case Opcode::FunctionReference:
        return convert(closureConstant(value.function()), type, value.rep);
    case Opcode::NewClosure:
        return convert(lowerNewClosure(value), type, value.rep);
    case Opcode::NewClosureOnce:
        return convert(lowerNewClosureOnce(value), type, value.rep);
    case Opcode::GetArgument: {
        uint32_t index = value.immediates[0];
        return ensure(either<Boxed>(below(word32(index), argumentCount()), [&] {
            return Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, argumentBuffer().value(), index * sizeof(EncodedJSValue))));
        }, [&] {
            return undefined();
        }), type);
    }
    case Opcode::CreateRest:
        return ensure(createRest(value.numberOfArgumentsToSkip()), Type::engineClass(m_builtins.arrayClass()), type);
    case Opcode::GetRestLength:
        return convert(restLength(value.numberOfArgumentsToSkip()), type, value.rep);
    case Opcode::GetMyArgumentByVal:
        return ensure(lowerGetMyArgumentByVal(value), type);
    case Opcode::CallForwardVarargs:
        return ensure(lowerCallForwardVarargs(value), type);
    case Opcode::CreateThis:
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (!literal->fields.isEmpty())
                return placeholder(value.rep);
            lowerObjectLiteral(*literal);
            return get(id);
        }
        return convert(countPlainAllocation(PlainAllocationKind::This, id, lowerCreateThis(value)), type, value.rep);
    case Opcode::ConstructSuper:
        return lowerConstructSuper(id, value);
    case Opcode::GetCapture: {
        // It was found to be a closure of the function, whatever its type says it may be.
        Operand closure = get(value.child(0));
        return captureOfClosure(closure.rep == Rep::JSValue ? asCell(closure.as<Boxed>()) : closure.as<CellPointer>(), m_program.at(value.function()), value.captureIndex());
    }
    case Opcode::GetModuleVariable:
        return lowerGetModuleVariable(id, value);
    case Opcode::PutModuleVariable:
        lowerPutModuleVariable(value);
        return undefined();
    case Opcode::GetGlobal:
    case Opcode::GetGlobalForTypeOf:
        if (BuiltinName name = m_program.builtinName(value.string()); m_builtins.isGlobal(name) && !m_program.isExtended(name))
            return convert(builtinCell(m_builtins.cellOf(name)), type, value.rep);
        return ensure(lowerGetGlobal(value), type);
    case Opcode::PutGlobal:
        lowerPutGlobal(value);
        return undefined();
    case Opcode::NewObjectWithShape:
        return lowerNewObject(id, value);
    case Opcode::GetField:
        if (!isObjectLayout(m_program.at(value.layout()).kind))
            return lowerGetEnvironmentField(id, value);
        return lowerGetField(id, value);
    case Opcode::PutField:
        if (!isObjectLayout(m_program.at(value.layout()).kind))
            lowerPutEnvironmentField(value);
        else
            lowerPutField(id, value);
        return undefined();
    case Opcode::GetById:
        if (value.readsMethodFor())
            return lowerMethodRead(value);
        return lowerGetById(id, value);
    case Opcode::PutById:
        lowerPutById(id, value);
        return undefined();
    case Opcode::InById:
        return lowerInById(value);
    case Opcode::DeleteById:
        if (!fieldFor(typeOf(value.child(0)), value.string()) && emitDeleteByVal(in<Boxed>(value.child(0)), box(stringConstant(value.string()))))
            return boolean(true);
        return AOT_TYPED_OPERATION(DeleteByName, in<Boxed>(value.child(0)), word32(value.string().index()));
    case Opcode::InByVal:
        if (!expectedTypeOf(value.child(1)).isNumber())
            return emitInByVal(in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
        return AOT_TYPED_OPERATION(HasKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::DeleteByVal:
        if (!expectedTypeOf(value.child(1)).isNumber() && emitDeleteByVal(in<Boxed>(value.child(0)), in<Boxed>(value.child(1))))
            return boolean(true);
        return AOT_TYPED_OPERATION(DeleteByKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::GetByVal:
        return lowerGetByVal(id, value);
    case Opcode::PutByVal:
        lowerPutByVal(value);
        return undefined();
    case Opcode::NewArray:
        if (isPassedInMemory(id))
            return placeholder(value.rep);
        return ensure(countPlainAllocation(PlainAllocationKind::Array, id, newArrayLiteral(id)), Type::engineClass(m_builtins.arrayClass()), type);
    case Opcode::NewObject:
        if (isLeftToItsFirstUse(id))
            return placeholder(value.rep);
        // It is made with its properties.
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (!literal->fields.isEmpty())
                return placeholder(value.rep);
            lowerObjectLiteral(*literal);
            return get(id);
        }
        return ensure(countPlainAllocation(PlainAllocationKind::Object, id, AOT_TYPED_OPERATION(NewObject)), m_program.at(m_program.types.object), type);
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
        if (auto fused = lowerFusedUse(m_currentIndexInBlock))
            return *fused;
        return lowerBinary(id, value);
    case Opcode::Negate:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::BitNot:
        return lowerUnary(id, value);
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        if (auto fused = lowerFusedUse(m_currentIndexInBlock))
            return *fused;
        return lowerRelational(value);
    case Opcode::StrictEq:
        return lowerStrictEq(value.child(0), value.child(1));
    case Opcode::Eq:
        return lowerLooseEq(value.child(0), value.child(1));
    case Opcode::LogicalNot:
        return logicalNot(get(value.child(0)).as<Word32>());
    case Opcode::ToBoolean: {
        SetForScope answerIsValue(m_answerIsValue, !isOnlyBranchedOn(id));
        return lowerToBoolean(value.child(0));
    }
    case Opcode::TypeOf:
        return convert(AOT_TYPED_OPERATION(TypeOf, in<Boxed>(value.child(0))), type, value.rep);
    case Opcode::InstanceOf:
        if (m_context.sites.size() < std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
            m_context.sites.append(Site { });
            Pointer slot = slotAddress(m_context.sites.size() - 1);
            return Word32(callICStub(Stub::InstanceOfCached, Int32, { { in<Boxed>(value.child(0)).value(), firstStubOperandGPR }, { in<Boxed>(value.child(1)).value(), GPRInfo::argumentGPR1 }, { slot.value(), GPRInfo::argumentGPR2 } }));
        }
        return AOT_TYPED_OPERATION(InstanceOf, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::ToString:
        return convert(toStringCell(value.child(0)), type, value.rep);
    case Opcode::ToPropertyKey:
        return ensure(AOT_TYPED_OPERATION(ToKey, in<Boxed>(value.child(0))), type);
    case Opcode::IsUndefinedOrNull: {
        SetForScope answerIsValue(m_answerIsValue, !isOnlyBranchedOn(id));
        return lowerIsUndefinedOrNull(value.child(0));
    }
    case Opcode::IsInstanceOfClass:
        return lowerIsInstanceOfClass(value);
    case Opcode::HasShapeOf:
        return lowerHasShapeOf(value);
    case Opcode::IsClassObjectAsDeclared:
        return lowerIsClassObjectAsDeclared(value);
    case Opcode::IsClosureOf:
        if (m_isCarriedOutByBranch.get(id.index()))
            return boolean(false);
        return lowerIsClosureOf(value);
    case Opcode::IsBuiltinFunction:
        return lowerIsBuiltinFunction(value);
    case Opcode::BuiltinFunction:
        if (isFetchedWhereNeeded(id))
            return Operand(value.rep, nullptr);
        return convert(builtinCell(m_builtins.cellOf(value.builtin())), type, value.rep);
    case Opcode::TypeOfIs:
        return lowerTypeOfIs(value);
    case Opcode::Results:
    case Opcode::Unspecified:
        return placeholder(value.rep);
    case Opcode::ResultAt:
        return lowerResultAt(value);
    case Opcode::Strcat:
        return convert(lowerStrcat(value), type, value.rep);
    case Opcode::IteratorOpen:
        return lowerIteratorOpen(id, value);
    case Opcode::IteratorHasNext:
        return lowerIteratorHasNext(value);
    case Opcode::IteratorValue:
        return lowerIteratorValue(id, value);
    case Opcode::IteratorClose:
        lowerIteratorClose(value);
        return undefined();
    case Opcode::Call:
        return lowerCall(id, value);
    case Opcode::Construct:
        return lowerConstruct(id, value);
    case Opcode::CallMethod:
        return lowerCallMethod(id, value);
    case Opcode::CallDirect:
        return lowerCallDirect(id, value);
    case Opcode::CallBuiltin:
        if (m_builtins.at(value.builtin()).implementation.kind == BuiltinImplementation::Operation)
            return lowerLanguageOperation(id, value);
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (!literal->fields.isEmpty())
                return placeholder(value.rep);
            lowerObjectLiteral(*literal);
            return get(id);
        }
        return lowerCallBuiltin(id, value);
    case Opcode::GetExternalModule:
        return ensure(AOT_TYPED_OPERATION(GetExternalModule, word32(value.string().index())), type);
    // The passes have turned these into something else.
    case Opcode::Operation:
    case Opcode::Await:
    case Opcode::Yield:
    case Opcode::GetResumeMode:
    case Opcode::AssertNotAllocated:
        RELEASE_ASSERT_NOT_REACHED();
    case Opcode::Cast:
        return lowerCast(id, value);
    case Opcode::CatchValue:
        return ensure(AOT_TYPED_OPERATION(Catch), type);
    case Opcode::LoopHint:
        poll();
        findICData();
        return undefined();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ImageKey keyOf(FunctionID function)
{
    ImageKey key { };
    key.module = 0;
    key.start = function.index();
    key.kind = ImageKey::typedFunction;
    return key;
}

ImageKey keyOfContinuation(FunctionID function, unsigned index)
{
    ImageKey key = keyOf(function);
    RELEASE_ASSERT(index + 1 < ImageKey::typedBody);
    key.kind |= index + 1;
    return key;
}

// Blocks of machine instructions that are the same to the letter and go on to the same place: one stays. No path runs an instruction more than before.
static void mergeIdenticalBlocks(Air::Code& code)
{
    auto isSame = [](Air::BasicBlock* a, Air::BasicBlock* b) {
        if (a->size() != b->size() || a->numSuccessors() != b->numSuccessors() || (a->numSuccessors() && a->successorBlock(0) != b->successorBlock(0)))
            return false;
        for (unsigned i = 0; i < a->size(); ++i) {
            if (a->at(i).kind != b->at(i).kind || !std::ranges::equal(a->at(i).args(), b->at(i).args()))
                return false;
        }
        return true;
    };
    // As Air::optimizeBlockOrder() has it.
    auto canBeInverted = [](Air::Inst& branch) {
        switch (branch.kind.opcode) {
        case Air::BranchOnFlags:
        case Air::Branch8:
        case Air::Branch32:
        case Air::Branch64:
        case Air::BranchTest8:
        case Air::BranchTest32:
        case Air::BranchTest64:
        case Air::BranchFloat:
        case Air::BranchDouble:
            return branch.args()[0].isInvertible();
        default:
            return false;
        }
    };
    // First what nothing falls into, until there is no more of it. Then what is fallen into.
    for (bool takesWhatIsFallenInto = false;;) {
        UncheckedKeyHashMap<unsigned, Vector<Air::BasicBlock*, 2>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> candidates;
        UncheckedKeyHashMap<Air::BasicBlock*, Air::BasicBlock*> fallsInto;
        Air::BasicBlock* previous = nullptr;
        for (Air::BasicBlock* block : code) {
            if (previous) {
                for (Air::BasicBlock* successor : previous->successorBlocks()) {
                    if (successor == block)
                        fallsInto.set(block, previous);
                }
            }
            previous = block;
            if (code.isEntrypoint(block) || block->numSuccessors() > 1 || (block->numSuccessors() && block->last().kind.opcode != Air::Jump))
                continue;
            unsigned hash = block->size() + (block->numSuccessors() ? block->successorBlock(0)->index() * 131 : 0);
            bool hasPatch = false;
            for (Air::Inst& inst : *block) {
                hasPatch |= inst.kind.opcode == Air::Patch;
                hash = hash * 33 + inst.kind.opcode;
                for (Air::Arg& arg : inst.args())
                    hash = hash * 33 + arg.hash();
            }
            if (!hasPatch)
                candidates.add(hash, Vector<Air::BasicBlock*, 2>()).iterator->value.append(block);
        }
        UncheckedKeyHashMap<Air::BasicBlock*, Air::BasicBlock*> replacements;
        Vector<Air::BasicBlock*, 4> inverted;
        Vector<Air::BasicBlock*, 4> postponed;
        for (auto& entry : candidates) {
            auto& blocks = entry.value;
            for (unsigned i = 0; i < blocks.size(); ++i) {
                if (!blocks[i])
                    continue;
                Vector<Air::BasicBlock*, 4> same { blocks[i] };
                for (unsigned k = i + 1; k < blocks.size(); ++k) {
                    if (blocks[k] && isSame(blocks[i], blocks[k])) {
                        same.append(blocks[k]);
                        blocks[k] = nullptr;
                    }
                }
                Air::BasicBlock* kept = same[0];
                for (Air::BasicBlock* block : same) {
                    if (block->numSuccessors() && block->successorBlock(0) == code.findNextBlock(block))
                        kept = block;
                }
                for (Air::BasicBlock* block : same) {
                    if (block == kept)
                        continue;
                    auto from = fallsInto.find(block);
                    if (from == fallsInto.end()) {
                        if (!takesWhatIsFallenInto)
                            replacements.add(block, kept);
                        continue;
                    }
                    Air::BasicBlock* source = from->value;
                    if (!takesWhatIsFallenInto || source->numSuccessors() != 2 || source->successorBlock(1) != block || source->successorBlock(0) == block)
                        continue;
                    if (source->successorBlock(0) != code.findNextBlock(block) || !canBeInverted(source->last()))
                        continue;
                    replacements.add(block, kept);
                    inverted.append(source);
                }
            }
        }
        // What lies behind a block that is given up must still lie there when the branch in front is inverted.
        inverted.removeAllMatching([&](Air::BasicBlock* source) {
            if (!replacements.contains(source->successorBlock(0)))
                return false;
            postponed.append(source->successorBlock(1));
            return true;
        });
        for (Air::BasicBlock* block : postponed)
            replacements.remove(block);
        if (replacements.isEmpty()) {
            if (takesWhatIsFallenInto)
                return;
            takesWhatIsFallenInto = true;
            continue;
        }
        for (Air::BasicBlock* block : code) {
            for (Air::BasicBlock*& successor : block->successorBlocks()) {
                if (auto found = replacements.find(successor); found != replacements.end())
                    successor = found->value;
            }
        }
        for (Air::BasicBlock* source : inverted) {
            std::swap(source->successor(0), source->successor(1));
            source->last().args()[0] = source->last().args()[0].inverted();
        }
        code.resetReachability();
        takesWhatIsFallenInto = false;
    }
}

// `split`: if the function can be split, that is all that is done.
static bool compilePart(const Program& program, FunctionID id, const Function& function, SplitFunction* split, bool& hasFrame, CompiledCode& result)
{
    LoweringContext context;
    Procedure proc(/* usesSIMD = */ false);
    if (!program.isHotByText(id))
        proc.setOptLevel(1);
    proc.setPositionIndependent();
    proc.setMaterializesConstantsByFrequency();
    proc.setRotatesLoops();
    pinRegisters(proc);
#if CPU(X86_64)
    proc.code().setUnsavedCalleeSaves(RegisterSet { X86Registers::r12 });
#endif
    proc.code().setChargesForCalleeSaves();
    if (Options::aotMapFilePath()) [[unlikely]]
        proc.setNeedsPCToOriginMap();
    context.nilTestMarker = createSharedTask<StackmapGeneratorFunction>([](CCallHelpers&, const StackmapGenerationParams&) { });
    proc.setLastPhase(createSharedTask<void(Procedure&)>([&](Procedure& proc) {
        useFaultsForNilTests(proc, context);
    }));
    Lowering lowering(proc, program, id, function, context);
    if (!lowering.run()) {
        result.whyDeclined = lowering.whyFailed();
        return false;
    }
    if (split && context.emitsCalls && !context.readsCallerFrame && splitFunction(program, id, function, context.blocksThatEmitCalls, *split))
        return true;
    estimateFrequencies(proc);
    if (Options::dumpAOTB3Graph() && result.mayBeDumped) [[unlikely]]
        dataLogLn("AOT B3:\n", proc);
    for (B3::Value* value : proc.values()) {
        if (!value->hasInt64())
            continue;
        uint64_t bits = value->asInt64();
        if (IntegersThatLookLikeAddresses::isOne(bits) && !context.wideIntegerConstants.contains(static_cast<int64_t>(bits))) {
            result.whyDeclined = "an address in the code"_s;
            return false;
        }
    }

    CCallHelpers::JumpList frameDoesNotFit;
    proc.code().setPrologueForEntrypoint(0, createSharedTask<Air::PrologueGeneratorFunction>([&](CCallHelpers& jit, Air::Code& code) {
        if (context.hasNoFrame(code))
            return;
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.emitFunctionPrologue();
        // A general entry calls its function, which tests at once.
        bool isLeftToCallee = function.isGenericEntrypoint() && function.inlineFrames.size() <= 1;
        if ((context.emitsCalls && !isLeftToCallee) || code.frameSize() > 256)
            frameDoesNotFit = emitStackCheck(jit, code.frameSize());
        else if (code.frameSize())
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(code.frameSize()), CCallHelpers::stackPointerRegister);
        jit.emitSave(code.calleeSaveRegisterAtOffsetList());
    }));
    proc.code().setEpilogueGenerator(createSharedTask<Air::PrologueGeneratorFunction>([&](CCallHelpers& jit, Air::Code& code) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context.emitFrameTeardown(jit, code);
        jit.ret();
    }));

    for (unsigned i = 0; i < context.numberOfCatchEntrypoints; ++i) {
        proc.code().setPrologueForEntrypoint(i + 1, createSharedTask<Air::PrologueGeneratorFunction>([](CCallHelpers& jit, Air::Code& code) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        }));
    }

    generateToAir(proc);
    usePinnedRegistersDirectly(proc.code());
    Air::prepareForGeneration(proc.code());
    mergeIdenticalBlocks(proc.code());
    CCallHelpers jit;
    // The function starts where its code starts.
    CCallHelpers::Jump jumpToMainEntrypoint;
    if (context.numberOfCatchEntrypoints)
        jumpToMainEntrypoint = jit.jump();
    CCallHelpers::Label codeStart = jit.label();
    jit.setOopsEmitsBreakpointOnly();
    generate(proc, jit);
    emitStackOverflow(jit, context.stubCalls, frameDoesNotFit);
    if (jumpToMainEntrypoint.isSet())
        jumpToMainEntrypoint.linkTo(proc.code().entrypointLabel(0), &jit);
    else
        RELEASE_ASSERT(!CCallHelpers::differenceBetween(codeStart, proc.code().entrypointLabel(0)));
    jit.breakpoint();
    jit.padBeforePatch();
    Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::FTL);

    CompiledFunctionInfo info;
    info.stubCalls = context.stubCalls.link(linkBuffer);
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    info.convention = Convention { Signature::Registers, 0, false };
    if (program.at(id).isGenericEntrypoint())
        info.convention = Convention { program.at(id).has(FunctionUsesArgumentBuffer) ? Signature::List : Signature::Registers, lowering.genericEntrypointParameterCount(), true };
    info.frameSizeInBytes = proc.frameSize();
    info.knownCallees = WTF::move(context.knownCallees);
    info.indexReferences = context.indexReferences.link(linkBuffer);
    info.numSlots = context.sites.size();
    info.sites = WTF::move(context.sites);
    info.siteConstants.fill(0, info.numSlots);
    info.plans = WTF::move(context.plans);
    for (auto [slot, row] : context.rowsOfSlots)
        info.siteConstants[slot] = (row + 1) | CompiledFunctionInfo::siteConstantIsPlan;
    info.startsCold = info.numSlots;
    for (StringID name : context.identifiers) {
        info.typedIdentifierIndices.append(program.identifierNumbers[name.index()]);
        info.typedIdentifierNames.append(String::fromUTF8(program.string(name)));
    }
    info.calleeSaveRegisters = proc.calleeSaveRegisterAtOffsetList();
    for (unsigned i = 0; i < context.numberOfCatchEntrypoints; ++i)
        info.catchEntrypoints.append({ i, static_cast<unsigned>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(proc.code().entrypointLabel(i + 1)).untaggedPtr()) - static_cast<uint8_t*>(start)) });
    info.typedCatchEntrypointsForCallSites = WTF::move(context.catchEntrypointsForCallSites);
    for (auto& access : context.faultingAccesses) {
        auto& test = context.nilTests[access.nilTest];
        uint32_t codeOffset = static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(access.instruction).untaggedPtr()) - static_cast<uint8_t*>(start);
        info.faultingAccesses.append({ codeOffset, test.callSite, throwSiteWords(test.kind, Rep::Cell, static_cast<unsigned>(access.base), test.a, test.b) });
    }
    std::ranges::sort(info.faultingAccesses, { }, &CompiledFunctionInfo::FaultingAccess::codeOffset);
    info.hasNoFrame = context.hasNoFrame(proc.code());
    info.typedFunction = id.index();
    // By index into info.typedSourceNames.
    Vector<ModuleID, 2> sources;
    auto positionIn = [&](const Function& owner, uint32_t position, uint32_t frame) {
        // No position: the start of the function. A module starts at offset 0.
        if (!position && owner.kind != FunctionKind::Module)
            position = owner.position;
        size_t source = sources.find(owner.module);
        if (source == notFound) {
            source = sources.size();
            sources.append(owner.module);
            StringID path = owner.module ? program.modules[owner.module.index()].displayPath : StringID();
            if (!path && owner.module)
                path = program.modules[owner.module.index()].path;
            info.typedSourceNames.append(path ? UTF8CString(byteCast<char8_t>(program.string(path))) : UTF8CString(u8"unknown"));
        }
        LineColumn lineColumn = program.lineColumnFor(owner.module, position);
        return TypedSitePosition { lineColumn.line, lineColumn.column, static_cast<uint32_t>(source), frame };
    };
    // The sites are numbered anew, in the order of their return addresses: the image has no number to store for them, and what the optimizer
    // removed has no entry anywhere. In front stay those that have a position and no code: see LoweringContext::numberOfSitesWithoutCode.
    {
        uint32_t numberOfSitesWithoutCode = context.numberOfSitesWithoutCode;
        info.numberOfSitesWithoutCode = numberOfSitesWithoutCode;
        Vector<std::pair<uint32_t, uint32_t*>> returns;
        for (auto& call : info.stubCalls) {
            if (call.callSite != StubCall::noCallSite && !call.isTailCall)
                returns.append({ call.offset, &call.callSite });
        }
        for (auto& access : info.faultingAccesses)
            returns.append({ access.codeOffset, &access.callSite });
        std::ranges::sort(returns, { }, [](auto& entry) {
            return entry.first;
        });
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<uint32_t> catchEntrypointOf;
        catchEntrypointOf.fill(none, context.sitePositions.size() + 1);
        for (auto [site, entrypoint] : info.typedCatchEntrypointsForCallSites)
            catchEntrypointOf[site] = entrypoint;
        decltype(context.sitePositions) positions;
        positions.append(context.sitePositions.span().first(numberOfSitesWithoutCode));
        decltype(info.typedCatchEntrypointsForCallSites) catchEntrypoints;
        for (auto [offset, site] : returns) {
            uint32_t old = CallSiteIndex(PackedSite::bits(*site)).bytecodeIndex().offset();
            RELEASE_ASSERT(old > numberOfSitesWithoutCode && old <= context.sitePositions.size());
            positions.append(context.sitePositions[old - 1]);
            uint32_t number = positions.size();
            if (catchEntrypointOf[old] != none)
                catchEntrypoints.append({ number, catchEntrypointOf[old] });
            *site = PackedSite::pack(PackedSite::inlineFrame(*site), CallSiteIndex(BytecodeIndex(number)).bits());
        }
        context.sitePositions = WTF::move(positions);
        info.typedCatchEntrypointsForCallSites = WTF::move(catchEntrypoints);
    }
    info.typedPositions.append(positionIn(function, function.position, 0));
    for (auto& site : context.sitePositions)
        info.typedPositions.append(positionIn(site.inlineFrame ? program.at(function.inlineFrames[site.inlineFrame].callee) : function, site.position, site.inlineFrame));
    for (unsigned frame = 0; frame < function.inlineFrames.size(); ++frame)
        info.inlineFrames.append({ function.inlineFrames[frame].parent, context.inlineCallSites[frame], function.inlineFrames[frame].callee.index(), false });
    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::dumpAOTDisassembly() && result.mayBeDumped, linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
    dropPaddingAtEnd(info, start, proc.code());
    if (Options::aotMapFilePath()) [[unlikely]]
        result.bytesByOperation = bytesByOperation(proc.pcToOriginMap(), linkBuffer, info.codeSize);
    result.bytes.append(std::span { static_cast<const uint8_t*>(start), static_cast<size_t>(info.codeSize) });
    result.info = WTF::move(info);
    hasFrame = !context.hasNoFrame(proc.code());
    return true;
}

bool compile(VM&, const Program& program, FunctionID id, CompiledCode& result)
{
    bool hasFrame = false;
    SplitFunction split;
    if (!compilePart(program, id, program.at(id), &split, hasFrame, result))
        return false;
    if (split.continuations.isEmpty())
        return true;
    bool mayBeDumped = result.mayBeDumped;
    auto compileParts = [&] {
        // The point of the entry part is that it has no frame.
        if (!compilePart(program, id, split.entry, nullptr, hasFrame, result) || hasFrame)
            return false;
        for (auto& continuation : split.continuations) {
            CompiledCode code;
            code.mayBeDumped = mayBeDumped;
            if (!compilePart(program, id, continuation, nullptr, hasFrame, code))
                return false;
            result.continuations.append(WTF::move(code));
        }
        return true;
    };
    if (compileParts())
        return true;
    result = CompiledCode { };
    result.mayBeDumped = mayBeDumped;
    return compilePart(program, id, program.at(id), nullptr, hasFrame, result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

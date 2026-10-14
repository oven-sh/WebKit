/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTInstructionsInCode.h"
#include "AOTOutliner.h"
#include "AirBlockInsertionSet.h"
#include "AirCode.h"
#include "AirGenerate.h"
#include "AirInstInlines.h"
#include "AirInsertionSet.h"
#include "B3BasicBlockInlines.h"
#include "B3Generate.h"
#include "B3InsertionSetInlines.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
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
    m_out.noteFoldedConstantsIn(m_context.wideIntegerConstants);
    static const char* const compactSetAsked = getenv("BUN_AOT_SWITCH_COMPACT2");
    static const char* const compactSet = compactSetAsked ? compactSetAsked : "g";
    static const char* const compactLevers = getenv("BUN_AOT_SWITCH_COMPACT2_LEVERS");
    m_isCompact = !function.isBodyOfSeveralSites && (!strcmp(compactSet, "all") || (strchr(compactSet, 'g') && program.isColdByGraph(id)) || strchr(compactSet, program.isHotByText(id) ? 'h' : program.isRareByText(id) ? 'r' : 'c'));
    m_compactLevers = compactLevers ? static_cast<unsigned>(atoi(compactLevers)) : 458781;
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
        if (m_stringBuilderAnalysis.isState(parameters[i])) {
            arguments.append(m_out.int64Zero);
            if (LValue phi = m_cellPhis.get(parameters[i].index()))
                cells.append({ phi, m_out.int64Zero });
            continue;
        }
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
        if (LValue phi = m_truthinessPhis.get(parameters[i].index()).isTruthy)
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
        initializeStringBuildersOnEdge(target);
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
                m_out.setOrigin(std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(m_function, m_function.at(terminator.value))) | static_cast<uintptr_t>(terminator.value.index() + 1) << 32));
            if (isAwaitMayContinue(m_function.at(terminator.value)))
                branchOnAwaitMayContinue(m_function.at(terminator.value), terminator.targets[0].block, isOne, isNone);
            else if (isGuardOfReceiver(m_function.at(terminator.value)))
                branchOnOriginalStructure(m_function.at(terminator.value), terminator, isOne, isNone);
            else if (isTestOfForInStamp(m_function.at(terminator.value)))
                branchOnForInStamp(m_function.at(terminator.value), isOne, isNone);
            else
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
            m_out.ret(terminator.value ? in<Boxed>(terminator.value).value() : undefined().value());
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
        if (LValue caught = terminator.targets.isEmpty() ? nullptr : m_caughtValues.get(terminator.targets[0].block.index())) {
            Boxed thrown = in<Boxed>(terminator.value);
            if (m_program.passesExceptionsOn && typeOf(terminator.value).isAnything() && !exceptionCaughtIn(terminator.targets[0].block))
                thrown = AOT_TYPED_OPERATION(ValueOfException, thrown);
            LBasicBlock handler = edgeTo(terminator.targets[0], false);
            m_out.addIncomingToPhi(caught, m_out.anchor(thrown.value()));
            m_out.jump(handler);
            return;
        }
        std::optional<Boxed> thrown;
        if (!isFetchedWhereNeeded(terminator.value))
            thrown = in<Boxed>(terminator.value);
        if (!terminator.targets.isEmpty())
            m_watchingHandler = &terminator.targets[0];
        if (thrown)
            AOT_TYPED_OPERATION(Throw, *thrown);
        else
            throwNewError(m_function.at(terminator.value));
        didLowerWatchedOperation();
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
    out.print("\nS");
    if (offsetOf(0))
        out.print("\t0\t0\t0\t", offsetOf(0));
    for (size_t i = 0; i < ranges.size(); ++i) {
        uintptr_t origin = std::bit_cast<uintptr_t>(ranges[i].origin.dfgOrigin());
        if (offsetOf(i + 1) > offsetOf(i))
            out.print("\t", origin >> 8 & 0xffff, "\t", origin >> 32 | (origin & 1) << 31, "\t", offsetOf(i), "\t", offsetOf(i + 1) - offsetOf(i));
    }
    return out.toString();
}

void Lowering::lowerBlock(BlockID id)
{
    m_out.appendTo(m_blocks[id.index()]);
    m_currentBlockID = id;
    m_isInLoop = m_loopBlocks.get(id.index());
    bool didEmitCalls = std::exchange(m_context.emitsCalls, false);
#if CPU(X86_64)
    m_context.holdsColdSites = false;
#endif
    auto& block = m_function.at(id);
    m_currentBlock = &block;
    m_leavesNextIntervalToSlowPath = isCompact(18);
    for (ValueID parameter : block.parameters)
        didDefineReceiverOfRun(parameter);
    materializeFinalStringBuilderStates(block);
    auto classForMap = [&](ValueID id) -> uintptr_t {
        if (!id)
            return 0;
        auto& type = typeOf(id);
        if (type.anyObject)
            return m_function.at(id).prediction ? 2 : 1;
        bool mayBeNil = type.mayBeUndefinedOrNull();
        if (type.hasObjects()) {
            if (type.primitives & ~Primitive::UndefinedOrNull)
                return 7;
            return (type.views.size() + type.engineClasses.size() + type.anyFunction == 1 ? 3 : 5) + mayBeNil;
        }
        return type.isNumber() || type.isString() || type.isBoolean() ? 8 : 9;
    };
    auto conditionForMap = [&](ValueID id) -> uintptr_t {
        while (id && m_function.at(id).opcode == Opcode::LogicalNot)
            id = m_function.at(id).child(0);
        if (!id)
            return 0;
        auto& condition = m_function.at(id);
        auto isOperand = [&](Opcode opcode) {
            return (condition.child(0) && m_function.at(condition.child(0)).opcode == opcode) || (condition.child(1) && m_function.at(condition.child(1)).opcode == opcode);
        };
        switch (condition.opcode) {
        case Opcode::StrictEq:
        case Opcode::Eq:
            return isOperand(Opcode::String) ? 1 : isOperand(Opcode::Integer) ? 2 : 3;
        case Opcode::ToBoolean:
            return 4;
        case Opcode::Less:
        case Opcode::LessEq:
            return 5;
        case Opcode::CallBuiltin:
            return 7;
        default:
            return 6;
        }
    };
    auto formForMap = [&](ValueID id, const Value& value) -> uintptr_t {
        if (value.isAtKnownPlace())
            return 13;
        const Field* field = fieldFor(typeOf(value.child(0)), value.string());
        if (!field)
            return 0;
        bool mayBeAbsent = field->stored && m_program.at(field->stored).primitives & Primitive::Absent;
        bool tests = false;
        if (value.opcode == Opcode::GetById && field->stored) {
            Type held = m_program.at(field->stored);
            if (held.primitives & Primitive::Absent)
                held.primitives = (held.primitives & ~Primitive::Absent) | Primitive::Undefined;
            tests = !held.isWithin(typeOf(id), m_program);
        }
        return 1 + (field->isOutOfLine ? 2 : mayBeAbsent ? 1 : 0) + 3 * (field->isLackedBySomeMember || field->isElsewhereInSomeMember) + 6 * tests;
    };
    for (unsigned i = 0; i < block.values.size(); ++i) {
        ValueID valueID = block.values[i];
        auto& value = m_function.at(valueID);
        m_currentIndexInBlock = i;
        if (block.terminator.kind == TerminatorKind::Invoke && i + 1 == block.values.size() && !m_handlersEnteredByBranch.get(block.terminator.targets[1].block.index()))
            m_watchingHandler = &block.terminator.targets[1];
        m_unconditionalBlock = m_out.m_block;
        if (Options::aotMapFilePath()) [[unlikely]]
            m_out.setOrigin(std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(m_function, value)) | static_cast<uintptr_t>(valueID.index() + 1) << 32 | classForMap(value.child(0)) << 56 | ((value.opcode == Opcode::GetById || value.opcode == Opcode::PutById) && valueID.index() + 1 < 1u << 20 ? (valueID.index() + 1 < 1u << 17 ? formForMap(valueID, value) << 3 | (value.rep == Rep::Int32 && typeOf(valueID).isBoolean() ? 0 : static_cast<uintptr_t>(value.rep)) : 15 << 3) : 0) << 49 | static_cast<uintptr_t>(m_isInLoop)));
        double frequency = m_out.m_block->frequency();
        Operand result = lower(valueID, value);
        didLowerWatchedOperation();
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
        if (m_isFillingReusedEnvironment && i + 1 == block.values.size()) [[unlikely]] {
            dataLogLn("AOT: f", m_id.index(), ": a block ends between the birth of an environment that is used again and its barrier");
            CRASH();
        }
        if (isFetchedWhereNeeded(valueID))
            continue;
        didDefineReceiverOfRun(valueID);
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
        m_out.setOrigin(std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(firstTerminatorForMap + static_cast<unsigned>(block.terminator.kind), false)) | static_cast<uintptr_t>(0x800000 + id.index() + 1) << 32 | (id.index() + 1 < 1u << 16 ? conditionForMap(block.terminator.value) : 0) << 48 | classForMap(block.terminator.value) << 56));
    lowerTerminator(block.terminator);
#if CPU(X86_64)
    if (m_context.holdsColdSites && LoweringContext::splitsAsIfColdSitesCalled())
        m_context.blocksThatEmitCalls.set(id.index());
#endif
    if (m_context.emitsCalls)
        m_context.blocksThatEmitCalls.set(id.index());
    m_context.emitsCalls |= didEmitCalls;
}

void Lowering::findRowOfRun(RunOfAccesses& run) const
{
    if (Options::auditAOTTypedAssertions())
        return;
    const Vector<uint32_t, 1>* fewest = nullptr;
    for (StringID name : run.names) {
        auto found = m_program.numbersOfRowsByName.find(name.index() + 1);
        if (found == m_program.numbersOfRowsByName.end())
            return;
        if (!fewest || found->value.size() < fewest->size())
            fewest = &found->value;
    }
    uint32_t only = Format::none;
    for (uint32_t number : *fewest) {
        auto& row = m_program.numberedRows[number];
        if (!std::ranges::all_of(run.names, [&](StringID name) { return row.names.contains(name); }))
            continue;
        if (only != Format::none)
            return;
        only = number;
    }
    if (only == Format::none || only >= m_program.firstNumberOfRowsOfThis || !m_program.rowsThatStayAsBorn.get(only))
        return;
    auto& row = m_program.numberedRows[only];
    if (row.names.size() > row.inlineCapacity)
        return;
    run.numberOfRow = only;
    for (StringID name : run.names)
        run.places.append(row.names.find(name));
}

void Lowering::findRunsOfAccesses(const Vector<BlockID>& order)
{
    if (m_function.isTyped() || !m_program.isHotByText(m_id) || m_program.runsOnce(m_id))
        return;
    struct Access {
        BlockID block;
        unsigned index;
        StringID name;
        bool isStore;
    };
    struct Receiver {
        ValueID value;
        Vector<Access, 4> accesses;
        unsigned numberDominated { 0 };
    };
    Vector<Receiver> receivers;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> indexOfReceiver;
    for (BlockID id : order) {
        auto& block = m_function.at(id);
        for (unsigned i = 0; i < block.values.size(); ++i) {
            auto& value = m_function.at(block.values[i]);
            if ((value.opcode != Opcode::GetById && value.opcode != Opcode::PutById) || !value.receiverIsExpectedOfOneRow || value.isFirstStoreToThis() || isFetchedWhereNeeded(block.values[i]) || value.string() == m_length)
                continue;
            ValueID object = value.child(0);
            if (m_function.at(object).rep != Rep::JSValue || isFetchedWhereNeeded(object))
                continue;
            auto added = indexOfReceiver.add(object.index(), receivers.size());
            if (added.isNewEntry)
                receivers.append(Receiver { object, { }, 0 });
            receivers[added.iterator->value].accesses.append(Access { id, i, value.string(), value.opcode == Opcode::PutById });
        }
    }
    auto isDominated = [&](const Receiver& receiver, const Access& access) {
        for (auto& other : receiver.accesses) {
            if (&other != &access && (other.block == access.block ? other.index < access.index : dominates(other.block, access.block)))
                return true;
        }
        return false;
    };
    for (auto& receiver : receivers) {
        for (auto& access : receiver.accesses)
            receiver.numberDominated += isDominated(receiver, access);
    }
    std::ranges::stable_sort(receivers, [](auto& a, auto& b) {
        return a.numberDominated > b.numberDominated;
    });
    auto callsOnTheUsualWay = [](Opcode opcode) {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Integer:
        case Opcode::String:
        case Opcode::Boolean:
        case Opcode::GetModuleVariable:
        case Opcode::PutModuleVariable:
        case Opcode::GetById:
        case Opcode::PutById:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::IsUndefinedOrNull:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::Cast:
            return false;
        default:
            return true;
        }
    };
    constexpr unsigned mostNames = 24;
    for (auto& receiver : receivers) {
        if (!receiver.numberDominated || m_runsOfAccesses.size() == 6)
            break;
        RunOfAccesses run;
        for (auto& access : receiver.accesses) {
            size_t index = run.names.find(access.name);
            if (index == notFound) {
                if (run.names.size() == mostNames)
                    continue;
                index = run.names.size();
                run.names.append(access.name);
            }
            if (access.isStore)
                run.stored |= 1u << index;
        }
        findRowOfRun(run);
        unsigned slots = run.numberOfRow != Format::none ? 0 : (run.names.size() + 7) / 8;
        if (m_context.sites.size() + slots > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
            break;
        run.header = m_context.sites.size();
        for (unsigned i = 0; i < slots; ++i)
            m_context.sites.append(Site { });
        run.verdict = m_proc.addVariable(Int32);
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), run.verdict, m_out.int32Zero);
        for (unsigned k = 0; k < receiver.accesses.size(); ++k) {
            auto& access = receiver.accesses[k];
            if (isDominated(receiver, access))
                m_accessesThatAnotherDominates.add(keyOfAccessInRun(access.block, access.index));
            if (!k || receiver.accesses[k - 1].block != access.block)
                continue;
            auto& block = m_function.at(access.block);
            bool follows = true;
            for (unsigned i = receiver.accesses[k - 1].index + 1; i < access.index; ++i)
                follows &= !callsOnTheUsualWay(m_function.at(block.values[i]).opcode);
            if (follows)
                m_accessesThatFollowAnother.add(keyOfAccessInRun(access.block, access.index));
        }
        m_runsOfAccesses.add(receiver.value.index(), WTF::move(run));
    }
}

void Lowering::didDefineReceiverOfRun(ValueID id)
{
    if (m_runsOfAccesses.isEmpty())
        return;
    if (auto found = m_runsOfAccesses.find(id.index()); found != m_runsOfAccesses.end())
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), found->value.verdict, m_out.int32Zero);
}

void Lowering::forgetVerdictsOfRuns()
{
    if (m_runsOfAccesses.isEmpty())
        return;
    HeapRange structureIDs = m_heaps.JSCell_structureID.range();
    InsertionSet insertions(m_proc);
    auto forgetAt = [&](unsigned index, B3::Origin origin) {
        B3::Value* none = insertions.insertIntConstant(index, origin, Int32, 0);
        for (auto& run : m_runsOfAccesses.values())
            insertions.insert<VariableValue>(index, B3::Set, origin, run.verdict, none);
    };
    UncheckedKeyHashSet<B3::BasicBlock*> behindTerminals;
    for (B3::BasicBlock* block : m_proc) {
        for (unsigned i = 0; i < block->size(); ++i) {
            B3::Value* value = block->at(i);
            HeapRange writes = value->effects().writes;
            if (!writes || (value->as<MemoryValue>() && (!writes.overlaps(structureIDs) || m_storesThatKeepStructures.contains(value))))
                continue;
            if (i + 1 == block->size()) {
                for (B3::BasicBlock* successor : block->successorBlocks())
                    behindTerminals.add(successor);
                continue;
            }
            forgetAt(i + 1, value->origin());
        }
        insertions.execute(block);
    }
    for (B3::BasicBlock* block : m_proc) {
        if (!behindTerminals.contains(block))
            continue;
        forgetAt(0, block->at(0)->origin());
        insertions.execute(block);
    }
}

bool Lowering::run()
{
    if (m_function.blocks.isEmpty()) {
        fail("a function without a body"_s);
        return false;
    }
    static_assert(maxInlineFrames == PackedSite::maxInlineFrames);
    for (uint32_t position : m_function.suspensionPositions) {
        ++m_nextSite;
        m_context.sitePositions.append({ position, 0 });
    }
    for (auto& frame : m_function.inlineFrames) {
        bool isFunctionItself = m_context.inlineCallSites.isEmpty();
        m_context.inlineCallSites.append(isFunctionItself ? 0 : CallSiteIndex(BytecodeIndex(m_nextSite++)).bits());
        if (!isFunctionItself)
            m_context.sitePositions.append({ frame.callPosition, frame.parent });
    }
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (!value.isInTailPosition || m_function.isBodyOfSeveralSites || m_context.framesLeftByTailCalls.contains(value.inlineFrame) || m_context.inlineCallSites.size() > maxInlineFrames)
                continue;
            if (m_context.inlineCallSites.isEmpty())
                m_context.inlineCallSites.append(0);
            m_context.framesLeftByTailCalls.append(value.inlineFrame);
            m_context.inlineCallSites.append(CallSiteIndex(BytecodeIndex(m_nextSite++)).bits());
            m_context.sitePositions.append({ 0, value.inlineFrame });
        }
    }
    m_context.numberOfSitesWithoutCode = m_nextSite - 1;
    for (uint32_t index = 0; index < m_function.values.size() && !Options::validateAOTTypedFacts(); ++index) {
        auto& value = m_function.values[index];
        if (!value.hasRowOfBirth() || !m_program.rowsOfBirths[value.rowOfBirth()].isWhole || m_context.sites.size() + 2 > maxSlotsOfFunction())
            continue;
        m_slotsOfWholeBirths.add(index, m_context.sites.size());
        m_context.sites.append(Site { });
        m_context.sites.append(Site { });
    }
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
    findHandlersEnteredByBranch(order);
    for (BlockID block : order) {
        m_blocks[block.index()] = m_out.newBlock();
        if (m_function.at(block).terminator.kind == TerminatorKind::Unreachable || m_function.at(block).terminator.kind == TerminatorKind::Throw || m_handlersEnteredByBranch.get(block.index()))
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
    m_out.appendTo(prologue);
    m_out.initializeConstants(m_proc, prologue);
    findPinnedRegisters();
    if (!lowerEntryParameters())
        return false;
    findTruthinessPhis();
    findClosureTestsCarriedOutByBranches();
    findLeavesFetchedLate();
    findConcatenationsWithNumbers();
    findStringsReadInLoops();
    findIterationsOfStrings();
    findEntriesOfArrays();
    findElementsTakenAtOnce();
    findIterationsInFrame();
    findParametersJoinedInParts();
    findNumbersJoinedWithIntegers(order);
    findNumbersJoinedWithInt32(order);
    BitVector isThrownToInItsFunction;
    for (BlockID block : order) {
        auto& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Throw || terminator.targets.isEmpty())
            continue;
        auto& handler = m_function.at(terminator.targets[0].block);
        if (handler.values.containsIf([&](ValueID id) { return m_function.at(id).opcode == Opcode::CatchValue; }))
            isThrownToInItsFunction.set(terminator.targets[0].block.index());
    }
    findRunsOfAccesses(order);
    for (BlockID block : order) {
        if (!block.index())
            continue;
        m_out.appendTo(m_blocks[block.index()]);
        if (isThrownToInItsFunction.get(block.index()))
            m_caughtValues.add(block.index(), m_out.phi(Int64));
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
                m_truthinessPhis.add(id.index(), Truthiness { m_values[id.index()].value, m_out.phi(Int32) });
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
    m_out.appendTo(prologue);
    m_out.jump(m_blocks[0]);
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
    m_heaps.computeRangesAndDecorateInstructions();
    forgetVerdictsOfRuns();
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
        else if (!isDouble && nextGPR < numberOfDirectArgumentGPRs) {
            value = registerOnEntry(directArgumentGPR(nextGPR++));
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

TypedPointer Lowering::inStatics(const FTL::AbstractHeap& heap, ptrdiff_t offset)
{
    if (!isX86())
        return m_out.address(heap, fixedPointer(Instance::offsetOfTypedStatics()), offset);
    return m_out.address(heap, m_instance, Instance::displacementOfTypedStatics(m_program.sizeOfStatics) + offset);
}

TypedPointer Lowering::cardOf(uint32_t offsetInStatics)
{
    if (!isX86())
        return m_out.address(m_heaps.AOTTyped_cards, fixedPointer(Instance::offsetOfTypedCards()), offsetInStatics >> Instance::typedCardShift);
    return m_out.address(m_heaps.AOTTyped_cards, m_instance, Instance::displacementOfTypedCards(m_program.sizeOfStatics) + static_cast<ptrdiff_t>(offsetInStatics >> Instance::typedCardShift));
}

LValue Lowering::startOfStatics()
{
    if (!isX86())
        return fixedPointer(Instance::offsetOfTypedStatics());
    return m_out.add(m_instance, m_out.constIntPtr(Instance::displacementOfTypedStatics(m_program.sizeOfStatics)));
}

LValue Lowering::startOfCards()
{
    if (!isX86())
        return fixedPointer(Instance::offsetOfTypedCards());
    return m_out.add(m_instance, m_out.constIntPtr(Instance::displacementOfTypedCards(m_program.sizeOfStatics)));
}

TypedPointer Lowering::addressOf(VariableID id)
{
    return inStatics(m_heaps.AOTTyped_variables[id.index()], m_program.variables[id.index()].offset);
}

// What was read from a variable that may hold nothing yet.
void Lowering::checkInitialized(Operand raw, StringID name)
{
    requireOrThrowAt(logicalNot(isAbsent(raw)), ThrowKind::TDZ, std::nullopt, name.index());
}

void Lowering::validateIsInitialized(Operand raw)
{
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isAbsent(raw).value(), rarely(doesNotHold), usually(continuation));
    m_out.appendTo(doesNotHold);
    {
        SetForScope isReporting(m_isReportingThatFactDoesNotHold, true);
        AOT_TYPED_OPERATION(FactDoesNotHold, Boxed(m_out.int64Zero), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
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
    if (variable.mayBeSeenUninitialized() && !value.findsVariableInitialized())
        checkInitialized(raw, variable.name);
    if (Options::validateAOTTypedFacts() && variable.mayBeSeenUninitialized() && value.findsVariableInitialized()) [[unlikely]]
        validateIsInitialized(raw);
    if (Options::validateAOTTypedFacts() && variable.onlyFunctionIsFact) [[unlikely]]
        validateOnlyFunction(raw, result, variable.onlyFunction, false);
    return ensure(raw, m_program.at(variable.type ? variable.type : m_program.types.anything), typeOf(result));
}

LValue Lowering::wordOfGlobalPropertyCache(uint32_t cacheNumber, size_t offset)
{
    return marked(m_out.load64(inStatics(m_heaps.AOTTyped_collectorState, m_program.globalPropertyCachesOffset + (cacheNumber - 1) * sizeof(GlobalPropertyCache) + offset)), IsSafe);
}

Boxed Lowering::valueOfGlobalPropertyOrElse(StringID name, uint32_t cacheNumber, LBasicBlock miss, bool isFreeName)
{
    Boxed value(wordOfGlobalPropertyCache(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, value)));
    orElse(m_out.notZero64(value.value()), miss);
    if (Options::validateAOTTypedFacts() && isFreeName) [[unlikely]]
        AOT_TYPED_OPERATION(ValidateGlobalForGood, word32(name.index()), value);
    return value;
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
    if (!cacheNumber || !m_out.m_block->frequency())
        return generally();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fast = m_out.anchor(valueOfGlobalPropertyOrElse(value.string(), cacheNumber, miss).value());
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
    ValueFromBlock fast = m_out.anchor(valueOfGlobalPropertyOrElse(name, cacheNumber, miss, false).value());
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
    auto generally = [&, isSloppy = isInSloppyCode()] {
        if (isSloppy)
            AOT_TYPED_OPERATION(SetFreeSloppy, name, word32(cacheNumber), stored);
        else
            AOT_TYPED_OPERATION(SetFree, name, word32(cacheNumber), stored);
    };
    if (!cacheNumber) {
        generally();
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
        generally();
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

void Lowering::lowerPutModuleVariable(const Value& value)
{
    auto& variable = m_program.variables[value.variable().index()];
    didEmitWrite(Writes::Fields);
    Operand stored = in(value.child(0), repOf(variable.store));
    if (Options::validateAOTTypedFacts() && (variable.onlyFunctionIsFact || variable.onlyFunctionOrNil)) [[unlikely]]
        validateOnlyFunction(stored, value.child(0), variable.onlyFunction, true);
    store(variable.store, stored, addressOf(value.variable()));
    if (variable.hasCard && holdsCells(variable.store))
        m_out.store32As8(m_out.constInt32(Instance::typedCardIsStoredTo), cardOf(variable.offset));
}

// Only compiled code gets to see an environment or the frame of a coroutine.
Operand Lowering::lowerGetEnvironmentField(ValueID result, const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        validateIsNotReleased(cell, value);
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
    if (Options::validateAOTTypedFacts() && field.onlyFunctionIsFact) [[unlikely]]
        validateOnlyFunction(raw, result, field.onlyFunction, false);
    if (Options::validateAOTTypedFacts() && field.otherFunction) [[unlikely]]
        validateOnlyFunction(raw, result, field.onlyFunction, true, field.otherFunction);
    return ensure(raw, stored.withoutPrimitives(Primitive::Absent), typeOf(result));
}

// Stores to one environment that stand side by side, as those to the frame of a coroutine in front of a suspension. Nothing between them allocates, so the barrier behind the last is that of all.
bool Lowering::isFollowedByStoreWithBarrier(ValueID environment) const
{
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        return false;
    for (size_t i = m_currentIndexInBlock + 1; i < m_currentBlock->values.size(); ++i) {
        auto& next = m_function.at(m_currentBlock->values[i]);
        if (next.opcode == Opcode::Integer || next.opcode == Opcode::Unspecified)
            continue;
        if (next.opcode != Opcode::PutField || next.child(0) != environment || isObjectLayout(m_program.at(next.layout()).kind))
            return false;
        if (isLeftToItsFirstUse(next.child(1)) || isFetchedWhereNeeded(next.child(1)) || isInitializingStore(m_program, m_function, *m_currentBlock, i))
            return false;
        if (holdsCells(m_program.at(next.layout()).fields[next.field()].store) && typeOf(next.child(1)).hasCells() && m_function.at(next.child(1)).opcode != Opcode::Unspecified)
            return true;
    }
    return false;
}

bool Lowering::leavesBarrierToPark(ValueID frame) const
{
    if (Options::validateAOTTypedFacts())
        return false;
    auto isOfFrame = [&](const Value& access) {
        return access.child(0) == frame && m_program.at(access.layout()).isCoroutineFrame;
    };
    auto isResumeClosure = [&](const Value& value) {
        return value.opcode == Opcode::GetField && isOfFrame(value) && value.field() == CoroutineFrame::resumeClosureField;
    };
    auto isThereAlready = [&](ValueID id) {
        return !isLeftToItsFirstUse(id) && !isFetchedWhereNeeded(id);
    };
    for (size_t i = m_currentIndexInBlock + 1; i < m_currentBlock->values.size(); ++i) {
        auto& next = m_function.at(m_currentBlock->values[i]);
        if (next.opcode == Opcode::Integer || next.opcode == Opcode::Unspecified || isResumeClosure(next))
            continue;
        if (next.opcode == Opcode::PutField && isOfFrame(next)) {
            Opcode stored = m_function.at(next.child(1)).opcode;
            if (stored != Opcode::Integer && stored != Opcode::Unspecified && !isThereAlready(next.child(1)))
                return false;
            continue;
        }
        if (next.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = m_builtins.at(next.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::SuspendAsyncFunction)
            return false;
        if (!isResumeClosure(m_function.at(next.arguments[0])) || !isThereAlready(next.arguments[1]))
            return false;
        return true;
    }
    return false;
}

void Lowering::lowerPutEnvironmentField(const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    ValueID stored = value.child(1);
    if (Options::validateAOTTypedFacts() && !isLoweringInitializingStore()) [[unlikely]]
        validateIsNotReleased(cell, value);
    didEmitWrite(isLocalAllocation(m_function, value.child(0)) ? Writes::RuntimeState : Writes::Fields);
    Operand operand = in(stored, repOf(field.store));
    if (Options::validateAOTTypedFacts() && (field.onlyFunctionIsFact || field.onlyFunctionOrNil || field.otherFunction)) [[unlikely]]
        validateOnlyFunction(operand, stored, field.onlyFunction, true, field.otherFunction);
    store(field.store, operand, fieldAt(cell, field));
    if (isLoweringInitializingStore())
        didLowerInitializingStore();
    else if (holdsCells(field.store) && typeOf(stored).hasCells() && m_function.at(stored).opcode != Opcode::Unspecified && !m_program.at(value.layout()).isInFrame) {
        m_environmentAwaitingBarrier = isFollowedByStoreWithBarrier(value.child(0)) || leavesBarrierToPark(value.child(0)) ? value.child(0) : ValueID();
        if (!m_environmentAwaitingBarrier)
            barrier(cell);
    }
    auto& layout = m_program.at(value.layout());
    if (!layout.reuseVariable || value.field() + 1 != layout.fields.size())
        return;
    auto& hasEscaped = layout.fields[value.field() - 1];
    auto& variable = m_program.variables[layout.reuseVariable.index()];
    RELEASE_ASSERT(hasEscaped.store == Store::Boolean);
    Operand released = in(value.child(0), repOf(variable.store));
    LBasicBlock release = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(load(hasEscaped.store, fieldAt(cell, hasEscaped), 0, cell.value()).value, unsure(continuation), unsure(release));
    m_out.appendTo(release);
    for (unsigned i = 0; i + 2 < layout.fields.size(); ++i) {
        auto& variableOfEnvironment = layout.fields[i];
        if (!holdsCells(variableOfEnvironment.store) || variableOfEnvironment.isOutOfLine || variableOfEnvironment.flags & fieldIsNeverStored)
            continue;
        Operand nothing;
        if (m_program.at(variableOfEnvironment.stored).primitives & Primitive::Undefined)
            nothing = convert(undefined(), Type::ofPrimitives(Primitive::Undefined), repOf(variableOfEnvironment.store));
        else if (repOf(variableOfEnvironment.store) == Rep::Cell)
            nothing = CellPointer(m_out.int64Zero);
        else if (repOf(variableOfEnvironment.store) == Rep::JSValue)
            nothing = Boxed(m_out.int64Zero);
        else
            continue;
        store(variableOfEnvironment.store, nothing, fieldAt(cell, variableOfEnvironment));
    }
    didEmitWrite(Writes::RuntimeState);
    if (!Options::validateAOTTypedFacts()) [[likely]] {
        TypedPointer place = addressOf(layout.reuseVariable);
        store(variable.store, released, TypedPointer(m_heaps.AOTTyped_runtimeState, place.base(), place.offset()));
    }
    if (variable.hasCard && holdsCells(variable.store) && !Options::validateAOTTypedFacts())
        m_out.store32As8(m_out.constInt32(Instance::typedCardIsStoredTo), cardOf(variable.offset));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

CellPointer Lowering::lowerNewPromise()
{
    static_assert(static_cast<unsigned>(Stub::TypedNewPromise) + 1 == static_cast<unsigned>(Stub::LeafTypedNewPromise));
    if (!m_isInLoop && !m_program.isHotOutsideItsLoops(m_id))
        return CellPointer(callAllocationStub(Stub::TypedNewPromise, { }, effectsOfOperation<operationAOTTypedNewPromise>(), false));
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

bool Lowering::isPromiseOfUnsuspendedFunction(ValueID promise) const
{
    if (m_function.startFunction) {
        bool isReturned = false;
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            ValueID returned = block.terminator.value;
            while (returned && m_function.at(returned).opcode == Opcode::Cast)
                returned = m_function.at(returned).child(0);
            if (returned != promise)
                return false;
            isReturned = true;
        }
        return isReturned;
    }
    if (!m_id.index() || m_function.blocks[0].parameters.isEmpty() || m_function.blocks[0].parameters.last() != promise)
        return false;
    auto& resume = m_program.at(FunctionID(m_id.index() - 1));
    if (!m_program.isResumeFunction(resume))
        return false;
    auto& frame = m_program.at(m_program.at(resume.captureTypes[0]).onlyView());
    return m_program.at(frame.fields[CoroutineFrame::promiseField].type).onlyEngineClass() == &m_builtins.promiseClass();
}

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
    auto& type = typeOf(value.arguments[1]);
    bool isFresh = isPromiseOfUnsuspendedFunction(value.arguments[0]);
    bool mayBePrimitiveCell = type.primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt);
    bool lacksThenUnlessMarked = this->lacksThenUnlessMarked(type);
    if (isFresh && type.isObject() && !lacksThenUnlessMarked) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(ResolvePromise, effects, promise, resolution);
        return;
    }
    bool mayGoOutOfLine = !isFresh || type.hasObjects();
    LBasicBlock inPlace = m_out.newBlock();
    LBasicBlock outOfLine = mayGoOutOfLine ? m_out.newBlock() : nullptr;
    LBasicBlock continuation = m_out.newBlock();
    if (!isFresh) {
        LBasicBlock isUntouched = m_out.newBlock();
        m_out.branch(isZero(loadField<Integer>(promise, m_heaps.JSPromise_packed)).value(), unsure(isUntouched), unsure(outOfLine));
        m_out.appendTo(isUntouched);
    }
    if (type.hasObjects()) {
        LBasicBlock isCellOfSomeKind = m_out.newBlock();
        m_out.branch(isCell(resolution, type).value(), unsure(isCellOfSomeKind), unsure(inPlace));
        m_out.appendTo(isCellOfSomeKind);
        LBasicBlock isObjectCase = lacksThenUnlessMarked ? m_out.newBlock() : outOfLine;
        if (mayBePrimitiveCell)
            m_out.branch(below(typeOfCell(asCell(resolution)), word32(ObjectType)).value(), unsure(inPlace), unsure(isObjectCase));
        else
            m_out.jump(isObjectCase);
        if (lacksThenUnlessMarked) {
            m_out.appendTo(isObjectCase);
            m_out.branch(lacksBits(orderWordOf(asCell(resolution)), orderWordHasExtraProperties).value(), usually(inPlace), rarely(outOfLine));
        }
    } else
        m_out.jump(inPlace);
    m_out.appendTo(inPlace);
    didEmitWrite(effects.writes);
    static_assert(CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer == 48);
    storeField(promise, m_heaps.JSPromise_slot, resolution);
    storeField(promise, m_heaps.JSPromise_packed, integer(static_cast<int64_t>(static_cast<uint64_t>(JSPromise::Status::Fulfilled) | JSPromise::isFirstResolvingFunctionCalledFlag) << 48));
    if (mayBePrimitiveCell || lacksThenUnlessMarked)
        barrier(promise);
    m_out.jump(continuation);
    if (mayGoOutOfLine) {
        m_out.appendTo(outOfLine);
        AOT_TYPED_OPERATION_WITH_EFFECTS(ResolvePromise, effects, promise, resolution);
        m_out.jump(continuation);
    }
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
        if (m_blockOfDefinitionsMadeWithCopy == m_currentBlock && m_definitionsMadeWithCopy.contains(m_currentIndexInBlock))
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
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
        if (!m_isCarryingOutMergeGroup) {
            if (auto group = findMergeGroup(m_currentIndexInBlock)) {
                m_mergeGroup = group;
                return placeholder(value.rep);
            }
        }
        if (auto group = findCloneGroup(m_currentIndexInBlock)) {
            m_cloneGroup = WTF::move(group);
            if (m_cloneGroup->isDecidedAtClone)
                decideCloneGroup(*m_cloneGroup);
            return placeholder(value.rep);
        }
        return ensure(countPlainAllocation(PlainAllocationKind::Clone, id, cloneObject(value.arguments[0], true)), m_program.at(m_program.types.object), type);
    case LanguageOperation::CopyDataProperties:
        if (m_mergeGroup && m_mergeGroup->first + 1 == m_currentIndexInBlock)
            return placeholder(value.rep);
        if (!m_isCarryingOutMergeGroup && isMadeHere(value.arguments[0])) {
            if (auto group = findMergeGroup(m_currentIndexInBlock)) {
                m_mergeGroup = group;
                return placeholder(value.rep);
            }
        }
        if (isMadeHere(value.arguments[0])) {
            // In the place of a NewObject: what follows counts on an ordinary object.
            CellPointer object = countPlainAllocation(PlainAllocationKind::Clone, value.arguments[0], cloneObject(value.arguments[1], false, whatIsDefinedOnCopy(value.arguments[0], keyOrdersExpectedOf(value.arguments[1]).isEmpty())));
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
    if (builtin.implementation.operation == LanguageOperation::RequireIteratorResultObject) {
        Boxed step = in<Boxed>(value.arguments[0]);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isNoObject = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isCell(step).value(), usually(isCellCase), rarely(isNoObject));
        m_out.appendTo(isCellCase);
        m_out.branch(below(typeOfCell(asCell(step)), word32(ObjectType)).value(), rarely(isNoObject), usually(continuation));
        m_out.appendTo(isNoObject);
        outOfLine();
        m_out.unreachable();
        m_out.appendTo(continuation);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (builtin.implementation.operation == LanguageOperation::CopyDataPropertiesExcluding || builtin.implementation.operation == LanguageOperation::CloneObjectWith || builtin.implementation.operation == LanguageOperation::CloneObjectAfter) {
        auto expected = predictionOf(id);
        LayoutID view = expected ? expected->onlyView() : LayoutID();
        if (view && m_program.at(view).kind == LayoutKind::Record && m_program.at(view).shape && !Options::auditAOTTypedAssertions()) {
            CellPointer made = copyRecordAs(value, view, builtin.implementation.operation == LanguageOperation::CopyDataPropertiesExcluding ? NamesOfCopy::Excluded : builtin.implementation.operation == LanguageOperation::CloneObjectWith ? NamesOfCopy::StoredNext : NamesOfCopy::StoredFirst);
            LBasicBlock isMade = m_out.newBlock();
            LBasicBlock isNotMade = newColdBlock();
            Join join(m_out, value.rep, b3TypeOf(value.rep));
            m_out.branch(m_out.notZero64(made.value()), usually(isMade), rarely(isNotMade));
            m_out.appendTo(isMade);
            join.add(ensure(made, m_program.at(m_program.types.object), type));
            m_out.appendTo(isNotMade);
            Vector<StringID, 4> names;
            bool isLiteral = builtin.implementation.operation == LanguageOperation::CloneObjectAfter && value.arguments.size() % 2;
            for (size_t i = 1; i < value.arguments.size() && isLiteral; i += 2) {
                auto& name = m_function.at(value.arguments[i]);
                isLiteral = name.opcode == Opcode::String && !names.contains(name.string()) && !m_program.string(name.string()).empty() && !isASCIIDigit(m_program.string(name.string())[0]);
                if (isLiteral)
                    names.append(name.string());
            }
            if (!isLiteral) {
                join.add(outOfLine());
                return join.finish();
            }
            Vector<Boxed, 4> values;
            for (size_t i = 2; i < value.arguments.size(); i += 2)
                values.append(in<Boxed>(value.arguments[i]));
            LValue inBuffer[] = { box(countPlainAllocation(PlainAllocationKind::Object, id, newObjectLiteral(names.span(), values.span()))).value(), in<Boxed>(value.arguments[0]).value() };
            LValue buffer = transientBuffer(std::size(inBuffer) * sizeof(EncodedJSValue));
            for (unsigned i = 0; i < std::size(inBuffer); ++i)
                m_out.store64(inBuffer[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
            join.add(ensure(AOT_TYPED_OPERATION(LanguageOperation, word32(static_cast<int32_t>(LanguageOperation::CopyDataProperties)), word32(std::size(inBuffer)), Pointer(buffer)), type));
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
    if (auto deferred = isCompact(16) ? std::nullopt : deferToBranchOnText(id, value))
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
        if (isStoredAsLazyClosure(id))
            return placeholder(value.rep);
        if (auto* store = m_currentIndexInBlock + 1 == m_currentBlock->values.size() ? lazyClosureStoreBehindJump(*m_currentBlock) : nullptr) {
            putLazyClosure(in<Boxed>(store->child(0)), store->string(), value.function());
            return placeholder(value.rep);
        }
        return convert(lowerNewClosure(value), type, value.rep);
    case Opcode::NewClosureOnce:
        return convert(lowerNewClosureOnce(value), type, value.rep);
    case Opcode::GetArgument: {
        uint32_t index = value.immediates[0];
        return ensure(either<Boxed>(below(word32(index), m_argumentCount), [&] {
            return Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, m_argumentBuffer.value(), index * sizeof(EncodedJSValue))));
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
    case Opcode::ConstructSuperForwardVarargs:
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
    case Opcode::MaterializeEnvironment:
        return lowerMaterializeEnvironment(value);
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
        if (auto deleted = fieldFor(typeOf(value.child(0)), value.string()) ? std::optional<Word32>() : emitDeleteByVal(in<Boxed>(value.child(0)), box(stringConstant(value.string()))))
            return *deleted;
        if (isInSloppyCode())
            return AOT_TYPED_OPERATION(DeleteByValSloppy, in<Boxed>(value.child(0)), box(stringConstant(value.string())));
        return AOT_TYPED_OPERATION(DeleteByName, in<Boxed>(value.child(0)), word32(value.string().index()));
    case Opcode::InByVal:
        if (!expectedTypeOf(value.child(1)).isNumber())
            return emitInByVal(in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
        if (Operand operand = get(value.child(0)); holdsCells(operand.rep)) {
            LBasicBlock generally = newUnlikelyBlock();
            LBasicBlock absent = m_out.newBlock();
            LBasicBlock isDouble = m_out.newBlock();
            LBasicBlock isNotDouble = m_out.newBlock();
            Join join(m_out, Rep::Int32, Int32);
            if (operand.rep == Rep::JSValue)
                orElse(isCell(operand.as<Boxed>()), generally);
            CellPointer array = operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>());
            orElse(isPlainArray(array), generally);
            Integer index = indexOrElse(value.child(1), generally);
            Pointer butterfly = butterflyOf(array);
            orElse(below(index, publicLength(butterfly)), absent);
            Word32 shape = indexingModeOf(array, IndexingShapeMask);
            m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(isDouble), unsure(isNotDouble));
            m_out.appendTo(isDouble);
            Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            join.add(equal(asDouble, asDouble));
            m_out.appendTo(isNotDouble);
            orElse(notEqual(shape, word32(UndecidedShape)), absent);
            orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), generally);
            join.add(isNotEmpty(Boxed(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())))));
            m_out.appendTo(absent);
            orElse(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), generally);
            join.add(boolean(false));
            m_out.appendTo(generally);
            join.add(AOT_TYPED_OPERATION(HasKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1))));
            return join.finish();
        }
        return AOT_TYPED_OPERATION(HasKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::DeleteByVal:
        if (auto deleted = expectedTypeOf(value.child(1)).isNumber() ? std::optional<Word32>() : emitDeleteByVal(in<Boxed>(value.child(0)), in<Boxed>(value.child(1))))
            return *deleted;
        if (isInSloppyCode())
            return AOT_TYPED_OPERATION(DeleteByValSloppy, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
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
        {
            Vector<StringID, 16> names;
            unsigned numberOfKeys = 0;
            collectPropertiesGivenTo(m_program, m_function, id, 3, names, numberOfKeys);
            unsigned room = names.size() + numberOfKeys;
            if (value.hasRowOfBirth()) {
                auto& row = m_program.rowsOfBirths[value.rowOfBirth()];
                room = std::max<unsigned>(room, row.firstPlace + row.names.size());
                if (row.isWhole && !Options::validateAOTTypedFacts())
                    fail("a birth that was settled to be whole has no literal"_s);
            }
            return ensure(countPlainAllocation(PlainAllocationKind::Object, id, newEmptyObject(room)), m_program.at(m_program.types.object), type);
        }
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
    case Opcode::ToBoolean:
        return lowerToBoolean(value.child(0));
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
    case Opcode::IsUndefinedOrNull:
        return lowerIsUndefinedOrNull(value.child(0));
    case Opcode::IsInstanceOfClass:
        return lowerIsInstanceOfClass(value);
    case Opcode::HasShapeOf:
        return lowerHasShapeOf(value);
    case Opcode::IsClassObjectAsDeclared:
        return lowerIsClassObjectAsDeclared(value);
    case Opcode::GetStaticField:
        return lowerGetStaticField(id, value);
    case Opcode::PutStaticField:
        lowerPutStaticField(value);
        return undefined();
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
    case Opcode::ModuleEvaluationWord: {
        LValue data = fixedPointer(Instance::offsetOfTypedData());
        LValue table = m_out.add(data, m_out.zeroExtPtr(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, data, OBJECT_OFFSETOF(Data, moduleEvaluationsOffset))), IsFixed)));
        LValue address = m_out.add(table, m_out.shl(in<Integer>(value.child(0)).value(), m_out.constInt32(2)));
        return convert(Integer(m_out.signExt32To64(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, address, value.immediates[0] * sizeof(uint32_t))), IsFixed))), type, value.rep);
    }
    case Opcode::OffsetIndex:
        return convert(Integer(m_out.add(in<Integer>(value.child(0)).value(), m_out.constInt64(value.immediates[0]))), type, value.rep);
    case Opcode::GetModuleVariableAt:
        return ensure(Boxed(m_out.load64(TypedPointer(m_heaps.AOTTyped_variables.atAnyNumber(), m_out.add(startOfStatics(), in<Integer>(value.child(0)).value())))), type);
    case Opcode::PutModuleVariableAt:
        didEmitWrite(Writes::Fields);
        m_out.store64(in<Boxed>(value.child(1)).value(), TypedPointer(m_heaps.AOTTyped_variables.atAnyNumber(), m_out.add(startOfStatics(), in<Integer>(value.child(0)).value())));
        m_out.store32As8(m_out.constInt32(Instance::typedCardIsStoredTo), TypedPointer(m_heaps.AOTTyped_cards, m_out.add(startOfCards(), m_out.lShr(in<Integer>(value.child(0)).value(), m_out.constInt32(Instance::typedCardShift)))));
        return convert(undefined(), type, value.rep);
    case Opcode::CallModuleFunction:
        return ensure(Boxed(callICStub(Stub::CallModuleFunction, Int64, { { in<Integer>(value.child(0)).value(), GPRInfo::argumentGPR1 } })), type);
    case Opcode::FailModuleCycle:
        AOT_TYPED_OPERATION(FailModuleCycle, in<Integer>(value.child(0)), word32(value.immediates[0]), in<Boxed>(value.child(1)));
        return convert(undefined(), type, value.rep);
    case Opcode::AddToModuleEvaluationDepth: {
        didEmitWrite(Writes::RuntimeState);
        TypedPointer depth = m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfModuleEvaluationDepth());
        m_out.store32(m_out.add(m_out.load32(depth), m_out.constInt32(static_cast<int32_t>(value.immediates[0]))), depth);
        return convert(undefined(), type, value.rep);
    }
    case Opcode::JumpToModuleEvaluator:
        jumpToFunction(indexOfCallee(m_program.moduleEvaluatorForCallFromOutside()), { in<Integer>(value.child(0)).value() });
        return convert(undefined(), type, value.rep);
    // The passes have turned these into something else.
    case Opcode::Operation:
    case Opcode::Await:
    case Opcode::Yield:
    case Opcode::GetResumeMode:
    case Opcode::AssertNotAllocated:
        RELEASE_ASSERT_NOT_REACHED();
    case Opcode::Cast:
        return lowerCast(id, value);
    case Opcode::CatchValue: {
        if (m_handlersEnteredByBranch.get(m_currentBlockID.index()))
            return convert(undefined(), m_program.at(m_program.types.undefined), value.rep);
        if (ValueID exception = exceptionCaughtIn(m_currentBlockID); exception && exception != id)
            return ensure(AOT_TYPED_OPERATION(ValueOfException, in<Boxed>(exception)), type);
        if (LValue caught = m_caughtValues.get(m_currentBlockID.index()))
            return ensure(Boxed(caught), type);
        return ensure(catchException(m_currentBlockID), type);
    }
    case Opcode::LoopHint:
        poll();
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

static void resequenceMovesBeforePatches(Air::Code& code)
{
    auto isSimpleMove = [](Air::Inst& inst) {
        if ((inst.kind.opcode != Air::Move && inst.kind.opcode != Air::MoveDouble) || inst.args().size() != 2 || !inst.args()[1].isReg())
            return false;
        if (inst.args()[0].isReg())
            return true;
        return inst.kind.opcode == Air::Move && (inst.args()[0].isSomeImm() || inst.args()[0].isZeroReg());
    };
    struct PendingMove {
        Air::Arg source;
        Reg destination;
        Air::Inst* original;
    };
    for (Air::BasicBlock* block : code) {
        bool didChange = false;
        for (unsigned end = 0; end < block->size(); ++end) {
            Air::Inst& patch = block->at(end);
            if (patch.kind.opcode != Air::Patch)
                continue;
            unsigned begin = end;
            while (begin && block->at(begin - 1) && isSimpleMove(block->at(begin - 1)))
                --begin;
            if (end - begin < 2)
                continue;
            Vector<PendingMove, 8> pending;
            bool handsOnWideImmediate = false;
            auto valueOf = [&](Air::Arg arg) {
                if (!arg.isReg())
                    return arg;
                for (auto& move : pending) {
                    if (move.destination == arg.reg()) {
                        handsOnWideImmediate |= move.source.isBigImm();
                        return move.source;
                    }
                }
                return arg;
            };
            for (unsigned i = begin; i < end; ++i) {
                Air::Inst& inst = block->at(i);
                Air::Arg source = valueOf(inst.args()[0]);
                Reg destination = inst.args()[1].reg();
                pending.removeAllMatching([&](auto& move) { return move.destination == destination; });
                pending.append({ source, destination, &inst });
            }
            if (handsOnWideImmediate)
                continue;
            RegisterSet used;
            RegisterSet overwritten = patch.extraEarlyClobberedRegs();
            overwritten.merge(patch.extraClobberedRegs());
            patch.forEachTmp([&](Air::Tmp& tmp, Air::Arg::Role role, Bank, Width) {
                if (!tmp.isReg())
                    return;
                if (Air::Arg::isAnyUse(role))
                    used.add(tmp.reg(), IgnoreVectors);
                if (Air::Arg::isAnyDef(role))
                    overwritten.add(tmp.reg(), IgnoreVectors);
            });
            pending.removeAllMatching([&](auto& move) {
                if (move.source.isReg() && move.source.reg() == move.destination)
                    return true;
                return overwritten.contains(move.destination, IgnoreVectors) && !used.contains(move.destination, IgnoreVectors);
            });
            if (pending.size() >= end - begin)
                continue;
            Vector<Air::Inst, 8> ordered;
            while (!pending.isEmpty()) {
                size_t next = pending.findIf([&](auto& candidate) {
                    return !pending.containsIf([&](auto& other) {
                        return &other != &candidate && other.source.isReg() && other.source.reg() == candidate.destination;
                    });
                });
                if (next == notFound)
                    break;
                auto& move = pending[next];
                ordered.append(Air::Inst(move.destination.isGPR() ? Air::Move : Air::MoveDouble, move.original->origin, move.source, Air::Tmp(move.destination)));
                pending.removeAt(next);
            }
            if (!pending.isEmpty())
                continue;
            for (unsigned i = begin; i < end; ++i)
                block->at(i) = i - begin < ordered.size() ? ordered[i - begin] : Air::Inst();
            didChange = true;
        }
        if (didChange)
            block->insts().removeAllMatching([](const Air::Inst& inst) { return !inst; });
    }
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
static bool isEveryVariableSetWhereItIsRead(B3::Procedure& proc)
{
    if (!proc.variables().size())
        return true;
    auto indexOf = [](B3::Value* value) {
        return value->as<B3::VariableValue>()->variable()->index();
    };
    Vector<BitVector> setAtHead(proc.size());
    BitVector isReached;
    isReached.set(proc.at(0)->index());
    Vector<B3::BasicBlock*, 16> worklist { proc.at(0) };
    while (!worklist.isEmpty()) {
        B3::BasicBlock* block = worklist.takeLast();
        BitVector isSet = setAtHead[block->index()];
        for (B3::Value* value : *block) {
            if (value->opcode() == B3::Set)
                isSet.set(indexOf(value));
        }
        for (B3::BasicBlock* successor : block->successorBlocks()) {
            BitVector& atHead = setAtHead[successor->index()];
            if (!isReached.get(successor->index())) {
                isReached.set(successor->index());
                atHead = isSet;
                worklist.append(successor);
                continue;
            }
            BitVector onEveryWay = atHead;
            onEveryWay.filter(isSet);
            if (onEveryWay == atHead)
                continue;
            atHead = WTF::move(onEveryWay);
            worklist.append(successor);
        }
    }
    for (B3::BasicBlock* block : proc) {
        if (!isReached.get(block->index()))
            continue;
        BitVector isSet = setAtHead[block->index()];
        for (B3::Value* value : *block) {
            if (value->opcode() == B3::Set)
                isSet.set(indexOf(value));
            else if (value->opcode() == B3::Get && !isSet.get(indexOf(value)))
                return false;
        }
    }
    return true;
}

static void placeResultsOfInvokes(Air::Code& code)
{
    code.resetReachability();
    Air::BlockInsertionSet blockInsertionSet(code);
    Air::InsertionSet insertionSet(code);
    for (Air::BasicBlock* block : code) {
        unsigned terminalIndex = block->size() - 1;
        while (!block->at(terminalIndex).isTerminal())
            --terminalIndex;
        if (terminalIndex + 1 == block->size())
            continue;
        Vector<Air::Inst, 2> moves;
        for (unsigned i = terminalIndex + 1; i < block->size(); ++i)
            moves.append(block->at(i));
        Air::FrequentedBlock& continuation = block->successor(0);
        if (continuation.block()->numPredecessors() == 1) {
            insertionSet.insertInsts(0, moves);
            insertionSet.execute(continuation.block());
        } else {
            Air::BasicBlock* edge = blockInsertionSet.insertBefore(continuation.block(), continuation.block()->frequency());
            for (const Air::Inst& move : moves)
                edge->appendInst(move);
            edge->appendInst(Air::Inst(Air::Jump, moves.last().origin));
            edge->successors().append(continuation.block());
            continuation.block() = edge;
        }
        block->resize(terminalIndex + 1);
    }
    if (blockInsertionSet.execute())
        code.resetReachability();
}

static bool compilePart(const Program& program, FunctionID id, const Function& function, SplitFunction* split, bool& hasFrame, CompiledCode& result)
{
    LoweringContext context;
    Procedure proc(/* usesSIMD = */ false);
    if (Options::airDumpPhaseStats() || Options::airGreedyRegAllocDumpFunction()) [[unlikely]]
        proc.setName(makeString('#', id.index(), '#'));
    proc.setPositionIndependent();
    proc.setMaterializesConstantsByFrequency();
    proc.setRotatesLoops();
    pinRegisters(proc);
#if CPU(X86_64)
    proc.code().setUnsavedCalleeSaves(RegisterSet { X86Registers::r12 });
#endif
    proc.code().setChargesForCalleeSaves();
    proc.code().setLaysRareBlocksInGaps();
    if (auto& outlining = OutliningOptions::asked(); outlining && outlining->takesRareCode)
        proc.code().setNotesStartOfRareCode();
    if (Options::aotMapFilePath()) [[unlikely]]
        proc.setNeedsPCToOriginMap();
    context.nilTestMarker = createSharedTask<StackmapGeneratorFunction>([](CCallHelpers&, const StackmapGenerationParams&) { });
    proc.setLastPhase(createSharedTask<void(Procedure&)>([&](Procedure& proc) {
        foldNumberBoxings(proc, context);
        askSignExtendedWordsIn32Bits(proc);
        askZeroExtendedWordsIn32Bits(proc);
        useFaultsForNilTests(proc, context);
        askMemoryAtLoopPolls(proc);
    }));
    Lowering lowering(proc, program, id, function, context);
    if (!lowering.run()) {
        result.whyDeclined = lowering.whyFailed();
        return false;
    }
    if (split && context.emitsCalls && !context.readsCallerFrame && splitFunction(program, id, function, context.blocksThatEmitCalls, *split))
        return true;
    splitBranchConditions(proc);
    estimateFrequencies(proc);
    placeSlotBlockFetches(proc, context);
    if (Options::dumpAOTB3Graph() && result.mayBeDumped) [[unlikely]]
        dataLogLn("AOT B3:\n", proc);
    if (!isEveryVariableSetWhereItIsRead(proc)) {
        result.whyDeclined = "a variable of the back end's is read that is not set on every way there"_s;
        return false;
    }
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
    bool fitsByProof = false;
    proc.code().setPrologueForEntrypoint(0, createSharedTask<Air::PrologueGeneratorFunction>([&](CCallHelpers& jit, Air::Code& code) {
        if (context.hasNoFrame(code))
            return;
        AllowMacroScratchRegisterUsage allowScratch(jit);
        // A general entry calls its function, which tests at once.
        bool isLeftToCallee = function.isGenericEntrypoint() && function.inlineFrames.size() <= 1;
        static_assert(maxFrameSizeWithoutStackCheck <= 256);
        fitsByProof = context.emitsCalls && !isLeftToCallee && program.isEnteredWithRoomOnStack(id) && code.frameSize() <= maxFrameSizeWithoutStackCheck;
        isLeftToCallee |= fitsByProof;
#if CPU(ARM64)
        if (code.placesSpillsAboveFrameRecord()) {
            jit.m_assembler.stp<64>(GPRInfo::callFrameRegister, CCallHelpers::linkRegister, CCallHelpers::stackPointerRegister, PairPreIndex(-static_cast<int>(code.bytesAboveFrameRecord() + sizeof(CallerFrameAndPC))));
            jit.move(CCallHelpers::stackPointerRegister, GPRInfo::callFrameRegister);
            if ((context.emitsCalls && !isLeftToCallee) || code.frameSize() > 256)
                frameDoesNotFit = emitStackCheck(jit, 0);
            pushCalleeSaves(jit, code.calleeSaveRegisterAtOffsetList());
            return;
        }
#endif
        jit.emitFunctionPrologue();
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

    if (!context.readsCallerFrame && !(function.declaredAs & functionIsAsync))
        proc.code().setMayPlaceSpillsAboveFrameRecord();
    generateToAir(proc);
    if (context.unwindsToLandingPads)
        placeResultsOfInvokes(proc.code());
    usePinnedRegistersDirectly(proc.code());
    Air::prepareForGeneration(proc.code());
    resequenceMovesBeforePatches(proc.code());
    mergeIdenticalBlocks(proc.code());
    static const char* const recordsAsked = getenv("BUN_AOT_SWITCH_INSTRUCTION_RECORDS");
    uint8_t regionsForOutlining = OutliningOptions::isFromTypedLowering | (program.isHotByText(id) ? 0 : OutliningOptions::isNotHotByText) | (program.isColdByGraph(id) ? OutliningOptions::isColdByGraph : 0);
    const bool keepsRecords = (recordsAsked && recordsAsked[0] == '1') || OutliningOptions::asked();
    context.notesCode = keepsRecords;
    CCallHelpers jit;
#if CPU(X86_64)
    jit.m_assembler.noteLinkedJumps();
#endif
    CCallHelpers::Label codeStart = jit.label();
    jit.setOopsEmitsBreakpointOnly();
    generate(proc, jit);
    emitStackOverflow(jit, context.stubCalls, frameDoesNotFit, proc.code().frameSize());
    RELEASE_ASSERT(!CCallHelpers::differenceBetween(codeStart, proc.code().entrypointLabel(0)));
    jit.breakpoint();
    jit.padBeforePatch();
#if CPU(X86_64)
    jit.m_assembler.shortenLinkedJumps();
#endif
    Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::FTL);

    CompiledFunctionInfo info;
    info.stubCalls = context.stubCalls.link(linkBuffer);
    info.argumentMoves = context.stubCalls.takeArgumentMoves();
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    info.convention = Convention { Signature::Registers, 0, false };
    if (program.at(id).isGenericEntrypoint())
        info.convention = Convention { program.at(id).has(FunctionUsesArgumentBuffer) ? Signature::List : Signature::Registers, lowering.genericEntrypointParameterCount(), true };
    info.frameSizeInBytes = proc.frameSize();
    info.knownCallees = WTF::move(context.knownCallees);
    info.indexReferences = context.indexReferences.link(linkBuffer);
    info.numSlots = context.sites.size();
    info.numberOfEmptyArrayLiteralsMadeInPlace = context.numberOfEmptyArrayLiteralsMadeInPlace;
    info.numberOfEmptyArrayLiteralsMadeByStub = context.numberOfEmptyArrayLiteralsMadeByStub;
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
    for (auto [site, landingPad] : context.landingPadsForCallSites)
        info.typedCatchEntrypointsForCallSites.append({ site, static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(*context.landingPads[landingPad]).untaggedPtr()) - static_cast<uint8_t*>(start)) });
    for (auto& access : context.faultingAccesses) {
        auto& test = context.nilTests[access.nilTest];
        uint32_t codeOffset = static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(access.instruction).untaggedPtr()) - static_cast<uint8_t*>(start);
        info.faultingAccesses.append({ codeOffset, test.callSite, throwSiteWords(test.kind, Rep::Cell, static_cast<unsigned>(access.base), test.a, test.b) });
    }
    for (auto& trap : context.trapsAtThrowSites)
        info.faultingAccesses.append({ static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(trap.instruction).untaggedPtr()) - static_cast<uint8_t*>(start)), trap.callSite, { } });
    std::ranges::sort(info.faultingAccesses, { }, &CompiledFunctionInfo::FaultingAccess::codeOffset);
    info.hasNoFrame = context.hasNoFrame(proc.code());
    info.isEnteredWithoutStackCheck = frameDoesNotFit.empty();
    info.fitsOnStackByProof = fitsByProof;
    info.typedFunction = id.index();
    info.isGenericEntrypoint = function.isGenericEntrypoint();
    info.kindByte = kindByteOf(program, function);
    // By index into info.typedSourceNames.
    Vector<ModuleID, 2> sources;
    constexpr ModuleID moduleOfBuiltins { UINT32_MAX - 1 };
    auto sourceOfBuiltins = [&] {
        size_t source = sources.find(moduleOfBuiltins);
        if (source == notFound) {
            source = sources.size();
            sources.append(moduleOfBuiltins);
            info.typedSourceNames.append(UTF8CString(u8""));
        }
        return static_cast<uint32_t>(source);
    };
    auto positionIn = [&](const Function& owner, uint32_t position, uint32_t frame) {
        if (BuiltinID builtin = program.builtinCarriedOutBy(owner)) {
            bool isHostFunction = Builtins::singleton().isHostFunctionInEngine(builtin);
            return TypedSitePosition { isHostFunction ? 0u : 1u, isHostFunction ? 0u : 11u, sourceOfBuiltins(), frame };
        }
        if (program.isOfFileOfBuiltinFunctions(owner)) {
            auto* around = program.builtinFunctionAround(owner);
            if (!around || program.at(around->function).position == owner.position)
                return TypedSitePosition { 1, 11, sourceOfBuiltins(), frame };
            LineColumn start = program.lineColumnFor(owner.module, owner.position);
            return TypedSitePosition { start.line - program.lineColumnFor(owner.module, program.at(around->function).position).line + 1, start.column, sourceOfBuiltins(), frame };
        }
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
    size_t numberOfSitesThatAreNoItems;
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
            for (auto entry = std::ranges::lower_bound(context.itemSites, old, { }, &SpreadSite::callSite); entry != context.itemSites.end() && entry->callSite == old; ++entry)
                info.spreadSites.append({ *site, entry->item, entry->site });
        }
        numberOfSitesThatAreNoItems = positions.size();
        for (auto& entry : info.spreadSites) {
            positions.append(context.sitePositions[CallSiteIndex(PackedSite::bits(entry.site)).bytecodeIndex().offset() - 1]);
            entry.site = PackedSite::pack(PackedSite::inlineFrame(entry.site), CallSiteIndex(BytecodeIndex(positions.size())).bits());
        }
        context.sitePositions = WTF::move(positions);
        info.typedCatchEntrypointsForCallSites = WTF::move(catchEntrypoints);
        for (uint32_t i = numberOfSitesWithoutCode; i < context.sitePositions.size(); ++i) {
            if (context.sitePositions[i].constructs)
                info.sitesThatConstruct.append(i + 1);
        }
    }
    info.typedPositions.append(positionIn(function, function.position, 0));
    uint32_t firstFrameOfTailCall = context.framesLeftByTailCalls.isEmpty() ? UINT32_MAX : static_cast<uint32_t>(context.inlineCallSites.size() - context.framesLeftByTailCalls.size());
    for (auto& site : context.sitePositions) {
        uint32_t frame = site.inlineFrame >= firstFrameOfTailCall ? context.framesLeftByTailCalls[site.inlineFrame - firstFrameOfTailCall] : site.inlineFrame;
        info.typedPositions.append(positionIn(frame ? program.at(function.inlineFrames[frame].callee) : function, site.position, info.typedPositions.size() > numberOfSitesThatAreNoItems ? 0 : site.inlineFrame));
    }
    for (unsigned frame = 0; frame < function.inlineFrames.size(); ++frame)
        info.inlineFrames.append({ function.inlineFrames[frame].parent, context.inlineCallSites[frame], function.inlineFrames[frame].callee.index(), function.inlineFrames[frame].isTailCall, function.inlineFrames[frame].isConstruction });
    if (function.inlineFrames.isEmpty() && !context.framesLeftByTailCalls.isEmpty())
        info.inlineFrames.append({ 0, 0, FunctionID().index(), false, false });
    for (uint32_t left : context.framesLeftByTailCalls) {
        auto entrypoint = program.genericEntrypoints.find((left ? function.inlineFrames[left].callee : id).index());
        info.inlineFrames.append({ left, context.inlineCallSites[info.inlineFrames.size()], (entrypoint == program.genericEntrypoints.end() ? program.entries[0] : entrypoint->value).index(), true, false });
    }
    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::dumpAOTDisassembly() && result.mayBeDumped, linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
    dropPaddingAtEnd(info, start, proc.code());
    if (Options::aotMapFilePath()) [[unlikely]]
        result.bytesByOperation = makeString(bytesByOperation(proc.pcToOriginMap(), linkBuffer, info.codeSize), context.literalsForMap.isEmpty() ? ""_s : "\nV"_s, context.literalsForMap.toString());
    result.bytes.append(std::span { static_cast<const uint8_t*>(start), static_cast<size_t>(info.codeSize) });
    if (keepsRecords) {
        auto offsetOf = [&](CCallHelpers::Label label) {
            return static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(label).untaggedPtr()) - static_cast<uint8_t*>(start));
        };
        if (proc.code().notesStartOfRareCode())
            result.startOfRareCode = std::min<uint32_t>(offsetOf(proc.code().startOfRareCode()), info.codeSize);
        for (auto& data : context.dataInCode)
            result.dataInCode.append({ offsetOf(data.start), data.numberOfWords, false });
        for (auto [table, numberOfEntries] : proc.code().tablesInCode())
            result.dataInCode.append({ offsetOf(table), numberOfEntries, true });
        std::ranges::sort(result.dataInCode, { }, &DataInCode::offset);
#if CPU(X86_64)
        result.instructions = recordsOfInstructions(result.bytes.span(), result.dataInCode.span());
#endif
        result.hasNotes = true;
    }
    result.info = WTF::move(info);
    result.regionsForOutlining = regionsForOutlining;
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

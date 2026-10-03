/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3SwitchValue.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "FTLSwitchCase.h"
#include "JSCInlines.h"
#include "PreciseJumpTargetsInlines.h"
#include "UnlinkedCodeBlock.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT {

using namespace B3;

void Lowering::lowerTerminalOrFallThrough(BasicBlock* block, Node* node)
{
    if (!node) {
        RELEASE_ASSERT(block->successors.size() == 1);
        m_out.jump(edgeTo(block->successors[0]));
        return;
    }

    auto conditional = [&](LValue condition) {
        RELEASE_ASSERT(block->successors.size() == 2);
        m_out.branch(condition, unsure(edgeTo(block->successors[0])), unsure(edgeTo(block->successors[1])));
    };
    auto isUndefinedOrNull = [&](Node* value) { return this->isUndefinedOrNull(value); };
    auto equalsNull = [&](Node* value) { return this->equalsNull(value, true); };
    lowerTerminal(block, node, conditional, isUndefinedOrNull, equalsNull);
}

LValue Lowering::isUndefinedOrNull(Node* value)
{
    if (isSubtype(value->type, TOther))
        return m_out.booleanTrue;
    if (!mayBe(value->type, TOther))
        return m_out.booleanFalse;
    return isOther(lowJSValue(value));
}

LValue Lowering::equalsNull(Node* value, bool nullCounts)
{
    auto notCellCase = [&](LValue jsValue) {
        return nullCounts ? isOther(jsValue) : m_out.equal(jsValue, m_out.constInt64(JSValue::encode(jsUndefined())));
    };
    {
        if (!mayBe(value->type, TOther | TAnyObject))
            return m_out.booleanFalse;
        if (!mayBe(value->type, TAnyObject))
            return notCellCase(lowJSValue(value));
        LValue jsValue = lowJSValue(value);
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock masquerades = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 3> results;
        results.append(m_out.anchor(notCellCase(jsValue)));
        m_out.branch(isCell(jsValue), unsure(cellCase), unsure(continuation));
        m_out.appendTo(cellCase, masquerades);
        results.append(m_out.anchor(m_out.booleanFalse));
        m_out.branch(m_out.testNonZero32(m_out.load8ZeroExt32(jsValue, m_heaps.JSCell_typeInfoFlags), m_out.constInt32(MasqueradesAsUndefined)), rarely(masquerades), usually(continuation));
        m_out.appendTo(masquerades, continuation);
        results.append(m_out.anchor(m_out.equal(m_out.loadPtr(structureOf(jsValue), m_heaps.Structure_realm), m_globalObject)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, results);
    }
}

template<typename Conditional, typename IsUndefinedOrNull, typename EqualsNull>
void Lowering::lowerTerminal(BasicBlock* block, Node* node, const Conditional& conditional, const IsUndefinedOrNull& isUndefinedOrNull, const EqualsNull& equalsNull)
{
    switch (node->opcode) {
    case op_jmp:
        m_out.jump(edgeTo(block->successors[0]));
        return;
    case op_ret:
        if (!m_returnBlock)
            m_returnBlock = m_out.newBlock();
        if (m_graph.numberOfRegisterReturnValues) {
            Node* object = node->use(node->as<OpRet>().m_value);
            for (unsigned i = 0; i < m_registerReturnValues.size(); ++i)
                m_registerReturnValues[i].append(m_out.anchor(lowAs(object->use(NewObjectPlan::registerOf(i)), m_returnValueReps[i])));
        } else
            m_returnValues.append(m_out.anchor(lowAs(node->use(node->as<OpRet>().m_value), m_valueRepresentations.result)));
        m_out.jump(m_returnBlock);
        return;
    case op_unreachable:
        m_out.unreachable();
        return;
    case op_throw:
        vmCall(node, Void, Entry::operationAOTThrow, m_instance, lowJSValue(node->use(node->as<OpThrow>().m_value)));
        m_out.unreachable();
        return;
    case op_throw_static_error:
        throwStaticError(node);
        return;
    case op_iterator_close_check:
        conditional(iteratorCloseCheckCondition(node));
        return;
    case op_jtrue:
        conditional(toBoolean(node->use(node->as<OpJtrue>().m_condition)));
        return;
    case op_jfalse:
        conditional(m_out.logicalNot(toBoolean(node->use(node->as<OpJfalse>().m_condition))));
        return;
    case op_jeq_null:
        conditional(equalsNull(node->use(node->as<OpJeqNull>().m_value)));
        return;
    case op_jneq_null:
        conditional(m_out.logicalNot(equalsNull(node->use(node->as<OpJneqNull>().m_value))));
        return;
    case op_jundefined_or_null:
        conditional(isUndefinedOrNull(node->use(node->as<OpJundefinedOrNull>().m_value)));
        return;
    case op_jnundefined_or_null:
        conditional(m_out.logicalNot(isUndefinedOrNull(node->use(node->as<OpJnundefinedOrNull>().m_value))));
        return;
    case op_jeq_ptr: {
        auto bytecode = node->as<OpJeqPtr>();
        conditional(m_out.equal(lowJSValue(node->use(bytecode.m_value)), lowJSValue(node->use(bytecode.m_specialPointer))));
        return;
    }
    case op_jneq_ptr: {
        auto bytecode = node->as<OpJneqPtr>();
        conditional(m_out.notEqual(lowJSValue(node->use(bytecode.m_value)), lowJSValue(node->use(bytecode.m_specialPointer))));
        return;
    }

#define AOT_COMPARE_JUMP(Struct, opcodeName, canonical, negate) \
    case opcodeName: { \
        auto bytecode = node->as<Struct>(); \
        LValue result = lowerCompare(node, canonical, bytecode.m_lhs, bytecode.m_rhs); \
        conditional(negate ? m_out.logicalNot(result) : result); \
        return; \
    }
    AOT_COMPARE_JUMP(OpJless, op_jless, op_less, false)
    AOT_COMPARE_JUMP(OpJlesseq, op_jlesseq, op_lesseq, false)
    AOT_COMPARE_JUMP(OpJgreater, op_jgreater, op_greater, false)
    AOT_COMPARE_JUMP(OpJgreatereq, op_jgreatereq, op_greatereq, false)
    AOT_COMPARE_JUMP(OpJnless, op_jnless, op_less, true)
    AOT_COMPARE_JUMP(OpJnlesseq, op_jnlesseq, op_lesseq, true)
    AOT_COMPARE_JUMP(OpJngreater, op_jngreater, op_greater, true)
    AOT_COMPARE_JUMP(OpJngreatereq, op_jngreatereq, op_greatereq, true)
    AOT_COMPARE_JUMP(OpJbelow, op_jbelow, op_below, false)
    AOT_COMPARE_JUMP(OpJbeloweq, op_jbeloweq, op_beloweq, false)
#undef AOT_COMPARE_JUMP

    case op_jeq: {
        auto bytecode = node->as<OpJeq>();
        conditional(lowerEquality(node, false, bytecode.m_lhs, bytecode.m_rhs));
        return;
    }
    case op_jneq: {
        auto bytecode = node->as<OpJneq>();
        conditional(m_out.logicalNot(lowerEquality(node, false, bytecode.m_lhs, bytecode.m_rhs)));
        return;
    }
    case op_jstricteq: {
        auto bytecode = node->as<OpJstricteq>();
        conditional(lowerEquality(node, true, bytecode.m_lhs, bytecode.m_rhs));
        return;
    }
    case op_jnstricteq: {
        auto bytecode = node->as<OpJnstricteq>();
        conditional(m_out.logicalNot(lowerEquality(node, true, bytecode.m_lhs, bytecode.m_rhs)));
        return;
    }
    case op_switch_imm:
    case op_switch_char:
    case op_switch_string:
        lowerSwitch(node);
        return;
    default:
        unsupported(node);
        m_out.unreachable();
        return;
    }
}

void Lowering::dispatchOnString(Node* place, Node* scrutinee, LValue value, Vector<StringCase, 16>& all, LBasicBlock defaultBlock, bool isKnownCell)
{
    std::ranges::sort(all, [](const StringCase& a, const StringCase& b) {
        if (a.string->length() != b.string->length())
            return a.string->length() < b.string->length();
        return memcmp(a.string->span8().data(), b.string->span8().data(), a.string->length()) < 0;
    });
    auto proceedIf = [&](LValue condition, LBasicBlock otherwise) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition, usually(next), rarely(otherwise));
        m_out.appendTo(next);
    };
    if (!isKnownCell && !isSubtype(scrutinee->type, TCell))
        proceedIf(isCell(value), defaultBlock);
    if (!isSubtype(scrutinee->type, TString | ~TCell))
        proceedIf(m_out.equal(cellType(value), m_out.constInt32(StringType)), defaultBlock);

    if (!isCompact() && all.size() <= 8 && isAtomIfShortString(scrutinee) && std::ranges::all_of(all, [](const StringCase& one) { return one.constant && one.string->length() <= TypedLayoutTable::maxAtomizedStringLength; })) {
        LValue impl = m_out.loadPtr(value, m_heaps.JSRopeString_fiber0);
        for (auto& one : all) {
            LBasicBlock next = m_out.newBlock();
            m_out.branch(m_out.equal(impl, m_out.loadPtr(lowJSValue(one.constant), m_heaps.JSRopeString_fiber0)), unsure(one.target), unsure(next));
            m_out.appendTo(next);
        }
        m_out.jump(defaultBlock);
        return;
    }

    LBasicBlock slowCase = newColdBlock();
    LBasicBlock dispatch = m_out.newBlock();
    LBasicBlock ifLengthMatches = m_out.newBlock();
    Vector<ValueFromBlock, 2> lengthsOtherwise;
    auto [rawCharacters, rawLength] = latin1CharactersOf(value, ifLengthMatches, lengthsOtherwise);
    ValueFromBlock plainCharacters = m_out.anchor(rawCharacters);
    ValueFromBlock plainLength = m_out.anchor(rawLength);
    m_out.jump(dispatch);

    m_out.appendTo(ifLengthMatches);
    {
        LValue stringLength = m_out.phi(Int32, lengthsOtherwise);
        unsigned longest = all.isEmpty() ? 0 : all.last().string->length();
        proceedIf(m_out.belowOrEqual(stringLength, m_out.constInt32(longest)), defaultBlock);
        if (longest < 64) {
            uint64_t lengths = 0;
            for (auto& one : all)
                lengths |= 1ull << one.string->length();
            m_graph.wideIntegerConstants.add(static_cast<int64_t>(lengths));
            m_out.branch(m_out.testNonZero64(m_out.lShr(m_out.constInt64(lengths), stringLength), m_out.constInt64(1)), unsure(slowCase), unsure(defaultBlock));
        } else
            m_out.jump(slowCase);
    }

    m_out.appendTo(slowCase);
    LValue atom = vmCall(place, pointerType(), Entry::operationAOTLatin1StringEqualTo, m_instance, value);
    proceedIf(m_out.notNull(atom), defaultBlock);
    ValueFromBlock atomCharacters = m_out.anchor(m_out.loadPtr(atom, m_heaps.StringImpl_data));
    ValueFromBlock atomLength = m_out.anchor(m_out.load32(atom, m_heaps.StringImpl_length));
    m_out.jump(dispatch);

    m_out.appendTo(dispatch);
    LValue characters = m_out.phi(pointerType(), plainCharacters, atomCharacters);
    LValue length = m_out.phi(Int32, plainLength, atomLength);
    Vector<FTL::SwitchCase> cases;
    Vector<std::tuple<LBasicBlock, unsigned, unsigned>, 8> groups;
    for (unsigned first = 0; first < all.size();) {
        unsigned end = first;
        while (end < all.size() && all[end].string->length() == all[first].string->length())
            ++end;
        LBasicBlock lengthMatchCase = m_out.newBlock();
        cases.append(FTL::SwitchCase(m_out.constInt32(all[first].string->length()), lengthMatchCase, FTL::Weight()));
        groups.append({ lengthMatchCase, first, end });
        first = end;
    }
    m_out.switchInstruction(length, cases, defaultBlock, FTL::Weight());
    for (auto [lengthMatchCase, first, end] : groups) {
        m_out.appendTo(lengthMatchCase);
        for (unsigned i = first; i < end; ++i) {
            LBasicBlock next = i + 1 < end ? m_out.newBlock() : defaultBlock;
            m_out.branch(m_out.isZero64(compareWithLiteral(characters, all[i].string->span8())), unsure(all[i].target), unsure(next));
            if (i + 1 < end)
                m_out.appendTo(next);
        }
    }
}

void Lowering::findComparisonChains()
{
    struct Comparison {
        Node* value;
        Node* constant;
        BasicBlock* ifEqual;
        BasicBlock* notTakenBlock;
    };
    auto isTrackedNode = [&](Node* node) {
        if (node->kind == NodeKind::Constant)
            return node->constant && !node->constant.isCell();
        return constantStringOf(node) && isAtomIfString(node);
    };
    auto terminalComparisonOf = [&](BasicBlock* block) -> std::optional<Comparison> {
        Node* terminal = block->terminal();
        if (!terminal || terminal->kind != NodeKind::Bytecode || terminal->guard || terminal->guarded || block->endsWithGuard)
            return std::nullopt;
        if (block->successors.size() != 2 || block->successors[0] == block->successors[1])
            return std::nullopt;
        Node* left;
        Node* right;
        bool jumpsIfEqual = terminal->opcode == op_jstricteq;
        if (jumpsIfEqual) {
            auto bytecode = terminal->as<OpJstricteq>();
            left = terminal->use(bytecode.m_lhs), right = terminal->use(bytecode.m_rhs);
        } else if (terminal->opcode == op_jnstricteq) {
            auto bytecode = terminal->as<OpJnstricteq>();
            left = terminal->use(bytecode.m_lhs), right = terminal->use(bytecode.m_rhs);
        } else
            return std::nullopt;
        if (isTrackedNode(left) == isTrackedNode(right))
            return std::nullopt;
        if (isTrackedNode(left))
            std::swap(left, right);
        return Comparison { left, right, block->successors[jumpsIfEqual ? 0 : 1], block->successors[jumpsIfEqual ? 1 : 0] };
    };
    for (BasicBlock* head : m_graph.m_rpo) {
        if (m_blocksInsideChains.contains(head))
            continue;
        auto first = terminalComparisonOf(head);
        if (!first)
            continue;
        ComparisonChain chain;
        chain.value = first->value;
        chain.arms.append({ head, first->constant, first->ifEqual });
        BasicBlock* next = first->notTakenBlock;
        while (chain.arms.size() < 256) {
            if (next == head || next->predecessors.size() != 1 || !next->phis.isEmpty() || next->loweredAhead)
                break;
            if (next->isCatchEntrypoint || next->isLoopHeader || next->isPreHeader || next->isReentry)
                break;
            if (next->isGeneric != head->isGeneric || next->isRarelyExecuted != head->isRarelyExecuted || next->isInLoop != head->isInLoop || next->graph != head->graph)
                break;
            if (!std::ranges::all_of(next->nodes, [&](Node* node) { return node == next->terminal() || node->isElided; }))
                break;
            auto comparison = terminalComparisonOf(next);
            if (!comparison || comparison->value != chain.value)
                break;
            chain.arms.append({ next, comparison->constant, comparison->ifEqual });
            next = comparison->notTakenBlock;
        }
        if (chain.arms.size() < 3)
            continue;
        chain.otherwise = next;
        for (unsigned i = 1; i < chain.arms.size(); ++i)
            m_blocksInsideChains.add(chain.arms[i].block);
        m_chainsByFirstBlock.add(head, m_chains.size());
        m_chains.append(WTF::move(chain));
    }
}

void Lowering::lowerComparisonChain(BasicBlock* head, const ComparisonChain& chain)
{
    Node* place = head->terminal();
    Node* scrutinee = chain.value;
    struct Way {
        BasicBlock* from;
        BasicBlock* to;
        LBasicBlock edge;
    };
    Vector<Way, 8> ways;
    auto edgeBlockFrom = [&](BasicBlock* from, BasicBlock* to) -> LBasicBlock {
        SetForScope blockScope(m_block, from);
        if (to->phis.isEmpty())
            return entryBlockFor(to);
        LBasicBlock edge = m_out.newBlock();
        ways.append({ from, to, edge });
        return edge;
    };

    Vector<StringCase, 16> strings;
    Vector<FTL::SwitchCase> integers;
    Vector<std::pair<double, LBasicBlock>, 4> fractions;
    Vector<FTL::SwitchCase> others;
    UncheckedKeyHashSet<const StringImpl*> stringsSeen;
    UncheckedKeyHashSet<int64_t, WTF::IntHash<int64_t>, WTF::UnsignedWithZeroKeyHashTraits<int64_t>> bitsSeen;
    for (auto& arm : chain.arms) {
        if (arm.constant->kind == NodeKind::ConstantCell) {
            const StringImpl* string = asString(arm.constant->codeBlockOfConstant()->getConstant(arm.constant->reg))->tryGetValueImpl();
            if (stringsSeen.add(string).isNewEntry)
                strings.append({ string, edgeBlockFrom(arm.block, arm.target), arm.constant });
            continue;
        }
        JSValue constant = arm.constant->constant;
        if (!constant.isNumber()) {
            if (bitsSeen.add(JSValue::encode(constant)).isNewEntry)
                others.append(FTL::SwitchCase(m_out.constInt64(JSValue::encode(constant)), edgeBlockFrom(arm.block, arm.target), FTL::Weight()));
            continue;
        }
        double number = constant.asNumber();
        if (number != number)
            continue;
        if (number >= std::numeric_limits<int32_t>::min() && number <= std::numeric_limits<int32_t>::max() && number == static_cast<int32_t>(number)) {
            if (bitsSeen.add(JSValue::encode(jsNumber(static_cast<int32_t>(number)))).isNewEntry)
                integers.append(FTL::SwitchCase(m_out.constInt32(static_cast<int32_t>(number)), edgeBlockFrom(arm.block, arm.target), FTL::Weight()));
        } else if (bitsSeen.add(JSValue::encode(jsDoubleNumber(number))).isNewEntry)
            fractions.append({ number, edgeBlockFrom(arm.block, arm.target) });
    }
    LBasicBlock otherwise = edgeBlockFrom(chain.arms.last().block, chain.otherwise);

    auto forDouble = [&](LValue number) {
        LBasicBlock isWhole = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        LValue asInt = m_out.doubleToInt32(number);
        m_out.branch(m_out.doubleEqual(m_out.intToDouble(asInt), number), unsure(isWhole), unsure(isNot));
        m_out.appendTo(isWhole);
        m_out.switchInstruction(asInt, integers, otherwise, FTL::Weight());
        m_out.appendTo(isNot);
        for (auto [fraction, target] : fractions) {
            LBasicBlock next = m_out.newBlock();
            m_out.branch(m_out.doubleEqual(number, m_out.constDouble(fraction)), unsure(target), unsure(next));
            m_out.appendTo(next);
        }
        m_out.jump(otherwise);
    };

    Type type = scrutinee->type;
    bool hasStrings = !strings.isEmpty() && mayBe(type, TString);
    bool hasNumbers = (!integers.isEmpty() || !fractions.isEmpty()) && mayBe(type, TNumber);
    bool hasOthers = !others.isEmpty() && mayBe(type, TOther | TBoolean);
    if (scrutinee->rep() == Rep::Int32)
        m_out.switchInstruction(lowInt32(scrutinee), integers, otherwise, FTL::Weight());
    else if (scrutinee->rep() == Rep::Double)
        forDouble(lowDouble(scrutinee));
    else {
        LValue value = lowJSValue(scrutinee);
        bool isDone = false;
        if (hasStrings) {
            if (hasNumbers || hasOthers) {
                LBasicBlock cell = m_out.newBlock();
                LBasicBlock notCell = m_out.newBlock();
                m_out.branch(isCell(value), unsure(cell), unsure(notCell));
                m_out.appendTo(cell);
                dispatchOnString(place, scrutinee, value, strings, otherwise, true);
                m_out.appendTo(notCell);
            } else {
                dispatchOnString(place, scrutinee, value, strings, otherwise);
                isDone = true;
            }
        }
        if (!isDone && hasNumbers) {
            LBasicBlock isInt = m_out.newBlock();
            LBasicBlock isNotInt = m_out.newBlock();
            LBasicBlock isDouble = m_out.newBlock();
            LBasicBlock rest = hasOthers ? m_out.newBlock() : otherwise;
            m_out.branch(isInt32(value), unsure(isInt), unsure(isNotInt));
            m_out.appendTo(isInt);
            m_out.switchInstruction(unboxInt32(value), integers, otherwise, FTL::Weight());
            m_out.appendTo(isNotInt);
            m_out.branch(isNumber(value), unsure(isDouble), unsure(rest));
            m_out.appendTo(isDouble);
            forDouble(unboxDouble(value));
            if (hasOthers)
                m_out.appendTo(rest);
            else
                isDone = true;
        }
        if (!isDone) {
            if (hasOthers)
                m_out.switchInstruction(value, others, otherwise, FTL::Weight());
            else
                m_out.jump(otherwise);
        }
    }

    for (auto& way : ways) {
        SetForScope blockScope(m_block, way.from);
        m_out.appendTo(way.edge);
        emitUpsilons(way.from, way.to);
        m_out.jump(entryBlockFor(way.to));
    }
}

void Lowering::lowerSwitch(Node* node)
{
    UnlinkedCodeBlock* codeBlock = code().codeBlock();
    Vector<FTL::SwitchCase> cases;
    UncheckedKeyHashSet<int64_t, WTF::IntHash<int64_t>, WTF::UnsignedWithZeroKeyHashTraits<int64_t>> seen;
    auto addCase = [&](int32_t value, int32_t offset) {
        if (!offset || !seen.add(static_cast<uint32_t>(value)).isNewEntry)
            return;
        cases.append(FTL::SwitchCase(m_out.constInt32(value), blockFor(node, offset), FTL::Weight()));
    };

    if (node->opcode == op_switch_string) {
        auto bytecode = node->as<OpSwitchString>();
        const auto& table = codeBlock->unlinkedStringSwitchJumpTable(bytecode.m_tableIndex);
        Vector<std::pair<StringImpl*, int32_t>, 16> entries;
        for (auto& entry : table.m_offsetTable)
            entries.append({ entry.key.get(), entry.value.m_branchOffset });
        std::ranges::sort(entries, [](auto& a, auto& b) { return codePointCompare(StringView { *a.first }, StringView { *b.first }) < 0; });
        Vector<StringCase, 16> all;
        bool allAreLatin1 = true;
        for (auto [string, offset] : entries) {
            allAreLatin1 &= string->is8Bit();
            all.append({ string, blockFor(node, offset), nullptr });
        }
        if (allAreLatin1) {
            Node* scrutinee = node->use(bytecode.m_scrutinee);
            dispatchOnString(node, scrutinee, lowJSValue(scrutinee), all, blockFor(node, table.m_defaultOffset));
            return;
        }
        LValue offset = vmCall(node, Int32, Entry::operationAOTSwitchString, m_instance, lowJSValue(node->use(bytecode.m_scrutinee)), m_out.constInt32(bytecode.m_tableIndex), m_out.constInt32(bytecodeOwner(node)));
        for (auto [string, offset] : entries)
            addCase(offset, offset);
        m_out.switchInstruction(offset, cases, blockFor(node, table.m_defaultOffset), FTL::Weight());
        return;
    }

    bool isChar = node->opcode == op_switch_char;
    unsigned tableIndex = isChar ? node->as<OpSwitchChar>().m_tableIndex : node->as<OpSwitchImm>().m_tableIndex;
    Node* scrutinee = node->use(isChar ? node->as<OpSwitchChar>().m_scrutinee : node->as<OpSwitchImm>().m_scrutinee);
    const auto& table = codeBlock->unlinkedSwitchJumpTable(tableIndex);
    LBasicBlock defaultBlock = blockFor(node, table.m_defaultOffset);

    LValue value;
    if (isChar)
        value = vmCall(node, Int32, Entry::operationAOTSwitchChar, m_instance, lowJSValue(scrutinee));
    else if (scrutinee->rep() == Rep::Int32)
        value = lowInt32(scrutinee);
    else {
        LValue jsValue = lowJSValue(scrutinee);
        LBasicBlock notInt = m_out.newBlock();
        LBasicBlock isDouble = m_out.newBlock();
        LBasicBlock dispatch = m_out.newBlock();
        ValueFromBlock intResult = m_out.anchor(unboxInt32(jsValue));
        m_out.branch(isInt32(jsValue), usually(dispatch), rarely(notInt));
        m_out.appendTo(notInt, isDouble);
        m_out.branch(isNumber(jsValue), unsure(isDouble), unsure(defaultBlock));
        m_out.appendTo(isDouble, dispatch);
        LValue asDouble = unboxDouble(jsValue);
        LValue asInt = m_out.doubleToInt32(asDouble);
        ValueFromBlock doubleResult = m_out.anchor(asInt);
        m_out.branch(m_out.doubleEqual(m_out.intToDouble(asInt), asDouble), unsure(dispatch), unsure(defaultBlock));
        m_out.appendTo(dispatch);
        value = m_out.phi(Int32, intResult, doubleResult);
    }

    if (table.isList()) {
        for (unsigned i = 0; i < table.m_branchOffsets.size(); i += 2)
            addCase(table.m_branchOffsets[i], table.m_branchOffsets[i + 1]);
    } else {
        for (unsigned i = 0; i < table.m_branchOffsets.size(); ++i)
            addCase(table.m_min + i, table.m_branchOffsets[i]);
    }
    m_out.switchInstruction(value, cases, defaultBlock, FTL::Weight());
}

void Lowering::lowerCatch(Node* node)
{
    auto bytecode = node->as<OpCatch>();
    LValue exception = plainCall(pointerType(), Entry::operationAOTCatch, m_vm);
    LBasicBlock caught = m_out.newBlock();
    LBasicBlock notForCatching = newColdBlock();
    m_out.branch(m_out.isNull(exception), rarely(notForCatching), usually(caught));
    m_out.appendTo(notForCatching);
    callStub(Stub::HandleException, Void, { }, { });
    m_out.unreachable();
    m_out.appendTo(caught);
    setProj(node, bytecode.m_exception, exception);
    setProj(node, bytecode.m_thrownValue, m_out.load64(m_out.address(m_heaps.root, exception, Exception::valueOffset())));
}

void Lowering::emitTypeTests(Node* value, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock undecided)
{
    emitTypeTests(nullptr, value->type, jsValue, mask, passed, undecided);
}

void Lowering::emitTypeTests(std::nullptr_t, Type valueType, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock undecided)
{
    Type candidates = valueType & typeProvingMask(mask);
    auto passIf = [&](LValue condition) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition, unsure(passed), unsure(next));
        m_out.appendTo(next);
    };
    if (mayBe(candidates, TNumber))
        passIf(isNumber(jsValue));
    if (isSubtype(TOther, candidates))
        passIf(isOther(jsValue));
    else if (mayBe(candidates, TUndefined))
        passIf(m_out.equal(jsValue, m_out.constInt64(JSValue::ValueUndefined)));
    else if (mayBe(candidates, TNull))
        passIf(m_out.equal(jsValue, m_out.constInt64(JSValue::ValueNull)));
    if (mayBe(candidates, TBoolean))
        passIf(isBoolean(jsValue));
    if (mayBe(valueType & typeAcceptedByMask(mask), TCell)) {
        if (!isSubtype(valueType, TCell)) {
            LBasicBlock cellCase = m_out.newBlock();
            m_out.branch(isCell(jsValue), unsure(cellCase), rarely(undecided));
            m_out.appendTo(cellCase);
        }
        LValue type = cellType(jsValue);
        auto isType = [&](JSType jsType) { return m_out.equal(type, m_out.constInt32(jsType)); };
        if (isSubtype(TAnyObject & valueType, candidates) && mayBe(candidates, TAnyObject))
            passIf(m_out.aboveOrEqual(type, m_out.constInt32(ObjectType)));
        else {
            if (mayBe(candidates, TFinalObject))
                passIf(isType(FinalObjectType));
            if (Type others = candidates & TObject & ~(TFinalObject | TOtherObject); others && numberOfBitsIn(others) <= 2) {
                for (auto& kind : objectKinds) {
                    if (mayBe(others, kind.type))
                        passIf(isType(kind.jsType));
                }
            }
            if (isSubtype(TTypedArray, candidates))
                passIf(m_out.below(m_out.sub(type, m_out.constInt32(FirstTypedArrayType)), m_out.constInt32(NumberOfTypedArrayTypesExcludingDataView)));
            else {
                for (unsigned i = 0; i < NumberOfTypedArrayTypesExcludingDataView; ++i) {
                    JSType typedArrayType = static_cast<JSType>(FirstTypedArrayType + i);
                    if (mayBe(candidates, typeForTypedArray(typedArrayType)))
                        passIf(isType(typedArrayType));
                }
            }
            if (mayBe(candidates, TArray))
                passIf(m_out.bitOr(isType(ArrayType), isType(DerivedArrayType)));
            if (mayBe(candidates, TFunction))
                passIf(m_out.bitOr(isType(JSFunctionType), isType(InternalFunctionType)));
        }
        if (mayBe(candidates, TString))
            passIf(isType(StringType));
        if (mayBe(candidates, TSymbol))
            passIf(isType(SymbolType));
        if (mayBe(candidates, TBigInt))
            passIf(isType(HeapBigIntType));
    }
    m_out.jump(undecided);
}

bool Lowering::tryLowerMisc(Node* node)
{
    auto typeTest = [&](Node* value, Type type, auto&& test) {
        if (isSubtype(value->type, type))
            setBoolean(node, m_out.booleanTrue);
        else if (!mayBe(value->type, type))
            setBoolean(node, m_out.booleanFalse);
        else
            setBoolean(node, test(lowJSValue(value)));
    };
    auto cellTest = [&](LValue value, auto&& test) -> LValue {
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock notCellResult = m_out.anchor(m_out.booleanFalse);
        m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));
        m_out.appendTo(cellCase, continuation);
        ValueFromBlock cellResult = m_out.anchor(test(value));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, notCellResult, cellResult);
    };

    switch (node->opcode) {
    case op_loop_hint:
        return true;
    case op_check_traps: {
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.testNonZero32(trapBits(), m_out.constInt32(VMTraps::AsyncEvents)), rarely(slowPath), usually(continuation));
        m_out.appendTo(slowPath, continuation);
        coldCall(node, Entry::operationAOTHandleTraps, nullptr, nullptr, ColdCall::ChangesNothing);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return true;
    }
    case op_catch:
        lowerCatch(node);
        return true;
    case op_get_scope:
        if (Node* scope = code().closureScope; scope && !node->useCount)
            return true;
        if (Node* scope = code().closureScope) {
            setJSValue(node, lowJSValue(scope));
            return true;
        }
        if (Node* function = code().currentClosureFunction()) {
            if (node->useCount)
                setJSValue(node, m_out.loadPtr(lowCell(function), m_heaps.JSCallee_scope));
            return true;
        }
        if (code().scopeIsModuleEnvironment()) {
            if (node->useCount)
                setJSValue(node, environmentAt(code().moduleEnvironmentDepth()));
            return true;
        }
        setJSValue(node, m_out.loadPtr(callee(), m_heaps.JSCallee_scope));
        return true;
    case op_get_parent_scope:
        if (node->scopeToStartFrom) {
            setJSValue(node, ancestorScope(node->scopeToStartFrom, node->remainingHops));
            return true;
        }
        setJSValue(node, m_out.loadPtr(lowCell(node->use(node->as<OpGetParentScope>().m_scope)), m_heaps.JSScope_next));
        return true;
    case op_argument_count:
        setInt32(node, numberOfArgumentsPassed());
        return true;
    case op_get_argument: {
        unsigned index = node->as<OpGetArgument>().m_index - 1;
        if (m_graph.convention().signature == Signature::List)
            setJSValue(node, argumentPassedOrUndefined(index));
        else
            setJSValue(node, lowParameterOnEntryAsJSValue(index));
        return true;
    }
    case op_check_tdz: {
        Node* value = node->use(node->as<OpCheckTdz>().m_targetVirtualRegister);
        if (!mayBe(value->type, TEmpty))
            return true;
        LBasicBlock slowPath = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.isZero64(lowJSValue(value)), rarely(slowPath), usually(continuation));
        m_out.appendTo(slowPath, continuation);
        throwTDZError(node);
        m_out.appendTo(continuation);
        return true;
    }
    case op_type_tag: {
        Node* value = node->uses[0].node;
        if (node->narrowedTo || value->hasLayoutInRange(node->firstLayout, node->lastLayout) || (TypeTable::hasTypedFields() && TypeTable::shared()->isUsable(node->firstLayout) && TypeTable::shared()->usesFieldIDs(node->firstLayout))) {
            m_aliasTarget = value;
            setResult(node, lowRaw(value), value->rep());
            m_aliasTarget = nullptr;
            return true;
        }
        LValue jsValue = lowJSValue(value);
        if (!node->isTrusted) {
            LValue view = cachedCoercionFor(value, node->firstLayout);
            m_coercions.set(node, view ? view : coerceToTypedLayout(node, value, jsValue, node->firstLayout));
            m_aliasTarget = value;
            setResult(node, lowRaw(value), value->rep());
            m_aliasTarget = nullptr;
            return true;
        }
        checkTypedLayout(node, value, jsValue, node->firstLayout);
        setJSValue(node, jsValue);
        return true;
    }
    case op_check_type: {
        auto bytecode = node->as<OpCheckType>();
        Node* value = node->use(bytecode.m_value);
        unsigned mask = bytecode.m_mask;
        if (value->isKnownToPass(mask)) {
            m_aliasTarget = value;
            setResult(node, lowRaw(value), value->rep());
            m_aliasTarget = nullptr;
            return true;
        }

        LValue jsValue = lowJSValue(value);
        LBasicBlock slowPath = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        emitTypeTests(value, jsValue, mask, continuation, slowPath);

        m_out.appendTo(slowPath, continuation);
        coldCall(node, Entry::operationAOTCheckType, jsValue, m_out.constInt32(mask), ColdCall::ChangesNothing);
        Type admitted = value->type & typeAcceptedByMask(mask);
        Type candidates = value->type & typeProvingMask(mask);
        bool allObjectsPass = isSubtype(TAnyObject & value->type, candidates) && mayBe(candidates, TAnyObject);
        if (isSubtype(admitted, allObjectsPass ? TPrimitive | TAnyObject : TPrimitive) && isSubtype(admitted, candidates))
            m_out.unreachable();
        else
            m_out.jump(continuation);

        m_out.appendTo(continuation);
        setJSValue(node, jsValue);
        return true;
    }
    case op_not: {
        setBoolean(node, m_out.logicalNot(toBoolean(node->use(node->as<OpNot>().m_operand))));
        return true;
    }
    case op_eq_null:
        setBoolean(node, equalsNull(node->use(node->as<OpEqNull>().m_operand), true));
        return true;
    case op_neq_null:
        setBoolean(node, m_out.logicalNot(equalsNull(node->use(node->as<OpNeqNull>().m_operand), true)));
        return true;
    case op_typeof_is_undefined:
        setBoolean(node, equalsNull(node->use(node->as<OpTypeofIsUndefined>().m_operand), false));
        return true;
    case op_is_empty:
        typeTest(node->use(node->as<OpIsEmpty>().m_operand), TEmpty, [&](LValue v) { return m_out.isZero64(v); });
        return true;
    case op_is_undefined_or_null:
        typeTest(node->use(node->as<OpIsUndefinedOrNull>().m_operand), TOther, [&](LValue v) { return isOther(v); });
        return true;
    case op_is_boolean:
        typeTest(node->use(node->as<OpIsBoolean>().m_operand), TBoolean, [&](LValue v) { return isBoolean(v); });
        return true;
    case op_is_number:
        typeTest(node->use(node->as<OpIsNumber>().m_operand), TNumber, [&](LValue v) { return isNumber(v); });
        return true;
    case op_is_object:
        typeTest(node->use(node->as<OpIsObject>().m_operand), TAnyObject, [&](LValue v) {
            return cellTest(v, [&](LValue cell) { return isObjectCell(cell); });
        });
        return true;
    case op_is_big_int:
        typeTest(node->use(node->as<OpIsBigInt>().m_operand), TBigInt, [&](LValue v) {
            return cellTest(v, [&](LValue cell) { return isCellOfType(cell, HeapBigIntType); });
        });
        return true;
    case op_is_cell_with_type: {
        auto bytecode = node->as<OpIsCellWithType>();
        setBoolean(node, cellTest(lowJSValue(node->use(bytecode.m_operand)), [&](LValue cell) { return isCellOfType(cell, bytecode.m_type); }));
        return true;
    }
    case op_get_internal_field: {
        auto bytecode = node->as<OpGetInternalField>();
        setJSValue(node, m_out.load64(lowCell(node->use(bytecode.m_base)), m_heaps.JSInternalFieldObjectImpl_internalFields[bytecode.m_index]));
        return true;
    }
    case op_put_internal_field: {
        auto bytecode = node->as<OpPutInternalField>();
        LValue base = lowCell(node->use(bytecode.m_base));
        m_out.store64(lowJSValue(node->use(bytecode.m_value)), base, m_heaps.JSInternalFieldObjectImpl_internalFields[bytecode.m_index]);
        if (mayBe(node->use(bytecode.m_value)->type, TCell))
            storeBarrier(base);
        return true;
    }
    case op_profile_control_flow:
    case op_profile_type:
    case op_debug:
    case op_log_shadow_chicken_prologue:
    case op_log_shadow_chicken_tail:
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

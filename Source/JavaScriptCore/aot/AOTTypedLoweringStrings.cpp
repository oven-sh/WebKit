/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTTypedCompactTable.h"
#include "AOTTypedThreads.h"
#include "B3BasicBlockInlines.h"
#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMap.h"
#include "JSSet.h"
#include "TypeInfoBlob.h"
#include <unicode/utf16.h>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

template<> struct OperationParameterType<const Latin1Character*> { using Type = Pointer; };

Lowering::StringReceiver Lowering::stringReceiver(CellPointer string)
{
    return { string, safeIfReaches(string, sizeof(JSString)), { }, { } };
}

Lowering::StringReceiver Lowering::stringReceiver(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep == Rep::Cell) {
        StringReceiver receiver = stringReceiver(operand.as<CellPointer>());
        receiver.id = id;
        return receiver;
    }
    Boxed value = operand.as<Boxed>();
    auto& type = typeOf(id);
    StringReceiver receiver = stringReceiver(asCell(value));
    receiver.id = id;
    if (type.mayBeUndefinedOrNull()) {
        receiver.flags = 0;
        receiver.isString = isCell(value, type);
        receiver.isCellOrNullish = !(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull));
        receiver.value = value;
    }
    return receiver;
}

void Lowering::rejectNullish(const StringReceiver& receiver, StringID name)
{
    if (!receiver.isString)
        return;
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(receiver.isString.value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(receiver.value, name, receiver.isCellOrNullish ? receiver.string : CellPointer());
    m_out.appendTo(continuation);
}

bool Lowering::isCapturedStringVariableRead(ValueID id) const
{
    auto& read = m_function.at(id);
    if (read.opcode != Opcode::GetField || !holdsCells(read.rep) || isObjectLayout(m_program.at(read.layout()).kind))
        return false;
    auto& field = m_program.at(read.layout()).fields[read.field()];
    return field.stored && m_program.at(field.stored).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull) && m_program.at(field.stored).isSubsetOf(m_program.at(read.type), m_program);
}

bool Lowering::mayStoreToCapturedVariable(const Block& block, size_t index, const Value& read) const
{
    auto& value = m_function.at(block.values[index]);
    if (value.opcode == Opcode::PutField)
        return m_program.at(value.layout()).fields[value.field()].slot == m_program.at(read.layout()).fields[read.field()].slot;
    Writes writes = value.opcode == Opcode::CallDirect ? m_program.at(value.function()).writes : writesOf(m_program, m_function, block, index);
    if (writes < Writes::Fields)
        return false;
    bool storesToSingleLocation = value.opcode == Opcode::PutById || value.opcode == Opcode::PutByVal || value.opcode == Opcode::PutModuleVariable;
    return !storesToSingleLocation || writes > Writes::Fields;
}

bool Lowering::findStoresToCapturedVariableInLoop(const Loop& loop, const Value& read, Vector<ValueID, 4>& stores) const
{
    for (size_t b : loop.blocks) {
        auto& block = m_function.blocks[b];
        if (block.terminator.kind == TerminatorKind::Invoke || (block.terminator.kind == TerminatorKind::Throw && !block.terminator.targets.isEmpty()))
            return false;
        for (size_t index = 0; index < block.values.size(); ++index) {
            Opcode opcode = m_function.at(block.values[index]).opcode;
            if (opcode == Opcode::Await || opcode == Opcode::Yield || opcode == Opcode::GetResumeMode)
                return false;
            if (!mayStoreToCapturedVariable(block, index, read))
                continue;
            if (opcode == Opcode::PutField)
                return false;
            stores.append(block.values[index]);
        }
    }
    if (stores.isEmpty())
        return true;
    auto isSameRead = [&](ValueID id) {
        auto& other = m_function.at(id);
        return other.opcode == Opcode::GetField && other.child(0) == read.child(0) && other.layout() == read.layout() && other.field() == read.field();
    };
    BitVector reachesRead(m_function.blocks.size());
    for (size_t b : loop.blocks) {
        if (m_function.blocks[b].values.containsIf(isSameRead))
            reachesRead.set(b);
    }
    auto successorReachesRead = [&](const Block& block) {
        for (auto& target : successorsOf(block.terminator)) {
            if (loop.blocks.get(target.block.index()) && reachesRead.get(target.block.index()))
                return true;
        }
        return false;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t b : loop.blocks) {
            if (!reachesRead.get(b) && successorReachesRead(m_function.blocks[b])) {
                reachesRead.set(b);
                changed = true;
            }
        }
    }
    stores.removeAllMatching([&](ValueID store) {
        for (size_t b : loop.blocks) {
            auto& block = m_function.blocks[b];
            size_t index = block.values.find(store);
            if (index == notFound)
                continue;
            return !successorReachesRead(block) && std::ranges::none_of(block.values.span().subspan(index + 1), isSameRead);
        }
        return false;
    });
    return true;
}

void Lowering::findStringsReadInLoops()
{
    if (m_loops.isEmpty())
        return;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        if (m_innermostLoop[b] == noLoop)
            continue;
        auto& blockValues = m_function.blocks[b].values;
        for (size_t indexInBlock = 0; indexInBlock < blockValues.size(); ++indexInBlock) {
            ValueID id = blockValues[indexInBlock];
            auto& value = m_function.at(id);
            bool readsCharacters = true;
            if (value.opcode == Opcode::CallBuiltin) {
                auto& implementation = m_builtins.at(value.builtin()).implementation;
                if (implementation.kind != BuiltinImplementation::Inline)
                    continue;
                switch (implementation.inlined) {
                case InlineBuiltin::StringLength:
                    readsCharacters = false;
                    break;
                case InlineBuiltin::StringCharCodeAt:
                case InlineBuiltin::StringCodePointAt:
                case InlineBuiltin::StringCharAt:
                case InlineBuiltin::StringAt:
                    break;
                default:
                    continue;
                }
            } else if (value.opcode != Opcode::GetByVal || keyedAccessOf(m_program, m_function, value) != KeyedAccess::String)
                continue;
            ValueID string = stringDefinedBy(value.child(0));
            if (isResidentInStringBuilder(string))
                continue;
            auto& type = m_program.at(m_function.at(string).type);
            if (!(type.primitives & Primitive::String) || !type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
                continue;
            bool isCapturedVariable = false;
            if (isCapturedStringVariableRead(string)) {
                size_t readIndex = blockValues.find(string);
                isCapturedVariable = readIndex != notFound && readIndex < indexInBlock;
                for (size_t between = readIndex + 1; isCapturedVariable && between < indexInBlock; ++between)
                    isCapturedVariable = !mayStoreToCapturedVariable(m_function.blocks[b], between, m_function.at(string));
            }
            for (unsigned i = 0; i < m_loops.size(); ++i) {
                auto& loop = m_loops[i];
                if (!loop.blocks.get(b) || !loop.header.index())
                    continue;
                bool isDefinedOutside = isLoopInvariant(loop, string);
                if (!isDefinedOutside && (!isCapturedVariable || !isLoopInvariant(loop, m_function.at(string).child(0))))
                    continue;
                if (m_stringsOpenedBeforeLoops.isEmpty())
                    m_stringsOpenedBeforeLoops.grow(m_loops.size());
                auto& opened = m_stringsOpenedBeforeLoops[i];
                size_t index = opened.findIf([&](auto& one) {
                    if (isDefinedOutside || !one.capturedVariableCell)
                        return one.string == string;
                    auto& read = m_function.at(string);
                    auto& other = m_function.at(one.string);
                    return other.child(0) == read.child(0) && other.layout() == read.layout() && other.field() == read.field();
                });
                if (index == notFound) {
                    Vector<ValueID, 4> stores;
                    if (!isDefinedOutside && !findStoresToCapturedVariableInLoop(loop, m_function.at(string), stores))
                        continue;
                    index = opened.size();
                    opened.append(OpenedString { string, m_proc.addVariable(Int64), m_proc.addVariable(Int64), m_proc.addVariable(Int32) });
                    if (!isDefinedOutside)
                        opened[index].capturedVariableCell = m_proc.addVariable(Int64);
                    for (ValueID store : stores)
                        m_stringsOpenedAgainAfter.add(store.index(), Vector<std::pair<unsigned, unsigned>, 1> { }).iterator->value.append({ i, static_cast<unsigned>(index) });
                }
                if (!isDefinedOutside)
                    m_openedCapturedVariableReads.set(id.index());
                opened[index].isOpened |= readsCharacters;
                break;
            }
        }
    }
}

void Lowering::openStringsBeforeLoopsEnteredBy(const Terminator& terminator)
{
    if (m_stringsOpenedBeforeLoops.isEmpty())
        return;
    for (auto& target : successorsOf(terminator)) {
        for (unsigned i = 0; i < m_loops.size(); ++i) {
            if (m_loops[i].header != target.block || m_loops[i].blocks.get(m_currentBlockID.index()))
                continue;
            for (auto& opened : m_stringsOpenedBeforeLoops[i])
                open(opened);
        }
    }
}

void Lowering::openStringsAgainAfter(ValueID id)
{
    if (m_stringsOpenedAgainAfter.isEmpty())
        return;
    auto found = m_stringsOpenedAgainAfter.find(id.index());
    if (found == m_stringsOpenedAgainAfter.end())
        return;
    for (auto [loop, index] : found->value)
        open(m_stringsOpenedBeforeLoops[loop][index]);
}

void Lowering::open(const OpenedString& opened)
{
    auto set = [&](B3::Variable* variable, LValue value) {
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), variable, value);
    };
    StringReceiver receiver;
    if (opened.capturedVariableCell) {
        Operand held = lowerGetEnvironmentField(opened.string, m_function.at(opened.string));
        auto& type = typeOf(opened.string);
        set(opened.capturedVariableCell, held.value);
        receiver = stringReceiver(held.rep == Rep::Cell ? held.as<CellPointer>() : asCell(held.as<Boxed>()));
        if (held.rep != Rep::Cell && type.mayBeUndefinedOrNull()) {
            receiver.flags = 0;
            receiver.isString = isCell(held.as<Boxed>(), type);
        }
    } else
        receiver = stringReceiver(opened.string);
    LBasicBlock continuation = m_out.newBlock();
    if (receiver.isString) {
        set(opened.length, m_out.int64Zero);
        if (opened.isOpened) {
            set(opened.data, m_out.int64Zero);
            set(opened.flags, m_out.int32Zero);
        }
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), usually(isString), rarely(continuation));
        m_out.appendTo(isString);
    }
    Pointer fiber = fiberOf(receiver);
    if (!opened.isOpened) {
        set(opened.length, zeroExtended(either<Word32>(hasNoBits(fiber, JSString::isRopeInPointer), [&] {
            return loadField<Word32>(fiber, m_heaps.StringImpl_length);
        }, [&] {
            return loadField<Word32>(receiver.string, m_heaps.JSRopeString_length);
        })).value());
        ensureStillAliveHere(receiver.string);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    Pointer impl = eitherPointer(hasNoBits(fiber, JSString::isRopeInPointer), [&] {
        return fiber;
    }, [&] {
        return AOT_TYPED_OPERATION(Flatten, receiver.string);
    });
    set(opened.data, loadField<Pointer>(impl, m_heaps.StringImpl_data).value());
    set(opened.length, zeroExtended(loadField<Word32>(impl, m_heaps.StringImpl_length)).value());
    set(opened.flags, loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
    ensureStillAliveHere(receiver.string);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

const Lowering::OpenedString* Lowering::openedString(ValueID id) const
{
    if (m_stringsOpenedBeforeLoops.isEmpty() || !id || m_innermostLoop[m_currentBlockID.index()] == noLoop)
        return nullptr;
    id = stringDefinedBy(id);
    for (unsigned i = 0; i < m_loops.size(); ++i) {
        if (!m_loops[i].blocks.get(m_currentBlockID.index()))
            continue;
        for (auto& opened : m_stringsOpenedBeforeLoops[i]) {
            if (opened.string == id && !opened.capturedVariableCell)
                return &opened;
            ValueID user = currentValue();
            if (!opened.capturedVariableCell || !user || !m_openedCapturedVariableReads.get(user.index()) || stringDefinedBy(m_function.at(user).child(0)) != id)
                continue;
            auto& read = m_function.at(id);
            auto& other = m_function.at(opened.string);
            if (read.opcode == Opcode::GetField && other.child(0) == read.child(0) && other.layout() == read.layout() && other.field() == read.field())
                return &opened;
        }
    }
    return nullptr;
}

std::optional<Lowering::Characters> Lowering::openedStringCharacters(const StringReceiver& receiver)
{
    auto* opened = openedString(receiver.id);
    if (!opened || !opened->isOpened)
        return std::nullopt;
    auto get = [&](B3::Variable* variable) {
        return m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), variable);
    };
    return Characters { Pointer(get(opened->data)), Word32(get(opened->flags)), Integer(get(opened->length)), opened->capturedVariableCell ? CellPointer(get(opened->capturedVariableCell)) : receiver.string };
}

std::optional<unsigned> fieldReadAtConstantIndex(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::GetByVal || value.child(0) == value.child(1))
        return std::nullopt;
    ValueID constant = program.constantOf(function, value.child(1));
    if (!constant || function.at(constant).opcode != Opcode::Integer)
        return std::nullopt;
    int64_t index = function.at(constant).integer();
    if (index < 0 || index >= SplitOnlyTakenApart::maxFields)
        return std::nullopt;
    return static_cast<unsigned>(index);
}

SplitsOnlyTakenApart findSplitsOnlyTakenApart(const Program& program, const Function& function)
{
    SplitsOnlyTakenApart splits;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations())
        return splits;
    auto& builtins = Builtins::singleton();
    auto inlinedAs = [&](const Value& value) -> std::optional<InlineBuiltin> {
        if (value.opcode != Opcode::CallBuiltin || !value.child(0))
            return std::nullopt;
        auto& implementation = builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline)
            return std::nullopt;
        return implementation.inlined;
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (inlinedAs(value) != InlineBuiltin::StringSplit || value.arguments.size() != 1 || value.isInTailPosition || program.typeOf(function, value.child(0)).hasObjects())
                continue;
            ValueID constant = program.constantOf(function, value.arguments[0]);
            if (!constant || function.at(constant).opcode != Opcode::String)
                continue;
            auto text = program.string(function.at(constant).string());
            if (text.size() == 1 && isASCII(text[0]))
                splits.add(id.index(), SplitOnlyTakenApart { 0, false, text[0] });
        }
    }
    if (splits.isEmpty())
        return splits;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& user = function.at(id);
            user.forEachChild([&](ValueID child) {
                auto found = splits.find(child.index());
                if (found == splits.end())
                    return;
                if (auto field = fieldReadAtConstantIndex(program, function, user); field && child == user.child(0))
                    found->value.numberOfPositions = std::max(found->value.numberOfPositions, *field + 1);
                else if (inlinedAs(user) == InlineBuiltin::ArrayLength && user.arguments.isEmpty())
                    found->value.countsAllSeparators = true;
                else
                    splits.remove(found);
            });
        }
        if (block.terminator.value)
            splits.remove(block.terminator.value.index());
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                splits.remove(argument.index());
        }
    }
    return splits;
}

std::optional<MatchProperty> matchPropertyRead(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::GetById || value.methodGetByIdBuiltin())
        return std::nullopt;
    auto name = program.string(value.string());
    if (equalSpans(name, asBytes("index"_s.span8())))
        return MatchProperty::Index;
    if (equalSpans(name, asBytes("input"_s.span8())))
        return MatchProperty::Input;
    return std::nullopt;
}

MatchesOnlyTakenApart findMatchesOnlyTakenApart(const Program& program, const Function& function)
{
    MatchesOnlyTakenApart matches;
    return matches;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations())
        return matches;
    auto& builtins = Builtins::singleton();
    auto inlinedAs = [&](const Value& value) -> std::optional<InlineBuiltin> {
        if (value.opcode != Opcode::CallBuiltin || !value.child(0))
            return std::nullopt;
        auto& implementation = builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline)
            return std::nullopt;
        return implementation.inlined;
    };
    auto isLiteralOnlyMatchedOnce = [&](ValueID id, ValueID call) {
        auto& literal = function.at(id);
        if (literal.opcode != Opcode::CallBuiltin || literal.arguments.size() != 2)
            return false;
        auto& implementation = builtins.at(literal.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::RegExpNew)
            return false;
        auto& flags = function.at(literal.arguments[1]);
        if (function.at(literal.arguments[0]).opcode != Opcode::String || flags.opcode != Opcode::String || std::ranges::contains(program.string(flags.string()), 'g'))
            return false;
        bool isUsedElsewhere = false;
        for (auto& block : function.blocks) {
            for (ValueID user : block.values) {
                function.at(user).forEachChild([&](ValueID child) {
                    isUsedElsewhere |= child == id && user != call;
                });
            }
            isUsedElsewhere |= block.terminator.value == id;
            for (auto& target : block.terminator.targets)
                isUsedElsewhere |= target.arguments.contains(id);
        }
        return !isUsedElsewhere;
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            auto which = inlinedAs(value);
            if (value.arguments.size() != 1 || value.isInTailPosition)
                continue;
            if (which == InlineBuiltin::StringMatch) {
                if (!program.typeOf(function, value.child(0)).hasObjects() && value.child(0) != value.arguments[0] && isLiteralOnlyMatchedOnce(value.arguments[0], id))
                    matches.add(id.index(), 1);
                continue;
            }
            auto& subject = program.typeOf(function, value.arguments[0]);
            if (which != InlineBuiltin::RegExpExec || !(subject.primitives & Primitive::String) || !subject.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
                continue;
            if (hasGenericFallback(program, function, value) && !value.receiverHasOriginalStructure)
                continue;
            matches.add(id.index(), 1);
        }
    }
    if (matches.isEmpty())
        return matches;
    auto isNil = [&](ValueID id) {
        Opcode opcode = function.at(id).opcode;
        return opcode == Opcode::Null || opcode == Opcode::Undefined;
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& user = function.at(id);
            user.forEachChild([&](ValueID child) {
                auto found = matches.find(child.index());
                if (found == matches.end())
                    return;
                if (auto field = fieldReadAtConstantIndex(program, function, user); field && child == user.child(0)) {
                    found->value = std::max(found->value, *field + 1);
                    return;
                }
                if (user.opcode == Opcode::ToBoolean || user.opcode == Opcode::IsUndefinedOrNull || matchPropertyRead(program, user))
                    return;
                if ((user.opcode == Opcode::StrictEq || user.opcode == Opcode::Eq) && isNil(user.child(user.child(0) == child)))
                    return;
                if (inlinedAs(user) == InlineBuiltin::ArrayLength && user.arguments.isEmpty())
                    return;
                matches.remove(found);
            });
        }
        if (block.terminator.value)
            matches.remove(block.terminator.value.index());
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                matches.remove(argument.index());
        }
    }
    return matches;
}

bool Lowering::fieldMayBeAbsent(ValueID id) const
{
    auto& value = m_function.at(id);
    return value.opcode == Opcode::GetByVal && *fieldReadAtConstantIndex(m_program, m_function, value) && m_program.at(value.type).primitives & Primitive::Undefined;
}

void Lowering::findSplitsOnlyTakenApart()
{
    m_splitsOnlyTakenApart = Typed::findSplitsOnlyTakenApart(m_program, m_function);
    for (uint32_t index : m_splitsOnlyTakenApart.keys())
        m_isLazilyFetched.set(index);
    m_matchesOnlyTakenApart = findMatchesOnlyTakenApart(m_program, m_function);
}

std::optional<Operand> Lowering::lowerPropertyOfMatch(ValueID result, const Value& value)
{
    if (m_matchesInFrame.isEmpty())
        return std::nullopt;
    auto found = m_matchesInFrame.find(value.child(0).index());
    if (found == m_matchesInFrame.end())
        return std::nullopt;
    nonNullish(value.child(0), value.string());
    if (*matchPropertyRead(m_program, value) == MatchProperty::Index)
        return ensure(found->value->starts[0], Type::integer(Range::length()), typeOf(result));
    return ensure(found->value->subject.string, m_program.at(m_program.types.string), typeOf(result));
}

std::optional<Operand> Lowering::lowerSplitOnlyTakenApart(ValueID result, const Value& value, CellPointer string)
{
    auto taken = m_splitsOnlyTakenApart.find(result.index());
    if (taken == m_splitsOnlyTakenApart.end() || !isOnMainPath())
        return std::nullopt;
    unsigned numberOfPositions = taken->value.numberOfPositions;
    uint32_t wanted = numberOfPositions | (taken->value.countsAllSeparators ? 1u << 31 : 0);
    SeparatedString separated;
    separated.subject = stringReceiver(string);
    separated.subject.id = value.child(0);
    separated.characters = charactersOf(separated.subject);
    m_context.emitsCalls = true;
    LValue positions = m_out.lockedStackSlot(std::max<size_t>(WTF::roundUpToMultipleOf<sizeof(uint64_t)>(numberOfPositions * sizeof(uint32_t)), sizeof(uint64_t)));
    Word32 separator = word32(taken->value.separator);
    Integer data(separated.characters.data.value());
    Word32 length = low32(separated.characters.length);
    separated.numberOfSeparators = zeroExtended(either<Word32>(hasBits(separated.characters.flags, StringImpl::flagIs8Bit()), [&] {
        return AOT_TYPED_OPERATION(FindSeparators8, data, length, separator, Integer(positions), word32(wanted));
    }, [&] {
        return AOT_TYPED_OPERATION(FindSeparators16, data, length, separator, Integer(positions), word32(wanted));
    }));
    for (unsigned i = 0; i < numberOfPositions; ++i)
        separated.positions.append(zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, positions, i * sizeof(uint32_t))))));
    ensureStillAliveHere(separated.characters.owner);
    m_separatedStrings.set(result.index(), makeUniqueWithoutFastMallocCheck<SeparatedString>(WTF::move(separated)));
    return Operand(value.rep, nullptr);
}

std::optional<Operand> Lowering::lowerFieldOfSeparatedString(ValueID result, const Value& value)
{
    if (m_separatedStrings.isEmpty() && m_matchesInFrame.isEmpty())
        return std::nullopt;
    unsigned field = fieldReadAtConstantIndex(m_program, m_function, value).value_or(0);
    if (auto found = m_matchesInFrame.find(value.child(0).index()); found != m_matchesInFrame.end()) {
        const MatchInFrame& match = *found->value;
        nonNullish(value.child(0), { });
        Word32 isPresent = field ? logicalAnd(lessThanOrEqual(integer(field), match.numberOfGroups), greaterThanOrEqual(match.starts[field], integer(0))) : Word32();
        return partOfSubject(result, value, field, match.subject, stringLength(match.subject), match.starts[field], match.ends[field], isPresent, std::nullopt);
    }
    auto found = m_separatedStrings.find(value.child(0).index());
    if (found == m_separatedStrings.end())
        return std::nullopt;
    const SeparatedString& separated = *found->value;
    Integer length = separated.characters.length;
    Integer from = field ? add(separated.positions[field - 1], integer(1)) : integer(0);
    Integer to = select(lessThan(integer(field), separated.numberOfSeparators), separated.positions[field], length);
    Word32 isPresent = field ? lessThanOrEqual(integer(field), separated.numberOfSeparators) : Word32();
    return partOfSubject(result, value, field, separated.subject, length, from, to, isPresent, separated.characters);
}

Operand Lowering::partOfSubject(ValueID result, const Value& value, unsigned field, const StringReceiver& subject, Integer length, Integer from, Integer to, Word32 isPresent, std::optional<Characters> characters)
{
    auto& type = typeOf(result);
    auto& stringType = m_program.at(m_program.types.string);
    bool mayBeAbsent = fieldMayBeAbsent(result);
    if (field && !mayBeAbsent) {
        LBasicBlock isAbsent = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isPresent.value(), usually(continuation), rarely(isAbsent));
        m_out.appendTo(isAbsent);
        ensure(undefined(), type);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
    if (isOnMainPath()) {
        (mayBeAbsent ? m_optionalSubstrings : m_substrings).set(result.index(), Substring { subject, length, from, to, mayBeAbsent ? isPresent : Word32(), characters });
        if (m_substringsOnlyInspected.get(result.index()))
            return Operand(value.rep, nullptr);
    }
    auto make = [&] {
        uint64_t key = static_cast<uint64_t>(value.child(0).index()) << 8 | field;
        if (mayBeAbsent || !isOnMainPath())
            return makeSubstring(subject, length, from, to);
        if (auto made = m_fieldsMadeInBlock.find(key); made != m_fieldsMadeInBlock.end() && made->value.first == m_currentBlockID.index())
            return CellPointer(made->value.second);
        CellPointer cell = makeSubstring(subject, length, from, to);
        m_fieldsMadeInBlock.set(key, std::pair { m_currentBlockID.index(), cell.value() });
        return cell;
    };
    if (!mayBeAbsent)
        return ensure(make(), stringType, type);
    return ensure(either<Boxed>(isPresent, [&] {
        return box(make());
    }, [&] {
        return undefined();
    }), type);
}

std::optional<Operand> Lowering::lowerLengthOfSeparatedString(ValueID result, const Value& value)
{
    if ((m_separatedStrings.isEmpty() && m_matchesInFrame.isEmpty()) || !value.child(0))
        return std::nullopt;
    if (auto match = m_matchesInFrame.find(value.child(0).index()); match != m_matchesInFrame.end()) {
        nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        return ensure(add(match->value->numberOfGroups, integer(1)), Type::integer(Range::length()), typeOf(result));
    }
    auto found = m_separatedStrings.find(value.child(0).index());
    if (found == m_separatedStrings.end())
        return std::nullopt;
    return ensure(add(found->value->numberOfSeparators, integer(1)), Type::integer(Range::length()), typeOf(result));
}

void Lowering::findSubstringsOnlyInspected()
{
    auto inlinedAs = [&](const Value& value) -> std::optional<InlineBuiltin> {
        if (value.opcode != Opcode::CallBuiltin)
            return std::nullopt;
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline)
            return std::nullopt;
        return implementation.inlined;
    };
    auto isSlice = [](std::optional<InlineBuiltin> which) {
        return which && (*which == InlineBuiltin::StringSlice || *which == InlineBuiltin::StringSubstring || *which == InlineBuiltin::StringSubstr);
    };
    BitVector isPart;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            bool isField = value.opcode == Opcode::GetByVal && (m_splitsOnlyTakenApart.contains(value.child(0).index()) || m_matchesOnlyTakenApart.contains(value.child(0).index()));
            if ((isField || (value.child(0) && isSlice(inlinedAs(value)))) && holdsCells(value.rep))
                isPart.set(id.index());
        }
    }
    if (isPart.isEmpty())
        return;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& user = m_function.at(id);
            auto which = inlinedAs(user);
            bool inspectsReceiver = which && user.child(0) && (*which == InlineBuiltin::StringLength || *which == InlineBuiltin::StringCharCodeAt || *which == InlineBuiltin::StringCodePointAt || *which == InlineBuiltin::StringCharAt || *which == InlineBuiltin::StringAt || isSlice(which));
            bool parsesFirstArgument = which == InlineBuiltin::ToNumber ? user.arguments.size() == 1 : which == InlineBuiltin::ParseInt && (user.arguments.size() == 1 || (user.arguments.size() == 2 && typeOf(user.arguments[1]).isInteger() && typeOf(user.arguments[1]).integers.isSubsetOf(Range::int32())));
            user.forEachChild([&](ValueID child) {
                if (!isPart.get(child.index()))
                    return;
                bool mayBeAbsent = fieldMayBeAbsent(child);
                if (parsesFirstArgument && !user.child(0) && child == user.arguments[0] && (!mayBeAbsent || which == InlineBuiltin::ToNumber) && (user.arguments.size() == 1 || user.arguments[1] != child))
                    return;
                if (!mayBeAbsent && inspectsReceiver && child == user.child(0) && !user.arguments.contains(child))
                    return;
                if (user.opcode == Opcode::StrictEq && constantASCIIString(user.child(user.child(0) == child)))
                    return;
                if (mayBeAbsent && user.opcode == Opcode::StrictEq && (m_function.at(user.child(user.child(0) == child)).opcode == Opcode::Undefined || m_function.at(user.child(user.child(0) == child)).opcode == Opcode::Null))
                    return;
                isPart.clear(child.index());
            });
        }
        if (block.terminator.value)
            isPart.clear(block.terminator.value.index());
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                isPart.clear(argument.index());
        }
    }
    for (size_t index : isPart)
        m_isLazilyFetched.set(index);
    m_substringsOnlyInspected = WTF::move(isPart);
}

const Lowering::Substring* Lowering::substringCreatedBy(ValueID id) const
{
    if (m_substrings.isEmpty() || !id)
        return nullptr;
    auto found = m_substrings.find(id.index());
    return found == m_substrings.end() ? nullptr : &found->value;
}

Integer Lowering::lengthOf(const Substring& part)
{
    Integer length = sub(part.to, part.from);
    return select(lessThan(length, integer(0)), integer(0), length);
}

Pointer Lowering::fiberOf(const StringReceiver& receiver)
{
    return loadField<Pointer>(receiver.string, m_heaps.JSRopeString_fiber0, receiver.flags | IsFixed);
}

Integer Lowering::stringLength(const StringReceiver& receiver)
{
    if (auto* opened = openedString(receiver.id))
        return Integer(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), opened->length));
    Pointer fiber = fiberOf(receiver);
    Integer length = zeroExtended(either<Word32>(hasNoBits(fiber, JSString::isRopeInPointer), [&] {
        return loadField<Word32>(fiber, m_heaps.StringImpl_length);
    }, [&] {
        return loadField<Word32>(receiver.string, m_heaps.JSRopeString_length);
    }));
    ensureStillAliveHere(receiver.string);
    return length;
}

Lowering::Characters Lowering::charactersOf(const StringReceiver& receiver, LBasicBlock ifRope)
{
    if (auto characters = openedStringCharacters(receiver))
        return *characters;
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> data;
    Vector<ValueFromBlock, 3> flags;
    Vector<ValueFromBlock, 3> lengths;
    auto yield = [&](Pointer first, Word32 implFlags, Word32 length) {
        data.append(m_out.anchor(first.value()));
        flags.append(m_out.anchor(implFlags.value()));
        lengths.append(m_out.anchor(length.value()));
        m_out.jump(continuation);
    };
    auto returnWholeString = [&](Pointer impl) {
        yield(loadField<Pointer>(impl, m_heaps.StringImpl_data), loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed), loadField<Word32>(impl, m_heaps.StringImpl_length));
    };

    Pointer fiber = fiberOf(receiver);
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(rope));
    m_out.appendTo(flat);
    returnWholeString(fiber);

    m_out.appendTo(rope);
    if (!ifRope && !m_activeHandler) {
        SubstringCharacters opened = ropeCharacters(receiver.string);
        yield(Pointer(opened.data), Word32(opened.flags), Word32(opened.length));
        m_out.appendTo(continuation);
        return { Pointer(m_out.phi(Int64, data)), Word32(m_out.phi(Int32, flags)), zeroExtended(Word32(m_out.phi(Int32, lengths))), receiver.string };
    }
    LBasicBlock substring = newColdBlock();
    LBasicBlock notSubstring = newColdBlock();
    CellPointer string(m_out.opaque(receiver.string.value()));
    Pointer current = loadField<Pointer>(string, m_heaps.JSRopeString_fiber0);
    m_out.branch(hasNoBits(current, JSRopeString::isSubstringInPointer).value(), unsure(notSubstring), unsure(substring));

    m_out.appendTo(substring);
    SubstringCharacters part = substringCharacters(receiver.string);
    yield(Pointer(part.data), Word32(part.flags), Word32(part.length));

    m_out.appendTo(notSubstring);
    Word32 isNowFlat = hasNoBits(current, JSString::isRopeInPointer);
    if (ifRope) {
        orElse(isNowFlat, ifRope);
        returnWholeString(current);
    } else {
        returnWholeString(eitherPointer(isNowFlat, [&] {
            return current;
        }, [&] {
            return AOT_TYPED_OPERATION(Flatten, receiver.string);
        }));
    }

    m_out.appendTo(continuation);
    return { Pointer(m_out.phi(Int64, data)), Word32(m_out.phi(Int32, flags)), zeroExtended(Word32(m_out.phi(Int32, lengths))), receiver.string };
}

Word32 Lowering::codeUnitAt(const Characters& characters, Integer index)
{
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasBits(characters.flags, StringImpl::flagIs8Bit()).value(), usually(narrow), rarely(wide));
    m_out.appendTo(narrow);
    ValueFromBlock fromNarrow = m_out.anchor(marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, index, 0).value())), IsFixed));
    m_out.jump(continuation);
    m_out.appendTo(wide);
    ValueFromBlock fromWide = m_out.anchor(marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), offsetBy(characters.data, index, 1).value())), IsFixed));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    Word32 unit(m_out.phi(Int32, fromNarrow, fromWide));
    ensureStillAliveHere(characters.owner);
    return unit;
}

Word32 Lowering::codePointAt(const Characters& characters, Integer index)
{
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = newColdBlock();
    LBasicBlock isLead = newColdBlock();
    LBasicBlock hasNext = newColdBlock();
    LBasicBlock isPair = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    auto isSurrogate = [&](Word32 unit, uint32_t first) {
        return equal(bitAnd(unit, word32(~0x3ff)), word32(first));
    };
    auto wideUnitAt = [&](Integer at) {
        return Word32(marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), offsetBy(characters.data, at, 1).value())), IsFixed));
    };
    Vector<ValueFromBlock, 5> results;
    m_out.branch(hasBits(characters.flags, StringImpl::flagIs8Bit()).value(), usually(narrow), rarely(wide));
    m_out.appendTo(narrow);
    results.append(m_out.anchor(marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, index, 0).value())), IsFixed)));
    m_out.jump(continuation);
    m_out.appendTo(wide);
    Word32 lead = wideUnitAt(index);
    results.append(m_out.anchor(lead.value()));
    m_out.branch(isSurrogate(lead, 0xd800).value(), unsure(isLead), unsure(continuation));
    m_out.appendTo(isLead);
    Integer next = add(index, integer(1));
    results.append(m_out.anchor(lead.value()));
    m_out.branch(below(next, characters.length).value(), usually(hasNext), rarely(continuation));
    m_out.appendTo(hasNext);
    Word32 trail = wideUnitAt(next);
    results.append(m_out.anchor(lead.value()));
    m_out.branch(isSurrogate(trail, 0xdc00).value(), usually(isPair), rarely(continuation));
    m_out.appendTo(isPair);
    results.append(m_out.anchor(add(add(shiftLeft(lead, word32(10)), trail), word32(-U16_SURROGATE_OFFSET)).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    Word32 codePoint(m_out.phi(Int32, results));
    ensureStillAliveHere(characters.owner);
    return codePoint;
}

CellPointer Lowering::stringFromCodeUnit(Word32 unit)
{
    return either<CellPointer>(belowOrEqual(unit, word32(maxSingleCharacterString)), [&] {
        return CellPointer(singleCharacterString(unit.value()));
    }, [&] {
        return AOT_TYPED_OPERATION(CharToString, unit);
    });
}

CellPointer Lowering::singleCharacterStringAt(const Characters& characters, Integer index, Word32* codeUnit)
{
    static_assert(maxSingleCharacterString == 0xff);
    Vector<ValueFromBlock, 2> codeUnits;
    CellPointer string = either<CellPointer>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
        LValue narrow = marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, index, 0).value())), IsFixed);
        if (codeUnit)
            codeUnits.append(m_out.anchor(narrow));
        return CellPointer(singleCharacterString(narrow));
    }, [&] {
        LValue wide = marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), offsetBy(characters.data, index, 1).value())), IsFixed);
        CellPointer created = stringFromCodeUnit(Word32(wide));
        if (codeUnit)
            codeUnits.append(m_out.anchor(wide));
        return created;
    });
    if (codeUnit)
        *codeUnit = Word32(m_out.phi(Int32, codeUnits));
    ensureStillAliveHere(characters.owner);
    return string;
}

Operand Lowering::getStringElement(const StringReceiver& receiver, ValueID key, const Type& type, LBasicBlock otherwise, B3::Variable* codeUnitVariable)
{
    Rep rep = repOf(type);
    LBasicBlock outside = otherwise ? otherwise : newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    rejectNullish(receiver, { });
    Integer index = indexOrElse(key, outside);
    Characters characters = charactersOf(receiver);
    if (!std::exchange(m_indexIsKnownInBounds, false))
        orElse(below(index, characters.length), outside);
    bool tracksCodeUnit = !otherwise;
    Word32 codeUnit;
    CellPointer string = singleCharacterStringAt(characters, index, tracksCodeUnit || codeUnitVariable ? &codeUnit : nullptr);
    if (codeUnitVariable)
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), codeUnitVariable, zeroExtended(codeUnit).value());
    Vector<ValueFromBlock, 2> codeUnits;
    if (tracksCodeUnit)
        codeUnits.append(m_out.anchor(zeroExtended(codeUnit).value()));
    join.add(ensure(string, m_program.at(m_program.types.string), type));
    if (otherwise)
        return join.finish();

    m_out.appendTo(outside);
    if (auto nothing = absentElement(type)) {
        if (tracksCodeUnit)
            codeUnits.append(m_out.anchor(m_out.constInt64(undefinedCodeUnit)));
        join.add(*nothing);
    }
    Operand joined = join.finish();
    if (tracksCodeUnit)
        didCreateStringFromCodeUnit(m_currentBlock->values[m_currentIndexInBlock], joined, codeUnits);
    return joined;
}

Integer Lowering::positionOf(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep == Rep::Int64)
        return operand.as<Integer>();
    Number asNumber = operand.rep == Rep::OptionalDouble ? assumedNumber(operand.as<OptionalNumber>()) : operand.as<Number>();
#if CPU(ARM64)
    return truncateToInteger(asNumber);
#else
    return select(equal(asNumber, asNumber), truncateToInteger(asNumber), integer(0));
#endif
}

Operand Lowering::numberOrIfUndefined(ValueID id, Number otherwise)
{
    Operand operand = get(id);
    switch (operand.rep) {
    case Rep::Int64:
    case Rep::Double:
        return operand;
    case Rep::OptionalInt64:
        return Operand(Rep::Double, select(isMarker(operand), otherwise, toNumber(assumedInteger(operand.as<OptionalInteger>()))).value());
    case Rep::OptionalDouble:
        return Operand(Rep::Double, select(isMarker(operand), otherwise, assumedNumber(operand.as<OptionalNumber>())).value());
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Integer Lowering::relativeStringIndex(ValueID id, Integer length, bool isRelativeToEnd)
{
    return relativeStringIndex(get(id), typeOf(id), length, isRelativeToEnd);
}

Integer Lowering::relativeStringIndex(Operand operand, const Type& type, Integer length, bool isRelativeToEnd)
{
    Integer x;
    if (operand.rep == Rep::Int64)
        x = operand.as<Integer>();
    else {
#if CPU(ARM64)
        x = truncateToInteger(operand.as<Number>());
#else
        if (!m_out.m_block->frequency())
            return Integer(AOT_TYPED_OPERATION(IndexInString, operand.as<Number>(), length, word32(isRelativeToEnd)).value());
        Number limit = number(static_cast<double>(1ll << 32));
        Number given = operand.as<Number>();
        given = select(equal(given, given), given, number(0));
        given = select(greaterThan(given, limit), limit, given);
        given = select(lessThan(given, neg(limit)), neg(limit), given);
        x = truncateToInteger(given);
#endif
    }
    Integer capped = select(greaterThan(x, length), length, x);
    if (type.isInteger() && type.integers.minimum >= 0)
        return capped;
    Integer zero = integer(0);
    Integer fromEnd = zero;
    if (isRelativeToEnd) {
        Integer sum = add(length, x);
        fromEnd = select(lessThan(sum, zero), zero, sum);
    }
    return select(lessThan(x, zero), fromEnd, capped);
}

CellPointer Lowering::allocateRope(LBasicBlock slowPath)
{
    LValue cell = allocateHeapCell(fixedPointer(Instance::offsetOfRopeStringAllocator()), sizeof(JSRopeString), slowPath);
    applyEffects(cell->as<PatchpointValue>(), { Writes::RuntimeState, false });
    storeHeader(cell, fixed64(Instance::offsetOfStringHeader()));
    return CellPointer(cell);
}

CellPointer Lowering::makeSubstring(const StringReceiver& receiver, Integer length, Integer from, Integer to)
{
    auto byStub = [&](bool isSlowPathCall) {
        return CellPointer(callAllocationStub(Stub::Substring, { { receiver.string.value(), GPRInfo::toArgumentRegister(1) }, { from.value(), GPRInfo::toArgumentRegister(2) }, { to.value(), GPRInfo::toArgumentRegister(3) } }, effectsForOperation<operationAOTTypedSubstring>(), true, isSlowPathCall));
    };
    if (prefersGenericPathInColdCode())
        return byStub(false);
    if (!m_isInLoop && !m_activeHandler) {
        LBasicBlock several = m_out.newBlock();
        LBasicBlock part = m_out.newBlock();
        LBasicBlock flat = m_out.newBlock();
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, Rep::Cell, Int64);
        Integer span = sub(to, from);
        m_out.branch(greaterThan(span, integer(1)).value(), usually(several), rarely(otherwise));
        m_out.appendTo(several);
        m_out.branch(equal(span, length).value(), rarely(otherwise), usually(part));
        m_out.appendTo(part);
        static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
        Pointer fiber = fiberOf(receiver);
        m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(otherwise));
        m_out.appendTo(flat);
        Word32 flags = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
        LValue base = receiver.string.value();
        CellPointer created = allocateRope(otherwise);
        m_out.store64(m_out.bitOr(m_out.constInt64(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), zeroExtended(bitAnd(flags, word32(StringImpl::flagIs8Bit()))).value()), created.value(), m_heaps.JSRopeString_fiber0);
        m_out.store64(m_out.bitOr(span.value(), m_out.shl(base, m_out.constInt32(32))), created.value(), m_heaps.JSRopeString_fiber1);
        m_out.store64(m_out.bitOr(m_out.lShr(base, m_out.constInt32(32)), m_out.shl(from.value(), m_out.constInt32(16))), created.value(), m_heaps.JSRopeString_fiber2);
        fenceAfterInitialization();
        join.add(created);
        m_out.appendTo(otherwise);
        join.add(byStub(true));
        return join.finish().as<CellPointer>();
    }
    LBasicBlock atMostOne = m_out.newBlock();
    LBasicBlock none = m_out.newBlock();
    LBasicBlock one = m_out.newBlock();
    LBasicBlock several = m_out.newBlock();
    LBasicBlock whole = m_out.newBlock();
    LBasicBlock part = m_out.newBlock();
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = newColdBlock();
    LBasicBlock substring = newColdBlock();
    LBasicBlock make = m_out.newBlock();
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Cell, Int64);

    Integer span = sub(to, from);
    m_out.branch(greaterThan(span, integer(1)).value(), usually(several), rarely(atMostOne));

    m_out.appendTo(atMostOne);
    m_out.branch(equal(span, integer(1)).value(), unsure(one), unsure(none));
    m_out.appendTo(none);
    join.add(CellPointer(emptyString()));
    m_out.appendTo(one);
    join.add(singleCharacterStringAt(charactersOf(receiver), from));

    m_out.appendTo(several);
    m_out.branch(equal(span, length).value(), rarely(whole), usually(part));
    m_out.appendTo(whole);
    join.add(receiver.string);

    m_out.appendTo(part);
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    Pointer fiber = fiberOf(receiver);
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(rope));
    m_out.appendTo(flat);
    ValueFromBlock flatBase = m_out.anchor(receiver.string.value());
    ValueFromBlock flatStart = m_out.anchor(from.value());
    ValueFromBlock flatFlags = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
    m_out.jump(make);

    m_out.appendTo(rope);
    CellPointer string(m_out.opaque(receiver.string.value()));
    Pointer current = loadField<Pointer>(string, m_heaps.JSRopeString_fiber0);
    m_out.branch(hasNoBits(current, JSRopeString::isSubstringInPointer).value(), unsure(slowPath), unsure(substring));
    m_out.appendTo(substring);
    LValue lengthAndBaseLow = m_out.load64(string.value(), m_heaps.JSRopeString_fiber1);
    LValue baseHighAndOffset = m_out.load64(string.value(), m_heaps.JSRopeString_fiber2);
    ValueFromBlock substringBase = m_out.anchor(m_out.bitOr(m_out.lShr(lengthAndBaseLow, m_out.constInt32(32)), m_out.shl(m_out.bitAnd(baseHighAndOffset, m_out.constInt64(0xffff)), m_out.constInt32(32))));
    ValueFromBlock startInBase = m_out.anchor(m_out.add(from.value(), m_out.lShr(baseHighAndOffset, m_out.constInt32(16))));
    ValueFromBlock substringFlags = m_out.anchor(m_out.castToInt32(current.value()));
    m_out.jump(make);

    m_out.appendTo(make);
    LValue base = m_out.phi(Int64, flatBase, substringBase);
    LValue start = m_out.phi(Int64, flatStart, startInBase);
    Word32 flags(m_out.phi(Int32, flatFlags, substringFlags));
    CellPointer created = allocateRope(slowPath);
    m_out.store64(m_out.bitOr(m_out.constInt64(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), zeroExtended(bitAnd(flags, word32(StringImpl::flagIs8Bit()))).value()), created.value(), m_heaps.JSRopeString_fiber0);
    m_out.store64(m_out.bitOr(span.value(), m_out.shl(base, m_out.constInt32(32))), created.value(), m_heaps.JSRopeString_fiber1);
    m_out.store64(m_out.bitOr(m_out.lShr(base, m_out.constInt32(32)), m_out.shl(start, m_out.constInt32(16))), created.value(), m_heaps.JSRopeString_fiber2);
    fenceAfterInitialization();
    join.add(created);

    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(Substring, receiver.string, low32(from), low32(to)));
    return join.finish().as<CellPointer>();
}

bool Lowering::searchesForConstantWithoutCell(const Value& value) const
{
    if (value.arguments.isEmpty() || m_function.at(value.arguments[0]).opcode != Opcode::String)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline)
        return false;
    auto text = constantASCIIString(value.arguments[0]);
    if (!text)
        return false;
    auto isOutsideLoops = [&] {
        BlockID block = m_definingBlock[&value - m_function.values.span().data()];
        return block && !m_loopBlocks.get(block.index());
    };
    switch (implementation.inlined) {
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
        return text->size() <= maxInlineStringCompareLength || (value.arguments.size() == 1 && isOutsideLoops());
    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf:
    case InlineBuiltin::StringLastIndexOf:
        return text->size() == 1 || (!text->empty() && isOutsideLoops());
    default:
        return false;
    }
}

ValueID Lowering::stringConstantOf(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast && typeOf(id).primitives & Primitive::String)
        id = m_function.at(id).child(0);
    return m_function.at(id).opcode == Opcode::String ? id : ValueID();
}

std::optional<std::span<const uint8_t>> Lowering::constantASCIIString(ValueID id) const
{
    ValueID constant = stringConstantOf(id);
    if (!constant)
        return std::nullopt;
    auto text = m_program.string(m_function.at(constant).string());
    if (!charactersAreAllASCII(byteCast<Latin1Character>(text)))
        return std::nullopt;
    return text;
}

std::optional<char16_t> Lowering::constantCodeUnit(ValueID id) const
{
    ValueID constant = stringConstantOf(id);
    if (!constant)
        return std::nullopt;
    auto text = m_program.string(m_function.at(constant).string());
    if (text.size() == 1 && isASCII(text[0]))
        return text[0];
    if (text.size() == 2 && (text[0] & 0xe0) == 0xc0 && (text[1] & 0xc0) == 0x80)
        return (text[0] & 0x1f) << 6 | (text[1] & 0x3f);
    if (text.size() == 3 && (text[0] & 0xf0) == 0xe0 && (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80)
        return (text[0] & 0x0f) << 12 | (text[1] & 0x3f) << 6 | (text[2] & 0x3f);
    return std::nullopt;
}

std::optional<Word32> Lowering::areTrackedCodeUnitsEqual(ValueID a, ValueID b)
{
    for (auto [constant, other] : { std::pair { a, b }, std::pair { b, a } }) {
        auto unit = constantCodeUnit(constant);
        if (!unit)
            continue;
        if (auto kept = stringCodeUnit(other))
            return equal(*kept, integer(*unit));
    }
    return std::nullopt;
}

BitVector computeMapKeyValues(const Program& program, FunctionID id, const Function& function)
{
    BitVector keys;
    auto& builtins = Builtins::singleton();
    bool returnsMapKeys = program.functionsReturningMapKeys.get(id.index());
    auto lookAt = [&](ValueID id) {
        auto& value = function.at(id);
        if (value.opcode == Opcode::GetByVal || value.opcode == Opcode::PutByVal) {
            keys.set(value.child(1).index());
            return;
        }
        if (value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && program.at(value.function()).isSharedOutlinedBody) {
            auto& body = program.at(value.function());
            BitVector bodyKeys = computeMapKeyValues(program, value.function(), body);
            auto& parameters = body.blocks[0].parameters;
            for (unsigned i = 0; i < std::min(parameters.size(), value.arguments.size()); ++i) {
                if (bodyKeys.get(parameters[i].index()))
                    keys.set(value.arguments[i].index());
            }
            return;
        }
        if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty())
            return;
        auto& implementation = builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline)
            return;
        switch (implementation.inlined) {
        case InlineBuiltin::MapGet:
        case InlineBuiltin::MapSet:
        case InlineBuiltin::MapHas:
        case InlineBuiltin::MapDelete:
        case InlineBuiltin::SetAdd:
        case InlineBuiltin::SetHas:
        case InlineBuiltin::SetDelete:
            keys.set(value.arguments[0].index());
            break;
        default:
            break;
        }
    };
    static constexpr OpcodeSet mayCreateKeys { Opcode::GetByVal, Opcode::PutByVal, Opcode::CallDirect, Opcode::CallBuiltin };
    if (&function == &program.at(id))
        program.functions.forEachIndexedValue(id.index(), mayCreateKeys, lookAt);
    else {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values)
                lookAt(id);
        }
    }
    if (returnsMapKeys) {
        for (auto& block : function.blocks) {
            if (block.terminator.kind == TerminatorKind::Return && block.terminator.value)
                keys.set(block.terminator.value.index());
        }
    }
    for (bool changed = !keys.isEmpty(); changed;) {
        changed = false;
        for (size_t b = function.blocks.size(); b--;) {
            auto& block = function.blocks[b];
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (unsigned i = 0; i < std::min(parameters.size(), target.arguments.size()); ++i) {
                    if (keys.get(parameters[i].index()))
                        changed |= !keys.set(target.arguments[i].index());
                }
            }
            for (size_t i = block.values.size(); i--;) {
                ValueID id = block.values[i];
                if (!keys.get(id.index()))
                    continue;
                auto& value = function.at(id);
                if (value.opcode == Opcode::Cast || value.opcode == Opcode::ToString)
                    changed |= !keys.set(value.child(0).index());
                else if (value.opcode == Opcode::Add && program.at(value.type).isString()) {
                    changed |= !keys.set(value.child(0).index());
                    changed |= !keys.set(value.child(1).index());
                } else if (value.opcode == Opcode::Strcat) {
                    for (ValueID operand : value.arguments)
                        changed |= !keys.set(operand.index());
                }
            }
        }
    }
    return keys;
}

void computeFunctionsReturningMapKeys(Program& program)
{
    unsigned count = program.functions.size();
    program.functionsReturningMapKeys = BitVector(count);
    Vector<unsigned> worklist;
    for (unsigned i = 0; i < count; ++i) {
        if (program.hasCode(FunctionID(i)))
            worklist.append(i);
    }
    while (!worklist.isEmpty()) {
        Vector<Vector<unsigned>> callees(worklist.size());
        forEachIndexInParallel(worklist.size(), [&](unsigned k) {
            unsigned i = worklist[k];
            auto& function = std::as_const(program).functions[i];
            for (size_t index : computeMapKeyValues(program, FunctionID(i), function)) {
                auto& value = function.values[index];
                if (value.opcode != Opcode::CallDirect || value.isJumpToContinuation() || std::as_const(program).at(value.function()).has(Format::FunctionIsHiddenInStackTraces))
                    continue;
                callees[k].append(value.function().index());
            }
        });
        worklist.shrink(0);
        for (auto& perFunction : callees) {
            for (unsigned callee : perFunction) {
                if (!program.functionsReturningMapKeys.set(callee))
                    worklist.append(callee);
            }
        }
    }
}

StringBuilderAnalysis analyzeStringBuilders(const Program& program, FunctionID functionID, const Function& function)
{
    StringBuilderAnalysis result;
    const Function* whole = &function == &program.at(functionID) ? nullptr : &program.at(functionID);
    unsigned numberOfValues = function.values.size();
    unsigned numberOfBlocks = function.blocks.size();
    auto typeOf = [&](ValueID id) -> const Type& {
        return program.typeOf(function, id);
    };
    auto isStringOrNumber = [&](ValueID id) {
        return typeOf(id).isString() || typeOf(id).isNumber();
    };
    auto mayBeBuilderState = [&](ValueID id) {
        auto& type = typeOf(id);
        return holdsCells(function.at(id).rep) && type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    };
    auto isNumberOrNullish = [&](ValueID id) {
        auto& type = typeOf(id);
        return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    };
    auto isAppendable = [&](ValueID id) {
        return typeOf(id).isString() || isNumberOrNullish(id) || mayBeBuilderState(id);
    };
    auto isConcatenation = [&](const Value& value, bool allowsStateAsFirstOperand) {
        if ((value.opcode != Opcode::Add && value.opcode != Opcode::Strcat) || !holdsCells(value.rep) || !program.at(value.type).isString())
            return false;
        if (value.opcode == Opcode::Add) {
            bool leftIsString = typeOf(value.child(0)).isString() || (allowsStateAsFirstOperand && mayBeBuilderState(value.child(0)));
            return (leftIsString && isAppendable(value.child(1))) || (typeOf(value.child(1)).isString() && isAppendable(value.child(0)));
        }
        if (value.opcode != Opcode::Strcat || value.arguments.isEmpty())
            return false;
        for (ValueID operand : value.arguments) {
            if (!isAppendable(operand))
                return false;
        }
        return true;
    };
    auto hasBoundedLength = [&](auto& hasBoundedLength, ValueID id) -> bool {
        auto& value = function.at(id);
        switch (value.opcode) {
        case Opcode::String:
            return true;
        case Opcode::ToString:
            return hasBoundedLength(hasBoundedLength, value.child(0));
        case Opcode::Add:
            return isConcatenation(value, false) && hasBoundedLength(hasBoundedLength, value.child(0)) && hasBoundedLength(hasBoundedLength, value.child(1));
        case Opcode::Strcat:
            if (!isConcatenation(value, false))
                return false;
            for (ValueID operand : value.arguments) {
                if (!hasBoundedLength(hasBoundedLength, operand))
                    return false;
            }
            return true;
        default:
            return isNumberOrNullish(id);
        }
    };
    bool hasConcatenation = false;
    for (auto& block : function.blocks) {
        if (block.terminator.kind == TerminatorKind::Invoke || (block.terminator.kind == TerminatorKind::Throw && !block.terminator.targets.isEmpty()))
            return result;
        for (ValueID id : block.values)
            hasConcatenation |= isConcatenation(function.at(id), true);
    }
    if (!hasConcatenation)
        return result;

    Vector<uint32_t> useCounts;
    useCounts.fill(0, numberOfValues);
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            function.at(id).forEachChild([&](ValueID child) {
                ++useCounts[child.index()];
            });
        }
        if (block.terminator.value)
            ++useCounts[block.terminator.value.index()];
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                ++useCounts[argument.index()];
        }
    }

    UncheckedKeyHashMap<uint32_t, StringBuilderAnalysis::Operands, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> foldedValuesForAppend;
    BitVector isFolded(numberOfValues);
    BitVector isInBlock(numberOfValues);
    Vector<unsigned> indexInBlock;
    indexInBlock.fill(0, numberOfValues);
    Vector<unsigned> observableBefore;
    Vector<ValueID> appends;
    for (auto& block : function.blocks) {
        isInBlock.clearAll();
        observableBefore.shrink(0);
        observableBefore.append(0);
        for (unsigned i = 0; i < block.values.size(); ++i) {
            ValueID id = block.values[i];
            isInBlock.set(id.index());
            indexInBlock[id.index()] = i;
            auto& value = function.at(id);
            bool isObservable;
            switch (value.opcode) {
            case Opcode::String:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::Boolean:
            case Opcode::Undefined:
            case Opcode::Null:
                isObservable = false;
                break;
            case Opcode::ToString:
                isObservable = !isAppendable(value.child(0));
                break;
            default:
                isObservable = !isConcatenation(value, true);
                break;
            }
            observableBefore.append(observableBefore.last() + isObservable);
        }
        for (size_t i = block.values.size(); i--;) {
            ValueID root = block.values[i];
            if (isFolded.get(root.index()) || !isConcatenation(function.at(root), true))
                continue;
            StringBuilderAnalysis::Operands operands;
            StringBuilderAnalysis::Operands folded;
            auto collect = [&](auto& collect, ValueID id) -> void {
                auto& value = function.at(id);
                if (id != root) {
                    bool canFold = isInBlock.get(id.index()) && useCounts[id.index()] == 1;
                    if (canFold && value.opcode == Opcode::ToString)
                        canFold = isAppendable(value.child(0));
                    else if (canFold)
                        canFold = isConcatenation(value, false) && (observableBefore[i] == observableBefore[indexInBlock[id.index()] + 1] || hasBoundedLength(hasBoundedLength, id));
                    if (!canFold) {
                        operands.append(id);
                        return;
                    }
                    folded.append(id);
                    isFolded.set(id.index());
                }
                if (value.opcode == Opcode::Strcat) {
                    for (ValueID operand : value.arguments)
                        collect(collect, operand);
                    return;
                }
                collect(collect, value.child(0));
                if (value.opcode == Opcode::Add)
                    collect(collect, value.child(1));
            };
            collect(collect, root);
            result.appendOperands.add(root.index(), WTF::move(operands));
            foldedValuesForAppend.add(root.index(), WTF::move(folded));
            appends.append(root);
        }
    }

    Vector<uint32_t> parent(numberOfValues);
    for (unsigned i = 0; i < numberOfValues; ++i)
        parent[i] = i;
    auto find = [&](uint32_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    BitVector isCandidate(numberOfValues);
    for (ValueID id : appends)
        isCandidate.set(id.index());
    for (unsigned b = 1; b < numberOfBlocks; ++b) {
        for (ValueID parameter : function.blocks[b].parameters) {
            if (mayBeBuilderState(parameter))
                isCandidate.set(parameter.index());
        }
    }
    for (ValueID id : appends) {
        ValueID first = result.appendOperands.get(id.index())[0];
        if (isCandidate.get(first.index()))
            parent[find(id.index())] = find(first.index());
    }
    for (auto& block : function.blocks) {
        for (auto& target : block.terminator.targets) {
            auto& parameters = function.at(target.block).parameters;
            for (unsigned i = 0; i < std::min(parameters.size(), target.arguments.size()); ++i) {
                if (isCandidate.get(parameters[i].index()) && isCandidate.get(target.arguments[i].index()))
                    parent[find(parameters[i].index())] = find(target.arguments[i].index());
            }
        }
    }
    UncheckedKeyHashMap<uint32_t, ASCIILiteral, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> rejectionReasons;
    auto reject = [&](ValueID id, ASCIILiteral why) {
        rejectionReasons.add(find(id.index()), why);
    };
    auto isSameBuilder = [&](ValueID a, ValueID b) {
        return isCandidate.get(a.index()) && isCandidate.get(b.index()) && find(a.index()) == find(b.index());
    };

    auto isShortASCII = [&](StringID string) {
        auto text = program.string(string);
        return text.size() <= maxStringBuilderInlineConstantLength && charactersAreAllASCII(byteCast<Latin1Character>(text));
    };
    auto shortConstantLength = [&](ValueID id) -> std::optional<size_t> {
        auto& value = function.at(id);
        if (value.opcode != Opcode::String || !isShortASCII(value.string()))
            return std::nullopt;
        return program.string(value.string()).size();
    };
    struct InitialValue {
        bool isString { false };
        StringID text;
    };
    auto initialValueForContinuation = [&](const Target& target, unsigned index) -> InitialValue {
        ValueID argument = target.arguments[index];
        if (!whole || argument.index() < whole->values.size() || function.at(argument).opcode != Opcode::Parameter || whole->blocks.size() != numberOfBlocks)
            return { };
        InitialValue result;
        bool isFirst = true;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (b && (!function.blocks[b].values.isEmpty() || !function.blocks[b].terminator.targets.isEmpty() || function.blocks[b].terminator.kind != TerminatorKind::Unreachable))
                continue;
            for (auto& edge : whole->blocks[b].terminator.targets) {
                if (edge.block != target.block)
                    continue;
                if (index >= edge.arguments.size() || !program.typeOf(*whole, edge.arguments[index]).isString())
                    return { };
                auto& incoming = whole->at(edge.arguments[index]);
                StringID text = incoming.opcode == Opcode::String && isShortASCII(incoming.string()) ? incoming.string() : StringID();
                if (isFirst)
                    result = { true, text };
                else if (result.text != text)
                    result.text = { };
                isFirst = false;
            }
        }
        return result;
    };

    BitVector isConsumed(numberOfValues);
    BitVector escapes(numberOfValues);
    BitVector startsEmpty(numberOfValues);
    BitVector hasNumber(numberOfValues);
    BitVector hasAppendToState(numberOfValues);
    BitVector hasStringOperand(numberOfValues);
    Vector<std::pair<ValueID, unsigned>> escapeSites;
    Vector<std::pair<ValueID, unsigned>> initializationSites;
    auto& builtins = Builtins::singleton();
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        auto escape = [&](ValueID id) {
            escapes.set(id.index());
            escapeSites.append({ id, b });
        };
        for (ValueID id : block.values) {
            if (isFolded.get(id.index()))
                continue;
            auto& value = function.at(id);
            if (auto found = result.appendOperands.find(id.index()); found != result.appendOperands.end()) {
                auto& operands = found->value;
                uint32_t builder = find(id.index());
                for (unsigned i = 0; i < operands.size(); ++i) {
                    ValueID operand = operands[i];
                    if (!i && isSameBuilder(operand, id)) {
                        isConsumed.set(operand.index());
                        hasAppendToState.set(builder);
                        continue;
                    }
                    if (!i && !isConcatenation(value, false))
                        reject(id, "first operand may not be a string"_s);
                    if (isSameBuilder(operand, id))
                        reject(id, "state is appended to another string"_s);
                    if (isCandidate.get(operand.index()))
                        escape(operand);
                    if (isNumberOrNullish(operand))
                        hasNumber.set(builder);
                    else if (!shortConstantLength(operand))
                        hasStringOperand.set(builder);
                }
                continue;
            }
            bool readsLength = false;
            if (value.opcode == Opcode::CallBuiltin) {
                auto& implementation = builtins.at(value.builtin()).implementation;
                readsLength = implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::StringLength;
            }
            value.forEachChild([&](ValueID child) {
                if (!isCandidate.get(child.index()) || (readsLength && child == value.child(0)))
                    return;
                escape(child);
            });
        }
        auto& terminator = block.terminator;
        if (terminator.value && isCandidate.get(terminator.value.index()))
            escape(terminator.value);
        for (auto& target : terminator.targets) {
            auto& parameters = function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size(); ++i) {
                ValueID argument = target.arguments[i];
                bool targetIsCandidate = i < parameters.size() && isCandidate.get(parameters[i].index());
                if (targetIsCandidate && isCandidate.get(argument.index())) {
                    isConsumed.set(argument.index());
                    continue;
                }
                if (isCandidate.get(argument.index()))
                    escape(argument);
                if (!targetIsCandidate)
                    continue;
                InitialValue initial;
                if (shortConstantLength(argument))
                    initial = { true, function.at(argument).string() };
                else if (typeOf(argument).isString())
                    initial.isString = true;
                else if (!b)
                    initial = initialValueForContinuation(target, i);
                if (!initial.isString) {
                    reject(parameters[i], "initial value may not be a string"_s);
                    continue;
                }
                initializationSites.append({ parameters[i], b });
                if (initial.text)
                    result.initialString.add(argument.index(), initial.text);
                else
                    hasStringOperand.set(find(parameters[i].index()));
                if (!initial.text || program.string(initial.text).empty())
                    startsEmpty.set(find(parameters[i].index()));
            }
        }
    }
    BitVector isViable(numberOfValues);
    for (ValueID id : appends) {
        uint32_t builder = find(id.index());
        if (hasAppendToState.get(builder) && hasNumber.get(builder) && !rejectionReasons.contains(builder))
            isViable.set(builder);
    }
    if (!isViable.isEmpty()) {
        Vector<uint32_t> residents;
        UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> residentNumbers;
        for (size_t i : isCandidate) {
            if (isViable.get(find(i)))
                residentNumbers.add(i, residents.size()), residents.append(i);
        }
        auto numberOfResident = [&](ValueID id) -> std::optional<unsigned> {
            if (!isCandidate.get(id.index()) || !isConsumed.get(id.index()))
                return std::nullopt;
            auto found = residentNumbers.find(id.index());
            return found == residentNumbers.end() ? std::nullopt : std::optional<unsigned>(found->value);
        };
        struct Event {
            std::optional<unsigned> defined;
            Vector<unsigned, 2> used;
        };
        Vector<Vector<Event, 0>> events(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (ValueID id : function.blocks[b].values) {
                Event event;
                if (auto found = residentNumbers.find(id.index()); found != residentNumbers.end())
                    event.defined = found->value;
                if (!isFolded.get(id.index())) {
                    auto use = [&](ValueID operand) {
                        if (auto number = numberOfResident(operand))
                            event.used.append(*number);
                    };
                    if (auto found = result.appendOperands.find(id.index()); found != result.appendOperands.end()) {
                        for (ValueID operand : found->value)
                            use(operand);
                    } else
                        function.at(id).forEachChild(use);
                }
                if (event.defined || !event.used.isEmpty())
                    events[b].append(WTF::move(event));
            }
        }
        Vector<BitVector> liveAtHead(numberOfBlocks);
        auto walkBackward = [&](unsigned b, const auto& onDefinition) {
            auto& block = function.blocks[b];
            BitVector live(residents.size());
            if (block.terminator.value) {
                if (auto number = numberOfResident(block.terminator.value))
                    live.set(*number);
            }
            for (auto& target : block.terminator.targets) {
                live.merge(liveAtHead[target.block.index()]);
                for (ValueID argument : target.arguments) {
                    if (auto number = numberOfResident(argument))
                        live.set(*number);
                }
            }
            for (size_t i = events[b].size(); i--;) {
                auto& event = events[b][i];
                if (event.defined) {
                    onDefinition(*event.defined, live);
                    live.clear(*event.defined);
                }
                for (unsigned number : event.used)
                    live.set(number);
            }
            for (ValueID parameter : block.parameters) {
                if (auto found = residentNumbers.find(parameter.index()); found != residentNumbers.end())
                    onDefinition(found->value, live);
            }
            for (ValueID parameter : block.parameters) {
                if (auto found = residentNumbers.find(parameter.index()); found != residentNumbers.end())
                    live.clear(found->value);
            }
            return live;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned b = numberOfBlocks; b--;) {
                BitVector live = walkBackward(b, [](unsigned, const BitVector&) { });
                if (live != liveAtHead[b]) {
                    liveAtHead[b] = WTF::move(live);
                    changed = true;
                }
            }
        }
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            walkBackward(b, [&](unsigned defined, const BitVector& live) {
                for (size_t other : live) {
                    if (other != defined && find(residents[other]) == find(residents[defined]))
                        reject(ValueID(residents[defined]), "two states are live at once"_s);
                }
            });
        }
    }

    BitVector isLoopCarried(numberOfValues);
    UncheckedKeyHashMap<uint32_t, BitVector, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> loopBlocks;
    if (!isViable.isEmpty()) {
        Vector<unsigned> place;
        place.fill(0, numberOfBlocks);
        BitVector seen(numberOfBlocks);
        Vector<std::pair<unsigned, unsigned>> stack;
        unsigned next = numberOfBlocks;
        seen.set(0);
        stack.append({ 0, 0 });
        while (!stack.isEmpty()) {
            auto& [block, index] = stack.last();
            auto& targets = function.blocks[block].terminator.targets;
            if (index < targets.size()) {
                unsigned successor = targets[index++].block.index();
                if (!seen.set(successor))
                    stack.append({ successor, 0 });
                continue;
            }
            place[block] = --next;
            stack.removeLast();
        }
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (auto& target : function.blocks[b].terminator.targets)
                predecessors[target.block.index()].append(b);
        }
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (auto& target : function.blocks[b].terminator.targets) {
                if (!seen.get(b) || place[target.block.index()] > place[b])
                    continue;
                Vector<uint32_t, 2> carried;
                auto& parameters = function.at(target.block).parameters;
                for (unsigned i = 0; i < std::min(parameters.size(), target.arguments.size()); ++i) {
                    if (isSameBuilder(parameters[i], target.arguments[i]))
                        carried.append(find(parameters[i].index()));
                }
                if (carried.isEmpty())
                    continue;
                BitVector body(numberOfBlocks);
                body.set(target.block.index());
                Vector<unsigned, 16> worklist;
                if (!body.set(b))
                    worklist.append(b);
                while (!worklist.isEmpty()) {
                    for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                        if (!body.set(predecessor))
                            worklist.append(predecessor);
                    }
                }
                for (uint32_t builder : carried) {
                    isLoopCarried.set(builder);
                    loopBlocks.add(builder, BitVector(numberOfBlocks)).iterator->value.merge(body);
                }
            }
        }
    }
    for (auto [state, block] : escapeSites) {
        if (!isViable.get(find(state.index())))
            continue;
        auto loop = loopBlocks.find(find(state.index()));
        if (loop == loopBlocks.end() ? isConsumed.get(state.index()) : loop->value.get(block))
            reject(state, "intermediate value escapes"_s);
    }
    for (auto [parameter, block] : initializationSites) {
        if (auto loop = loopBlocks.find(find(parameter.index())); loop != loopBlocks.end() && loop->value.get(block))
            reject(parameter, "reinitialized in the loop"_s);
    }
    std::optional<BitVector> keys;
    BitVector isKey(numberOfValues);
    for (unsigned i = 0; i < numberOfValues; ++i) {
        if (!isCandidate.get(i) || !escapes.get(i) || !hasNumber.get(find(i)) || (isLoopCarried.get(find(i)) && !hasStringOperand.get(find(i))))
            continue;
        if (!keys)
            keys = computeMapKeyValues(program, functionID, function);
        if (keys->get(i))
            isKey.set(find(i));
    }
    for (ValueID id : appends) {
        uint32_t builder = find(id.index());
        if (!hasAppendToState.get(builder))
            reject(id, "single concatenation"_s);
        else if (!hasNumber.get(builder))
            reject(id, "no numeric operand"_s);
        else if (!isLoopCarried.get(builder) && !isKey.get(builder))
            reject(id, "not in a loop and not a map key"_s);
        else if (hasStringOperand.get(builder) && !isKey.get(builder))
            reject(id, "string operand and not a map key"_s);
    }

    result.builderForValue.fill(StringBuilderAnalysis::invalidBuilder, numberOfValues);
    result.isFinalState.ensureSize(numberOfValues);
    result.isFoldedIntoAppend.ensureSize(numberOfValues);
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> builderIndices;
    for (ValueID id : appends) {
        uint32_t builder = find(id.index());
        if (auto why = rejectionReasons.find(builder); why != rejectionReasons.end()) {
            result.rejections.append({ id, why->value });
            result.appendOperands.remove(id.index());
            continue;
        }
        builderIndices.add(builder, builderIndices.size());
        for (ValueID operand : foldedValuesForAppend.get(id.index()))
            result.isFoldedIntoAppend.set(operand.index());
    }
    result.builderCount = builderIndices.size();
    result.mayBeEmpty.ensureSize(result.builderCount);
    for (unsigned i = 0; i < numberOfValues; ++i) {
        if (!isCandidate.get(i))
            continue;
        auto number = builderIndices.find(find(i));
        if (number == builderIndices.end())
            continue;
        result.builderForValue[i] = number->value;
        if (!isConsumed.get(i) && useCounts[i])
            result.isFinalState.set(i);
        if (startsEmpty.get(find(i)))
            result.mayBeEmpty.set(number->value);
    }
    return result;
}

void dumpStringBuilderRemarks(const Program& program, PrintStream& out)
{
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (!program.hasCode(FunctionID(i)))
            continue;
        auto& function = program.functions[i];
        StringBuilderAnalysis analysis = analyzeStringBuilders(program, FunctionID(i), function);
        auto dumpRemark = [&](ValueID id, ASCIILiteral outcome, ASCIILiteral why) {
            out.print(function.name ? StringView(byteCast<Latin1Character>(program.string(function.name))) : StringView("-"_s), " f", i, " v", id.index(), ": ", nameOf(function.at(id).opcode), ": ", outcome);
            if (why)
                out.print(": ", why);
            out.print("\n");
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (analysis.appendOperands.contains(id.index()))
                    dumpRemark(id, "stack"_s, analysis.isFinalState.get(id.index()) ? "materialized here"_s : ASCIILiteral());
            }
        }
        for (auto& [id, why] : analysis.rejections)
            dumpRemark(id, "rejected"_s, why);
    }
}

Lowering::StringPart Lowering::stringPartOf(CellPointer string)
{
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer fiber = fiberOf(stringReceiver(string));
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), unsure(flat), unsure(rope));
    m_out.appendTo(flat);
    ValueFromBlock flatLength = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_length).value());
    ValueFromBlock flatFlags = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
    m_out.jump(continuation);
    m_out.appendTo(rope);
    ValueFromBlock ropeLength = m_out.anchor(loadField<Word32>(string, m_heaps.JSRopeString_length).value());
    ValueFromBlock ropeFlags = m_out.anchor(m_out.castToInt32(fiber.value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    StringPart part { string, zeroExtended(Word32(m_out.phi(Int32, flatLength, ropeLength))), Word32(m_out.phi(Int32, flatFlags, ropeFlags)) };
    ensureStillAliveHere(string);
    return part;
}

Lowering::StringPart Lowering::stringPartOf(ValueID id)
{
    CellPointer string = toStringCell(id);
    if (auto text = constantASCIIString(id))
        return { string, integer(text->size()), word32(StringImpl::flagIs8Bit()), text->empty() };
    return stringPartOf(string);
}

Lowering::StringPart Lowering::concatenate(const StringPart& first, const StringPart& second, const StringPart* third)
{
    Integer length = add(first.length, second.length);
    Word32 flags = bitAnd(first.flags, second.flags);
    bool mayBeEmpty = first.mayBeEmpty && second.mayBeEmpty;
    if (third) {
        length = add(length, third->length);
        flags = bitAnd(flags, third->flags);
        mayBeEmpty &= third->mayBeEmpty;
    }
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Cell, Int64);
    auto returnIfEmpty = [&](Integer partLength, CellPointer result) {
        LBasicBlock isEmpty = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(isZero(partLength).value(), rarely(isEmpty), usually(isNot));
        m_out.appendTo(isEmpty);
        join.add(result);
        m_out.appendTo(isNot);
    };
    if (third) {
        if (mayBeEmpty)
            returnIfEmpty(length, CellPointer(emptyString()));
    } else {
        if (first.mayBeEmpty)
            returnIfEmpty(first.length, second.string);
        if (second.mayBeEmpty)
            returnIfEmpty(second.length, first.string);
    }
    static_assert(JSString::MaxLength == std::numeric_limits<int32_t>::max());
    orElse(isZero(bitAnd(length, integer(~static_cast<int64_t>(JSString::MaxLength)))), slowPath);
    CellPointer rope = allocateRope(slowPath);
    LValue mark = m_out.bitOr(m_out.constInt64(JSString::isRopeInPointer), zeroExtended(bitAnd(flags, word32(StringImpl::flagIs8Bit()))).value());
    m_out.store64(m_out.bitOr(first.string.value(), mark), rope.value(), m_heaps.JSRopeString_fiber0);
    m_out.store64(m_out.bitOr(length.value(), m_out.shl(second.string.value(), m_out.constInt32(32))), rope.value(), m_heaps.JSRopeString_fiber1);
    LValue secondHigh = m_out.lShr(second.string.value(), m_out.constInt32(32));
    m_out.store64(third ? m_out.bitOr(secondHigh, m_out.shl(third->string.value(), m_out.constInt32(16))) : secondHigh, rope.value(), m_heaps.JSRopeString_fiber2);
    fenceAfterInitialization();
    join.add(rope);

    m_out.appendTo(slowPath);
    CellPointer slow = AOT_TYPED_OPERATION(Concat, first.string, second.string);
    join.add(third ? AOT_TYPED_OPERATION(Concat, slow, third->string) : slow);
    return { join.finish().as<CellPointer>(), length, flags, mayBeEmpty };
}

CellPointer Lowering::concatenateByStub(std::span<const ValueID> parts)
{
    constexpr GPRReg first = isARM64() ? stubTemporaryGPRs[2] : GPRInfo::returnValueGPR;
    constexpr GPRReg second = isARM64() ? stubTemporaryGPRs[3] : GPRInfo::argumentGPR1;
    constexpr GPRReg third = isARM64() ? GPRInfo::argumentGPR3 : GPRInfo::argumentGPR2;
    CellPointer result = toStringCell(parts[0]);
    for (unsigned i = 1; i < parts.size();) {
        Vector<std::pair<LValue, GPRReg>, 4> operands { { result.value(), first }, { toStringCell(parts[i++]).value(), second } };
        bool hasThird = i < parts.size();
        if (hasThird)
            operands.append({ toStringCell(parts[i++]).value(), third });
        result = CellPointer(callAllocationStub(hasThird ? Stub::ConcatenateThree : Stub::ConcatenateTwo, operands, effectsForOperation<operationAOTTypedConcat>(), true));
    }
    return result;
}

CellPointer Lowering::concatenateByStub(CellPointer first, CellPointer second)
{
    return CellPointer(callAllocationStub(Stub::ConcatenateTwo, { { first.value(), isARM64() ? stubTemporaryGPRs[2] : GPRInfo::returnValueGPR }, { second.value(), isARM64() ? stubTemporaryGPRs[3] : GPRInfo::argumentGPR1 } }, effectsForOperation<operationAOTTypedConcat>(), true));
}

CellPointer Lowering::concatenateAsRopes(std::span<const ValueID> parts)
{
    if (parts.empty())
        return CellPointer(emptyString());
    return concatenateByStub(parts);
}

CellPointer Lowering::lowerStrcat(const Value& value)
{
    return concatenateAsRopes(value.arguments.span());
}

static bool isNumberOrNullish(const Type& type)
{
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
}

bool Lowering::isConcatenation(const Value& value) const
{
    auto isStringOrNumber = [&](ValueID id) {
        auto& type = m_program.at(m_function.at(id).type);
        return type.isString() || isNumberOrNullish(type);
    };
    switch (value.opcode) {
    case Opcode::ToString:
        return isStringOrNumber(value.child(0));
    case Opcode::Add:
        return isStringOrNumber(value.child(0)) && isStringOrNumber(value.child(1)) && m_program.at(value.type).isString();
    case Opcode::Strcat:
        for (ValueID part : value.arguments) {
            if (!isStringOrNumber(part))
                return false;
        }
        return true;
    default:
        return false;
    }
}

bool Lowering::collectConcatenationParts(const BitVector& isInBlock, ValueID id, bool isLast, ConcatenationParts& parts, ConcatenationParts& lookedThrough)
{
    auto& value = m_function.at(id);
    if (isLast || (isInBlock.get(id.index()) && useCountOf(id) == 1 && isConcatenation(value) && !m_stringBuilderAnalysis.isState(id))) {
        size_t numberOfParts = parts.size();
        size_t searchedCount = lookedThrough.size();
        if (!isLast)
            lookedThrough.append(id);
        bool hasOnlyNumbersAndConstants = true;
        auto collect = [&](ValueID part) {
            hasOnlyNumbersAndConstants &= collectConcatenationParts(isInBlock, part, false, parts, lookedThrough);
        };
        if (value.opcode == Opcode::Strcat) {
            for (ValueID part : value.arguments)
                collect(part);
        } else {
            collect(value.child(0));
            if (value.opcode == Opcode::Add)
                collect(value.child(1));
        }
        if (isLast || value.opcode == Opcode::ToString || hasOnlyNumbersAndConstants)
            return hasOnlyNumbersAndConstants;
        parts.shrink(numberOfParts);
        lookedThrough.shrink(searchedCount);
    }
    parts.append(id);
    return value.opcode == Opcode::String || isNumberOrNullish(m_program.at(value.type));
}

void Lowering::findConcatenationsWithNumbers()
{
    prepareStringBuilders();
    std::optional<BitVector> keys;
    BitVector isInBlock;
    for (auto& block : m_function.blocks) {
        bool hasConcatenation = false;
        for (ValueID id : block.values) {
            Opcode opcode = m_function.at(id).opcode;
            hasConcatenation |= opcode == Opcode::Add || opcode == Opcode::Strcat;
        }
        if (!hasConcatenation)
            continue;
        isInBlock.clearAll();
        for (ValueID id : block.values)
            isInBlock.set(id.index());
        for (size_t i = block.values.size(); i--;) {
            ValueID id = block.values[i];
            auto& value = m_function.at(id);
            if ((value.opcode != Opcode::Add && value.opcode != Opcode::Strcat) || value.rep != Rep::Cell || isLazilyFetched(id) || m_stringBuilderAnalysis.isState(id) || !isConcatenation(value))
                continue;
            if (!keys)
                keys = computeMapKeyValues(m_program, m_id, m_function);
            if (!keys->get(id.index()))
                continue;
            ConcatenationParts parts;
            ConcatenationParts lookedThrough;
            collectConcatenationParts(isInBlock, id, true, parts, lookedThrough);
            if (parts.size() < 2 || parts.size() > 8)
                continue;
            for (ValueID part : lookedThrough)
                m_isLazilyFetched.set(part.index());
            m_concatenationParts.add(id.index(), WTF::move(parts));
            if (useCountOf(id) == 1) {
                for (size_t j = i + 1; j < block.values.size(); ++j) {
                    auto& user = m_function.at(block.values[j]);
                    bool isLookupKey = user.opcode == Opcode::GetByVal && user.child(1) == id && user.child(0) != id;
                    if (user.opcode == Opcode::CallBuiltin && user.arguments.size() == 1 && user.arguments[0] == id && user.child(0) != id) {
                        auto& implementation = m_builtins.at(user.builtin()).implementation;
                        isLookupKey = implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::MapGet || implementation.inlined == InlineBuiltin::MapHas || implementation.inlined == InlineBuiltin::SetHas);
                    }
                    if (isLookupKey) {
                        m_sunkKeyConcatenations.set(id.index());
                        m_isLazilyFetched.set(id.index());
                    }
                }
            }
        }
    }
}

void Lowering::prepareStringBuilders()
{
    m_stringBuilderAnalysis = analyzeStringBuilders(m_program, m_id, m_function);
    if (!m_stringBuilderAnalysis.builderCount)
        return;
    for (unsigned i = 0; i < m_function.values.size(); ++i) {
        if (m_stringBuilderAnalysis.isFoldedIntoAppend.get(i) || isResidentInStringBuilder(ValueID(i)))
            m_isLazilyFetched.set(i);
    }
    for (unsigned i = 0; i < m_stringBuilderAnalysis.builderCount; ++i)
        m_stringBuilders.append(StackStringBuilder { m_proc.addVariable(Int64), m_proc.addVariable(Int64), m_proc.addStackSlot(stringBuilderCapacity + maxStringBuilderAppendRun), m_proc.addVariable(Int64), m_proc.addVariable(Int64) });
}

Integer Lowering::stringBuilderLength(const StackStringBuilder& builder)
{
    return Integer(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), builder.length));
}

void Lowering::setStringBuilderLength(const StackStringBuilder& builder, Integer length)
{
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.length, length.value());
}

LValue Lowering::stringBuilderPrefix(const StackStringBuilder& builder)
{
    return m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), builder.prefix);
}

Pointer Lowering::stringBuilderBuffer(const StackStringBuilder& builder)
{
    return Pointer(m_out.m_block->appendNew<SlotBaseValue>(m_proc, m_out.origin(), builder.buffer));
}

void Lowering::resetStringBuilder(const StackStringBuilder& builder)
{
    setStringBuilderLength(builder, Integer(m_out.int64Zero));
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.prefix, m_out.int64Zero);
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.whole, m_out.int64Zero);
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.lengthOfWhole, m_out.int64Zero);
}

void Lowering::flushStringBuilder(const StackStringBuilder& builder, Boxed operand)
{
    Pointer prefixAndLength = AOT_TYPED_OPERATION(StringBuilderFlush, CellPointer(stringBuilderPrefix(builder)), stringBuilderBuffer(builder), low32(stringBuilderLength(builder)), operand);
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.prefix, m_out.bitAnd(prefixAndLength.value(), m_out.constInt64((1ll << 48) - 1)));
    setStringBuilderLength(builder, Integer(m_out.lShr(prefixAndLength.value(), m_out.constInt32(48))));
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.whole, m_out.int64Zero);
}

void Lowering::flushStringBuilderIfOverCapacity(const StackStringBuilder& builder)
{
    LBasicBlock overCapacity = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(belowOrEqual(stringBuilderLength(builder), integer(stringBuilderCapacity)).value(), usually(continuation), rarely(overCapacity));
    m_out.appendTo(overCapacity);
    flushStringBuilder(builder, Boxed(m_out.int64Zero));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::appendConstantToStringBuilder(const StackStringBuilder& builder, std::span<const uint8_t> text)
{
    if (text.empty())
        return;
    Integer length = stringBuilderLength(builder);
    LValue cursor = m_out.add(stringBuilderBuffer(builder).value(), length.value());
    for (size_t offset = 0; offset < text.size();) {
        size_t remaining = text.size() - offset;
        unsigned width = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
        uint64_t bits = 0;
        for (unsigned i = 0; i < width; ++i)
            bits |= static_cast<uint64_t>(text[offset + i]) << (8 * i);
        TypedPointer pointer = m_out.address(m_heaps.AOTTyped_argumentBuffer, cursor, offset);
        switch (width) {
        case 8:
            m_out.store64(integer(static_cast<int64_t>(bits)).value(), pointer);
            break;
        case 4:
            m_out.store32(m_out.constInt32(static_cast<int32_t>(bits)), pointer);
            break;
        case 2:
            m_out.store32As16(m_out.constInt32(static_cast<int32_t>(bits)), pointer);
            break;
        default:
            m_out.store32As8(m_out.constInt32(static_cast<int32_t>(bits)), pointer);
            break;
        }
        offset += width;
    }
    setStringBuilderLength(builder, add(length, integer(text.size())));
}

void Lowering::appendNumberToStringBuilder(const StackStringBuilder& builder, ValueID id)
{
    static_assert(maxStringBuilderInlineDigits == sizeof(uint64_t));
    auto& type = typeOf(id);
    Operand operand = get(id);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock digitLoop = m_out.newBlock();
    LBasicBlock store = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    bool isDouble = operand.rep == Rep::Double || operand.rep == Rep::OptionalDouble;
    RELEASE_ASSERT(isDouble || operand.rep == Rep::Int64 || operand.rep == Rep::OptionalInt64);
    std::optional<Integer> exact = isDouble ? knownIntegerFor(id) : std::nullopt;
    Integer integer = exact ? *exact : isDouble ? integralOrElse(Number(operand.value), Range::int32(), slowPath) : Integer(operand.value);
    LBasicBlock severalDigits = m_out.newBlock();
    LBasicBlock oneDigit = digitLoop;
    auto constant = [&](uint64_t bits) {
        return this->integer(static_cast<int64_t>(bits)).value();
    };
    constexpr uint64_t asciiZeros = 0x3030303030303030ull;
    m_out.branch(below(integer, this->integer(10)).value(), unsure(oneDigit), unsure(severalDigits));

    m_out.appendTo(oneDigit);
    ValueFromBlock oneDigitString = m_out.anchor(m_out.add(integer.value(), constant('0')));
    ValueFromBlock oneDigitCount = m_out.anchor(m_out.constInt64(1));
    m_out.jump(store);

    m_out.appendTo(severalDigits);
    LBasicBlock eightDigits = m_out.newBlock();
    m_out.branch(below(integer, this->integer(100000000)).value(), usually(eightDigits), rarely(slowPath));

    m_out.appendTo(eightDigits);
    LValue upperHalf = m_out.lShr(m_out.mul(integer.value(), constant(0xd1b71759ull)), m_out.constInt32(45));
    LValue lowerHalf = m_out.sub(integer.value(), m_out.mul(upperHalf, constant(10000)));
    LValue halves = m_out.bitOr(upperHalf, m_out.shl(lowerHalf, m_out.constInt32(32)));
    LValue upperPairs = m_out.bitAnd(m_out.lShr(m_out.mul(halves, constant(10486)), m_out.constInt32(20)), constant(0x7full << 32 | 0x7f));
    LValue lowerPairs = m_out.sub(halves, m_out.mul(upperPairs, constant(100)));
    LValue pairs = m_out.add(m_out.shl(lowerPairs, m_out.constInt32(16)), upperPairs);
    LValue tens = m_out.bitAnd(m_out.lShr(m_out.mul(pairs, constant(103)), m_out.constInt32(10)), constant(0x000f000f000f000full));
    LValue digits = m_out.add(tens, m_out.shl(m_out.sub(pairs, m_out.mul(tens, constant(10))), m_out.constInt32(8)));
    PatchpointValue* trailingZeros = m_out.patchpoint(Int64);
    trailingZeros->append(ConstrainedValue(digits, ValueRep::SomeRegister));
    trailingZeros->effects = Effects::none();
    trailingZeros->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.countTrailingZeros64WithoutNullCheck(params[1].gpr(), params[0].gpr());
    });
    LValue leadingZeroBits = m_out.bitAnd(trailingZeros, constant(0x38));
    ValueFromBlock multiDigitString = m_out.anchor(m_out.lShr(m_out.add(digits, constant(asciiZeros)), m_out.castToInt32(leadingZeroBits)));
    ValueFromBlock multiDigitCount = m_out.anchor(m_out.sub(constant(8), m_out.lShr(leadingZeroBits, m_out.constInt32(3))));
    m_out.jump(store);

    m_out.appendTo(store);
    LValue text = m_out.phi(Int64, oneDigitString, multiDigitString);
    LValue count = m_out.phi(Int64, oneDigitCount, multiDigitCount);
    Integer length = stringBuilderLength(builder);
    m_out.store64(text, m_out.address(m_heaps.AOTTyped_argumentBuffer, m_out.add(stringBuilderBuffer(builder).value(), length.value()), 0));
    setStringBuilderLength(builder, add(length, Integer(count)));
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    flushStringBuilder(builder, convertTo<Boxed>(operand, type));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::copyLatin1Characters(Pointer destination, Pointer source, Integer length)
{
    LBasicBlock shortCopy = m_out.newBlock();
    LBasicBlock wordLoop = m_out.newBlock();
    LBasicBlock lastWord = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    auto from = [&](LValue offset) {
        return TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(source.value(), offset));
    };
    auto to = [&](LValue offset) {
        return TypedPointer(m_heaps.AOTTyped_argumentBuffer, m_out.add(destination.value(), offset));
    };
    ValueFromBlock initialOffset = m_out.anchor(m_out.int64Zero);
    m_out.branch(below(length, integer(8)).value(), unsure(shortCopy), unsure(wordLoop));

    m_out.appendTo(wordLoop);
    LValue offset = m_out.phi(Int64, initialOffset);
    m_out.store64(marked(m_out.load64(from(offset)), IsFixed), to(offset));
    LValue nextOffset = m_out.add(offset, m_out.constInt64(8));
    m_out.addIncomingToPhi(offset, m_out.anchor(nextOffset));
    m_out.branch(m_out.belowOrEqual(m_out.add(nextOffset, m_out.constInt64(8)), length.value()), unsure(wordLoop), unsure(lastWord));

    m_out.appendTo(lastWord);
    LValue lastOffset = m_out.sub(length.value(), m_out.constInt64(8));
    m_out.store64(marked(m_out.load64(from(lastOffset)), IsFixed), to(lastOffset));
    m_out.jump(continuation);

    m_out.appendTo(shortCopy);
    auto copyIfBitSet = [&](int64_t bit, LValue offset, auto&& copy) {
        LBasicBlock isSet = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        m_out.branch(m_out.testNonZero64(length.value(), m_out.constInt64(bit)), unsure(isSet), unsure(next));
        m_out.appendTo(isSet);
        copy(offset);
        m_out.jump(next);
        m_out.appendTo(next);
    };
    copyIfBitSet(4, m_out.int64Zero, [&](LValue offset) {
        m_out.store32(marked(m_out.load32(from(offset)), IsFixed), to(offset));
    });
    copyIfBitSet(2, m_out.bitAnd(length.value(), m_out.constInt64(4)), [&](LValue offset) {
        m_out.store32As16(marked(m_out.load16ZeroExt32(from(offset)), IsFixed), to(offset));
    });
    copyIfBitSet(1, m_out.bitAnd(length.value(), m_out.constInt64(6)), [&](LValue offset) {
        m_out.store32As8(marked(m_out.load8ZeroExt32(from(offset)), IsFixed), to(offset));
    });
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::appendStringToStringBuilder(const StackStringBuilder& builder, CellPointer string)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer impl = fiberOf(stringReceiver(string));
    orElse(hasNoBits(impl, JSString::isRopeInPointer), slowPath);
    orElse(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed), StringImpl::flagIs8Bit()), slowPath);
    Integer length = stringBuilderLength(builder);
    Integer newLength = add(length, zeroExtended(loadField<Word32>(impl, m_heaps.StringImpl_length)));
    orElse(belowOrEqual(newLength, integer(stringBuilderCapacity)), slowPath);
    copyLatin1Characters(Pointer(m_out.add(stringBuilderBuffer(builder).value(), length.value())), loadField<Pointer>(impl, m_heaps.StringImpl_data), sub(newLength, length));
    ensureStillAliveHere(string);
    setStringBuilderLength(builder, newLength);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    flushStringBuilder(builder, box(string));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::appendStringOrNullishToStringBuilder(const StackStringBuilder& builder, Boxed value)
{
    LBasicBlock isString = m_out.newBlock();
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(value).value(), usually(isString), rarely(isNil));
    m_out.appendTo(isString);
    appendStringToStringBuilder(builder, asCell(value));
    m_out.jump(continuation);
    m_out.appendTo(isNil);
    flushStringBuilder(builder, value);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::appendToStringBuilder(const StackStringBuilder& builder, std::span<const ValueID> operands, bool isFinal)
{
    unsigned uncheckedLength = 0;
    for (ValueID operand : operands) {
        bool isNumber = typeOf(operand).primitives & Primitive::Number;
        auto text = isNumber ? std::nullopt : constantASCIIString(operand);
        if (!isNumber && (!text || text->size() > maxStringBuilderInlineConstantLength)) {
            if (typeOf(operand).isString())
                appendStringToStringBuilder(builder, in<CellPointer>(operand));
            else
                appendStringOrNullishToStringBuilder(builder, in<Boxed>(operand));
            uncheckedLength = 0;
            continue;
        }
        unsigned maxLength = isNumber ? maxStringBuilderInlineDigits : text->size();
        if (uncheckedLength + maxLength > maxStringBuilderAppendRun) {
            flushStringBuilderIfOverCapacity(builder);
            uncheckedLength = 0;
        }
        uncheckedLength += maxLength;
        if (isNumber)
            appendNumberToStringBuilder(builder, operand);
        else
            appendConstantToStringBuilder(builder, *text);
    }
    if (uncheckedLength && !isFinal)
        flushStringBuilderIfOverCapacity(builder);
}

CellPointer Lowering::finishStringBuilder(const StackStringBuilder& builder, bool mayBeEmpty, bool staysLive)
{
    auto finish = [&] {
        LBasicBlock isWhole = m_out.newBlock();
        LBasicBlock hasMore = m_out.newBlock();
        Join join(m_out, Rep::Cell, Int64);
        LValue whole = m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), builder.whole);
        Integer lengthOfWhole(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), builder.lengthOfWhole));
        m_out.branch(logicalAnd(Word32(m_out.notZero64(whole)), equal(stringBuilderLength(builder), lengthOfWhole)).value(), unsure(isWhole), unsure(hasMore));
        m_out.appendTo(isWhole);
        join.add(CellPointer(whole));
        m_out.appendTo(hasMore);
        if (staysLive) {
            flushStringBuilder(builder, Boxed(m_out.int64Zero));
            LValue prefix = stringBuilderPrefix(builder);
            m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.whole, prefix);
            m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.lengthOfWhole, stringBuilderLength(builder).value());
            join.add(CellPointer(prefix));
            return join.finish().as<CellPointer>();
        }
        join.add(AOT_TYPED_OPERATION(StringBuilderFinish, CellPointer(stringBuilderPrefix(builder)), stringBuilderBuffer(builder), low32(stringBuilderLength(builder))));
        return join.finish().as<CellPointer>();
    };
    if (!mayBeEmpty)
        return finish();
    LBasicBlock isEmpty = newUnlikelyBlock();
    LBasicBlock hasCharacters = m_out.newBlock();
    Join join(m_out, Rep::Cell, Int64);
    m_out.branch(m_out.isZero64(m_out.bitOr(stringBuilderPrefix(builder), stringBuilderLength(builder).value())), unsure(isEmpty), unsure(hasCharacters));
    m_out.appendTo(isEmpty);
    join.add(CellPointer(emptyString()));
    m_out.appendTo(hasCharacters);
    join.add(finish());
    return join.finish().as<CellPointer>();
}

void Lowering::initializeStringBuildersOnEdge(const Target& target)
{
    if (!m_stringBuilderAnalysis.builderCount)
        return;
    auto& parameters = m_function.at(target.block).parameters;
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        ValueID argument = target.arguments[i];
        if (!m_stringBuilderAnalysis.isState(parameters[i]) || m_stringBuilderAnalysis.isState(argument))
            continue;
        auto& builder = stringBuilderFor(parameters[i]);
        resetStringBuilder(builder);
        if (auto text = m_stringBuilderAnalysis.initialString.find(argument.index()); text != m_stringBuilderAnalysis.initialString.end())
            appendConstantToStringBuilder(builder, m_program.string(text->value));
        else {
            CellPointer initial = typeOf(argument).isString() ? in<CellPointer>(argument) : asCell(in<Boxed>(argument));
            appendStringToStringBuilder(builder, initial);
            m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.whole, initial.value());
            m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), builder.lengthOfWhole, stringBuilderLength(builder).value());
        }
    }
}

void Lowering::materializeFinalStringBuilderStates(const Block& block)
{
    if (!m_stringBuilderAnalysis.builderCount)
        return;
    for (ValueID parameter : block.parameters) {
        if (!m_stringBuilderAnalysis.isState(parameter) || isResidentInStringBuilder(parameter))
            continue;
        CellPointer string = finishStringBuilder(stringBuilderFor(parameter), stringBuilderMayBeEmpty(parameter));
        define(parameter, convert(string, m_program.at(m_program.types.string), m_function.at(parameter).rep));
    }
}

std::optional<Operand> Lowering::tryLowerWithStringBuilder(ValueID id, const Value& value)
{
    if (!m_stringBuilderAnalysis.builderCount)
        return std::nullopt;
    if (value.opcode == Opcode::CallBuiltin) {
        if (!value.child(0) || !isResidentInStringBuilder(value.child(0)))
            return std::nullopt;
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::StringLength)
            return std::nullopt;
        auto& builder = stringBuilderFor(value.child(0));
        LValue prefix = stringBuilderPrefix(builder);
        Integer length = stringBuilderLength(builder);
        LBasicBlock hasPrefix = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock withoutPrefix = m_out.anchor(length.value());
        m_out.branch(m_out.isZero64(prefix), usually(continuation), rarely(hasPrefix));
        m_out.appendTo(hasPrefix);
        Integer prefixLength = stringLength(stringReceiver(CellPointer(prefix)));
        Integer unbiasedLimit = integer(JSString::MaxLength - stringBuilderCapacity);
        ValueFromBlock withPrefix = m_out.anchor(add(length, select(greaterThan(prefixLength, unbiasedLimit), unbiasedLimit, prefixLength)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(Integer(m_out.phi(Int64, withoutPrefix, withPrefix)), Type::integer(Range::length()), typeOf(id));
    }
    auto found = m_stringBuilderAnalysis.appendOperands.find(id.index());
    if (found == m_stringBuilderAnalysis.appendOperands.end())
        return std::nullopt;
    auto& builder = stringBuilderFor(id);
    std::span<const ValueID> operands = found->value.span();
    if (isResidentInStringBuilder(operands[0]) && &stringBuilderFor(operands[0]) == &builder)
        operands = operands.subspan(1);
    else
        resetStringBuilder(builder);
    bool isFinal = !isResidentInStringBuilder(id);
    appendToStringBuilder(builder, operands, isFinal);
    if (!isFinal)
        return Operand(value.rep, nullptr);
    return convert(finishStringBuilder(builder, false), typeOf(id), value.rep);
}

std::optional<Operand> Lowering::lowerAsConcatenationPart(ValueID id, const Value& value)
{
    if (auto result = tryLowerWithStringBuilder(id, value))
        return result;
    if ((m_concatenationParts.isEmpty() && !m_stringBuilderAnalysis.builderCount) || (value.opcode != Opcode::Add && value.opcode != Opcode::Strcat && value.opcode != Opcode::ToString))
        return std::nullopt;
    if (isLazilyFetched(id))
        return Operand(value.rep, nullptr);
    auto found = m_concatenationParts.find(id.index());
    if (found == m_concatenationParts.end())
        return std::nullopt;
    return convert(concatenateInOnePiece(found->value), typeOf(id), value.rep);
}

std::optional<Operand> Lowering::tryLookupWithSunkKey(ValueID key, const Type& type, const ScopedLambda<Boxed(Word32, Pointer)>& ask, const ScopedLambda<Operand()>& slowPath)
{
    if (!m_sunkKeyConcatenations.get(key.index()) || m_keyBeingMaterialized == key)
        return std::nullopt;
    auto& parts = m_concatenationParts.find(key.index())->value;
    LValue buffer = m_out.lockedStackSlot(parts.size() * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < parts.size(); ++i) {
        auto& partType = typeOf(parts[i]);
        Operand operand = get(parts[i]);
        Boxed boxed;
        if (partType.isNumber() && operand.rep == Rep::Double) {
            Number number = operand.as<Number>();
            Word32 low = low32(truncateToInteger(number));
            boxed = select(equal(toNumber(low), number), boxInt32(low), box(purified(number)));
        } else if (operand.rep == Rep::OptionalDouble) {
            Number number = assumedNumber(operand.as<OptionalNumber>());
            Word32 low = low32(truncateToInteger(number));
            boxed = select(equal(toNumber(low), number), boxInt32(low), convertTo<Boxed>(operand, partType));
        } else
            boxed = convertTo<Boxed>(operand, partType);
        m_out.store64(boxed.value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    }
    Rep rep = repOf(type);
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    Boxed answer = ask(word32(parts.size()), Pointer(buffer));
    m_out.branch(m_out.notZero64(answer.value()), unsure(hit), unsure(miss));
    m_out.appendTo(hit);
    join.add(ensure(answer, type));
    m_out.appendTo(miss);
    SetForScope keyBeingMaterialized(m_keyBeingMaterialized, key);
    auto& keyType = typeOf(key);
    m_assumptions.append(Assumption { key, convert(concatenateAsRopes(parts.span()), keyType, m_function.at(key).rep), &keyType });
    join.add(slowPath());
    m_assumptions.removeLast();
    return join.finish();
}

CellPointer Lowering::concatenateIfNeeded(const Value& value)
{
    if (value.opcode == Opcode::CallBuiltin) {
        auto* part = substringCreatedBy(ValueID(static_cast<uint32_t>(&value - m_function.values.span().data())));
        RELEASE_ASSERT(part);
        return makeSubstring(part->base, part->lengthOfBase, part->from, part->to);
    }
    if (ValueID id(static_cast<uint32_t>(&value - m_function.values.span().data())); isResidentInStringBuilder(id))
        return finishStringBuilder(stringBuilderFor(id), stringBuilderMayBeEmpty(id), true);
    if (size_t index = &value - m_function.values.span().data(); m_sunkKeyConcatenations.get(index))
        return concatenateInOnePiece(m_concatenationParts.find(index)->value);
    if (value.opcode == Opcode::ToString)
        return toStringCell(value.child(0));
    if (value.opcode == Opcode::Strcat)
        return lowerStrcat(value);
    RELEASE_ASSERT(value.opcode == Opcode::Add);
    if (!m_isInLoop && !m_program.isHotOutsideLoops(m_id)) {
        CellPointer first = toStringCell(value.child(0));
        return concatenateByStub(first, toStringCell(value.child(1)));
    }
    StringPart first = stringPartOf(value.child(0));
    return concatenate(first, stringPartOf(value.child(1))).string;
}

CellPointer Lowering::concatenateInOnePiece(const ConcatenationParts& parts)
{
    static constexpr int64_t maxLength = 64;
    static constexpr int64_t maxInt32StringLength = 11;
    int64_t room = maxLength;
    unsigned numberOfNumbers = 0;
    ValueID lastNumber;
    bool hasConstant = false;
    bool hasOtherStrings = false;
    for (ValueID part : parts) {
        if (isNumberOrNullish(typeOf(part))) {
            room -= maxInt32StringLength;
            ++numberOfNumbers;
            lastNumber = part;
        } else if (auto text = constantASCIIString(part)) {
            room -= text->size();
            hasConstant |= !text->empty();
        } else
            hasOtherStrings = true;
    }
    if (room < (hasOtherStrings ? 8 : 0))
        return concatenateAsRopes(parts.span());

    LBasicBlock asRopes = hasOtherStrings ? newUnlikelyBlock() : nullptr;
    Join join(m_out, Rep::Cell, Int64);
    auto require = [&](Word32 condition) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition.value(), unsure(next), unsure(asRopes));
        m_out.appendTo(next);
    };
    Vector<Boxed, 8> passed;
    Integer otherStringsLength;
    Word32 flags;
    for (ValueID part : parts) {
        auto& type = typeOf(part);
        Operand operand = get(part);
        if (isNumberOrNullish(type)) {
            if (operand.rep == Rep::Double) {
                Number number = operand.as<Number>();
                Word32 low = low32(truncateToInteger(number));
                passed.append(select(equal(toNumber(low), number), boxInt32(low), box(purified(number))));
            } else if (operand.rep == Rep::OptionalDouble) {
                Number number = assumedNumber(operand.as<OptionalNumber>());
                Word32 low = low32(truncateToInteger(number));
                passed.append(select(equal(toNumber(low), number), boxInt32(low), convertTo<Boxed>(operand, type)));
            } else
                passed.append(convertTo<Boxed>(operand, type));
            continue;
        }
        CellPointer string = operand.as<CellPointer>();
        passed.append(box(string));
        if (constantASCIIString(part))
            continue;
        Pointer fiber = fiberOf(stringReceiver(string));
        require(hasNoBits(fiber, JSString::isRopeInPointer));
        Integer length = zeroExtended(loadField<Word32>(fiber, m_heaps.StringImpl_length));
        Word32 implFlags = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
        otherStringsLength = otherStringsLength ? add(otherStringsLength, length) : length;
        flags = flags ? bitAnd(flags, implFlags) : implFlags;
    }
    if (hasOtherStrings) {
        require(logicalAnd(belowOrEqual(otherStringsLength, integer(room)), hasBits(flags, StringImpl::flagIs8Bit())));
        if (numberOfNumbers == 1 && !hasConstant) {
            LBasicBlock isOnlyNumber = m_out.newBlock();
            LBasicBlock next = m_out.newBlock();
            m_out.branch(isZero(otherStringsLength).value(), unsure(isOnlyNumber), unsure(next));
            m_out.appendTo(isOnlyNumber);
            join.add(toStringCell(lastNumber));
            m_out.appendTo(next);
        } else if (!numberOfNumbers && !hasConstant)
            require(logicalNot(isZero(otherStringsLength)));
    }
    LValue buffer = transientBuffer(passed.size() * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < passed.size(); ++i)
        m_out.store64(passed[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    join.add(AOT_TYPED_OPERATION(ConcatenateWithIntegers, word32(passed.size()), Pointer(buffer)));
    if (hasOtherStrings) {
        m_out.appendTo(asRopes);
        join.add(concatenateAsRopes(parts.span()));
    }
    return join.finish().as<CellPointer>();
}

CellPointer Lowering::integerToString(Integer value, const Type& type)
{
    if (!m_isInLoop && !m_program.isHotOutsideLoops(m_id)) {
        auto byStub = [&] {
            return CellPointer(callAllocationStub(Stub::Int32ToString, { { value.value(), isARM64() ? stubTemporaryGPRs[2] : GPRInfo::argumentGPR1 } }, effectsForOperation<operationAOTTypedIntToString>(), false));
        };
        if (type.isInteger() && type.integers.isSubsetOf(Range::int32()))
            return byStub();
        return either<CellPointer>(equal(signExtended(low32(value)), value), byStub, [&] {
            return AOT_TYPED_OPERATION(IntToString, value);
        });
    }
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Cell, Int64);
    Word32 low = low32(value);
    if (!type.isInteger() || !type.integers.isSubsetOf(Range::int32()))
        orElse(equal(signExtended(low), value), slowPath);
    join.add(CellPointer(int32ToString(low.value(), slowPath)));
    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(IntToString, value));
    return join.finish().as<CellPointer>();
}

Word32 Lowering::hasText(Pointer data, std::span<const uint8_t> text)
{
    size_t size = text.size();
    Word32 result;
    auto compare = [&](size_t offset, unsigned width) {
        uint64_t expected = 0;
        for (unsigned i = 0; i < width; ++i)
            expected |= static_cast<uint64_t>(text[offset + i]) << (8 * i);
        TypedPointer pointer = m_out.address(m_heaps.characters8.atAnyIndex(), data.value(), offset);
        Word32 isSame;
        switch (width) {
        case 8:
            isSame = equal(Integer(marked(m_out.load64(pointer), IsFixed)), integer(static_cast<int64_t>(expected)));
            break;
        case 4:
            isSame = equal(Word32(marked(m_out.load32(pointer), IsFixed)), word32(static_cast<int32_t>(expected)));
            break;
        case 2:
            isSame = equal(Word32(marked(m_out.load16ZeroExt32(pointer), IsFixed)), word32(static_cast<int32_t>(expected)));
            break;
        default:
            isSame = equal(Word32(marked(m_out.load8ZeroExt32(pointer), IsFixed)), word32(static_cast<int32_t>(expected)));
            break;
        }
        result = result ? logicalAnd(result, isSame) : isSame;
    };
    unsigned width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
    size_t offset = 0;
    for (; offset + width <= size; offset += width)
        compare(offset, width);
    if (size_t left = size - offset) {
        unsigned last = left > 4 ? 8 : left > 2 ? 4 : left;
        compare(size - last, last);
    }
    return result;
}

Word32 Lowering::hasSubstringAt(const StringReceiver& receiver, Integer place, ValueID constant, std::span<const uint8_t> text)
{
    auto genericPath = [&] {
        return AOT_TYPED_OPERATION(StringHasAt, receiver.string, in<CellPointer>(constant), low32(place));
    };
    if (text.size() > maxInlineStringCompareLength)
        return genericPath();
    Characters characters = charactersOf(receiver);
    return either<Word32>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
        Word32 result = hasText(offsetBy(characters.data, place, 0), text);
        ensureStillAliveHere(characters.owner);
        return result;
    }, genericPath);
}

Word32 Lowering::areBytesEqual(Pointer p, Pointer q, Integer length)
{
    LBasicBlock isShort = m_out.newBlock();
    LBasicBlock hasFour = m_out.newBlock();
    LBasicBlock isBelowFour = m_out.newBlock();
    LBasicBlock hasTwo = m_out.newBlock();
    LBasicBlock isBelowTwo = m_out.newBlock();
    LBasicBlock hasOne = m_out.newBlock();
    LBasicBlock wordLoop = m_out.newBlock();
    LBasicBlock nextWord = m_out.newBlock();
    LBasicBlock lastWord = m_out.newBlock();
    LBasicBlock isSame = m_out.newBlock();
    LBasicBlock isDifferent = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    auto byteAt = [&](Pointer data, LValue index) {
        return marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    auto twoAt = [&](Pointer data, LValue index) {
        return marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    auto fourAt = [&](Pointer data, LValue index) {
        return marked(m_out.load32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    auto areFirstAndLastEqual = [&](auto& at, int64_t width) {
        LValue last = m_out.sub(length.value(), m_out.constInt64(width));
        return Word32(m_out.isZero32(m_out.bitOr(m_out.bitXor(at(p, m_out.int64Zero), at(q, m_out.int64Zero)), m_out.bitXor(at(p, last), at(q, last)))));
    };
    auto wordAt = [&](Pointer data, LValue index) {
        return marked(m_out.load64(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    ValueFromBlock firstWord = m_out.anchor(m_out.int64Zero);
    m_out.branch(below(length, integer(8)).value(), unsure(isShort), unsure(wordLoop));

    m_out.appendTo(isShort);
    m_out.branch(below(length, integer(4)).value(), unsure(isBelowFour), unsure(hasFour));
    m_out.appendTo(hasFour);
    join.add(areFirstAndLastEqual(fourAt, 4));
    m_out.appendTo(isBelowFour);
    m_out.branch(below(length, integer(2)).value(), unsure(isBelowTwo), unsure(hasTwo));
    m_out.appendTo(hasTwo);
    join.add(areFirstAndLastEqual(twoAt, 2));
    m_out.appendTo(isBelowTwo);
    m_out.branch(isZero(length).value(), rarely(isSame), usually(hasOne));
    m_out.appendTo(hasOne);
    join.add(Word32(m_out.equal(byteAt(p, m_out.int64Zero), byteAt(q, m_out.int64Zero))));

    m_out.appendTo(wordLoop);
    LValue wordIndex = m_out.phi(Int64, firstWord);
    m_out.branch(m_out.equal(wordAt(p, wordIndex), wordAt(q, wordIndex)), unsure(nextWord), unsure(isDifferent));
    m_out.appendTo(nextWord);
    LValue nextWordIndex = m_out.add(wordIndex, m_out.constInt64(8));
    m_out.addIncomingToPhi(wordIndex, m_out.anchor(nextWordIndex));
    m_out.branch(m_out.belowOrEqual(m_out.add(nextWordIndex, m_out.constInt64(8)), length.value()), unsure(wordLoop), unsure(lastWord));
    m_out.appendTo(lastWord);
    LValue lastIndex = m_out.sub(length.value(), m_out.constInt64(8));
    join.add(Word32(m_out.equal(wordAt(p, lastIndex), wordAt(q, lastIndex))));

    m_out.appendTo(isSame);
    join.add(boolean(true));
    m_out.appendTo(isDifferent);
    join.add(boolean(false));
    return join.finish().as<Word32>();
}

Word32 Lowering::equalsLiteralByStub(CellPointer string, ValueID constant)
{
    constant = stringConstantOf(constant);
    RELEASE_ASSERT(constant);
    LValue number = m_out.constInt64(m_function.at(constant).string().index());
    return Word32(m_out.castToInt32(callAllocationStub(Stub::StringEqualsLiteral, { { number, stubTemporaryGPRs[0] }, { string.value(), stubTemporaryGPRs[2] } }, { Writes::Nothing, false }, false)));
}

void Lowering::branchOnLiteralByOneCall(ValueID subject, ValueID constant, std::span<const uint8_t> text, LBasicBlock isText, LBasicBlock isOther)
{
    Operand operand = get(subject);
    auto& type = typeOf(subject);
    bool isString = operand.rep == Rep::Cell && type.hasOnlyPrimitives(Primitive::String);
    branchOnLiteralByOneCall(operand.rep == Rep::Cell ? operand.as<CellPointer>().value() : operand.as<Boxed>().value(), isString, constant, text, isText, isOther);
}

void Lowering::branchOnLiteralByOneCall(LValue value, bool isString, ValueID constant, std::span<const uint8_t> text, LBasicBlock isText, LBasicBlock isOther)
{
    if (text.size() > maxRegisterStringLength) {
        constant = stringConstantOf(constant);
        RELEASE_ASSERT(constant);
        LValue number = m_out.constInt64(m_function.at(constant).string().index());
        LValue isSame = callAllocationStub(isString ? Stub::StringEqualsLiteral : Stub::ValueEqualsLiteral, { { number, stubTemporaryGPRs[0] }, { value, stubTemporaryGPRs[2] } }, { Writes::Nothing, false }, false);
        m_out.branch(m_out.castToInt32(isSame), unsure(isText), unsure(isOther));
        return;
    }
    Stub stub = static_cast<Stub>(static_cast<unsigned>(Stub::ValueEqualsLiteralOfLength1) + 4 * (text.size() - 1) + (isString ? 2 : 0));
    uint64_t packed = packStringIntoRegister(text);
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, { Writes::Nothing, false });
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(value, ValueRep::reg(stubTemporaryGPRs[2])));
    RegisterSet changed = *registersClobberedBy(stub);
    patchpoint->clobberLate(changed);
    patchpoint->clobberEarly(changed);
    m_out.appendSuccessor(unsure(isOther));
    m_out.appendSuccessor(unsure(isText));
    patchpoint->setGenerator([context = &m_context, stub, packed](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool hasNoFrame = context->hasNoFrame(params.proc().code());
        bool isLeaf = hasFramelessFunctionStubs && hasNoFrame;
        if (packed <= UINT32_MAX)
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(packed)), stubTemporaryGPRs[0]);
        else
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(packed)), stubTemporaryGPRs[0]);
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        context->stubCalls.call(jit, isLeaf ? static_cast<Stub>(static_cast<unsigned>(stub) + 1) : stub, CallSite { });
        bool leavesIfSame = params.fallsThroughToSuccessor(0);
#if CPU(X86_64)
        CCallHelpers::Jump leaves = CCallHelpers::Jump(jit.m_assembler.jCC(leavesIfSame ? X86Assembler::ConditionE : X86Assembler::ConditionNE));
#else
        CCallHelpers::Jump leaves = jit.branchOnFlags(leavesIfSame ? CCallHelpers::Equal : CCallHelpers::NotEqual);
#endif
        CCallHelpers::Jump isSame;
        if (!leavesIfSame && !params.fallsThroughToSuccessor(1))
            isSame = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            leaves.linkTo(*labels[leavesIfSame], &jit);
            if (isSame.isSet())
                isSame.linkTo(*labels[1], &jit);
        });
    });
}

Lowering::Characters Lowering::charactersOfStringWithLength(const StringReceiver& receiver, size_t length, LBasicBlock isNotAsLong, LBasicBlock otherwise)
{
    auto look = [&](Word32 found, bool isCold) {
        LBasicBlock isAsLong = isCold ? newColdBlock() : m_out.newBlock();
        m_out.branch(equal(found, word32(static_cast<uint32_t>(length))).value(), unsure(isAsLong), unsure(isNotAsLong));
        m_out.appendTo(isAsLong);
    };
    if (auto characters = openedStringCharacters(receiver)) {
        look(low32(characters->length), false);
        return *characters;
    }
    LBasicBlock isFlat = m_out.newBlock();
    LBasicBlock isRope = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer fiber = fiberOf(receiver);
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(isRope));
    m_out.appendTo(isFlat);
    look(loadField<Word32>(fiber, m_heaps.StringImpl_length), false);
    Pointer data = loadField<Pointer>(fiber, m_heaps.StringImpl_data);
    Word32 flags = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
    m_out.jump(continuation);

    m_out.appendTo(isRope);
    look(loadField<Word32>(receiver.string, m_heaps.JSRopeString_length), true);
    m_out.jump(otherwise);

    m_out.appendTo(continuation);
    return { data, flags, integer(length), receiver.string };
}

Word32 Lowering::equalsLiteralOfLengthByStub(const StringReceiver& receiver, ValueID constant, std::span<const uint8_t> text)
{
    LBasicBlock isNotAsLong = m_out.newBlock();
    LBasicBlock isAsLong = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(isNotAsLong));
        m_out.appendTo(isString);
    }
    if (text.size() > maxInlineComparedLiteralLength) {
        m_out.branch(equal(stringLength(receiver), integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
        m_out.appendTo(isNotAsLong);
        join.add(boolean(false));
        m_out.appendTo(isAsLong);
        join.add(equalsLiteralByStub(receiver.string, constant));
        return join.finish().as<Word32>();
    }
    LBasicBlock byStub = newColdBlock();
    LBasicBlock isText = m_out.newBlock();
    Characters characters = charactersOfStringWithLength(receiver, text.size(), isNotAsLong, byStub);
    m_out.jump(isAsLong);
    m_out.appendTo(isNotAsLong);
    join.add(boolean(false));
    m_out.appendTo(isAsLong);
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), byStub);
    Word32 isSame = hasText(characters.data, text);
    ensureStillAliveHere(characters.owner);
    m_out.branch(isSame.value(), unsure(isText), unsure(isNotAsLong));
    m_out.appendTo(isText);
    join.add(boolean(true));
    m_out.appendTo(byStub);
    branchOnLiteralByOneCall(receiver.string.value(), true, constant, text, isText, isNotAsLong);
    return join.finish().as<Word32>();
}

Word32 Lowering::isSubstringEqualTo(const Substring& part, ValueID constant, std::span<const uint8_t> text)
{
    Integer length = lengthOf(part);
    if (text.empty())
        return isZero(length);
    LBasicBlock isAsLong = m_out.newBlock();
    LBasicBlock isNotAsLong = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    m_out.branch(equal(length, integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
    m_out.appendTo(isAsLong);
    join.add(hasSubstringAt(part.base, part.from, constant, text));
    m_out.appendTo(isNotAsLong);
    join.add(boolean(false));
    return join.finish().as<Word32>();
}

std::optional<Word32> Lowering::isOptionalSubstringEqualTo(ValueID id, ValueID constant)
{
    auto found = m_optionalSubstrings.find(id.index());
    if (found == m_optionalSubstrings.end() || !m_substringsOnlyInspected.get(id.index()))
        return std::nullopt;
    Substring part = found->value;
    if (m_function.at(constant).opcode == Opcode::Undefined)
        return logicalNot(part.isPresent);
    if (m_function.at(constant).opcode == Opcode::Null)
        return boolean(false);
    auto text = constantASCIIString(constant);
    RELEASE_ASSERT(text);
    return either<Word32>(part.isPresent, [&] {
        return isSubstringEqualTo(part, constant, *text);
    }, [&] {
        return boolean(false);
    });
}

Word32 Lowering::isStringEqualTo(ValueID id, ValueID constant, std::span<const uint8_t> text)
{
    if (auto* part = substringCreatedBy(id))
        return isSubstringEqualTo(*part, constant, text);
    Boxed value = in<Boxed>(id);
    if (text.empty())
        return equal(value, box(CellPointer(emptyString())));
    if (text.size() == 1) {
        if (auto codeUnit = stringCodeUnit(id))
            return equal(*codeUnit, integer(text[0]));
    }
    if (!m_isInLoop) {
        StringReceiver receiver = stringReceiver(id);
        if (openedStringCharacters(receiver) || typeOf(id).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
            return equalsLiteralOfLengthByStub(receiver, constant, text);
        LBasicBlock isText = m_out.newBlock();
        LBasicBlock isOther = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnLiteralByOneCall(id, constant, text, isText, isOther);
        m_out.appendTo(isText);
        join.add(boolean(true));
        m_out.appendTo(isOther);
        join.add(boolean(false));
        return join.finish().as<Word32>();
    }
    LBasicBlock isAsLong = m_out.newBlock();
    LBasicBlock isNotAsLong = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    StringReceiver receiver = stringReceiver(id);
    if (text.size() > maxInlineStringCompareLength) {
        LBasicBlock isSameCell = m_out.newBlock();
        LBasicBlock isOtherCell = m_out.newBlock();
        m_out.branch(equal(value, in<Boxed>(constant)).value(), unsure(isSameCell), unsure(isOtherCell));
        m_out.appendTo(isSameCell);
        join.add(boolean(true));
        m_out.appendTo(isOtherCell);
    }
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(isNotAsLong));
        m_out.appendTo(isString);
    }
    m_out.branch(equal(stringLength(receiver), integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
    m_out.appendTo(isNotAsLong);
    join.add(boolean(false));
    m_out.appendTo(isAsLong);
    if (text.size() > maxInlineStringCompareLength) {
        join.add(hasSubstringAt(receiver, integer(0), constant, text));
        return join.finish().as<Word32>();
    }
    Characters characters = charactersOf(receiver);
    join.add(either<Word32>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
        Word32 result = hasText(characters.data, text);
        ensureStillAliveHere(characters.owner);
        return result;
    }, [&] {
        return Word32(AOT_TYPED_OPERATION(StringEqualsLiteral, receiver.string, word32(m_function.at(stringConstantOf(constant)).string().index())));
    }));
    return join.finish().as<Word32>();
}

unsigned Lowering::numberOfPredecessorsOf(BlockID id)
{
    if (m_numberOfPredecessors.isEmpty()) {
        m_numberOfPredecessors.fill(0, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++m_numberOfPredecessors[target.block.index()];
        }
    }
    return m_numberOfPredecessors[id.index()];
}

std::optional<Lowering::ComparisonWithLiteral> Lowering::comparisonWithLiteralBranchedOn(const Block& block)
{
    auto& terminator = block.terminator;
    if (terminator.kind != TerminatorKind::Branch || !block.values.contains(terminator.value) || useCountOf(terminator.value) != 1)
        return std::nullopt;
    auto& condition = m_function.at(terminator.value);
    constexpr uint16_t either = Primitive::True | Primitive::False;
    if (condition.opcode != Opcode::StrictEq || (m_program.at(condition.type).primitives & either) != either)
        return std::nullopt;
    auto isOneCodeUnitBeyondLatin1 = [&](ValueID id) {
        if (m_function.at(id).opcode != Opcode::String)
            return false;
        auto bytes = m_program.string(m_function.at(id).string());
        return (bytes.size() == 2 && bytes[0] >= 0xc4 && bytes[0] < 0xe0) || (bytes.size() == 3 && bytes[0] >= 0xe0 && bytes[0] < 0xf0);
    };
    for (unsigned i = 0; i < 2; ++i) {
        ValueID constant = condition.child(i);
        ValueID subject = condition.child(1 - i);
        if (m_function.at(subject).rep == Rep::LiteralString) {
            if (m_function.at(constant).opcode == Opcode::String && m_program.literalStringIndex(m_function.at(constant).string()) && !m_pinnedReads.contains(subject.index()))
                return ComparisonWithLiteral { subject, constant, { } };
            continue;
        }
        auto text = constantASCIIString(constant);
        if (text && text->empty())
            continue;
        if (!text && isOneCodeUnitBeyondLatin1(constant))
            text = std::span<const uint8_t> { };
        if (!text || text->size() > maxInlineStringCompareLength || m_function.at(constant).opcode != Opcode::String || !isLazilyFetched(constant))
            continue;
        if (m_pinnedReads.contains(subject.index()))
            continue;
        if (substringCreatedBy(subject) || m_optionalSubstrings.contains(subject.index()))
            continue;
        auto& type = m_program.at(m_function.at(subject).type);
        if (!(type.primitives & Primitive::String) || !holdsCells(m_function.at(subject).rep))
            continue;
        return ComparisonWithLiteral { subject, constant, *text };
    }
    return std::nullopt;
}

std::optional<Operand> Lowering::deferToBranchOnLiteral(ValueID id, const Value& value)
{
    if (value.opcode != Opcode::StrictEq || m_currentBlock->terminator.value != id || m_firstRepeatedShortStringComparisons.contains(id.index()))
        return std::nullopt;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return std::nullopt;
    auto comparison = comparisonWithLiteralBranchedOn(*m_currentBlock);
    if (!comparison || successorsOf(m_currentBlock->terminator).size() != 2)
        return std::nullopt;
    if (constantCodeUnit(comparison->constant) && stringCodeUnit(comparison->subject))
        return std::nullopt;
    m_isLazilyFetched.set(id.index());
    return Operand(value.rep, nullptr);
}

void Lowering::lowerBranchOnLiteral()
{
    struct Case {
        std::span<const uint8_t> text;
        ValueID constant;
        LBasicBlock taken;
    };
    Vector<Case, 8> cases;
    SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
    const Block* block = m_currentBlock;
    ValueID subject = comparisonWithLiteralBranchedOn(*block)->subject;
    bool flattensRopes = m_isInLoop;
    for (;;) {
        auto comparison = *comparisonWithLiteralBranchedOn(*block);
        if (block != m_currentBlock) {
            openStringsBeforeLoopsEnteredBy(block->terminator);
            convertNumberArraysBeforeLoops(block->terminator, true);
            convertNumberArraysBeforeLoops(block->terminator, false);
        }
        cases.append(Case { comparison.text, comparison.constant, edgeTo(block->terminator.targets[0], true) });
        auto& next = block->terminator.targets[1];
        auto& following = m_function.at(next.block);
        if (!next.arguments.isEmpty() || !following.parameters.isEmpty() || numberOfPredecessorsOf(next.block) != 1 || cases.size() == 64)
            break;
        auto another = comparisonWithLiteralBranchedOn(following);
        if (!another || stringDefinedBy(another->subject) != stringDefinedBy(subject))
            break;
        bool onlyCompares = true;
        for (ValueID id : following.values)
            onlyCompares &= id == following.terminator.value || (m_function.at(id).opcode == Opcode::String && (isLazilyFetched(id) || m_function.at(id).rep == Rep::LiteralString));
        if (!onlyCompares)
            break;
        m_blocksTakenIntoBranchOnLiteral.set(next.block.index());
        for (ValueID id : following.values) {
            if (id == following.terminator.value || m_function.at(id).rep != Rep::LiteralString)
                continue;
            bool isUsedLater = isUsedByTerminator(id);
            for (ValueID user : usersOf(id))
                isUsedLater |= user != following.terminator.value;
            if (isUsedLater)
                define(id, literalStringConstant(m_function.at(id).string()));
        }
        block = &following;
    }
    LBasicBlock otherwise = edgeTo(block->terminator.targets[1], true);

    if (m_function.at(subject).rep == Rep::LiteralString) {
        LValue word = rawWordOf(get(subject).as<LiteralStringWord>());
        bool usesNumber = cases.size() >= 128;
        if (usesNumber)
            word = m_out.lShr(m_out.sub(word, m_out.constInt64(emptyLiteralStringWord)), m_out.constInt32(literalStringShift));
        Vector<FTL::SwitchCase, 8> byWord;
        UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
        for (auto& one : cases) {
            uint32_t number = *m_program.literalStringIndex(m_function.at(one.constant).string());
            if (seen.add(number).isNewEntry)
                byWord.append(FTL::SwitchCase(m_out.constInt64(usesNumber ? number : literalStringWord(number)), one.taken, FTL::Weight()));
        }
        if (byWord.size() == 1)
            m_out.branch(m_out.equal(word, byWord[0].value()), unsure(cases[0].taken), unsure(otherwise));
        else
            m_out.switchInstruction(word, byWord, otherwise, FTL::Weight());
        return;
    }

    std::optional<SetForScope<double>> frequencyWithoutCodeUnit;
    if (auto codeUnit = stringCodeUnit(subject, true)) {
        Vector<FTL::SwitchCase, 8> byCodeUnit;
        BitVector visitedUnits;
        bool areAllOneCodeUnit = true;
        for (auto& one : cases) {
            auto unit = constantCodeUnit(one.constant);
            areAllOneCodeUnit &= !!unit;
            if (unit && !visitedUnits.set(*unit))
                byCodeUnit.append(FTL::SwitchCase(m_out.constInt64(*unit), one.taken, FTL::Weight()));
        }
        if (areAllOneCodeUnit) {
            LBasicBlock isKept = m_out.newBlock();
            LBasicBlock isNotRetained = newColdBlock();
            m_out.branch(notEqual(*codeUnit, integer(codeUnitIsUntracked)).value(), usually(isKept), rarely(isNotRetained));
            m_out.appendTo(isKept);
            m_out.switchInstruction(codeUnit->value(), byCodeUnit, otherwise, FTL::Weight());
            m_out.appendTo(isNotRetained);
            frequencyWithoutCodeUnit.emplace(m_out.m_frequency, 0);
        }
    }

    StringReceiver receiver = stringReceiver(subject);
    if (!flattensRopes && cases.size() <= maxLiteralsComparedByCalls && !openedStringCharacters(receiver) && !typeOf(subject).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull) && !cases.containsIf([](auto& one) { return one.text.empty(); })) {
        for (unsigned i = 0; i < cases.size(); ++i) {
            LBasicBlock next = i + 1 < cases.size() ? m_out.newBlock() : otherwise;
            branchOnLiteralByOneCall(subject, cases[i].constant, cases[i].text, cases[i].taken, next);
            if (i + 1 < cases.size())
                m_out.appendTo(next);
        }
        return;
    }
    bool mayBeNonString = !typeOf(subject).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    if (mayBeNonString) {
        receiver.flags = 0;
        if (!receiver.isString && get(subject).rep != Rep::Cell)
            receiver.isString = isCell(get(subject).as<Boxed>(), typeOf(subject));
    }
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(otherwise));
        m_out.appendTo(isString);
    }
    if (mayBeNonString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(equal(loadCellType(receiver.string), word32(StringType)).value(), unsure(isString), unsure(otherwise));
        m_out.appendTo(isString);
    }
    auto lengthOf = [](const Case& one) -> size_t {
        return one.text.empty() ? 1 : one.text.size();
    };
    if (prefersGenericPathInColdCode()) {
        if (cases.size() == 1 && !cases[0].text.empty() && !openedString(receiver.id)) {
            LBasicBlock isFlat = m_out.newBlock();
            LBasicBlock mayBeString = m_out.newBlock();
            Pointer fiber = fiberOf(receiver);
            m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(mayBeString));
            m_out.appendTo(isFlat);
            m_out.branch(equal(loadField<Word32>(fiber, m_heaps.StringImpl_length), word32(static_cast<uint32_t>(cases[0].text.size()))).value(), unsure(mayBeString), unsure(otherwise));
            m_out.appendTo(mayBeString);
            branchOnLiteralByOneCall(receiver.string.value(), true, cases[0].constant, cases[0].text, cases[0].taken, otherwise);
            return;
        }
        Integer subjectLength = stringLength(receiver);
        for (unsigned i = 0; i < cases.size(); ++i) {
            LBasicBlock next = i + 1 < cases.size() ? m_out.newBlock() : otherwise;
            LBasicBlock isAsLong = m_out.newBlock();
            m_out.branch(equal(subjectLength, integer(lengthOf(cases[i]))).value(), unsure(isAsLong), unsure(next));
            m_out.appendTo(isAsLong);
            if (cases[i].text.empty())
                m_out.branch(Word32(AOT_TYPED_OPERATION(StringEq, receiver.string, in<CellPointer>(cases[i].constant))).value(), unsure(cases[i].taken), unsure(next));
            else
                branchOnLiteralByOneCall(receiver.string.value(), true, cases[i].constant, cases[i].text, cases[i].taken, next);
            if (i + 1 < cases.size())
                m_out.appendTo(next);
        }
        return;
    }
    Vector<size_t, 8> lengths;
    for (auto& one : cases) {
        if (!lengths.contains(lengthOf(one)))
            lengths.append(lengthOf(one));
    }
    bool checksLengthFirst = !flattensRopes && lengths.size() == 1;
    LBasicBlock wide = newColdBlock();
    auto flatStringCharacters = [&]() -> Characters {
        if (auto opened = openedStringCharacters(receiver))
            return *opened;
        LBasicBlock isFlat = m_out.newBlock();
        Pointer fiber = fiberOf(receiver);
        m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(wide));
        m_out.appendTo(isFlat);
        Pointer data = loadField<Pointer>(fiber, m_heaps.StringImpl_data);
        Word32 flags = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
        return { data, flags, zeroExtended(loadField<Word32>(fiber, m_heaps.StringImpl_length)), receiver.string };
    };
    Characters characters = flattensRopes ? charactersOf(receiver) : checksLengthFirst ? charactersOfStringWithLength(receiver, lengths[0], otherwise, wide) : flatStringCharacters();
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), wide);

    bool returnsNarrow = !flattensRopes && cases.size() > 1;
    for (auto& one : cases)
        returnsNarrow &= !one.text.empty();
    LBasicBlock narrow = nullptr;
    LValue selectedData = nullptr;
    LValue selectedLength = nullptr;
    if (returnsNarrow) {
        narrow = m_out.newBlock();
        ValueFromBlock flatData = m_out.anchor(characters.data.value());
        ValueFromBlock flatStringLength = m_out.anchor(characters.length.value());
        m_out.jump(narrow);
        m_out.appendTo(narrow);
        selectedData = m_out.phi(Int64, flatData);
        selectedLength = m_out.phi(Int64, flatStringLength);
        characters.data = Pointer(selectedData);
        characters.length = Integer(selectedLength);
    }

    Vector<LBasicBlock, 8> blocksByLength;
    Vector<FTL::SwitchCase, 8> byLength;
    for (size_t length : lengths) {
        blocksByLength.append(m_out.newBlock());
        byLength.append(FTL::SwitchCase(m_out.constInt64(length), blocksByLength.last(), FTL::Weight()));
    }
    if (checksLengthFirst)
        m_out.jump(blocksByLength[0]);
    else if (lengths.size() == 1)
        m_out.branch(equal(characters.length, integer(lengths[0])).value(), unsure(blocksByLength[0]), unsure(otherwise));
    else
        m_out.switchInstruction(characters.length.value(), byLength, otherwise, FTL::Weight());
    for (unsigned i = 0; i < lengths.size(); ++i) {
        m_out.appendTo(blocksByLength[i]);
        unsigned left = 0;
        for (auto& one : cases)
            left += one.text.size() == lengths[i];
        if (!left)
            m_out.jump(otherwise);
        using Group = Vector<const Case*, 8>;
        auto compareSequentially = [&](const Group& group) {
            unsigned left = group.size();
            for (auto* one : group) {
                Word32 isSame = hasText(characters.data, one->text);
                ensureStillAliveHere(characters.owner);
                LBasicBlock next = --left ? m_out.newBlock() : otherwise;
                m_out.branch(isSame.value(), unsure(one->taken), unsure(next));
                if (left)
                    m_out.appendTo(next);
            }
        };
        Group group;
        for (auto& one : cases) {
            if (one.text.size() == lengths[i])
                group.append(&one);
        }
        constexpr unsigned minStringsForWordDispatch = 3;
        unsigned width = lengths[i] >= 4 ? 4 : lengths[i] >= 2 ? 2 : 0;
        auto wordAt = [&](const Case& one, size_t offset) {
            uint32_t word = 0;
            for (unsigned k = 0; k < width; ++k)
                word |= static_cast<uint32_t>(one.text[offset + k]) << (8 * k);
            return word;
        };
        size_t place = 0;
        unsigned maxWords = 0;
        if (width && group.size() >= minStringsForWordDispatch) {
            for (size_t offset = 0; offset + width <= lengths[i]; ++offset) {
                Vector<uint32_t, 8> words;
                for (auto* one : group) {
                    if (!words.contains(wordAt(*one, offset)))
                        words.append(wordAt(*one, offset));
                }
                if (words.size() > maxWords) {
                    maxWords = words.size();
                    place = offset;
                }
            }
        }
        if (maxWords < 2) {
            compareSequentially(group);
            continue;
        }
        TypedPointer pointer = m_out.address(m_heaps.characters8.atAnyIndex(), characters.data.value(), place);
        LValue word = m_out.zeroExtPtr(marked(width == 4 ? m_out.load32(pointer) : m_out.load16ZeroExt32(pointer), IsFixed));
        ensureStillAliveHere(characters.owner);
        Vector<FTL::SwitchCase, 8> byWord;
        Vector<std::tuple<uint32_t, LBasicBlock, Group>, 8> leaves;
        for (auto* one : group) {
            uint32_t has = wordAt(*one, place);
            size_t at = leaves.findIf([&](auto& leaf) {
                return std::get<0>(leaf) == has;
            });
            if (at == notFound) {
                at = leaves.size();
                leaves.append({ has, m_out.newBlock(), Group { } });
                byWord.append(FTL::SwitchCase(m_out.constInt64(has), std::get<1>(leaves.last()), FTL::Weight()));
            }
            std::get<2>(leaves[at]).append(one);
        }
        m_out.switchInstruction(word, byWord, otherwise, FTL::Weight());
        for (auto& [has, block, texts] : leaves) {
            m_out.appendTo(block);
            compareSequentially(texts);
        }
    }

    m_out.appendTo(wide);
    if (returnsNarrow) {
        LBasicBlock hasCharacters = newColdBlock();
        Pointer copied = AOT_TYPED_OPERATION(CharactersToCompareWithLiterals, receiver.string);
        m_out.branch(m_out.notNull(copied.value()), unsure(hasCharacters), unsure(otherwise));
        m_out.appendTo(hasCharacters);
        m_out.addIncomingToPhi(selectedData, m_out.anchor(copied.value()));
        m_out.addIncomingToPhi(selectedLength, m_out.anchor((checksLengthFirst ? integer(lengths[0]) : stringLength(receiver)).value()));
        m_out.jump(narrow);
        return;
    }
    if (!flattensRopes) {
        Integer length;
        if (!checksLengthFirst)
            length = stringLength(receiver);
        for (unsigned i = 0; i < cases.size(); ++i) {
            LBasicBlock next = i + 1 < cases.size() ? newColdBlock() : otherwise;
            if (!checksLengthFirst) {
                LBasicBlock isAsLong = newColdBlock();
                m_out.branch(equal(length, integer(lengthOf(cases[i]))).value(), unsure(isAsLong), unsure(next));
                m_out.appendTo(isAsLong);
            }
            if (cases[i].text.empty())
                m_out.branch(Word32(AOT_TYPED_OPERATION(StringEq, receiver.string, in<CellPointer>(cases[i].constant))).value(), unsure(cases[i].taken), unsure(next));
            else
                branchOnLiteralByOneCall(receiver.string.value(), true, cases[i].constant, cases[i].text, cases[i].taken, next);
            if (i + 1 < cases.size())
                m_out.appendTo(next);
        }
        return;
    }
    for (unsigned i = 0; i < cases.size(); ++i) {
        LBasicBlock isAsLong = newColdBlock();
        LBasicBlock next = i + 1 < cases.size() ? newColdBlock() : otherwise;
        m_out.branch(equal(characters.length, integer(lengthOf(cases[i]))).value(), unsure(isAsLong), unsure(next));
        m_out.appendTo(isAsLong);
        Word32 isSame = cases[i].text.empty() ? Word32(AOT_TYPED_OPERATION(StringHasAt, receiver.string, in<CellPointer>(cases[i].constant), word32(0))) : Word32(AOT_TYPED_OPERATION(StringEqualsLiteral, receiver.string, word32(m_function.at(stringConstantOf(cases[i].constant)).string().index())));
        m_out.branch(isSame.value(), unsure(cases[i].taken), unsure(next));
        if (i + 1 < cases.size())
            m_out.appendTo(next);
    }
}

Word32 Lowering::areStringsEqual(ValueID a, ValueID b)
{
    if (auto text = constantASCIIString(b))
        return isStringEqualTo(a, b, *text);
    if (auto text = constantASCIIString(a))
        return isStringEqualTo(b, a, *text);
    if (auto first = stringCodeUnit(a)) {
        if (auto second = stringCodeUnit(b))
            return equal(*first, *second);
    }

    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    StringReceiver first = stringReceiver(a);
    StringReceiver second = stringReceiver(b);
    Join join(m_out, Rep::Int32, Int32);
    auto when = [&](Word32 condition, Word32 answer) {
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition.value(), unsure(holds), unsure(next));
        m_out.appendTo(holds);
        join.add(answer);
        m_out.appendTo(next);
    };
    when(equal(x, y), boolean(true));
    for (auto* receiver : { &first, &second }) {
        if (receiver->isString)
            when(logicalNot(receiver->isString), boolean(false));
    }
    Integer length = stringLength(first);
    when(notEqual(length, stringLength(second)), boolean(false));
    if (!m_isInLoop) {
        join.add(Word32(m_out.castToInt32(callAllocationStub(Stub::StringsAreEqual, { { first.string.value(), stubTemporaryGPRs[2] }, { second.string.value(), stubTemporaryGPRs[3] } }, { Writes::Nothing, false }, false))));
        return join.finish().as<Word32>();
    }
    Characters p = charactersOf(first);
    Characters q = charactersOf(second);
    when(equal(p.data, q.data), boolean(true));
    Word32 common = bitAnd(p.flags, q.flags);
    when(hasBits(common, StringImpl::flagIsAtom()), boolean(false));
    LBasicBlock slowPath = newColdBlock();
    orElse(hasBits(common, StringImpl::flagIs8Bit()), slowPath);

    join.add(areBytesEqual(p.data, q.data, length));
    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(StringEq, first.string, second.string));
    Word32 result = join.finish().as<Word32>();
    ensureStillAliveHere(p.owner);
    ensureStillAliveHere(q.owner);
    return result;
}

Boxed Lowering::mapKey(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep == Rep::OptionalDouble)
        return select(equal(assumedNumber(operand.as<OptionalNumber>()), number(0)), boxInt32(word32(0)), convertTo<Boxed>(operand, typeOf(id)));
    if (operand.rep != Rep::Double)
        return convertTo<Boxed>(operand, typeOf(id));
    Number key = operand.as<Number>();
    return select(equal(key, number(0)), boxInt32(word32(0)), boxNumber(key));
}

static bool isComparedByBits(const Type& key)
{
    if (key.primitives & (Primitive::String | Primitive::BigInt))
        return false;
    Rep rep = repOf(key);
    return !(key.primitives & Primitive::Number) || rep == Rep::Int64 || rep == Rep::Double || rep == Rep::OptionalInt64 || rep == Rep::OptionalDouble;
}

Word32 Lowering::mapKeyHash(Boxed key)
{
    LValue a = m_out.bitXor(key.value(), fixed64(Instance::offsetOfHashSecrets()));
    LValue b = m_out.bitXor(key.value(), fixed64(Instance::offsetOfHashSecrets() + sizeof(uint64_t)));
    return Word32(m_out.castToInt32(m_out.bitXor(m_out.mul(a, b), m_out.uMulHigh(a, b))));
}

Pointer Lowering::findInMapOrSet(CellPointer table, bool isMap, Boxed key, Word32 hash, LBasicBlock absent)
{
    static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
    LBasicBlock hasStorage = m_out.newBlock();
    LBasicBlock loop = m_out.newBlock();
    LBasicBlock hasEntry = m_out.newBlock();
    LBasicBlock next = m_out.newBlock();
    LBasicBlock found = m_out.newBlock();
    auto at = [&](Pointer base, ptrdiff_t index) {
        return m_out.address(m_heaps.OrderedHashTableData, base.value(), index * sizeof(EncodedJSValue));
    };

    Pointer storage = loadField<Pointer>(table, isMap ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
    m_out.branch(m_out.notNull(storage.value()), usually(hasStorage), rarely(absent));

    m_out.appendTo(hasStorage);
    Pointer data(m_out.add(storage.value(), m_out.constIntPtr(JSCellButterfly::offsetOfData())));
    Word32 capacity(m_out.load32(at(data, JSMap::Helper::capacityIndex())));
    Word32 bucket = add(bitAnd(hash, add(capacity, word32(-1))), word32(JSMap::Helper::hashTableStartIndex()));
    ValueFromBlock first = m_out.anchor(m_out.load64(at(offsetBy(data, zeroExtended(bucket), 3), 0)));
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue index = m_out.phi(Int64, first);
    m_out.branch(m_out.isZero64(index), unsure(absent), unsure(hasEntry));

    m_out.appendTo(hasEntry);
    Pointer entry = offsetBy(data, zeroExtended(Word32(m_out.castToInt32(index))), 3);
    m_out.branch(equal(Boxed(m_out.load64(at(entry, 0))), key).value(), unsure(found), unsure(next));

    m_out.appendTo(next);
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.load64(at(entry, isMap ? JSMap::Helper::ChainOffset : JSSet::Helper::ChainOffset))));
    m_out.jump(loop);

    m_out.appendTo(found);
    return entry;
}

bool Lowering::tableStubChecksReceiver(ValueID result, const Value& value)
{
    if (m_isInLoop || value.methodGetById() || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline)
        return false;
    auto& type = typeOf(result);
    bool isUnused = !useCountOf(result) && m_program.at(m_program.types.anything).isSubsetOf(type, m_program);
    unsigned numberOfArguments = 1;
    switch (implementation.inlined) {
    case InlineBuiltin::MapGet:
        break;
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas:
        if (&type != &m_program.at(m_program.types.boolean))
            return false;
        break;
    case InlineBuiltin::MapSet:
    case InlineBuiltin::SetAdd:
        if (!isUnused)
            return false;
        numberOfArguments = implementation.inlined == InlineBuiltin::MapSet ? 2 : 1;
        break;
    default:
        return false;
    }
    if (value.arguments.size() != numberOfArguments || isComparedByBits(typeOf(value.arguments[0])) || m_sunkKeyConcatenations.get(value.arguments[0].index()))
        return false;
    return true;
}

Integer Lowering::mapOrSetSize(CellPointer table, bool isMap, bool hasStringKeysByType)
{
    static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex());
    auto none = [&] {
        return integer(0);
    };
    auto inStorage = [&](auto otherwise) {
        Pointer storage = loadField<Pointer>(table, isMap ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
        return either<Integer>(Word32(m_out.notNull(storage.value())), [&] {
            return zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.OrderedHashTableData, storage.value(), JSMap::Helper::offsetOfAliveEntryCount()))));
        }, otherwise);
    };
    auto inCompactTable = [&](auto otherwise) {
        Pointer compactTable = loadField<Pointer>(table, isMap ? m_heaps.JSMap_compactTable : m_heaps.JSSet_compactTable);
        return either<Integer>(Word32(m_out.notNull(compactTable.value())), [&] {
            return zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.AOTTypedCompactTable_used, compactTable.value(), m_heaps.AOTTypedCompactTable_alive.offset()))));
        }, otherwise);
    };
    Integer size = hasStringKeysByType ? inCompactTable([&] {
        return inStorage(none);
    }) : inStorage([&] {
        return inCompactTable(none);
    });
    ensureStillAliveHere(table);
    return size;
}

Operand Lowering::lowerMapOrSetBuiltin(InlineBuiltin which, ValueID result, const Value& value, Writes inlinePath)
{
    auto& type = std::exchange(m_mapGetReturnsAnything, false) ? m_program.at(m_program.types.anything) : typeOf(result);
    auto& arguments = value.arguments;
    auto& booleanType = m_program.at(m_program.types.boolean);
    if (which == InlineBuiltin::MapNew)
        return ensure(AOT_TYPED_OPERATION(NewMap), Type::engineClass(m_builtins.classes()[value.engineClass()]), type);

    CellPointer table = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    bool isMap = which != InlineBuiltin::SetAdd && which != InlineBuiltin::SetHas && which != InlineBuiltin::SetSize;
    bool checksReceiver = std::exchange(m_tableStubChecksReceiver, false);
    if (which == InlineBuiltin::MapSize || which == InlineBuiltin::SetSize)
        return ensure(mapOrSetSize(table, isMap, value.hasStringKeysByType), Type::integer(Range::length()), type);
    auto tableResult = [&] {
        return ensure(table, typeOf(value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull), type);
    };
    if (which == InlineBuiltin::MapGet || which == InlineBuiltin::MapHas || which == InlineBuiltin::SetHas) {
        if (auto answer = tryLookupWithSunkKey(arguments[0], type, [&](Word32 count, Pointer parts) -> Boxed {
            return AOT_TYPED_OPERATION(TryMapOrSetLookupWithKeyParts, table, word32(which == InlineBuiltin::MapGet), count, parts);
        }, [&]() -> Operand {
            return lowerMapOrSetBuiltin(which, result, value, inlinePath);
        }))
            return *answer;
    }
    Boxed key = mapKey(arguments[0]);

    if (!isComparedByBits(typeOf(arguments[0]))) {
        auto callStub = [&](Stub stub) {
            Vector<std::pair<LValue, GPRReg>, 4> operands { { table.value(), thisGPR }, { key.value(), argumentGPR(0) } };
            if (arguments.size() > 1)
                operands.append(std::pair { in<Boxed>(arguments[1]).value(), argumentGPR(1) });
            return Boxed(callICStub(checksReceiver ? checkingReceiver(stub) : stub, Int64, operands, std::nullopt, checksReceiver ? OperationEffects { } : OperationEffects { inlinePath, inlinePath != Writes::Nothing }));
        };
        auto& keyType = typeOf(arguments[0]);
        bool keyIsStringByType = keyType.primitives & Primitive::String && keyType.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
        switch (which) {
        case InlineBuiltin::MapGet:
            return ensure(callStub(keyIsStringByType ? Stub::MapGetWithStringKey : Stub::MapGet), type);
        case InlineBuiltin::MapHas:
            return ensure(unboxedBoolean(callStub(keyIsStringByType ? Stub::MapHasWithStringKey : Stub::MapHas)), booleanType, type);
        case InlineBuiltin::SetHas:
            return ensure(unboxedBoolean(callStub(keyIsStringByType ? Stub::SetHasWithStringKey : Stub::SetHas)), booleanType, type);
        default:
            break;
        }

        if (keyIsStringByType)
            callStub(isMap ? Stub::MapSetWithStringKey : Stub::SetAddWithStringKey);
        else
            callStub(isMap ? Stub::MapSet : Stub::SetAdd);
        return tableResult();
    }

    if (!m_isInLoop && !m_program.isHotOutsideLoops(m_id)) {
        Stub stub = Stub::SetAddByBits;
        switch (which) {
        case InlineBuiltin::MapGet:
            stub = Stub::MapGetByBits;
            break;
        case InlineBuiltin::MapHas:
            stub = Stub::MapHasByBits;
            break;
        case InlineBuiltin::MapSet:
            stub = Stub::MapSetByBits;
            break;
        case InlineBuiltin::SetHas:
            stub = Stub::SetHasByBits;
            break;
        default:
            RELEASE_ASSERT(which == InlineBuiltin::SetAdd);
            break;
        }
        Vector<std::pair<LValue, GPRReg>, 4> operands { { table.value(), thisGPR }, { key.value(), argumentGPR(0) } };
        if (which == InlineBuiltin::MapSet)
            operands.append(std::pair { in<Boxed>(arguments[1]).value(), argumentGPR(1) });
        Boxed returnedValue(callICStub(stub, Int64, operands, std::nullopt, OperationEffects { inlinePath, inlinePath != Writes::Nothing }));
        if (which == InlineBuiltin::MapGet)
            return ensure(returnedValue, type);
        if (which == InlineBuiltin::MapHas || which == InlineBuiltin::SetHas)
            return ensure(unboxedBoolean(returnedValue), booleanType, type);
        return tableResult();
    }

    Word32 hash = mapKeyHash(key);
    LBasicBlock absent = m_out.newBlock();
    Pointer entry = findInMapOrSet(table, isMap, key, hash, absent);
    TypedPointer entryValue = m_out.address(m_heaps.OrderedHashTableData, entry.value(), sizeof(EncodedJSValue));
    switch (which) {
    case InlineBuiltin::MapGet: {
        Rep rep = repOf(type);
        Join join(m_out, rep, b3TypeOf(rep));
        join.add(ensure(Boxed(m_out.load64(entryValue)), type));
        m_out.appendTo(absent);
        join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
        Operand found = join.finish();
        ensureStillAliveHere(table);
        return found;
    }
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas: {
        Join join(m_out, Rep::Int32, Int32);
        join.add(boolean(true));
        m_out.appendTo(absent);
        join.add(boolean(false));
        Operand has = join.finish();
        ensureStillAliveHere(table);
        return ensure(has, booleanType, type);
    }
    default:
        break;
    }

    Join join(m_out, Rep::Void, Void);
    OperationEffects addEffects { Writes::RuntimeState, true };
    if (isMap) {
        didEmitWrite(Writes::RuntimeState);
        m_out.store64(in<Boxed>(arguments[1]).value(), entryValue);
        if (typeOf(arguments[1]).hasCells())
            barrier(loadField<CellPointer>(table, m_heaps.JSMap_storage));
    }
    join.add({ });
    m_out.appendTo(absent);
    if (isMap)
        callOperation<operationAOTMapSet>(Entry::operationAOTMapSet, addEffects, table, key, in<Boxed>(arguments[1]), hash);
    else
        callOperation<operationAOTSetAdd>(Entry::operationAOTSetAdd, addEffects, table, key, hash);
    join.add({ });
    join.finish();
    return tableResult();
}

bool Lowering::checksMapGetResultAfterJoin(const Value& value)
{
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::MapGet || value.arguments.size() != 1)
        return false;
    if (isComparedByBits(typeOf(value.arguments[0])) || m_sunkKeyConcatenations.get(value.arguments[0].index()))
        return false;
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

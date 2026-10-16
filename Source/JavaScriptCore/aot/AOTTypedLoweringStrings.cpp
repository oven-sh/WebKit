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

template<> struct ParameterOfOperation<const Latin1Character*> { using Type = Pointer; };

// Strings, maps and sets.
//
// A string points to a StringImpl, which has its characters, or it is a rope: the word that would point there is marked, and the string is
// a part of another string (a substring) or up to three strings one after the other. A rope is made flat when someone needs it to be,
// once. From then on it points to a StringImpl like any other string.
//
// What the back end is told:
//
// The word is read as if it never changed (fiberOf()), so that it is read once, before a loop, and all that is read through it with it.
// That is sound. Without the mark it is as good as any later one: a string that is made a property key of comes to point to another
// StringImpl, which has the same characters, and the engine keeps the old one for as long as the collector finds the string on the stack
// (JSString::swapToAtomString()). So after every look at the characters the string has to be found still: ensureStillAliveHere(). With the
// mark it only says that the string was a rope: the path for ropes reads the word again, as part of the state of the run time.
//
// Length, width and place of the characters of a StringImpl never change. How long a string is a rope says itself, in a place that stays
// as it is when the rope is made flat.
//
// A substring is looked at where its characters are, in the string it is a part of. It is not made flat for that.

Lowering::StringReceiver Lowering::stringReceiver(CellPointer string)
{
    return { string, safeIfReaches(string, sizeof(JSString)), { }, { } };
}

// The value is a string, or `undefined` or `null` if its type says so: then nothing is loaded from it before `isString` has been looked at.
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
        receiver.isCellOrNil = !(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull));
        receiver.value = value;
    }
    return receiver;
}

void Lowering::turnAwayNil(const StringReceiver& receiver, StringID name)
{
    if (!receiver.isString)
        return;
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(receiver.isString.value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(receiver.value, name, receiver.isCellOrNil ? receiver.string : CellPointer());
    m_out.appendTo(continuation);
}

bool Lowering::isReadOfCapturedStringVariable(ValueID id) const
{
    auto& read = m_function.at(id);
    if (read.opcode != Opcode::GetField || !holdsCells(read.rep) || isObjectLayout(m_program.at(read.layout()).kind))
        return false;
    auto& field = m_program.at(read.layout()).fields[read.field()];
    return field.stored && m_program.at(field.stored).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull) && m_program.at(field.stored).isWithin(m_program.at(read.type), m_program);
}

bool Lowering::mayStoreToCapturedVariable(const Block& block, size_t index, const Value& read) const
{
    auto& value = m_function.at(block.values[index]);
    if (value.opcode == Opcode::PutField)
        return m_program.at(value.layout()).fields[value.field()].slot == m_program.at(read.layout()).fields[read.field()].slot;
    Writes writes = value.opcode == Opcode::CallDirect ? m_program.at(value.function()).writes : writesOf(m_program, m_function, block, index);
    if (writes < Writes::Fields)
        return false;
    bool storesToOnePlace = value.opcode == Opcode::PutById || value.opcode == Opcode::PutByVal || value.opcode == Opcode::PutModuleVariable;
    return !storesToOnePlace || writes > Writes::Fields;
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
    BitVector comesToRead(m_function.blocks.size());
    for (size_t b : loop.blocks) {
        if (m_function.blocks[b].values.containsIf(isSameRead))
            comesToRead.set(b);
    }
    auto successorComesToRead = [&](const Block& block) {
        for (auto& target : successorsOf(block.terminator)) {
            if (loop.blocks.get(target.block.index()) && comesToRead.get(target.block.index()))
                return true;
        }
        return false;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t b : loop.blocks) {
            if (!comesToRead.get(b) && successorComesToRead(m_function.blocks[b])) {
                comesToRead.set(b);
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
            return !successorComesToRead(block) && std::ranges::none_of(block.values.span().subspan(index + 1), isSameRead);
        }
        return false;
    });
    return true;
}

// The strings that loops read and do not define. Each is opened in front of the outermost loop that does not define it, by every block that enters that loop.
void Lowering::findStringsReadInLoops()
{
    if (m_loops.isEmpty())
        return;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        if (m_innermostLoop[b] == noLoop)
            continue;
        auto& valuesOfBlock = m_function.blocks[b].values;
        for (size_t indexInBlock = 0; indexInBlock < valuesOfBlock.size(); ++indexInBlock) {
            ValueID id = valuesOfBlock[indexInBlock];
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
            if (isReadOfCapturedStringVariable(string)) {
                size_t indexOfRead = valuesOfBlock.find(string);
                isCapturedVariable = indexOfRead != notFound && indexOfRead < indexInBlock;
                for (size_t between = indexOfRead + 1; isCapturedVariable && between < indexInBlock; ++between)
                    isCapturedVariable = !mayStoreToCapturedVariable(m_function.blocks[b], between, m_function.at(string));
            }
            // An outer loop comes first.
            for (unsigned i = 0; i < m_loops.size(); ++i) {
                auto& loop = m_loops[i];
                if (!loop.blocks.get(b) || !loop.header.index())
                    continue;
                bool isDefinedOutside = isLoopInvariant(loop, string);
                if (!isDefinedOutside && (!isCapturedVariable || !isLoopInvariant(loop, m_function.at(string).child(0))))
                    continue;
                if (m_stringsOpenedInFrontOfLoops.isEmpty())
                    m_stringsOpenedInFrontOfLoops.grow(m_loops.size());
                auto& opened = m_stringsOpenedInFrontOfLoops[i];
                size_t index = opened.findIf([&](auto& one) {
                    if (isDefinedOutside || !one.cellOfCapturedVariable)
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
                        opened[index].cellOfCapturedVariable = m_proc.addVariable(Int64);
                    for (ValueID store : stores)
                        m_stringsOpenedAgainAfter.add(store.index(), Vector<std::pair<unsigned, unsigned>, 1> { }).iterator->value.append({ i, static_cast<unsigned>(index) });
                }
                if (!isDefinedOutside)
                    m_readsOfOpenedCapturedVariables.set(id.index());
                opened[index].isOpened |= readsCharacters;
                break;
            }
        }
    }
}

void Lowering::openStringsInFrontOfLoopsEnteredBy(const Terminator& terminator)
{
    if (m_stringsOpenedInFrontOfLoops.isEmpty())
        return;
    for (auto& target : successorsOf(terminator)) {
        for (unsigned i = 0; i < m_loops.size(); ++i) {
            if (m_loops[i].header != target.block || m_loops[i].blocks.get(m_currentBlockID.index()))
                continue;
            for (auto& opened : m_stringsOpenedInFrontOfLoops[i])
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
        open(m_stringsOpenedInFrontOfLoops[loop][index]);
}

void Lowering::open(const OpenedString& opened)
{
    auto set = [&](B3::Variable* variable, LValue value) {
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), variable, value);
    };
    StringReceiver receiver;
    if (opened.cellOfCapturedVariable) {
        Operand held = lowerGetEnvironmentField(opened.string, m_function.at(opened.string));
        auto& type = typeOf(opened.string);
        set(opened.cellOfCapturedVariable, held.value);
        receiver = stringReceiver(held.rep == Rep::Cell ? held.as<CellPointer>() : asCell(held.as<Boxed>()));
        if (held.rep != Rep::Cell && type.mayBeUndefinedOrNull()) {
            receiver.flags = 0;
            receiver.isString = isCell(held.as<Boxed>(), type);
        }
    } else
        receiver = stringReceiver(opened.string);
    LBasicBlock continuation = m_out.newBlock();
    // The read in the loop turns `undefined` and `null` away before it looks at any of this.
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
        set(opened.length, zeroExtended(either<Word32>(lacksBits(fiber, JSString::isRopeInPointer), [&] {
            return loadField<Word32>(fiber, m_heaps.StringImpl_length);
        }, [&] {
            return loadField<Word32>(receiver.string, m_heaps.JSRopeString_length);
        })).value());
        ensureStillAliveHere(receiver.string);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    // Every rope is made flat: a substring that is looked at in its base lets go of the base when something in the loop makes it flat.
    Pointer impl = eitherPointer(lacksBits(fiber, JSString::isRopeInPointer), [&] {
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

// For the block that is being lowered.
const Lowering::OpenedString* Lowering::openedString(ValueID id) const
{
    if (m_stringsOpenedInFrontOfLoops.isEmpty() || !id || m_innermostLoop[m_currentBlockID.index()] == noLoop)
        return nullptr;
    id = stringDefinedBy(id);
    for (unsigned i = 0; i < m_loops.size(); ++i) {
        if (!m_loops[i].blocks.get(m_currentBlockID.index()))
            continue;
        for (auto& opened : m_stringsOpenedInFrontOfLoops[i]) {
            if (opened.string == id && !opened.cellOfCapturedVariable)
                return &opened;
            ValueID user = valueBeingLowered();
            if (!opened.cellOfCapturedVariable || !user || !m_readsOfOpenedCapturedVariables.get(user.index()) || stringDefinedBy(m_function.at(user).child(0)) != id)
                continue;
            auto& read = m_function.at(id);
            auto& other = m_function.at(opened.string);
            if (read.opcode == Opcode::GetField && other.child(0) == read.child(0) && other.layout() == read.layout() && other.field() == read.field())
                return &opened;
        }
    }
    return nullptr;
}

std::optional<Lowering::Characters> Lowering::charactersOfOpenedString(const StringReceiver& receiver)
{
    auto* opened = openedString(receiver.id);
    if (!opened || !opened->isOpened)
        return std::nullopt;
    auto get = [&](B3::Variable* variable) {
        return m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), variable);
    };
    return Characters { Pointer(get(opened->data)), Word32(get(opened->flags)), Integer(get(opened->length)), opened->cellOfCapturedVariable ? CellPointer(get(opened->cellOfCapturedVariable)) : receiver.string };
}

void Lowering::findPartsOfStringsThatAreOnlyLookedAt()
{
    auto inlinedAs = [&](const Value& value) -> std::optional<InlineBuiltin> {
        if (value.opcode != Opcode::CallBuiltin || !value.child(0))
            return std::nullopt;
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline)
            return std::nullopt;
        return implementation.inlined;
    };
    BitVector isPart;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto which = inlinedAs(m_function.at(id));
            if (which && (*which == InlineBuiltin::StringSlice || *which == InlineBuiltin::StringSubstring || *which == InlineBuiltin::StringSubstr) && holdsCells(m_function.at(id).rep))
                isPart.set(id.index());
        }
    }
    if (isPart.isEmpty())
        return;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& user = m_function.at(id);
            auto which = inlinedAs(user);
            bool looksAtReceiver = which && (*which == InlineBuiltin::StringLength || *which == InlineBuiltin::StringCharCodeAt || *which == InlineBuiltin::StringCodePointAt || *which == InlineBuiltin::StringCharAt || *which == InlineBuiltin::StringAt);
            user.forEachChild([&](ValueID child) {
                if (!isPart.get(child.index()))
                    return;
                if (looksAtReceiver && child == user.child(0) && !user.arguments.contains(child))
                    return;
                if (user.opcode == Opcode::StrictEq && constantASCIIText(user.child(user.child(0) == child)))
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
        m_isFetchedLate.set(index);
    m_partsOfStringsThatAreOnlyLookedAt = WTF::move(isPart);
}

const Lowering::PartOfString* Lowering::partOfStringMadeBy(ValueID id) const
{
    if (m_partsOfStrings.isEmpty() || !id)
        return nullptr;
    auto found = m_partsOfStrings.find(id.index());
    return found == m_partsOfStrings.end() ? nullptr : &found->value;
}

Integer Lowering::lengthOf(const PartOfString& part)
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
    Integer length = zeroExtended(either<Word32>(lacksBits(fiber, JSString::isRopeInPointer), [&] {
        return loadField<Word32>(fiber, m_heaps.StringImpl_length);
    }, [&] {
        return loadField<Word32>(receiver.string, m_heaps.JSRopeString_length);
    }));
    ensureStillAliveHere(receiver.string);
    return length;
}

// A rope that is no substring is made flat, or left to `ifRope`.
Lowering::Characters Lowering::charactersOf(const StringReceiver& receiver, LBasicBlock ifRope)
{
    if (auto characters = charactersOfOpenedString(receiver))
        return *characters;
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> data;
    Vector<ValueFromBlock, 3> flags;
    Vector<ValueFromBlock, 3> lengths;
    auto yield = [&](Pointer first, Word32 flagsOfImpl, Word32 length) {
        data.append(m_out.anchor(first.value()));
        flags.append(m_out.anchor(flagsOfImpl.value()));
        lengths.append(m_out.anchor(length.value()));
        m_out.jump(continuation);
    };
    auto yieldAllOf = [&](Pointer impl) {
        yield(loadField<Pointer>(impl, m_heaps.StringImpl_data), loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed), loadField<Word32>(impl, m_heaps.StringImpl_length));
    };

    Pointer fiber = fiberOf(receiver);
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(rope));
    m_out.appendTo(flat);
    yieldAllOf(fiber);

    m_out.appendTo(rope);
    if (!ifRope && !m_watchingHandler) {
        CharactersOfSubstring opened = charactersOfRope(receiver.string);
        yield(Pointer(opened.data), Word32(opened.flags), Word32(opened.length));
        m_out.appendTo(continuation);
        return { Pointer(m_out.phi(Int64, data)), Word32(m_out.phi(Int32, flags)), zeroExtended(Word32(m_out.phi(Int32, lengths))), receiver.string };
    }
    LBasicBlock substring = newColdBlock();
    LBasicBlock notSubstring = newColdBlock();
    // Through a pointer of its own: a load is taken for an earlier one from the same place that is said never to change.
    CellPointer string(m_out.opaque(receiver.string.value()));
    Pointer current = loadField<Pointer>(string, m_heaps.JSRopeString_fiber0);
    m_out.branch(lacksBits(current, JSRopeString::isSubstringInPointer).value(), unsure(notSubstring), unsure(substring));

    // Nothing comes between these loads and the look at the characters that could make the substring flat, and so let go of its base.
    m_out.appendTo(substring);
    CharactersOfSubstring part = charactersOfSubstring(receiver.string);
    yield(Pointer(part.data), Word32(part.flags), Word32(part.length));

    m_out.appendTo(notSubstring);
    Word32 isFlatByNow = lacksBits(current, JSString::isRopeInPointer);
    if (ifRope) {
        orElse(isFlatByNow, ifRope);
        yieldAllOf(current);
    } else {
        yieldAllOf(eitherPointer(isFlatByNow, [&] {
            return current;
        }, [&] {
            return AOT_TYPED_OPERATION(Flatten, receiver.string);
        }));
    }

    m_out.appendTo(continuation);
    return { Pointer(m_out.phi(Int64, data)), Word32(m_out.phi(Int32, flags)), zeroExtended(Word32(m_out.phi(Int32, lengths))), receiver.string };
}

// The index is inside.
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

// The index is inside.
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

CellPointer Lowering::stringOfCodeUnit(Word32 unit)
{
    return either<CellPointer>(belowOrEqual(unit, word32(maxSingleCharacterString)), [&] {
        return CellPointer(singleCharacterString(unit.value()));
    }, [&] {
        return AOT_TYPED_OPERATION(CharToString, unit);
    });
}

// The index is inside.
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
        CellPointer made = stringOfCodeUnit(Word32(wide));
        if (codeUnit)
            codeUnits.append(m_out.anchor(wide));
        return made;
    });
    if (codeUnit)
        *codeUnit = Word32(m_out.phi(Int32, codeUnits));
    ensureStillAliveHere(characters.owner);
    return string;
}

Operand Lowering::getStringElement(const StringReceiver& receiver, ValueID key, const Type& type, LBasicBlock otherwise, B3::Variable* codeUnitIfKept)
{
    Rep rep = repOf(type);
    LBasicBlock outside = otherwise ? otherwise : newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    turnAwayNil(receiver, { });
    Integer index = indexOrElse(key, outside);
    Characters characters = charactersOf(receiver);
    if (!std::exchange(m_indexIsShownToBeInside, false))
        orElse(below(index, characters.length), outside);
    // Not in an arm of a test on what is expected: what is defined there is not there behind it.
    bool keepsCodeUnit = !otherwise;
    Word32 codeUnit;
    CellPointer string = singleCharacterStringAt(characters, index, keepsCodeUnit || codeUnitIfKept ? &codeUnit : nullptr);
    if (codeUnitIfKept)
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), codeUnitIfKept, zeroExtended(codeUnit).value());
    Vector<ValueFromBlock, 2> codeUnits;
    if (keepsCodeUnit)
        codeUnits.append(m_out.anchor(zeroExtended(codeUnit).value()));
    join.add(ensure(string, m_program.at(m_program.types.string), type));
    if (otherwise)
        return join.finish();

    m_out.appendTo(outside);
    if (auto nothing = absentElement(type)) {
        if (keepsCodeUnit)
            codeUnits.append(m_out.anchor(m_out.constInt64(codeUnitOfUndefined)));
        join.add(*nothing);
    }
    Operand joined = join.finish();
    if (keepsCodeUnit)
        didMakeStringOfCodeUnit(m_currentBlock->values[m_currentIndexInBlock], joined, codeUnits);
    return joined;
}

// ToIntegerOrInfinity of a number: what is not a number is zero. What is too large to be kept as an integer stays outside every string.
Integer Lowering::positionOf(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep == Rep::Int64)
        return operand.as<Integer>();
    Number asNumber = operand.as<Number>();
#if CPU(ARM64)
    // The conversion yields 0 for what is not a number.
    return truncateToInteger(asNumber);
#else
    return select(equal(asNumber, asNumber), truncateToInteger(asNumber), integer(0));
#endif
}

// An argument of `slice` or `substring`, which is a number, as a place in a string of the length: not outside, and for `slice` counted
// from the end if negative.
Integer Lowering::placeInString(ValueID id, Integer length, bool isRelativeToEnd)
{
    auto& type = typeOf(id);
    Integer x;
    if (Operand operand = get(id); operand.rep == Rep::Int64)
        x = operand.as<Integer>();
    else {
#if CPU(ARM64)
        // Likewise, and the least or the greatest integer for what does not fit.
        x = truncateToInteger(operand.as<Number>());
#else
        // No string is as long as the limit.
        Number limit = number(static_cast<double>(1ll << 32));
        Number given = operand.as<Number>();
        given = select(equal(given, given), given, number(0));
        given = select(greaterThan(given, limit), limit, given);
        given = select(lessThan(given, neg(limit)), neg(limit), given);
        x = truncateToInteger(given);
#endif
    }
    Integer capped = select(greaterThan(x, length), length, x);
    if (type.isInteger() && type.integers.least >= 0)
        return capped;
    Integer zero = integer(0);
    Integer fromEnd = zero;
    if (isRelativeToEnd) {
        Integer sum = add(length, x);
        fromEnd = select(lessThan(sum, zero), zero, sum);
    }
    return select(lessThan(x, zero), fromEnd, capped);
}

// With its header. The three words that make it a rope are for whoever asks to store, followed by fenceAfterInitialization().
CellPointer Lowering::allocateRope(LBasicBlock slowPath)
{
    LValue cell = allocateHeapCell(fixedPointer(Instance::offsetOfRopeStringAllocator()), sizeof(JSRopeString), slowPath);
    applyEffects(cell->as<PatchpointValue>(), { Writes::RuntimeState, false });
    storeHeader(cell, fixed64(Instance::offsetOfStringHeader()));
    return CellPointer(cell);
}

// From the one place to the other, neither of which is outside. Nothing, if they are the wrong way round.
CellPointer Lowering::partOfString(const StringReceiver& receiver, Integer length, Integer from, Integer to)
{
    auto byStub = [&](bool isOffUsualPath) {
        return CellPointer(callAllocationStub(Stub::PartOfString, { { receiver.string.value(), GPRInfo::toArgumentRegister(1) }, { from.value(), GPRInfo::toArgumentRegister(2) }, { to.value(), GPRInfo::toArgumentRegister(3) } }, effectsOfOperation<operationAOTTypedSubstring>(), true, isOffUsualPath));
    };
    if (!m_program.isWarmByText(m_id))
        return byStub(false);
    if (!m_isInLoop && !m_watchingHandler) {
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
        m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(otherwise));
        m_out.appendTo(flat);
        Word32 flags = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
        LValue base = receiver.string.value();
        CellPointer made = allocateRope(otherwise);
        m_out.store64(m_out.bitOr(m_out.constInt64(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), zeroExtended(bitAnd(flags, word32(StringImpl::flagIs8Bit()))).value()), made.value(), m_heaps.JSRopeString_fiber0);
        m_out.store64(m_out.bitOr(span.value(), m_out.shl(base, m_out.constInt32(32))), made.value(), m_heaps.JSRopeString_fiber1);
        m_out.store64(m_out.bitOr(m_out.lShr(base, m_out.constInt32(32)), m_out.shl(from.value(), m_out.constInt32(16))), made.value(), m_heaps.JSRopeString_fiber2);
        fenceAfterInitialization();
        join.add(made);
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

    // A substring of a string that is no rope. A part of a substring is a part of what that is a part of.
    m_out.appendTo(part);
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    Pointer fiber = fiberOf(receiver);
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(rope));
    m_out.appendTo(flat);
    ValueFromBlock flatBase = m_out.anchor(receiver.string.value());
    ValueFromBlock flatStart = m_out.anchor(from.value());
    ValueFromBlock flatFlags = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
    m_out.jump(make);

    m_out.appendTo(rope);
    CellPointer string(m_out.opaque(receiver.string.value()));
    Pointer current = loadField<Pointer>(string, m_heaps.JSRopeString_fiber0);
    m_out.branch(lacksBits(current, JSRopeString::isSubstringInPointer).value(), unsure(slowPath), unsure(substring));
    m_out.appendTo(substring);
    LValue lengthAndBaseLow = m_out.load64(string.value(), m_heaps.JSRopeString_fiber1);
    LValue baseHighAndOffset = m_out.load64(string.value(), m_heaps.JSRopeString_fiber2);
    ValueFromBlock baseOfSubstring = m_out.anchor(m_out.bitOr(m_out.lShr(lengthAndBaseLow, m_out.constInt32(32)), m_out.shl(m_out.bitAnd(baseHighAndOffset, m_out.constInt64(0xffff)), m_out.constInt32(32))));
    ValueFromBlock startInBase = m_out.anchor(m_out.add(from.value(), m_out.lShr(baseHighAndOffset, m_out.constInt32(16))));
    ValueFromBlock flagsOfSubstring = m_out.anchor(m_out.castToInt32(current.value()));
    m_out.jump(make);

    m_out.appendTo(make);
    LValue base = m_out.phi(Int64, flatBase, baseOfSubstring);
    LValue start = m_out.phi(Int64, flatStart, startInBase);
    Word32 flags(m_out.phi(Int32, flatFlags, flagsOfSubstring));
    CellPointer made = allocateRope(slowPath);
    m_out.store64(m_out.bitOr(m_out.constInt64(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), zeroExtended(bitAnd(flags, word32(StringImpl::flagIs8Bit()))).value()), made.value(), m_heaps.JSRopeString_fiber0);
    m_out.store64(m_out.bitOr(span.value(), m_out.shl(base, m_out.constInt32(32))), made.value(), m_heaps.JSRopeString_fiber1);
    m_out.store64(m_out.bitOr(m_out.lShr(base, m_out.constInt32(32)), m_out.shl(start, m_out.constInt32(16))), made.value(), m_heaps.JSRopeString_fiber2);
    fenceAfterInitialization();
    join.add(made);

    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(Substring, receiver.string, low32(from), low32(to)));
    return join.finish().as<CellPointer>();
}

// Of a call of a built-in: its first argument is a text that is compared or looked for by loads and immediates. The cell of the constant is asked for in the general arm alone: see hasTextAt() and lowerStringSearch().
bool Lowering::looksForConstantWithoutItsCell(const Value& value) const
{
    if (value.arguments.isEmpty() || m_function.at(value.arguments[0]).opcode != Opcode::String)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline)
        return false;
    auto text = constantASCIIText(value.arguments[0]);
    if (!text)
        return false;
    // There the text goes by its number.
    auto isOutsideLoops = [&] {
        BlockID block = m_definingBlock[&value - m_function.values.span().data()];
        return block && !m_loopBlocks.get(block.index());
    };
    switch (implementation.inlined) {
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
        return text->size() <= maxLengthOfTextComparedInPlace || (value.arguments.size() == 1 && isOutsideLoops());
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

std::optional<std::span<const uint8_t>> Lowering::constantASCIIText(ValueID id) const
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

std::optional<Word32> Lowering::areKeptCodeUnitsEqual(ValueID a, ValueID b)
{
    for (auto [constant, other] : { std::pair { a, b }, std::pair { b, a } }) {
        auto unit = constantCodeUnit(constant);
        if (!unit)
            continue;
        if (auto kept = codeUnitOfString(other))
            return equal(*kept, integer(*unit));
    }
    return std::nullopt;
}

// Concatenation.

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
        if (value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && program.at(value.function()).isBodyOfSeveralSites) {
            auto& body = program.at(value.function());
            BitVector keysOfBody = computeMapKeyValues(program, value.function(), body);
            auto& parameters = body.blocks[0].parameters;
            for (unsigned i = 0; i < std::min(parameters.size(), value.arguments.size()); ++i) {
                if (keysOfBody.get(parameters[i].index()))
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
    static constexpr OpcodeSet mayMakeKeys { Opcode::GetByVal, Opcode::PutByVal, Opcode::CallDirect, Opcode::CallBuiltin };
    if (&function == &program.at(id))
        program.functions.forEachValueInOrder(id.index(), mayMakeKeys, lookAt);
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
        for (auto& ofFunction : callees) {
            for (unsigned callee : ofFunction) {
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
    auto isNumberOrNil = [&](ValueID id) {
        auto& type = typeOf(id);
        return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    };
    auto isAppendable = [&](ValueID id) {
        return typeOf(id).isString() || isNumberOrNil(id) || mayBeBuilderState(id);
    };
    auto isConcatenation = [&](const Value& value, bool allowsStateAsFirstOperand) {
        if (!holdsCells(value.rep) || !program.at(value.type).isString())
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
            return isNumberOrNil(id);
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
                    if (isNumberOrNil(operand))
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
                    result.initialText.add(argument.index(), initial.text);
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
    // A rope says how wide its characters are with the bit a StringImpl says it with. What it was made with holds for what it is made flat to.
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer fiber = fiberOf(stringReceiver(string));
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), unsure(flat), unsure(rope));
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

// Of what is made a string of first, if it is none.
Lowering::StringPart Lowering::stringPartOf(ValueID id)
{
    CellPointer string = toStringCell(id);
    if (auto text = constantASCIIText(id))
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
    // Goes on where it is not.
    auto whereEmpty = [&](Integer lengthOfPart, CellPointer result) {
        LBasicBlock isEmpty = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(isZero(lengthOfPart).value(), rarely(isEmpty), usually(isNot));
        m_out.appendTo(isEmpty);
        join.add(result);
        m_out.appendTo(isNot);
    };
    // There is one empty string, and no rope stands for it.
    if (third) {
        if (mayBeEmpty)
            whereEmpty(length, CellPointer(emptyString()));
    } else {
        if (first.mayBeEmpty)
            whereEmpty(first.length, second.string);
        if (second.mayBeEmpty)
            whereEmpty(second.length, first.string);
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
        result = CellPointer(callAllocationStub(hasThird ? Stub::ConcatenateThree : Stub::ConcatenateTwo, operands, effectsOfOperation<operationAOTTypedConcat>(), true));
    }
    return result;
}

CellPointer Lowering::concatenateByStub(CellPointer first, CellPointer second)
{
    return CellPointer(callAllocationStub(Stub::ConcatenateTwo, { { first.value(), isARM64() ? stubTemporaryGPRs[2] : GPRInfo::returnValueGPR }, { second.value(), isARM64() ? stubTemporaryGPRs[3] : GPRInfo::argumentGPR1 } }, effectsOfOperation<operationAOTTypedConcat>(), true));
}

// A rope has room for three.
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

static bool isNumberOrNil(const Type& type)
{
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
}

// Of strings and numbers: it runs no code of the program.
bool Lowering::isConcatenation(const Value& value) const
{
    auto isStringOrNumber = [&](ValueID id) {
        auto& type = m_program.at(m_function.at(id).type);
        return type.isString() || isNumberOrNil(type);
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

// Yields whether all parts are numbers and constants.
bool Lowering::collectPartsOfConcatenation(const BitVector& isInBlock, ValueID id, bool isLast, PartsOfConcatenation& parts, PartsOfConcatenation& lookedThrough)
{
    auto& value = m_function.at(id);
    if (isLast || (isInBlock.get(id.index()) && numberOfUsesOf(id) == 1 && isConcatenation(value) && !m_stringBuilderAnalysis.isState(id))) {
        size_t numberOfParts = parts.size();
        size_t numberLookedThrough = lookedThrough.size();
        if (!isLast)
            lookedThrough.append(id);
        bool hasOnlyNumbersAndConstants = true;
        auto collect = [&](ValueID part) {
            hasOnlyNumbersAndConstants &= collectPartsOfConcatenation(isInBlock, part, false, parts, lookedThrough);
        };
        if (value.opcode == Opcode::Strcat) {
            for (ValueID part : value.arguments)
                collect(part);
        } else {
            collect(value.child(0));
            if (value.opcode == Opcode::Add)
                collect(value.child(1));
        }
        // To make a string of a number or of a string cannot fail, whenever it is done. To put strings together can, where they are too long for one: that stays where it is.
        if (isLast || value.opcode == Opcode::ToString || hasOnlyNumbersAndConstants)
            return hasOnlyNumbersAndConstants;
        parts.shrink(numberOfParts);
        lookedThrough.shrink(numberLookedThrough);
    }
    parts.append(id);
    return value.opcode == Opcode::String || isNumberOrNil(m_program.at(value.type));
}

// `a.id + "," + b.id`: the parts of the last Add or Strcat are what stands behind the ToString, Add and Strcat of its block that nothing else uses. Those are lowered nowhere, like a constant that is fetched where it is needed.
// Made in one piece a text costs a malloc, a hash and a destructor, which pays where it is looked up by at once, and nowhere else: a rope is cheaper to make.
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
        // The last one comes first.
        for (size_t i = block.values.size(); i--;) {
            ValueID id = block.values[i];
            auto& value = m_function.at(id);
            if ((value.opcode != Opcode::Add && value.opcode != Opcode::Strcat) || value.rep != Rep::Cell || isFetchedWhereNeeded(id) || m_stringBuilderAnalysis.isState(id) || !isConcatenation(value))
                continue;
            if (!keys)
                keys = computeMapKeyValues(m_program, m_id, m_function);
            if (!keys->get(id.index()))
                continue;
            PartsOfConcatenation parts;
            PartsOfConcatenation lookedThrough;
            collectPartsOfConcatenation(isInBlock, id, true, parts, lookedThrough);
            if (parts.size() < 2 || parts.size() > 8)
                continue;
            for (ValueID part : lookedThrough)
                m_isFetchedLate.set(part.index());
            m_partsOfConcatenations.add(id.index(), WTF::move(parts));
            if (numberOfUsesOf(id) == 1) {
                for (size_t j = i + 1; j < block.values.size(); ++j) {
                    auto& user = m_function.at(block.values[j]);
                    bool isLookupKey = user.opcode == Opcode::GetByVal && user.child(1) == id && user.child(0) != id;
                    if (user.opcode == Opcode::CallBuiltin && user.arguments.size() == 1 && user.arguments[0] == id && user.child(0) != id) {
                        auto& implementation = m_builtins.at(user.builtin()).implementation;
                        isLookupKey = implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::MapGet || implementation.inlined == InlineBuiltin::MapHas || implementation.inlined == InlineBuiltin::SetHas);
                    }
                    if (isLookupKey) {
                        m_sunkKeyConcatenations.set(id.index());
                        m_isFetchedLate.set(id.index());
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
            m_isFetchedLate.set(i);
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
    std::optional<Integer> exact = isDouble ? integerOfNumber(id) : std::nullopt;
    Integer integer = exact ? *exact : isDouble ? integralOrElse(Number(operand.value), Range::int32(), slowPath) : Integer(operand.value);
    LBasicBlock severalDigits = m_out.newBlock();
    LBasicBlock oneDigit = digitLoop;
    auto constant = [&](uint64_t bits) {
        return this->integer(static_cast<int64_t>(bits)).value();
    };
    constexpr uint64_t asciiZeros = 0x3030303030303030ull;
    m_out.branch(below(integer, this->integer(10)).value(), unsure(oneDigit), unsure(severalDigits));

    m_out.appendTo(oneDigit);
    ValueFromBlock textOfOneDigit = m_out.anchor(m_out.add(integer.value(), constant('0')));
    ValueFromBlock countOfOneDigit = m_out.anchor(m_out.constInt64(1));
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
    LValue bitsOfLeadingZeros = m_out.bitAnd(trailingZeros, constant(0x38));
    ValueFromBlock textOfSeveralDigits = m_out.anchor(m_out.lShr(m_out.add(digits, constant(asciiZeros)), m_out.castToInt32(bitsOfLeadingZeros)));
    ValueFromBlock countOfSeveralDigits = m_out.anchor(m_out.sub(constant(8), m_out.lShr(bitsOfLeadingZeros, m_out.constInt32(3))));
    m_out.jump(store);

    m_out.appendTo(store);
    LValue text = m_out.phi(Int64, textOfOneDigit, textOfSeveralDigits);
    LValue count = m_out.phi(Int64, countOfOneDigit, countOfSeveralDigits);
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
    orElse(lacksBits(impl, JSString::isRopeInPointer), slowPath);
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

void Lowering::appendStringOrNilToStringBuilder(const StackStringBuilder& builder, Boxed value)
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
        auto text = isNumber ? std::nullopt : constantASCIIText(operand);
        if (!isNumber && (!text || text->size() > maxStringBuilderInlineConstantLength)) {
            if (typeOf(operand).isString())
                appendStringToStringBuilder(builder, in<CellPointer>(operand));
            else
                appendStringOrNilToStringBuilder(builder, in<Boxed>(operand));
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

CellPointer Lowering::finishStringBuilder(const StackStringBuilder& builder, bool mayBeEmpty, bool goesOnLiving)
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
        if (goesOnLiving) {
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
        if (auto text = m_stringBuilderAnalysis.initialText.find(argument.index()); text != m_stringBuilderAnalysis.initialText.end())
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

std::optional<Operand> Lowering::lowerAsPartOfConcatenation(ValueID id, const Value& value)
{
    if (auto result = tryLowerWithStringBuilder(id, value))
        return result;
    if ((m_partsOfConcatenations.isEmpty() && !m_stringBuilderAnalysis.builderCount) || (value.opcode != Opcode::Add && value.opcode != Opcode::Strcat && value.opcode != Opcode::ToString))
        return std::nullopt;
    if (isFetchedWhereNeeded(id))
        return Operand(value.rep, nullptr);
    auto found = m_partsOfConcatenations.find(id.index());
    if (found == m_partsOfConcatenations.end())
        return std::nullopt;
    return convert(concatenateInOnePiece(found->value), typeOf(id), value.rep);
}

std::optional<Operand> Lowering::tryLookupWithSunkKey(ValueID key, const Type& type, const ScopedLambda<Boxed(Word32, Pointer)>& ask, const ScopedLambda<Operand()>& slowPath)
{
    if (!m_sunkKeyConcatenations.get(key.index()) || m_keyBeingMaterialized == key)
        return std::nullopt;
    auto& parts = m_partsOfConcatenations.find(key.index())->value;
    LValue buffer = m_out.lockedStackSlot(parts.size() * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < parts.size(); ++i) {
        auto& typeOfPart = typeOf(parts[i]);
        Operand operand = get(parts[i]);
        Boxed boxed;
        if (typeOfPart.isNumber() && operand.rep == Rep::Double) {
            Number number = operand.as<Number>();
            Word32 low = low32(truncateToInteger(number));
            boxed = select(equal(toNumber(low), number), boxInt32(low), box(purified(number)));
        } else if (operand.rep == Rep::OptionalDouble) {
            Number number = assumedNumber(operand.as<OptionalNumber>());
            Word32 low = low32(truncateToInteger(number));
            boxed = select(equal(toNumber(low), number), boxInt32(low), convertTo<Boxed>(operand, typeOfPart));
        } else
            boxed = convertTo<Boxed>(operand, typeOfPart);
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
    auto& typeOfKey = typeOf(key);
    m_assumptions.append(Assumption { key, convert(concatenateAsRopes(parts.span()), typeOfKey, m_function.at(key).rep), &typeOfKey });
    join.add(slowPath());
    m_assumptions.removeLast();
    return join.finish();
}

// For get(): one that was looked through, by itself.
CellPointer Lowering::concatenateWhereNeeded(const Value& value)
{
    if (value.opcode == Opcode::CallBuiltin) {
        auto* part = partOfStringMadeBy(ValueID(static_cast<uint32_t>(&value - m_function.values.span().data())));
        RELEASE_ASSERT(part);
        return partOfString(part->base, part->lengthOfBase, part->from, part->to);
    }
    if (ValueID id(static_cast<uint32_t>(&value - m_function.values.span().data())); isResidentInStringBuilder(id))
        return finishStringBuilder(stringBuilderFor(id), stringBuilderMayBeEmpty(id), true);
    if (size_t index = &value - m_function.values.span().data(); m_sunkKeyConcatenations.get(index))
        return concatenateInOnePiece(m_partsOfConcatenations.find(index)->value);
    if (value.opcode == Opcode::ToString)
        return toStringCell(value.child(0));
    if (value.opcode == Opcode::Strcat)
        return lowerStrcat(value);
    RELEASE_ASSERT(value.opcode == Opcode::Add);
    if (!m_isInLoop && !m_program.isHotOutsideItsLoops(m_id)) {
        CellPointer first = toStringCell(value.child(0));
        return concatenateByStub(first, toStringCell(value.child(1)));
    }
    StringPart first = stringPartOf(value.child(0));
    return concatenate(first, stringPartOf(value.child(1))).string;
}

// Flat and hashed, where it is short: as a rule it is the key of a Map. See operationAOTTypedConcatenateWithIntegers.
CellPointer Lowering::concatenateInOnePiece(const PartsOfConcatenation& parts)
{
    static constexpr int64_t maxLength = 64;
    static constexpr int64_t maxLengthOfInt32 = 11;
    int64_t room = maxLength;
    unsigned numberOfNumbers = 0;
    ValueID lastNumber;
    bool hasConstant = false;
    bool hasOtherStrings = false;
    for (ValueID part : parts) {
        if (isNumberOrNil(typeOf(part))) {
            room -= maxLengthOfInt32;
            ++numberOfNumbers;
            lastNumber = part;
        } else if (auto text = constantASCIIText(part)) {
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
    Integer lengthOfOtherStrings;
    Word32 flags;
    for (ValueID part : parts) {
        auto& type = typeOf(part);
        Operand operand = get(part);
        if (isNumberOrNil(type)) {
            if (operand.rep == Rep::Double) {
                // Minus zero reads "0".
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
        if (constantASCIIText(part))
            continue;
        Pointer fiber = fiberOf(stringReceiver(string));
        require(lacksBits(fiber, JSString::isRopeInPointer));
        Integer length = zeroExtended(loadField<Word32>(fiber, m_heaps.StringImpl_length));
        Word32 flagsOfImpl = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
        lengthOfOtherStrings = lengthOfOtherStrings ? add(lengthOfOtherStrings, length) : length;
        flags = flags ? bitAnd(flags, flagsOfImpl) : flagsOfImpl;
    }
    if (hasOtherStrings) {
        require(logicalAnd(belowOrEqual(lengthOfOtherStrings, integer(room)), hasBits(flags, StringImpl::flagIs8Bit())));
        // Of one number by itself the engine keeps the string. There is one empty string, and the operation makes a new one.
        if (numberOfNumbers == 1 && !hasConstant) {
            LBasicBlock isNumberAlone = m_out.newBlock();
            LBasicBlock next = m_out.newBlock();
            m_out.branch(isZero(lengthOfOtherStrings).value(), unsure(isNumberAlone), unsure(next));
            m_out.appendTo(isNumberAlone);
            join.add(toStringCell(lastNumber));
            m_out.appendTo(next);
        } else if (!numberOfNumbers && !hasConstant)
            require(logicalNot(isZero(lengthOfOtherStrings)));
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

// The engine keeps the strings of the integers it was asked for last.
CellPointer Lowering::integerToString(Integer value, const Type& type)
{
    if (!m_isInLoop && !m_program.isHotOutsideItsLoops(m_id)) {
        auto byStub = [&] {
            return CellPointer(callAllocationStub(Stub::StringOfInt32, { { value.value(), isARM64() ? stubTemporaryGPRs[2] : GPRInfo::argumentGPR1 } }, effectsOfOperation<operationAOTTypedIntToString>(), false));
        };
        if (type.isInteger() && type.integers.isWithin(Range::int32()))
            return byStub();
        return either<CellPointer>(equal(signExtended(low32(value)), value), byStub, [&] {
            return AOT_TYPED_OPERATION(IntToString, value);
        });
    }
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Cell, Int64);
    Word32 low = low32(value);
    if (!type.isInteger() || !type.integers.isWithin(Range::int32()))
        orElse(equal(signExtended(low), value), slowPath);
    join.add(CellPointer(int32ToString(low.value(), slowPath)));
    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(IntToString, value));
    return join.finish().as<CellPointer>();
}

// Comparison.

// Whether the 8-bit characters from there on, of which there are as many, are the text, which is not empty.
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
    // What is left, by the narrowest load that has room for it: its constant is the shortest. Where that is wider, with those before them once more.
    if (size_t left = size - offset) {
        unsigned last = left > 4 ? 8 : left > 2 ? 4 : left;
        compare(size - last, last);
    }
    return result;
}

// Whether the characters of the string from the place on are the text, which is not empty and has room there. The value is the text as a string.
Word32 Lowering::hasTextAt(const StringReceiver& receiver, Integer place, ValueID constant, std::span<const uint8_t> text)
{
    auto generally = [&] {
        return AOT_TYPED_OPERATION(StringHasAt, receiver.string, in<CellPointer>(constant), low32(place));
    };
    if (text.size() > maxLengthOfTextComparedInPlace)
        return generally();
    Characters characters = charactersOf(receiver);
    return either<Word32>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
        Word32 result = hasText(offsetBy(characters.data, place, 0), text);
        ensureStillAliveHere(characters.owner);
        return result;
    }, generally);
}

Word32 Lowering::areBytesEqual(Pointer p, Pointer q, Integer length)
{
    LBasicBlock isShort = m_out.newBlock();
    LBasicBlock byteLoop = m_out.newBlock();
    LBasicBlock nextByte = m_out.newBlock();
    LBasicBlock wordLoop = m_out.newBlock();
    LBasicBlock nextWord = m_out.newBlock();
    LBasicBlock lastWord = m_out.newBlock();
    LBasicBlock isSame = m_out.newBlock();
    LBasicBlock isDifferent = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    auto byteAt = [&](Pointer data, LValue index) {
        return marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    auto wordAt = [&](Pointer data, LValue index) {
        return marked(m_out.load64(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    ValueFromBlock firstWord = m_out.anchor(m_out.int64Zero);
    m_out.branch(below(length, integer(8)).value(), unsure(isShort), unsure(wordLoop));

    m_out.appendTo(isShort);
    ValueFromBlock firstByte = m_out.anchor(m_out.int64Zero);
    m_out.branch(isZero(length).value(), rarely(isSame), usually(byteLoop));
    m_out.appendTo(byteLoop);
    LValue byteIndex = m_out.phi(Int64, firstByte);
    m_out.branch(m_out.equal(byteAt(p, byteIndex), byteAt(q, byteIndex)), unsure(nextByte), unsure(isDifferent));
    m_out.appendTo(nextByte);
    LValue nextByteIndex = m_out.add(byteIndex, m_out.constInt64(1));
    m_out.addIncomingToPhi(byteIndex, m_out.anchor(nextByteIndex));
    m_out.branch(m_out.below(nextByteIndex, length.value()), unsure(byteLoop), unsure(isSame));

    m_out.appendTo(wordLoop);
    LValue wordIndex = m_out.phi(Int64, firstWord);
    m_out.branch(m_out.equal(wordAt(p, wordIndex), wordAt(q, wordIndex)), unsure(nextWord), unsure(isDifferent));
    m_out.appendTo(nextWord);
    LValue nextWordIndex = m_out.add(wordIndex, m_out.constInt64(8));
    m_out.addIncomingToPhi(wordIndex, m_out.anchor(nextWordIndex));
    m_out.branch(m_out.belowOrEqual(m_out.add(nextWordIndex, m_out.constInt64(8)), length.value()), unsure(wordLoop), unsure(lastWord));
    // The last ones once more, with those before them.
    m_out.appendTo(lastWord);
    LValue lastIndex = m_out.sub(length.value(), m_out.constInt64(8));
    join.add(Word32(m_out.equal(wordAt(p, lastIndex), wordAt(q, lastIndex))));

    m_out.appendTo(isSame);
    join.add(boolean(true));
    m_out.appendTo(isDifferent);
    join.add(boolean(false));
    return join.finish().as<Word32>();
}

// The text is all ASCII and not empty. A few instructions at the site, and no cell for the text; what is in place is some sixty, and five fewer where it runs.
Word32 Lowering::isTextByStub(CellPointer string, ValueID constant)
{
    constant = stringConstantOf(constant);
    RELEASE_ASSERT(constant);
    LValue number = m_out.constInt64(m_function.at(constant).string().index());
    return Word32(m_out.castToInt32(callAllocationStub(Stub::StringIsText, { { number, stubTemporaryGPRs[0] }, { string.value(), stubTemporaryGPRs[2] } }, { Writes::Nothing, false }, false)));
}

// Ends the block. The text is all ASCII and not empty. See generateIsTextOfLength().
void Lowering::branchOnTextByOneCall(ValueID subject, ValueID constant, std::span<const uint8_t> text, LBasicBlock isText, LBasicBlock isOther)
{
    Operand operand = get(subject);
    auto& type = typeOf(subject);
    bool isString = operand.rep == Rep::Cell && type.hasOnlyPrimitives(Primitive::String);
    branchOnTextByOneCall(operand.rep == Rep::Cell ? operand.as<CellPointer>().value() : operand.as<Boxed>().value(), isString, constant, text, isText, isOther);
}

void Lowering::branchOnTextByOneCall(LValue value, bool isString, ValueID constant, std::span<const uint8_t> text, LBasicBlock isText, LBasicBlock isOther)
{
    if (text.size() > maxLengthOfTextInRegister) {
        constant = stringConstantOf(constant);
        RELEASE_ASSERT(constant);
        LValue number = m_out.constInt64(m_function.at(constant).string().index());
        LValue isSame = callAllocationStub(isString ? Stub::StringIsText : Stub::ValueIsText, { { number, stubTemporaryGPRs[0] }, { value, stubTemporaryGPRs[2] } }, { Writes::Nothing, false }, false);
        m_out.branch(m_out.castToInt32(isSame), unsure(isText), unsure(isOther));
        return;
    }
    Stub stub = static_cast<Stub>(static_cast<unsigned>(Stub::ValueIsTextOfLength1) + 4 * (text.size() - 1) + (isString ? 2 : 0));
    uint64_t packed = textInRegister(text);
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, { Writes::Nothing, false });
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(value, ValueRep::reg(stubTemporaryGPRs[2])));
    RegisterSet changed = *registersChangedBy(stub);
    patchpoint->clobberLate(changed);
    patchpoint->clobberEarly(changed);
    m_out.appendSuccessor(unsure(isOther));
    m_out.appendSuccessor(unsure(isText));
    patchpoint->setGenerator([context = &m_context, stub, packed](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        bool hasNoFrame = context->hasNoFrame(params.proc().code());
        bool isLeaf = hasStubsForFunctionsWithoutFrame && hasNoFrame;
        if (packed <= UINT32_MAX)
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(packed)), stubTemporaryGPRs[0]);
        else
            jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(packed)), stubTemporaryGPRs[0]);
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        context->stubCalls.call(jit, isLeaf ? static_cast<Stub>(static_cast<unsigned>(stub) + 1) : stub, CallSite { });
#if CPU(X86_64)
        CCallHelpers::Jump differs = CCallHelpers::Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE));
#else
        CCallHelpers::Jump differs = jit.branchOnFlags(CCallHelpers::NotEqual);
#endif
        CCallHelpers::Jump isSame;
        if (!params.fallsThroughToSuccessor(1))
            isSame = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            differs.linkTo(*labels[0], &jit);
            if (isSame.isSet())
                isSame.linkTo(*labels[1], &jit);
        });
    });
}

// The opening of a string that is wanted only if it is so long. The length is looked at first, of whatever kind the string is: it tells most strings from a text, and nothing else of them is loaded. Then a flat
// string goes on. A rope that is as long goes to `otherwise`.
Lowering::Characters Lowering::charactersOfStringOfLength(const StringReceiver& receiver, size_t length, LBasicBlock isNotAsLong, LBasicBlock otherwise)
{
    // Goes on where the string is as long.
    auto look = [&](Word32 found, bool isCold) {
        LBasicBlock isAsLong = isCold ? newColdBlock() : m_out.newBlock();
        m_out.branch(equal(found, word32(static_cast<uint32_t>(length))).value(), unsure(isAsLong), unsure(isNotAsLong));
        m_out.appendTo(isAsLong);
    };
    if (auto characters = charactersOfOpenedString(receiver)) {
        look(low32(characters->length), false);
        return *characters;
    }
    LBasicBlock isFlat = m_out.newBlock();
    LBasicBlock isRope = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer fiber = fiberOf(receiver);
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(isRope));
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

// The ways that are taken stand at the site as they do in a loop: the length, a flat string and a part of one, 8 bits, the characters against immediates. Any other rope and 16 bits are one call of the stub; so is a
// long text where the string is as long.
Word32 Lowering::isTextOfLengthByStub(const StringReceiver& receiver, ValueID constant, std::span<const uint8_t> text)
{
    LBasicBlock isNotAsLong = m_out.newBlock();
    LBasicBlock isAsLong = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    // `undefined` and `null` go where a string of another length goes: one block says no.
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(isNotAsLong));
        m_out.appendTo(isString);
    }
    if (text.size() > maxLengthOfTextComparedAtEverySite) {
        m_out.branch(equal(stringLength(receiver), integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
        m_out.appendTo(isNotAsLong);
        join.add(boolean(false));
        m_out.appendTo(isAsLong);
        join.add(isTextByStub(receiver.string, constant));
        return join.finish().as<Word32>();
    }
    LBasicBlock byStub = newColdBlock();
    LBasicBlock isText = m_out.newBlock();
    Characters characters = charactersOfStringOfLength(receiver, text.size(), isNotAsLong, byStub);
    m_out.jump(isAsLong);
    m_out.appendTo(isNotAsLong);
    join.add(boolean(false));
    m_out.appendTo(isAsLong);
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), byStub);
    Word32 isSame = hasText(characters.data, text);
    ensureStillAliveHere(characters.owner);
    // Branched on, the comparisons are one chain of flags.
    m_out.branch(isSame.value(), unsure(isText), unsure(isNotAsLong));
    m_out.appendTo(isText);
    join.add(boolean(true));
    m_out.appendTo(byStub);
    branchOnTextByOneCall(receiver.string.value(), true, constant, text, isText, isNotAsLong);
    return join.finish().as<Word32>();
}

// `value === "text"`, for a value that is a string, `undefined` or `null`.
Word32 Lowering::isStringWithText(ValueID id, ValueID constant, std::span<const uint8_t> text)
{
    if (auto* part = partOfStringMadeBy(id)) {
        Integer length = lengthOf(*part);
        if (text.empty())
            return isZero(length);
        LBasicBlock isAsLong = m_out.newBlock();
        LBasicBlock isNotAsLong = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(equal(length, integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
        m_out.appendTo(isAsLong);
        join.add(hasTextAt(part->base, part->from, constant, text));
        m_out.appendTo(isNotAsLong);
        join.add(boolean(false));
        return join.finish().as<Word32>();
    }
    Boxed value = in<Boxed>(id);
    // There is one empty string.
    if (text.empty())
        return equal(value, box(CellPointer(emptyString())));
    if (text.size() == 1) {
        if (auto codeUnit = codeUnitOfString(id))
            return equal(*codeUnit, integer(text[0]));
    }
    if (!m_isInLoop) {
        StringReceiver receiver = stringReceiver(id);
        if (charactersOfOpenedString(receiver) || typeOf(id).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
            return isTextOfLengthByStub(receiver, constant, text);
        LBasicBlock isText = m_out.newBlock();
        LBasicBlock isOther = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnTextByOneCall(id, constant, text, isText, isOther);
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
    // Where the characters are compared with immediates, the cell of the constant is not fetched for a look at whether it is the same cell.
    if (text.size() > maxLengthOfTextComparedInPlace) {
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
    if (text.size() > maxLengthOfTextComparedInPlace) {
        join.add(hasTextAt(receiver, integer(0), constant, text));
        return join.finish().as<Word32>();
    }
    Characters characters = charactersOf(receiver);
    join.add(either<Word32>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
        Word32 result = hasText(characters.data, text);
        ensureStillAliveHere(characters.owner);
        return result;
    }, [&] {
        return Word32(AOT_TYPED_OPERATION(StringIsText, receiver.string, word32(m_function.at(stringConstantOf(constant)).string().index())));
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

std::optional<Lowering::ComparisonWithText> Lowering::comparisonWithTextBranchedOn(const Block& block)
{
    auto& terminator = block.terminator;
    if (terminator.kind != TerminatorKind::Branch || !block.values.contains(terminator.value) || numberOfUsesOf(terminator.value) != 1)
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
                return ComparisonWithText { subject, constant, { } };
            continue;
        }
        auto text = constantASCIIText(constant);
        if (text && text->empty())
            continue;
        if (!text && isOneCodeUnitBeyondLatin1(constant))
            text = std::span<const uint8_t> { };
        if (!text || text->size() > maxLengthOfTextComparedInPlace || m_function.at(constant).opcode != Opcode::String || !isFetchedWhereNeeded(constant))
            continue;
        // There is no value: the shape says what the field holds. See lowerStrictEq().
        if (m_pinnedReads.contains(subject.index()))
            continue;
        // See isStringWithText().
        if (partOfStringMadeBy(subject))
            continue;
        // Whatever may be a string: what is none is equal to no text.
        auto& type = m_program.at(m_function.at(subject).type);
        if (!(type.primitives & Primitive::String) || !holdsCells(m_function.at(subject).rep))
            continue;
        return ComparisonWithText { subject, constant, *text };
    }
    return std::nullopt;
}

// The comparison is carried out by the Branch.
std::optional<Operand> Lowering::deferToBranchOnText(ValueID id, const Value& value)
{
    if (value.opcode != Opcode::StrictEq || m_currentBlock->terminator.value != id)
        return std::nullopt;
    // There every value is looked at by itself.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return std::nullopt;
    auto comparison = comparisonWithTextBranchedOn(*m_currentBlock);
    if (!comparison || successorsOf(m_currentBlock->terminator).size() != 2)
        return std::nullopt;
    // Then it is a comparison of two integers.
    if (constantCodeUnit(comparison->constant) && codeUnitOfString(comparison->subject))
        return std::nullopt;
    m_isFetchedLate.set(id.index());
    return Operand(value.rep, nullptr);
}

// `switch (s)` over texts, `if (s === "a") .. else if (s === "b") ..`: the blocks on the "not equal" side that do nothing but compare the same string with another text are taken in. The string is opened once; what tells
// most texts apart is how long they are.
void Lowering::lowerBranchOnText()
{
    struct Case {
        std::span<const uint8_t> text;
        ValueID constant;
        LBasicBlock taken;
    };
    Vector<Case, 8> cases;
    SetForScope isInArm(m_isInArm, true);
    const Block* block = m_currentBlock;
    ValueID subject = comparisonWithTextBranchedOn(*block)->subject;
    bool makesRopesFlat = m_isInLoop;
    for (;;) {
        auto comparison = *comparisonWithTextBranchedOn(*block);
        // The first block's own terminator has been seen to: see lowerTerminator().
        if (block != m_currentBlock) {
            openStringsInFrontOfLoopsEnteredBy(block->terminator);
            convertNumberArraysInFrontOfLoopsEnteredBy(block->terminator, true);
            convertNumberArraysInFrontOfLoopsEnteredBy(block->terminator, false);
        }
        cases.append(Case { comparison.text, comparison.constant, edgeTo(block->terminator.targets[0], true) });
        auto& next = block->terminator.targets[1];
        auto& following = m_function.at(next.block);
        if (!next.arguments.isEmpty() || !following.parameters.isEmpty() || numberOfPredecessorsOf(next.block) != 1 || cases.size() == 64)
            break;
        auto another = comparisonWithTextBranchedOn(following);
        if (!another || stringDefinedBy(another->subject) != stringDefinedBy(subject))
            break;
        bool onlyCompares = true;
        for (ValueID id : following.values)
            onlyCompares &= id == following.terminator.value || (m_function.at(id).opcode == Opcode::String && (isFetchedWhereNeeded(id) || m_function.at(id).rep == Rep::LiteralString));
        if (!onlyCompares)
            break;
        m_blocksTakenIntoBranchOnText.set(next.block.index());
        block = &following;
    }
    LBasicBlock otherwise = edgeTo(block->terminator.targets[1], true);

    if (m_function.at(subject).rep == Rep::LiteralString) {
        LValue word = rawWordOf(get(subject).as<LiteralStringWord>());
        bool goesByNumber = cases.size() >= 128;
        if (goesByNumber)
            word = m_out.lShr(m_out.sub(word, m_out.constInt64(emptyLiteralStringWord)), m_out.constInt32(literalStringShift));
        Vector<FTL::SwitchCase, 8> byWord;
        UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
        for (auto& one : cases) {
            uint32_t number = *m_program.literalStringIndex(m_function.at(one.constant).string());
            if (seen.add(number).isNewEntry)
                byWord.append(FTL::SwitchCase(m_out.constInt64(goesByNumber ? number : literalStringWord(number)), one.taken, FTL::Weight()));
        }
        if (byWord.size() == 1)
            m_out.branch(m_out.equal(word, byWord[0].value()), unsure(cases[0].taken), unsure(otherwise));
        else
            m_out.switchInstruction(word, byWord, otherwise, FTL::Weight());
        return;
    }

    std::optional<SetForScope<double>> frequencyWithoutCodeUnit;
    if (auto codeUnit = codeUnitOfString(subject, true)) {
        Vector<FTL::SwitchCase, 8> byCodeUnit;
        BitVector unitsSeen;
        bool areAllOneCodeUnit = true;
        for (auto& one : cases) {
            auto unit = constantCodeUnit(one.constant);
            areAllOneCodeUnit &= !!unit;
            if (unit && !unitsSeen.set(*unit))
                byCodeUnit.append(FTL::SwitchCase(m_out.constInt64(*unit), one.taken, FTL::Weight()));
        }
        if (areAllOneCodeUnit) {
            LBasicBlock isKept = m_out.newBlock();
            LBasicBlock isNotKept = newColdBlock();
            m_out.branch(notEqual(*codeUnit, integer(codeUnitIsNotKept)).value(), usually(isKept), rarely(isNotKept));
            m_out.appendTo(isKept);
            m_out.switchInstruction(codeUnit->value(), byCodeUnit, otherwise, FTL::Weight());
            m_out.appendTo(isNotKept);
            frequencyWithoutCodeUnit.emplace(m_out.m_frequency, 0);
        }
    }

    StringReceiver receiver = stringReceiver(subject);
    if (!makesRopesFlat && cases.size() <= maxNumberOfTextsComparedByOneCallEach && !charactersOfOpenedString(receiver) && !typeOf(subject).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull) && !cases.containsIf([](auto& one) { return one.text.empty(); })) {
        for (unsigned i = 0; i < cases.size(); ++i) {
            LBasicBlock next = i + 1 < cases.size() ? m_out.newBlock() : otherwise;
            branchOnTextByOneCall(subject, cases[i].constant, cases[i].text, cases[i].taken, next);
            if (i + 1 < cases.size())
                m_out.appendTo(next);
        }
        return;
    }
    bool mayBeNoString = !typeOf(subject).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    if (mayBeNoString) {
        receiver.flags = 0;
        if (!receiver.isString && get(subject).rep != Rep::Cell)
            receiver.isString = isCell(get(subject).as<Boxed>(), typeOf(subject));
    }
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(otherwise));
        m_out.appendTo(isString);
    }
    if (mayBeNoString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(equal(typeOfCell(receiver.string), word32(StringType)).value(), unsure(isString), unsure(otherwise));
        m_out.appendTo(isString);
    }
    auto lengthOf = [](const Case& one) -> size_t {
        return one.text.empty() ? 1 : one.text.size();
    };
    if (goesGenerallyWhereTextIsCold()) {
        if (cases.size() == 1 && !cases[0].text.empty() && !openedString(receiver.id)) {
            LBasicBlock isFlat = m_out.newBlock();
            LBasicBlock mayBeText = m_out.newBlock();
            Pointer fiber = fiberOf(receiver);
            m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(mayBeText));
            m_out.appendTo(isFlat);
            m_out.branch(equal(loadField<Word32>(fiber, m_heaps.StringImpl_length), word32(static_cast<uint32_t>(cases[0].text.size()))).value(), unsure(mayBeText), unsure(otherwise));
            m_out.appendTo(mayBeText);
            branchOnTextByOneCall(receiver.string.value(), true, cases[0].constant, cases[0].text, cases[0].taken, otherwise);
            return;
        }
        Integer lengthOfSubject = stringLength(receiver);
        for (unsigned i = 0; i < cases.size(); ++i) {
            LBasicBlock next = i + 1 < cases.size() ? m_out.newBlock() : otherwise;
            LBasicBlock isAsLong = m_out.newBlock();
            m_out.branch(equal(lengthOfSubject, integer(lengthOf(cases[i]))).value(), unsure(isAsLong), unsure(next));
            m_out.appendTo(isAsLong);
            if (cases[i].text.empty())
                m_out.branch(Word32(AOT_TYPED_OPERATION(StringEq, receiver.string, in<CellPointer>(cases[i].constant))).value(), unsure(cases[i].taken), unsure(next));
            else
                branchOnTextByOneCall(receiver.string.value(), true, cases[i].constant, cases[i].text, cases[i].taken, next);
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
    // Outside loops, where all texts are as long as each other: the length first, of whatever kind the string is: nothing else of a string of another length is loaded.
    bool looksAtLengthFirst = !makesRopesFlat && lengths.size() == 1;
    LBasicBlock wide = newColdBlock();
    // Outside loops no rope is opened or made flat: it goes with 16 bits, through the texts that are as long by the stub, which compares a part of a flat string itself.
    auto charactersOfFlatString = [&]() -> Characters {
        if (auto opened = charactersOfOpenedString(receiver))
            return *opened;
        LBasicBlock isFlat = m_out.newBlock();
        Pointer fiber = fiberOf(receiver);
        m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(wide));
        m_out.appendTo(isFlat);
        Pointer data = loadField<Pointer>(fiber, m_heaps.StringImpl_data);
        Word32 flags = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
        return { data, flags, zeroExtended(loadField<Word32>(fiber, m_heaps.StringImpl_length)), receiver.string };
    };
    Characters characters = makesRopesFlat ? charactersOf(receiver) : looksAtLengthFirst ? charactersOfStringOfLength(receiver, lengths[0], otherwise, wide) : charactersOfFlatString();
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), wide);

    bool comesBackNarrow = !makesRopesFlat && cases.size() > 1;
    for (auto& one : cases)
        comesBackNarrow &= !one.text.empty();
    LBasicBlock narrow = nullptr;
    LValue dataOfEither = nullptr;
    LValue lengthOfEither = nullptr;
    if (comesBackNarrow) {
        narrow = m_out.newBlock();
        ValueFromBlock dataOfFlat = m_out.anchor(characters.data.value());
        ValueFromBlock lengthOfFlat = m_out.anchor(characters.length.value());
        m_out.jump(narrow);
        m_out.appendTo(narrow);
        dataOfEither = m_out.phi(Int64, dataOfFlat);
        lengthOfEither = m_out.phi(Int64, lengthOfFlat);
        characters.data = Pointer(dataOfEither);
        characters.length = Integer(lengthOfEither);
    }

    Vector<LBasicBlock, 8> blocksByLength;
    Vector<FTL::SwitchCase, 8> byLength;
    for (size_t length : lengths) {
        blocksByLength.append(m_out.newBlock());
        byLength.append(FTL::SwitchCase(m_out.constInt64(length), blocksByLength.last(), FTL::Weight()));
    }
    if (looksAtLengthFirst)
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
        auto compareInTurn = [&](const Group& group) {
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
        constexpr unsigned fewestTextsToldApartByWord = 3;
        unsigned width = lengths[i] >= 4 ? 4 : lengths[i] >= 2 ? 2 : 0;
        auto wordAt = [&](const Case& one, size_t offset) {
            uint32_t word = 0;
            for (unsigned k = 0; k < width; ++k)
                word |= static_cast<uint32_t>(one.text[offset + k]) << (8 * k);
            return word;
        };
        size_t place = 0;
        unsigned mostWords = 0;
        if (width && group.size() >= fewestTextsToldApartByWord) {
            for (size_t offset = 0; offset + width <= lengths[i]; ++offset) {
                Vector<uint32_t, 8> words;
                for (auto* one : group) {
                    if (!words.contains(wordAt(*one, offset)))
                        words.append(wordAt(*one, offset));
                }
                if (words.size() > mostWords) {
                    mostWords = words.size();
                    place = offset;
                }
            }
        }
        if (mostWords < 2) {
            compareInTurn(group);
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
            compareInTurn(texts);
        }
    }

    m_out.appendTo(wide);
    if (comesBackNarrow) {
        LBasicBlock hasCharacters = newColdBlock();
        Pointer copied = AOT_TYPED_OPERATION(CharactersToCompareWithTexts, receiver.string);
        m_out.branch(m_out.notNull(copied.value()), unsure(hasCharacters), unsure(otherwise));
        m_out.appendTo(hasCharacters);
        m_out.addIncomingToPhi(dataOfEither, m_out.anchor(copied.value()));
        m_out.addIncomingToPhi(lengthOfEither, m_out.anchor((looksAtLengthFirst ? integer(lengths[0]) : stringLength(receiver)).value()));
        m_out.jump(narrow);
        return;
    }
    if (!makesRopesFlat) {
        Integer length;
        if (!looksAtLengthFirst)
            length = stringLength(receiver);
        for (unsigned i = 0; i < cases.size(); ++i) {
            LBasicBlock next = i + 1 < cases.size() ? newColdBlock() : otherwise;
            if (!looksAtLengthFirst) {
                LBasicBlock isAsLong = newColdBlock();
                m_out.branch(equal(length, integer(lengthOf(cases[i]))).value(), unsure(isAsLong), unsure(next));
                m_out.appendTo(isAsLong);
            }
            if (cases[i].text.empty())
                m_out.branch(Word32(AOT_TYPED_OPERATION(StringEq, receiver.string, in<CellPointer>(cases[i].constant))).value(), unsure(cases[i].taken), unsure(next));
            else
                branchOnTextByOneCall(receiver.string.value(), true, cases[i].constant, cases[i].text, cases[i].taken, next);
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
        Word32 isSame = cases[i].text.empty() ? Word32(AOT_TYPED_OPERATION(StringHasAt, receiver.string, in<CellPointer>(cases[i].constant), word32(0))) : Word32(AOT_TYPED_OPERATION(StringIsText, receiver.string, word32(m_function.at(stringConstantOf(cases[i].constant)).string().index())));
        m_out.branch(isSame.value(), unsure(cases[i].taken), unsure(next));
        if (i + 1 < cases.size())
            m_out.appendTo(next);
    }
}

// `a === b`, for values that are strings, `undefined` or `null`.
Word32 Lowering::areStringsEqual(ValueID a, ValueID b)
{
    if (auto text = constantASCIIText(b))
        return isStringWithText(a, b, *text);
    if (auto text = constantASCIIText(a))
        return isStringWithText(b, a, *text);
    if (auto first = codeUnitOfString(a)) {
        if (auto second = codeUnitOfString(b))
            return equal(*first, *second);
    }

    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    StringReceiver first = stringReceiver(a);
    StringReceiver second = stringReceiver(b);
    Join join(m_out, Rep::Int32, Int32);
    // Goes on where the condition does not hold.
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
    // Outside loops what has run so far is all that stands at the site: most strings that differ, differ in length. What is in place from here on is over a hundred instructions. See Stub::StringsAreEqual.
    if (!m_isInLoop) {
        join.add(Word32(m_out.castToInt32(callAllocationStub(Stub::StringsAreEqual, { { first.string.value(), stubTemporaryGPRs[2] }, { second.string.value(), stubTemporaryGPRs[3] } }, { Writes::Nothing, false }, false))));
        return join.finish().as<Word32>();
    }
    Characters p = charactersOf(first);
    Characters q = charactersOf(second);
    when(equal(p.data, q.data), boolean(true));
    // There is one atom of a text.
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

// Maps and sets.
//
// What is in a map or a set only the engine and the code here look into: it is part of the state of the run time.

// As a key a number is an int32 wherever it can be one, and what is not a number is one key.
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

// Whether two keys of the type, as mapKey() yields them, are one key exactly if they are the same bits. A number that is kept boxed
// may be a double that stands for an int32.
static bool isComparedByBits(const Type& key)
{
    if (key.primitives & (Primitive::String | Primitive::BigInt))
        return false;
    Rep rep = repOf(key);
    return !(key.primitives & Primitive::Number) || rep == Rep::Int64 || rep == Rep::Double || rep == Rep::OptionalInt64 || rep == Rep::OptionalDouble;
}

// Of a key that is compared by its bits: rapidHashMix64().
Word32 Lowering::hashOfMapKey(Boxed key)
{
    LValue a = m_out.bitXor(key.value(), fixed64(Instance::offsetOfHashSecrets()));
    LValue b = m_out.bitXor(key.value(), fixed64(Instance::offsetOfHashSecrets() + sizeof(uint64_t)));
    return Word32(m_out.castToInt32(m_out.bitXor(m_out.mul(a, b), m_out.uMulHigh(a, b))));
}

// For a key that is compared by its bits. Goes on where the table has it, and yields where: in a map the value comes next.
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

    // What is no key or value in there is an index or a count, boxed as an int32. No bits: the end of a chain.
    m_out.appendTo(hasStorage);
    Pointer data(m_out.add(storage.value(), m_out.constIntPtr(JSCellButterfly::offsetOfData())));
    Word32 capacity(m_out.load32(at(data, JSMap::Helper::capacityIndex())));
    Word32 bucket = add(bitAnd(hash, add(capacity, word32(-1))), word32(JSMap::Helper::hashTableStartIndex()));
    ValueFromBlock first = m_out.anchor(m_out.load64(at(offsetBy(data, zeroExtended(bucket), 3), 0)));
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue index = m_out.phi(Int64, first);
    m_out.branch(m_out.isZero64(index), unsure(absent), unsure(hasEntry));

    // What marks an entry as deleted is a cell no program gets hold of.
    m_out.appendTo(hasEntry);
    Pointer entry = offsetBy(data, zeroExtended(Word32(m_out.castToInt32(index))), 3);
    m_out.branch(equal(Boxed(m_out.load64(at(entry, 0))), key).value(), unsure(found), unsure(next));

    m_out.appendTo(next);
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.load64(at(entry, isMap ? JSMap::Helper::ChainOffset : JSSet::Helper::ChainOffset))));
    m_out.jump(loop);

    m_out.appendTo(found);
    return entry;
}

bool Lowering::tableStubAsksReceiver(ValueID result, const Value& value)
{
    if (m_isInLoop || value.methodRead() || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline)
        return false;
    auto& type = typeOf(result);
    bool isNotLookedAt = !numberOfUsesOf(result) && m_program.at(m_program.types.anything).isWithin(type, m_program);
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
        if (!isNotLookedAt)
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

Integer Lowering::sizeOfMapOrSet(CellPointer table, bool isMap, bool hasStringKeysByType)
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

Operand Lowering::lowerMapOrSetBuiltin(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    auto& type = std::exchange(m_mapGetYieldsAnything, false) ? m_program.at(m_program.types.anything) : typeOf(result);
    auto& arguments = value.arguments;
    auto& booleanType = m_program.at(m_program.types.boolean);
    if (which == InlineBuiltin::MapNew)
        return ensure(AOT_TYPED_OPERATION(NewMap), Type::engineClass(m_builtins.classes()[value.engineClass()]), type);

    CellPointer table = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    bool isMap = which != InlineBuiltin::SetAdd && which != InlineBuiltin::SetHas && which != InlineBuiltin::SetSize;
    bool asksReceiver = std::exchange(m_tableStubAsksReceiver, false);
    if (which == InlineBuiltin::MapSize || which == InlineBuiltin::SetSize)
        return ensure(sizeOfMapOrSet(table, isMap, value.hasStringKeysByType), Type::integer(Range::length()), type);
    auto yieldsTable = [&] {
        return ensure(table, typeOf(value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull), type);
    };
    if (which == InlineBuiltin::MapGet || which == InlineBuiltin::MapHas || which == InlineBuiltin::SetHas) {
        if (auto answer = tryLookupWithSunkKey(arguments[0], type, [&](Word32 count, Pointer parts) -> Boxed {
            return AOT_TYPED_OPERATION(TryMapOrSetLookupWithKeyParts, table, word32(which == InlineBuiltin::MapGet), count, parts);
        }, [&]() -> Operand {
            return lowerMapOrSetBuiltin(which, result, value, inPlace);
        }))
            return *answer;
    }
    Boxed key = mapKey(arguments[0]);

    // The stubs do strings in place that are no ropes and have been hashed, and leave big integers and doubles to the engine.
    if (!isComparedByBits(typeOf(arguments[0]))) {
        auto callStub = [&](Stub stub) {
            Vector<std::pair<LValue, GPRReg>, 4> operands { { table.value(), thisGPR }, { key.value(), argumentGPR(0) } };
            if (arguments.size() > 1)
                operands.append(std::pair { in<Boxed>(arguments[1]).value(), argumentGPR(1) });
            return Boxed(callICStub(asksReceiver ? askingReceiver(stub) : stub, Int64, operands, std::nullopt, asksReceiver ? OperationEffects { } : OperationEffects { inPlace, inPlace != Writes::Nothing }));
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
        return yieldsTable();
    }

    if (!m_isInLoop && !m_program.isHotOutsideItsLoops(m_id)) {
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
        Boxed yielded(callICStub(stub, Int64, operands, std::nullopt, OperationEffects { inPlace, inPlace != Writes::Nothing }));
        if (which == InlineBuiltin::MapGet)
            return ensure(yielded, type);
        if (which == InlineBuiltin::MapHas || which == InlineBuiltin::SetHas)
            return ensure(unboxedBoolean(yielded), booleanType, type);
        return yieldsTable();
    }

    Word32 hash = hashOfMapKey(key);
    LBasicBlock absent = m_out.newBlock();
    Pointer entry = findInMapOrSet(table, isMap, key, hash, absent);
    TypedPointer valueOfEntry = m_out.address(m_heaps.OrderedHashTableData, entry.value(), sizeof(EncodedJSValue));
    switch (which) {
    case InlineBuiltin::MapGet: {
        Rep rep = repOf(type);
        Join join(m_out, rep, b3TypeOf(rep));
        join.add(ensure(Boxed(m_out.load64(valueOfEntry)), type));
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
    OperationEffects effectsOfAdding { Writes::RuntimeState, true };
    if (isMap) {
        didEmitWrite(Writes::RuntimeState);
        m_out.store64(in<Boxed>(arguments[1]).value(), valueOfEntry);
        if (typeOf(arguments[1]).hasCells())
            barrier(loadField<CellPointer>(table, m_heaps.JSMap_storage));
    }
    join.add({ });
    m_out.appendTo(absent);
    if (isMap)
        callOperation<operationAOTMapSet>(Entry::operationAOTMapSet, effectsOfAdding, table, key, in<Boxed>(arguments[1]), hash);
    else
        callOperation<operationAOTSetAdd>(Entry::operationAOTSetAdd, effectsOfAdding, table, key, hash);
    join.add({ });
    join.finish();
    return yieldsTable();
}

bool Lowering::holdsResultOfMapGetBehindJoin(const Value& value)
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

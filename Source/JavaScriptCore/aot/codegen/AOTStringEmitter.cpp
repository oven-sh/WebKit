/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRepresentationRules.h"
#include "AOTResolvedCallEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringTable.h"
#include "AOTStubsStrings.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "JSCJSValueInlines.h"
#include "JSString.h"
#include "SmallStrings.h"
#include <algorithm>
#include <cmath>
#include <unicode/utf16.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT { namespace Typed {


void StringEmitter::continueIf(Condition condition, BlockEdge otherwise)
{
    LBasicBlock continuation = newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), otherwise);
    m_out.appendToContinuation(continuation);
}

Word32 StringEmitter::word32Of(ValueID id)
{
    Operand operand = get(id);
    if (operand.is<Word32>())
        return operand.as<Word32>();
    return low32(operand.as<Integer>());
}

String StringEmitter::textOf(StringID id) const
{
    Vector<char16_t> bufferForNonASCII;
    String text = m_generator.inputs().types.viewOf(id, bufferForNonASCII).toString();
    if (!text.is8Bit() && text.containsOnlyLatin1())
        return String::make8Bit(text.span16());
    return text;
}

void StringEmitter::recordInlined(StringID id)
{
    if (m_records.inlinedStrings.areRecorded()) [[unlikely]]
        m_records.inlinedStrings.append(types().bytesOf(id));
}

void StringEmitter::recordInlined(char16_t codeUnit)
{
    if (m_records.inlinedStrings.areRecorded()) [[unlikely]]
        m_records.inlinedStrings.append(encodeProgramString(std::span<const char16_t>(singleElementSpan(codeUnit))).span());
}

Word32 StringEmitter::flagsInImpl(Pointer impl)
{
    return loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags);
}

CharacterSource StringEmitter::sourceInImpl(Pointer impl)
{
    Pointer characters = loadField<Pointer>(impl, m_heaps.StringImpl_data);
    Word32 length = loadField<Word32>(impl, m_heaps.StringImpl_length);
    return { characters, length, flagsInImpl(impl) };
}

std::optional<FlatStringCell> StringEmitter::flatStringByType(ValueID string)
{
    if (!typeOf(string).isOpaque(OpaqueKind::FlatString))
        return std::nullopt;
    return FlatStringCell(in<CellPointer>(string));
}

Pointer StringEmitter::implOfFlatString(FlatStringCell flatString)
{
    CellPointer flat = flatString.cell();
    if (auto opened = m_openedStrings.find(flat.value()); opened != m_openedStrings.end())
        return opened->value.impl;
    Pointer impl(m_out.loadPtr(asPointer(flat).value(), m_heaps.JSString_value));
    return impl;
}

CharacterView StringEmitter::viewOfSource(const CharacterSource& source, CellPointer owner)
{
    bool isFoundNarrow = m_flagsFoundNarrow.contains(source.implFlags.value());
    if (const OperationContext* context = generator().contextBeingGenerated(); context && (isFoundNarrow || m_flagsFoundWide.contains(source.implFlags.value())) && emitter<ElementAccessEmitter>().checksBounds()) {
        Word32 flags(m_out.opaque(source.implFlags.value()));
        emitter<ThrowEmitter>().requireOrCrash(*context, isFoundNarrow ? hasBits(flags, StringImpl::flagIs8Bit()) : hasNoBits(flags, StringImpl::flagIs8Bit()), std::nullopt, ProofSource::RememberedFact);
        build().statistics().count(StatisticID::UnrolledLoopTypeProofsChecked);
    }
    Condition is8Bit = m_flagsFoundNarrow.contains(source.implFlags.value()) ? boolean(true) : m_flagsFoundWide.contains(source.implFlags.value()) ? boolean(false) : hasBits(source.implFlags, StringImpl::flagIs8Bit());
    return { source.characters, source.length, is8Bit, owner };
}

bool StringEmitter::widthIsFoundAtRun(ValueID flatString)
{
    auto opened = m_openedStrings.find(in<CellPointer>(flatString).value());
    return opened != m_openedStrings.end() && !opened->value.source.implFlags.value()->hasInt();
}

std::optional<Condition> StringEmitter::findWidth(ValueID flatString, bool isNarrow)
{
    auto opened = m_openedStrings.find(in<CellPointer>(flatString).value());
    if (opened == m_openedStrings.end())
        return std::nullopt;
    Word32 flags = opened->value.source.implFlags;
    Vector<LValue, 2>& found = isNarrow ? m_flagsFoundNarrow : m_flagsFoundWide;
    if (found.contains(flags.value()))
        return boolean(true);
    Condition hasWidth = isNarrow ? hasBits(flags, StringImpl::flagIs8Bit()) : hasNoBits(flags, StringImpl::flagIs8Bit());
    if (hasWidth.value()->isInt(0))
        return std::nullopt;
    found.append(flags.value());
    return hasWidth;
}

CharacterView StringEmitter::viewOfFlatString(CellPointer flat)
{
    return viewOfSource(sourceInImpl(loadField<Pointer>(flat, m_heaps.JSString_value)), flat);
}

CharacterSource StringEmitter::sourceOfRope(const OperationContext& context, CellPointer rope)
{
    Word32 length = loadField<Word32>(rope, m_heaps.JSRopeString_length);
    StubCallResult charactersAndImpl = callStub(context, ropeCharacters(), { rope.value() });
    return { Pointer(charactersAndImpl.first), length, flagsInImpl(Pointer(charactersAndImpl.second)) };
}

CharacterView StringEmitter::viewOfString(const OperationContext& context, CellPointer string)
{
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    LBasicBlock continuation = newBlock();
    Vector<ValueFromBlock, 2> characters;
    Vector<ValueFromBlock, 2> lengths;
    Vector<ValueFromBlock, 2> implFlags;
    auto join = [&](const CharacterSource& source) {
        characters.append(m_out.anchor(source.characters.value()));
        lengths.append(m_out.anchor(source.length.value()));
        implFlags.append(m_out.anchor(source.implFlags.value()));
        m_out.jump(continuation);
    };

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isFlat.value(), normalEdge(flat), unlikelyEdge(rope));

    m_out.appendToArm(flat, fork);
    join(sourceInImpl(fiber));

    m_out.appendToArm(rope, fork);
    join(sourceOfRope(context, string));

    m_out.appendToJoin(continuation, fork);
    Pointer joinedCharacters(m_out.phi(machineTypeOf(Pointer::representation), characters));
    Word32 joinedLength(m_out.phi(machineTypeOf(Word32::representation), lengths));
    Word32 joinedFlags(m_out.phi(machineTypeOf(Word32::representation), implFlags));
    return viewOfSource({ joinedCharacters, joinedLength, joinedFlags }, string);
}

CharacterView StringEmitter::viewWithoutResolving(const OperationContext& context, CellPointer string, BlockEdge otherwise)
{
    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    if (!context.block() || !generator().usesLoopForms(context.block())) {
        continueIf(isFlat, otherwise);
        return viewOfSource(sourceInImpl(fiber), string);
    }

    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    LBasicBlock substring = newBlock();
    LBasicBlock continuation = newBlock();
    Vector<ValueFromBlock, 2> characters;
    Vector<ValueFromBlock, 2> lengths;
    Vector<ValueFromBlock, 2> implFlags;
    auto join = [&](const CharacterSource& source) {
        characters.append(m_out.anchor(source.characters.value()));
        lengths.append(m_out.anchor(source.length.value()));
        implFlags.append(m_out.anchor(source.implFlags.value()));
        m_out.jump(continuation);
    };
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isFlat.value(), normalEdge(flat), unlikelyEdge(rope));

    m_out.appendToArm(flat, fork);
    join(sourceInImpl(fiber));

    m_out.appendToArm(rope, fork);
    m_out.branch(m_out.testNonZeroPtr(fiber.value(), JSRopeString::isSubstringInPointer), normalEdge(substring), otherwise);

    m_out.appendToContinuation(substring);
    StubCallResult found = callStub(context, substringCharacters(), { string.value() });
    join({ Pointer(found.first), loadField<Word32>(string, m_heaps.JSRopeString_length), flagsInImpl(Pointer(found.second)) });

    m_out.appendToJoin(continuation, fork);
    Pointer joinedCharacters(m_out.phi(machineTypeOf(Pointer::representation), characters));
    Word32 joinedLength(m_out.phi(machineTypeOf(Word32::representation), lengths));
    Word32 joinedFlags(m_out.phi(machineTypeOf(Word32::representation), implFlags));
    return viewOfSource({ joinedCharacters, joinedLength, joinedFlags }, string);
}

CellPointer StringEmitter::makeSubstring(const OperationContext& context, const OwnedCharacters& characters)
{
    return substring(context, characters.owner, { characters.start, add(characters.start, characters.source.length) });
}

CellPointer StringEmitter::makeCarriedSubstring(const ValueContext& cut, const CodeGenerator::CarriedSubstring& carried)
{
    Machine::DeclaredAccessesSuspension suspension(m_out);
    CellPointer whole = in<CellPointer>(carried.string);
    Word32 start(carried.start);
    Word32 length(carried.length);
    LBasicBlock isPart = newBlock();
    Join<CellPointer> join(m_out);
    Condition isWhole = logicalAnd(isZero(start), equal(length, lengthOf(whole)));
    LBasicBlock isAll = newBlock();
    m_out.branch(isWhole.value(), normalEdge(isAll), normalEdge(isPart));
    m_out.appendTo(isAll);
    join.add(whole);
    m_out.appendTo(isPart);
    join.add(CellPointer(callStub(cut, AOT::substring(), { whole.value(), zeroExtended(start).value(), zeroExtended(add(start, length)).value() }).first));
    return join.finish();
}

OwnedCharacters StringEmitter::loadCharacters(const OperationContext& context, CellPointer string, std::optional<SubstringBounds> bounds)
{
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    LBasicBlock substring = newBlock();
    LBasicBlock unresolved = newBlock();
    LBasicBlock continuation = newBlock();
    Vector<ValueFromBlock, 3> characters;
    Vector<ValueFromBlock, 3> lengths;
    Vector<ValueFromBlock, 3> implFlags;
    Vector<ValueFromBlock, 3> owners;
    Vector<ValueFromBlock, 3> starts;
    auto join = [&](Pointer impl, CellPointer owner, std::optional<Integer> offset, Word32 lengthOfString) {
        Word32 flags = flagsInImpl(impl);
        Pointer firstOfOwner = loadField<Pointer>(impl, m_heaps.StringImpl_data);
        if (bounds)
            offset = offset ? Integer(m_out.add(offset->value(), zeroExtended(bounds->start).value())) : zeroExtended(bounds->start);
        Condition is8Bit = hasBits(flags, StringImpl::flagIs8Bit());
        characters.append(m_out.anchor(offset ? select(is8Bit, offsetBy(firstOfOwner, *offset, 0), offsetBy(firstOfOwner, *offset, 1)).value() : firstOfOwner.value()));
        lengths.append(m_out.anchor(bounds ? bounds->length.value() : lengthOfString.value()));
        implFlags.append(m_out.anchor(flags.value()));
        owners.append(m_out.anchor(owner.value()));
        starts.append(m_out.anchor(offset ? low32(*offset).value() : m_out.constInt32(0)));
        m_out.jump(continuation);
    };
    auto lengthInImpl = [&](Pointer impl) {
        return bounds ? bounds->length : loadField<Word32>(impl, m_heaps.StringImpl_length);
    };
    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), normalEdge(flat), normalEdge(rope));

    m_out.appendToArm(flat, fork);
    join(fiber, string, std::nullopt, lengthInImpl(fiber));

    m_out.appendToArm(rope, fork);
    m_out.branch(m_out.testNonZeroPtr(fiber.value(), JSRopeString::isSubstringInPointer), normalEdge(substring), unlikelyEdge(unresolved));

    m_out.appendToContinuation(substring);
    using CompactFibers = JSRopeString::CompactFibers;
    static_assert(CompactFibers::widthOfFiber1LowerInLengthWord == 32 && CompactFibers::shiftOfFiber2InUpperWord == 16 && JSRopeString::offsetOfFiber2() == JSRopeString::offsetOfFiber1Lower() + 4);
    const Machine::AbstractHeap& storage = m_heaps.entire(AbstractHeap::StringStorage);
    LValue lower = m_out.zeroExtPtr(m_out.load32(m_out.address(storage, string.value(), JSRopeString::offsetOfFiber1Lower())));
    LValue upper = m_out.zeroExtPtr(m_out.load16ZeroExt32(m_out.address(storage, string.value(), JSRopeString::offsetOfFiber2())));
    LValue offset = m_out.lShr(m_out.load64(m_out.address(storage, string.value(), JSRopeString::offsetOfFiber2())), CompactFibers::shiftOfFiber2InUpperWord);
    CellPointer base(m_out.bitOr(lower, m_out.shl(upper, CompactFibers::widthOfFiber1LowerInLengthWord)));
    join(loadField<Pointer>(base, m_heaps.JSString_value), base, Integer(offset), bounds ? bounds->length : loadField<Word32>(string, m_heaps.JSRopeString_length));

    m_out.appendTo(unresolved);
    CellPointer resolved(callStub(context, flattenString(), { string.value() }).first);
    Pointer implOfResolved = loadField<Pointer>(resolved, m_heaps.JSString_value);
    join(implOfResolved, resolved, std::nullopt, lengthInImpl(implOfResolved));

    m_out.appendToJoin(continuation, fork);
    Pointer joinedCharacters(m_out.phi(machineTypeOf(Pointer::representation), characters));
    Word32 joinedLength(m_out.phi(machineTypeOf(Word32::representation), lengths));
    Word32 joinedFlags(m_out.phi(machineTypeOf(Word32::representation), implFlags));
    CellPointer joinedOwner(m_out.phi(machineTypeOf(CellPointer::representation), owners));
    Word32 joinedStart(m_out.phi(machineTypeOf(Word32::representation), starts));
    return { { joinedCharacters, joinedLength, joinedFlags }, joinedOwner, joinedStart };
}

CharacterView StringEmitter::viewOf(const OperationContext& context, ValueID string)
{
    if (const CodeGenerator::CarriedSubstring* carried = generator().findCarriedSubstring(string)) [[unlikely]]
        return viewOfCharacters(emitter<ResolvedCallEmitter>().loadCarriedSubstring(context, *carried, false));
    std::optional<FlatStringCell> flat = flatStringByType(string);
    if (!flat)
        return viewOfString(context, in<CellPointer>(string));
    CellPointer cell = flat->cell();
    if (auto opened = m_openedStrings.find(cell.value()); opened != m_openedStrings.end()) {
        if (!opened->value.mayBeRope)
            return viewOfSource(opened->value.source, cell);
        return viewOfPossibleRope(string, context.block(), cell);
    }
    return viewOfSource(sourceInImpl(implOfFlatString(*flat)), cell);
}

CharacterView StringEmitter::viewForReadOfCodeUnit(const OperationContext& context, ValueID string)
{
    if (opcodeOf(string) != Opcode::OpenString || !generator().mayStayRope(string))
        return viewOf(context, string);
    CharacterView view = viewOfPossibleRope(string, context.block(), in<CellPointer>(string));
    view.resolvesAt = &context;
    return view;
}

CharacterView StringEmitter::viewOfPossibleRope(ValueID string, BlockID block, CellPointer cell)
{
    const OpenedRope& rope = openedRopeAt(string, block);
    CharacterView view = viewOfSource({ rope.characters, Word32(), rope.implFlags }, cell);
    view.possibleRope = string;
    view.implFlags = rope.implFlags;
    return view;
}

Word32 StringEmitter::lengthOf(CellPointer string)
{
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    Join<Word32> join(m_out);

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isFlat.value(), normalEdge(flat), unlikelyEdge(rope));

    m_out.appendToArm(flat, fork);
    Word32 lengthInImpl = loadField<Word32>(fiber, m_heaps.StringImpl_length);
    ensureStillAliveHere(string);
    join.add(lengthInImpl);

    m_out.appendToArm(rope, fork);
    join.add(loadField<Word32>(string, m_heaps.JSRopeString_length));

    return join.finishBehind(fork);
}

Word32 StringEmitter::lengthOf(ValueID string)
{
    if (const CodeGenerator::CarriedSubstring* carried = generator().findCarriedSubstring(string)) [[unlikely]]
        return Word32(carried->length);
    std::optional<FlatStringCell> flat = flatStringByType(string);
    if (!flat)
        return lengthOf(in<CellPointer>(string));
    CellPointer cell = flat->cell();
    if (auto opened = m_openedStrings.find(cell.value()); opened != m_openedStrings.end())
        return opened->value.source.length;
    Word32 length = loadField<Word32>(implOfFlatString(*flat), m_heaps.StringImpl_length);
    ensureStillAliveHere(cell);
    return length;
}

Word32 StringEmitter::loadCodeUnit(Pointer characters, Word32 index, bool is8Bit, std::optional<Integer> indexInWholeNumber)
{
    LValue offset = indexInWholeNumber ? indexInWholeNumber->value() : zeroExtended(index).value();
    if (is8Bit)
        return Word32(m_out.load8ZeroExt32(m_out.baseIndex(m_heaps.characters8, characters.value(), offset)));
    return Word32(m_out.load16ZeroExt32(m_out.baseIndex(m_heaps.characters16, characters.value(), offset)));
}

const StringEmitter::OpenedRope& StringEmitter::openedRopeAt(ValueID string, BlockID block) const
{
    for (size_t index = m_openedRopes.size(); index--;) {
        const OpenedRope& opened = m_openedRopes[index];
        if (opened.string != string || !m_generator.blockDominates(opened.block, block))
            continue;
        EdgeID handler = opened.isRead && opened.block != block ? m_code.handlerOf(opened.block) : EdgeID();
        if (!handler || !m_generator.blockDominates(opened.block, m_code.at(handler).to))
            return opened;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StringEmitter::passOpenedRope(ValueID string, BlockID from, LValue characters, LValue implFlags)
{
    const OpenedRope& opened = openedRopeAt(string, from);
    m_out.upsilon(opened.characters.value(), characters);
    m_out.upsilon(opened.implFlags.value(), implFlags);
}

Word32 StringEmitter::codeUnitOfPossibleRope(const CharacterView& view, Word32 index)
{
    LBasicBlock narrow = newBlock();
    LBasicBlock other = newBlock();
    LBasicBlock wide = newBlock();
    LBasicBlock unresolved = newBlock();
    LBasicBlock resolved = newBlock();
    LBasicBlock rope = newBlock();
    LBasicBlock continuation = newBlock();
    Vector<ValueFromBlock, 4> units;
    Vector<ValueFromBlock, 4> characters;
    Vector<ValueFromBlock, 4> implFlags;
    auto join = [&](Word32 unit, Pointer charactersOfUnit, Word32 flagsOfUnit) {
        ensureStillAliveHere(view.owner);
        units.append(m_out.anchor(unit.value()));
        characters.append(m_out.anchor(charactersOfUnit.value()));
        implFlags.append(m_out.anchor(flagsOfUnit.value()));
        m_out.jump(continuation);
    };
    m_out.branch(view.is8Bit.value(), normalEdge(narrow), unlikelyEdge(other));
    m_out.appendTo(narrow);
    join(loadCodeUnit(view.characters, index, true), view.characters, view.implFlags);

    m_out.appendTo(other);
    m_out.branch(m_out.notZero64(view.characters.value()), normalEdge(wide), unlikelyEdge(unresolved));
    m_out.appendTo(wide);
    join(loadCodeUnit(view.characters, index, false), view.characters, view.implFlags);

    m_out.appendTo(unresolved);
    if (view.resolvesAt)
        callStub(*view.resolvesAt, flattenString(), { view.owner.value() });
    LValue operands[] = { view.owner.value(), index.value() };
    Word32 unit = low32(Integer(emitter<RuntimeCallEmitter>().callStubWithoutSite(stringCodeUnitAt(), operands).first));
    Pointer fiber = loadField<Pointer>(view.owner, m_heaps.JSString_value);
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), normalEdge(resolved), rareEdge(rope));
    m_out.appendTo(resolved);
    join(unit, loadField<Pointer>(fiber, m_heaps.StringImpl_data), flagsInImpl(fiber));
    m_out.appendTo(rope);
    join(unit, view.characters, view.implFlags);

    m_out.appendTo(continuation);
    m_openedRopes.append({ view.possibleRope, generator().blockBeingGenerated(), Pointer(m_out.phi(machineTypeOf(Pointer::representation), characters)), Word32(m_out.phi(machineTypeOf(Word32::representation), implFlags)), true });
    return Word32(m_out.phi(machineTypeOf(Word32::representation), units));
}

Word32 StringEmitter::codeUnitAt(const CharacterView& view, Word32 index, std::optional<Integer> indexInWholeNumber)
{
    if (view.possibleRope)
        return codeUnitOfPossibleRope(view, index);
    return selectByWidth<Word32>(view.is8Bit, [&](bool is8Bit) {
        Word32 codeUnit = loadCodeUnit(view.characters, index, is8Bit, indexInWholeNumber);
        ensureStillAliveHere(view.owner);
        return codeUnit;
    });
}

Word32 StringEmitter::codePointAt(const CharacterView& view, Word32 index)
{
    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    LBasicBlock isLead = newBlock();
    LBasicBlock hasNext = newBlock();
    LBasicBlock isPair = newBlock();
    LBasicBlock isSingleUnit = newBlock();
    Join<Word32> join(m_out);
    auto isSurrogate = [&](Word32 unit, int32_t first) {
        return equal(bitAnd(unit, ~0x3ff), first);
    };

    ScopeFork fork = m_out.forkScope();
    m_out.branch(view.is8Bit.value(), normalEdge(narrow), unlikelyEdge(wide));

    m_out.appendToArm(narrow, fork);
    join.add(loadCodeUnit(view.characters, index, true));

    m_out.appendToArm(wide, fork);
    Word32 lead = loadCodeUnit(view.characters, index, false);
    m_out.branch(isSurrogate(lead, 0xd800).value(), normalEdge(isLead), normalEdge(isSingleUnit));

    m_out.appendTo(isLead);
    Word32 next = add(index, word32(1));
    m_out.branch(below(next, view.length).value(), normalEdge(hasNext), unlikelyEdge(isSingleUnit));

    m_out.appendTo(hasNext);
    Word32 trail = loadCodeUnit(view.characters, next, false);
    m_out.branch(isSurrogate(trail, 0xdc00).value(), normalEdge(isPair), unlikelyEdge(isSingleUnit));

    m_out.appendTo(isPair);
    join.add(sub(add(shiftLeft(lead, 10), trail), U16_SURROGATE_OFFSET));

    m_out.appendTo(isSingleUnit);
    join.add(lead);

    Word32 codePoint = join.finishBehind(fork);
    ensureStillAliveHere(view.owner);
    return codePoint;
}

CellPointer StringEmitter::stringFromCodeUnit(const OperationContext& context, Word32 codeUnit)
{
    static_assert(maxSingleCharacterString == 0xff);
    LBasicBlock isLatin1 = newBlock();
    LBasicBlock isNotLatin1 = newBlock();
    Join<CellPointer> join(m_out);

    Condition fitsLatin1 = belowOrEqual(codeUnit, word32(maxSingleCharacterString));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(fitsLatin1.value(), normalEdge(isLatin1), unlikelyEdge(isNotLatin1));

    m_out.appendToArm(isLatin1, fork);
    join.add(singleCharacterString(codeUnit));

    m_out.appendToArm(isNotLatin1, fork);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::CharToString, { codeUnit }));

    return join.finishBehind(fork);
}

CellPointer StringEmitter::substring(const OperationContext& context, CellPointer string, CodeUnitRange range)
{
    LValue end = range.end.value();
    if (end->opcode() == Machine::Add && end->child(0) == range.start.value() && end->child(1)->isInt32(1))
        return CellPointer(callStub(context, characterAt(), { string.value(), zeroExtended(range.start).value() }).first);
    return CellPointer(callStub(context, AOT::substring(), { string.value(), zeroExtended(range.start).value(), zeroExtended(range.end).value() }).first);
}

static std::optional<LValue> integerBehindStringPosition(Number position)
{
    if (position.value()->opcode() != Machine::IToD)
        return std::nullopt;
    return position.value()->child(0);
}

Integer StringEmitter::truncateToInteger(Number position)
{
    if (position.value()->hasDouble()) {
        double constant = position.value()->asDouble();
        return integer(std::isnan(constant) ? 0 : static_cast<int64_t>(std::clamp(constant, -0x1p32, 0x1p32)));
    }
    if (std::optional<LValue> source = integerBehindStringPosition(position))
        return (*source)->type() == Machine::Int32 ? signExtended(Word32(*source)) : Integer(*source);
#if !CPU(ARM64)
    Number limit = number(static_cast<double>(1ll << 32));
    Number negativeLimit = number(-static_cast<double>(1ll << 32));
    position = select(equal(position, position), position, 0);
    position = select(greaterThan(position, limit), limit, position);
    position = select(lessThan(position, negativeLimit), negativeLimit, position);
    return emitter<ConversionEmitter>().toInteger(position);
#else
    return emitter<ConversionEmitter>().truncateUnchecked(position);
#endif
}

StringPosition StringEmitter::truncatedPositionOf(Number position, StringRangeMethod method)
{
#if !CPU(ARM64)
    bool isClamped = method == StringRangeMethod::Slice || method == StringRangeMethod::Substring || method == StringRangeMethod::Substr;
    if (isClamped && !position.value()->hasDouble() && !integerBehindStringPosition(position))
        return { .value = emitter<ConversionEmitter>().toInteger(position), .numberBehindBareConversion = position };
#else
    UNUSED_PARAM(method);
#endif
    return { .value = truncateToInteger(position) };
}

StringPosition StringEmitter::truncatedPositionOf(ValueID id, StringRangeMethod method)
{
    Type type = typeOf(id);
    Range range = type.isInteger() ? type.integers : Range::full();
    Operand operand = get(id);
    if (!operand.is<Number>())
        return { .value = integerOf(id), .range = range };
    StringPosition position = truncatedPositionOf(operand.as<Number>(), method);
    position.range = range;
    return position;
}

Word32 StringEmitter::clampToLength(const StringPosition& position, Word32 length, bool isRelativeToEnd)
{
    Integer x = position.value;
    Integer limit = zeroExtended(length);
    auto countedFromStart = [&] {
        return low32(select(greaterThan(x, limit), limit, x));
    };
    auto countedFromEnd = [&] {
        if (!isRelativeToEnd)
            return word32(0);
        Integer sum = add(limit, x);
        return low32(select(lessThan(sum, 0), 0, sum));
    };

    Range range = x.value()->hasInt() ? Range::one(x.value()->asInt()) : position.range;
    if (range.minimum >= 0 || range.maximum < 0) {
        if (range == Range::one(0))
            return word32(0);
        return range.minimum >= 0 ? countedFromStart() : countedFromEnd();
    }

    LBasicBlock isNotNegative = newBlock();
    LBasicBlock isNegative = newBlock();
    Join<Word32> join(m_out);
    Condition positionIsNotNegative = greaterThanOrEqual(x, integer(0));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(positionIsNotNegative.value(), normalEdge(isNotNegative), unlikelyEdge(isNegative));
    m_out.appendToArm(isNotNegative, fork);
    join.add(countedFromStart());
    m_out.appendToArm(isNegative, fork);
    Word32 clamped = countedFromEnd();
    if (position.numberBehindBareConversion)
        clamped = select(greaterThan(*position.numberBehindBareConversion, number(0)), length, clamped);
    join.add(clamped);
    return join.finishBehind(fork);
}

CodeUnitRange StringEmitter::clampRange(StringRangeMethod method, Word32 length, Number first, std::optional<Number> second)
{
    std::optional<StringPosition> truncatedSecond;
    if (second)
        truncatedSecond = truncatedPositionOf(*second, method);
    return clampRange(method, length, truncatedPositionOf(first, method), truncatedSecond);
}

CodeUnitRange StringEmitter::clampRange(StringRangeMethod method, Word32 length, StringPosition first, std::optional<StringPosition> second)
{
    auto singleUnit = [&](bool isRelativeToEnd) -> CodeUnitRange {
        Word32 index = low32(indexInString(first.value, length, isRelativeToEnd));
        Word32 start = select(lessThan(index, 0), 0, index);
        return { start, add(index, word32(1)) };
    };

    switch (method) {
    case StringRangeMethod::Slice: {
        Word32 from = clampToLength(first, length, true);
        if (!second)
            return { from, length };
        Word32 to = clampToLength(*second, length, true);
        if (from.value()->isInt32(0))
            return { from, to };
        return { from, select(lessThan(to, from), from, to) };
    }
    case StringRangeMethod::Substring: {
        Word32 from = clampToLength(first, length, false);
        if (!second)
            return { from, length };
        Word32 to = clampToLength(*second, length, false);
        if (from.value()->isInt32(0))
            return { from, to };
        Condition isReversed = greaterThan(from, to);
        return { select(isReversed, to, from), select(isReversed, from, to) };
    }
    case StringRangeMethod::Substr: {
        Word32 from = clampToLength(first, length, true);
        Word32 available = sub(length, from);
        Word32 count = second ? clampToLength(*second, available, false) : available;
        return { from, add(from, count) };
    }
    case StringRangeMethod::SingleIndex:
        return singleUnit(false);
    case StringRangeMethod::SingleRelativeIndex:
        return singleUnit(true);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Integer StringEmitter::indexInString(Integer x, Word32 length, bool isRelativeToEnd)
{
    Integer limit = zeroExtended(length);
    if (isRelativeToEnd) {
        Condition countsFromEnd = lessThan(x, 0);
        x = select(countsFromEnd, add(x, limit), x);
    }
    return select(below(x, limit), x, -1);
}

void StringEmitter::generateSubstring(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    const CodeGenerator::CarriedSubstring* carried = generator().findCarriedSubstring(value->stringOperand());
    if (ElementAccessEmitter& elements = emitter<ElementAccessEmitter>(); elements.checksBounds()) {
        elements.stopUnlessInside(value, belowOrEqual(start, end));
        elements.stopUnlessInside(value, belowOrEqual(end, carried ? Word32(carried->length) : lengthOf(string)));
    }
    if (carried) [[unlikely]] {
        OwnedCharacters characters = emitter<ResolvedCallEmitter>().loadCarriedSubstring(value, *carried);
        Word32 startInOwner = add(characters.start, start);
        define(value, generator().carrySubstring(value.id(), startInOwner.value(), sub(end, start).value()) ? characters.owner : substring(value, characters.owner, { startInOwner, add(characters.start, end) }));
        return;
    }
    define(value, substring(value, string, { start, end }));
}

void StringEmitter::generateSubstringOrUndefined(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    define(value, selectByBranch<Boxed>(logicalAnd(greaterThanOrEqual(start, word32(0)), greaterThanOrEqual(end, start)), [&] {
        if (ElementAccessEmitter& elements = emitter<ElementAccessEmitter>(); elements.checksBounds()) {
            elements.stopUnlessInside(value, belowOrEqual(start, end));
            elements.stopUnlessInside(value, belowOrEqual(end, lengthOf(string)));
        }
        return boxCell(substring(value, string, { start, end }));
    }, [&] {
        return undefined();
    }));
}

void StringEmitter::generateBoundsOfStringSplit(const ValueContext& value)
{
    uint32_t count = value->count();
    RELEASE_ASSERT(count < (1u << 31));
    CharacterView view = viewOf(value, value->stringOperand());
    static_assert(2 * sizeof(uint32_t) == sizeof(EncodedJSValue));
    Pointer bounds = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(std::max(count, 1u));
    Word32 countAndWhetherAllAreCounted = word32(static_cast<int32_t>(count | (value->countsEveryPiece() ? 1u << 31 : 0)));
    String separator = textOf(value->literal());
    bool isOneCodeUnit = separator.length() == 1;
    if (isOneCodeUnit)
        recordInlined(value->literal());
    Word32 sought = isOneCodeUnit ? word32(separator[0]) : numberOf(m_mentions.add(NumberSpace::String, value->literal()));
    Word32 numberOfPieces = selectByWidth<Word32>(view.is8Bit, [&](bool is8Bit) {
        RuntimeOperation operation = isOneCodeUnit
            ? (is8Bit ? RuntimeOperation::StringFindSeparators8 : RuntimeOperation::StringFindSeparators16)
            : (is8Bit ? RuntimeOperation::StringFindLongSeparators8 : RuntimeOperation::StringFindLongSeparators16);
        return low32(callLeaf<Integer>(operation, { view.characters, view.length, sought, countAndWhetherAllAreCounted, bounds }));
    });
    ensureStillAliveHere(view.owner);
    Vector<Operand, 9> results;
    for (uint32_t bound = 0; bound < 2 * count; ++bound)
        results.append(Word32(m_out.load32(m_out.address(m_heaps.argumentBuffer, bounds.value(), bound * sizeof(uint32_t)))));
    results.append(numberOfPieces);
    defineResults(value, results.span());
}

void StringEmitter::generateNewStringSplitArray(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StringSplitString, { in<CellPointer>(value->stringOperand()), in<CellPointer>(value->separatorOperand()), word32(-1) }));
}

CellPointer StringEmitter::openPossibleRope(const ValueContext& value, CellPointer string)
{
    LBasicBlock isFlat = newBlock();
    LBasicBlock isRope = newBlock();
    LBasicBlock continuation = newBlock();

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isAlreadyFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isAlreadyFlat.value(), normalEdge(isFlat), unlikelyEdge(isRope));

    m_out.appendToArm(isFlat, fork);
    ValueFromBlock cellWhenFlat = m_out.anchor(string.value());
    ValueFromBlock charactersWhenFlat = m_out.anchor(loadField<Pointer>(fiber, m_heaps.StringImpl_data).value());
    ValueFromBlock flagsWhenFlat = m_out.anchor(flagsInImpl(fiber).value());
    m_out.jump(continuation);

    m_out.appendToArm(isRope, fork);
    ValueFromBlock cellWhenRope = m_out.anchor(string.value());
    ValueFromBlock charactersWhenRope = m_out.anchor(m_out.constIntPtr(0));
    ValueFromBlock flagsWhenRope = m_out.anchor(word32(0).value());
    m_out.jump(continuation);

    m_out.appendToJoin(continuation, fork);
    CellPointer opened(m_out.phi(machineTypeOf(CellPointer::representation), cellWhenFlat, cellWhenRope));
    Pointer characters(m_out.phi(machineTypeOf(Pointer::representation), charactersWhenFlat, charactersWhenRope));
    Word32 flags(m_out.phi(machineTypeOf(Word32::representation), flagsWhenFlat, flagsWhenRope));
    m_openedStrings.add(opened.value(), OpenedString { fiber, { characters, Word32(), flags }, true });
    m_openedRopes.append({ value.id(), value.block(), characters, flags });
    generator().forEachCarryingLoop(value.id(), [&](BlockID head, LValue carriedCharacters, LValue carriedFlags) {
        m_openedRopes.append({ value.id(), head, Pointer(carriedCharacters), Word32(carriedFlags) });
    });
    return opened;
}

void StringEmitter::generateOpenString(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    define(value, generator().mayStayRope(value.id()) ? openPossibleRope(value, string) : string);
}

void StringEmitter::generateFlattenString(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    if (generator().mayStayRope(value.id())) {
        define(value, openPossibleRope(value, string));
        return;
    }

    LBasicBlock isFlat = newBlock();
    LBasicBlock isRope = newBlock();
    LBasicBlock continuation = newBlock();

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isAlreadyFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isAlreadyFlat.value(), normalEdge(isFlat), unlikelyEdge(isRope));

    m_out.appendToArm(isFlat, fork);
    ValueFromBlock cellWhenFlat = m_out.anchor(string.value());
    Vector<ValueFromBlock, 3> cells { cellWhenFlat };
    Vector<ValueFromBlock, 3> impls { m_out.anchor(fiber.value()) };
    m_out.jump(continuation);
    auto join = [&](CellPointer cell) {
        cells.append(m_out.anchor(cell.value()));
        impls.append(m_out.anchor(loadField<Pointer>(cell, m_heaps.JSString_value).value()));
        m_out.jump(continuation);
    };

    m_out.appendToArm(isRope, fork);
    LBasicBlock isNotRead = value->isReadOperands().empty() ? nullptr : newBlock();
    for (ValueID isRead : value->isReadOperands()) {
        LBasicBlock mayBeRead = newBlock();
        m_out.branch(in<Condition>(isRead).value(), normalEdge(mayBeRead), normalEdge(isNotRead));
        m_out.appendTo(mayBeRead);
    }
    join(CellPointer(callStub(value, flattenString(), { string.value() }).first));
    if (isNotRead) {
        m_out.appendTo(isNotRead);
        join(emptyString());
    }

    m_out.appendToJoin(continuation, fork);
    CellPointer flat(m_out.phi(machineTypeOf(CellPointer::representation), cells));
    Pointer impl(m_out.phi(machineTypeOf(Pointer::representation), impls));
    m_openedStrings.add(flat.value(), OpenedString { impl, sourceInImpl(impl) });
    define(value, flat);
}

void StringEmitter::generateTryResolveRope(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    LBasicBlock isFlat = newBlock();
    LBasicBlock isRope = newBlock();
    LBasicBlock continuation = newBlock();
    Vector<ValueFromBlock, 3> outcomes;
    m_out.branch(isRopeString(string).value(), unlikelyEdge(isRope), normalEdge(isFlat));

    m_out.appendTo(isRope);
    for (ValueID isRead : value->isReadOperands()) {
        LBasicBlock mayBeRead = newBlock();
        m_out.branch(in<Condition>(isRead).value(), normalEdge(mayBeRead), normalEdge(isFlat));
        m_out.appendTo(mayBeRead);
    }
    Condition isResolved = callWithoutSite<Condition>(RuntimeOperation::TryResolveRope, { string });
    outcomes.append(m_out.anchor(isResolved.value()));
    m_out.jump(continuation);

    m_out.appendTo(isFlat);
    outcomes.append(m_out.anchor(boolean(true).value()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    defineTest(value, Condition(m_out.phi(Machine::Int32, outcomes)));
}

void StringEmitter::generateStringRange(const ValueContext& value)
{
    StringRangeMethod method = value->method();
    Word32 length = word32Of(value->lengthOperand());
    StringPosition first = truncatedPositionOf(value->firstOperand(), method);
    std::optional<StringPosition> second;
    if (ValueID operand = value->secondOperand())
        second = truncatedPositionOf(operand, method);
    CodeUnitRange range = clampRange(method, length, first, second);
    defineResults(value, { range.start, range.end });
}

StringID StringEmitter::findNumberedStringConstant(const OperationContext& context, ValueID id)
{
    if (opcodeOf(id) != Opcode::String)
        return { };
    StringID constant = operationAt(id).string();
    return emitter<ConstantEmitter>().loadsStringConstantByStub(context, constant) ? constant : StringID();
}

CellPointer StringEmitter::cellOf(const OperationContext& context, const StringPart& part)
{
    if (part.integer)
        return integerToString(context, *part.integer);
    return part.cell ? *part.cell : emitter<ConstantEmitter>().stringConstant(context, part.constant);
}

std::optional<ConstantsOfConcatenation> StringEmitter::chooseConstants(std::span<const StringPart> parts)
{
    auto isConstant = [&](size_t index) {
        return parts[index].isConstant();
    };
    if (parts.size() == 2) {
        if (isConstant(0))
            return ConstantsOfConcatenation::FirstOfTwo;
        return isConstant(1) ? std::optional(ConstantsOfConcatenation::SecondOfTwo) : std::nullopt;
    }
    if (isConstant(0))
        return isConstant(2) ? ConstantsOfConcatenation::FirstAndThirdOfThree : ConstantsOfConcatenation::FirstOfThree;
    if (isConstant(2))
        return ConstantsOfConcatenation::ThirdOfThree;
    return isConstant(1) ? std::optional(ConstantsOfConcatenation::SecondOfThree) : std::nullopt;
}

std::optional<CellPointer> StringEmitter::tryConcatenateInteger(const OperationContext& context, std::span<const StringPart> parts)
{
    auto formOf = [&]() -> std::optional<ConcatenatedInteger> {
        auto isString = [&](size_t index) {
            return !parts[index].integer && !parts[index].unconverted;
        };
        if (parts.size() == 2) {
            if (parts[0].integer && isString(1))
                return parts[1].isConstant() ? ConcatenatedInteger::IntegerThenConstant : ConcatenatedInteger::IntegerThenString;
            if (parts[1].integer && isString(0))
                return parts[0].isConstant() ? ConcatenatedInteger::ConstantThenInteger : ConcatenatedInteger::StringThenInteger;
            return std::nullopt;
        }
        if (parts[2].integer && isString(0) && isString(1)) {
            if (parts[0].isConstant())
                return ConcatenatedInteger::ConstantStringInteger;
            return parts[1].isConstant() ? ConcatenatedInteger::StringConstantInteger : ConcatenatedInteger::StringsThenInteger;
        }
        if (!parts[1].integer || !isString(0) || !isString(2))
            return std::nullopt;
        if (parts[0].isConstant())
            return parts[2].isConstant() ? ConcatenatedInteger::ConstantIntegerConstant : ConcatenatedInteger::ConstantIntegerString;
        return parts[2].isConstant() ? ConcatenatedInteger::StringIntegerConstant : ConcatenatedInteger::StringIntegerString;
    };
    std::optional<ConcatenatedInteger> form = formOf();
    if (!form)
        return std::nullopt;
    Vector<LValue, 3> operands;
    for (const StringPart& part : parts)
        operands.append(part.integer ? part.integer->value() : part.isConstant() && !(form == ConcatenatedInteger::ConstantStringInteger && &part == &parts[1]) ? m_out.wideNumber(m_mentions.add(NumberSpace::StringCell, part.constant)) : cellOf(context, part).value());
    return CellPointer(callStub(context, concatenateInteger(*form), operands.span()).first);
}

CellPointer StringEmitter::concatenateTwoOrThree(const OperationContext& context, std::span<const StringPart> parts)
{
    if (std::optional<CellPointer> joined = tryConcatenateInteger(context, parts))
        return *joined;
    std::optional<ConstantsOfConcatenation> constants = chooseConstants(parts);
    Vector<LValue, 3> operands;
    bool converts = false;
    for (unsigned index = 0; index < parts.size(); ++index) {
        if (constants && isConstantOperand(*constants, index))
            operands.append(m_out.wideNumber(m_mentions.add(NumberSpace::StringCell, parts[index].constant)));
        else if (parts[index].unconverted) {
            RELEASE_ASSERT(!converts && index == lastOperandThatIsNoConstant(constants, static_cast<unsigned>(parts.size())));
            converts = true;
            operands.append(parts[index].unconverted->value());
        } else
            operands.append(cellOf(context, parts[index]).value());
    }
    if (converts) {
        const StubKey stub = convertAndConcatenate(constants, static_cast<unsigned>(parts.size()));
        return CellPointer(generator().withConversionLeftToConcatenation(context, [&](const OperationContext& conversion) {
            return callStub(conversion, stub, operands.span()).first;
        }));
    }
    StubKey stub = constants ? concatenateWithConstants(*constants) : AOT::concatenate(static_cast<unsigned>(parts.size()));
    return CellPointer(callStub(context, stub, operands.span()).first);
}

CellPointer StringEmitter::concatenate(const OperationContext& context, std::span<const StringPart> parts)
{
    if (parts.empty())
        return emptyString();
    if (parts.size() == 1)
        return cellOf(context, parts[0]);

    bool startsWithRepeatedString = parts[0].cell && parts[1].cell && parts[0].cell->value() == parts[1].cell->value();
    size_t numberOfPartsJoined = std::min<size_t>(parts.size(), startsWithRepeatedString ? 2 : 3);
    CellPointer joined = concatenateTwoOrThree(context, parts.first(numberOfPartsJoined));
    while (numberOfPartsJoined < parts.size()) {
        Vector<StringPart, 3> next { joined, parts[numberOfPartsJoined++] };
        if (numberOfPartsJoined < parts.size())
            next.append(parts[numberOfPartsJoined++]);
        joined = concatenateTwoOrThree(context, next.span());
    }
    return joined;
}

CellPointer StringEmitter::concatenateKey(const OperationContext& context, std::span<const Boxed> primitives, KeyUse keyUse)
{
    if (primitives.size() == 2)
        return CellPointer(callStub(context, concatenateKeyOfPair(keyUse == KeyUse::NameOfProperty ? ConcatenatedKeyForm::Name : ConcatenatedKeyForm::Key), { primitives[0].value(), primitives[1].value() }).first);
    UseOfJoinedText use = keyUse == KeyUse::NameOfProperty ? UseOfJoinedText::LikelyName : UseOfJoinedText::LikelyKey;
    return callOperation<CellPointer>(context, RuntimeOperation::Concatenate, { word32(static_cast<int32_t>(primitives.size())), argumentBuffer(primitives), word32(static_cast<int32_t>(use)) });
}

CellPointer StringEmitter::integerToString(const OperationContext& context, Integer value)
{
    return CellPointer(callStub(context, AOT::integerToString(), { value.value() }).first);
}

CellPointer StringEmitter::numberToString(const OperationContext& context, Number value)
{
    return callOperation<CellPointer>(context, RuntimeOperation::DoubleToString, { value });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

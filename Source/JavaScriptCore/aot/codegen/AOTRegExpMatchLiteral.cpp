/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolvedCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConstantEmitter.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTStringEmitter.h"
#include "AOTResultsJoin.h"
#include "AOTStubsRegExp.h"
#include "AOTInstance.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "RegExp.h"
#include "RegExpObject.h"
#include "VM.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned maxRangesOfFirstCharacters = 3;
static constexpr unsigned maxFirstCharacters = 32;

struct RangeOfFirstCharacters {
    unsigned first;
    unsigned last;
};

static Vector<RangeOfFirstCharacters, maxRangesOfFirstCharacters + 1> rangesOfFirstCharacters(const WTF::BitSet<256>& characters, bool isIn)
{
    Vector<RangeOfFirstCharacters, maxRangesOfFirstCharacters + 1> ranges;
    for (unsigned first = characters.findBit(0, isIn); first < 256 && ranges.size() <= maxRangesOfFirstCharacters;) {
        unsigned end = characters.findBit(first, !isIn);
        ranges.append({ first, end - 1 });
        first = end < 256 ? characters.findBit(end, isIn) : 256;
    }
    return ranges;
}

template<typename T> T ResolvedCallEmitter::matchByFirstCharacter(const ValueContext& value, CellPointer string, T miss, NOESCAPE const Invocable<T()> auto& match)
{
    if (!m_generator.usesLoopForms(value.block()) || value->regExpFlags().containsAny(flagsOfMatchesUsingObject))
        return match();
    StringEmitter& strings = emitter<StringEmitter>();
    const RegExpSummary& summary = build().regExpSummaries().summaryOf(value->pattern(), toYarrFlags(value->regExpFlags()), strings.textOf(value->pattern()));
    if (!summary.firstCharacters)
        return match();
    const ValueID asked = definitionOf(value->stringOperand());
    if (!m_stringsAskedFirstCharacter.contains(asked)) {
        if (summary.firstCharacters->count() > maxFirstCharacters)
            return match();
        m_stringsAskedFirstCharacter.append(asked);
    } else if (summary.firstCharacters->count() == 256)
        return match();
    Vector<RangeOfFirstCharacters, maxRangesOfFirstCharacters + 1> ranges = rangesOfFirstCharacters(*summary.firstCharacters, true);
    const bool rangesMatch = ranges.size() <= maxRangesOfFirstCharacters;
    if (!rangesMatch)
        ranges = rangesOfFirstCharacters(*summary.firstCharacters, false);
    if (ranges.size() > maxRangesOfFirstCharacters)
        return match();

    LBasicBlock matches = newBlock();
    LBasicBlock misses = newBlock();
    Join<T> join(m_out);
    ScopeFork fork = m_out.forkScope();
    CharacterView view = strings.viewWithoutResolving(value, string, unlikelyEdge(matches));
    if (!summary.firstCharactersAreLatin1)
        strings.continueIf(view.is8Bit, unlikelyEdge(matches));
    strings.continueIf(isNotZero(view.length), unlikelyEdge(misses));
    CharacterView narrowView { view.characters, view.length, boolean(true), view.owner };
    Word32 character = strings.codeUnitAt(narrowView, word32(0));
    for (size_t index = 0; index < ranges.size(); ++index) {
        auto [first, last] = ranges[index];
        Condition isInRange = first == last ? equal(character, word32(static_cast<int32_t>(first))) : belowOrEqual(add(character, -static_cast<int32_t>(first)), static_cast<int32_t>(last - first));
        LBasicBlock next = index + 1 < ranges.size() ? newBlock() : rangesMatch ? misses : matches;
        m_out.branch(isInRange.value(), normalEdge(rangesMatch ? matches : misses), normalEdge(next));
        if (index + 1 < ranges.size())
            m_out.appendTo(next);
    }
    if (ranges.isEmpty())
        m_out.jump(rangesMatch ? misses : matches);

    m_out.appendTo(misses);
    join.add(miss);

    m_out.appendTo(matches);
    join.add(match());
    return join.finishBehind(fork);
}

static constexpr int32_t maxLengthSearchedInline = 64;

static constexpr unsigned maxCostInPlace = 14;

const RegExpMatchPlan* ResolvedCallEmitter::matchPlanOf(BlockID block, const RegExpMatch& match)
{
    if (!block || !m_generator.usesLoopForms(block))
        return nullptr;
    const bool isSearchedAtStart = match.resetsLastIndex && match.flags.contains(RegExpFlags::Sticky);
    if (isSearchedAtStart || (match.usesLastIndex() && !match.isHeld) || match.flags.contains(RegExpFlags::HasIndices))
        return nullptr;
    const RegExpMatchPlan* plan = build().regExpSummaries().matchPlanOf(match.pattern, toYarrFlags(match.flags), emitter<StringEmitter>().textOf(match.pattern));
    return plan && (plan->cost <= maxCostInPlace || match.usesLastIndex()) ? plan : nullptr;
}

Pointer ResolvedCallEmitter::matchOutput()
{
    return Pointer(m_out.loadPtr(m_out.address(m_heaps.entire(AbstractHeap::RegExpState), vm().value(), VM::offsetOfAOTState() + VMState::offsetOfRegExpMatchOutput())));
}

void ResolvedCallEmitter::storeBounds(RegExpMatchEmitter& matcher, const RegExpMatchEmitter::Bounds& bounds)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    Pointer output = matchOutput();
    for (size_t index = 0; index < bounds.size(); ++index) {
        Word32 bound = index < 2 ? matcher.indexOf(bounds[index]) : matcher.indexOfGroupBound(bounds[index]);
        m_out.store32(bound.value(), m_out.address(state, output.value(), index * sizeof(int)));
    }
}

Boxed ResolvedCallEmitter::createMatchArray(const OperationContext& context, const RegExpMatch& match)
{
    const RegExpSummary& summary = build().regExpSummaries().summaryOf(match.pattern, toYarrFlags(match.flags), emitter<StringEmitter>().textOf(match.pattern));
    Word32 record = numberOf(m_mentions.regExp(match.pattern, toYarrFlags(match.flags)));
    if (!match.regExpObject && summary.hasNamedGroups()) {
        CellPointer noObject(pointerConstant(0).value());
        return callOperation<Boxed>(context, RuntimeOperation::RegExpMatchLiteral, { noObject, match.string, record, word32(static_cast<int32_t>(RegExpMatchForm::Exec)) });
    }
    auto finish = [&](FinishRegExpExecForm withObject, FinishRegExpExecForm withoutObject) {
        if (match.regExpObject)
            return Boxed(callStub(context, finishRegExpExec(withObject), { match.regExpObject.value(), match.string.value() }).first);
        return Boxed(callStub(context, finishRegExpExec(withoutObject), { record.value(), match.string.value(), m_out.constInt32(summary.numSubpatterns) }).first);
    };
    LBasicBlock isFlat = newBlock();
    LBasicBlock isSubstring = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(isRopeString(match.string).value(), unlikelyEdge(isSubstring), normalEdge(isFlat));
    m_out.appendTo(isFlat);
    join.add(finish(FinishRegExpExecForm::WithObject, FinishRegExpExecForm::WithoutObject));
    m_out.appendTo(isSubstring);
    join.add(finish(FinishRegExpExecForm::OnSubstringWithObject, FinishRegExpExecForm::OnSubstringWithoutObject));
    return join.finish();
}

CharacterView ResolvedCallEmitter::viewOfSubject(const OperationContext& context, const RegExpMatch& match, LBasicBlock calls)
{
    StringEmitter& strings = emitter<StringEmitter>();
    if (match.isHeld) {
        const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
        strings.continueIf(isZero(Word32(m_out.load8ZeroExt32(mutableInstanceField(AOT_INSTANCE_OFFSET(regExpsAreRecompiled))))), rareEdge(calls));
        if (match.isArgument)
            strings.continueIf(equal(loadStructureID(match.regExpObject), loadFilledOnce32(Instance::offsetOfReceiverStructureID(Receiver::RegExp))), rareEdge(calls));
        Boxed lastIndex(m_out.load64(m_out.address(state, match.regExpObject.value(), RegExpObject::offsetOfLastIndex())));
        if (match.resetsLastIndex)
            strings.continueIf(equal(lastIndex, boxedConstant(jsNumber(0))), unlikelyEdge(calls));
        else
            strings.continueIf(match.usesLastIndex() ? isInt32(lastIndex) : isNumber(lastIndex), unlikelyEdge(calls));
        if (match.usesLastIndex() || match.flags.contains(RegExpFlags::Global)) {
            LValue regExpAndFlags = m_out.load64(m_out.address(state, match.regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags()));
            strings.continueIf(Condition(m_out.testIsZero64(regExpAndFlags, RegExpObject::lastIndexIsNotWritableFlag)), rareEdge(calls));
        }
    }
    CharacterView view = strings.viewWithoutResolving(context, match.string, unlikelyEdge(calls));
    strings.continueIf(view.is8Bit, unlikelyEdge(calls));
    return view;
}

template<typename T> T ResolvedCallEmitter::matchInPlace(const OperationContext& context, const RegExpMatchPlan& plan, const RegExpMatch& match, RegExpMatchAnswer answer, T miss, const ScopedLambda<T()>& call)
{
    const bool recordsMatch = m_facts.mayReadLegacyRegExpStatics();
    const bool keepsGroups = answer == RegExpMatchAnswer::Array || answer == RegExpMatchAnswer::Parts;
    StringEmitter& strings = emitter<StringEmitter>();
    LBasicBlock calls = newBlock();
    LBasicBlock misses = newBlock();
    Join<T> join(m_out);
    ScopeFork fork = m_out.forkScope();

    const bool usesLastIndex = match.usesLastIndex();
    const TypedPointer lastIndexOfObject = match.regExpObject ? m_out.address(m_heaps.entire(AbstractHeap::RegExpState), match.regExpObject.value(), RegExpObject::offsetOfLastIndex()) : TypedPointer();
    CharacterView view = viewOfSubject(context, match, calls);
    RegExpMatchEmitter matcher(m_generator, plan, view.characters, view.length, keepsGroups ? RegExpMatchEmitter::Groups::Kept : RegExpMatchEmitter::Groups::Ignored);
    RegExpMatchEmitter::Begin begin = !usesLastIndex ? RegExpMatchEmitter::Begin::AtStart : match.flags.contains(RegExpFlags::Sticky) ? RegExpMatchEmitter::Begin::AtPlace : RegExpMatchEmitter::Begin::AtOrAfterPosition;
    Word32 length = view.length;
    LValue first { nullptr };
    if (usesLastIndex) {
        Word32 firstIndex = low32(Integer(m_out.load64(lastIndexOfObject)));
        strings.continueIf(belowOrEqual(firstIndex, view.length), unlikelyEdge(calls));
        first = matcher.addressAt(firstIndex);
        length = sub(view.length, firstIndex);
    }
    if (matcher.isSearched(begin))
        strings.continueIf(belowOrEqual(length, maxLengthSearchedInline), normalEdge(calls));
    RegExpMatchEmitter::Bounds bounds = matcher.match(misses, begin, first);

    if (keepsGroups)
        storeBounds(matcher, bounds);
    ensureStillAliveHere(view.owner);
    if (usesLastIndex && answer != RegExpMatchAnswer::Array)
        m_out.store64(boxInt32(matcher.indexOf(bounds[1])).value(), lastIndexOfObject);
    if (recordsMatch && answer != RegExpMatchAnswer::Array)
        callStub(context, regExpMatch(RegExpMatchResult::Record), { match.regExpObject.value(), match.string.value(), bounds[0], bounds[1], view.characters.value() });
    if constexpr (std::is_same_v<T, Condition>)
        join.add(boolean(true));
    else
        join.add(answer == RegExpMatchAnswer::Array ? createMatchArray(context, match) : answer == RegExpMatchAnswer::Index ? boxInt32(matcher.indexOf(bounds[0])) : boxedConstant(jsBoolean(true)));
    if (misses->numberOfIncomingEdges()) {
        m_out.appendTo(misses);
        ensureStillAliveHere(view.owner);
        if (usesLastIndex)
            m_out.store64(m_out.constInt64(JSValue::encode(jsNumber(0))), lastIndexOfObject);
        join.add(miss);
    }
    m_out.appendTo(calls);
    join.add(call());
    return join.finishBehind(fork);
}

template Boxed ResolvedCallEmitter::matchInPlace(const OperationContext&, const RegExpMatchPlan&, const RegExpMatch&, RegExpMatchAnswer, Boxed, const ScopedLambda<Boxed()>&);
template Condition ResolvedCallEmitter::matchInPlace(const OperationContext&, const RegExpMatchPlan&, const RegExpMatch&, RegExpMatchAnswer, Condition, const ScopedLambda<Condition()>&);

static JSValue missOfMatchForm(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Parts:
        return jsBoolean(false);
    case RegExpMatchForm::Search:
        return jsNumber(-1);
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        return jsNull();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static RegExpMatchAnswer answerOfMatchForm(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return RegExpMatchAnswer::Boolean;
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        return RegExpMatchAnswer::Array;
    case RegExpMatchForm::Search:
        return RegExpMatchAnswer::Index;
    case RegExpMatchForm::Parts:
        return RegExpMatchAnswer::Parts;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ResolvedCallEmitter::generateRegExpMatchLiteral(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    ValueID regExp = value->regExpOperand();
    bool recordsMatch = m_facts.mayReadLegacyRegExpStatics();
    if (!regExp && (value->regExpFlags().containsAny(flagsOfMatchesUsingObject) || recordsMatch)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    CellPointer string = in<CellPointer>(value->stringOperand());
    NumberedImmediate record { m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())) };
    Boxed miss(m_out.constInt64(JSValue::encode(missOfMatchForm(value->matchForm()))));
    auto call = [&] {
        if (!regExp)
            return Boxed(callStub(value, regExpMatchLiteralWithoutObject(value->matchForm()), { string.value() }, { .numberedImmediate = record }).first);
        StubKey stub = regExpMatchLiteral(value->matchForm(), value->regExpFlags().contains(RegExpFlags::Global), recordsMatch);
        return Boxed(callStub(value, stub, { in<CellPointer>(regExp).value(), string.value() }, { .numberedImmediate = record }).first);
    };
    RegExpMatch match { value->pattern(), value->regExpFlags(), string, regExp ? in<CellPointer>(regExp) : CellPointer() };
    match.resetsLastIndex = value->matchForm() == RegExpMatchForm::Search;
    const bool listsMatches = value->matchForm() == RegExpMatchForm::Match && value->regExpFlags().contains(RegExpFlags::Global);
    if (const RegExpMatchPlan* plan = listsMatches ? nullptr : matchPlanOf(value.block(), match)) {
        define(value, matchInPlace<Boxed>(value, *plan, match, answerOfMatchForm(value->matchForm()), miss, call));
        return;
    }
    define(value, matchByFirstCharacter<Boxed>(value, string, miss, call));
}

void ResolvedCallEmitter::generateBoundsOfRegExpMatch(const ValueContext& value)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    Pointer output = matchOutput();
    Vector<Operand, 8> results;
    for (uint32_t bound = 0; bound < 2 * value->count(); ++bound)
        results.append(Word32(m_out.load32(m_out.address(state, output.value(), bound * sizeof(int)))));
    defineResults(value, results.span());
}

void ResolvedCallEmitter::generateRegExpMatchLiteralInParts(const ValueContext& value)
{
    ValueID regExp = value->regExpOperand();
    bool recordsMatch = m_facts.mayReadLegacyRegExpStatics();
    if (!regExp && (value->regExpFlags().containsAny(flagsOfMatchesUsingObject) || recordsMatch)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, boolean(false));
        return;
    }

    CellPointer string = in<CellPointer>(value->stringOperand());
    NumberedImmediate record { m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())) };
    auto call = [&] {
        if (!regExp)
            return unboxBoolean(Boxed(callStub(value, regExpMatchLiteralWithoutObject(RegExpMatchForm::Parts), { string.value() }, { .numberedImmediate = record }).first));
        StubKey stub = regExpMatchLiteral(RegExpMatchForm::Parts, value->regExpFlags().contains(RegExpFlags::Global), recordsMatch);
        return unboxBoolean(Boxed(callStub(value, stub, { in<CellPointer>(regExp).value(), string.value() }, { .numberedImmediate = record }).first));
    };
    RegExpMatch match { value->pattern(), value->regExpFlags(), string, regExp ? in<CellPointer>(regExp) : CellPointer() };
    if (const RegExpMatchPlan* plan = matchPlanOf(value.block(), match)) {
        define(value, matchInPlace<Condition>(value, *plan, match, RegExpMatchAnswer::Parts, boolean(false), call));
        return;
    }
    define(value, matchByFirstCharacter<Condition>(value, string, boolean(false), call));
}

void ResolvedCallEmitter::generateMatchRegExpIntoFrame(const ValueContext& value)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    CellPointer regExpObject = in<CellPointer>(value->regExpOperand());
    CellPointer string = in<CellPointer>(value->stringOperand());
    LValue slot = m_out.lockedStackSlot(sizeof(uint64_t) * (1 + maxPartsOfMatchInFrame));
    slot->slot()->setIsDeadAtSuspensions(true);
    LBasicBlock isInFrame = newBlock();
    LBasicBlock isElsewhere = newBlock();
    LBasicBlock isInOutput = newBlock();
    LBasicBlock isAbsent = newBlock();
    LBasicBlock fits = newBlock();
    LBasicBlock isLarge = newBlock();
    LBasicBlock loop = newBlock();
    LBasicBlock next = newBlock();
    LBasicBlock isCopied = newBlock();
    Join<Pointer> join(m_out);

    LValue found = callStub(value, regExpMatch(RegExpMatchResult::PartsInFrame), { regExpObject.value(), string.value(), slot }, { .calledBuiltin = functionOf(value->overload()) }).first;
    m_out.branch(m_out.equal(found, matchIsInFrame), normalEdge(isInFrame), normalEdge(isElsewhere));
    m_out.appendTo(isInFrame);
    join.add(Pointer(slot));

    m_out.appendTo(isElsewhere);
    branch(unboxBoolean(Boxed(found)), { unlikelyEdge(isInOutput), normalEdge(isAbsent) });
    m_out.appendTo(isAbsent);
    join.add(pointerConstant(0));

    m_out.appendTo(isInOutput);
    LValue regExp = m_out.bitAnd(m_out.load64(m_out.address(state, regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags())), static_cast<int64_t>(RegExpObject::regExpMask));
    LValue count = m_out.add(m_out.zeroExtPtr(m_out.load32(m_out.address(state, regExp, RegExp::offsetOfNumSubpatterns()))), 1);
    m_out.branch(m_out.belowOrEqual(count, maxPartsOfMatchInFrame), normalEdge(fits), rareEdge(isLarge));

    m_out.appendTo(fits);
    Pointer output = matchOutput();
    m_out.store64(count, m_out.address(state, slot));
    ValueFromBlock first = m_out.anchor(m_out.int64Zero);
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue index = m_out.phi(Machine::Int64, first);
    LValue offset = m_out.shl(index, 3);
    m_out.store64(m_out.load64(m_out.address(state, m_out.add(output.value(), offset))), m_out.address(state, m_out.add(slot, offset), sizeof(uint64_t)));
    LValue following = m_out.add(index, 1);
    m_out.branch(m_out.below(following, count), normalEdge(next), normalEdge(isCopied));
    m_out.appendTo(next);
    m_out.addIncomingToPhi(index, m_out.anchor(following));
    m_out.jump(loop);

    m_out.appendTo(isCopied);
    join.add(Pointer(slot));

    m_out.appendTo(isLarge);
    join.add(callOperation<Pointer>(value, RuntimeOperation::CopyRegExpMatch, { Word32(m_out.castToInt32(count)) }));
    define(value, join.finish());
}

void ResolvedCallEmitter::generateIsMatchInFrame(const ValueContext& value)
{
    define(value, Condition(m_out.notZero64(in<Pointer>(value->matchOperand()).value())));
}

void ResolvedCallEmitter::generateLengthOfMatchInFrame(const ValueContext& value)
{
    Pointer record = in<Pointer>(value->matchOperand());
    define(value, Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::RegExpState), record.value()))));
}

void ResolvedCallEmitter::generateBoundsOfMatchInFrame(const ValueContext& value)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    Pointer record = in<Pointer>(value->matchOperand());
    Operand operand = get(value->indexOperand());
    Integer index = operand.is<Word32>() ? signExtended(operand.as<Word32>()) : operand.as<Integer>();
    LBasicBlock isInside = newBlock();
    LBasicBlock isOutside = newBlock();
    ResultsJoin<Word32, Word32> join(m_out);
    LValue count = m_out.load64(m_out.address(state, record.value()));
    m_out.branch(m_out.below(index.value(), count), normalEdge(isInside), unlikelyEdge(isOutside));
    m_out.appendTo(isInside);
    LValue place = m_out.add(record.value(), m_out.shl(index.value(), 3));
    join.add(Word32(m_out.load32(m_out.address(state, place, sizeof(uint64_t)))), Word32(m_out.load32(m_out.address(state, place, sizeof(uint64_t) + sizeof(uint32_t)))));
    m_out.appendTo(isOutside);
    join.add(word32(-1), word32(-1));
    auto bounds = join.finish();
    defineResults(value, bounds);
}

void ResolvedCallEmitter::generateNewArrayOfRegExpMatch(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    CellPointer regExpObject = in<CellPointer>(value->regExpOperand());
    define(value, unboxCell(callOperation<Boxed>(value, RuntimeOperation::RegExpMatchArray, { regExpObject, string })));
}

void ResolvedCallEmitter::generateNewRegExpMatchArray(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 record = numberOf(m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())));
    const RegExpSummary& summary = build().regExpSummaries().summaryOf(value->pattern(), toYarrFlags(value->regExpFlags()), emitter<StringEmitter>().textOf(value->pattern()));
    if (!summary.hasNamedGroups()) {
        Word32 numberOfGroups = word32(static_cast<int32_t>(summary.numSubpatterns));
        define(value, unboxCell(Boxed(callStub(value, finishRegExpExec(FinishRegExpExecForm::WithoutObject), { record.value(), string.value(), numberOfGroups.value() }).first)));
        return;
    }
    CellPointer noObject(pointerConstant(0).value());
    define(value, unboxCell(callOperation<Boxed>(value, RuntimeOperation::RegExpMatchLiteral, { noObject, string, record, word32(static_cast<int32_t>(RegExpMatchForm::Exec)) })));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

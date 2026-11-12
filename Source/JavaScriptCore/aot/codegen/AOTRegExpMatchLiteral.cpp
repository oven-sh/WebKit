/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolvedCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
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
#include "JSGlobalObject.h"
#include "RegExp.h"
#include "RegExpCachedResult.h"
#include "RegExpGlobalData.h"
#include "RegExpMatchesArray.h"
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

bool ResolvedCallEmitter::isMatchOf(ValueID operation, ValueID string) const
{
    Opcode opcode = opcodeOf(operation);
    return (opcode == Opcode::RegExpMatchLiteral || opcode == Opcode::RegExpMatchLiteralInParts) && (!string || definitionOf(operationAt(operation).stringOperand()) == string);
}

bool ResolvedCallEmitter::isMatchedSeveralTimes(ValueID string)
{
    if (!m_subjectsInLoops) {
        m_subjectsInLoops.emplace();
        for (uint32_t block = 0; block < m_code.numberOfBlocks(); ++block) {
            if (!m_code.isReached(BlockID(block)) || !m_generator.usesLoopForms(BlockID(block)))
                continue;
            for (ValueID operation : m_code.valuesOf(BlockID(block))) {
                if (isMatchOf(operation, { }))
                    m_subjectsInLoops->append(definitionOf(operationAt(operation).stringOperand()));
            }
        }
    }
    return std::ranges::count(*m_subjectsInLoops, string) > 1;
}

std::optional<OwnedCharacters> ResolvedCallEmitter::findLoadedSubject(const OperationContext& context, CellPointer string)
{
    if (m_generator.keepsValuesInHeapFrame())
        return std::nullopt;
    for (const LoadedSubject& loaded : m_loadedSubjects) {
        if (loaded.string != string.value() || !m_generator.blockDominates(loaded.block, context.block()))
            continue;
        EdgeID handler = loaded.block != context.block() ? m_code.handlerOf(loaded.block) : EdgeID();
        if (!handler || !m_generator.blockDominates(loaded.block, m_code.at(handler).to))
            return loaded.characters;
    }
    return std::nullopt;
}

const CodeGenerator::CarriedSubstring* ResolvedCallEmitter::carriedSubjectOf(const RegExpMatch& match) const
{
    return match.subject ? m_generator.findCarriedSubstring(match.subject) : nullptr;
}

OwnedCharacters ResolvedCallEmitter::loadCarriedSubstring(const OperationContext& context, const CodeGenerator::CarriedSubstring& carried, bool isBeforeBranches)
{
    const bool isNamedByItsOwner = !!carried.block;
    CellPointer string = in<CellPointer>(carried.string);
    if (std::optional<OwnedCharacters> loaded = isNamedByItsOwner ? findLoadedSubject(context, string) : std::nullopt)
        return *loaded;
    OwnedCharacters characters = emitter<StringEmitter>().loadCharacters(context, string, SubstringBounds { Word32(carried.start), Word32(carried.length) });
    if (isBeforeBranches && isNamedByItsOwner)
        m_loadedSubjects.append({ context.block(), string.value(), characters });
    return characters;
}

CellPointer ResolvedCallEmitter::subjectOf(const OperationContext& context, const RegExpMatch& match)
{
    const CodeGenerator::CarriedSubstring* carried = carriedSubjectOf(match);
    return carried ? emitter<StringEmitter>().makeSubstring(context, loadCarriedSubstring(context, *carried)) : match.string;
}

std::optional<OwnedCharacters> ResolvedCallEmitter::loadSubject(const OperationContext& context, const RegExpMatch& match)
{
    if (!match.subject)
        return std::nullopt;
    if (const CodeGenerator::CarriedSubstring* carried = carriedSubjectOf(match))
        return loadCarriedSubstring(context, *carried);
    if (std::optional<OwnedCharacters> loaded = findLoadedSubject(context, match.string))
        return loaded;
    if (m_generator.keepsValuesInHeapFrame() || !isMatchedSeveralTimes(definitionOf(match.subject)))
        return std::nullopt;
    OwnedCharacters characters = emitter<StringEmitter>().loadCharacters(context, match.string);
    m_loadedSubjects.append({ context.block(), match.string.value(), characters });
    return characters;
}

template<typename T> T ResolvedCallEmitter::matchByFirstCharacter(const ValueContext& value, const RegExpMatch& match, T miss, NOESCAPE const Invocable<T()> auto& call)
{
    if (!m_generator.usesLoopForms(value.block()) || value->regExpFlags().containsAny(flagsOfMatchesUsingObject))
        return call();
    StringEmitter& strings = emitter<StringEmitter>();
    const RegExpSummary& summary = build().regExpSummaries().summaryOf(value->pattern(), toYarrFlags(value->regExpFlags()), strings.textOf(value->pattern()));
    if (!summary.firstCharacters)
        return call();
    const ValueID asked = definitionOf(value->stringOperand());
    if (!m_stringsAskedFirstCharacter.contains(asked)) {
        if (summary.firstCharacters->count() > maxFirstCharacters)
            return call();
        m_stringsAskedFirstCharacter.append(asked);
    } else if (summary.firstCharacters->count() == 256)
        return call();
    Vector<RangeOfFirstCharacters, maxRangesOfFirstCharacters + 1> ranges = rangesOfFirstCharacters(*summary.firstCharacters, true);
    const bool rangesMatch = ranges.size() <= maxRangesOfFirstCharacters;
    if (!rangesMatch)
        ranges = rangesOfFirstCharacters(*summary.firstCharacters, false);
    if (ranges.size() > maxRangesOfFirstCharacters)
        return call();

    const CodeGenerator::CarriedSubstring* carried = carriedSubjectOf(match);
    std::optional<OwnedCharacters> loaded = carried ? loadCarriedSubstring(value, *carried) : findLoadedSubject(value, match.string);
    LBasicBlock matches = newBlock();
    LBasicBlock misses = newBlock();
    Join<T> join(m_out);
    ScopeFork fork = m_out.forkScope();
    CharacterView view = loaded ? strings.viewOfCharacters(*loaded) : strings.viewWithoutResolving(value, match.string, unlikelyEdge(matches));
    if (!summary.firstCharactersAreLatin1)
        strings.continueIf(view.is8Bit, unlikelyEdge(matches));
    strings.continueIf(isNotZero(view.length), unlikelyEdge(misses));
    CharacterView narrowView { view.characters, view.length, boolean(true), view.owner };
    Word32 character = strings.codeUnitAt(narrowView, word32(0));
    ensureStillAliveHere(view.owner);
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
    join.add(call());
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

void ResolvedCallEmitter::storeBounds(RegExpMatchEmitter& matcher, const RegExpMatchEmitter::Bounds& bounds, LValue output, ptrdiff_t offset)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    for (size_t index = 0; index < bounds.size(); ++index) {
        Word32 bound = index < 2 ? matcher.indexOf(bounds[index]) : matcher.indexOfGroupBound(bounds[index]);
        m_out.store32(bound.value(), m_out.address(state, output, offset + index * sizeof(int)));
    }
}

RegExpMatchEmitter::Begin ResolvedCallEmitter::beginOfMatch(const RegExpMatch& match, RegExpMatchEmitter& matcher, const CharacterView& view, LBasicBlock calls, LValue& first)
{
    StringEmitter& strings = emitter<StringEmitter>();
    const bool usesLastIndex = match.usesLastIndex();
    RegExpMatchEmitter::Begin begin = !usesLastIndex ? RegExpMatchEmitter::Begin::AtStart : match.flags.contains(RegExpFlags::Sticky) ? RegExpMatchEmitter::Begin::AtPlace : RegExpMatchEmitter::Begin::AtOrAfterPosition;
    Word32 length = view.length;
    if (usesLastIndex) {
        TypedPointer lastIndex = m_out.address(m_heaps.entire(AbstractHeap::RegExpState), match.regExpObject.value(), RegExpObject::offsetOfLastIndex());
        Integer firstIndex = match.lastIndexStored ? *match.lastIndexStored : zeroExtended(low32(Integer(m_out.load64(lastIndex))));
        strings.continueIf(belowOrEqual(firstIndex, zeroExtended(view.length)), unlikelyEdge(calls));
        first = matcher.addressAt(low32(firstIndex));
        length = sub(view.length, low32(firstIndex));
    }
    if (matcher.isSearched(begin))
        strings.continueIf(belowOrEqual(length, maxLengthSearchedInline), normalEdge(calls));
    return begin;
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

CharacterView ResolvedCallEmitter::viewOfSubject(const OperationContext& context, const RegExpMatch& match, const std::optional<OwnedCharacters>& loaded, LBasicBlock calls)
{
    StringEmitter& strings = emitter<StringEmitter>();
    if (match.isHeld) {
        const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
        if (!match.isPrivate && m_facts.mayRecompileRegExps())
            strings.continueIf(isZero(Word32(m_out.load8ZeroExt32(mutableInstanceField(AOT_INSTANCE_OFFSET(regExpsAreRecompiled))))), rareEdge(calls));
        if (match.isArgument)
            strings.continueIf(equal(loadStructureID(match.regExpObject), loadFilledOnce32(Instance::offsetOfReceiverStructureID(Receiver::RegExp))), rareEdge(calls));
        Boxed lastIndex(m_out.load64(m_out.address(state, match.regExpObject.value(), RegExpObject::offsetOfLastIndex())));
        if (match.resetsLastIndex)
            strings.continueIf(equal(lastIndex, boxedConstant(jsNumber(0))), unlikelyEdge(calls));
        else if (!match.lastIndexStored)
            strings.continueIf(match.usesLastIndex() ? isInt32(lastIndex) : isNumber(lastIndex), unlikelyEdge(calls));
        if (!match.isPrivate && (match.usesLastIndex() || match.flags.contains(RegExpFlags::Global))) {
            LValue regExpAndFlags = m_out.load64(m_out.address(state, match.regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags()));
            strings.continueIf(Condition(m_out.testIsZero64(regExpAndFlags, RegExpObject::lastIndexIsNotWritableFlag)), rareEdge(calls));
        }
    }
    CharacterView view = loaded ? strings.viewOfCharacters(*loaded) : strings.viewWithoutResolving(context, match.string, unlikelyEdge(calls));
    strings.continueIf(view.is8Bit, unlikelyEdge(calls));
    return view;
}

bool ResolvedCallEmitter::canTestByMatcher(const RegExpMatchPlan& plan, const RegExpMatch& match, RegExpMatchAnswer answer)
{
    if (answer != RegExpMatchAnswer::Boolean || plan.isPrefix || match.isArgument || match.usesLastIndex() || carriedSubjectOf(match))
        return false;
    return !!build().regExpSummaries().matcherOf(match.pattern, toYarrFlags(match.flags), emitter<StringEmitter>().textOf(match.pattern), build());
}

Boxed ResolvedCallEmitter::testByMatcher(const OperationContext& context, const RegExpMatch& match)
{
    LValue matcher = m_out.relocatable(SymbolSpace::RegExp, m_mentions.regExpMatcher(match.pattern, toYarrFlags(match.flags)), entryOfMatcher);
    const StubKey stub = regExpTest(match.isHeld, m_facts.mayReadLegacyRegExpStatics());
    if (testTakesRegExp(stub))
        return Boxed(callStub(context, stub, { match.regExpObject.value(), match.string.value(), matcher }, { .calledBuiltin = match.calledBuiltin }).first);
    return Boxed(callStub(context, stub, { match.string.value(), matcher }).first);
}

template<typename T> T ResolvedCallEmitter::matchInPlace(const OperationContext& context, const RegExpMatchPlan& plan, const RegExpMatch& matchOfSubject, RegExpMatchAnswer answer, T miss, const ScopedLambda<T()>& call)
{
    RegExpMatch match = matchOfSubject;
    if (canTestByMatcher(plan, match, answer)) {
        Boxed isMatch = testByMatcher(context, match);
        if constexpr (std::is_same_v<T, Condition>)
            return unboxBoolean(isMatch);
        else
            return isMatch;
    }
    const bool recordsMatch = m_facts.mayReadLegacyRegExpStatics();
    const bool keepsGroups = answer == RegExpMatchAnswer::Array || answer == RegExpMatchAnswer::Parts;
    std::optional<OwnedCharacters> loaded = loadSubject(context, match);
    LBasicBlock calls = newBlock();
    LBasicBlock misses = newBlock();
    Join<T> join(m_out);
    ScopeFork fork = m_out.forkScope();

    const bool usesLastIndex = match.usesLastIndex();
    const TypedPointer lastIndexOfObject = match.regExpObject ? m_out.address(m_heaps.entire(AbstractHeap::RegExpState), match.regExpObject.value(), RegExpObject::offsetOfLastIndex()) : TypedPointer();
    CharacterView view = viewOfSubject(context, match, loaded, calls);
    RegExpMatchEmitter matcher(m_generator, plan, view.characters, view.length, keepsGroups ? RegExpMatchEmitter::Groups::Kept : RegExpMatchEmitter::Groups::Ignored);
    LValue first { nullptr };
    RegExpMatchEmitter::Begin begin = beginOfMatch(match, matcher, view, calls, first);
    RegExpMatchEmitter::Bounds bounds = matcher.match(misses, begin, first);

    if (plan.isPrefix) {
        ensureStillAliveHere(view.owner);
        m_out.jump(calls);
    } else {
        if (keepsGroups)
            storeBounds(matcher, bounds, matchOutput().value());
        ensureStillAliveHere(view.owner);
        const bool isCarried = carriedSubjectOf(match);
        if (isCarried && answer == RegExpMatchAnswer::Array)
            match.string = subjectOf(context, match);
        if (usesLastIndex && answer != RegExpMatchAnswer::Array)
            m_out.store64(boxInt32(matcher.indexOf(bounds[1])).value(), lastIndexOfObject);
        if (recordsMatch && (usesLastIndex || isCarried) && answer != RegExpMatchAnswer::Array)
            recordMatch(context, match, isCarried ? loaded : std::nullopt, matcher.indexOf(bounds[0]), matcher.indexOf(bounds[1]));
        else if (recordsMatch && answer != RegExpMatchAnswer::Array)
            callStub(context, regExpMatch(RegExpMatchResult::Record), { match.regExpObject.value(), match.string.value(), bounds[0], bounds[1], view.characters.value() });
        if constexpr (std::is_same_v<T, Condition>)
            join.add(boolean(true));
        else
            join.add(answer == RegExpMatchAnswer::Array ? createMatchArray(context, match) : answer == RegExpMatchAnswer::Index ? boxInt32(matcher.indexOf(bounds[0])) : boxedConstant(jsBoolean(true)));
    }
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

    NumberedImmediate record { m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())) };
    Boxed miss(m_out.constInt64(JSValue::encode(missOfMatchForm(value->matchForm()))));
    RegExpMatch match { value->pattern(), value->regExpFlags(), in<CellPointer>(value->stringOperand()), regExp ? in<CellPointer>(regExp) : CellPointer() };
    auto call = [&] {
        CellPointer string = subjectOf(value, match);
        if (!regExp)
            return Boxed(callStub(value, regExpMatchLiteralWithoutObject(value->matchForm()), { string.value() }, { .numberedImmediate = record }).first);
        StubKey stub = regExpMatchLiteral(value->matchForm(), value->regExpFlags().contains(RegExpFlags::Global), recordsMatch);
        return Boxed(callStub(value, stub, { in<CellPointer>(regExp).value(), string.value() }, { .numberedImmediate = record }).first);
    };
    match.resetsLastIndex = value->matchForm() == RegExpMatchForm::Search;
    match.subject = value->stringOperand();
    const bool listsMatches = value->matchForm() == RegExpMatchForm::Match && value->regExpFlags().contains(RegExpFlags::Global);
    if (const RegExpMatchPlan* plan = listsMatches ? nullptr : matchPlanOf(value.block(), match)) {
        define(value, matchInPlace<Boxed>(value, *plan, match, answerOfMatchForm(value->matchForm()), miss, call));
        return;
    }
    define(value, matchByFirstCharacter<Boxed>(value, match, miss, call));
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

    NumberedImmediate record { m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())) };
    RegExpMatch match { value->pattern(), value->regExpFlags(), in<CellPointer>(value->stringOperand()), regExp ? in<CellPointer>(regExp) : CellPointer() };
    auto call = [&] {
        CellPointer string = subjectOf(value, match);
        if (!regExp)
            return unboxBoolean(Boxed(callStub(value, regExpMatchLiteralWithoutObject(RegExpMatchForm::Parts), { string.value() }, { .numberedImmediate = record }).first));
        StubKey stub = regExpMatchLiteral(RegExpMatchForm::Parts, value->regExpFlags().contains(RegExpFlags::Global), recordsMatch);
        return unboxBoolean(Boxed(callStub(value, stub, { in<CellPointer>(regExp).value(), string.value() }, { .numberedImmediate = record }).first));
    };
    match.subject = value->stringOperand();
    if (const RegExpMatchPlan* plan = matchPlanOf(value.block(), match)) {
        define(value, matchInPlace<Condition>(value, *plan, match, RegExpMatchAnswer::Parts, boolean(false), call));
        return;
    }
    define(value, matchByFirstCharacter<Condition>(value, match, boolean(false), call));
}

void ResolvedCallEmitter::recordMatch(const OperationContext& context, const RegExpMatch& match, const std::optional<OwnedCharacters>& part, Word32 start, Word32 end)
{
    constexpr ptrdiff_t cachedResult = JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult();
    constexpr ptrdiff_t result = cachedResult + RegExpCachedResult::offsetOfResult();
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    CellPointer globalObject(loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)).value());
    auto field = [&](ptrdiff_t offset) { return m_out.address(state, globalObject.value(), offset); };
    LValue regExp = m_out.bitAnd(m_out.load64(m_out.address(state, match.regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags())), static_cast<int64_t>(RegExpObject::regExpMask));
    m_out.store64(regExp, field(cachedResult + RegExpCachedResult::offsetOfLastRegExp()));
    m_out.store64(part ? part->owner.value() : match.string.value(), field(cachedResult + RegExpCachedResult::offsetOfLastInput()));
    m_out.store64(zeroExtended(part ? add(part->start, start) : start).value(), field(result + OBJECT_OFFSETOF(MatchResult, start)));
    m_out.store64(zeroExtended(part ? add(part->start, end) : end).value(), field(result + OBJECT_OFFSETOF(MatchResult, end)));
    if (part) {
        m_out.store32(part->start.value(), field(cachedResult + RegExpCachedResult::offsetOfInputStart()));
        m_out.store32(part->source.length.value(), field(cachedResult + RegExpCachedResult::offsetOfInputLength()));
    }
    const RegExpCachedResult::State recordedState = part ? RegExpCachedResult::State::LazyWithSubstringInput : RegExpCachedResult::State::Lazy;
    m_out.store32As8(m_out.constInt32(static_cast<int32_t>(recordedState)), field(cachedResult + RegExpCachedResult::offsetOfReified()));
    m_out.store32As8(m_out.constInt32(0), field(cachedResult + RegExpCachedResult::offsetOfOneCharacterMatch()));
    emitter<BarrierEmitter>().barrier(context, globalObject);
}

void ResolvedCallEmitter::clearAbsentGroupBounds(CellPointer regExpObject, unsigned count)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    LBasicBlock isRecompiled = newBlock();
    LBasicBlock done = newBlock();
    branch(isZero(Word32(m_out.load8ZeroExt32(mutableInstanceField(AOT_INSTANCE_OFFSET(regExpsAreRecompiled))))), { normalEdge(done), rareEdge(isRecompiled) });
    m_out.appendTo(isRecompiled);
    LValue regExp = m_out.bitAnd(m_out.load64(m_out.address(state, regExpObject.value(), RegExpObject::offsetOfRegExpAndFlags())), static_cast<int64_t>(RegExpObject::regExpMask));
    Word32 numberOfGroups(m_out.load32(m_out.address(state, regExp, RegExp::offsetOfNumSubpatterns())));
    Pointer output = matchOutput();
    for (unsigned group = 1; group < count; ++group) {
        for (unsigned bound = 2 * group; bound < 2 * (group + 1); ++bound) {
            TypedPointer address = m_out.address(state, output.value(), bound * sizeof(int));
            m_out.store32(m_out.select(m_out.aboveOrEqual(numberOfGroups.value(), m_out.constInt32(group)), m_out.load32(address), m_out.constInt32(-1)), address);
        }
    }
    m_out.jump(done);
    m_out.appendTo(done);
}

ValueID ResolvedCallEmitter::findDeferredLastIndexStore(ValueID match)
{
    const Operation& operation = operationAt(match);
    if (operation.opcode != Opcode::MatchHeldRegExpParts || !operation.isPrivate() || !operation.lastIndexOperand() || !operation.regExpFlags().containsAny({ RegExpFlags::Global, RegExpFlags::Sticky }))
        return { };
    auto root = [&](ValueID id) { return m_code.valueBehindNames<cellPreservingOpcodes>(id); };
    for (ValueID id = operation.previous; id; id = operationAt(id).previous) {
        const Operation& previous = operationAt(id);
        if (previous.traits().isUnobservable())
            continue;
        if (previous.opcode != Opcode::PropSet || previous.isFreeName() || root(previous.objectOperand()) != root(operation.regExpOperand()) || root(previous.valueOperand()) != root(operation.lastIndexOperand()))
            return { };
        const PropertyKey& name = types().at(previous.key());
        return name.kind == PropertyKeyKind::Text && m_program.nameClass(name.text) == NameClass::LastIndex ? id : ValueID();
    }
    return { };
}

bool ResolvedCallEmitter::isDeferredLastIndexStore(const ValueContext& store)
{
    if (m_blockOfDeferredStores != store.block()) {
        m_blockOfDeferredStores = store.block();
        m_deferredLastIndexStores.shrink(0);
        for (ValueID id = m_code.lastOperationOf(store.block()); id; id = operationAt(id).previous) {
            if (ValueID deferred = findDeferredLastIndexStore(id))
                m_deferredLastIndexStores.append(deferred);
        }
    }
    if (!m_deferredLastIndexStores.contains(store.id()))
        return false;
    build().statistics().count(StatisticID::LastIndexStoresDeferred);
    return true;
}

void ResolvedCallEmitter::generateMatchHeldRegExpParts(const ValueContext& value)
{
    RegExpMatch match { value->pattern(), value->regExpFlags(), in<CellPointer>(value->stringOperand()), in<CellPointer>(value->regExpOperand()) };
    match.isHeld = true;
    match.isPrivate = value->isPrivate();
    if (ValueID lastIndex = value->lastIndexOperand()) {
        Operand operand = get(lastIndex);
        match.lastIndexStored = operand.is<Word32>() ? signExtended(operand.as<Word32>()) : operand.as<Integer>();
    }
    const ValueID deferredStore = findDeferredLastIndexStore(value.id());
    auto call = [&] {
        if (deferredStore)
            m_out.store64(in<Boxed>(operationAt(deferredStore).valueOperand()).value(), m_out.address(m_heaps.entire(AbstractHeap::RegExpState), match.regExpObject.value(), RegExpObject::offsetOfLastIndex()));
        Condition isFound = unboxBoolean(Boxed(callStub(value, regExpMatch(RegExpMatchResult::Parts), { match.regExpObject.value(), match.string.value() }, { .calledBuiltin = functionOf(value->overload()) }).first));
        if (!match.isPrivate && m_facts.mayRecompileRegExps())
            clearAbsentGroupBounds(match.regExpObject, value->count());
        return isFound;
    };
    const RegExpMatchPlan* plan = matchPlanOf(value.block(), match);
    define(value, plan ? matchInPlace<Condition>(value, *plan, match, RegExpMatchAnswer::Parts, boolean(false), call) : call());
}

void ResolvedCallEmitter::generateGetRegExpLastIndex(const ValueContext& value)
{
    CellPointer regExpObject = in<CellPointer>(value->regExpOperand());
    define(value, low32(Integer(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::RegExpState), regExpObject.value(), RegExpObject::offsetOfLastIndex())))));
}

void ResolvedCallEmitter::generateStringLengthWithoutMatches(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    CellPointer regExpObject = in<CellPointer>(value->regExpOperand());
    LValue length = callStub(value, stringReplaceRegExpMayNotMatch(ReplacementOfMatches::NothingForLength), { string.value(), regExpObject.value() }, { .calledBuiltin = functionOf(value->overload()) }).first;
    define(value, low32(Integer(length)));
}

Pointer ResolvedCallEmitter::matchIntoFrameByCall(const ValueContext& value, LValue slot)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    CellPointer regExpObject = in<CellPointer>(value->regExpOperand());
    CellPointer string = in<CellPointer>(value->stringOperand());
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
    m_out.branch(m_out.belowOrEqual(count, maxFrameMatchParts), normalEdge(fits), rareEdge(isLarge));

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
    return join.finish();
}

void ResolvedCallEmitter::generateMatchRegExpIntoFrame(const ValueContext& value)
{
    LValue slot = m_out.lockedStackSlot(sizeof(uint64_t) * (1 + maxFrameMatchParts));
    slot->slot()->setIsDeadAtSuspensions(true);
    RegExpMatch match { value->pattern(), value->regExpFlags(), in<CellPointer>(value->stringOperand()), in<CellPointer>(value->regExpOperand()) };
    match.isHeld = true;
    match.isPrivate = value->isPrivate();
    const RegExpMatchPlan* plan = match.pattern ? matchPlanOf(value.block(), match) : nullptr;
    if (!plan || plan->isPrefix) {
        define(value, matchIntoFrameByCall(value, slot));
        return;
    }
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    const TypedPointer lastIndex = m_out.address(state, match.regExpObject.value(), RegExpObject::offsetOfLastIndex());
    LBasicBlock calls = newBlock();
    LBasicBlock misses = newBlock();
    Join<Pointer> join(m_out);
    ScopeFork fork = m_out.forkScope();
    CharacterView view = viewOfSubject(value, match, std::nullopt, calls);
    RegExpMatchEmitter matcher(m_generator, *plan, view.characters, view.length, RegExpMatchEmitter::Groups::Kept);
    LValue first { nullptr };
    RegExpMatchEmitter::Begin begin = beginOfMatch(match, matcher, view, calls, first);
    RegExpMatchEmitter::Bounds bounds = matcher.match(misses, begin, first);
    if (bounds.size() <= 2 * maxFrameMatchParts) {
        m_out.store64(m_out.constInt64(bounds.size() / 2), m_out.address(state, slot));
        storeBounds(matcher, bounds, slot, sizeof(uint64_t));
        ensureStillAliveHere(view.owner);
        if (match.usesLastIndex())
            m_out.store64(boxInt32(matcher.indexOf(bounds[1])).value(), lastIndex);
        if (m_facts.mayReadLegacyRegExpStatics())
            callStub(value, regExpMatch(RegExpMatchResult::Record), { match.regExpObject.value(), match.string.value(), bounds[0], bounds[1], view.characters.value() });
        join.add(Pointer(slot));
    } else {
        ensureStillAliveHere(view.owner);
        m_out.jump(calls);
    }
    if (misses->numberOfIncomingEdges()) {
        m_out.appendTo(misses);
        ensureStillAliveHere(view.owner);
        if (match.usesLastIndex())
            m_out.store64(m_out.constInt64(JSValue::encode(jsNumber(0))), lastIndex);
        join.add(pointerConstant(0));
    }
    m_out.appendTo(calls);
    join.add(matchIntoFrameByCall(value, slot));
    define(value, join.finishBehind(fork));
}

void ResolvedCallEmitter::generateIsMatchInFrame(const ValueContext& value)
{
    define(value, Condition(m_out.notZero64(in<Pointer>(value->matchOperand()).value())));
}

void ResolvedCallEmitter::generateFrameMatchLength(const ValueContext& value)
{
    Pointer record = in<Pointer>(value->matchOperand());
    define(value, Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::RegExpState), record.value()))));
}

void ResolvedCallEmitter::generateFrameMatchBounds(const ValueContext& value)
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
    define(value, unboxCell(callOperation<Boxed>(value, RuntimeOperation::RegExpMatchArray, { regExpObject, string, in<Pointer>(value->matchOperand()) })));
}

void ResolvedCallEmitter::generateGetMatchArrayGroups(const ValueContext& value)
{
    Machine::DeclaredAccessesSuspension suspension(m_out);
    Pointer butterfly = loadField<Pointer>(in<CellPointer>(value->matchArrayOperand()), m_heaps.JSObject_butterfly);
    define(value, unboxCell(Boxed(m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), butterfly.value(), offsetInButterfly(RegExpMatchesArrayGroupsPropertyOffset) * sizeof(EncodedJSValue))))));
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

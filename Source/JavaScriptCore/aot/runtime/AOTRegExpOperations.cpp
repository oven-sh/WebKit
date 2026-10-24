/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTStubsRegExp.h"
#include "AOTThreshold.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "InterpreterInlines.h"
#include "JSCInlines.h"
#include "RegExp.h"
#include "RegExpConstructor.h"
#include "RegExpMatchesArray.h"
#include "RegExpObjectInlines.h"
#include "RegExpPrototype.h"
#include "SlotVisitorInlines.h"
#include "StringPrototype.h"
#include "StringPrototypeInlines.h"
#include "VM.h"
#include "YarrFlags.h"
#include "YarrFlatProgram.h"
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <wtf/HashFunctions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpExec, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpExec);
    auto* object = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpExec);
    OPERATION_RETURN(scope, JSValue::encode(object->exec(globalObject, cellArgument<JSString>(string, Entry::operationAOTRegExpExec))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpTest, size_t, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpTest);
    auto* object = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpTest);
    OPERATION_RETURN(scope, object->testInline(globalObject, cellArgument<JSString>(string, Entry::operationAOTRegExpTest)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpTestBoxed, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpTestBoxed);
    auto* object = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpTestBoxed);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(object->testInline(globalObject, cellArgument<JSString>(string, Entry::operationAOTRegExpTestBoxed)))));
}

// What test answers for a RegExp without the flags g and y. RegExp.$1 and its like stay what the last match of the program made them.
JSC_DEFINE_JIT_OPERATION(operationAOTMatchesUnrecorded, size_t, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMatchesUnrecorded);
    auto input = cellArgument<JSString>(string, Entry::operationAOTMatchesUnrecorded)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, !!cellArgument<RegExpObject>(regExp, Entry::operationAOTMatchesUnrecorded)->regExp()->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0));
}

static bool findMatch(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto input = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    unsigned lastIndex = getRegExpObjectLastIndexAsUnsigned(globalObject, object, input);
    RETURN_IF_EXCEPTION(scope, false);
    RegExp* regExp = object->regExp();
    bool globalOrSticky = regExp->globalOrSticky();
    if (lastIndex == UINT_MAX && globalOrSticky) {
        scope.release();
        object->setLastIndex(globalObject, 0);
        return false;
    }
    if (!globalOrSticky)
        lastIndex = 0;
    auto offsets = regExp->ovectorSpan();
    int position = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, lastIndex, offsets);
    RETURN_IF_EXCEPTION(scope, false);
    if (position == -1) {
        scope.release();
        if (globalOrSticky)
            object->setLastIndex(globalObject, 0);
        return false;
    }
    MatchResult result(position, offsets[1]);
    if (globalOrSticky)
        object->setLastIndex(globalObject, result.end);
    RETURN_IF_EXCEPTION(scope, false);
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, result, false);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpFind, size_t, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpFind);
    OPERATION_RETURN(scope, findMatch(globalObject, vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpFind), cellArgument<JSString>(string, Entry::operationAOTRegExpFind)));
}

static RegExp* regExpOfRecord(VM& vm, uint32_t record)
{
    RegExpRecordID id(record);
    RELEASE_ASSERT(id);
    return vm.aotState().regExpLiterals->regExpFor(vm, id, RegExpRecords(*vm.aotState().image).at(id).flags());
}

static JSValue createMatchValue(Instance* instance, RegExpMatchForm form, RegExp* regExp, JSString* subject, MatchResult result, std::span<const int> offsets)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    switch (form) {
    case RegExpMatchForm::Test:
        return jsBoolean(true);
    case RegExpMatchForm::Search:
        return jsNumber(result.start);
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        break;
    }
    unsigned numSubpatterns = regExp->numSubpatterns();
    bool hasNamedCaptures = regExp->hasNamedCaptures();
    bool createIndices = regExp->hasIndices();
    if (hasNamedCaptures || createIndices) [[unlikely]]
        return createRegExpMatchesArrayWithGroupsOrIndices(vm, globalObject, subject, regExp, result, offsets, numSubpatterns, hasNamedCaptures, createIndices);
    if (globalObject->isHavingABadTime()) [[unlikely]]
        return createRegExpMatchesArrayForPlainRegExpHavingABadTime(vm, globalObject, subject, result, offsets, numSubpatterns);
    prepareInlineAllocation<JSArray>(instance, InlineAllocation::RegExpMatchesArray, globalObject->regExpMatchesArrayStructure());
    return createRegExpMatchesArrayForPlainRegExp(vm, globalObject, subject, result, offsets, numSubpatterns);
}

// Leaves the next match in match. Without one match is false.
static ALWAYS_INLINE void findMatchBehind(JSGlobalObject* globalObject, RegExp* regExp, JSString* string, StringView source, MatchResult& match)
{
    uint64_t start = match.empty() ? advanceStringIndex(source, source.length(), match.end, regExp->eitherUnicode()) : match.end;
    if (start > source.length()) {
        match = MatchResult::failed();
        return;
    }
    int* offsets;
    match = globalObject->regExpGlobalData().performMatch(globalObject, regExp, string, source, static_cast<int>(start), &offsets);
}

// What String.prototype.match answers for a global RegExp whose first match in the subject is known.
static JSValue collectMatchesFrom(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject, MatchResult match)
{
    // Beyond this RegExpObject::matchGlobal() counts the matches before it creates them.
    AOT_THRESHOLD(unsigned, maxMatchesCreatedUncounted, 100000);
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, { });
    auto source = subject->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* array = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, subject, match, false);
    unsigned count = 0;
    do {
        if (count == maxMatchesCreatedUncounted) [[unlikely]]
            RELEASE_AND_RETURN(scope, regExpMatchFast(globalObject, object, subject));
        array->putDirectIndex(globalObject, count++, jsSubstringOfResolved(vm, subject, match.start, match.end - match.start));
        RETURN_IF_EXCEPTION(scope, { });
        findMatchBehind(globalObject, regExp, subject, source, match);
        RETURN_IF_EXCEPTION(scope, { });
    } while (match);
    return array;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpMatchLiteral, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string, uint32_t regExpRecord, uint32_t form))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpMatchLiteral);
    RegExpObject* object = regExp ? cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpMatchLiteral) : nullptr;
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTRegExpMatchLiteral);
    RegExp* cell = object ? object->regExp() : regExpOfRecord(vm, regExpRecord);
    ASSERT(!RegExpRecordID(regExpRecord) || cell->imageRecord() == &RegExpRecords(*vm.aotState().image).at(RegExpRecordID(regExpRecord)));

    ASSERT(static_cast<size_t>(cell->offsetVectorSize()) <= std::max<size_t>(vm.aotState().image->header().counts.maxRegExpOutputSize, 2 * (VMState::maxGroupsInRegExpMatchOutput + 1)));
    auto offsets = unsafeMakeSpan(vm.aotState().regExpMatchOutput.get(), static_cast<size_t>(cell->offsetVectorSize()));
    MatchResult result(offsets[0], offsets[1]);
    RELEASE_ASSERT(result.end >= result.start);

    auto matchForm = static_cast<RegExpMatchForm>(form);
    if (matchForm == RegExpMatchForm::Match && cell->global())
        OPERATION_RETURN(scope, JSValue::encode(collectMatchesFrom(globalObject, vm, object, subject, result)));

    JSValue value = createMatchValue(instance, matchForm, cell, subject, result, offsets);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!object)
        OPERATION_RETURN(scope, JSValue::encode(value));

    if (matchForm != RegExpMatchForm::Search && cell->globalOrSticky()) {
        object->setLastIndex(globalObject, result.end);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, cell, subject, result, false);
    OPERATION_RETURN(scope, JSValue::encode(value));
}

// Answers as the code of a pattern in the image does: where the match starts, -1 where there is none, less where the match has to be given to somebody else.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRegExpMatchFlatProgram8, int64_t, (void* program, const Latin1Character* characters, uint32_t length, uint32_t* output))
{
    switch (Yarr::matchFlatProgram(*static_cast<const Yarr::FlatProgram*>(program), std::span { characters, length }, 0, output)) {
    case Yarr::JSRegExpResult::Match:
        return output[0];
    case Yarr::JSRegExpResult::NoMatch:
        return -1;
    default:
        return static_cast<int64_t>(Yarr::JSRegExpResult::JITCodeFailure);
    }
}

static JSValue valueOfFailedMatch(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
        return jsBoolean(false);
    case RegExpMatchForm::Exec:
    case RegExpMatchForm::Match:
        return jsNull();
    case RegExpMatchForm::Search:
        return jsNumber(-1);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpMatchLiteralWithoutObject, EncodedJSValue, (Instance* instance, JSCell* string, uint32_t regExpRecord, uint32_t form))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpMatchLiteralWithoutObject);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTRegExpMatchLiteralWithoutObject);
    RegExp* regExp = regExpOfRecord(vm, regExpRecord);
    RELEASE_ASSERT(!regExp->globalOrSticky());

    auto input = subject->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto offsets = regExp->ovectorSpan();
    int position = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0, offsets);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (position == -1)
        OPERATION_RETURN(scope, JSValue::encode(valueOfFailedMatch(static_cast<RegExpMatchForm>(form))));

    MatchResult result(position, offsets[1]);
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, subject, result, false);
    OPERATION_RETURN(scope, JSValue::encode(createMatchValue(instance, static_cast<RegExpMatchForm>(form), regExp, subject, result, offsets)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpExecPrefix, size_t, (Instance* instance, JSCell* regExp, JSCell* string, uint32_t count, uint32_t* bounds))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpExecPrefix);
    std::fill_n(bounds, 2 * static_cast<size_t>(count), static_cast<uint32_t>(absentBound));
    auto* object = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpExecPrefix);
    bool isFound = findMatch(globalObject, vm, object, cellArgument<JSString>(string, Entry::operationAOTRegExpExecPrefix));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!isFound)
        OPERATION_RETURN(scope, static_cast<size_t>(noMatchPrefixLength));

    static_assert(absentBound == -1, "A group that took no part in the match has the offsets -1.");
    size_t length = static_cast<size_t>(object->regExp()->numSubpatterns()) + 1;
    std::ranges::copy(object->regExp()->ovectorSpan().first(2 * std::min<size_t>(count, length)), bounds);
    OPERATION_RETURN(scope, length);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringMatchPrefix, size_t, (Instance* instance, JSCell* string, JSCell* regExp, uint32_t count, EncodedJSValue* elements))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringMatchPrefix);
    std::fill_n(elements, count, JSValue::encode(jsUndefined()));
    JSValue result = regExpMatchFast(globalObject, cellArgument<RegExpObject>(regExp, Entry::operationAOTStringMatchPrefix), cellArgument<JSString>(string, Entry::operationAOTStringMatchPrefix));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (result.isNull())
        OPERATION_RETURN(scope, static_cast<size_t>(noMatchPrefixLength));
    auto* array = uncheckedDowncast<JSArray>(result);
    unsigned length = array->length();
    for (unsigned i = 0; i < std::min(count, length); ++i) {
        elements[i] = JSValue::encode(array->getIndex(globalObject, i));
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    }
    OPERATION_RETURN(scope, static_cast<size_t>(length));
}

static void setSharedLiteral(VM& vm, Instance* instance, uint32_t staticsOffset, RegExpObject* object)
{
    vm.mutatorFence();
    instance->state->statics->setCell(staticsOffset, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExpLiteral, JSCell*, (Instance* instance, uint32_t regExpRecord, uint32_t flags, uint32_t staticsOffset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRegExpLiteral);
    RegExp* regExp = vm.aotState().regExpLiterals->regExpFor(vm, RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags));
    if (!regExp->isValid()) [[unlikely]] {
        throwException(globalObject, scope, regExp->errorToThrow(globalObject));
        OPERATION_RETURN(scope, nullptr);
    }
    if (staticsOffset != noSharedRegExpOffset && !instance->state->statics->at<JSCell*>(staticsOffset))
        setSharedLiteral(vm, instance, staticsOffset, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
    prepareInlineAllocation<RegExpObject>(instance, InlineAllocation::RegExpObject, globalObject->regExpStructure());
    OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSharedRegExpLiteral, JSCell*, (Instance* instance, uint32_t staticsOffset, uint32_t regExpRecord, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSharedRegExpLiteral);
    DeferTerminationForAWhile deferTermination(vm);
    DeferGCForAWhile deferGC(vm);
    RegExp* regExp = vm.aotState().regExpLiterals->regExpFor(vm, RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags));
    ASSERT(regExp->isValid());
    RegExpObject* object = RegExpObject::create(vm, globalObject->regExpStructure(), regExp);
    setSharedLiteral(vm, instance, staticsOffset, object);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExp, JSCell*, (Instance* instance, JSCell* patternCell, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRegExp);
    auto pattern = cellArgument<JSString>(patternCell, Entry::operationAOTNewRegExp)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, regExpCreate(globalObject, JSValue(), pattern, OptionSet<Yarr::Flags>::fromRaw(flags)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpFlags, JSCell*, (Instance* instance, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpFlags);
    OPERATION_RETURN(scope, jsString(vm, String { Yarr::flagsString(cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpFlags)->regExp()->flags()).span() }));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpSource, JSCell*, (Instance* instance, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpSource);
    OPERATION_RETURN(scope, jsString(vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpSource)->regExp()->escapedPattern()));
}

static JSString* replaceMatchesGeneric(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSValue replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = regExpReplaceGeneric(globalObject, regExpObject, string, replacement);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return asString(result);
}

static JSString* replaceMatches(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSValue replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[likely]]
        RELEASE_AND_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, string, regExpObject, replacement));
    RELEASE_AND_RETURN(scope, replaceMatchesGeneric(globalObject, vm, string, regExpObject, replacement));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, EncodedJSValue replacement))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceRegExp);
    OPERATION_RETURN(scope, replaceMatches(globalObject, vm, subject, regExpObject, JSValue::decode(replacement)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceAllRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, EncodedJSValue replacement))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceAllRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceAllRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceAllRegExp);
    if (!regExpObject->regExp()->global()) [[unlikely]] {
        throwException(globalObject, scope, createReplaceAllNonGlobalRegExpError(globalObject));
        OPERATION_RETURN(scope, nullptr);
    }
    OPERATION_RETURN(scope, replaceMatches(globalObject, vm, subject, regExpObject, JSValue::decode(replacement)));
}

static JSString* replaceOneMatch(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, const String& replacement, MatchResult match, const int* offsets)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (replacement.isEmpty()) {
        if (!match.start)
            return jsSubstringOfResolved(vm, string, match.end, source.length() - match.end);
        if (match.end == source.length())
            return jsSubstringOfResolved(vm, string, 0, match.start);
    }
    StringBuilder substituted(OverflowPolicy::RecordOverflow);
    StringView inserted = replacement;
    if (size_t dollar = replacement.find('$'); dollar != notFound) {
        substituteBackreferencesSlow(substituted, replacement, source, offsets, regExp, dollar);
        if (substituted.hasOverflowed()) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
        inserted = substituted;
    }
    auto result = tryMakeString(source.left(match.start), inserted, source.substring(match.end));
    if (!result) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    RELEASE_AND_RETURN(scope, jsString(vm, WTF::move(result)));
}

static JSString* replaceEveryMatchWithPlainText(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, JSString* replacement, const String& replacementText, MatchResult match)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool removes = replacementText.isEmpty();
    Vector<Range<int32_t>, 64> kept;
    size_t endOfLastMatch = 0;
    int matches = 0;
    do {
        if (!removes || endOfLastMatch < match.start) {
            if (!kept.tryConstructAndAppend(endOfLastMatch, match.start)) [[unlikely]]
                OUT_OF_MEMORY(globalObject, scope);
        }
        ++matches;
        endOfLastMatch = match.end;
        findMatchBehind(globalObject, regExp, string, source, match);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } while (match);
    if (removes && !endOfLastMatch)
        return string;
    if (endOfLastMatch < source.length()) {
        if (!kept.tryConstructAndAppend(endOfLastMatch, source.length())) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
    }
    if (removes)
        RELEASE_AND_RETURN(scope, jsSpliceSubstrings(globalObject, string, source, kept.span()));
    RELEASE_AND_RETURN(scope, jsSpliceSubstringsWithSeparator(globalObject, string, source, kept.span().data(), kept.size(), replacement, replacementText, matches));
}

static JSString* replaceEveryMatchWithTemplate(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, const String& replacementText, size_t dollar, MatchResult match, const int* offsets)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringReplaceTemplateParts parts;
    parseReplacementTemplate(parts, replacementText, regExp, dollar);

    MatchResult first = match;
    StringBuilder firstReplacement(OverflowPolicy::RecordOverflow);
    appendReplacementUsingTemplate(firstReplacement, parts.span(), replacementText, source, offsets, regExp);
    if (firstReplacement.hasOverflowed()) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    findMatchBehind(globalObject, regExp, string, source, match);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!match) {
        auto resolved = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_AND_RETURN(scope, jsString(globalObject, resolved->substringSharingImpl(0, first.start), firstReplacement.toString(), resolved->substringSharingImpl(first.end)));
    }

    StringBuilder result(OverflowPolicy::RecordOverflow);
    result.reserveCapacity(source.length());
    result.append(source.left(first.start));
    result.append(firstReplacement);
    size_t endOfLastMatch = first.end;
    offsets = regExp->ovectorSpan().data();
    do {
        result.append(source.substring(endOfLastMatch, match.start - endOfLastMatch));
        appendReplacementUsingTemplate(result, parts.span(), replacementText, source, offsets, regExp);
        if (result.hasOverflowed()) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
        endOfLastMatch = match.end;
        findMatchBehind(globalObject, regExp, string, source, match);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } while (match);
    result.append(source.substring(endOfLastMatch));
    if (result.hasOverflowed()) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    RELEASE_AND_RETURN(scope, jsString(vm, result.toString()));
}

// The caller has searched the subject from its start for a RegExp that is not sticky, and offsets are those of the match. lastIndex of a global RegExp is 0.
static JSString* replaceFromFirstMatch(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExp* regExp, JSString* replacement, const String& replacementText, const int* offsets)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    MatchResult match(offsets[0], offsets[1]);
    RELEASE_ASSERT(match.start <= match.end && match.end <= source->length());
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, match, false);
    if (!regExp->global())
        RELEASE_AND_RETURN(scope, replaceOneMatch(globalObject, vm, string, source, regExp, replacementText, match, offsets));
    size_t dollar = replacementText.find('$');
    if (dollar == notFound)
        RELEASE_AND_RETURN(scope, replaceEveryMatchWithPlainText(globalObject, vm, string, source, regExp, replacement, replacementText, match));
    RELEASE_AND_RETURN(scope, replaceEveryMatchWithTemplate(globalObject, vm, string, source, regExp, replacementText, dollar, match, offsets));
}

// oneBehindStartOfFirstMatch is 0 where nobody has searched. Else the caller has searched the subject from its start; the code of a pattern has left the offsets of the match in the output of the VM.
JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExpString, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacementCell, uint32_t oneBehindStartOfFirstMatch))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceRegExpString);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceRegExpString);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceRegExpString);
    auto* replacement = cellArgument<JSString>(replacementCell, Entry::operationAOTStringReplaceRegExpString);
    if (!regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, replaceMatchesGeneric(globalObject, vm, subject, regExpObject, replacement));
    CallData callData;
    auto replacementText = replacement->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    RegExp* pattern = regExpObject->regExp();
    bool isAtom = pattern->hasValidAtom();
    // Every place of a text that is sought as it stands and removed is found in one pass.
    if (!oneBehindStartOfFirstMatch || (isAtom && pattern->global() && replacementText->isEmpty()))
        OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, subject, regExpObject, callData, replacementText, replacement));
    if (!isAtom)
        OPERATION_RETURN(scope, replaceFromFirstMatch(globalObject, vm, subject, pattern, replacement, replacementText, vm.aotState().regExpMatchOutput.get()));
    int start = static_cast<int>(oneBehindStartOfFirstMatch - 1);
    std::array offsets { start, start + static_cast<int>(pattern->atom().length()) };
    OPERATION_RETURN(scope, replaceFromFirstMatch(globalObject, vm, subject, pattern, replacement, replacementText, offsets.data()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSplitRegExp, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, uint32_t limit))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSplitRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringSplitRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringSplitRegExp);
    if (regExpObject->isSymbolSplitFastAndNonObservable(globalObject)) [[likely]]
        OPERATION_RETURN(scope, regExpSplitFast(globalObject, regExpObject, subject, limit));
    JSValue result = regExpSplitSlow(globalObject, regExpObject, subject, jsNumber(limit));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, result.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringMatchRegExp, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringMatchRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringMatchRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringMatchRegExp);
    if (regExpObject->isSymbolMatchFastAndNonObservable(globalObject)) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(regExpMatchFast(globalObject, regExpObject, subject)));
    JSValue matcher = globalObject->linkTimeConstant(LinkTimeConstant::regExpPrototypeSymbolMatch);
    auto callData = JSC::getCallData(matcher);
    ASSERT(callData.type != CallData::Type::None);
    auto args = WTF::toArray<EncodedJSValue>({
        JSValue::encode(subject),
    });
    OPERATION_RETURN(scope, JSValue::encode(call(globalObject, matcher, callData, regExpObject, ArgList { args.data(), args.size() })));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSearchRegExp, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSearchRegExp);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringSearchRegExp);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringSearchRegExp);
    if (regExpObject->isSymbolSearchFastAndNonObservable(globalObject)) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(regExpSearchFast(globalObject, regExpObject, subject)));
    OPERATION_RETURN(scope, JSValue::encode(regExpSearchGeneric(globalObject, regExpObject, subject)));
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpLiterals);

RegExpRecords::RegExpRecords(const Image& image)
    : m_records(image.section<ImageRegExp>(ImageSection::RegExps))
{
}

const ImageRegExp* findRegExpRecord(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags)
{
    if (!vm.hasCompiledCode())
        return nullptr;

    const Image& image = *vm.aotState().image;
    RegExpRecords records(image);
    uint16_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    for (const ImageRegExpHash& entry : std::ranges::equal_range(image.section<ImageRegExpHash>(ImageSection::RegExpHashes), ImageRegExp::hashOf(pattern.hash(), flags), { }, &ImageRegExpHash::hash)) {
        const ImageRegExp& record = records.at(RegExpRecordID(entry.record));
        if (record.rawFlags == rawFlags && image.text().equals(StringID(record.pattern), pattern))
            return &record;
    }
    count(vm, RuntimeCounterID::RegExpsCreatedWithoutRecord);
    return nullptr;
}

RegExpEntrypoints regExpEntrypointsOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.form() == RegExpRecordForm::Code);
    std::span<const uint8_t> code = vm.aotState().image->regExpCode();
    return { code.subspan(record.codeFor8Bit).data(), code.subspan(record.codeFor16Bit).data() };
}

static std::span<const uint8_t> detailsOf(VM& vm, const ImageRegExp& record)
{
    return vm.aotState().image->section<uint8_t>(ImageSection::RegExpDetails).subspan(record.details);
}

Yarr::CharacterSetView regExpCharacterSetOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.hasCharacterSet());
    std::span<const uint8_t> rest = detailsOf(vm, record);
    const auto& set = consumeAndReinterpretCastTo<ImageRegExpCharacterSet>(rest);
    return Yarr::CharacterSetView(set.bits, spanReinterpretCast<const Yarr::CharacterRange>(rest.first(set.bits.wideRangeCount * sizeof(Yarr::CharacterRange))));
}

String regExpPatternOf(VM& vm, const ImageRegExp& record)
{
    return vm.aotState().image->text().createString(StringID(record.pattern));
}

String regExpAtomOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.form() == RegExpRecordForm::Atom);
    return vm.aotState().image->text().createString(StringID(record.atom));
}

RegExpNamedGroups decodeRegExpNamedGroups(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.hasNamedGroups);
    const ImageText& text = vm.aotState().image->text();
    std::span<const uint8_t> rest = detailsOf(vm, record);
    const auto& header = consumeAndReinterpretCastTo<ImageRegExpNamedGroups>(rest);
    auto words = spanReinterpretCast<const uint32_t>(rest.first(header.numberOfWords * sizeof(uint32_t)));

    RegExpNamedGroups groups;
    groups.numDuplicateNamedCaptureGroups = header.numDuplicateNamedCaptureGroups;
    groups.captureGroupNames = FixedVector<AtomString>(header.numberOfCaptureGroupNames);
    for (AtomString& name : groups.captureGroupNames) {
        if (uint32_t stringPlusOne = consume(words))
            name = AtomString(text.createAtom(StringID(stringPlusOne - 1)));
    }
    for (uint32_t group = 0; group < header.numberOfNamedGroups; ++group) {
        String name = text.createAtom(StringID(consume(words)));
        uint32_t numberOfIndices = consume(words);
        groups.namedGroupToParenIndices.add(WTF::move(name), Vector<unsigned>(consumeSpan(words, numberOfIndices)));
    }
    RELEASE_ASSERT(words.empty());
    return groups;
}

namespace {

struct Literal {
    const ImageRegExp* record;
    OptionSet<Yarr::Flags> flags;
};

struct LiteralTranslator {
    static unsigned hash(const Literal& literal)
    {
        constexpr unsigned bitsAboveFlags = 64 - 8 * sizeof(literal.flags);
        return intHash(static_cast<uint64_t>(std::bit_cast<uintptr_t>(literal.record)) ^ static_cast<uint64_t>(literal.flags.toRaw()) << bitsAboveFlags);
    }

    static bool equal(RegExp* regExp, const Literal& literal)
    {
        return regExp->imageRecord() == literal.record && regExp->flags() == literal.flags;
    }
};

} // anonymous namespace

unsigned RegExpLiterals::CellHash::hash(RegExp* regExp)
{
    return LiteralTranslator::hash({ regExp->imageRecord(), regExp->flags() });
}

template<typename Visitor>
void RegExpLiterals::visitRoots(Visitor& visitor)
{
    // A cell lasts as long as the program. One that a collection has marked is old, and only the next full collection has to mark it again.
    size_t first = 0;
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (visitor.collectionScope() == CollectionScope::Full && m_lastFullVisitVersion != visitor.markingVersion())
            m_lastFullVisitVersion = visitor.markingVersion();
        else
            first = m_numberOfCellsVisited;
        m_numberOfCellsVisited = m_cellsInOrderOfCreation.size();
    }
    for (RegExp* regExp : m_cellsInOrderOfCreation.span().subspan(first))
        visitor.appendUnbarriered(regExp);
}

RegExpLiterals::RegExpLiterals(CacheRegistry& registry)
{
    registry.add(CacheKind::StrongRoots, "regexp literals"_s, *this, RootVolatility::GreyedByExecution);
}

RegExp* RegExpLiterals::regExpFor(VM& vm, RegExpRecordID id, OptionSet<Yarr::Flags> flags)
{
    const ImageRegExp& record = RegExpRecords(*vm.aotState().image).at(id);
    if (auto known = m_cells.find<LiteralTranslator>(Literal { &record, flags }); known != m_cells.end())
        return *known;

    RegExp* regExp = RegExp::createFromImage(vm, regExpPatternOf(vm, record), flags, record);
    m_cells.add(regExp);
    m_cellsInOrderOfCreation.append(regExp);
    return regExp;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

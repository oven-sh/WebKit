/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTCheckedSpan.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeReporter.h"
#include "AOTState.h"
#include "AOTStubsRegExp.h"
#include "AOTVMState.h"
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

static ALWAYS_INLINE MatchResult matchFromStart(JSGlobalObject* globalObject, VM& vm, RegExp* regExp, JSString* string, StringView input)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    MatchResult result = regExp->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, 0);
    RETURN_IF_EXCEPTION(scope, MatchResult::failed());
    if (result)
        globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, result, false);
    return result;
}

static ALWAYS_INLINE bool testRegExp(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    if (regExp->globalOrSticky() || !object->getLastIndex().isUInt32()) [[unlikely]]
        RELEASE_AND_RETURN(scope, object->test(globalObject, string));
    auto input = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, !!matchFromStart(globalObject, vm, regExp, string, input));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpTest, size_t, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpTest);
    OPERATION_RETURN(scope, testRegExp(globalObject, vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpTest), cellArgument<JSString>(string, Entry::operationAOTRegExpTest)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpTestBoxed, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpTestBoxed);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(testRegExp(globalObject, vm, cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpTestBoxed), cellArgument<JSString>(string, Entry::operationAOTRegExpTestBoxed)))));
}

static RegExp* regExpOfRecord(VM& vm, uint32_t record)
{
    RegExpRecordID id(record);
    RELEASE_ASSERT(id);
    return vm.aotState().regExpLiterals->regExpFor(vm, id, RegExpRecords(*vm.aotState().image).at(id).flags());
}

static std::span<int> matchOutput(VM& vm, RegExp* regExp)
{
    size_t size = regExp->offsetVectorSize();
    RELEASE_ASSERT(size <= vm.aotState().sizeOfRegExpMatchOutput);
    return unsafeMakeSpan(vm.aotState().regExpMatchOutput.get(), size);
}

static void leaveOffsetsInMatchOutput(VM& vm, RegExp* regExp, std::span<const int> offsets)
{
    std::span<int> output = matchOutput(vm, regExp);
    memcpySpan(output, offsets.first(output.size()));
}

static JSValue createMatchValue(Instance* instance, RegExpMatchForm form, RegExp* regExp, JSString* subject, MatchResult result, std::span<const int> offsets)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    switch (form) {
    case RegExpMatchForm::Test:
        return jsBoolean(true);
    case RegExpMatchForm::Parts:
        leaveOffsetsInMatchOutput(vm, regExp, offsets);
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
    leaveCellsForRopes(instance);
    return createRegExpMatchesArrayForPlainRegExp(vm, globalObject, subject, result, offsets, numSubpatterns);
}

static ALWAYS_INLINE void findNextMatch(JSGlobalObject* globalObject, RegExp* regExp, JSString* string, StringView source, MatchResult& match)
{
    uint64_t start = match.empty() ? advanceStringIndex(source, source.length(), match.end, regExp->eitherUnicode()) : match.end;
    if (start > source.length()) {
        match = MatchResult::failed();
        return;
    }
    int* offsets;
    match = globalObject->regExpGlobalData().performMatch(globalObject, regExp, string, source, static_cast<int>(start), &offsets);
}

class RunOfMatches {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(RunOfMatches);
public:
    RunOfMatches(JSGlobalObject* globalObject, VM& vm, RegExp* regExp, JSString* string, StringView source)
        : m_globalObject(globalObject)
        , m_vm(vm)
        , m_regExp(regExp)
        , m_string(string)
        , m_source(source)
    {
        if (regExp->hasImageCodeFor(source.is8Bit()) && (source.is8Bit() || !regExp->eitherUnicode()))
            m_context.emplace(vm, regExp, Yarr::MatchFrom::VMThread);
    }

    ~RunOfMatches()
    {
        m_context.reset();
        if (m_last)
            m_globalObject->regExpGlobalData().recordMatch(m_vm, m_globalObject, m_regExp, m_string, m_last, false);
    }

    ALWAYS_INLINE void findNext(MatchResult& match)
    {
        if (m_context) [[likely]] {
            uint64_t start = match.empty() ? advanceStringIndex(m_source, m_source.length(), match.end, m_regExp->eitherUnicode()) : match.end;
            if (start > m_source.length()) {
                match = MatchResult::failed();
                return;
            }
            int found = m_source.is8Bit() ? m_regExp->executeImageCode(m_source.span8(), static_cast<unsigned>(start), *m_context) : m_regExp->executeImageCode(m_source.span16(), static_cast<unsigned>(start), *m_context);
            if (found >= 0) [[likely]] {
                match = MatchResult(found, m_regExp->ovectorSpan()[1]);
                m_last = match;
                return;
            }
            if (found == -1) {
                match = MatchResult::failed();
                return;
            }
            m_context.reset();
            if (m_last)
                m_globalObject->regExpGlobalData().recordMatch(m_vm, m_globalObject, m_regExp, m_string, m_last, false);
            m_last = MatchResult::failed();
        }
        findNextMatch(m_globalObject, m_regExp, m_string, m_source, match);
    }

private:
    JSGlobalObject* m_globalObject;
    VM& m_vm;
    RegExp* m_regExp;
    JSString* m_string;
    StringView m_source;
    std::optional<Yarr::MatchingContextHolder> m_context;
    MatchResult m_last { MatchResult::failed() };
};

static void recordLastMatch(JSGlobalObject* globalObject, VM& vm, RegExp* regExp, JSString* string, const Vector<unsigned, 128>& matches)
{
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, MatchResult { matches[matches.size() - 2], matches.last() }, false);
}

static JSValue matchEveryPlaceInOnePass(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    if (!regExp->global())
        return { };
    auto source = subject->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    Vector<unsigned, 128> matches;
    if (!regExp->collectMatchesOfOneSetOrRun(vm, source, matches))
        return { };
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (matches.isEmpty())
        return jsNull();
    JSArray* array = constructEmptyArray(globalObject, nullptr, matches.size() / 2);
    RETURN_IF_EXCEPTION(scope, { });
    for (size_t i = 0; i < matches.size(); i += 2) {
        array->putDirectIndex(globalObject, i / 2, jsSubstringOfResolved(vm, subject, matches[i], matches[i + 1] - matches[i]));
        RETURN_IF_EXCEPTION(scope, { });
    }
    recordLastMatch(globalObject, vm, regExp, subject, matches);
    return array;
}

static JSValue collectMatchesFrom(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject, MatchResult match)
{
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
    RunOfMatches run(globalObject, vm, regExp, subject, source);
    do {
        array->putDirectIndex(globalObject, count++, jsSubstringOfResolved(vm, subject, match.start, match.end - match.start));
        RETURN_IF_EXCEPTION(scope, { });
        run.findNext(match);
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

    std::span<int> offsets = matchOutput(vm, cell);
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

static ALWAYS_INLINE int64_t answerOfFlatProgram(Yarr::JSRegExpResult result, const uint32_t* output)
{
    switch (result) {
    case Yarr::JSRegExpResult::Match:
        return output[0];
    case Yarr::JSRegExpResult::NoMatch:
        return -1;
    default:
        return static_cast<int64_t>(Yarr::JSRegExpResult::JITCodeFailure);
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRegExpMatchFlatProgram8, int64_t, (void* program, const Latin1Character* characters, uint32_t length, uint32_t* output, void* instance))
{
    countEvent(*static_cast<Instance*>(instance)->vm, RuntimeCounterID::RegExpMatchesInterpreted);
    return answerOfFlatProgram(Yarr::matchFlatProgram(*static_cast<const Yarr::FlatProgram*>(program), std::span { characters, length }, 0, output), output);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRegExpMatchFlatProgram16, int64_t, (void* program, const char16_t* characters, uint32_t length, uint32_t* output, void* instance))
{
    countEvent(*static_cast<Instance*>(instance)->vm, RuntimeCounterID::RegExpMatchesInterpreted);
    const auto& flatProgram = *static_cast<const Yarr::FlatProgram*>(program);
    StringView subject(std::span { characters, length });
    unsigned start = 0;
    while (true) {
        Yarr::JSRegExpResult result = Yarr::matchFlatProgram(flatProgram, subject, start, output);
        if (result == Yarr::JSRegExpResult::JITCodeFailure) [[unlikely]]
            result = Yarr::matchFlatProgramBehindSurrogate(flatProgram, subject, start, output);
        if (result != Yarr::JSRegExpResult::Match || !flatProgram.isInUnicodeMode || !RegExp::splitsSurrogatePair(subject, output[0])) [[likely]]
            return answerOfFlatProgram(result, output);
        start = output[0] + 1;
    }
}

static JSValue valueOfFailedMatch(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Parts:
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

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpMatchInParts, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpMatchInParts);
    auto* object = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpMatchInParts);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTRegExpMatchInParts);
    auto input = subject->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    unsigned lastIndex = getRegExpObjectLastIndexAsUnsigned(globalObject, object, input);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    RegExp* cell = object->regExp();
    bool globalOrSticky = cell->globalOrSticky();
    if (lastIndex == UINT_MAX && globalOrSticky) {
        object->setLastIndex(globalObject, 0);
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    }
    if (!globalOrSticky)
        lastIndex = 0;

    auto offsets = cell->ovectorSpan();
    int position = cell->matchInline<Yarr::MatchFrom::VMThread>(globalObject, vm, input, lastIndex, offsets);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (position == -1) {
        if (globalOrSticky)
            object->setLastIndex(globalObject, 0);
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    }

    MatchResult result(position, offsets[1]);
    if (globalOrSticky) {
        object->setLastIndex(globalObject, result.end);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, cell, subject, result, false);
    leaveOffsetsInMatchOutput(vm, cell, offsets);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(true)));
}

static RegExpObject* ensureSharedRegExpLiteral(VM& vm, Instance* instance, uint32_t staticsOffset, RegExp* regExp)
{
    ProgramStatics& statics = *instance->state->statics;
    if (JSCell* cell = statics.at<JSCell*>(staticsOffset)) {
        auto* shared = dynamicDowncast<RegExpObject>(cell);
        RELEASE_ASSERT(shared && shared->regExp() == regExp);
        return shared;
    }
    RegExpObject* shared = RegExpObject::create(vm, WTF::opaque(instance->globalObject->regExpStructure()), regExp);
    vm.mutatorFence();
    statics.setCell(staticsOffset, shared);
    return shared;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExpLiteral, JSCell*, (Instance* instance, uint32_t regExpRecord, uint32_t flags, uint32_t staticsOffset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRegExpLiteral);
    RegExpLiterals& literals = *vm.aotState().regExpLiterals;
    RegExp* regExp = literals.regExpIfCreated(RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags));
    if (regExp && staticsOffset != noSharedRegExpOffset)
        ensureSharedRegExpLiteral(vm, instance, staticsOffset, regExp);
    if (!regExp)
        regExp = literals.regExpFor(vm, RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags));
    ASSERT(regExp->isValid());
    Structure* structure = WTF::opaque(globalObject->regExpStructure());
    prepareInlineAllocation<RegExpObject>(instance, InlineAllocation::RegExpObject, structure);
    OPERATION_RETURN(scope, RegExpObject::create(vm, structure, regExp));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSharedRegExpLiteral, JSCell*, (Instance* instance, uint32_t staticsOffset, uint32_t regExpRecord, uint32_t flags))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTSharedRegExpLiteral);
    DeferTerminationForAWhile deferTermination(vm);
    RegExp* regExp = vm.aotState().regExpLiterals->regExpFor(vm, RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags));
    ASSERT(regExp->isValid());
    OPERATION_RETURN(scope, ensureSharedRegExpLiteral(vm, instance, staticsOffset, regExp));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyOfSharedRegExpLiteral, JSCell*, (Instance* instance, JSCell* shared))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyOfSharedRegExpLiteral);
    RegExp* regExp = cellArgument<RegExpObject>(shared, Entry::operationAOTCopyOfSharedRegExpLiteral)->regExp();
    OPERATION_RETURN(scope, RegExpObject::create(vm, WTF::opaque(globalObject->regExpStructure()), regExp));
}

static const ImageRegExp* findRegExpRecordWithoutCounting(VM& vm, StringView pattern, unsigned hash, OptionSet<Yarr::Flags> flags)
{
    uint16_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    const Image& image = *vm.aotState().image;
    RegExpRecords records(image);
    for (const ImageRegExpHash& entry : std::ranges::equal_range(image.section<ImageRegExpHash>(ImageSection::RegExpHashes), ImageRegExp::hashOf(hash, flags), { }, &ImageRegExpHash::hash)) {
        const ImageRegExp& record = records.at(RegExpRecordID(entry.record));
        if (record.rawFlags == rawFlags && image.text().equals(StringID(record.pattern), pattern))
            return &record;
    }
    return nullptr;
}

static String textOfImageForPattern(VM& vm, JSString& text, OptionSet<Yarr::Flags> flags)
{
    auto find = [&](StringView pattern, unsigned hash) -> String {
        const ImageRegExp* record = findRegExpRecordWithoutCounting(vm, pattern, hash, flags);
        if (!record)
            return { };
        countEvent(vm, RuntimeCounterID::RegExpPatternsFoundInImage);
        return vm.aotState().image->text().createString(StringID(record->pattern));
    };
    if (StringImpl* flat = text.tryGetValueImpl())
        return find(StringView(*flat), flat->hash());
    auto resolveAndFind = [&](auto& buffer) -> String {
        if (!buffer.tryGrow(text.length()))
            return { };
        text.resolveToBuffer(buffer.mutableSpan());
        StringView resolved(buffer.span());
        return find(resolved, resolved.hash());
    };
    if (text.is8Bit()) {
        Vector<Latin1Character, 2048> buffer;
        return resolveAndFind(buffer);
    }
    Vector<char16_t, 1024> buffer;
    return resolveAndFind(buffer);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExp, JSCell*, (Instance* instance, JSCell* patternCell, uint32_t flags))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRegExp);
    auto* text = cellArgument<JSString>(patternCell, Entry::operationAOTNewRegExp);
    if (String foreseen = textOfImageForPattern(vm, *text, OptionSet<Yarr::Flags>::fromRaw(flags)); !foreseen.isNull())
        OPERATION_RETURN(scope, regExpCreate(globalObject, JSValue(), foreseen, OptionSet<Yarr::Flags>::fromRaw(flags)));
    auto pattern = text->value(globalObject);
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

static JSString* replaceInOnePass(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* object, JSString* replacement, const String& replacementText, bool& hasReplaced)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    hasReplaced = false;
    RegExp* regExp = object->regExp();
    if (!regExp->global() || replacementText.find('$') != notFound)
        return nullptr;
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Vector<unsigned, 128> matches;
    if (!regExp->collectMatchesOfOneSetOrRun(vm, source, matches))
        return nullptr;
    hasReplaced = true;
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (matches.isEmpty())
        return string;
    Vector<Range<int32_t>, 64> kept;
    bool removes = replacementText.isEmpty();
    size_t endOfLastMatch = 0;
    for (size_t i = 0; i < matches.size(); i += 2) {
        if ((!removes || endOfLastMatch < matches[i]) && !kept.tryConstructAndAppend(endOfLastMatch, matches[i])) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
        endOfLastMatch = matches[i + 1];
    }
    if (endOfLastMatch < source->length() && !kept.tryConstructAndAppend(endOfLastMatch, source->length())) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    recordLastMatch(globalObject, vm, regExp, string, matches);
    if (removes)
        RELEASE_AND_RETURN(scope, jsSpliceSubstrings(globalObject, string, source, kept.span()));
    RELEASE_AND_RETURN(scope, jsSpliceSubstringsWithSeparator(globalObject, string, source, kept.span().data(), kept.size(), replacement, replacementText, matches.size() / 2));
}

static JSString* replaceMatches(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* regExpObject, JSValue replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[likely]] {
        if (replacement.isString()) {
            auto text = asString(replacement)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            bool hasReplaced;
            JSString* result = replaceInOnePass(globalObject, vm, string, regExpObject, asString(replacement), text, hasReplaced);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (hasReplaced)
                return result;
        }
        RELEASE_AND_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, string, regExpObject, replacement));
    }
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

static JSString* removeEveryMatch(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, MatchResult match)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<Range<int32_t>, 64> kept;
    size_t endOfLastMatch = 0;
    {
        RunOfMatches run(globalObject, vm, regExp, string, source);
        do {
            if (endOfLastMatch < match.start) {
                if (!kept.tryConstructAndAppend(endOfLastMatch, match.start)) [[unlikely]]
                    OUT_OF_MEMORY(globalObject, scope);
            }
            endOfLastMatch = match.end;
            run.findNext(match);
            RETURN_IF_EXCEPTION(scope, nullptr);
        } while (match);
    }
    if (!endOfLastMatch)
        return string;
    if (endOfLastMatch < source.length()) {
        if (!kept.tryConstructAndAppend(endOfLastMatch, source.length())) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
    }
    RELEASE_AND_RETURN(scope, jsSpliceSubstrings(globalObject, string, source, kept.span()));
}

static JSString* replaceEveryMatchWithPlainText(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp, JSString* replacement, const String& replacementText, MatchResult match)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<Range<int32_t>, 16> kept;
    size_t endOfLastMatch = 0;
    int matches = 0;
    {
        RunOfMatches run(globalObject, vm, regExp, string, source);
        do {
            if (!kept.tryConstructAndAppend(endOfLastMatch, match.start)) [[unlikely]]
                OUT_OF_MEMORY(globalObject, scope);
            ++matches;
            endOfLastMatch = match.end;
            run.findNext(match);
            RETURN_IF_EXCEPTION(scope, nullptr);
        } while (match);
    }
    if (endOfLastMatch < source.length()) {
        if (!kept.tryConstructAndAppend(endOfLastMatch, source.length())) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);
    }
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
    RunOfMatches run(globalObject, vm, regExp, string, source);
    run.findNext(match);
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
        run.findNext(match);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } while (match);
    result.append(source.substring(endOfLastMatch));
    if (result.hasOverflowed()) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    RELEASE_AND_RETURN(scope, jsString(vm, result.toString()));
}

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
    if (replacementText.isEmpty())
        RELEASE_AND_RETURN(scope, removeEveryMatch(globalObject, vm, string, source, regExp, match));
    size_t dollar = replacementText.find('$');
    if (dollar == notFound)
        RELEASE_AND_RETURN(scope, replaceEveryMatchWithPlainText(globalObject, vm, string, source, regExp, replacement, replacementText, match));
    RELEASE_AND_RETURN(scope, replaceEveryMatchWithTemplate(globalObject, vm, string, source, regExp, replacementText, dollar, match, offsets));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExpString, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacementCell, uint32_t startOfFirstMatchPlusOne))
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
    if (!startOfFirstMatchPlusOne) {
        bool hasReplaced;
        JSString* result = replaceInOnePass(globalObject, vm, subject, regExpObject, replacement, replacementText, hasReplaced);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        if (hasReplaced)
            OPERATION_RETURN(scope, result);
    }
    if (!startOfFirstMatchPlusOne || (isAtom && pattern->global() && replacementText->isEmpty()))
        OPERATION_RETURN(scope, replaceUsingRegExpSearch(vm, globalObject, subject, regExpObject, callData, replacementText, replacement));
    if (!isAtom)
        OPERATION_RETURN(scope, replaceFromFirstMatch(globalObject, vm, subject, pattern, replacement, replacementText, vm.aotState().regExpMatchOutput.get()));
    int start = static_cast<int>(startOfFirstMatchPlusOne - 1);
    std::array offsets { start, start + static_cast<int>(pattern->atom().length()) };
    OPERATION_RETURN(scope, replaceFromFirstMatch(globalObject, vm, subject, pattern, replacement, replacementText, offsets.data()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceRegExpWithFunction, JSCell*, (Instance* instance, JSCell* string, JSCell* regExp, JSCell* replacement))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceRegExpWithFunction);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringReplaceRegExpWithFunction);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringReplaceRegExpWithFunction);
    if (!regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, replaceMatches(globalObject, vm, subject, regExpObject, replacement));
    JSValue result = callMethodInEngine(instance, Builtins::StringPrototypeReplace, subject, regExpObject, replacement);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, result.asCell());
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
    if (regExpObject->isSymbolMatchFastAndNonObservable(globalObject)) [[likely]] {
        JSValue value = matchEveryPlaceInOnePass(globalObject, vm, regExpObject, subject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (value)
            OPERATION_RETURN(scope, JSValue::encode(value));
        OPERATION_RETURN(scope, JSValue::encode(regExpMatchFast(globalObject, regExpObject, subject)));
    }
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

    if (const ImageRegExp* record = findRegExpRecordWithoutCounting(vm, pattern, pattern.hash(), flags))
        return record;
    countEvent(vm, RuntimeCounterID::RegExpsCreatedWithoutRecord);
    return nullptr;
}

RegExpEntrypoints regExpEntrypointsOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.form() == RegExpRecordForm::Code);
    std::span<const uint8_t> code = vm.aotState().image->regExpCode();
    return { checkedSubspan(code, record.codeFor8Bit).data(), checkedSubspan(code, record.codeFor16Bit).data() };
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

template<typename Visitor>
void RegExpLiterals::visitRoots(Visitor& visitor)
{
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

uint64_t RegExpLiterals::keyOf(RegExpRecordID id, OptionSet<Yarr::Flags> flags)
{
    static_assert(sizeof(flags.toRaw()) <= sizeof(uint32_t));
    return static_cast<uint64_t>(id.index()) << 32 | flags.toRaw();
}

RegExp* RegExpLiterals::regExpIfCreated(RegExpRecordID id, OptionSet<Yarr::Flags> flags) const
{
    uint32_t firstCell = m_records ? m_firstCellOfRecord[id.index()] : 0;
    if (!firstCell)
        return nullptr;
    if (RegExp* first = m_cellsInOrderOfCreation[firstCell - 1]; first->flags() == flags) [[likely]]
        return first;
    return m_cellsWithOtherFlags.get(keyOf(id, flags));
}

RegExp* RegExpLiterals::regExpFor(VM& vm, RegExpRecordID id, OptionSet<Yarr::Flags> flags)
{
    if (RegExp* known = regExpIfCreated(id, flags))
        return known;
    if (!m_records) [[unlikely]] {
        RegExpRecords records(*vm.aotState().image);
        m_records = &records.at(id) - id.index();
        m_firstCellOfRecord.fill(0, records.size());
    }
    const ImageRegExp& record = m_records[id.index()];
    RegExp* regExp = RegExp::createFromImage(vm, regExpPatternOf(vm, record), flags, record);
    if (uint32_t& firstCell = m_firstCellOfRecord[id.index()])
        m_cellsWithOtherFlags.add(keyOf(id, flags), regExp);
    else
        firstCell = static_cast<uint32_t>(m_cellsInOrderOfCreation.size() + 1);
    m_cellsInOrderOfCreation.append(regExp);
    return regExp;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

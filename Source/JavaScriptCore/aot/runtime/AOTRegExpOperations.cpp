/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTCheckedSpan.h"
#include "AOTCompiledCall.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTRegExpFlags.h"
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

static JSValue createArrayOfLength(JSGlobalObject* globalObject, VM& vm, size_t length)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!length)
        return jsNull();
    JSArray* array = length > MAX_STORAGE_VECTOR_LENGTH ? nullptr : JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithArrayStorage), static_cast<unsigned>(length));
    if (!array) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    return array;
}

static JSValue countMatchesFrom(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject, MatchResult match)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, { });
    auto source = subject->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, subject, match, false);
    size_t count = 0;
    {
        RunOfMatches run(globalObject, vm, regExp, subject, source);
        do {
            ++count;
            run.findNext(match);
            RETURN_IF_EXCEPTION(scope, { });
        } while (match);
    }
    RELEASE_AND_RETURN(scope, createArrayOfLength(globalObject, vm, count));
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
        OPERATION_RETURN(scope, JSValue::encode(object->isSharedLiteral() ? countMatchesFrom(globalObject, vm, object, subject, result) : collectMatchesFrom(globalObject, vm, object, subject, result)));

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

JSC_DEFINE_JIT_OPERATION(operationAOTRegExpMatchArray, EncodedJSValue, (Instance* instance, JSCell* regExp, JSCell* string, void* record))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRegExpMatchArray);
    RegExp* cell = cellArgument<RegExpObject>(regExp, Entry::operationAOTRegExpMatchArray)->regExp();
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTRegExpMatchArray);
    std::span<int> offsets = matchOutput(vm, cell);
    const auto* words = static_cast<const uint64_t*>(record);
    RELEASE_ASSERT(words[0] == cell->numSubpatterns() + 1u);
    std::span<const int> bounds = unsafeMakeSpan(reinterpret_cast<const int*>(words + 1), 2 * static_cast<size_t>(words[0]));
    memcpySpan(offsets.first(bounds.size()), bounds);
    RELEASE_ASSERT(offsets[0] >= 0 && offsets[0] <= offsets[1] && static_cast<unsigned>(offsets[1]) <= subject->length());
    OPERATION_RETURN(scope, JSValue::encode(createMatchValue(instance, RegExpMatchForm::Exec, cell, subject, MatchResult(offsets[0], offsets[1]), offsets)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyRegExpMatch, void*, (Instance* instance, uint32_t count))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyRegExpMatch);
    size_t bytes = sizeof(uint64_t) * (1 + static_cast<size_t>(count));
    RELEASE_ASSERT(bytes - sizeof(uint64_t) <= vm.aotState().sizeOfRegExpMatchOutput * sizeof(int));
    auto* record = static_cast<uint64_t*>(vm.auxiliarySpace().allocate(vm, bytes, nullptr, AllocationFailureMode::ReturnNull));
    if (!record) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    record[0] = count;
    memcpy(record + 1, vm.aotState().regExpMatchOutput.get(), bytes - sizeof(uint64_t));
    vm.mutatorFence();
    OPERATION_RETURN(scope, record);
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

static RegExpObject* ensureSharedRegExpLiteral(VM& vm, Instance* instance, uint32_t staticsOffset, RegExp* regExp, bool countsMatches = false)
{
    ProgramStatics& statics = *instance->state->statics;
    if (JSCell* cell = statics.at<JSCell*>(staticsOffset)) {
        auto* shared = dynamicDowncast<RegExpObject>(cell);
        RELEASE_ASSERT(shared && shared->regExp() == regExp);
        return shared;
    }
    Structure* structure = WTF::opaque(instance->globalObject->regExpStructure());
    RegExpObject* shared = countsMatches ? RegExpObject::createSharedLiteral(vm, structure, regExp) : RegExpObject::create(vm, structure, regExp);
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
    RegExp* regExp = vm.aotState().regExpLiterals->regExpFor(vm, RegExpRecordID(regExpRecord), OptionSet<Yarr::Flags>::fromRaw(flags & ~Typed::countedMatchesParameter));
    ASSERT(regExp->isValid());
    OPERATION_RETURN(scope, ensureSharedRegExpLiteral(vm, instance, staticsOffset, regExp, flags & Typed::countedMatchesParameter));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyOfSharedRegExpLiteral, JSCell*, (Instance* instance, JSCell* shared))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyOfSharedRegExpLiteral);
    RegExp* regExp = cellArgument<RegExpObject>(shared, Entry::operationAOTCopyOfSharedRegExpLiteral)->regExp();
    OPERATION_RETURN(scope, RegExpObject::create(vm, WTF::opaque(globalObject->regExpStructure()), regExp));
}

static std::optional<RegExpRecordID> findRegExpRecordID(VM& vm, StringView pattern, unsigned hash, OptionSet<Yarr::Flags> flags)
{
    uint16_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    const Image& image = *vm.aotState().image;
    RegExpRecords records(image);
    for (const ImageRegExpHash& entry : std::ranges::equal_range(image.section<ImageRegExpHash>(ImageSection::RegExpHashes), ImageRegExp::hashOf(hash, flags), { }, &ImageRegExpHash::hash)) {
        const ImageRegExp& record = records.at(RegExpRecordID(entry.record));
        if (record.rawFlags == rawFlags && image.text().equals(StringID(record.pattern), pattern))
            return RegExpRecordID(entry.record);
    }
    return std::nullopt;
}

static RegExpLiterals::RopeWords ropeWordsOf(const JSString& rope)
{
    ASSERT(rope.isRope());
    static_assert(sizeof(JSRopeString) == JSString::offsetOfValue() + sizeof(RegExpLiterals::RopeWords));
    return WTF::unalignedLoad<RegExpLiterals::RopeWords>(std::bit_cast<const uint8_t*>(&rope) + JSString::offsetOfValue());
}

static std::optional<RegExpRecordID> findRegExpRecordForPattern(VM& vm, JSString& text, OptionSet<Yarr::Flags> flags)
{
    auto find = [&](StringView pattern, unsigned hash) {
        std::optional<RegExpRecordID> record = findRegExpRecordID(vm, pattern, hash, flags);
        if (record)
            countEvent(vm, RuntimeCounterID::RegExpPatternsFoundInImage);
        return record;
    };
    if (StringImpl* flat = text.tryGetValueImpl())
        return find(StringView(*flat), flat->hash());
    const RegExpLiterals::RopeWords words = ropeWordsOf(text);
    RegExpLiterals::RopePattern& known = vm.aotState().regExpLiterals->ropePattern(words);
    if (known.recordNumber && known.words == words && known.flags == flags.toRaw()) {
        countEvent(vm, RuntimeCounterID::RegExpPatternsFoundInImage);
        return RegExpRecordID(known.recordNumber - 1);
    }
    auto resolveAndFind = [&](auto& buffer) -> std::optional<RegExpRecordID> {
        if (!buffer.tryGrow(text.length()))
            return std::nullopt;
        text.resolveToBuffer(buffer.mutableSpan());
        StringView resolved(buffer.span());
        std::optional<RegExpRecordID> record = find(resolved, resolved.hash());
        if (record)
            known = { words, flags.toRaw(), record->index() + 1 };
        return record;
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
    if (std::optional<RegExpRecordID> record = findRegExpRecordForPattern(vm, *text, OptionSet<Yarr::Flags>::fromRaw(flags))) {
        if (RegExp* regExp = vm.aotState().regExpLiterals->regExpFor(vm, *record, OptionSet<Yarr::Flags>::fromRaw(flags)); regExp->isValid()) [[likely]]
            OPERATION_RETURN(scope, RegExpObject::create(vm, WTF::opaque(globalObject->regExpStructure()), regExp));
    }
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

static constexpr unsigned mostOperandsOfReplacer = 6;
static constexpr unsigned operandsOfReplacerBesideGroups = 3;

static JSString* replaceByCompiledCode(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExpObject* object, JSObject* replacer, const CallData& callData)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    unsigned length = source->length();
    unsigned numberOfGroups = regExp->numSubpatterns() + 1;
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, nullptr);

    Vector<Range<int32_t>, 16> kept;
    Vector<String, 16> replacements;
    size_t endOfLastMatch = 0;
    uint64_t start = 0;
    while (start <= length) {
        int* groups;
        MatchResult match = globalObject->regExpGlobalData().performMatch(globalObject, regExp, string, source, static_cast<unsigned>(start), &groups);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!match)
            break;
        if (!kept.tryConstructAndAppend(endOfLastMatch, match.start)) [[unlikely]]
            OUT_OF_MEMORY(globalObject, scope);

        std::array<EncodedJSValue, mostOperandsOfReplacer> operands;
        unsigned numberOfOperands = 0;
        for (unsigned group = 0; group < numberOfGroups; ++group) {
            int first = groups[group * 2];
            int end = groups[group * 2 + 1];
            JSValue text = jsUndefined();
            if (first >= 0 && end >= first) {
                text = jsSubstring(globalObject, vm, string, first, end - first);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
            operands[numberOfOperands++] = JSValue::encode(text);
        }
        operands[numberOfOperands++] = JSValue::encode(jsNumber(match.start));
        operands[numberOfOperands++] = JSValue::encode(string);
        JSValue answer = callCompiledCode(vm, replacer, callData, jsUndefined(), nullptr, ArgList { operands.data(), numberOfOperands });
        RETURN_IF_EXCEPTION(scope, nullptr);
        replacements.append(answer.toWTFString(globalObject));
        RETURN_IF_EXCEPTION(scope, nullptr);

        endOfLastMatch = match.end;
        start = match.empty() ? advanceStringIndex(source, length, match.end, regExp->eitherUnicode()) : match.end;
    }
    if (replacements.isEmpty())
        return string;
    if (endOfLastMatch < length && !kept.tryConstructAndAppend(endOfLastMatch, length)) [[unlikely]]
        OUT_OF_MEMORY(globalObject, scope);
    RELEASE_AND_RETURN(scope, jsSpliceSubstringsWithSeparators(globalObject, string, source, kept.span().data(), kept.size(), replacements.span().data(), replacements.size()));
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
        if (RegExp* regExp = regExpObject->regExp(); replacement.isCell() && regExp->global() && !regExp->hasNamedCaptures() && regExp->numSubpatterns() + operandsOfReplacerBesideGroups <= mostOperandsOfReplacer && string->length() < Options::thresholdForStringReplaceCache()) {
            if (CallData callData = compiledCallDataOf(replacement.asCell()); callData.type == CallData::Type::Compiled)
                RELEASE_AND_RETURN(scope, replaceByCompiledCode(globalObject, vm, string, regExpObject, asObject(replacement), callData));
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

static constexpr ASCIILiteral patternOfAnySurroundingSpaces = "^\\s*|\\s*$"_s;
static constexpr ASCIILiteral patternOfSurroundingSpaces = "^\\s+|\\s+$"_s;

static bool matchesSurroundingSpaces(RegExp* regExp)
{
    const String& pattern = regExp->pattern();
    return pattern.length() == patternOfSurroundingSpaces.length() && pattern.is8Bit() && regExp->flags() == OptionSet<Yarr::Flags> { Yarr::Flags::Global }
        && (equalSpans(pattern.span8(), patternOfSurroundingSpaces.span8()) || equalSpans(pattern.span8(), patternOfAnySurroundingSpaces.span8()));
}

static ALWAYS_INLINE Range<unsigned> rangeInsideSurroundingSpaces(JSGlobalObject* globalObject, VM& vm, JSString* string, StringView source, RegExp* regExp)
{
    const unsigned length = source.length();
    unsigned start = 0;
    unsigned end = length;
    while (start < length && isStrWhiteSpace(source[start]))
        ++start;
    while (end > start && isStrWhiteSpace(source[end - 1]))
        --end;
    MatchResult last = equalSpans(regExp->pattern().span8(), patternOfAnySurroundingSpaces.span8()) ? MatchResult(length, length) : end < length ? MatchResult(end, length) : MatchResult(0, start);
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, last, false);
    return { start, end };
}

static JSString* removeSurroundingSpaces(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExp* regExp)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Range<unsigned> inside = rangeInsideSurroundingSpaces(globalObject, vm, string, source, regExp);
    if (!inside.begin() && inside.end() == source->length())
        return string;
    RELEASE_AND_RETURN(scope, jsSubstringOfResolved(vm, string, inside.begin(), inside.distance()));
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
    if (startOfFirstMatchPlusOne && replacementText->isEmpty() && matchesSurroundingSpaces(pattern))
        OPERATION_RETURN(scope, removeSurroundingSpaces(globalObject, vm, subject, pattern));
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

static bool cannotOverlapItself(std::span<const Latin1Character> text)
{
    for (size_t length = 1; length < text.size(); ++length) {
        if (equalSpans(text.first(length), text.last(length)))
            return false;
    }
    return true;
}

static size_t countOccurrences(std::span<const Latin1Character> source, std::span<const Latin1Character> text)
{
    if (source.size() < text.size())
        return 0;
    if (text.size() == 1)
        return countCharacter(source, text[0]);
    constexpr size_t stride = SIMD::stride<uint8_t>;
    constexpr size_t maxStridesSummed = 255;
    const size_t numberOfStarts = source.size() - text.size() + 1;
    const uint8_t* firsts = byteCast<uint8_t>(source.data());
    const uint8_t* lasts = firsts + text.size() - 1;
    const auto first = SIMD::splat<uint8_t>(text.front());
    const auto last = SIMD::splat<uint8_t>(text.back());
    const auto inside = text.subspan(1, text.size() - 2);
    const auto isAt = [&](size_t start) {
        return equalSpans(source.subspan(start + 1, inside.size()), inside);
    };
    size_t count = 0;
    size_t start = 0;
    while (start + stride <= numberOfStarts) {
        auto sums = SIMD::splat<uint8_t>(0);
        for (size_t strides = 0; strides < maxStridesSummed && start + stride <= numberOfStarts; ++strides, start += stride) {
            auto matches = SIMD::bitAnd(SIMD::equal(SIMD::load(firsts + start), first), SIMD::equal(SIMD::load(lasts + start), last));
            if (inside.empty()) {
                sums = SIMD::sub(sums, matches);
                continue;
            }
            if (!SIMD::isNonZero(matches)) [[likely]]
                continue;
            for (size_t index = 0; index < stride; ++index) {
                size_t candidate = start + index;
                count += source[candidate] == text.front() && source[candidate + text.size() - 1] == text.back() && isAt(candidate);
            }
        }
        count += SIMD::sum(sums);
    }
    for (; start < numberOfStarts; ++start)
        count += source[start] == text.front() && source[start + text.size() - 1] == text.back() && isAt(start);
    return count;
}

static size_t lengthOfEveryOccurrence(VM& vm, StringView source, StringView text, size_t start, size_t& startOfLast)
{
    startOfLast = start;
    if (source.is8Bit() && text.is8Bit() && cannotOverlapItself(text.span8())) {
        startOfLast = source.reverseFind(text);
        return countOccurrences(source.span8().subspan(start), text.span8()) * text.length();
    }
    size_t length = 0;
    for (; start != notFound; start = source.find(vm.adaptiveStringSearcherTables(), text, start + text.length())) {
        startOfLast = start;
        length += text.length();
    }
    return length;
}

static size_t countCharacterInSubstring(RegExpLiterals::SubstringCount& last, JSString* base, unsigned offset, RegExp* regExp, std::span<const Latin1Character> characters, size_t& startOfLast)
{
    const Latin1Character character = regExp->atom()[0];
    const auto findLast = [&](size_t start) {
        size_t found = characters.size();
        while (found > start && characters[found - 1] != character)
            --found;
        return found - 1;
    };
    const size_t length = characters.size();
    size_t count = 0;
    const bool isOfSameString = last.base == base && last.regExp == regExp && last.offset == offset;
    if (isOfSameString && length >= last.length) {
        const size_t added = countCharacter(characters.subspan(last.length), character);
        count = last.count + added;
        startOfLast = added ? findLast(last.length) : last.startOfLast;
    } else if (isOfSameString && last.length - length < length) {
        auto whole = base->tryGetValueImpl()->span8();
        const size_t removed = countCharacter(whole.subspan(offset + length, last.length - length), character);
        count = last.count - removed;
        startOfLast = removed && count ? findLast(0) : last.startOfLast;
    } else {
        count = countCharacter(characters, character);
        startOfLast = count ? findLast(0) : 0;
    }
    last = { base, regExp, offset, static_cast<uint32_t>(length), static_cast<uint32_t>(count), static_cast<uint32_t>(startOfLast) };
    ensureStillAliveHere(base);
    return count;
}

static JSValue countMatches(JSGlobalObject* globalObject, VM& vm, RegExpObject* object, JSString* subject)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RegExp* regExp = object->regExp();
    const bool hasAtom = regExp->hasValidAtom() && !regExp->atom().isEmpty();
    JSString* substringBase = nullptr;
    unsigned substringOffset = 0;
    if (hasAtom && subject->isSubstring()) {
        if (regExp->atom().length() != 1 || !regExp->atom().is8Bit())
            RELEASE_AND_RETURN(scope, regExpMatchFast(globalObject, object, subject));
        auto* substring = static_cast<JSRopeString*>(subject);
        substringBase = substring->substringBase();
        substringOffset = substring->substringOffset();
    }
    auto source = subject->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    object->setLastIndex(globalObject, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (hasAtom) {
        const String& atom = regExp->atom();
        size_t startOfLast = 0;
        size_t count = 0;
        if (substringBase && source->is8Bit())
            count = countCharacterInSubstring(vm.aotState().regExpLiterals->lastSubstringCount(), substringBase, substringOffset, regExp, source->span8(), startOfLast);
        else if (size_t startOfFirst = source->find(vm.adaptiveStringSearcherTables(), atom, 0); startOfFirst != notFound)
            count = lengthOfEveryOccurrence(vm, source, atom, startOfFirst, startOfLast) / atom.length();
        if (!count)
            return jsNull();
        globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, subject, MatchResult(startOfLast, startOfLast + atom.length()), false);
        RELEASE_AND_RETURN(scope, createArrayOfLength(globalObject, vm, count));
    }
    Vector<unsigned, 128> matches;
    if (regExp->collectMatchesOfOneSetOrRun(vm, source, matches)) {
        if (!matches.isEmpty())
            recordLastMatch(globalObject, vm, regExp, subject, matches);
        RELEASE_AND_RETURN(scope, createArrayOfLength(globalObject, vm, matches.size() / 2));
    }
    int* offsets;
    MatchResult first = globalObject->regExpGlobalData().performMatch(globalObject, regExp, subject, source, 0, &offsets);
    RETURN_IF_EXCEPTION(scope, { });
    if (!first)
        return jsNull();
    RELEASE_AND_RETURN(scope, countMatchesFrom(globalObject, vm, object, subject, first));
}

static size_t lengthWithoutMatches(JSGlobalObject* globalObject, VM& vm, JSString* string, RegExp* regExp, size_t startOfFirstMatch, const int* offsets)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto source = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    if (matchesSurroundingSpaces(regExp))
        return rangeInsideSurroundingSpaces(globalObject, vm, string, source, regExp).distance();
    if (regExp->hasValidAtom()) {
        const String& atom = regExp->atom();
        size_t startOfLast = startOfFirstMatch;
        size_t removed = regExp->global() ? lengthOfEveryOccurrence(vm, source, atom, startOfFirstMatch, startOfLast) : atom.length();
        globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, MatchResult(startOfLast, startOfLast + atom.length()), false);
        return source->length() - removed;
    }
    MatchResult match(offsets[0], offsets[1]);
    RELEASE_ASSERT(match.start <= match.end && match.end <= source->length());
    globalObject->regExpGlobalData().recordMatch(vm, globalObject, regExp, string, match, false);
    size_t removed = match.end - match.start;
    if (regExp->global()) {
        RunOfMatches run(globalObject, vm, regExp, string, source);
        while (true) {
            run.findNext(match);
            RETURN_IF_EXCEPTION(scope, 0);
            if (!match)
                break;
            removed += match.end - match.start;
        }
    }
    return source->length() - removed;
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringLengthWithoutMatches, size_t, (Instance* instance, JSCell* string, JSCell* regExp, uint32_t startOfFirstMatchPlusOne))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringLengthWithoutMatches);
    auto* subject = cellArgument<JSString>(string, Entry::operationAOTStringLengthWithoutMatches);
    auto* regExpObject = cellArgument<RegExpObject>(regExp, Entry::operationAOTStringLengthWithoutMatches);
    if (startOfFirstMatchPlusOne && regExpObject->isSymbolReplaceFastAndNonObservable(globalObject)) [[likely]]
        OPERATION_RETURN(scope, lengthWithoutMatches(globalObject, vm, subject, regExpObject->regExp(), startOfFirstMatchPlusOne - 1, vm.aotState().regExpMatchOutput.get()));
    JSString* replaced = replaceMatches(globalObject, vm, subject, regExpObject, jsEmptyString(vm));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, replaced->length());
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
        if (regExpObject->isSharedLiteral() && regExpObject->regExp()->global())
            OPERATION_RETURN(scope, JSValue::encode(countMatches(globalObject, vm, regExpObject, subject)));
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

    if (std::optional<RegExpRecordID> record = findRegExpRecordID(vm, pattern, pattern.hash(), flags))
        return &RegExpRecords(*vm.aotState().image).at(*record);
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
    registry.addClearedEachCollection("patterns of ropes"_s, asMutableByteSpan(std::span { m_ropePatterns }));
    registry.addClearedEachCollection("counts in substrings"_s, asMutableByteSpan(std::span { m_lastSubstringCounts }));
}

RegExpLiterals::RopePattern& RegExpLiterals::ropePattern(const RopeWords& words)
{
    uint64_t mixed = (words[0] ^ std::rotl(words[1], 21) ^ std::rotl(words[2], 42)) * 0x9e3779b97f4a7c15ull;
    return m_ropePatterns[mixed >> (64 - ropePatternsShift)];
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

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTFormatPatterns.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedThreads.h"
#include "DateConstructor.h"
#include "DateInstance.h"
#include "IntlDateTimeFormat.h"
#include "IntlNumberFormat.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <unicode/ucal.h>
#include <unicode/udat.h>
#include <unicode/icudataver.h>
#include <unicode/ulocdata.h>
#include <unicode/uversion.h>
#include <wtf/Threading.h>
#include <wtf/TimeZone.h>
#include <wtf/text/MakeString.h>
#include <wtf/unicode/CharacterNames.h>

#if OS(DARWIN)
#include <sys/sysctl.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

void IntlDateTimeFormat::setCompiledPattern(const AOT::FormatPatternHeader* pattern, AOT::FormatPatternUse use)
{
    replaceCompiledPattern(pattern, use);
    m_timeZoneIDAtCreation = static_cast<uint32_t>(WTF::lastTimeZoneID());
}

void IntlDateTimeFormat::initializeIfDeferred(JSGlobalObject* globalObject)
{
    if (!m_impl) [[unlikely]]
        AOT::initializeFromCompiledPattern(globalObject, *this);
}

void IntlNumberFormat::initializeIfDeferred(JSGlobalObject* globalObject)
{
    if (!m_numberFormatter) [[unlikely]]
        AOT::initializeFromCompiledPattern(globalObject, *this);
}

namespace AOT {

static const std::optional<ICUStamp>& stampOfThisICU()
{
    static const std::optional<ICUStamp> stampOfThisProcess = []() -> std::optional<ICUStamp> {
        ICUStamp stamp { };
        UErrorCode status = U_ZERO_ERROR;
        u_getVersion(stamp.codeVersion);
        u_getDataVersion(stamp.dataVersion, &status);
        ulocdata_getCLDRVersion(stamp.cldrVersion, &status);
        const char* timeZoneDataVersion = ucal_getTZDataVersion(&status);
        if (U_FAILURE(status) || strlen(timeZoneDataVersion) >= sizeof(stamp.timeZoneDataVersion))
            return std::nullopt;
        memcpy(stamp.timeZoneDataVersion, timeZoneDataVersion, strlen(timeZoneDataVersion));
#if OS(DARWIN)
        size_t size = sizeof(stamp.systemBuild);
        if (sysctlbyname("kern.osversion", stamp.systemBuild, &size, nullptr, 0))
            return std::nullopt;
#endif
        return stamp;
    }();
    return stampOfThisProcess;
}

static ALWAYS_INLINE void countIfAsked(VM& vm, const char* what)
{
    if (Options::useAOTOperationCounters() && vm.m_aotRuntimeTable) [[unlikely]]
        vm.m_aotRuntimeTable->countOperation(what);
}

static NEVER_INLINE void findOriginOfFormatPatterns(Instance* instance)
{
    countIfAsked(*instance->vm, "the ICU stamp of the format patterns is compared");
    auto* data = reinterpret_cast<const Typed::Data*>(instance->typedData);
    auto& stamp = stampOfThisICU();
    bool isMadeByThisICU = stamp && !Options::formatPatternsOfAnotherICUForTesting() && !memcmp(data->at<ICUStamp>(data->icuStampOfFormatPatternsOffset), &*stamp, sizeof(ICUStamp));
    instance->formatPatternsOrigin = isMadeByThisICU ? FormatPatternsOrigin::ThisICU : FormatPatternsOrigin::AnotherICU;
}

static ALWAYS_INLINE bool areFormatPatternsMadeByThisICU(Instance* instance)
{
    if (instance->formatPatternsOrigin == FormatPatternsOrigin::NotCompared) [[unlikely]]
        findOriginOfFormatPatterns(instance);
    return instance->formatPatternsOrigin == FormatPatternsOrigin::ThisICU;
}

static ALWAYS_INLINE const FormatPatternHeader& formatPatternAt(Instance* instance, uint32_t index)
{
    auto* data = reinterpret_cast<const Typed::Data*>(instance->typedData);
    return *data->at<FormatPatternHeader>(data->at<uint32_t>(data->formatPatternsOffset)[index]);
}


static String formatNumberWithPattern(const FormatPatternHeader&, double number);

template<typename CharacterType, typename Write>
static ALWAYS_INLINE String formatWithPattern(const FormatPatternHeader& header, const Write& write)
{
    CharacterType buffer[maxFormatPatternLength];
    CharacterType* end = write(buffer, reinterpret_cast<const CharacterType*>(&header + 1));
    if (!end) [[unlikely]]
        return { };
    return String(std::span<const CharacterType> { buffer, end });
}

static String formatDateTimeWithPattern(const FormatPatternHeader& header, const std::optional<UTCDateTime>& fields)
{
    if (!fields) [[unlikely]]
        return { };
    auto write = [&](auto* cursor, auto* code) {
        return writeDateTime(cursor, code, *fields);
    };
    return header.is16Bit ? formatWithPattern<char16_t>(header, write) : formatWithPattern<Latin1Character>(header, write);
}

static std::optional<UTCDateTime> fieldsInDefaultTimeZone(VM& vm, double timeValue)
{
    if (!(std::abs(timeValue) <= maxTimeValue))
        return std::nullopt;
    timeValue = std::trunc(timeValue);
    auto local = vm.dateCache.msToGregorianDateTime(timeValue, WTF::TimeType::LocalTime);
    if (!local || local.year() < 1)
        return std::nullopt;
    double millisecond = std::fmod(timeValue, 1000);
    if (millisecond < 0)
        millisecond += 1000;
    return UTCDateTime { local.year(), static_cast<uint8_t>(local.month() + 1), static_cast<uint8_t>(local.monthDay()), static_cast<uint8_t>(local.weekDay()), static_cast<uint8_t>(local.hour()), static_cast<uint8_t>(local.minute()), static_cast<uint8_t>(local.second()), static_cast<uint16_t>(millisecond) };
}

static String timeZoneNamedBy(const FormatPatternHeader&);

using UTCOffsets = IntlDateTimeFormat::UTCOffsets;

struct FormatPatternState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FormatPatternState);
    Vector<std::unique_ptr<UTCOffsets>> utcOffsetsByPattern;
    UncheckedKeyHashMap<const FormatPatternHeader*, Vector<uint8_t>> plannedAgain;
};

void destroyFormatPatternState(Instance& instance)
{
    delete instance.formatPatternState;
}

static FormatPatternState& formatPatternStateOf(Instance* instance)
{
    if (!instance->formatPatternState)
        instance->formatPatternState = new FormatPatternState;
    return *instance->formatPatternState;
}

namespace Typed {
static std::optional<Vector<uint8_t>> planFormatPatternAgain(JSGlobalObject*, const FormatPatternHeader&);
}

static NEVER_INLINE const FormatPatternHeader* formatPatternPlannedAgain(Instance* instance, JSGlobalObject* globalObject, const FormatPatternHeader& patternOfImage)
{
    auto& plannedAgain = formatPatternStateOf(instance).plannedAgain;
    auto found = plannedAgain.find(&patternOfImage);
    if (found == plannedAgain.end()) {
        std::optional<Vector<uint8_t>> pattern;
        if (Options::formatPatternsOfAnotherICUForTesting() != 2)
            pattern = Typed::planFormatPatternAgain(globalObject, patternOfImage);
        countIfAsked(globalObject->vm(), pattern ? "a format pattern is planned again" : "a format pattern is refused when planned again");
        found = plannedAgain.add(&patternOfImage, pattern ? WTF::move(*pattern) : Vector<uint8_t>()).iterator;
    }
    return found->value.isEmpty() ? nullptr : reinterpret_cast<const FormatPatternHeader*>(found->value.span().data());
}

template<typename Formatter>
static NEVER_INLINE const FormatPatternHeader* planCompiledPatternAgain(JSGlobalObject* globalObject, Formatter& formatter)
{
    if (formatter.compiledPatternUse() == FormatPatternUse::OnlyInitializes)
        return nullptr;
    Instance* instance = globalObject->aotInstance();
    const FormatPatternHeader* pattern = nullptr;
    if (instance)
        pattern = areFormatPatternsMadeByThisICU(instance) ? formatter.compiledPattern() : formatPatternPlannedAgain(instance, globalObject, *formatter.compiledPattern());
    if (pattern)
        formatter.replaceCompiledPattern(pattern, FormatPatternUse::Formats);
    else
        formatter.replaceCompiledPattern(formatter.compiledPattern(), FormatPatternUse::OnlyInitializes);
    return pattern;
}

static NEVER_INLINE UTCOffsets* findUTCOffset(std::unique_ptr<UTCOffsets>& offsets, const FormatPatternHeader& header, double timeValue)
{
    if (!(std::abs(timeValue) <= maxTimeValue))
        return nullptr;
    UErrorCode status = U_ZERO_ERROR;
    if (!offsets) {
        String timeZone = timeZoneNamedBy(header);
        auto characters = StringView(timeZone).upconvertedCharacters();
        auto calendar = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(characters.get(), timeZone.length(), "", UCAL_GREGORIAN, &status));
        if (U_FAILURE(status))
            return nullptr;
        offsets = makeUnique<UTCOffsets>();
        offsets->calendar = WTF::move(calendar);
    }
    UCalendar* calendar = offsets->calendar.get();
    ucal_setMillis(calendar, timeValue, &status);
    double offset = ucal_get(calendar, UCAL_ZONE_OFFSET, &status);
    offset += ucal_get(calendar, UCAL_DST_OFFSET, &status);
    UDate previous = 0;
    UDate next = 0;
    bool hasPrevious = ucal_getTimeZoneTransitionDate(calendar, UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE, &previous, &status);
    bool hasNext = ucal_getTimeZoneTransitionDate(calendar, UCAL_TZ_TRANSITION_NEXT, &next, &status);
    if (U_FAILURE(status))
        return nullptr;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    offsets->start = hasPrevious ? previous : -infinity;
    offsets->end = hasNext ? next : infinity;
    offsets->offset = offset;
    return offsets.get();
}

static ALWAYS_INLINE String formatInNamedTimeZone(std::unique_ptr<UTCOffsets>& slot, const FormatPatternHeader& header, double timeValue)
{
    auto* offsets = slot.get();
    if (!offsets || !(timeValue >= offsets->start && timeValue < offsets->end)) [[unlikely]] {
        offsets = findUTCOffset(slot, header, timeValue);
        if (!offsets)
            return { };
    }
    return formatDateTimeWithPattern(header, fieldsAtUTCOffset(timeValue, offsets->offset));
}

static constexpr const char* textWrittenFromPattern = "a text is written from a format pattern";

template<typename Formatter>
static NEVER_INLINE void validateAgainstICU(JSGlobalObject* globalObject, Formatter& formatter, double value, const String& text)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    formatter.initializeIfDeferred(globalObject);
    RELEASE_ASSERT(!scope.exception());
    JSValue expected = formatter.format(globalObject, value);
    RELEASE_ASSERT(!scope.exception() && expected.isString());
    String expectedText = asString(expected)->value(globalObject);
    if (expectedText == text)
        return;
    dataLogLn("AOT: the pattern of a formatter made from constants writes \"", text, "\" for ", value, "; ICU writes \"", expectedText, "\"");
    CRASH();
}

static ALWAYS_INLINE String tryFormatTimeValueWithoutValidation(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter, double timeValue)
{
    auto* header = formatter.compiledPattern();
    if (!header) [[unlikely]]
        return { };
    if (formatter.compiledPatternUse() != FormatPatternUse::Formats) [[unlikely]] {
        header = planCompiledPatternAgain(globalObject, formatter);
        if (!header)
            return { };
    }
    switch (header->timeZone) {
    case FormatPatternTimeZone::FixedOffset:
        return formatDateTimeWithPattern(*header, fieldsForDateTimePattern(*header, timeValue));
    case FormatPatternTimeZone::Default:
        if (formatter.timeZoneIDAtCreation() != static_cast<uint32_t>(WTF::lastTimeZoneID())) [[unlikely]]
            return { };
        return formatDateTimeWithPattern(*header, fieldsInDefaultTimeZone(globalObject->vm(), timeValue));
    case FormatPatternTimeZone::Named:
        return formatInNamedTimeZone(formatter.utcOffsets(), *header, timeValue);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE String tryFormatTimeValue(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter, double timeValue)
{
    String text = tryFormatTimeValueWithoutValidation(globalObject, formatter, timeValue);
    if (text.isNull()) [[unlikely]]
        return text;
    countIfAsked(globalObject->vm(), textWrittenFromPattern);
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        validateAgainstICU(globalObject, formatter, std::trunc(timeValue) + 0.0, text);
    return text;
}

String tryFormatWithCompiledPattern(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter, JSValue value)
{
    if (value.isNumber())
        return tryFormatTimeValue(globalObject, formatter, value.asNumber());
    if (value.isUndefined())
        return tryFormatTimeValue(globalObject, formatter, dateNowImpl(globalObject).asNumber());
    auto* date = dynamicDowncast<DateInstance>(value);
    if (!date || date->structure() != globalObject->dateStructure() || !globalObject->datePrototype()->structure()->hasImmutableProperties())
        return { };
    return tryFormatTimeValue(globalObject, formatter, date->internalNumber());
}

String tryFormatWithCompiledPattern(JSGlobalObject* globalObject, IntlNumberFormat& formatter, double number)
{
    auto* header = formatter.compiledPattern();
    if (!header) [[unlikely]]
        return { };
    if (formatter.compiledPatternUse() != FormatPatternUse::Formats) [[unlikely]] {
        header = planCompiledPatternAgain(globalObject, formatter);
        if (!header)
            return { };
    }
    String text = formatNumberWithPattern(*header, number);
    if (text.isNull()) [[unlikely]]
        return text;
    countIfAsked(globalObject->vm(), textWrittenFromPattern);
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        validateAgainstICU(globalObject, formatter, number, text);
    return text;
}

namespace {

enum class ConstantKind : uint8_t { String, Boolean, Number };

class ConstructorArgumentsReader {
public:
    explicit ConstructorArgumentsReader(const FormatPatternHeader& header)
        : m_cursor(reinterpret_cast<const uint8_t*>(&header) + header.constructorArgumentsOffset)
    {
    }

    uint8_t readByte() { return *m_cursor++; }
    String readText()
    {
        size_t length = readByte();
        length |= static_cast<size_t>(readByte()) << 8;
        String text = String::fromUTF8(std::span { m_cursor, length });
        m_cursor += length;
        return text;
    }
    double readNumber()
    {
        double number;
        memcpy(&number, m_cursor, sizeof(number));
        m_cursor += sizeof(number);
        return number;
    }

private:
    const uint8_t* m_cursor;
};

} // namespace

static String timeZoneNamedBy(const FormatPatternHeader& header)
{
    ConstructorArgumentsReader reader(header);
    reader.readText();
    if (!reader.readByte())
        return { };
    for (unsigned count = reader.readByte(); count--;) {
        auto kind = static_cast<ConstantKind>(reader.readByte());
        String name = reader.readText();
        switch (kind) {
        case ConstantKind::String:
            if (String text = reader.readText(); name == "timeZone"_s)
                return text;
            break;
        case ConstantKind::Boolean:
            reader.readByte();
            break;
        case ConstantKind::Number:
            reader.readNumber();
            break;
        }
    }
    return { };
}

static std::pair<JSValue, JSValue> constructorArgumentsOf(JSGlobalObject* globalObject, const FormatPatternHeader& header)
{
    VM& vm = globalObject->vm();
    ConstructorArgumentsReader reader(header);
    JSValue locale = jsString(vm, reader.readText());
    if (!reader.readByte())
        return { locale, jsUndefined() };
    JSObject* options = constructEmptyObject(globalObject);
    for (unsigned count = reader.readByte(); count--;) {
        auto kind = static_cast<ConstantKind>(reader.readByte());
        Identifier name = Identifier::fromString(vm, reader.readText());
        JSValue value;
        switch (kind) {
        case ConstantKind::String:
            value = jsString(vm, reader.readText());
            break;
        case ConstantKind::Boolean:
            value = jsBoolean(reader.readByte());
            break;
        case ConstantKind::Number:
            value = jsNumber(reader.readNumber());
            break;
        }
        options->putDirect(vm, name, value);
    }
    return { locale, options };
}

void initializeFromCompiledPattern(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter)
{
    auto [locale, options] = constructorArgumentsOf(globalObject, *formatter.compiledPattern());
    uint8_t requiredComponentAndDefaults = formatter.compiledPattern()->requiredComponentAndDefaults;
    formatter.initializeDateTimeFormat(globalObject, locale, options, static_cast<IntlDateTimeFormat::RequiredComponent>(requiredComponentAndDefaults >> 4), static_cast<IntlDateTimeFormat::Defaults>(requiredComponentAndDefaults & 0xf));
}

void initializeFromCompiledPattern(JSGlobalObject* globalObject, IntlNumberFormat& formatter)
{
    auto [locale, options] = constructorArgumentsOf(globalObject, *formatter.compiledPattern());
    formatter.initializeNumberFormat(globalObject, locale, options);
}

static String formatNumberWithPattern(const FormatPatternHeader& header, double number)
{
    auto write = [&](auto* cursor, auto* code) {
        return writeNumber(cursor, code, number);
    };
    return header.is16Bit ? formatWithPattern<char16_t>(header, write) : formatWithPattern<Latin1Character>(header, write);
}

static ALWAYS_INLINE bool hasStructure(EncodedJSValue encodedValue, Structure* structure)
{
    JSValue value = JSValue::decode(encodedValue);
    return value.isCell() && value.asCell()->structureID() == structure->id();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewConstantFormatter, JSCell*, (Instance* instance, uint32_t pattern))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto& header = formatPatternAt(instance, pattern);
    bool isInNamedTimeZone = header.kind == FormatPatternKind::DateTime && header.timeZone == FormatPatternTimeZone::Named;
    bool isMadeByThisICU = isInNamedTimeZone ? areFormatPatternsMadeByThisICU(instance) : instance->formatPatternsOrigin == FormatPatternsOrigin::ThisICU;
    auto use = isMadeByThisICU ? FormatPatternUse::Formats : FormatPatternUse::MustBePlannedAgain;
    if (header.kind == FormatPatternKind::Number) {
        if (!ensureNumberFormattingIsAvailable(globalObject)) [[unlikely]]
            OPERATION_RETURN(scope, nullptr);
        auto* formatter = IntlNumberFormat::create(vm, globalObject->numberFormatStructure());
        formatter->setCompiledPattern(&header, use);
        OPERATION_RETURN(scope, formatter);
    }
    if (!ensureDateFormattingIsAvailable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    auto* formatter = IntlDateTimeFormat::create(vm, globalObject->dateTimeFormatStructure());
    formatter->setCompiledPattern(&header, use);
    if (header.timeZone == FormatPatternTimeZone::Default || (isInNamedTimeZone && !isMadeByThisICU))
        initializeFromCompiledPattern(globalObject, *formatter);
    OPERATION_RETURN(scope, formatter);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFormatDateTime, JSCell*, (Instance* instance, EncodedJSValue formatter, double timeValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (!hasStructure(formatter, globalObject->dateTimeFormatStructure())) [[unlikely]]
        return nullptr;
    String text = tryFormatTimeValue(globalObject, *uncheckedDowncast<IntlDateTimeFormat>(JSValue::decode(formatter).asCell()), timeValue);
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFormatNumber, JSCell*, (Instance* instance, EncodedJSValue formatter, double number))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (!hasStructure(formatter, globalObject->numberFormatStructure())) [[unlikely]]
        return nullptr;
    String text = tryFormatWithCompiledPattern(globalObject, *uncheckedDowncast<IntlNumberFormat>(JSValue::decode(formatter).asCell()), number);
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFormatTimeValueWithPattern, JSCell*, (Instance* instance, uint32_t pattern, double timeValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* patternForThisICU = &formatPatternAt(instance, pattern);
    if (!areFormatPatternsMadeByThisICU(instance)) [[unlikely]] {
        patternForThisICU = formatPatternPlannedAgain(instance, globalObject, *patternForThisICU);
        if (!patternForThisICU)
            return nullptr;
    }
    auto& header = *patternForThisICU;
    String text;
    switch (header.timeZone) {
    case FormatPatternTimeZone::FixedOffset:
        text = formatDateTimeWithPattern(header, fieldsForDateTimePattern(header, timeValue));
        break;
    case FormatPatternTimeZone::Default:
        text = formatDateTimeWithPattern(header, fieldsInDefaultTimeZone(vm, timeValue));
        break;
    case FormatPatternTimeZone::Named: {
        auto& byPattern = formatPatternStateOf(instance).utcOffsetsByPattern;
        if (byPattern.size() <= pattern)
            byPattern.grow(pattern + 1);
        text = formatInNamedTimeZone(byPattern[pattern], header, timeValue);
        break;
    }
    }
    if (text.isNull()) [[unlikely]]
        return nullptr;
    countIfAsked(vm, textWrittenFromPattern);
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        auto* formatter = IntlDateTimeFormat::create(vm, globalObject->dateTimeFormatStructure());
        formatter->setCompiledPattern(&header);
        validateAgainstICU(globalObject, *formatter, std::trunc(timeValue) + 0.0, text);
    }
    return jsString(vm, WTF::move(text));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFormatNumberWithPattern, JSCell*, (Instance* instance, uint32_t pattern, double number))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* patternForThisICU = &formatPatternAt(instance, pattern);
    if (!areFormatPatternsMadeByThisICU(instance)) [[unlikely]] {
        patternForThisICU = formatPatternPlannedAgain(instance, globalObject, *patternForThisICU);
        if (!patternForThisICU)
            return nullptr;
    }
    auto& header = *patternForThisICU;
    String text = formatNumberWithPattern(header, number);
    if (text.isNull()) [[unlikely]]
        return nullptr;
    countIfAsked(vm, textWrittenFromPattern);
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        auto* formatter = IntlNumberFormat::create(vm, globalObject->numberFormatStructure());
        formatter->setCompiledPattern(&header);
        validateAgainstICU(globalObject, *formatter, number, text);
    }
    return jsString(vm, WTF::move(text));
}

namespace Typed {

using namespace B3;

static bool isLocaleStringMethod(InlineBuiltin which)
{
    return which == InlineBuiltin::DateToLocaleString || which == InlineBuiltin::NumberToLocaleString;
}

namespace {

struct ConstantOption {
    using Kind = ConstantKind;
    String name;
    Kind kind;
    String string;
    double number { 0 };
};

struct FormatterRequest {
    FormatPatternKind kind;
    String locale;
    bool hasOptions { false };
    Vector<ConstantOption, 8> options;
    IntlDateTimeFormat::RequiredComponent required { IntlDateTimeFormat::RequiredComponent::Any };
    IntlDateTimeFormat::Defaults defaults { IntlDateTimeFormat::Defaults::Date };
    std::optional<uint32_t> pattern;

    String key() const
    {
        StringBuilder builder;
        builder.append(static_cast<unsigned>(kind), '\0', static_cast<unsigned>(required), '\0', static_cast<unsigned>(defaults), '\0', locale, '\0', hasOptions ? '1' : '0');
        for (auto& option : options)
            builder.append('\0', option.name, '\0', static_cast<unsigned>(option.kind), '\0', option.string, '\0', option.number);
        return builder.toString();
    }

    static FormatterRequest fromConstructorArgumentsOf(const FormatPatternHeader& header)
    {
        FormatterRequest request { header.kind, { }, false, { }, static_cast<IntlDateTimeFormat::RequiredComponent>(header.requiredComponentAndDefaults >> 4), static_cast<IntlDateTimeFormat::Defaults>(header.requiredComponentAndDefaults & 0xf), std::nullopt };
        ConstructorArgumentsReader reader(header);
        request.locale = reader.readText();
        request.hasOptions = reader.readByte();
        if (!request.hasOptions)
            return request;
        for (unsigned count = reader.readByte(); count--;) {
            ConstantOption option { { }, static_cast<ConstantKind>(reader.readByte()), { }, 0 };
            option.name = reader.readText();
            switch (option.kind) {
            case ConstantKind::String:
                option.string = reader.readText();
                break;
            case ConstantKind::Boolean:
                option.number = reader.readByte();
                break;
            case ConstantKind::Number:
                option.number = reader.readNumber();
                break;
            }
            request.options.append(WTF::move(option));
        }
        return request;
    }

    String description() const
    {
        StringBuilder builder;
        builder.append(kind == FormatPatternKind::DateTime ? "Intl.DateTimeFormat(\""_s : "Intl.NumberFormat(\""_s, locale, '"');
        for (auto& option : options) {
            builder.append(", "_s, option.name, ": "_s);
            if (option.kind == ConstantKind::String)
                builder.append(option.string);
            else
                builder.append(option.number);
        }
        builder.append(')');
        return builder.toString();
    }

    std::optional<Vector<uint8_t>> serializedConstructorArguments() const
    {
        Vector<uint8_t> bytes;
        bool isValid = options.size() <= 0xff;
        auto appendText = [&](const String& text) {
            auto utf8 = text.utf8();
            isValid &= utf8.length() <= 0xffff;
            bytes.append(static_cast<uint8_t>(utf8.length()));
            bytes.append(static_cast<uint8_t>(utf8.length() >> 8));
            bytes.append(utf8.span());
        };
        appendText(locale);
        bytes.append(hasOptions);
        if (hasOptions)
            bytes.append(static_cast<uint8_t>(options.size()));
        for (auto& option : options) {
            bytes.append(static_cast<uint8_t>(option.kind));
            appendText(option.name);
            switch (option.kind) {
            case ConstantKind::String:
                appendText(option.string);
                break;
            case ConstantKind::Boolean:
                bytes.append(!!option.number);
                break;
            case ConstantKind::Number:
                bytes.append(asByteSpan(option.number));
                break;
            }
        }
        if (!isValid)
            return std::nullopt;
        return bytes;
    }
};

class PatternCode {
public:
    void append(unsigned unit)
    {
        m_isValid &= unit <= 0xff;
        m_units.append(static_cast<char16_t>(unit));
    }
    void append(DateTimePatternOp op) { append(static_cast<unsigned>(op)); }
    void appendText(StringView text)
    {
        append(text.length());
        for (unsigned i = 0; i < text.length(); ++i) {
            m_is16Bit |= text[i] > 0xff;
            m_units.append(text[i]);
        }
    }
    void appendLiteral(StringView text)
    {
        if (text.isEmpty())
            return;
        append(DateTimePatternOp::Literal);
        appendText(text);
        m_maxLength += text.length();
    }
    void appendNames(DateTimePatternOp op, std::span<const String> names)
    {
        unsigned longest = 0;
        for (auto& name : names)
            longest = std::max(longest, name.length());
        append(op);
        append(names.size());
        append(longest + 1);
        for (auto& name : names) {
            appendText(name);
            for (unsigned i = name.length(); i < longest; ++i)
                m_units.append(0);
        }
        m_maxLength += longest;
    }
    void didAppendField(unsigned maxLength) { m_maxLength += maxLength; }

    std::optional<Vector<uint8_t>> finish(const FormatterRequest& request, FormatPatternTimeZone timeZone, int32_t utcOffsetInMinutes) const
    {
        auto constructorArguments = request.serializedConstructorArguments();
        size_t constructorArgumentsOffset = sizeof(FormatPatternHeader) + m_units.size() * (m_is16Bit ? 2 : 1);
        if (!m_isValid || m_maxLength > maxFormatPatternLength || !constructorArguments || constructorArgumentsOffset > 0xffff)
            return std::nullopt;
        FormatPatternHeader header { request.kind, m_is16Bit, timeZone, static_cast<uint8_t>(static_cast<unsigned>(request.required) << 4 | static_cast<unsigned>(request.defaults)), static_cast<uint16_t>(m_maxLength), static_cast<uint16_t>(constructorArgumentsOffset), utcOffsetInMinutes };
        Vector<uint8_t> bytes;
        bytes.append(asByteSpan(header));
        if (m_is16Bit)
            bytes.append(asBytes(m_units.span()));
        else {
            for (char16_t unit : m_units)
                bytes.append(static_cast<uint8_t>(unit));
        }
        bytes.appendVector(*constructorArguments);
        return bytes;
    }

    bool operator==(const PatternCode&) const = default;

private:
    Vector<char16_t, 128> m_units;
    unsigned m_maxLength { 0 };
    bool m_is16Bit { false };
    bool m_isValid { true };
};

static const FormatPatternHeader& headerOf(const Vector<uint8_t>& pattern)
{
    return *reinterpret_cast<const FormatPatternHeader*>(pattern.span().data());
}

class PseudoRandomNumbers {
public:
    double next()
    {
        m_state = m_state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(m_state >> 11) / static_cast<double>(1ull << 53);
    }

private:
    uint64_t m_state { 0x9e3779b97f4a7c15ull };
};

enum class CrossCheck : bool { Thorough, Brief };

class PatternCompiler {
public:
    PatternCompiler(JSGlobalObject* globalObject, CrossCheck crossCheck)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_isBrief(crossCheck == CrossCheck::Brief)
    {
    }

    std::optional<Vector<uint8_t>> compile(const FormatterRequest& request)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        SetForScope scopeOfThisRequest(m_scope, &scope);
        m_whyRefused = { };
        auto result = request.kind == FormatPatternKind::DateTime ? compileDateTime(request) : compileNumber(request);
        if (scope.exception()) [[unlikely]] {
            scope.clearExceptionExceptTermination();
            result = refuse("the constructor throws"_s);
        }
        dataLogLnIf(!result && Options::logAOTFormatPatterns(), "AOT: no format pattern for ", request.description(), ": ", m_whyRefused.isNull() ? StringView("what ICU says does not fit a plan"_s) : StringView(m_whyRefused));
        return result;
    }

    unsigned numberOfFailedCrossChecks() const { return m_numberOfFailedCrossChecks; }

private:
    template<typename... Parts>
    std::nullopt_t refuse(Parts&&... why)
    {
        if (Options::logAOTFormatPatterns()) [[unlikely]]
            m_whyRefused = makeString(std::forward<Parts>(why)...);
        return std::nullopt;
    }

    std::nullopt_t didFailCrossCheck(const FormatterRequest& request)
    {
        ++m_numberOfFailedCrossChecks;
        refuse("the plan differs from ICU on a sample"_s);
        dataLogLnIf(Options::verboseAOTCompilation() || Options::logAOTFormatPatterns(), "AOT: the pattern of ", request.description(), " differs from ICU on a sample: ICU formats for it");
        return std::nullopt;
    }

    bool hasException() { return !!m_scope->exception(); }

    JSValue optionsOf(const FormatterRequest& request, ASCIILiteral timeZone = { })
    {
        if (!request.hasOptions && timeZone.isNull())
            return jsUndefined();
        JSObject* options = constructEmptyObject(m_globalObject);
        if (!timeZone.isNull())
            options->putDirect(m_vm, m_vm.propertyNames->timeZone, jsString(m_vm, String(timeZone)));
        for (auto& option : request.options) {
            if (!timeZone.isNull() && option.name == "timeZone"_s)
                continue;
            JSValue value;
            switch (option.kind) {
            case ConstantOption::Kind::String:
                value = jsString(m_vm, option.string);
                break;
            case ConstantOption::Kind::Boolean:
                value = jsBoolean(option.number);
                break;
            case ConstantOption::Kind::Number:
                value = jsNumber(option.number);
                break;
            }
            options->putDirect(m_vm, Identifier::fromString(m_vm, option.name), value);
        }
        return options;
    }

    bool isSupportedLocale(const String& locale)
    {
        Vector<String> requested = canonicalizeLocaleList(m_globalObject, jsString(m_vm, locale));
        return !hasException() && requested.size() == 1 && !bestAvailableLocale(intlAvailableLocales(), requested[0]).isNull();
    }

    static std::optional<int32_t> fixedUTCOffsetInMinutes(const String& timeZone)
    {
        if (timeZone == "UTC"_s)
            return 0;
        if (timeZone.length() != 6 || (timeZone[0] != '+' && timeZone[0] != '-') || timeZone[3] != ':')
            return std::nullopt;
        for (unsigned index : { 1, 2, 4, 5 }) {
            if (!isASCIIDigit(timeZone[index]))
                return std::nullopt;
        }
        int32_t minutes = ((timeZone[1] - '0') * 10 + (timeZone[2] - '0')) * 60 + (timeZone[4] - '0') * 10 + (timeZone[5] - '0');
        return timeZone[0] == '-' ? -minutes : minutes;
    }

    template<typename Formatter>
    String formatted(const Formatter& formatter, double value)
    {
        JSValue text = formatter.format(m_globalObject, value);
        if (hasException() || !text.isString())
            return { };
        return asString(text)->value(m_globalObject);
    }

    static String withNormalSpaces(std::span<const char16_t> characters)
    {
        Vector<char16_t, 32> text(characters);
        for (auto& character : text) {
            if (character == narrowNoBreakSpace || character == thinSpace)
                character = space;
        }
        return String(text.span());
    }

    static bool appendSymbols(PatternCode& code, DateTimePatternOp op, const UDateFormat* dateFormat, UDateFormatSymbolType type, int32_t first, unsigned count)
    {
        Vector<String, 12> names;
        for (unsigned i = 0; i < count; ++i) {
            Vector<char16_t, 32> name;
            if (U_FAILURE(callBufferProducingFunction(udat_getSymbols, dateFormat, type, first + static_cast<int32_t>(i), name)) || name.isEmpty())
                return false;
            names.append(withNormalSpaces(name.span()));
        }
        code.appendNames(op, names.span());
        return true;
    }

    static bool appendField(PatternCode& code, const UDateFormat* dateFormat, char16_t letter, unsigned count)
    {
        auto number = [&](DateTimePatternOp oneDigit, DateTimePatternOp twoDigits) {
            if (count > 2)
                return false;
            code.append(count == 1 ? oneDigit : twoDigits);
            code.didAppendField(2);
            return true;
        };
        auto months = [&](UDateFormatSymbolType abbreviated, UDateFormatSymbolType wide, UDateFormatSymbolType narrow) {
            if (count <= 2)
                return number(DateTimePatternOp::Month, DateTimePatternOp::TwoDigitMonth);
            return count <= 5 && appendSymbols(code, DateTimePatternOp::MonthName, dateFormat, count == 3 ? abbreviated : count == 4 ? wide : narrow, 0, 12);
        };
        auto weekdays = [&](UDateFormatSymbolType abbreviated, UDateFormatSymbolType wide, UDateFormatSymbolType narrow, UDateFormatSymbolType shorter) {
            return count <= 6 && appendSymbols(code, DateTimePatternOp::WeekdayName, dateFormat, count <= 3 ? abbreviated : count == 4 ? wide : count == 5 ? narrow : shorter, 1, 7);
        };
        switch (letter) {
        case 'y':
            if (count > 2)
                return false;
            code.append(count == 1 ? DateTimePatternOp::Year : DateTimePatternOp::TwoDigitYear);
            code.didAppendField(count == 1 ? maxDecimalLengthOfYear : 2);
            return true;
        case 'M':
            return months(UDAT_SHORT_MONTHS, UDAT_MONTHS, UDAT_NARROW_MONTHS);
        case 'L':
            return months(UDAT_STANDALONE_SHORT_MONTHS, UDAT_STANDALONE_MONTHS, UDAT_STANDALONE_NARROW_MONTHS);
        case 'd':
            return number(DateTimePatternOp::Day, DateTimePatternOp::TwoDigitDay);
        case 'E':
            return weekdays(UDAT_SHORT_WEEKDAYS, UDAT_WEEKDAYS, UDAT_NARROW_WEEKDAYS, UDAT_SHORTER_WEEKDAYS);
        case 'c':
            return count >= 3 && weekdays(UDAT_STANDALONE_SHORT_WEEKDAYS, UDAT_STANDALONE_WEEKDAYS, UDAT_STANDALONE_NARROW_WEEKDAYS, UDAT_STANDALONE_SHORTER_WEEKDAYS);
        case 'a':
            return count <= 3 && appendSymbols(code, DateTimePatternOp::DayPeriodName, dateFormat, UDAT_AM_PMS, 0, 2);
        case 'H':
            return number(DateTimePatternOp::Hour23, DateTimePatternOp::TwoDigitHour23);
        case 'k':
            return number(DateTimePatternOp::Hour24, DateTimePatternOp::TwoDigitHour24);
        case 'h':
            return number(DateTimePatternOp::Hour12, DateTimePatternOp::TwoDigitHour12);
        case 'K':
            return number(DateTimePatternOp::Hour11, DateTimePatternOp::TwoDigitHour11);
        case 'm':
            return number(DateTimePatternOp::Minute, DateTimePatternOp::TwoDigitMinute);
        case 's':
            return number(DateTimePatternOp::Second, DateTimePatternOp::TwoDigitSecond);
        case 'S':
            if (count > 3)
                return false;
            code.append(count == 1 ? DateTimePatternOp::Tenths : count == 2 ? DateTimePatternOp::Hundredths : DateTimePatternOp::Milliseconds);
            code.didAppendField(count);
            return true;
        default:
            return false;
        }
    }

    std::optional<PatternCode> codeFromPatternOf(const UDateFormat* dateFormat)
    {
        Vector<char16_t, 64> pattern;
        if (U_FAILURE(callBufferProducingFunction(udat_toPattern, dateFormat, false, pattern)))
            return refuse("ICU does not give its pattern"_s);
        PatternCode code;
        Vector<char16_t, 32> literal;
        auto flushLiteral = [&] {
            code.appendLiteral(withNormalSpaces(literal.span()));
            literal.shrink(0);
        };
        for (size_t i = 0; i < pattern.size();) {
            char16_t character = pattern[i];
            if (character == '\'') {
                if (i + 1 < pattern.size() && pattern[i + 1] == '\'') {
                    literal.append('\'');
                    i += 2;
                    continue;
                }
                for (++i;; ++i) {
                    if (i == pattern.size())
                        return refuse("a quotation in ICU's pattern does not end"_s);
                    if (pattern[i] != '\'') {
                        literal.append(pattern[i]);
                        continue;
                    }
                    if (i + 1 == pattern.size() || pattern[i + 1] != '\'')
                        break;
                    literal.append('\'');
                    ++i;
                }
                ++i;
                continue;
            }
            if (!isASCIIAlpha(character)) {
                literal.append(character);
                ++i;
                continue;
            }
            size_t end = i;
            while (end < pattern.size() && pattern[end] == character)
                ++end;
            flushLiteral();
            if (!appendField(code, dateFormat, character, end - i))
                return refuse("ICU's pattern, "_s, StringView(pattern.span()), ", has "_s, end - i, " of the letter "_s, character);
            i = end;
        }
        flushLiteral();
        code.append(DateTimePatternOp::End);
        return code;
    }

    std::optional<Vector<uint8_t>> compileDateTime(const FormatterRequest& request)
    {
        if (!isSupportedLocale(request.locale))
            return refuse("ICU has no data for the locale"_s);
        bool namesTimeZone = request.options.containsIf([](auto& option) { return option.name == "timeZone"_s; });
        if (namesTimeZone) {
            if (auto planned = planDateTime(request, { }))
                return planned->first.finish(request, FormatPatternTimeZone::FixedOffset, planned->second);
            if (hasException())
                return std::nullopt;
        }
        auto inUTC = planDateTime(request, "UTC"_s);
        if (!inUTC)
            return std::nullopt;
        if (!namesTimeZone)
            return inUTC->first.finish(request, FormatPatternTimeZone::Default, 0);
        auto pattern = inUTC->first.finish(request, FormatPatternTimeZone::Named, 0);
        if (!pattern)
            return std::nullopt;
        if (!agreesInNamedTimeZone(request, *pattern))
            return didFailCrossCheck(request);
        return pattern;
    }

    bool agreesInNamedTimeZone(const FormatterRequest& request, const Vector<uint8_t>& pattern)
    {
        auto* throughICU = IntlDateTimeFormat::create(m_vm, m_globalObject->dateTimeFormatStructure());
        throughICU->initializeDateTimeFormat(m_globalObject, jsString(m_vm, request.locale), optionsOf(request), request.required, request.defaults);
        if (hasException())
            return false;
        auto* formatter = IntlDateTimeFormat::create(m_vm, m_globalObject->dateTimeFormatStructure());
        formatter->setCompiledPattern(&headerOf(pattern));
        constexpr double hour = 3600000;
        constexpr double startOf1900 = -2208988800000.0;
        constexpr double startOf2024 = 1704067200000.0;
        constexpr double startOf2100 = 4102444800000.0;
        constexpr double year = 365 * 24 * hour;
        Vector<double> timeValues;
        for (unsigned i = 0; i < 366 * 4; i += m_isBrief ? 61 : 1)
            timeValues.append(startOf2024 + i * 6 * hour + 1799999);
        PseudoRandomNumbers random;
        for (unsigned i = 0; i < (m_isBrief ? 12 : 600); ++i)
            timeValues.append(std::floor(i % 3 ? startOf1900 + random.next() * (startOf2100 - startOf1900) : firstTimeValueOfYear1 + 48 * hour + random.next() * (maxTimeValue - 96 * hour - firstTimeValueOfYear1)));
        bool agrees = true;
        for (double timeValue : timeValues) {
            String text = tryFormatTimeValueWithoutValidation(m_globalObject, *formatter, timeValue);
            agrees = !text.isNull() && text == formatted(*throughICU, timeValue);
            if (!agrees)
                break;
        }
        for (double transition = m_isBrief ? startOf2024 + 2 * year : startOf2100; agrees && std::isfinite(transition) && transition > (m_isBrief ? startOf2024 - year : startOf1900); transition = formatter->utcOffsets()->start) {
            for (double timeValue : { transition, transition - 1 }) {
                String text = tryFormatTimeValueWithoutValidation(m_globalObject, *formatter, timeValue);
                agrees = agrees && !text.isNull() && text == formatted(*throughICU, timeValue);
            }
        }
        formatter->setCompiledPattern(nullptr);
        return agrees;
    }

    std::optional<std::pair<PatternCode, int32_t>> planDateTime(const FormatterRequest& request, ASCIILiteral timeZone)
    {
        auto* formatter = IntlDateTimeFormat::create(m_vm, m_globalObject->dateTimeFormatStructure());
        formatter->initializeDateTimeFormat(m_globalObject, jsString(m_vm, request.locale), optionsOf(request, timeZone), request.required, request.defaults);
        if (hasException())
            return std::nullopt;
        if (formatter->ensureCalendar() != "gregory"_s || formatter->ensureNumberingSystem() != "latn"_s)
            return refuse("the calendar is "_s, formatter->ensureCalendar(), " and the numbering system "_s, formatter->ensureNumberingSystem());
        auto utcOffsetInMinutes = fixedUTCOffsetInMinutes(formatter->impl().m_timeZoneForResolvedOptions);
        if (!utcOffsetInMinutes)
            return std::nullopt;
        auto code = codeFromPatternOf(formatter->impl().m_dateFormat.get());
        if (!code)
            return std::nullopt;
        auto pattern = code->finish(request, FormatPatternTimeZone::FixedOffset, *utcOffsetInMinutes);
        if (!pattern)
            return refuse("the pattern or a text from it would be too long"_s);

        Vector<double> timeValues;
        constexpr double second = 1000;
        constexpr double minute = 60 * second;
        constexpr double hour = 60 * minute;
        constexpr double day = 24 * hour;
        constexpr double startOf2001 = 978307200000.0;
        double base = startOf2001 + 33 * day + 4 * hour + 5 * minute + 6 * second + 789 - *utcOffsetInMinutes * minute;
        for (unsigned i = 0; i < 48; ++i)
            timeValues.append(base + i * (31 * day + hour + 7 * minute + 11 * second + 101));
        if (!m_isBrief) {
            for (unsigned i = 0; i < 366; i += 5)
                timeValues.append(base + i * day);
            for (unsigned i = 0; i < 31; ++i)
                timeValues.append(base - 33 * day + i * day);
            for (unsigned i = 0; i < 24; ++i)
                timeValues.append(base - 4 * hour + i * hour);
            for (unsigned i = 0; i < 60; ++i) {
                timeValues.append(base - 5 * minute + i * minute);
                timeValues.append(base - 6 * second + i * second);
            }
            for (unsigned millisecond : { 0, 1, 5, 9, 10, 50, 99, 100, 500, 999 })
                timeValues.append(base - 789 + millisecond);
        }
        constexpr double daysIn400Years = 146097;
        for (double years : { -2000.0, -1999.0, -1992.0, -1991.0, -1990.0, -1902.0, -1901.0, -1900.0, -1002.0, -1001.0, -1000.0, -419.0, -401.0, -101.0, -32.0, -31.0, -2.0, -1.0, 23.0, 37.0, 7998.0, 7999.0, 10344.0, 97998.0, 97999.0, 273000.0 })
            timeValues.append(base + std::floor(years * daysIn400Years / 400) * day);

        PseudoRandomNumbers random;
        constexpr double startOf1900 = -2208988800000.0;
        constexpr double startOf2100 = 4102444800000.0;
        for (unsigned i = 0; i < (m_isBrief ? 8 : 400); ++i) {
            if (i & 1)
                timeValues.append(std::floor(startOf1900 + random.next() * (startOf2100 - startOf1900)));
            else
                timeValues.append(std::floor(firstTimeValueOfYear1 + day + random.next() * (maxTimeValue - 2 * day - firstTimeValueOfYear1)));
        }
        for (double timeValue : timeValues) {
            if (formatDateTimeWithPattern(headerOf(*pattern), fieldsForDateTimePattern(headerOf(*pattern), timeValue)) != formatted(*formatter, timeValue))
                return didFailCrossCheck(request);
        }
        return std::pair { WTF::move(*code), *utcOffsetInMinutes };
    }

    struct NumberParts {
        String prefix;
        String suffix;
        String digits;
        String groupSeparator;
        String decimalSeparator;
        Vector<unsigned, 8> groupLengths;
        unsigned fractionLength { 0 };

        bool operator==(const NumberParts&) const = default;
    };

    std::optional<NumberParts> partsOf(const IntlNumberFormat& formatter, double number, bool hasFraction)
    {
        String text = formatted(formatter, number);
        if (text.isNull())
            return std::nullopt;
        size_t first = text.find(isASCIIDigit<char16_t>);
        if (first == notFound)
            return std::nullopt;
        size_t end = text.length();
        while (!isASCIIDigit(text[end - 1]))
            --end;
        NumberParts parts;
        parts.prefix = text.left(first);
        parts.suffix = text.substring(end);
        StringBuilder digits;
        Vector<String, 8> separators;
        for (size_t i = first; i < end;) {
            size_t start = i;
            while (i < end && isASCIIDigit(text[i]))
                ++i;
            parts.groupLengths.append(i - start);
            digits.append(StringView(text).substring(start, i - start));
            start = i;
            while (i < end && !isASCIIDigit(text[i]))
                ++i;
            if (i > start)
                separators.append(text.substring(start, i - start));
        }
        parts.digits = digits.toString();
        if (hasFraction) {
            if (separators.isEmpty())
                return std::nullopt;
            parts.decimalSeparator = separators.takeLast();
            parts.fractionLength = parts.groupLengths.takeLast();
        }
        for (auto& separator : separators) {
            if (separator != separators[0])
                return std::nullopt;
        }
        if (!separators.isEmpty())
            parts.groupSeparator = separators[0];
        if (!m_isBrief && partsByFieldPositions(formatter, number) != parts)
            return std::nullopt;
        return parts;
    }

    std::optional<NumberParts> partsByFieldPositions(const IntlNumberFormat& formatter, double number)
    {
        auto* list = dynamicDowncast<JSArray>(formatter.formatToParts(m_globalObject, number));
        if (hasException() || !list)
            return std::nullopt;
        enum class Section : uint8_t { Prefix, Integer, Fraction, Suffix };
        Section section = Section::Prefix;
        NumberParts parts;
        StringBuilder prefix;
        StringBuilder suffix;
        StringBuilder digits;
        for (unsigned i = 0; i < list->length(); ++i) {
            auto* part = list->canGetIndexQuickly(i) ? dynamicDowncast<JSObject>(list->getIndexQuickly(i)) : nullptr;
            if (!part)
                return std::nullopt;
            JSValue type = part->getDirect(m_vm, m_vm.propertyNames->type);
            JSValue value = part->getDirect(m_vm, m_vm.propertyNames->value);
            if (!type || !type.isString() || !value || !value.isString())
                return std::nullopt;
            String kind = asString(type)->value(m_globalObject);
            String text = asString(value)->value(m_globalObject);
            if (kind == "integer"_s) {
                if (section > Section::Integer)
                    return std::nullopt;
                section = Section::Integer;
                parts.groupLengths.append(text.length());
                digits.append(text);
            } else if (kind == "group"_s) {
                if (section != Section::Integer || (!parts.groupSeparator.isNull() && parts.groupSeparator != text))
                    return std::nullopt;
                parts.groupSeparator = text;
            } else if (kind == "decimal"_s) {
                if (section != Section::Integer)
                    return std::nullopt;
                section = Section::Fraction;
                parts.decimalSeparator = text;
            } else if (kind == "fraction"_s) {
                if (section != Section::Fraction || parts.fractionLength)
                    return std::nullopt;
                parts.fractionLength = text.length();
                digits.append(text);
            } else if (section == Section::Prefix)
                prefix.append(text);
            else {
                section = Section::Suffix;
                suffix.append(text);
            }
        }
        if (section == Section::Prefix)
            return std::nullopt;
        parts.prefix = prefix.toString();
        parts.suffix = suffix.toString();
        parts.digits = digits.toString();
        return parts;
    }

    std::optional<Vector<uint8_t>> compileNumber(const FormatterRequest& request)
    {
        if (!isSupportedLocale(request.locale))
            return refuse("ICU has no data for the locale"_s);
        auto* formatter = IntlNumberFormat::create(m_vm, m_globalObject->numberFormatStructure());
        formatter->initializeNumberFormat(m_globalObject, jsString(m_vm, request.locale), optionsOf(request));
        if (hasException())
            return std::nullopt;

        JSObject* resolved = formatter->resolvedOptions(m_globalObject);
        auto option = [&](ASCIILiteral name) {
            return resolved->getDirect(m_vm, Identifier::fromString(m_vm, name));
        };
        auto textOf = [&](ASCIILiteral name) -> String {
            JSValue value = option(name);
            if (!value || !value.isString())
                return { };
            return asString(value)->value(m_globalObject);
        };
        auto numberOf = [&](ASCIILiteral name) {
            JSValue value = option(name);
            return value && value.isNumber() ? value.asNumber() : PNaN;
        };
        String style = textOf("style"_s);
        bool isCurrency = style == "currency"_s;
        bool isPercent = style == "percent"_s;
        if (!isCurrency && !isPercent && style != "decimal"_s)
            return refuse("the style is "_s, style);
        unsigned decimalShift = isPercent ? 2 : 0;
        if (isCurrency) {
            String display = textOf("currencyDisplay"_s);
            String sign = textOf("currencySign"_s);
            if ((display != "symbol"_s && display != "narrowSymbol"_s && display != "code"_s) || (sign != "standard"_s && sign != "accounting"_s))
                return refuse("currencyDisplay is "_s, display, " and currencySign "_s, sign);
        }
        for (auto [name, expected] : { std::pair { "numberingSystem"_s, "latn"_s }, std::pair { "notation"_s, "standard"_s }, std::pair { "signDisplay"_s, "auto"_s }, std::pair { "roundingMode"_s, "halfExpand"_s }, std::pair { "roundingPriority"_s, "auto"_s }, std::pair { "trailingZeroDisplay"_s, "auto"_s } }) {
            if (textOf(name) != expected)
                return refuse(name, " is "_s, textOf(name));
        }
        if (numberOf("roundingIncrement"_s) != 1)
            return refuse("there is a rounding increment"_s);
        double resolvedMinIntegerDigits = numberOf("minimumIntegerDigits"_s);
        if (option("minimumSignificantDigits"_s) || option("maximumSignificantDigits"_s) || !(resolvedMinIntegerDigits >= 1 && resolvedMinIntegerDigits <= 3))
            return refuse("significant digits are asked for, or more than three integer digits"_s);
        unsigned minIntegerDigits = static_cast<unsigned>(resolvedMinIntegerDigits);
        double resolvedMinFractionDigits = numberOf("minimumFractionDigits"_s);
        double resolvedMaxFractionDigits = numberOf("maximumFractionDigits"_s);
        if (!(resolvedMinFractionDigits >= 0 && resolvedMinFractionDigits <= resolvedMaxFractionDigits && resolvedMaxFractionDigits <= maxFractionDigitsInPattern))
            return refuse("more than "_s, maxFractionDigitsInPattern, " fraction digits are asked for"_s);
        unsigned minFractionDigits = static_cast<unsigned>(resolvedMinFractionDigits);
        unsigned maxFractionDigits = static_cast<unsigned>(resolvedMaxFractionDigits);
        JSValue useGrouping = option("useGrouping"_s);
        if (!useGrouping || !(useGrouping.isFalse() || useGrouping.isString()))
            return std::nullopt;

        double scale = std::pow(10, decimalShift);
        auto positive = partsOf(*formatter, 1, minFractionDigits);
        auto negative = partsOf(*formatter, -1, minFractionDigits);
        auto large = partsOf(*formatter, 1234567890123.0 / scale, minFractionDigits);
        if (!positive || !negative || !large)
            return refuse("ICU's text for a number is not a prefix, digits with separators and a suffix, or not as its field positions say"_s);
        StringBuilder one;
        for (unsigned digits = 1 + decimalShift; digits < minIntegerDigits; ++digits)
            one.append('0');
        one.append('1');
        for (unsigned i = 0; i < decimalShift + minFractionDigits; ++i)
            one.append('0');
        if (positive->digits != one.toString() || negative->digits != positive->digits || positive->fractionLength != minFractionDigits || !large->digits.startsWith("1234567890123"_s))
            return refuse("ICU's digits for 1, -1 or 1234567890123 are not as planned"_s);
        if (large->prefix != positive->prefix || large->suffix != positive->suffix)
            return refuse("what stands around the digits depends on the number"_s);

        unsigned primaryGroupSize = 0;
        unsigned secondaryGroupSize = 0;
        String separator = emptyString();
        if (auto& lengths = large->groupLengths; lengths.size() > 1) {
            primaryGroupSize = lengths.last();
            secondaryGroupSize = lengths[lengths.size() - 2];
            bool isRegular = !useGrouping.isFalse() && lengths.size() > 2 && lengths[0] <= secondaryGroupSize;
            for (size_t i = 1; i + 1 < lengths.size(); ++i)
                isRegular &= lengths[i] == secondaryGroupSize;
            if (!isRegular || primaryGroupSize > 4 || secondaryGroupSize > 4 || primaryGroupSize < 3)
                return refuse("the groups of 1234567890123 are not regular"_s);
            separator = large->groupSeparator;
        }

        String decimalSeparator = emptyString();
        if (maxFractionDigits) {
            auto half = partsOf(*formatter, 0.5 / scale, true);
            if (!half || half->decimalSeparator.isNull() || half->prefix != positive->prefix || half->suffix != positive->suffix)
                return refuse("ICU's parts of one half are not as planned"_s);
            decimalSeparator = half->decimalSeparator;
        }

        unsigned minDigitsToGroup = 0;
        if (primaryGroupSize) {
            double number = std::pow(10, primaryGroupSize) / scale;
            for (unsigned digits = primaryGroupSize + 1; digits <= primaryGroupSize + 2 && !minDigitsToGroup; ++digits, number *= 10) {
                auto parts = partsOf(*formatter, number, minFractionDigits);
                if (!parts)
                    return std::nullopt;
                if (parts->groupLengths.size() > 1)
                    minDigitsToGroup = digits;
            }
            if (!minDigitsToGroup)
                return refuse("more than two digits are needed before the first group separator"_s);
        }

        PatternCode code;
        for (unsigned parameter : { primaryGroupSize, secondaryGroupSize, minDigitsToGroup, minFractionDigits, maxFractionDigits, minIntegerDigits, decimalShift })
            code.append(parameter);
        for (auto& text : { positive->prefix, positive->suffix, negative->prefix, negative->suffix, separator, decimalSeparator })
            code.appendText(text);
        code.didAppendField(std::max(positive->prefix.length() + positive->suffix.length(), negative->prefix.length() + negative->suffix.length()) + maxDecimalLengthOfInteger * (1 + separator.length()) + decimalSeparator.length() + maxFractionDigitsInPattern);
        auto pattern = code.finish(request, FormatPatternTimeZone::FixedOffset, 0);
        if (!pattern)
            return std::nullopt;

        Vector<double> numbers { 0, 5, 9007199254740991.0, 2147483647, 2147483648.0, 4294967295.0, 4294967296.0 };
        for (double power = 1; power < 1e16; power *= 10)
            numbers.appendList({ power - 1, power, power + 1, power * 5, power * 9 });
        for (double scale = 1; scale <= 1e9; scale *= 10) {
            for (double numerator : { 1.0, 4.0, 5.0, 6.0, 9.0, 15.0, 25.0, 35.0, 45.0, 49.0, 50.0, 51.0, 95.0, 99.0, 105.0, 125.0, 375.0, 995.0, 999.0, 1005.0, 1255.0, 2675.0, 8345.0, 9995.0, 99995.0, 123456789.0 })
                numbers.appendList({ numerator / scale, 1 + numerator / scale, 1234567 + numerator / scale });
        }
        numbers.appendList({ 0.1 + 0.2, 1.0 / 3, 2.0 / 3, 1e-6, 1.5e-6, 9.999999e-7, 0.000001234, 4503599627370495.5, 1125899906842623.75, 999999.9999999999 });
        PseudoRandomNumbers random;
        for (unsigned i = 0; i < 200; ++i)
            numbers.append(std::floor(std::pow(10, random.next() * 15.9)));
        for (unsigned i = 0; i < 400; ++i) {
            double scale = std::pow(10, std::floor(random.next() * 8));
            numbers.append(i & 1 ? std::floor(random.next() * 1e9) / scale : std::pow(10, random.next() * 14 - 5));
        }
        for (size_t i = 0; i < numbers.size(); i += m_isBrief ? 29 : 1) {
            for (double signedNumber : { numbers[i], -numbers[i] }) {
                String text = formatNumberWithPattern(headerOf(*pattern), signedNumber);
                if (!text.isNull() && text != formatted(*formatter, signedNumber))
                    return didFailCrossCheck(request);
            }
        }
        return pattern;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    bool m_isBrief;
    String m_whyRefused;
    TopExceptionScope* m_scope { nullptr };
    unsigned m_numberOfFailedCrossChecks { 0 };
};

} // namespace

static std::optional<Vector<uint8_t>> planFormatPatternAgain(JSGlobalObject* globalObject, const FormatPatternHeader& header)
{
    return PatternCompiler(globalObject, CrossCheck::Brief).compile(FormatterRequest::fromConstructorArgumentsOf(header));
}

namespace {

class ConstantFormatterSpecialization {
public:
    explicit ConstantFormatterSpecialization(Program& program)
        : m_program(program)
    {
    }

    void run()
    {
        findCandidates();
        findConstantOptions();
        findConstructions();
        findLocaleStringCalls();
        if (m_requests.isEmpty())
            return;
        compilePatterns();
        findVariables();
        findCalls();
        rewriteCalls();
        rewriteConstructions();
        attachPatternsToLocaleStringCalls();
    }

private:
    struct Call {
        unsigned function;
        ValueID value;
        unsigned request;
        ValueID methodGetById;
    };

    enum class CandidateKind : uint8_t { StoreOfConstantOptions, StoreOfConstruction, Construction, LocaleStringCall, FormatMethodCall, FormatValueCall };
    struct Candidate {
        unsigned function;
        CandidateKind kind;
        ValueID value;
        std::unique_ptr<Vector<ConstantOption, 8>> options;
    };
    struct Aliases {
        unsigned function;
        Vector<uint32_t> escapedVariables;
        Vector<std::pair<ValueID, uint32_t>> variableOfValue;
    };

    bool isNamed(StringID id, ASCIILiteral name) const { return id && equalSpans(m_program.string(id), asBytes(name.span8())); }
    String textOf(StringID id) const { return String::fromUTF8(m_program.string(id)); }
    static uint64_t keyOf(unsigned function, ValueID value) { return static_cast<uint64_t>(function) << 32 | value.index(); }

    std::optional<FormatPatternKind> kindConstructedBy(const Function& function, const Value& construction) const
    {
        auto& constructor = function.at(m_program.underlyingValue(function, construction.child(0)));
        if (constructor.opcode != Opcode::GetById)
            return std::nullopt;
        auto& holder = function.at(m_program.underlyingValue(function, constructor.child(0)));
        if (holder.opcode != Opcode::GetGlobal || !isNamed(holder.string(), "Intl"_s))
            return std::nullopt;
        if (isNamed(constructor.string(), "DateTimeFormat"_s))
            return FormatPatternKind::DateTime;
        if (isNamed(constructor.string(), "NumberFormat"_s))
            return FormatPatternKind::Number;
        return std::nullopt;
    }

    std::optional<ConstantOption> constantOption(const Function& function, StringID name, ValueID id) const
    {
        auto& value = function.at(m_program.underlyingValue(function, id));
        ConstantOption option { textOf(name), ConstantOption::Kind::Number, { }, 0 };
        switch (value.opcode) {
        case Opcode::String:
            option.kind = ConstantOption::Kind::String;
            option.string = textOf(value.string());
            return option;
        case Opcode::Boolean:
            option.kind = ConstantOption::Kind::Boolean;
            option.number = value.boolean();
            return option;
        case Opcode::Integer:
            option.number = static_cast<double>(value.integer());
            return option;
        case Opcode::Double:
            option.number = std::bit_cast<double>(value.bits());
            return option;
        default:
            return std::nullopt;
        }
    }

    bool collectOptions(unsigned index, const Function& function, ValueID user, ValueID argument, FormatterRequest& request) const
    {
        ValueID object = m_program.underlyingValue(function, argument);
        if (function.at(object).opcode == Opcode::Undefined)
            return true;
        request.hasOptions = true;
        if (auto alias = m_aliasesOfConstantOptions.find(keyOf(index, object)); alias != m_aliasesOfConstantOptions.end()) {
            auto found = m_constantOptions.find(alias->value);
            if (found == m_constantOptions.end() || !found->value)
                return false;
            request.options = *found->value;
            return true;
        }
        return collectOptionsOfLiteral(function, user, object, request.options);
    }

    bool collectOptionsOfLiteral(const Function& function, ValueID construction, ValueID object, Vector<ConstantOption, 8>& options) const
    {
        auto& allocation = function.at(object);
        if (allocation.opcode != Opcode::NewObject && allocation.opcode != Opcode::NewObjectWithShape)
            return false;
        auto& builtins = Builtins::singleton();
        auto isObject = [&](ValueID id) {
            return id && m_program.underlyingValue(function, id) == object;
        };
        auto uses = [&](const Value& value) {
            bool result = false;
            value.forEachChild([&](ValueID child) {
                result |= isObject(child);
            });
            return result;
        };
        auto onlyReads = [&](const Value& user) {
            return user.arguments.size() == 2 && isObject(user.arguments[1]) && readsOnlyOptions(function, user, user.arguments[1]);
        };
        bool isConstructed = false;
        bool hasOtherReaders = false;
        for (auto& block : function.blocks) {
            bool isHome = block.values.contains(object);
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (!uses(value))
                    continue;
                if (id == construction) {
                    isConstructed = true;
                    continue;
                }
                if (value.opcode == Opcode::Cast && isObject(id))
                    continue;
                if (onlyReads(value)) {
                    hasOtherReaders = true;
                    continue;
                }
                if (!isHome || isConstructed || hasOtherReaders)
                    return false;
                std::optional<ConstantOption> option;
                if (value.opcode == Opcode::PutField && isObject(value.child(0)))
                    option = constantOption(function, m_program.at(value.layout()).fields[value.field()].name, value.child(1));
                else if (value.opcode == Opcode::PutById && isObject(value.child(0)))
                    option = constantOption(function, value.string(), value.child(1));
                else if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 3 && isObject(value.arguments[0])) {
                    auto& implementation = builtins.at(value.builtin()).implementation;
                    auto& name = function.at(value.arguments[1]);
                    if (implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField && name.opcode == Opcode::String)
                        option = constantOption(function, name.string(), value.arguments[2]);
                }
                if (!option || options.containsIf([&](auto& earlier) { return earlier.name == option->name; }))
                    return false;
                options.append(WTF::move(*option));
            }
            bool isUsedByTerminator = false;
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                isUsedByTerminator |= isObject(child);
            });
            if (isUsedByTerminator)
                return false;
        }
        return isConstructed && (!hasOtherReaders || onlyReads(function.at(construction)));
    }

    std::optional<FormatterRequest> requestOf(unsigned index, const Function& function, ValueID id, std::optional<FormatPatternKind> kind) const
    {
        auto& user = function.at(id);
        if (!kind || user.arguments.isEmpty() || user.arguments.size() > 2)
            return std::nullopt;
        auto& locale = function.at(m_program.underlyingValue(function, user.arguments[0]));
        if (locale.opcode != Opcode::String)
            return std::nullopt;
        FormatterRequest request { *kind, textOf(locale.string()) };
        if (user.arguments.size() == 2 && !collectOptions(index, function, id, user.arguments[1], request))
            return std::nullopt;
        return request;
    }

    unsigned add(FormatterRequest&& request)
    {
        return m_requestsByKey.ensure(request.key(), [&] {
            m_requests.append(WTF::move(request));
            return m_requests.size() - 1;
        }).iterator->value;
    }

    template<typename Functor>
    void forEachCandidate(CandidateKind kind, const Functor& functor)
    {
        auto& functions = std::as_const(m_program).functions;
        for (auto& candidate : m_candidates) {
            if (candidate.kind == kind)
                functor(candidate.function, functions[candidate.function], candidate);
        }
    }

    BuiltinID localeStringMethodCalledBy(const Value& call) const
    {
        auto& builtins = Builtins::singleton();
        BuiltinID entry;
        if (call.opcode == Opcode::CallBuiltin)
            entry = call.builtin();
        else if (call.predictedReceiverClassPlusOne)
            entry = builtins.findForInstance(BuiltinKind::Method, builtins.classes()[call.predictedReceiverClassPlusOne - 1], m_program.builtinName(call.string()));
        if (!entry)
            return { };
        auto& builtin = builtins.at(entry);
        if (builtin.implementation.kind != BuiltinImplementation::Inline || !isLocaleStringMethod(builtin.implementation.inlined) || call.child(2))
            return { };
        return entry;
    }

    bool isFormatMethodCall(const Value& call) const
    {
        return call.arguments.size() == 1 && !call.child(1) && isNamed(call.string(), "format"_s);
    }

    bool isFormatValueCall(const Function& function, const Value& call) const
    {
        auto& read = function.at(call.child(0));
        return call.arguments.size() == 1 && read.opcode == Opcode::GetById && isNamed(read.string(), "format"_s);
    }

    void findCandidates()
    {
        auto& functions = std::as_const(m_program).functions;
        m_numberOfStores.fill(0, m_program.variables.size());
        Lock lock;
        forEachIndexInParallel(functions.size(), [&](unsigned index) {
            auto& function = functions[index];
            Vector<Candidate, 4> found;
            static constexpr OpcodeSet storesConstructionsAndCalls { Opcode::PutModuleVariable, Opcode::Construct, Opcode::CallBuiltin, Opcode::CallMethod, Opcode::Call };
            functions.forEachIndexedValue(index, storesConstructionsAndCalls, [&](ValueID id) {
                auto& value = function.at(id);
                switch (value.opcode) {
                case Opcode::PutModuleVariable: {
                    WTF::atomicExchangeAdd(&m_numberOfStores[value.variable().index()], 1u, std::memory_order_relaxed);
                    ValueID origin = m_program.underlyingValue(function, value.child(0));
                    ConstantOptions options;
                    if (collectOptionsOfLiteral(function, id, origin, options))
                        found.append({ index, CandidateKind::StoreOfConstantOptions, id, makeUniqueWithoutFastMallocCheck<ConstantOptions>(WTF::move(options)) });
                    if (function.at(origin).opcode == Opcode::Construct)
                        found.append({ index, CandidateKind::StoreOfConstruction, id, nullptr });
                    break;
                }
                case Opcode::Construct:
                    if (kindConstructedBy(function, value))
                        found.append({ index, CandidateKind::Construction, id, nullptr });
                    break;
                case Opcode::CallBuiltin:
                    if (localeStringMethodCalledBy(value))
                        found.append({ index, CandidateKind::LocaleStringCall, id, nullptr });
                    break;
                case Opcode::CallMethod:
                    if (localeStringMethodCalledBy(value))
                        found.append({ index, CandidateKind::LocaleStringCall, id, nullptr });
                    if (isFormatMethodCall(value))
                        found.append({ index, CandidateKind::FormatMethodCall, id, nullptr });
                    break;
                case Opcode::Call:
                    if (isFormatValueCall(function, value))
                        found.append({ index, CandidateKind::FormatValueCall, id, nullptr });
                    break;
                default:
                    break;
                }
            });
            if (found.isEmpty())
                return;
            Locker locker { lock };
            m_candidates.appendVector(WTF::move(found));
        });
        std::ranges::stable_sort(m_candidates, { }, &Candidate::function);
    }

    bool readsOnlyOptions(const Function& function, const Value& user, ValueID options) const
    {
        unsigned numberOfUses = 0;
        user.forEachChild([&](ValueID child) {
            numberOfUses += child == options;
        });
        if (numberOfUses != 1 || user.arguments.size() != 2 || user.arguments[1] != options)
            return false;
        if (user.opcode == Opcode::Construct)
            return !!kindConstructedBy(function, user);
        if (user.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = Builtins::singleton().at(user.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::NumberToLocaleString && m_program.typeOf(function, user.child(0)).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    }

    void findConstantOptions()
    {
        forEachCandidate(CandidateKind::StoreOfConstantOptions, [&](unsigned, const Function& function, Candidate& candidate) {
            uint32_t variable = function.at(candidate.value).variable().index();
            if (m_numberOfStores[variable] == 1)
                m_constantOptions.add(variable, WTF::move(candidate.options));
        });
        if (m_constantOptions.isEmpty())
            return;
        auto& functions = std::as_const(m_program).functions;
        Vector<Aliases> aliasesByFunction;
        Lock lock;
        forEachIndexInParallel(functions.size(), [&](unsigned index) {
            Aliases aliases { index, { }, { } };
            findAliasesOfConstantOptions(index, aliases);
            if (aliases.escapedVariables.isEmpty() && aliases.variableOfValue.isEmpty())
                return;
            Locker locker { lock };
            aliasesByFunction.append(WTF::move(aliases));
        });
        std::ranges::sort(aliasesByFunction, { }, &Aliases::function);
        for (auto& aliases : aliasesByFunction) {
            for (uint32_t variable : aliases.escapedVariables)
                m_constantOptions.find(variable)->value = nullptr;
            for (auto [value, variable] : aliases.variableOfValue)
                m_aliasesOfConstantOptions.add(keyOf(aliases.function, value), variable);
        }
    }

    void findAliasesOfConstantOptions(unsigned index, Aliases& result) const
    {
        static constexpr uint32_t none = UINT32_MAX;
        auto& function = std::as_const(m_program).functions[index];
        Vector<uint32_t> variableOf;
        auto escapes = [&](uint32_t variable) {
            if (variable != none && m_constantOptions.contains(variable))
                result.escapedVariables.append(variable);
        };
        auto add = [&](ValueID id, uint32_t variable) {
            uint32_t& known = variableOf[id.index()];
            if (known == variable)
                return false;
            if (known != none) {
                escapes(known);
                escapes(variable);
                return false;
            }
            known = variable;
            return true;
        };
        static constexpr OpcodeSet variableReads { Opcode::GetModuleVariable };
        std::as_const(m_program).functions.forEachIndexedValue(index, variableReads, [&](ValueID id) {
            auto& value = function.at(id);
            if (!m_constantOptions.contains(value.variable().index()))
                return;
            if (variableOf.isEmpty())
                variableOf.fill(none, function.values.size());
            add(id, value.variable().index());
        });
        if (variableOf.isEmpty())
            return;
        auto alias = [&](ValueID id) { return id ? variableOf[id.index()] : none; };
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::Cast && alias(value.child(0)) != none)
                        changed |= add(id, alias(value.child(0)));
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (alias(target.arguments[i]) != none)
                            changed |= add(parameters[i], alias(target.arguments[i]));
                    }
                }
            }
        }
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& user = function.at(id);
                user.forEachChild([&](ValueID child) {
                    if (alias(child) != none && user.opcode != Opcode::Cast && user.opcode != Opcode::IsUndefinedOrNull && !readsOnlyOptions(function, user, child))
                        escapes(alias(child));
                });
            }
            if (alias(block.terminator.value) != none)
                escapes(alias(block.terminator.value));
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (alias(parameters[i]) != alias(target.arguments[i]))
                        escapes(alias(parameters[i]));
                }
            }
        }
        for (unsigned i = 0; i < variableOf.size(); ++i) {
            if (variableOf[i] != none)
                result.variableOfValue.append({ ValueID(i), variableOf[i] });
        }
    }

    void findConstructions()
    {
        forEachCandidate(CandidateKind::Construction, [&](unsigned index, const Function& function, Candidate& candidate) {
            ValueID id = candidate.value;
            auto request = requestOf(index, function, id, kindConstructedBy(function, function.at(id)));
            if (!request)
                return;
            unsigned number = add(WTF::move(*request));
            m_constructions.add(keyOf(index, id), number);
            m_constructionSites.append({ index, id, number, { } });
        });
    }

    void findLocaleStringCalls()
    {
        using RequiredComponent = IntlDateTimeFormat::RequiredComponent;
        using Defaults = IntlDateTimeFormat::Defaults;
        forEachCandidate(CandidateKind::LocaleStringCall, [&](unsigned index, const Function& function, Candidate& candidate) {
            ValueID id = candidate.value;
            auto& builtin = Builtins::singleton().at(localeStringMethodCalledBy(function.at(id)));
            auto kind = FormatPatternKind::DateTime;
            auto required = RequiredComponent::Any;
            auto defaults = Defaults::Date;
            if (builtin.implementation.inlined == InlineBuiltin::NumberToLocaleString)
                kind = FormatPatternKind::Number;
            else if (builtin.name == "toLocaleDateString"_s)
                required = RequiredComponent::Date;
            else if (builtin.name == "toLocaleTimeString"_s) {
                required = RequiredComponent::Time;
                defaults = Defaults::Time;
            } else
                defaults = Defaults::All;
            auto request = requestOf(index, function, id, kind);
            if (!request)
                return;
            request->required = required;
            request->defaults = defaults;
            m_localeStringCalls.append({ index, id, add(WTF::move(*request)), { } });
        });
    }

    void findVariables()
    {
        forEachCandidate(CandidateKind::StoreOfConstruction, [&](unsigned index, const Function& function, Candidate& candidate) {
            auto& store = function.at(candidate.value);
            if (m_numberOfStores[store.variable().index()] != 1)
                return;
            auto construction = m_constructions.find(keyOf(index, m_program.underlyingValue(function, store.child(0))));
            if (construction != m_constructions.end() && m_requests[construction->value].pattern)
                m_variables.add(store.variable().index(), construction->value);
        });
    }

    std::optional<unsigned> requestOfFormatter(unsigned index, const Function& function, ValueID id) const
    {
        ValueID origin = m_program.underlyingValue(function, id);
        auto& value = function.at(origin);
        if (value.opcode == Opcode::GetModuleVariable) {
            if (auto found = m_variables.find(value.variable().index()); found != m_variables.end())
                return found->value;
            return std::nullopt;
        }
        if (auto found = m_constructions.find(keyOf(index, origin)); found != m_constructions.end() && m_requests[found->value].pattern)
            return found->value;
        return std::nullopt;
    }

    void findCalls()
    {
        forEachCandidate(CandidateKind::FormatMethodCall, [&](unsigned index, const Function& function, Candidate& candidate) {
            ValueID id = candidate.value;
            auto& call = function.at(id);
            auto request = requestOfFormatter(index, function, call.child(0));
            if (!request)
                return;
            m_nameOfFormatMethod = call.string();
            m_calls.append({ index, id, *request, { } });
        });
        forEachCandidate(CandidateKind::FormatValueCall, [&](unsigned index, const Function& function, Candidate& candidate) {
            ValueID id = candidate.value;
            auto& call = function.at(id);
            ValueID readID = call.child(0);
            auto& read = function.at(readID);
            if (m_program.underlyingValue(function, read.child(0)) != m_program.underlyingValue(function, call.child(1)))
                return;
            auto request = requestOfFormatter(index, function, read.child(0));
            if (!request || !canReadMethodAtCall(function, readID, id))
                return;
            m_nameOfFormatMethod = read.string();
            m_calls.append({ index, id, *request, readID });
        });
    }

    bool canReadMethodAtCall(const Function& function, ValueID read, ValueID call) const
    {
        unsigned numberOfUses = 0;
        bool isInOneBlock = false;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                function.at(id).forEachChild([&](ValueID child) {
                    numberOfUses += child == read;
                });
            }
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                numberOfUses += child == read;
            });
            size_t first = block.values.find(read);
            size_t last = block.values.find(call);
            if (first == notFound || last == notFound || last < first)
                continue;
            isInOneBlock = true;
            for (size_t index = first + 1; index < last; ++index) {
                switch (function.at(block.values[index]).opcode) {
                case Opcode::Call:
                case Opcode::CallDirect:
                case Opcode::CallMethod:
                case Opcode::Construct:
                    return false;
                default:
                    break;
                }
                if (writesOf(m_program, function, block, index) >= Writes::Heap)
                    return false;
            }
        }
        return isInOneBlock && numberOfUses == 1;
    }

    void compilePatterns()
    {
        constexpr size_t stackSize = 4 * MB;
        Vector<std::optional<Vector<uint8_t>>> patterns(m_requests.size());
        unsigned numberOfFailedCrossChecks = 0;
        Thread::create("AOT format patterns"_s, [&] {
            RefPtr<VM> vm = VM::create(HeapType::Small);
            JSLockHolder locker(*vm);
            {
                Strong<JSGlobalObject> globalObject(*vm, JSGlobalObject::create(*vm, JSGlobalObject::createStructure(*vm, jsNull())));
                PatternCompiler compiler(globalObject.get(), CrossCheck::Thorough);
                for (unsigned i = 0; i < m_requests.size(); ++i)
                    patterns[i] = compiler.compile(m_requests[i]);
                numberOfFailedCrossChecks = compiler.numberOfFailedCrossChecks();
            }
            vm = nullptr;
        }, ThreadType::Unknown, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(stackSize))->waitForCompletion();
        auto& stamp = stampOfThisICU();
        if (!stamp)
            return;
        m_program.icuStampOfFormatPatterns = Vector<uint8_t>(asByteSpan(*stamp));
        unsigned numberCompiled = 0;
        for (unsigned i = 0; i < m_requests.size(); ++i) {
            if (!patterns[i])
                continue;
            m_requests[i].pattern = m_program.formatPatterns.size();
            m_program.formatPatterns.append(WTF::move(*patterns[i]));
            ++numberCompiled;
        }
        dataLogLnIf(Options::verboseAOTCompilation() || Options::logAOTFormatPatterns(), "AOT: ", m_requests.size(), " Intl formatters have a constant locale and constant options; ", numberCompiled, " have a pattern in the image; ", numberOfFailedCrossChecks, " patterns differed from ICU on a sample");
    }

    void rewriteCalls()
    {
        m_program.nameOfFormatMethod = m_nameOfFormatMethod;
        for (auto& call : m_calls) {
            auto& function = m_program.functions[call.function];
            if (call.methodGetById) {
                for (auto& block : function.blocks)
                    block.values.removeFirst(call.methodGetById);
            }
            auto& value = function.at(call.value);
            ValueID formatter = value.child(call.methodGetById ? 1 : 0);
            ValueID argument = value.arguments[0];
            becomeOperation(value, m_requests[call.request].kind == FormatPatternKind::DateTime ? Operations::FormatDateTime : Operations::FormatNumber, { formatter, argument });
        }
    }

    static void becomeOperation(Value& value, BuiltinID operation, Arguments&& arguments)
    {
        value.opcode = Opcode::CallBuiltin;
        value.children = { };
        value.immediates = { operation.index(), Format::none };
        value.setCallSignature(Format::none);
        value.isInTailPosition = false;
        value.arguments = WTF::move(arguments);
    }

    ValueID insertPatternIndexBefore(Function& function, ValueID user, uint32_t pattern) const
    {
        Value constant;
        constant.opcode = Opcode::Integer;
        constant.position = function.at(user).position;
        constant.inlineFrame = function.at(user).inlineFrame;
        constant.type = m_program.types.integer;
        constant.rep = repOf(m_program.at(constant.type));
        constant.setBits(pattern);
        ValueID id(function.values.size());
        function.values.append(WTF::move(constant));
        for (auto& block : function.blocks) {
            if (size_t index = block.values.find(user); index != notFound)
                block.values.insert(index, id);
        }
        return id;
    }

    void attachPatternsToLocaleStringCalls()
    {
        for (auto& site : m_localeStringCalls) {
            if (auto pattern = m_requests[site.request].pattern) {
                auto& function = m_program.functions[site.function];
                ValueID constant = insertPatternIndexBefore(function, site.value, *pattern);
                function.at(site.value).children[2] = constant;
            }
        }
    }

    void rewriteConstructions()
    {
        for (auto& site : m_constructionSites) {
            auto& request = m_requests[site.request];
            if (!request.pattern)
                continue;
            auto& function = m_program.functions[site.function];
            ValueID constantID = insertPatternIndexBefore(function, site.value, *request.pattern);
            auto& construction = function.at(site.value);
            ValueID options = construction.arguments.size() == 2 ? m_program.underlyingValue(function, construction.arguments[1]) : ValueID();
            becomeOperation(construction, Operations::NewConstantFormatter, { constantID });
            removeUnusedOperands(function, options);
        }
    }

    void removeUnusedOperands(Function& function, ValueID options) const
    {
        auto& builtins = Builtins::singleton();
        auto initializedObject = [&](const Value& value) -> ValueID {
            if (value.opcode == Opcode::PutField || value.opcode == Opcode::PutById)
                return value.child(0);
            if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 3)
                return { };
            auto& implementation = builtins.at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField ? value.arguments[0] : ValueID();
        };
        for (bool changed = true; changed;) {
            changed = false;
            Vector<unsigned> numberOfUses(FillWith { }, function.values.size(), 0u);
            Vector<unsigned> numberOfInitializations(FillWith { }, function.values.size(), 0u);
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = std::as_const(function).at(id);
                    value.forEachChild([&](ValueID child) {
                        ++numberOfUses[child.index()];
                    });
                    if (ValueID object = initializedObject(value))
                        ++numberOfInitializations[object.index()];
                }
                block.terminator.forEachChild([&](ValueID& child) {
                    ++numberOfUses[child.index()];
                });
            }
            auto isUnusedLiteral = [&](ValueID id) {
                return id == options && numberOfUses[id.index()] == numberOfInitializations[id.index()];
            };
            for (auto& block : function.blocks) {
                if (block.terminator.kind == TerminatorKind::Invoke)
                    continue;
                changed |= !!block.values.removeAllMatching([&](ValueID id) {
                    auto& value = function.at(id);
                    if (ValueID object = initializedObject(value); object && isUnusedLiteral(object))
                        return true;
                    if (numberOfUses[id.index()])
                        return false;
                    switch (value.opcode) {
                    case Opcode::String:
                    case Opcode::Boolean:
                    case Opcode::Integer:
                    case Opcode::Double:
                    case Opcode::Undefined:
                        return true;
                    case Opcode::NewObject:
                    case Opcode::NewObjectWithShape:
                        return isUnusedLiteral(id);
                    case Opcode::GetGlobal:
                        return isNamed(value.string(), "Intl"_s);
                    case Opcode::GetById: {
                        auto& holder = function.at(m_program.underlyingValue(function, value.child(0)));
                        return holder.opcode == Opcode::GetGlobal && isNamed(holder.string(), "Intl"_s) && (isNamed(value.string(), "DateTimeFormat"_s) || isNamed(value.string(), "NumberFormat"_s));
                    }
                    default:
                        return false;
                    }
                });
            }
        }
    }

    Program& m_program;
    Vector<FormatterRequest> m_requests;
    UncheckedKeyHashMap<uint64_t, unsigned, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_constructions;
    UncheckedKeyHashMap<uint32_t, unsigned, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_variables;
    Vector<Call> m_calls;
    Vector<Call> m_constructionSites;
    Vector<Call> m_localeStringCalls;
    UncheckedKeyHashMap<String, unsigned> m_requestsByKey;
    Vector<Candidate> m_candidates;
    Vector<uint32_t> m_numberOfStores;
    using ConstantOptions = Vector<ConstantOption, 8>;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<ConstantOptions>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constantOptions;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_aliasesOfConstantOptions;
    StringID m_nameOfFormatMethod;
};

} // namespace

void specializeConstantFormatters(Program& program)
{
    ConstantFormatterSpecialization(program).run();
}

std::optional<Writes> writesOfInlineIntlTemporal(InlineBuiltin which, std::span<const Type> arguments)
{
    if (isLocaleStringMethod(which))
        return writesIf(arguments.size() == 1 || arguments.size() == 2, Writes::Heap);
    return which == InlineBuiltin::NewConstantFormatter ? Writes::RuntimeState : Writes::Heap;
}

std::optional<Operand> Lowering::lowerInlineIntlTemporal(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    if (isLocaleStringMethod(which)) {
        bool formatsNumber = which == InlineBuiltin::NumberToLocaleString;
        auto callFunction = [&] {
            Boxed returnedValue = callBuiltinFunction(value, OperationEffects { writes, true });
            return guaranteesResultType(m_program, m_function, m_function.at(result)) ? convert(returnedValue, type, rep) : ensure(returnedValue, type);
        };
        if (!value.child(2) || (formatsNumber && !typeOf(value.child(0)).isNumber()))
            return callFunction();
        Word32 pattern = word32(static_cast<int32_t>(m_function.at(value.child(2)).integer()));
        LBasicBlock slowPath = newBlockForGenericCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        CellPointer text;
        if (formatsNumber)
            text = AOT_TYPED_OPERATION(FormatNumberWithPattern, pattern, in<Number>(value.child(0)));
        else {
            CellPointer date = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameID));
            Number timeValue(m_out.loadDouble(m_out.address(m_heaps.AOTTyped_runtimeState, date.value(), DateInstance::offsetOfInternalNumber())));
            text = AOT_TYPED_OPERATION(FormatTimeValueWithPattern, pattern, timeValue);
        }
        branchUnless(Word32(m_out.notNull(text.value())), slowPath);
        join.add(ensure(text, m_program.at(m_program.types.string), type));
        if (!isSharedGenericPath(slowPath)) {
            m_out.appendTo(slowPath);
            join.add(callFunction());
        }
        return join.finish();
    }
    if (which == InlineBuiltin::NewConstantFormatter) {
        CellPointer formatter = AOT_TYPED_OPERATION(NewConstantFormatter, word32(static_cast<int32_t>(m_function.at(value.arguments[0]).integer())));
        return ensure(box(formatter), type);
    }
    ValueID formatter = value.arguments[0];
    ValueID argument = value.arguments[1];
    auto genericPath = [&] {
        return ensure(callMethodGeneric(in<Boxed>(formatter), m_program.nameOfFormatMethod, Arguments { argument }), type);
    };
    auto stringResult = [&](CellPointer text, LBasicBlock slowPath) {
        branchUnless(Word32(m_out.notNull(text.value())), slowPath);
        return ensure(text, m_program.at(m_program.types.string), type);
    };
    if (which == InlineBuiltin::FormatNumber || typeOf(argument).isNumber()) {
        auto& number = m_program.at(m_program.types.number);
        return ifPredictionHolds({ Expectation { argument, &number } }, rep, [&](LBasicBlock slowPath) {
            if (which == InlineBuiltin::FormatNumber)
                return stringResult(AOT_TYPED_OPERATION(FormatNumber, in<Boxed>(formatter), in<Number>(argument)), slowPath);
            return stringResult(AOT_TYPED_OPERATION(FormatDateTime, in<Boxed>(formatter), in<Number>(argument)), slowPath);
        }, genericPath);
    }
    RELEASE_ASSERT(which == InlineBuiltin::FormatDateTime);
    auto classes = m_builtins.classes();
    Type date = Type::engineClass(*std::ranges::find_if(classes, [](auto& engineClass) { return engineClass.type == JSDateType; }));
    return ifPredictionHolds({ Expectation { argument, &date, true } }, rep, [&](LBasicBlock slowPath) {
        Number timeValue(m_out.loadDouble(m_out.address(m_heaps.AOTTyped_runtimeState, in<CellPointer>(argument).value(), DateInstance::offsetOfInternalNumber())));
        return stringResult(AOT_TYPED_OPERATION(FormatDateTime, in<Boxed>(formatter), timeValue), slowPath);
    }, genericPath);
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

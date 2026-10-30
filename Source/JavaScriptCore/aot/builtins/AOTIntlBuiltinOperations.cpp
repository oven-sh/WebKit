/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTFormatPatterns.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRemovableFeature.h"
#include "AOTVMState.h"
#include "DateInstance.h"
#include "DeferTermination.h"
#include "IntlDateTimeFormat.h"
#include "IntlNumberFormat.h"
#include "IntlObjectInlines.h"
#include "ObjectConstructor.h"
#include <atomic>
#include <unicode/icudataver.h>
#include <unicode/ucal.h>
#include <unicode/ulocdata.h>
#include <unicode/uversion.h>
#include <wtf/DateMath.h>
#include <wtf/HashMap.h>
#include <wtf/TimeZone.h>

#if OS(DARWIN)
#include <sys/sysctl.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

void IntlDateTimeFormat::setCompiledPattern(const AOT::FormatPatternHeader* pattern, AOT::FormatPatternUse use)
{
    m_compiledPattern = pattern;
    m_compiledPatternUse = use;
    m_timeZoneIDAtCreation = static_cast<uint32_t>(WTF::lastTimeZoneID());
}

void IntlDateTimeFormat::initializeIfDeferred(JSGlobalObject* globalObject)
{
    if (!m_impl && m_compiledPattern) [[unlikely]]
        AOT::initializeFromCompiledPattern(globalObject, *this);
}

void IntlNumberFormat::initializeIfDeferred(JSGlobalObject* globalObject)
{
    if (!m_numberFormatter && m_compiledPattern) [[unlikely]]
        AOT::initializeFromCompiledPattern(globalObject, *this);
}

namespace AOT {

struct FormatPatternState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FormatPatternState);
    // For the calls of toLocaleString, which have no formatter to keep it in.
    UncheckedKeyHashMap<const FormatPatternHeader*, UTCOffsetOfStretch> utcOffsetsByPattern;
    // Of an Object.prototype that was found to have none of the names that a formatter reads from its options.
    StructureID objectPrototypeWithoutOptions;
};

void FormatPatternStateDeleter::operator()(FormatPatternState* state) const
{
    delete state;
}

static FormatPatternState& formatPatternStateOf(VM& vm)
{
    auto& state = vm.aotState().formatPatternState;
    if (!state) [[unlikely]]
        state.reset(new FormatPatternState);
    return *state;
}

std::optional<uint64_t> digestOfThisICU()
{
    static constexpr uint64_t notAsked = 0;
    static constexpr uint64_t notSaid = 1;
    static std::atomic<uint64_t> digestOfProcess { notAsked };
    uint64_t digest = digestOfProcess.load(std::memory_order_relaxed);
    if (digest == notAsked) [[unlikely]] {
        struct {
            uint8_t codeVersion[4];
            uint8_t dataVersion[4];
            uint8_t cldrVersion[4];
            char timeZoneDataVersion[12];
            char systemBuild[24];
        } stamp { };
        UErrorCode status = U_ZERO_ERROR;
        u_getVersion(stamp.codeVersion);
        u_getDataVersion(stamp.dataVersion, &status);
        ulocdata_getCLDRVersion(stamp.cldrVersion, &status);
        const char* timeZoneDataVersion = ucal_getTZDataVersion(&status);
        bool isSaid = U_SUCCESS(status) && strlen(timeZoneDataVersion) < sizeof(stamp.timeZoneDataVersion);
        if (isSaid)
            memcpy(stamp.timeZoneDataVersion, timeZoneDataVersion, strlen(timeZoneDataVersion));
#if OS(DARWIN)
        // ICU belongs to the system there, and a system may change its data and keep its versions.
        size_t size = sizeof(stamp.systemBuild);
        isSaid = isSaid && !sysctlbyname("kern.osversion", stamp.systemBuild, &size, nullptr, 0);
#endif
        digest = 14695981039346656037ull;
        for (uint8_t byte : asByteSpan(stamp))
            digest = (digest ^ byte) * 1099511628211ull;
        if (!isSaid || digest == notAsked)
            digest = notSaid;
        digestOfProcess.store(digest, std::memory_order_relaxed);
    }
    if (digest == notSaid)
        return std::nullopt;
    return digest;
}

static ALWAYS_INLINE bool isMadeByThisICU(const FormatPatternHeader& header)
{
    std::optional<uint64_t> digest = digestOfThisICU();
    return digest && header.digestOfICU[0] == static_cast<uint32_t>(*digest) && header.digestOfICU[1] == static_cast<uint32_t>(*digest >> 32);
}

// A literal inherits from Object.prototype, and a formatter asks its options for each of these names.
static NEVER_INLINE bool findsNoOptionOnObjectPrototype(JSGlobalObject* globalObject, Structure* structure)
{
    static constexpr ASCIILiteral names[] = {
        "calendar"_s, "compactDisplay"_s, "currency"_s, "currencyDisplay"_s, "currencySign"_s, "dateStyle"_s, "day"_s, "dayPeriod"_s, "era"_s,
        "formatMatcher"_s, "fractionalSecondDigits"_s, "hour"_s, "hour12"_s, "hourCycle"_s, "localeMatcher"_s, "maximumFractionDigits"_s,
        "maximumSignificantDigits"_s, "minimumFractionDigits"_s, "minimumIntegerDigits"_s, "minimumSignificantDigits"_s, "minute"_s, "month"_s,
        "notation"_s, "numberingSystem"_s, "roundingIncrement"_s, "roundingMode"_s, "roundingPriority"_s, "second"_s, "signDisplay"_s, "style"_s,
        "timeStyle"_s, "timeZone"_s, "timeZoneName"_s, "trailingZeroDisplay"_s, "unit"_s, "unitDisplay"_s, "useGrouping"_s, "weekday"_s, "year"_s,
    };
    VM& vm = globalObject->vm();
    for (ASCIILiteral name : names) {
        if (structure->get(vm, Identifier::fromString(vm, name)) != invalidOffset)
            return false;
    }
    if (!structure->isDictionary())
        formatPatternStateOf(vm).objectPrototypeWithoutOptions = structure->id();
    return true;
}

static ALWAYS_INLINE bool readsOptionsAsBuildDid(JSGlobalObject* globalObject, const FormatPatternHeader& header)
{
    if (!header.readsOptions)
        return true;
    Structure* structure = globalObject->objectPrototype()->structure();
    if (formatPatternStateOf(globalObject->vm()).objectPrototypeWithoutOptions == structure->id()) [[likely]]
        return true;
    return findsNoOptionOnObjectPrototype(globalObject, structure);
}

template<typename CharacterType, typename Write>
static ALWAYS_INLINE String writeWithPattern(const FormatPatternHeader& header, const Write& write)
{
    CharacterType buffer[maxFormatPatternLength];
    CharacterType* end = write(buffer, reinterpret_cast<const CharacterType*>(&header + 1));
    if (!end) [[unlikely]]
        return { };
    return String(std::span<const CharacterType> { buffer, end });
}

String formatDateTimeWithPattern(const FormatPatternHeader& header, const std::optional<UTCDateTime>& fields)
{
    if (!fields) [[unlikely]]
        return { };
    auto write = [&](auto* cursor, auto* code) {
        return writeDateTime(cursor, code, *fields);
    };
    return header.is16Bit ? writeWithPattern<char16_t>(header, write) : writeWithPattern<Latin1Character>(header, write);
}

String formatNumberWithPattern(const FormatPatternHeader& header, double number)
{
    auto write = [&](auto* cursor, auto* code) {
        return writeNumber(cursor, code, number);
    };
    return header.is16Bit ? writeWithPattern<char16_t>(header, write) : writeWithPattern<Latin1Character>(header, write);
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

namespace {

class FormatterArgumentsReader {
public:
    explicit FormatterArgumentsReader(const FormatPatternHeader& header)
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

} // anonymous namespace

static String timeZoneNamedBy(const FormatPatternHeader& header)
{
    FormatterArgumentsReader reader(header);
    reader.readText();
    if (!reader.readByte())
        return { };
    for (unsigned count = reader.readByte(); count--;) {
        auto kind = static_cast<FormatterOptionKind>(reader.readByte());
        String name = reader.readText();
        switch (kind) {
        case FormatterOptionKind::String:
            if (String text = reader.readText(); name == "timeZone"_s)
                return text;
            break;
        case FormatterOptionKind::Boolean:
            reader.readByte();
            break;
        case FormatterOptionKind::Number:
            reader.readNumber();
            break;
        }
    }
    return { };
}

static NEVER_INLINE bool findUTCOffset(UTCOffsetOfStretch& stretch, const FormatPatternHeader& header, double timeValue)
{
    if (!(std::abs(timeValue) <= maxTimeValue))
        return false;
    UErrorCode status = U_ZERO_ERROR;
    String timeZone = timeZoneNamedBy(header);
    auto characters = StringView(timeZone).upconvertedCharacters();
    auto calendar = std::unique_ptr<UCalendar, ICUDeleter<ucal_close>>(ucal_open(characters.get(), timeZone.length(), "", UCAL_GREGORIAN, &status));
    if (U_FAILURE(status))
        return false;
    ucal_setMillis(calendar.get(), timeValue, &status);
    double offset = ucal_get(calendar.get(), UCAL_ZONE_OFFSET, &status);
    offset += ucal_get(calendar.get(), UCAL_DST_OFFSET, &status);
    UDate previous = 0;
    UDate next = 0;
    bool hasPrevious = ucal_getTimeZoneTransitionDate(calendar.get(), UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE, &previous, &status);
    bool hasNext = ucal_getTimeZoneTransitionDate(calendar.get(), UCAL_TZ_TRANSITION_NEXT, &next, &status);
    if (U_FAILURE(status))
        return false;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    stretch.start = hasPrevious ? previous : -infinity;
    stretch.end = hasNext ? next : infinity;
    stretch.offset = offset;
    return true;
}

String formatInNamedTimeZone(UTCOffsetOfStretch& stretch, const FormatPatternHeader& header, double timeValue)
{
    if (!(timeValue >= stretch.start && timeValue < stretch.end)) [[unlikely]] {
        if (!findUTCOffset(stretch, header, timeValue))
            return { };
    }
    return formatDateTimeWithPattern(header, fieldsAtUTCOffset(timeValue, stretch.offset));
}

static String tryFormatWithCompiledPattern(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter, double timeValue)
{
    auto* header = formatter.compiledPattern();
    if (!header || formatter.compiledPatternUse() != FormatPatternUse::Formats) [[unlikely]]
        return { };
    switch (header->timeZone) {
    case FormatPatternTimeZone::FixedOffset:
        return formatDateTimeWithPattern(*header, fieldsForDateTimePattern(*header, timeValue));
    case FormatPatternTimeZone::Default:
        // A formatter keeps the time zone that it was made in.
        if (formatter.timeZoneIDAtCreation() != static_cast<uint32_t>(WTF::lastTimeZoneID())) [[unlikely]]
            return { };
        return formatDateTimeWithPattern(*header, fieldsInDefaultTimeZone(globalObject->vm(), timeValue));
    case FormatPatternTimeZone::Named:
        return formatInNamedTimeZone(formatter.utcOffsetOfStretch(), *header, timeValue);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String tryFormatWithCompiledPattern(JSGlobalObject*, IntlNumberFormat& formatter, double number)
{
    auto* header = formatter.compiledPattern();
    if (!header || formatter.compiledPatternUse() != FormatPatternUse::Formats) [[unlikely]]
        return { };
    return formatNumberWithPattern(*header, number);
}

JSValue formatWithCompiledPattern(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (IntlDateTimeFormat::isTemporalObject(value)) {
        formatter.initializeIfDeferred(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, formatter.format(globalObject, value));
    }
    // It looks at the formatter only for an object of Temporal. The value is converted once.
    auto record = IntlDateTimeFormat::handleDateTimeValue(globalObject, &formatter, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (String text = tryFormatWithCompiledPattern(globalObject, formatter, record.value); !text.isNull())
        return jsString(vm, WTF::move(text));
    formatter.initializeIfDeferred(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, formatter.format(globalObject, record.value));
}

// Where the formatter is made, its options are what the literal of the program is. Later they are what the literal was then: nothing was inherited, or no set-up would have been put off.
enum class OptionsInherit : bool { Nothing, FromObjectPrototype };

static std::pair<JSValue, JSValue> formatterArgumentsOf(JSGlobalObject* globalObject, const FormatPatternHeader& header, OptionsInherit inherit)
{
    VM& vm = globalObject->vm();
    FormatterArgumentsReader reader(header);
    JSValue locale = jsString(vm, reader.readText());
    if (!reader.readByte())
        return { locale, jsUndefined() };
    JSObject* options = inherit == OptionsInherit::FromObjectPrototype ? constructEmptyObject(globalObject) : constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    for (unsigned count = reader.readByte(); count--;) {
        auto kind = static_cast<FormatterOptionKind>(reader.readByte());
        Identifier name = Identifier::fromString(vm, reader.readText());
        JSValue value;
        switch (kind) {
        case FormatterOptionKind::String:
            value = jsString(vm, reader.readText());
            break;
        case FormatterOptionKind::Boolean:
            value = jsBoolean(reader.readByte());
            break;
        case FormatterOptionKind::Number:
            value = jsNumber(reader.readNumber());
            break;
        }
        options->putDirect(vm, name, value);
    }
    return { locale, options };
}

static void initializeFromCompiledPattern(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter, OptionsInherit inherit)
{
    auto [locale, options] = formatterArgumentsOf(globalObject, *formatter.compiledPattern(), inherit);
    uint8_t requiredComponentAndDefaults = formatter.compiledPattern()->requiredComponentAndDefaults;
    formatter.initializeDateTimeFormat(globalObject, locale, options, static_cast<IntlDateTimeFormat::RequiredComponent>(requiredComponentAndDefaults >> 4), static_cast<IntlDateTimeFormat::Defaults>(requiredComponentAndDefaults & 0xf));
}

static void initializeFromCompiledPattern(JSGlobalObject* globalObject, IntlNumberFormat& formatter, OptionsInherit inherit)
{
    auto [locale, options] = formatterArgumentsOf(globalObject, *formatter.compiledPattern(), inherit);
    formatter.initializeNumberFormat(globalObject, locale, options);
}

void initializeFromCompiledPattern(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter)
{
    initializeFromCompiledPattern(globalObject, formatter, OptionsInherit::Nothing);
}

void initializeFromCompiledPattern(JSGlobalObject* globalObject, IntlNumberFormat& formatter)
{
    initializeFromCompiledPattern(globalObject, formatter, OptionsInherit::Nothing);
}

// The function is bound to the formatter: what the formatter has got under a name since makes no difference to it.
template<typename Formatter>
static ALWAYS_INLINE JSCell* tryFormat(JSGlobalObject* globalObject, EncodedJSValue encodedFormatter, double value)
{
    auto* formatter = dynamicDowncast<Formatter>(JSValue::decode(encodedFormatter));
    if (!formatter) [[unlikely]]
        return nullptr;
    VM& vm = globalObject->vm();
    if (String text = tryFormatWithCompiledPattern(globalObject, *formatter, value); !text.isNull())
        return jsString(vm, WTF::move(text));
    auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    formatter->initializeIfDeferred(globalObject);
    JSValue text = topScope.exception() ? JSValue() : formatter->format(globalObject, value);
    if (topScope.exception()) [[unlikely]] {
        topScope.clearExceptionExceptTermination();
        return nullptr;
    }
    return text.asCell();
}

static JSCell* tryFormatDateTime(JSGlobalObject* globalObject, EncodedJSValue formatter, double timeValue)
{
    double clippedTimeValue = WTF::timeClip(timeValue);
    if (!std::isfinite(clippedTimeValue)) [[unlikely]]
        return nullptr;
    return tryFormat<IntlDateTimeFormat>(globalObject, formatter, clippedTimeValue);
}

static JSCell* tryFormatNumber(JSGlobalObject* globalObject, EncodedJSValue formatter, double number)
{
    return tryFormat<IntlNumberFormat>(globalObject, formatter, number);
}

static JSCell* tryFormatDateWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header, EncodedJSValue value)
{
    VM& vm = globalObject->vm();
    auto* date = dynamicDowncast<DateInstance>(JSValue::decode(value));
    if (!date || !isMadeByThisICU(*header) || !readsOptionsAsBuildDid(globalObject, *header)) [[unlikely]]
        return nullptr;
    double timeValue = date->internalNumber();
    String text;
    switch (header->timeZone) {
    case FormatPatternTimeZone::FixedOffset:
        text = formatDateTimeWithPattern(*header, fieldsForDateTimePattern(*header, timeValue));
        break;
    case FormatPatternTimeZone::Default:
        text = formatDateTimeWithPattern(*header, fieldsInDefaultTimeZone(vm, timeValue));
        break;
    case FormatPatternTimeZone::Named:
        text = formatInNamedTimeZone(formatPatternStateOf(vm).utcOffsetsByPattern.add(header, UTCOffsetOfStretch { }).iterator->value, *header, timeValue);
        break;
    }
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

static JSCell* tryFormatNumberWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header, EncodedJSValue value)
{
    JSValue number = JSValue::decode(value);
    if (!number.isNumber() || !isMadeByThisICU(*header) || !readsOptionsAsBuildDid(globalObject, *header)) [[unlikely]]
        return nullptr;
    String text = formatNumberWithPattern(*header, number.asNumber());
    return text.isNull() ? nullptr : jsString(globalObject->vm(), WTF::move(text));
}

// It does not throw. With a locale alone there is nothing to refuse, and what the build's ICU took, with nothing inherited, this same ICU takes.
// The rest is left to operationAOTSetUpFormatterWithoutPattern, which follows.
template<typename Formatter>
static ALWAYS_INLINE JSCell* newFormatterWithPattern(JSGlobalObject* globalObject, Structure* structure, const FormatPatternHeader& header, bool readsStateOfProcess)
{
    VM& vm = globalObject->vm();
    auto* formatter = Formatter::create(vm, structure);
    bool formats = isMadeByThisICU(header) && readsOptionsAsBuildDid(globalObject, header);
    formatter->setCompiledPattern(&header, formats ? FormatPatternUse::Formats : FormatPatternUse::OnlyInitializes);
    if (formats ? readsStateOfProcess : !header.readsOptions) {
        auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        initializeFromCompiledPattern(globalObject, *formatter, OptionsInherit::Nothing);
        RELEASE_ASSERT(!topScope.exception());
    }
    return formatter;
}

template<typename Formatter>
static ALWAYS_INLINE void setUpWithoutPattern(JSGlobalObject* globalObject, Formatter& formatter)
{
    if (formatter.compiledPatternUse() == FormatPatternUse::OnlyInitializes) [[unlikely]]
        initializeFromCompiledPattern(globalObject, formatter, OptionsInherit::FromObjectPrototype);
}

static void setUpDateTimeFormatWithoutPattern(JSGlobalObject* globalObject, JSCell* formatter)
{
    setUpWithoutPattern(globalObject, *uncheckedDowncast<IntlDateTimeFormat>(formatter));
}

static void setUpNumberFormatWithoutPattern(JSGlobalObject* globalObject, JSCell* formatter)
{
    setUpWithoutPattern(globalObject, *uncheckedDowncast<IntlNumberFormat>(formatter));
}

static JSCell* newDateTimeFormatWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header)
{
    return newFormatterWithPattern<IntlDateTimeFormat>(globalObject, globalObject->dateTimeFormatStructure(), *header, header->timeZone == FormatPatternTimeZone::Default);
}

static JSCell* newNumberFormatWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header)
{
    return newFormatterWithPattern<IntlNumberFormat>(globalObject, globalObject->numberFormatStructure(), *header, false);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFormatDateTime, JSCell*, (Instance* instance, EncodedJSValue formatter, double timeValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFormatDateTime);
    DeferTerminationForAWhile deferTermination(vm);
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, tryFormatDateTime>(globalObject, formatter, timeValue));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFormatNumber, JSCell*, (Instance* instance, EncodedJSValue formatter, double number))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFormatNumber);
    DeferTerminationForAWhile deferTermination(vm);
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, tryFormatNumber>(globalObject, formatter, number));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFormatWithPattern, EncodedJSValue, (Instance* instance, uint32_t request, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFormatWithPattern);
    const FormatPatternHeader* header = &requestAt<FormatPatternHeader>(instance, request);
    JSCell* text = header->kind == FormatPatternKind::Number
        ? callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, tryFormatNumberWithPattern>(globalObject, header, value)
        : callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, tryFormatDateWithPattern>(globalObject, header, value);
    OPERATION_RETURN(scope, JSValue::encode(text ? JSValue(text) : jsNull()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetUpFormatterWithoutPattern, void, (Instance* instance, JSCell* formatter))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetUpFormatterWithoutPattern);
    if (formatter->inherits<IntlNumberFormat>())
        callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, setUpNumberFormatWithoutPattern>(globalObject, formatter);
    else
        callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, setUpDateTimeFormatWithoutPattern>(globalObject, formatter);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewFormatterWithPattern, JSCell*, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewFormatterWithPattern);
    const FormatPatternHeader* header = &requestAt<FormatPatternHeader>(instance, request);
    if (header->kind == FormatPatternKind::Number)
        OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, newNumberFormatWithPattern>(globalObject, header));
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, newDateTimeFormatWithPattern>(globalObject, header));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

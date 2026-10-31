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
#include "IntlNumberFormatInlines.h"
#include "IntlObjectInlines.h"
#include "JSBoundFunctionInlines.h"
#include "ObjectConstructor.h"
#include "WeakInlines.h"
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

namespace JSC { namespace AOT {

struct FormatPatternState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FormatPatternState);
    UncheckedKeyHashMap<const FormatPatternHeader*, UTCOffsetOfStretch> utcOffsetsByPattern;
    // Of an Object.prototype that was found to have none of the names that a formatter reads from its options.
    Weak<Structure> objectPrototypeWithoutOptions;
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
        formatPatternStateOf(vm).objectPrototypeWithoutOptions = Weak<Structure>(structure);
    return true;
}

static ALWAYS_INLINE bool readsOptionsAsBuildDid(JSGlobalObject* globalObject, const FormatPatternHeader& header)
{
    if (!header.readsOptions)
        return true;
    Structure* structure = globalObject->objectPrototype()->structure();
    if (formatPatternStateOf(globalObject->vm()).objectPrototypeWithoutOptions.get() == structure) [[likely]]
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
    std::span<const CharacterType> written { buffer, end };
    if constexpr (std::is_same_v<CharacterType, char16_t>)
        return String(written);
    // ICU hands out 16-bit texts whatever they hold, and a program can tell: Bun.inspect breaks a list of them elsewhere.
    std::span<char16_t> destination;
    String text = String::createUninitialized(written.size(), destination);
    StringImpl::copyCharacters(destination, written);
    return text;
}

static String formatFieldsWithPattern(const FormatPatternHeader& header, const std::optional<UTCDateTime>& fields)
{
    if (!fields) [[unlikely]]
        return { };
    auto write = [&](auto* cursor, auto* code) {
        return writeDateTime(cursor, code, *fields);
    };
    return header.is16Bit ? writeWithPattern<char16_t>(header, write) : writeWithPattern<Latin1Character>(header, write);
}

String formatAtFixedOffsetWithPattern(const FormatPatternHeader* header, double timeValue)
{
    return formatFieldsWithPattern(*header, fieldsForDateTimePattern(*header, timeValue));
}

String formatNumberWithPattern(const FormatPatternHeader* header, double number)
{
    auto write = [&](auto* cursor, auto* code) {
        return writeNumber(cursor, code, number);
    };
    return header->is16Bit ? writeWithPattern<char16_t>(*header, write) : writeWithPattern<Latin1Character>(*header, write);
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

String formatInNamedTimeZoneWithPattern(UTCOffsetOfStretch* stretch, const FormatPatternHeader* header, double timeValue)
{
    if (!(timeValue >= stretch->start && timeValue < stretch->end)) [[unlikely]] {
        if (!findUTCOffset(*stretch, *header, timeValue))
            return { };
    }
    return formatFieldsWithPattern(*header, fieldsAtUTCOffset(timeValue, stretch->offset));
}

static String formatTimeValueWithPattern(VM& vm, const FormatPatternHeader& header, double timeValue)
{
    switch (header.timeZone) {
    case FormatPatternTimeZone::FixedOffset:
        return formatAtFixedOffsetWithPattern(&header, timeValue);
    case FormatPatternTimeZone::Default:
        return formatFieldsWithPattern(header, fieldsInDefaultTimeZone(vm, timeValue));
    case FormatPatternTimeZone::Named:
        return formatInNamedTimeZoneWithPattern(&formatPatternStateOf(vm).utcOffsetsByPattern.add(&header, UTCOffsetOfStretch { }).iterator->value, &header, timeValue);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::pair<JSValue, JSValue> formatterArgumentsOf(JSGlobalObject* globalObject, const FormatPatternHeader& header)
{
    VM& vm = globalObject->vm();
    FormatterArgumentsReader reader(header);
    JSValue locale = jsString(vm, reader.readText());
    if (!reader.readByte())
        return { locale, jsUndefined() };
    JSObject* options = constructEmptyObject(globalObject);
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

static void setUpFromArguments(JSGlobalObject* globalObject, IntlDateTimeFormat& formatter, const FormatPatternHeader& header)
{
    auto [locale, options] = formatterArgumentsOf(globalObject, header);
    uint8_t requiredComponentAndDefaults = header.requiredComponentAndDefaults;
    formatter.initializeDateTimeFormat(globalObject, locale, options, static_cast<IntlDateTimeFormat::RequiredComponent>(requiredComponentAndDefaults >> 4), static_cast<IntlDateTimeFormat::Defaults>(requiredComponentAndDefaults & 0xf));
}

static void setUpFromArguments(JSGlobalObject* globalObject, IntlNumberFormat& formatter, const FormatPatternHeader& header)
{
    auto [locale, options] = formatterArgumentsOf(globalObject, header);
    formatter.initializeNumberFormat(globalObject, locale, options);
}

template<typename Formatter>
static ALWAYS_INLINE JSObject* newFormatterFromArguments(JSGlobalObject* globalObject, Structure* structure, const FormatPatternHeader& header)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* formatter = Formatter::create(vm, structure);
    setUpFromArguments(globalObject, *formatter, header);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return formatter;
}

JSObject* newDateTimeFormatFromArguments(JSGlobalObject* globalObject, const FormatPatternHeader* header)
{
    return newFormatterFromArguments<IntlDateTimeFormat>(globalObject, globalObject->dateTimeFormatStructure(), *header);
}

JSObject* newNumberFormatFromArguments(JSGlobalObject* globalObject, const FormatPatternHeader* header)
{
    return newFormatterFromArguments<IntlNumberFormat>(globalObject, globalObject->numberFormatStructure(), *header);
}

// The `format` function of a formatter that has a pattern is bound to these as well as to the formatter.
enum class BoundToPattern : uint8_t {
    Header,
    TimeZoneIDAtCreation,
    MayFormat,
};
static constexpr unsigned numberOfArgumentsBoundToPattern = 3;
static_assert(numberOfArgumentsBoundToPattern <= JSBoundFunction::maxEmbeddedArgs);

struct PatternOfFormatter {
    const FormatPatternHeader* header { nullptr };
    uint64_t timeZoneIDAtCreation { 0 };
    bool mayFormat { false };
};

template<typename Formatter>
static ALWAYS_INLINE PatternOfFormatter patternOf(const Formatter& formatter)
{
    JSBoundFunction* format = formatter.boundFormat();
    if (!format || format->boundArgsLength() != numberOfArgumentsBoundToPattern)
        return { };
    PatternOfFormatter pattern;
    unsigned index = 0;
    format->forEachBoundArg([&](JSValue argument) {
        switch (static_cast<BoundToPattern>(index++)) {
        case BoundToPattern::Header:
            pattern.header = reinterpret_cast<const FormatPatternHeader*>(static_cast<uintptr_t>(argument.asNumber()));
            break;
        case BoundToPattern::TimeZoneIDAtCreation:
            pattern.timeZoneIDAtCreation = static_cast<uint64_t>(argument.asNumber());
            break;
        case BoundToPattern::MayFormat:
            pattern.mayFormat = argument.asBoolean();
            break;
        }
        return IterationStatus::Continue;
    });
    return pattern;
}

static String tryFormatWithPatternOf(JSGlobalObject* globalObject, const IntlDateTimeFormat& formatter, double timeValue)
{
    PatternOfFormatter pattern = patternOf(formatter);
    if (!pattern.mayFormat)
        return { };
    if (pattern.header->timeZone == FormatPatternTimeZone::Default && pattern.timeZoneIDAtCreation != WTF::lastTimeZoneID()) [[unlikely]]
        return { };
    return formatTimeValueWithPattern(globalObject->vm(), *pattern.header, timeValue);
}

static String tryFormatWithPatternOf(JSGlobalObject*, const IntlNumberFormat& formatter, double number)
{
    PatternOfFormatter pattern = patternOf(formatter);
    if (!pattern.mayFormat)
        return { };
    return formatNumberWithPattern(pattern.header, number);
}

static JSC_DECLARE_HOST_FUNCTION(formatDateTimeBoundToPattern);
static JSC_DECLARE_HOST_FUNCTION(formatNumberBoundToPattern);

// The pattern is read from the formatter, never from the arguments, which a caller could forge.
JSC_DEFINE_HOST_FUNCTION(formatDateTimeBoundToPattern, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* formatter = dynamicDowncast<IntlDateTimeFormat>(callFrame->thisValue());
    if (!formatter) [[unlikely]]
        return JSValue::encode(throwTypeError(globalObject, scope, "Intl.DateTimeFormat.prototype.format called on value that's not a DateTimeFormat"_s));
    JSValue value = callFrame->argument(numberOfArgumentsBoundToPattern);
    if (IntlDateTimeFormat::isTemporalObject(value))
        RELEASE_AND_RETURN(scope, JSValue::encode(formatter->format(globalObject, value)));
    auto record = IntlDateTimeFormat::handleDateTimeValue(globalObject, formatter, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (String text = tryFormatWithPatternOf(globalObject, *formatter, record.value); !text.isNull())
        return JSValue::encode(jsString(vm, WTF::move(text)));
    RELEASE_AND_RETURN(scope, JSValue::encode(formatter->format(globalObject, record.value)));
}

JSC_DEFINE_HOST_FUNCTION(formatNumberBoundToPattern, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* formatter = dynamicDowncast<IntlNumberFormat>(callFrame->thisValue());
    if (!formatter) [[unlikely]]
        return JSValue::encode(throwTypeError(globalObject, scope, "Intl.NumberFormat.prototype.format called on value that's not a NumberFormat"_s));
    auto value = toIntlMathematicalValue(globalObject, callFrame->argument(numberOfArgumentsBoundToPattern));
    RETURN_IF_EXCEPTION(scope, { });
    if (auto number = value.tryGetDouble()) {
        if (String text = tryFormatWithPatternOf(globalObject, *formatter, number.value()); !text.isNull())
            return JSValue::encode(jsString(vm, WTF::move(text)));
        RELEASE_AND_RETURN(scope, JSValue::encode(formatter->format(globalObject, number.value())));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(formatter->format(globalObject, WTF::move(value))));
}

template<typename Formatter>
static void bindFormatToPattern(JSGlobalObject* globalObject, Formatter& formatter, NativeFunction function, const FormatPatternHeader* header, bool mayFormat)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer bound;
    bound.append(jsNumber(static_cast<double>(reinterpret_cast<uintptr_t>(header))));
    bound.append(jsNumber(static_cast<double>(WTF::lastTimeZoneID())));
    bound.append(jsBoolean(mayFormat));
    JSFunction* target = JSFunction::create(vm, globalObject, 1, "format"_s, function, ImplementationVisibility::Public);
    JSBoundFunction* format = JSBoundFunction::create(vm, globalObject, target, &formatter, ArgList(bound), 1, jsEmptyString(vm), SourceTaintedOrigin::Untainted);
    RETURN_IF_EXCEPTION(scope, void());
    format->template reifyLazyPropertyIfNeeded<>(vm, globalObject, vm.propertyNames->name);
    RETURN_IF_EXCEPTION(scope, void());
    format->putDirect(vm, vm.propertyNames->name, jsEmptyString(vm), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    formatter.setBoundFormat(vm, format);
}

// Where the formatter has options, operationAOTSetUpFormatterWithOptions follows at once.
template<typename Formatter>
static ALWAYS_INLINE JSCell* newFormatterWithPattern(JSGlobalObject* globalObject, Structure* structure, const FormatPatternHeader* header, NativeFunction function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto* formatter = Formatter::create(vm, structure);
    bindFormatToPattern(globalObject, *formatter, function, header, isMadeByThisICU(*header) && readsOptionsAsBuildDid(globalObject, *header));
    if (!header->readsOptions)
        setUpFromArguments(globalObject, *formatter, *header);
    scope.releaseAssertNoException();
    return formatter;
}

static JSCell* newDateTimeFormatWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header)
{
    return newFormatterWithPattern<IntlDateTimeFormat>(globalObject, globalObject->dateTimeFormatStructure(), header, formatDateTimeBoundToPattern);
}

static JSCell* newNumberFormatWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header)
{
    return newFormatterWithPattern<IntlNumberFormat>(globalObject, globalObject->numberFormatStructure(), header, formatNumberBoundToPattern);
}

template<typename Formatter>
static void setUpWithOptions(JSGlobalObject* globalObject, JSCell* cell)
{
    auto& formatter = *uncheckedDowncast<Formatter>(cell);
    setUpFromArguments(globalObject, formatter, *patternOf(formatter).header);
}

static ALWAYS_INLINE bool hasStructure(EncodedJSValue encodedValue, Structure* structure)
{
    JSValue value = JSValue::decode(encodedValue);
    return value.isCell() && value.asCell()->structureID() == structure->id();
}

template<typename Formatter>
static ALWAYS_INLINE JSCell* tryFormat(JSGlobalObject* globalObject, EncodedJSValue encodedFormatter, Structure* structure, double value)
{
    if (!hasStructure(encodedFormatter, structure)) [[unlikely]]
        return nullptr;
    VM& vm = globalObject->vm();
    auto* formatter = uncheckedDowncast<Formatter>(JSValue::decode(encodedFormatter).asCell());
    if (String text = tryFormatWithPatternOf(globalObject, *formatter, value); !text.isNull())
        return jsString(vm, WTF::move(text));
    auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue text = formatter->format(globalObject, value);
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
    return tryFormat<IntlDateTimeFormat>(globalObject, formatter, globalObject->dateTimeFormatStructure(), clippedTimeValue);
}

static JSCell* tryFormatNumber(JSGlobalObject* globalObject, EncodedJSValue formatter, double number)
{
    return tryFormat<IntlNumberFormat>(globalObject, formatter, globalObject->numberFormatStructure(), number);
}

static JSCell* tryFormatDateWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header, EncodedJSValue value)
{
    VM& vm = globalObject->vm();
    auto* date = dynamicDowncast<DateInstance>(JSValue::decode(value));
    if (!date || !isMadeByThisICU(*header) || !readsOptionsAsBuildDid(globalObject, *header)) [[unlikely]]
        return nullptr;
    String text = formatTimeValueWithPattern(vm, *header, date->internalNumber());
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

static JSCell* tryFormatNumberWithPattern(JSGlobalObject* globalObject, const FormatPatternHeader* header, EncodedJSValue value)
{
    JSValue number = JSValue::decode(value);
    if (!number.isNumber() || !isMadeByThisICU(*header) || !readsOptionsAsBuildDid(globalObject, *header)) [[unlikely]]
        return nullptr;
    String text = formatNumberWithPattern(header, number.asNumber());
    return text.isNull() ? nullptr : jsString(globalObject->vm(), WTF::move(text));
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
    DeferTerminationForAWhile deferTermination(vm);
    const FormatPatternHeader* header = &requestAt<FormatPatternHeader>(instance, request);
    JSCell* text = header->kind == FormatPatternKind::Number
        ? callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, tryFormatNumberWithPattern>(globalObject, header, value)
        : callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, tryFormatDateWithPattern>(globalObject, header, value);
    OPERATION_RETURN(scope, JSValue::encode(text ? JSValue(text) : jsNull()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewFormatterWithPattern, JSCell*, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewFormatterWithPattern);
    DeferTerminationForAWhile deferTermination(vm);
    const FormatPatternHeader* header = &requestAt<FormatPatternHeader>(instance, request);
    if (header->kind == FormatPatternKind::Number)
        OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, newNumberFormatWithPattern>(globalObject, header));
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, newDateTimeFormatWithPattern>(globalObject, header));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetUpFormatterWithOptions, void, (Instance* instance, JSCell* formatter))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetUpFormatterWithOptions);
    if (formatter->inherits<IntlNumberFormat>())
        callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, setUpWithOptions<IntlNumberFormat>>(globalObject, formatter);
    else
        callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, setUpWithOptions<IntlDateTimeFormat>>(globalObject, formatter);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

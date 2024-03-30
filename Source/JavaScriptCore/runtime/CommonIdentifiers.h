/*
 *  Copyright (C) 2003-2023 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "Identifier.h"
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

#if USE(BUN_JSC_ADDITIONS)
#define BUN_SHADED_JSC_PROPERTY_IDENTIFIERS(macro, ...) \
    /* Object */ \
    macro(__proto__, ##__VA_ARGS__) \
    /* SuppressedError */ \
    macro(error, ##__VA_ARGS__) \
    macro(suppressed, ##__VA_ARGS__) \

#define BUN_SHARED_JSC_TYPED_ARRAY_IDENTIFIERS(macro, ...) \
    macro(DataView, ##__VA_ARGS__) \

#define BUN_SHARED_JSC_COMMON_IDENTIFIERS(macro, ...) \
    macro(console, ##__VA_ARGS__) \
    macro(decodeURI, ##__VA_ARGS__) \
    macro(decodeURIComponent, ##__VA_ARGS__) \
    macro(encodeURI, ##__VA_ARGS__) \
    macro(encodeURIComponent, ##__VA_ARGS__) \
    macro(escape, ##__VA_ARGS__) \
    macro(globalThis, ##__VA_ARGS__) \
    macro(isNaN, ##__VA_ARGS__) \
    macro(JSON, ##__VA_ARGS__) \
    macro(Math, ##__VA_ARGS__) \
    macro(Proxy, ##__VA_ARGS__) \
    macro(RangeError, ##__VA_ARGS__) \
    macro(ReferenceError, ##__VA_ARGS__) \
    macro(SuppressedError, ##__VA_ARGS__) \
    macro(SyntaxError, ##__VA_ARGS__) \
    macro(TypeError, ##__VA_ARGS__) \
    macro(URIError, ##__VA_ARGS__) \
    macro(WeakSet, ##__VA_ARGS__) \

#define BUN_SHARED_JSC_COMMON_PRIVATE_IDENTIFIERS(macro, ...) \
    macro(eval, ##__VA_ARGS__) \
    macro(parseFloat, ##__VA_ARGS__) \
    macro(parseInt, ##__VA_ARGS__) \
    macro(unescape, ##__VA_ARGS__) \
    macro(Atomics, ##__VA_ARGS__) \
    macro(BigInt, ##__VA_ARGS__) \
    macro(Boolean, ##__VA_ARGS__) \
    macro(Date, ##__VA_ARGS__) \
    macro(Error, ##__VA_ARGS__) \
    macro(EvalError, ##__VA_ARGS__) \
    macro(FinalizationRegistry, ##__VA_ARGS__) \
    macro(Function, ##__VA_ARGS__) \
    macro(Intl, ##__VA_ARGS__) \
    macro(Reflect, ##__VA_ARGS__) \
    macro(Symbol, ##__VA_ARGS__) \
    macro(WeakMap, ##__VA_ARGS__) \
    macro(WeakRef, ##__VA_ARGS__) \
    /* AsyncLocalStorage */ \
    macro(asyncContextTuple, ##__VA_ARGS__) \

#define BUN_JSC_COMMON_IDENTIFIERS(macro) \
    BUN_SHARED_JSC_COMMON_IDENTIFIERS(macro) \
    BUN_SHADED_JSC_PROPERTY_IDENTIFIERS(macro) \
    BUN_SHARED_JSC_TYPED_ARRAY_IDENTIFIERS(macro) \
    macro(BigInt64Array) \
    macro(Float64Array) \
    macro(Int8Array) \
    macro(Uint8Array) \

#define BUN_JSC_COMMON_PRIVATE_IDENTIFIERS(macro) \
    BUN_SHARED_JSC_COMMON_IDENTIFIERS(macro) \
    BUN_SHARED_JSC_COMMON_PRIVATE_IDENTIFIERS(macro) \
    BUN_SHADED_JSC_PROPERTY_IDENTIFIERS(macro) \
    BUN_SHARED_JSC_TYPED_ARRAY_IDENTIFIERS(macro) \
    /* General */ \
    macro(constructor) \
    macro(prototype) \
    /* Array */ \
    macro(join) \
    macro(lastIndexOf) \
    macro(length) \
    macro(reverse) \
    macro(slice) \
    macro(splice) \
    macro(unshift) \
    macro(valueOf) \
    /* AsyncLocalStorage */ \
    macro(asyncContextData) \
    /* Atomics */ \
    macro(and) \
    macro(compareExchange) \
    macro(exchange) \
    macro(isLockFree) \
    macro(load) \
    macro(notify) \
    macro(or) \
    macro(store) \
    macro(wait) \
    macro(xor) \
    macro(waitAsync) \
    /* BigInt */ \
    macro(asIntN) \
    macro(asUintN) \
    /* DataView */ \
    macro(getBigInt64) \
    macro(getBigUint64) \
    macro(getFloat32) \
    macro(getFloat64) \
    macro(getInt8) \
    macro(getInt16) \
    macro(getInt32) \
    macro(getUint8) \
    macro(getUint16) \
    macro(getUint32) \
    macro(setBigInt64) \
    macro(setBigUint64) \
    macro(setFloat32) \
    macro(setFloat64) \
    macro(setInt8) \
    macro(setInt16) \
    macro(setInt32) \
    macro(setUint8) \
    macro(setUint16) \
    macro(setUint32) \
    /* Date */ \
    macro(getDate) \
    macro(getDay) \
    macro(getFullYear) \
    macro(getHours) \
    macro(getMilliseconds) \
    macro(getMinutes) \
    macro(getMonth) \
    macro(getSeconds) \
    macro(getTime) \
    macro(getTimezoneOffset) \
    macro(getUTCDate) \
    macro(getUTCDay) \
    macro(getUTCFullYear) \
    macro(getUTCHours) \
    macro(getUTCMilliseconds) \
    macro(getUTCMinutes) \
    macro(getUTCMonth) \
    macro(getUTCSeconds) \
    macro(now) \
    macro(setDate) \
    macro(setFullYear) \
    macro(setHours) \
    macro(setMilliseconds) \
    macro(setMinutes) \
    macro(setMonth) \
    macro(setSeconds) \
    macro(setTime) \
    macro(setUTCDate) \
    macro(setUTCFullYear) \
    macro(setUTCHours) \
    macro(setUTCMilliseconds) \
    macro(setUTCMinutes) \
    macro(setUTCMonth) \
    macro(setUTCSeconds) \
    macro(toDateString) \
    macro(toGMTString) \
    macro(toISOString) \
    macro(toJSON) \
    macro(toLocaleDateString) \
    macro(toLocaleTimeString) \
    macro(toTimeString) \
    macro(toUTCString) \
    macro(UTC) \
    /* Error */ \
    macro(captureStackTrace) \
    macro(message) \
    macro(prepareStackTrace) \
    macro(stack) \
    macro(stackTraceLimit) \
    /* FinalizationRegistry */ \
    macro(register) \
    macro(unregister) \
    /* Function */ \
    macro(bind) \
    macro(name) \
    /* Intl */ \
    macro(Collator) \
    macro(DateTimeFormat) \
    macro(DisplayNames) \
    macro(DurationFormat) \
    macro(ListFormat) \
    macro(Locale) \
    macro(NumberFormat) \
    macro(PluralRules) \
    macro(RelativeTimeFormat) \
    macro(Segmenter) \
    macro(getCanonicalLocales) \
    macro(supportedValuesOf) \
    /* Intl.Xyz instance methods */ \
    macro(compare) \
    macro(format) \
    macro(formatRange) \
    macro(formatToParts) \
    macro(resolvedOptions) \
    macro(select) \
    macro(selectRange) \
    macro(segment) \
    macro(supportedLocalesOf) \
    /* Intl.Locale */ \
    macro(baseName) \
    macro(calendar) \
    macro(caseFirst) \
    macro(collation) \
    macro(getCalendars) \
    macro(getCollations) \
    macro(getHourCycles) \
    macro(getNumberingSystems) \
    macro(getTextInfo) \
    macro(getTimeZones) \
    macro(getWeekendInfo) \
    macro(hourCycle) \
    macro(language) \
    macro(maximize) \
    macro(minimize) \
    macro(numberingSystem) \
    macro(numeric) \
    macro(region) \
    macro(script) \
    /* Iterator */ \
    macro(drop) \
    macro(take) \
    /* JSON */ \
    macro(parse) \
    macro(stringify) \
    /* Math */ \
    macro(E) \
    macro(LN10) \
    macro(LN2) \
    macro(LOG10E) \
    macro(LOG2E) \
    macro(PI) \
    macro(SQRT1_2) \
    macro(SQRT2) \
    macro(abs) \
    macro(acos) \
    macro(acosh) \
    macro(asin) \
    macro(asinh) \
    macro(atan) \
    macro(atanh) \
    macro(atan2) \
    macro(cbrt) \
    macro(ceil) \
    macro(clz32) \
    macro(cos) \
    macro(cosh) \
    macro(exp) \
    macro(expm1) \
    macro(floor) \
    macro(fround) \
    macro(hypot) \
    macro(imul) \
    macro(log) \
    macro(log10) \
    macro(log1p) \
    macro(log2) \
    macro(max) \
    macro(pow) \
    macro(random) \
    macro(round) \
    macro(sign) \
    macro(sin) \
    macro(sinh) \
    macro(sqrt) \
    macro(tan) \
    macro(tanh) \
    macro(trunc) \
    /* Object */ \
    macro(__defineGetter__) \
    macro(__defineSetter__) \
    macro(__lookupGetter__) \
    macro(__lookupSetter__) \
    macro(assign) \
    macro(freeze) \
    macro(getOwnPropertyDescriptors) \
    macro(is) \
    macro(isFrozen) \
    macro(isPrototypeOf) \
    macro(isSealed) \
    macro(propertyIsEnumerable) \
    macro(seal) \
    /* Number */ \
    macro(EPSILON) \
    macro(MAX_VALUE) \
    macro(MIN_SAFE_INTEGER) \
    macro(MIN_VALUE) \
    macro(NaN) \
    macro(NEGATIVE_INFINITY) \
    macro(POSITIVE_INFINITY) \
    macro(isInteger) \
    macro(isSafeInteger) \
    macro(toExponential) \
    macro(toFixed) \
    macro(toPrecision) \
    /* Proxy */ \
    macro(revocable) \
    /* Reflect */ \
    macro(construct) \
    macro(isExtensible) \
    macro(ownKeys) \
    macro(preventExtensions) \
    macro(setPrototypeOf) \
    /* RegExp */ \
    macro(dotAll) \
    macro(flags) \
    macro(global) \
    macro(hasIndices) \
    macro(ignoreCase) \
    macro(multiline) \
    macro(source) \
    macro(sticky) \
    macro(unicode) \
    /* String */ \
    macro(charAt) \
    macro(codePointAt) \
    macro(fromCharCode) \
    macro(fromCodePoint) \
    macro(isWellFormed) \
    macro(localeCompare) \
    macro(normalize) \
    macro(startsWith) \
    macro(substring) \
    macro(toLowerCase) \
    macro(toUpperCase) \
    macro(toLocaleLowerCase) \
    macro(toLocaleUpperCase) \
    macro(toWellFormed) \
    macro(trim) \
    macro(trimLeft) \
    macro(trimStart) \
    macro(trimEnd) \
    macro(trimRight) \
    /* Symbol */ \
    macro($asyncDispose) \
    macro($asyncIterator) \
    macro($dispose) \
    macro($hasInstance) \
    macro($isConcatSpreadable) \
    macro($iterator) \
    macro($match) \
    macro($matchAll) \
    macro($replace) \
    macro($search) \
    macro($species) \
    macro($split) \
    macro($toPrimitive) \
    macro($toStringTag) \
    macro($unscopables) \
    macro(asyncDispose) \
    macro(asyncIterator) \
    macro(description) \
    macro(dispose) \
    macro(for) \
    macro(hasInstance) \
    macro(isConcatSpreadable) \
    macro(iterator) \
    macro(keyFor) \
    macro(species) \
    macro(toPrimitive) \
    macro(toStringTag) \
    macro(unscopables) \
    /* TypedArray */ \
    macro(BYTES_PER_ELEMENT) \
    macro(buffer) \
    macro(byteLength) \
    macro(byteOffset) \
    macro(subarray) \
    /* WeakRef */ \
    macro(deref) \

#define BUN_JSC_LINK_TIME_CONSTANTS(macro) \
    BUN_SHARED_JSC_COMMON_IDENTIFIERS(macro, nullptr) \
    BUN_SHARED_JSC_COMMON_PRIVATE_IDENTIFIERS(macro, nullptr) \
    BUN_SHARED_JSC_TYPED_ARRAY_IDENTIFIERS(macro, nullptr) \
    macro(Number, nullptr) \

#else
#define BUN_JSC_COMMON_IDENTIFIERS(macro, ...)
#define BUN_JSC_COMMON_PRIVATE_IDENTIFIERS(macro, ...)
#define BUN_JSC_LINK_TIME_CONSTANTS(macro, ...)
#endif

// MarkedArgumentBuffer of property names, passed to a macro so we can do set them up various
// ways without repeating the list.
#define JSC_COMMON_IDENTIFIERS_EACH_PROPERTY_NAME(macro) \
    BUN_JSC_COMMON_IDENTIFIERS(macro) \
    macro(Array) \
    macro(ArrayBuffer) \
    macro(Atomics) \
    macro(BYTES_PER_ELEMENT) \
    macro(BigInt) \
    macro(Boolean) \
    macro(Collator) \
    macro(DurationFormat) \
    macro(Date) \
    macro(DateTimeFormat) \
    macro(DisplayNames) \
    macro(Error) \
    macro(EvalError) \
    macro(FinalizationRegistry) \
    macro(Function) \
    macro(Infinity) \
    macro(Intl) \
    macro(ListFormat) \
    macro(Loader) \
    macro(Locale) \
    macro(Map) \
    macro(NaN) \
    macro(Number) \
    macro(NumberFormat) \
    macro(Object) \
    macro(PluralRules) \
    macro(Promise) \
    macro(ShadowRealm) \
    macro(Reflect) \
    macro(RegExp) \
    macro(RelativeTimeFormat) \
    macro(RemotePlayback) \
    macro(Segmenter) \
    macro(Set) \
    macro(SharedArrayBuffer) \
    macro(String) \
    macro(Symbol) \
    macro(Temporal) \
    macro(WeakRef) \
    macro(__defineGetter__) \
    macro(__defineSetter__) \
    macro(__lookupGetter__) \
    macro(__lookupSetter__) \
    macro(add) \
    macro(additionalJettisonReason) \
    macro(anonymous) \
    macro(arguments) \
    macro(as) \
    macro(async) \
    macro(back) \
    macro(bind) \
    macro(byteLength) \
    macro(byteOffset) \
    macro(bytecode) \
    macro(bytecodeIndex) \
    macro(bytecodes) \
    macro(bytecodesID) \
    macro(calendar) \
    macro(callee) \
    macro(caller) \
    macro(captureStackTrace) \
    macro(caseFirst) \
    macro(cause) \
    macro(clear) \
    macro(collation) \
    macro(column) \
    macro(compilationKind) \
    macro(compilationUID) \
    macro(compilations) \
    macro(compile) \
    macro(configurable) \
    macro(constructor) \
    macro(count) \
    macro(counters) \
    macro(dateStyle) \
    macro(day) \
    macro(days) \
    macro(daysDisplay) \
    macro(dayPeriod) \
    macro(defineProperty) \
    macro(deref) \
    macro(description) \
    macro(descriptions) \
    macro(detached) \
    macro(detail) \
    macro(displayName) \
    macro(done) \
    macro(dotAll) \
    macro(enumerable) \
    macro(era) \
    macro(eraYear) \
    macro(errors) \
    macro(eval) \
    macro(events) \
    macro(exec) \
    macro(executionCount) \
    macro(exitKind) \
    macro(exports) \
    macro(fallback) \
    macro(flags) \
    macro(forEach) \
    macro(formatMatcher) \
    macro(formatToParts) \
    macro(forward) \
    macro(fractionalDigits) \
    macro(fractionalSecondDigits) \
    macro(from) \
    macro(fromCharCode) \
    macro(get) \
    macro(getOwnPropertyDescriptor) \
    macro(global) \
    macro(go) \
    macro(granularity) \
    macro(groups) \
    macro(grow) \
    macro(growable) \
    macro(has) \
    macro(hasIndices) \
    macro(hasOwn) \
    macro(hasOwnProperty) \
    macro(hash) \
    macro(header) \
    macro(hour) \
    macro(hours) \
    macro(hoursDisplay) \
    macro(hourCycle) \
    macro(hour12) \
    macro(id) \
    macro(ignoreCase) \
    macro(ignorePunctuation) \
    macro(index) \
    macro(indices) \
    macro(inferredName) \
    macro(input) \
    macro(isoDay) \
    macro(isoHour) \
    macro(isoMicrosecond) \
    macro(isoMillisecond) \
    macro(isoMinute) \
    macro(isoMonth) \
    macro(isoNanosecond) \
    macro(isoSecond) \
    macro(isoYear) \
    macro(instructionCount) \
    macro(isArray) \
    macro(isEnabled) \
    macro(isPrototypeOf) \
    macro(isView) \
    macro(isWatchpoint) \
    macro(isWellFormed) \
    macro(isWordLike) \
    macro(jettisonReason) \
    macro(join) \
    macro(language) \
    macro(languageDisplay) \
    macro(largestUnit) \
    macro(lastIndex) \
    macro(length) \
    macro(line) \
    macro(locale) \
    macro(localeMatcher) \
    macro(maxByteLength) \
    macro(maximumFractionDigits) \
    macro(maximumSignificantDigits) \
    macro(message) \
    macro(microsecond) \
    macro(microseconds) \
    macro(microsecondsDisplay) \
    macro(millisecond) \
    macro(milliseconds) \
    macro(millisecondsDisplay) \
    macro(minimumFractionDigits) \
    macro(minimumIntegerDigits) \
    macro(minimumSignificantDigits) \
    macro(minute) \
    macro(minutes) \
    macro(minutesDisplay) \
    macro(month) \
    macro(monthCode) \
    macro(months) \
    macro(monthsDisplay) \
    macro(multiline) \
    macro(name) \
    macro(nanosecond) \
    macro(nanoseconds) \
    macro(nanosecondsDisplay) \
    macro(next) \
    macro(now) \
    macro(numInlinedCalls) \
    macro(numInlinedGetByIds) \
    macro(numInlinedPutByIds) \
    macro(numberingSystem) \
    macro(numeric) \
    macro(of) \
    macro(opcode) \
    macro(origin) \
    macro(osrExitSites) \
    macro(osrExits) \
    macro(overflow) \
    macro(ownKeys) \
    macro(parse) \
    macro(parseInt) \
    macro(parseFloat) \
    macro(profiledBytecodes) \
    macro(propertyIsEnumerable) \
    macro(prototype) \
    macro(raw) \
    macro(region) \
    macro(replace) \
    macro(resizable) \
    macro(resize) \
    macro(resolve) \
    macro(roundingIncrement) \
    macro(roundingMode) \
    macro(roundingPriority) \
    macro(script) \
    macro(second) \
    macro(seconds) \
    macro(secondsDisplay) \
    macro(segment) \
    macro(selectRange) \
    macro(sensitivity) \
    macro(set) \
    macro(size) \
    macro(slice) \
    macro(smallestUnit) \
    macro(source) \
    macro(sourceCode) \
    macro(sourceURL) \
    macro(stack) \
    macro(stackTraceLimit) \
    macro(sticky) \
    macro(style) \
    macro(subarray) \
    macro(summary) \
    macro(target) \
    macro(test) \
    macro(then) \
    macro(time) \
    macro(timeStyle) \
    macro(timeZone) \
    macro(timeZoneName) \
    macro(toExponential) \
    macro(toFixed) \
    macro(toISOString) \
    macro(toJSON) \
    macro(toLocaleString) \
    macro(toPrecision) \
    macro(toString) \
    macro(toTemporalInstant) \
    macro(toWellFormed) \
    macro(trailingZeroDisplay) \
    macro(transfer) \
    macro(transferToFixedLength) \
    macro(type) \
    macro(uid) \
    macro(unicode) \
    macro(unicodeSets) \
    macro(unit) \
    macro(usage) \
    macro(value) \
    macro(valueOf) \
    macro(week) \
    macro(weekday) \
    macro(weeks) \
    macro(weeksDisplay) \
    macro(writable) \
    macro(year) \
    macro(years) \
    macro(yearsDisplay) \

#define JSC_COMMON_IDENTIFIERS_EACH_PRIVATE_FIELD(macro) \
    macro(constructor)

#define JSC_COMMON_IDENTIFIERS_EACH_KEYWORD(macro) \
    macro(await) \
    macro(break) \
    macro(case) \
    macro(catch) \
    macro(class) \
    macro(const) \
    macro(continue) \
    macro(debugger) \
    macro(default) \
    macro(delete) \
    macro(do) \
    macro(else) \
    macro(enum) \
    macro(export) \
    macro(extends) \
    macro(false) \
    macro(finally) \
    macro(for) \
    macro(function) \
    macro(if) \
    macro(implements) \
    macro(import) \
    macro(in) \
    macro(instanceof) \
    macro(interface) \
    macro(let) \
    macro(new) \
    macro(null) \
    macro(package) \
    macro(private) \
    macro(protected) \
    macro(public) \
    macro(return) \
    macro(static) \
    macro(super) \
    macro(switch) \
    macro(this) \
    macro(throw) \
    macro(true) \
    macro(try) \
    macro(typeof) \
    macro(undefined) \
    macro(var) \
    macro(void) \
    macro(while) \
    macro(with) \
    macro(yield)

#define JSC_COMMON_PRIVATE_IDENTIFIERS_EACH_WELL_KNOWN_SYMBOL(macro) \
    macro(hasInstance) \
    macro(isConcatSpreadable) \
    macro(asyncIterator) \
    macro(iterator) \
    macro(match) \
    macro(matchAll) \
    macro(replace) \
    macro(search) \
    macro(species) \
    macro(split) \
    macro(toPrimitive) \
    macro(toStringTag) \
    macro(unscopables) \
    macro(dispose) \
    macro(asyncDispose)


#define JSC_PARSER_PRIVATE_NAMES(macro) \
    macro(generator) \
    macro(generatorState) \
    macro(generatorValue) \
    macro(generatorResumeMode) \
    macro(generatorFrame) \
    macro(meta) \
    macro(starDefault) \
    macro(starNamespace) \
    macro(undefined) \

namespace JSC {
    
    class BuiltinNames;
    
    class CommonIdentifiers {
        WTF_MAKE_NONCOPYABLE(CommonIdentifiers);
        WTF_MAKE_TZONE_ALLOCATED(CommonIdentifiers);
    private:
        CommonIdentifiers(VM&);
        ~CommonIdentifiers();
        friend class VM;
        
    public:
        const BuiltinNames& builtinNames() const { return *m_builtinNames; }
        const Identifier nullIdentifier;
        const Identifier emptyIdentifier;
        const Identifier underscoreProto;
        const Identifier useStrictIdentifier;
        const Identifier timesIdentifier;
    private:
        std::unique_ptr<BuiltinNames> m_builtinNames;

    public:

#define JSC_IDENTIFIER_DECLARE_PARSER_PRIVATE_NAME(name) const Identifier name##PrivateName;
        JSC_PARSER_PRIVATE_NAMES(JSC_IDENTIFIER_DECLARE_PARSER_PRIVATE_NAME)
#undef JSC_IDENTIFIER_DECLARE_PARSER_PRIVATE_NAME
        
#define JSC_IDENTIFIER_DECLARE_KEYWORD_NAME_GLOBAL(name) const Identifier name##Keyword;
        JSC_COMMON_IDENTIFIERS_EACH_KEYWORD(JSC_IDENTIFIER_DECLARE_KEYWORD_NAME_GLOBAL)
#undef JSC_IDENTIFIER_DECLARE_KEYWORD_NAME_GLOBAL
        
#define JSC_IDENTIFIER_DECLARE_PROPERTY_NAME_GLOBAL(name) const Identifier name;
        JSC_COMMON_IDENTIFIERS_EACH_PROPERTY_NAME(JSC_IDENTIFIER_DECLARE_PROPERTY_NAME_GLOBAL)
#undef JSC_IDENTIFIER_DECLARE_PROPERTY_NAME_GLOBAL

#define JSC_IDENTIFIER_DECLARE_PRIVATE_WELL_KNOWN_SYMBOL_GLOBAL(name) const Identifier name##Symbol;
        JSC_COMMON_PRIVATE_IDENTIFIERS_EACH_WELL_KNOWN_SYMBOL(JSC_IDENTIFIER_DECLARE_PRIVATE_WELL_KNOWN_SYMBOL_GLOBAL)
#undef JSC_IDENTIFIER_DECLARE_PRIVATE_WELL_KNOWN_SYMBOL_GLOBAL
        const Identifier intlLegacyConstructedSymbol;

#define JSC_IDENTIFIER_DECLARE_PRIVATE_FIELD_GLOBAL(name) const Identifier name##PrivateField;
        JSC_COMMON_IDENTIFIERS_EACH_PRIVATE_FIELD(JSC_IDENTIFIER_DECLARE_PRIVATE_FIELD_GLOBAL)
#undef JSC_IDENTIFIER_DECLARE_PRIVATE_FIELD_GLOBAL

        // Callers of this method should make sure that identifiers given to this method 
        // survive the lifetime of CommonIdentifiers and related VM.
        JS_EXPORT_PRIVATE void appendExternalName(const Identifier& publicName, const Identifier& privateName);
    };

} // namespace JSC

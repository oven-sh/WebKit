#pragma once

#if USE(BUN_JSC_ADDITIONS)
#define BUN_SHADED_JSC_PROPERTY_IDENTIFIERS(macro, ...) \
    /* Bun_SuppressedError */ \
    macro(error, ##__VA_ARGS__) \
    macro(suppressed, ##__VA_ARGS__) \
    /* Object */ \
    macro(__proto__, ##__VA_ARGS__) \

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
    /* Bun AsyncLocalStorage */ \
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
    /* bun.js/bindings/webcore/ReadableStream */ \
    macro(asyncContextData) \
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

#define BUN_JSC_WELL_KNOWN_SYMBOLS(macro) \
    macro(dispose) \
    macro(asyncDispose) \

#define BUN_JSC_LINK_TIME_CONSTANTS(macro) \
    BUN_SHARED_JSC_COMMON_IDENTIFIERS(macro, nullptr) \
    BUN_SHARED_JSC_COMMON_PRIVATE_IDENTIFIERS(macro, nullptr) \
    BUN_SHARED_JSC_TYPED_ARRAY_IDENTIFIERS(macro, nullptr) \
    macro(Number, nullptr) \

#else
#define BUN_JSC_COMMON_IDENTIFIERS(macro)
#define BUN_JSC_COMMON_PRIVATE_IDENTIFIERS(macro)
#define BUN_JSC_LINK_TIME_CONSTANTS(macro)
#define BUN_JSC_WELL_KNOWN_SYMBOLS(macro)
#endif

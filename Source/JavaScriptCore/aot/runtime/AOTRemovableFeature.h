/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AOTBuiltinFeatures.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <wtf/Platform.h>
#include <wtf/ThreadSafetyAnalysis.h>

#if COMPILER(CLANG)
#define JSC_CALL_WITHOUT_INLINING [[clang::noinline]]
#else
#define JSC_CALL_WITHOUT_INLINING
#endif

namespace JSC {

class JSGlobalObject;
class JSValue;
typedef int64_t EncodedJSValue;

#define JSC_DECLARE_REMOVABLE_FEATURE(name, index) name = index,
enum class RemovableFeature : uint16_t {
    FOR_EACH_ENGINE_REMOVABLE_FEATURE(JSC_DECLARE_REMOVABLE_FEATURE)
};
#undef JSC_DECLARE_REMOVABLE_FEATURE

inline constexpr unsigned firstEmbedderRemovableFeature = 128;
inline constexpr unsigned removableFeatureIndexLimit = 512;

#define JSC_CHECK_REMOVABLE_FEATURE(name, index) static_assert(index < firstEmbedderRemovableFeature);
FOR_EACH_ENGINE_REMOVABLE_FEATURE(JSC_CHECK_REMOVABLE_FEATURE)
#undef JSC_CHECK_REMOVABLE_FEATURE

enum class RemovedFeatureStub : uint8_t { Abort, Throw, ReturnUndefined, ReturnDefault };
enum class FeatureRemoval : uint8_t { Present, Unused, Requested };
// Builtin: the record has the number of a built-in in the table. BuiltinNamedByEngine: it has the position of one in the engine's list of names, which is all that code compiled with the engine knows.
enum class GuardOrigin : uint8_t { NativeCode, Builtin, Definition, BuiltinNamedByEngine };

struct RemovableFeatureState {
    uint8_t magic[8];
    uint32_t isTrimmed;
    uint32_t removedRangeCount;
    int64_t removedRangesOffset;
    uint8_t removal[removableFeatureIndexLimit];
};
static_assert(sizeof(RemovableFeatureState) == 536);
static_assert(offsetof(RemovableFeatureState, removal) == 24);

struct RemovedRange {
    int64_t start;
    uint32_t size;
    uint16_t feature;
    uint16_t reserved;
};
static_assert(sizeof(RemovedRange) == 16 && alignof(RemovedRange) == 8);

AOT_THRESHOLD(uint32_t, maxRemovedRanges, 32768);

struct RemovableEntryPointRecord {
    uint64_t guardSite;
    uint64_t entryPoint;
    uint16_t feature;
    uint8_t stub;
    uint8_t origin;
    uint32_t builtin;
};
static_assert(sizeof(RemovableEntryPointRecord) == 24);
static_assert(offsetof(RemovableEntryPointRecord, origin) == 19 && offsetof(RemovableEntryPointRecord, builtin) == 20);

extern "C" const RemovableFeatureState Bun__removableFeatureState;
extern "C" [[noreturn]] void Bun__abortForRemovedFeature(uint16_t feature);
extern "C" EncodedJSValue Bun__throwForRemovedFeature(JSGlobalObject*, uint16_t feature);
extern "C" EncodedJSValue Bun__throwForRemovedBuiltin(JSGlobalObject*, uint16_t feature, uint32_t builtin);
// Without a name the error names the feature.
extern "C" EncodedJSValue Bun__throwForRemovedFunction(JSGlobalObject*, uint16_t feature, StringImpl* name);

ALWAYS_INLINE FeatureRemoval removalOf(RemovableFeature feature)
{
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    return static_cast<FeatureRemoval>(Bun__removableFeatureState.removal[static_cast<uint16_t>(feature)]);
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}

ALWAYS_INLINE bool isFeatureRemoved(RemovableFeature feature)
{
    return removalOf(feature) != FeatureRemoval::Present;
}

inline std::optional<RemovableFeature> removedFeatureAt(const void* address)
{
    const RemovableFeatureState& state = Bun__removableFeatureState;
    if (!state.isTrimmed) [[likely]]
        return std::nullopt;
    if (!state.removedRangeCount || state.removedRangeCount > maxRemovedRanges || state.removedRangesOffset % static_cast<int64_t>(alignof(RemovedRange)))
        return std::nullopt;

    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    uintptr_t base = reinterpret_cast<uintptr_t>(&state);
    auto* ranges = reinterpret_cast<const RemovedRange*>(base + static_cast<uintptr_t>(state.removedRangesOffset));
    int64_t offset = static_cast<int64_t>(reinterpret_cast<uintptr_t>(address) - base);
    auto* next = std::upper_bound(ranges, ranges + state.removedRangeCount, offset, [](int64_t value, const RemovedRange& range) {
        return value < range.start;
    });
    if (next == ranges)
        return std::nullopt;
    const RemovedRange& range = next[-1];
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    if (static_cast<uint64_t>(offset) - static_cast<uint64_t>(range.start) >= range.size || range.feature >= removableFeatureIndexLimit)
        return std::nullopt;
    return static_cast<RemovableFeature>(range.feature);
}

#if COMPILER(CLANG) && (CPU(X86_64) || CPU(ARM64))

#if OS(DARWIN)
#define JSC_REMOVABLE_RECORD_SECTION ".pushsection __DWARF,__bun_entries,regular,debug\n"
#elif OS(WINDOWS)
#define JSC_REMOVABLE_RECORD_SECTION ".pushsection .bun_removable_entry_points,\"yn\"\n"
#else
#define JSC_REMOVABLE_RECORD_SECTION ".pushsection .bun_removable_entry_points,\"e\",@progbits\n"
#endif

#if CPU(X86_64)
#define JSC_REMOVABLE_RECORD_SYMBOL "Ws"
#define JSC_REMOVABLE_RECORD_NAME(operand) "%p" #operand
#else
#define JSC_REMOVABLE_RECORD_SYMBOL "S"
#define JSC_REMOVABLE_RECORD_NAME(operand) "%" #operand
#endif

#define JSC_EMIT_REMOVABLE_RECORD(label, guardSite, recordedEntryPoint, recordedOrigin, ...) \
    __asm__ volatile( \
        label JSC_REMOVABLE_RECORD_SECTION \
        ".p2align 3\n" \
        ".quad " guardSite "\n" \
        ".quad " JSC_REMOVABLE_RECORD_NAME([entryPoint]) "\n" \
        ".short %c[feature]\n" \
        ".byte %c[stub]\n" \
        ".byte %c[origin]\n" \
        ".long %c[builtin]\n" \
        ".popsection\n" \
        : \
        : [entryPoint] JSC_REMOVABLE_RECORD_SYMBOL(recordedEntryPoint), [feature] "i"(static_cast<uint16_t>(feature)), [stub] "i"(static_cast<uint8_t>(stub)), [origin] "i"(static_cast<uint8_t>(recordedOrigin)), [builtin] "i"(builtin) __VA_ARGS__)

#else
#define JSC_EMIT_REMOVABLE_RECORD(...)
#endif

template<RemovableFeature feature, GuardOrigin origin, uint32_t builtin>
inline constexpr bool isRecordOfGuard = static_cast<uint16_t>(feature) < removableFeatureIndexLimit && origin != GuardOrigin::Definition && (origin == GuardOrigin::Builtin || origin == GuardOrigin::BuiltinNamedByEngine || !builtin);

template<RemovableFeature feature, RemovedFeatureStub stub, auto entryPoint, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0>
inline __attribute__((__always_inline__)) void recordRemovableEntryPoint()
{
    static_assert(isRecordOfGuard<feature, origin, builtin>);
    JSC_EMIT_REMOVABLE_RECORD("771:\n", "771b", entryPoint, origin);
}

template<auto& table, RemovableFeature feature, RemovedFeatureStub stub, auto entryPoint, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0>
inline __attribute__((__always_inline__)) void recordRemovableTableEntry()
{
    static_assert(isRecordOfGuard<feature, origin, builtin>);
    JSC_EMIT_REMOVABLE_RECORD("", JSC_REMOVABLE_RECORD_NAME([table]), entryPoint, origin, , [table] JSC_REMOVABLE_RECORD_SYMBOL(&table));
}

template<RemovableFeature feature, auto function, RemovedFeatureStub stub = RemovedFeatureStub::Abort, uint32_t builtin = 0>
inline __attribute__((__always_inline__)) void recordRemovableDefinition()
{
    static_assert(static_cast<uint16_t>(feature) < removableFeatureIndexLimit);
    static_assert(stub == RemovedFeatureStub::Abort || stub == RemovedFeatureStub::Throw);
    static_assert(stub == RemovedFeatureStub::Throw || !builtin);
#if COMPILER(CLANG) && (CPU(X86_64) || CPU(ARM64))
    // A function that records itself stays until the linker collects sections. LLVM 23 crashes (GlobalDCE, under link-time optimization)
    // when it removes a function that the constant of __builtin_function_start still names.
    [[gnu::used]] static const void* const start = __builtin_function_start(function);
#endif
    JSC_EMIT_REMOVABLE_RECORD("771:\n", "771b", __builtin_function_start(function), GuardOrigin::Definition);
}

#undef JSC_EMIT_REMOVABLE_RECORD
#undef JSC_REMOVABLE_RECORD_SECTION
#undef JSC_REMOVABLE_RECORD_SYMBOL
#undef JSC_REMOVABLE_RECORD_NAME

template<RemovableFeature feature, GuardOrigin origin, uint32_t builtin>
ALWAYS_INLINE EncodedJSValue throwForRemovedFeature(JSGlobalObject* realm)
{
    constexpr uint16_t index = static_cast<uint16_t>(feature);
    if constexpr (origin == GuardOrigin::Builtin)
        return Bun__throwForRemovedBuiltin(realm, index, builtin);
    else
        return Bun__throwForRemovedFeature(realm, index);
}

// A stub that throws finds the realm in the first argument.
template<RemovableFeature feature, RemovedFeatureStub stub, typename Result, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0, typename... Arguments>
ALWAYS_INLINE Result resultForRemovedFeature([[maybe_unused]] Arguments&... arguments)
{
    static_assert(stub != RemovedFeatureStub::ReturnUndefined);
    if constexpr (stub == RemovedFeatureStub::Abort)
        Bun__abortForRemovedFeature(static_cast<uint16_t>(feature));
    else if constexpr (stub == RemovedFeatureStub::Throw) {
        if constexpr (std::is_same_v<Result, EncodedJSValue>)
            return throwForRemovedFeature<feature, origin, builtin>(std::get<0>(std::tie(arguments...)));
        else {
            throwForRemovedFeature<feature, origin, builtin>(std::get<0>(std::tie(arguments...)));
            return Result { };
        }
    } else if constexpr (!std::is_void_v<Result>)
        return Result { };
}

template<RemovedFeatureStub stub, typename Result, typename FirstParameter>
inline constexpr bool removedFeatureStubFitsTypes = [] {
    if constexpr (stub == RemovedFeatureStub::Abort)
        return true;
    else if constexpr (stub == RemovedFeatureStub::Throw)
        return (std::is_same_v<Result, EncodedJSValue> || std::is_same_v<Result, JSValue> || std::is_same_v<Result, bool>) && std::is_convertible_v<FirstParameter, JSGlobalObject*>;
    else if constexpr (stub == RemovedFeatureStub::ReturnUndefined)
        return false;
    else
        return std::is_void_v<Result> || std::is_default_constructible_v<Result>;
}();

template<typename Function>
struct RemovableFunctionSignature;

template<RemovedFeatureStub stub, auto function>
concept RemovedFeatureStubFits = requires {
    typename RemovableFunctionSignature<decltype(function)>::ResultType;
} && removedFeatureStubFitsTypes<stub, typename RemovableFunctionSignature<decltype(function)>::ResultType, typename RemovableFunctionSignature<decltype(function)>::FirstParameter>;

#define JSC_DEFINE_REMOVABLE_FUNCTION_SIGNATURE(ATTRIBUTES) \
    template<typename Result, typename... Parameters, bool isNoexcept> \
    struct RemovableFunctionSignature<Result (ATTRIBUTES *)(Parameters...) noexcept(isNoexcept)> { \
        using ResultType = Result; \
        using FirstParameter = std::tuple_element_t<0, std::tuple<Parameters..., void>>; \
    };

JSC_DEFINE_REMOVABLE_FUNCTION_SIGNATURE()
#if OS(WINDOWS) && CPU(X86_64)
JSC_DEFINE_REMOVABLE_FUNCTION_SIGNATURE(SYSV_ABI)
#endif

#undef JSC_DEFINE_REMOVABLE_FUNCTION_SIGNATURE

template<RemovableFeature feature, RemovedFeatureStub stub, auto function, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0>
ALWAYS_INLINE decltype(function) removableFunction()
{
    static_assert(!std::is_member_pointer_v<decltype(function)>, "guard a member function at its definition");
    static_assert(RemovedFeatureStubFits<stub, function>);
    recordRemovableEntryPoint<feature, stub, function, origin, builtin>();
    return function;
}

template<RemovableFeature feature, RemovedFeatureStub stub, auto function, uint32_t builtin>
ALWAYS_INLINE decltype(function) removableBuiltinFunction()
{
    return removableFunction<feature, stub, function, GuardOrigin::Builtin, builtin>();
}

template<RemovableFeature feature, RemovedFeatureStub stub, auto function, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0, typename... Arguments>
ALWAYS_INLINE std::invoke_result_t<decltype(function), Arguments...> callRemovable(Arguments&&... arguments)
{
    static_assert(!std::is_member_pointer_v<decltype(function)>, "guard a member function at its definition");
    static_assert(RemovedFeatureStubFits<stub, function>);
    using Result = std::invoke_result_t<decltype(function), Arguments...>;
    if constexpr (stub != RemovedFeatureStub::Abort) {
        if (isFeatureRemoved(feature)) [[unlikely]]
            return resultForRemovedFeature<feature, stub, Result, origin, builtin>(arguments...);
    }
    recordRemovableEntryPoint<feature, stub, function, origin, builtin>();
    JSC_CALL_WITHOUT_INLINING return function(std::forward<Arguments>(arguments)...);
}

template<RemovableFeature feature, auto function, uint32_t builtin, typename... Arguments>
ALWAYS_INLINE std::invoke_result_t<decltype(function), Arguments...> callRemovableBuiltin(Arguments&&... arguments)
{
    return callRemovable<feature, RemovedFeatureStub::Throw, function, GuardOrigin::Builtin, builtin>(std::forward<Arguments>(arguments)...);
}

template<RemovableFeature feature, auto function>
ALWAYS_INLINE decltype(function) addressUnlessRemoved(decltype(function) replacement)
{
    static_assert(!std::is_member_pointer_v<decltype(function)>, "guard a member function at its definition");
    if (isFeatureRemoved(feature)) [[unlikely]]
        return replacement;
    recordRemovableEntryPoint<feature, RemovedFeatureStub::Abort, function>();
    return function;
}

// AOTRemovableFeature.cpp instantiates it for NativeFunction, GetValueFunc and PutValueFunc.
template<typename FunctionPointer>
FunctionPointer functionUnlessRemovedSlow(FunctionPointer);

// No guard names a function of a hand-written table. Every place where such a pointer becomes callable passes it through here.
template<typename FunctionPointer>
ALWAYS_INLINE FunctionPointer functionUnlessRemoved(FunctionPointer function)
{
    if (!Bun__removableFeatureState.isTrimmed) [[likely]]
        return function;
    return functionUnlessRemovedSlow(function);
}

template<RemovableFeature feature, RemovedFeatureStub stub, typename Result, typename Body>
struct RemovableBody {
    static NEVER_INLINE Result run(Body& body)
    {
        return body();
    }
};

template<RemovableFeature feature, RemovedFeatureStub stub, typename Result, typename Realm, typename Body>
ALWAYS_INLINE Result callRemovableBody(Realm realm, Body&& body)
{
    static_assert(stub != RemovedFeatureStub::Throw, "use JSC_REMOVABLE_HOST_FUNCTION_DEFINITION");
    static_assert(removedFeatureStubFitsTypes<stub, Result, Realm>);
    using EntryPoint = RemovableBody<feature, stub, Result, std::remove_reference_t<Body>>;
    if constexpr (stub != RemovedFeatureStub::Abort) {
        if (isFeatureRemoved(feature)) [[unlikely]]
            return resultForRemovedFeature<feature, stub, Result>(realm);
    }
    recordRemovableEntryPoint<feature, stub, &EntryPoint::run>();
    return EntryPoint::run(body);
}

} // namespace JSC

#define JSC_BEGIN_REMOVABLE_ENTRY_POINT(feature, stub, ...) \
    return ::JSC::callRemovableBody<::JSC::RemovableFeature::feature, ::JSC::RemovedFeatureStub::stub, __VA_ARGS__>(nullptr, [&]() ALWAYS_INLINE_LAMBDA WTF_IGNORES_THREAD_SAFETY_ANALYSIS -> __VA_ARGS__ {
#define JSC_END_REMOVABLE_ENTRY_POINT });
#define JSC_REMOVABLE_DEFINITION(feature, ...) ::JSC::recordRemovableDefinition<::JSC::RemovableFeature::feature, __VA_ARGS__>()
#define JSC_REMOVABLE_HOST_FUNCTION_DEFINITION(feature, function) ::JSC::recordRemovableDefinition<::JSC::RemovableFeature::feature, function, ::JSC::RemovedFeatureStub::Throw>()

#include "AOTBuiltinTableGuards.h"

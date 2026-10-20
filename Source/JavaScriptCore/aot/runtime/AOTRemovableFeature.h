/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AOTBuiltinFeatures.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
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
enum class FeatureRemoval : uint8_t { Present, Unused, ByName };
enum class GuardOrigin : uint8_t { NativeCode, Builtin, Definition };

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

inline constexpr uint32_t maxRemovedRanges = 32768;

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

template<RemovableFeature feature, RemovedFeatureStub stub, auto entryPoint, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0>
inline __attribute__((__always_inline__)) void recordRemovableEntryPoint()
{
    static_assert(static_cast<uint16_t>(feature) < removableFeatureIndexLimit);
    static_assert(origin != GuardOrigin::Definition);
    static_assert(origin == GuardOrigin::Builtin || !builtin);
    __asm__ volatile(
        "771:\n" JSC_REMOVABLE_RECORD_SECTION
        ".p2align 3\n"
        ".quad 771b\n"
        ".quad " JSC_REMOVABLE_RECORD_NAME(0) "\n"
        ".short %c1\n"
        ".byte %c2\n"
        ".byte %c3\n"
        ".long %c4\n"
        ".popsection\n"
        :
        : JSC_REMOVABLE_RECORD_SYMBOL(entryPoint), "i"(static_cast<uint16_t>(feature)), "i"(static_cast<uint8_t>(stub)), "i"(static_cast<uint8_t>(origin)), "i"(builtin));
}

template<auto& table, RemovableFeature feature, RemovedFeatureStub stub, auto entryPoint, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0>
inline __attribute__((__always_inline__)) void recordRemovableTableEntry()
{
    static_assert(static_cast<uint16_t>(feature) < removableFeatureIndexLimit);
    static_assert(origin != GuardOrigin::Definition);
    static_assert(origin == GuardOrigin::Builtin || !builtin);
    __asm__ volatile(
        JSC_REMOVABLE_RECORD_SECTION
        ".p2align 3\n"
        ".quad " JSC_REMOVABLE_RECORD_NAME(0) "\n"
        ".quad " JSC_REMOVABLE_RECORD_NAME(1) "\n"
        ".short %c2\n"
        ".byte %c3\n"
        ".byte %c4\n"
        ".long %c5\n"
        ".popsection\n"
        :
        : JSC_REMOVABLE_RECORD_SYMBOL(&table), JSC_REMOVABLE_RECORD_SYMBOL(entryPoint), "i"(static_cast<uint16_t>(feature)), "i"(static_cast<uint8_t>(stub)), "i"(static_cast<uint8_t>(origin)), "i"(builtin));
}

template<RemovableFeature feature, auto function, RemovedFeatureStub stub = RemovedFeatureStub::Abort, uint32_t builtin = 0>
inline __attribute__((__always_inline__)) void recordRemovableDefinition()
{
    static_assert(static_cast<uint16_t>(feature) < removableFeatureIndexLimit);
    static_assert(stub == RemovedFeatureStub::Abort || stub == RemovedFeatureStub::Throw);
    static_assert(stub == RemovedFeatureStub::Throw || !builtin);
    __asm__ volatile(
        "771:\n" JSC_REMOVABLE_RECORD_SECTION
        ".p2align 3\n"
        ".quad 771b\n"
        ".quad " JSC_REMOVABLE_RECORD_NAME(0) "\n"
        ".short %c1\n"
        ".byte %c2\n"
        ".byte %c3\n"
        ".long %c4\n"
        ".popsection\n"
        :
        : JSC_REMOVABLE_RECORD_SYMBOL(__builtin_function_start(function)), "i"(static_cast<uint16_t>(feature)), "i"(static_cast<uint8_t>(stub)), "i"(static_cast<uint8_t>(GuardOrigin::Definition)), "i"(builtin));
}

#undef JSC_REMOVABLE_RECORD_SECTION
#undef JSC_REMOVABLE_RECORD_SYMBOL
#undef JSC_REMOVABLE_RECORD_NAME

#else

template<RemovableFeature feature, RemovedFeatureStub stub, auto entryPoint, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0>
ALWAYS_INLINE void recordRemovableEntryPoint()
{
    static_assert(static_cast<uint16_t>(feature) < removableFeatureIndexLimit);
    static_assert(origin != GuardOrigin::Definition);
    static_assert(origin == GuardOrigin::Builtin || !builtin);
}

template<auto& table, RemovableFeature feature, RemovedFeatureStub stub, auto entryPoint, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0>
ALWAYS_INLINE void recordRemovableTableEntry()
{
    static_assert(static_cast<uint16_t>(feature) < removableFeatureIndexLimit);
    static_assert(origin != GuardOrigin::Definition);
    static_assert(origin == GuardOrigin::Builtin || !builtin);
}

template<RemovableFeature feature, auto function, RemovedFeatureStub stub = RemovedFeatureStub::Abort, uint32_t builtin = 0>
ALWAYS_INLINE void recordRemovableDefinition()
{
    static_assert(static_cast<uint16_t>(feature) < removableFeatureIndexLimit);
    static_assert(stub == RemovedFeatureStub::Abort || stub == RemovedFeatureStub::Throw);
    static_assert(stub == RemovedFeatureStub::Throw || !builtin);
}

#endif

template<RemovableFeature feature, GuardOrigin origin, uint32_t builtin>
ALWAYS_INLINE EncodedJSValue throwForRemovedFeature(JSGlobalObject* realm)
{
    constexpr uint16_t index = static_cast<uint16_t>(feature);
    if constexpr (origin == GuardOrigin::Builtin)
        return Bun__throwForRemovedBuiltin(realm, index, builtin);
    else
        return Bun__throwForRemovedFeature(realm, index);
}

template<RemovableFeature feature, RemovedFeatureStub stub, typename Result, GuardOrigin origin = GuardOrigin::NativeCode, uint32_t builtin = 0, typename Realm>
ALWAYS_INLINE Result resultForRemovedFeature([[maybe_unused]] Realm realm)
{
    static_assert(stub != RemovedFeatureStub::ReturnUndefined);
    if constexpr (stub == RemovedFeatureStub::Abort)
        Bun__abortForRemovedFeature(static_cast<uint16_t>(feature));
    else if constexpr (stub == RemovedFeatureStub::Throw) {
        if constexpr (std::is_same_v<Result, EncodedJSValue>)
            return throwForRemovedFeature<feature, origin, builtin>(realm);
        else {
            throwForRemovedFeature<feature, origin, builtin>(realm);
            return Result { };
        }
    } else if constexpr (!std::is_void_v<Result>)
        return Result { };
}

template<RemovedFeatureStub stub>
ALWAYS_INLINE std::nullptr_t realmForRemovedFeature()
{
    return nullptr;
}

template<RemovedFeatureStub stub, typename First, typename... Rest>
ALWAYS_INLINE auto realmForRemovedFeature([[maybe_unused]] First& first, Rest&...)
{
    if constexpr (stub == RemovedFeatureStub::Throw)
        return first;
    else
        return nullptr;
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

template<typename... Parameters>
struct RemovableFirstParameter {
    using Type = void;
};

template<typename First, typename... Rest>
struct RemovableFirstParameter<First, Rest...> {
    using Type = First;
};

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
        using FirstParameter = typename RemovableFirstParameter<Parameters...>::Type; \
    };

JSC_DEFINE_REMOVABLE_FUNCTION_SIGNATURE()
#if OS(WINDOWS) && CPU(X86_64)
JSC_DEFINE_REMOVABLE_FUNCTION_SIGNATURE(SYSV_ABI)
#endif

#undef JSC_DEFINE_REMOVABLE_FUNCTION_SIGNATURE

template<RemovableFeature feature, RemovedFeatureStub stub, auto function>
ALWAYS_INLINE decltype(function) removableFunction()
{
    static_assert(!std::is_member_pointer_v<decltype(function)>, "guard a member function at its definition");
    static_assert(RemovedFeatureStubFits<stub, function>);
    recordRemovableEntryPoint<feature, stub, function>();
    return function;
}

template<RemovableFeature feature, RemovedFeatureStub stub, auto function, uint32_t builtin>
ALWAYS_INLINE decltype(function) removableBuiltinFunction()
{
    static_assert(!std::is_member_pointer_v<decltype(function)>, "guard a member function at its definition");
    static_assert(RemovedFeatureStubFits<stub, function>);
    recordRemovableEntryPoint<feature, stub, function, GuardOrigin::Builtin, builtin>();
    return function;
}

template<RemovableFeature feature, RemovedFeatureStub stub, auto function, typename... Arguments>
ALWAYS_INLINE std::invoke_result_t<decltype(function), Arguments...> callRemovable(Arguments&&... arguments)
{
    static_assert(!std::is_member_pointer_v<decltype(function)>, "guard a member function at its definition");
    static_assert(RemovedFeatureStubFits<stub, function>);
    using Result = std::invoke_result_t<decltype(function), Arguments...>;
    if constexpr (stub != RemovedFeatureStub::Abort) {
        if (isFeatureRemoved(feature)) [[unlikely]]
            return resultForRemovedFeature<feature, stub, Result>(realmForRemovedFeature<stub>(arguments...));
    }
    recordRemovableEntryPoint<feature, stub, function>();
    JSC_CALL_WITHOUT_INLINING return function(std::forward<Arguments>(arguments)...);
}

template<RemovableFeature feature, auto function, uint32_t builtin, typename... Arguments>
ALWAYS_INLINE std::invoke_result_t<decltype(function), Arguments...> callRemovableBuiltin(Arguments&&... arguments)
{
    static_assert(!std::is_member_pointer_v<decltype(function)>, "guard a member function at its definition");
    static_assert(RemovedFeatureStubFits<RemovedFeatureStub::Throw, function>);
    using Result = std::invoke_result_t<decltype(function), Arguments...>;
    if (isFeatureRemoved(feature)) [[unlikely]]
        return resultForRemovedFeature<feature, RemovedFeatureStub::Throw, Result, GuardOrigin::Builtin, builtin>(realmForRemovedFeature<RemovedFeatureStub::Throw>(arguments...));
    recordRemovableEntryPoint<feature, RemovedFeatureStub::Throw, function, GuardOrigin::Builtin, builtin>();
    JSC_CALL_WITHOUT_INLINING return function(std::forward<Arguments>(arguments)...);
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

// AOTRemovableFeature.cpp instantiates it for NativeFunction and PutValueFunc.
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

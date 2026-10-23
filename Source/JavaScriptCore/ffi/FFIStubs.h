/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "FFICallingConvention.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "JSExportMacros.h"
#include "OperationResult.h"
#include <memory>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/PlatformCallingConventions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

#if ENABLE(FFI) && ENABLE(CODE_GENERATION) && !ENABLE(JIT_CAGE) && (CPU(X86_64) || (CPU(ARM64) && !CPU(ARM64E)))
#define FFI_STUBS_SUPPORTED 1
#else
#define FFI_STUBS_SUPPORTED 0
#endif

namespace JSC {

namespace FFI {

static constexpr unsigned maxArgumentRegistersPerClass = 8;
static_assert(integerArgumentRegisterCount(NativeCC::SysV64) <= maxArgumentRegistersPerClass && floatArgumentRegisterCount(NativeCC::SysV64) <= maxArgumentRegistersPerClass);
static_assert(integerArgumentRegisterCount(NativeCC::AAPCS64) <= maxArgumentRegistersPerClass && floatArgumentRegisterCount(NativeCC::AAPCS64) <= maxArgumentRegistersPerClass);
static_assert(integerArgumentRegisterCount(NativeCC::Win64) <= maxArgumentRegistersPerClass && floatArgumentRegisterCount(NativeCC::Win64) <= maxArgumentRegistersPerClass);

struct InvokeArguments {
    static constexpr unsigned maxStackBytes = (shadowStackBytes(NativeCC::Win64) + Signature::maxArguments * 8 + 15) & ~15u;

    static constexpr ptrdiff_t offsetOfGPR(unsigned index) { return OBJECT_OFFSETOF(InvokeArguments, gprs) + index * sizeof(uint64_t); }
    static constexpr ptrdiff_t offsetOfFPR(unsigned index) { return OBJECT_OFFSETOF(InvokeArguments, fprs) + index * sizeof(uint64_t); }
    static constexpr ptrdiff_t offsetOfReturnGPR() { return OBJECT_OFFSETOF(InvokeArguments, returnGPR); }
    static constexpr ptrdiff_t offsetOfReturnFPR() { return OBJECT_OFFSETOF(InvokeArguments, returnFPR); }
    static constexpr ptrdiff_t offsetOfStackBytes() { return OBJECT_OFFSETOF(InvokeArguments, stackBytes); }
    static constexpr ptrdiff_t offsetOfNumberOfFPRsUsed() { return OBJECT_OFFSETOF(InvokeArguments, numberOfFPRsUsed); }
    static constexpr ptrdiff_t offsetOfStack() { return OBJECT_OFFSETOF(InvokeArguments, stack); }

    uint64_t gprs[maxArgumentRegistersPerClass];
    uint64_t fprs[maxArgumentRegistersPerClass];
    uint64_t returnGPR;
    uint64_t returnFPR;
    uint32_t stackBytes;
    uint32_t numberOfFPRsUsed;
    alignas(16) uint8_t stack[maxStackBytes];
};
static_assert(!(InvokeArguments::offsetOfStack() % 16) && !(InvokeArguments::maxStackBytes % 16));

using InvokeStubFunction = void (JIT_OPERATION_ATTRIBUTES *)(void* target, InvokeArguments*);

struct CallbackRegisters {
    static constexpr ptrdiff_t offsetOfGPR(unsigned index) { return OBJECT_OFFSETOF(CallbackRegisters, gprs) + index * sizeof(uint64_t); }
    static constexpr ptrdiff_t offsetOfFPR(unsigned index) { return OBJECT_OFFSETOF(CallbackRegisters, fprs) + index * sizeof(uint64_t); }
    static constexpr ptrdiff_t offsetOfReturnValue() { return OBJECT_OFFSETOF(CallbackRegisters, returnValue); }

    uint64_t gprs[maxArgumentRegistersPerClass];
    uint64_t fprs[maxArgumentRegistersPerClass];
    uint64_t returnValue;
};

struct CallbackEntryContext {
    JS_EXPORT_PRIVATE static CallbackEntryContext forThisProcess();

    static constexpr ptrdiff_t offsetOfEnter() { return OBJECT_OFFSETOF(CallbackEntryContext, enter); }

    void* enter;
};

// Lies behind the constants of an image, where nothing is executed. Every offset counts from the first byte of the FFI code.
struct StubDirectory {
    static constexpr size_t sizeWithEntries(unsigned numberOfCallbackEntries) { return sizeof(StubDirectory) + numberOfCallbackEntries * sizeof(uint32_t); }

    std::span<const uint32_t> callbackEntries() const LIFETIME_BOUND { return unsafeMakeSpan(reinterpret_cast<const uint32_t*>(this + 1), numberOfCallbackEntries); }

    uint32_t invoke;
    uint32_t callbackContext;
    uint32_t callbackContextSize;
    uint32_t numberOfCallbackEntries;
};
static_assert(sizeof(StubDirectory) == 16);

enum class CodeSource : uint8_t {
    None,
    JIT,
    Stubs,
};

class Stubs {
    WTF_MAKE_NONCOPYABLE(Stubs);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Stubs);
public:
    JS_EXPORT_PRIVATE static const Stubs* current();

    InvokeStubFunction invoke() const { return m_invoke; }
    std::span<void* const> callbackEntries() const LIFETIME_BOUND { return m_callbackEntries.span(); }
    [[nodiscard]] bool prepareCallbackContext() const;

private:
    Stubs(InvokeStubFunction, Vector<void*>&& callbackEntries, std::span<uint8_t> reservedCallbackContext);

    static std::unique_ptr<const Stubs> tryCreateFromImage();

    InvokeStubFunction m_invoke;
    Vector<void*> m_callbackEntries;
    std::span<uint8_t> m_reservedCallbackContext;
};

JS_EXPORT_PRIVATE CodeSource codeSource();

JS_EXPORT_PRIVATE uint64_t normalizeSlot(Type, uint64_t rawBits);

JS_EXPORT_PRIVATE void invokeThroughStub(InvokeStubFunction, const Signature&, void* target, uint64_t* slots);

} // namespace FFI

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(ffiCallbackEnter, void, (uint32_t entry, FFI::CallbackRegisters*, const void* frame));

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)

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
#include <atomic>
#include <memory>
#include <optional>
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

class JSFFICallback;
namespace AOT {
class Image;
}

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
    JS_EXPORT_PRIVATE static CallbackEntryContext forThisProcess(std::atomic<JSFFICallback*>* callbacks);

    static constexpr ptrdiff_t offsetOfEnter() { return OBJECT_OFFSETOF(CallbackEntryContext, enter); }
    static constexpr ptrdiff_t offsetOfCallbacks() { return OBJECT_OFFSETOF(CallbackEntryContext, callbacks); }

    void* enter;
    // A word for each entry of the mapping of the code that this context lies behind.
    std::atomic<JSFFICallback*>* callbacks;
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

    // The code of the entries finds its context by its own address, so it can be mapped as often as the process needs entries.
    struct BlockOfCallbackEntries {
        uint8_t* code { nullptr };
        std::span<uint8_t> context;
    };

    InvokeStubFunction invoke() const { return m_invoke; }
    unsigned numberOfCallbackEntriesInBlock() const { return static_cast<unsigned>(m_callbackEntries.offsets.size()); }
    void* callbackEntry(const BlockOfCallbackEntries& block, unsigned index) const { return block.code + m_callbackEntries.offsets[index]; }
    // `callbacks` has a word for each entry of a block and stays where it is. Nothing: the process may not map the code once more, or may not write the context.
    [[nodiscard]] std::optional<BlockOfCallbackEntries> tryMakeBlockOfCallbackEntries(std::atomic<JSFFICallback*>* callbacks, bool isFirst) const;

private:
    struct CodeOfCallbackEntries {
        // Whole pages. Behind them the image has room for one context.
        std::span<uint8_t> pages;
        size_t contextSize { 0 };
        Vector<uint32_t> offsets;
        // Where the pages lie in a file that was open when the image was taken into use, or in the executable, which is opened anew.
        int descriptor { -1 };
        int64_t offsetInFile { 0 };
        const AOT::Image* imageInExecutable { nullptr };
    };

    Stubs(InvokeStubFunction, CodeOfCallbackEntries&&);

    static std::unique_ptr<const Stubs> tryCreateFromImage();
    std::optional<BlockOfCallbackEntries> tryMapCallbackEntries() const;

    InvokeStubFunction m_invoke;
    CodeOfCallbackEntries m_callbackEntries;
};

JS_EXPORT_PRIVATE CodeSource codeSource();

JS_EXPORT_PRIVATE uint64_t normalizeSlot(Type, uint64_t rawBits);

JS_EXPORT_PRIVATE void invokeThroughStub(InvokeStubFunction, const Signature&, void* target, uint64_t* slots);

} // namespace FFI

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(ffiCallbackEnter, void, (uint32_t entryInBlock, FFI::CallbackRegisters*, const void* frame, std::atomic<JSFFICallback*>* callbacksOfBlock));

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)

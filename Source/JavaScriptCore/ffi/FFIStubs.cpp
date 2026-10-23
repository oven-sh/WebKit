/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "FFIStubs.h"

#if USE(BUN_JSC_ADDITIONS)

#include "FFICallbackEntryPool.h"
#include "FFICallbackThunk.h"
#include "Options.h"
#include "VM.h"
#include <atomic>
#include <mutex>
#include <wtf/PageBlock.h>

#if ENABLE(AOT)
#include "AOTImage.h"
#endif

#if FFI_STUBS_SUPPORTED
#if OS(WINDOWS)
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#if OS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace FFI {

static_assert(!FFI_STUBS_SUPPORTED || FFI_CALLBACK_THUNK_SUPPORTED);

static constinit std::atomic<const Stubs*> s_currentStubs { nullptr };

CallbackEntryContext CallbackEntryContext::forThisProcess()
{
#if FFI_STUBS_SUPPORTED
    return { std::bit_cast<void*>(tagCFunction<OperationPtrTag>(ffiCallbackEnter)) };
#else
    return { nullptr };
#endif
}

Stubs::Stubs(InvokeStubFunction invoke, Vector<void*>&& callbackEntries, std::span<uint8_t> reservedCallbackContext)
    : m_invoke(invoke)
    , m_callbackEntries(WTF::move(callbackEntries))
    , m_reservedCallbackContext(reservedCallbackContext)
{
}

const Stubs* Stubs::current()
{
    static std::once_flag onceKey;
    std::call_once(onceKey, [] {
        s_currentStubs.store(tryCreateFromImage().release(), std::memory_order_release);
    });
    return s_currentStubs.load(std::memory_order_acquire);
}

#if ENABLE(AOT) && FFI_STUBS_SUPPORTED

static std::optional<uint8_t*> mapCallbackEntries(uint8_t* base, const StubDirectory& directory)
{
#if OS(DARWIN)
    // The kernel can refuse to replace pages of a signed executable mapping, so the entries run in a second mapping of the code, followed by fresh memory for the context.
    mach_vm_size_t codeSize = directory.callbackContext;
    mach_vm_size_t regionSize = codeSize + directory.callbackContextSize;
    mach_vm_address_t region = 0;
    if (mach_vm_allocate(mach_task_self(), &region, regionSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return std::nullopt;

    mach_vm_address_t target = region;
    vm_prot_t currentProtection;
    vm_prot_t maximumProtection;
    kern_return_t result = mach_vm_remap(mach_task_self(), &target, codeSize, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), reinterpret_cast<mach_vm_address_t>(base), FALSE, &currentProtection, &maximumProtection, VM_INHERIT_SHARE);
    if (result != KERN_SUCCESS || !(currentProtection & VM_PROT_EXECUTE)) {
        mach_vm_deallocate(mach_task_self(), region, regionSize);
        return std::nullopt;
    }
    return reinterpret_cast<uint8_t*>(region);
#else
    UNUSED_PARAM(directory);
    return base;
#endif
}

#endif // ENABLE(AOT) && FFI_STUBS_SUPPORTED

std::unique_ptr<const Stubs> Stubs::tryCreateFromImage()
{
#if ENABLE(AOT) && FFI_STUBS_SUPPORTED
    const AOT::Image* image = AOT::Image::current();
    if (!image)
        return nullptr;
    std::span<const uint8_t> code = image->ffiCode();
    if (code.empty())
        return nullptr;

    RELEASE_ASSERT(code.size() >= sizeof(StubDirectory));
    auto& directory = *reinterpret_cast<const StubDirectory*>(code.data());
    RELEASE_ASSERT(code.size() >= StubDirectory::sizeWithEntries(directory.numberOfCallbackEntries));
    RELEASE_ASSERT(directory.invoke < directory.callbackContext);
    RELEASE_ASSERT(directory.callbackContext + static_cast<uint64_t>(directory.callbackContextSize) <= code.size());
    RELEASE_ASSERT(directory.callbackContextSize >= sizeof(CallbackEntryContext));
    for (uint32_t offset : directory.callbackEntries())
        RELEASE_ASSERT(offset < directory.callbackContext);

    uint8_t* base = const_cast<uint8_t*>(code.data());
    RELEASE_ASSERT(isPageAligned(base) && isPageAligned(directory.callbackContext) && isPageAligned(directory.callbackContextSize));

    auto invoke = reinterpret_cast<InvokeStubFunction>(base + directory.invoke);
    std::optional<uint8_t*> entryBase = mapCallbackEntries(base, directory);
    if (!entryBase)
        return std::unique_ptr<const Stubs>(new Stubs(invoke, { }, { }));

    Vector<void*> entries;
    entries.reserveInitialCapacity(directory.numberOfCallbackEntries);
    for (uint32_t offset : directory.callbackEntries())
        entries.append(static_cast<void*>(*entryBase + offset));
    return std::unique_ptr<const Stubs>(new Stubs(invoke, WTF::move(entries), { *entryBase + directory.callbackContext, directory.callbackContextSize }));
#else
    return nullptr;
#endif
}

#if FFI_STUBS_SUPPORTED

static bool tryMakeContextWritable(std::span<uint8_t> context)
{
#if OS(DARWIN)
    UNUSED_PARAM(context);
    return true;
#elif OS(WINDOWS)
    // The context lies in a loaded section or in a copy-on-write view of a file, and this is the writable protection both allow.
    DWORD previousProtection;
    return !!VirtualProtect(context.data(), context.size(), PAGE_WRITECOPY, &previousProtection);
#else
    return mmap(context.data(), context.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == context.data();
#endif
}

static void makeContextReadOnly(std::span<uint8_t> context)
{
#if OS(WINDOWS)
    DWORD previousProtection;
    VirtualProtect(context.data(), context.size(), PAGE_READONLY, &previousProtection);
#else
    mprotect(context.data(), context.size(), PROT_READ);
#endif
}

#endif // FFI_STUBS_SUPPORTED

bool Stubs::prepareCallbackContext() const
{
    if (m_reservedCallbackContext.empty())
        return true;
#if FFI_STUBS_SUPPORTED
    if (!tryMakeContextWritable(m_reservedCallbackContext))
        return false;
    *reinterpret_cast<CallbackEntryContext*>(m_reservedCallbackContext.data()) = CallbackEntryContext::forThisProcess();
    makeContextReadOnly(m_reservedCallbackContext);
    return true;
#else
    return false;
#endif
}

CodeSource codeSource()
{
    if (Stubs::current())
        return CodeSource::Stubs;
    if (Options::useJIT() && VM::canUseAssembler())
        return CodeSource::JIT;
    return CodeSource::None;
}

uint64_t normalizeSlot(Type type, uint64_t rawBits)
{
    switch (argClass(type)) {
    case ArgClass::Void:
        return 0;
    case ArgClass::Float:
        return static_cast<uint32_t>(rawBits);
    case ArgClass::Double:
        return rawBits;
    case ArgClass::Int:
        break;
    }

    if (type == Type::Bool)
        return !!(rawBits & 0xff);
    switch (nativeSizeInBytes(type)) {
    case 1:
        return isSigned(type) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(rawBits))) : static_cast<uint8_t>(rawBits);
    case 2:
        return isSigned(type) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(rawBits))) : static_cast<uint16_t>(rawBits);
    case 4:
        return isSigned(type) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(rawBits))) : static_cast<uint32_t>(rawBits);
    case 8:
        return rawBits;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } // namespace JSC::FFI

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS)

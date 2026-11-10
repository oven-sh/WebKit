/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "FFIStubs.h"

#if ENABLE(FFI)

#include "FFICallbackEntryPool.h"
#include "FFICallbackThunk.h"
#include "Options.h"
#include "VM.h"
#include <atomic>
#include <wtf/Lock.h>
#include <wtf/PageBlock.h>

#if ENABLE(AOT)
#include "AOTImage.h"
#include "AOTImageLoading.h"
#endif

#if FFI_STUBS_SUPPORTED
#if OS(WINDOWS)
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/Scope.h>
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

CallbackEntryContext CallbackEntryContext::forThisProcess(std::atomic<JSFFICallback*>* callbacks)
{
#if FFI_STUBS_SUPPORTED
    return { std::bit_cast<void*>(tagCFunction<OperationPtrTag>(ffiCallbackEnter)), callbacks };
#else
    UNUSED_PARAM(callbacks);
    return { nullptr, nullptr };
#endif
}

Stubs::Stubs(InvokeStubFunction invoke, CodeOfCallbackEntries&& code)
    : m_invoke(invoke)
    , m_callbackEntries(WTF::move(code))
{
}

const Stubs* Stubs::current()
{
    // The stubs serve the whole process. Of several images, the first that is asked and has them gives them.
    if (const Stubs* stubs = s_currentStubs.load(std::memory_order_acquire)) [[likely]]
        return stubs;
    static Lock lock;
    Locker locker { lock };
    if (!s_currentStubs.load(std::memory_order_acquire))
        s_currentStubs.store(tryCreateFromImage().release(), std::memory_order_release);
    return s_currentStubs.load(std::memory_order_acquire);
}

std::optional<Stubs::BlockOfCallbackEntries> Stubs::tryMapCallbackEntries() const
{
#if FFI_STUBS_SUPPORTED
    const uint8_t* pages = m_callbackEntries.pages.data();
    size_t codeSize = m_callbackEntries.pages.size();
    size_t regionSize = codeSize + m_callbackEntries.contextSize;
#if OS(DARWIN)
    mach_vm_address_t region = 0;
    if (mach_vm_allocate(mach_task_self(), &region, regionSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return std::nullopt;

    mach_vm_address_t target = region;
    vm_prot_t currentProtection;
    vm_prot_t maximumProtection;
    kern_return_t result = mach_vm_remap(mach_task_self(), &target, codeSize, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), reinterpret_cast<mach_vm_address_t>(pages), FALSE, &currentProtection, &maximumProtection, VM_INHERIT_SHARE);
    if (result != KERN_SUCCESS || !(currentProtection & VM_PROT_EXECUTE)) {
        mach_vm_deallocate(mach_task_self(), region, regionSize);
        return std::nullopt;
    }
    auto* code = reinterpret_cast<uint8_t*>(region);
#elif OS(WINDOWS)
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!code)
        return std::nullopt;
    memcpy(code, pages, codeSize);
    DWORD previousProtection;
    if (!VirtualProtect(code, codeSize, PAGE_EXECUTE_READ, &previousProtection)) {
        VirtualFree(code, 0, MEM_RELEASE);
        return std::nullopt;
    }
    FlushInstructionCache(GetCurrentProcess(), code, codeSize);
#else
    // A process that may not make memory executable may still map a file for execution.
    // The number of the descriptor may stand for another file by now. A mapping behind the end of a shorter file succeeds, and the first look at it faults.
    int descriptor = m_callbackEntries.descriptor;
#if ENABLE(AOT)
    if (m_callbackEntries.imageInExecutable)
        descriptor = AOT::openExecutableContaining(*m_callbackEntries.imageInExecutable);
#endif
    auto closeExecutable = makeScopeExit([&] {
        if (m_callbackEntries.imageInExecutable && descriptor >= 0)
            close(descriptor);
    });
    auto mapRegion = [&](int protection, int flags, int file, off_t offset) {
        void* region = mmap(nullptr, regionSize, protection, MAP_PRIVATE | flags, file, offset);
        return region == MAP_FAILED ? nullptr : static_cast<uint8_t*>(region);
    };
    struct stat status;
    bool fileHoldsPages = descriptor >= 0 && !fstat(descriptor, &status) && S_ISREG(status.st_mode) && status.st_size >= m_callbackEntries.offsetInFile + static_cast<int64_t>(codeSize);
    uint8_t* code = fileHoldsPages ? mapRegion(PROT_READ | PROT_EXEC, 0, descriptor, m_callbackEntries.offsetInFile) : nullptr;
    if (code && mmap(code + codeSize, m_callbackEntries.contextSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
        munmap(code, regionSize);
        code = nullptr;
    }
    if (code && memcmp(code, pages, codeSize)) {
        munmap(code, regionSize);
        code = nullptr;
    }
    if (!code) {
        code = mapRegion(PROT_READ | PROT_WRITE, MAP_ANON, -1, 0);
        if (!code)
            return std::nullopt;
        memcpy(code, pages, codeSize);
        if (mprotect(code, codeSize, PROT_READ | PROT_EXEC)) {
            munmap(code, regionSize);
            return std::nullopt;
        }
        __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + codeSize));
    }
#endif
    return BlockOfCallbackEntries { code, { code + codeSize, m_callbackEntries.contextSize } };
#else
    return std::nullopt;
#endif
}

std::unique_ptr<const Stubs> Stubs::tryCreateFromImage()
{
#if ENABLE(AOT) && FFI_STUBS_SUPPORTED
    const AOT::Image* image = AOT::Image::current();
    if (!image)
        return nullptr;
    std::span<const uint8_t> code = image->ffiCode();
    if (code.empty())
        return nullptr;

    std::span<const uint8_t> directoryBytes = image->ffiDirectory();
    RELEASE_ASSERT(directoryBytes.size() >= sizeof(StubDirectory));
    auto& directory = *reinterpret_cast<const StubDirectory*>(directoryBytes.data());
    RELEASE_ASSERT(directoryBytes.size() >= StubDirectory::sizeWithEntries(directory.numberOfCallbackEntries));
    RELEASE_ASSERT(directory.invoke < directory.callbackContext);
    RELEASE_ASSERT(directory.callbackContext + static_cast<uint64_t>(directory.callbackContextSize) <= code.size());
    RELEASE_ASSERT(directory.callbackContextSize >= sizeof(CallbackEntryContext));
    for (uint32_t offset : directory.callbackEntries())
        RELEASE_ASSERT(offset < directory.callbackContext);

    uint8_t* base = const_cast<uint8_t*>(code.data());
    RELEASE_ASSERT(isPageAligned(base) && isPageAligned(directory.callbackContext) && isPageAligned(directory.callbackContextSize));

    auto invoke = reinterpret_cast<InvokeStubFunction>(base + directory.invoke);
    CodeOfCallbackEntries entries { { base, directory.callbackContext }, directory.callbackContextSize, Vector<uint32_t>(directory.callbackEntries()) };
    if (const auto& position = image->positionOfCodeInFile()) {
        entries.descriptor = position->descriptor;
        entries.offsetInFile = position->offset + (base - image->code().data());
    } else if (const auto& place = image->placeInExecutable()) {
        entries.imageInExecutable = image;
        entries.offsetInFile = place->offset + image->header().codeOffset + (base - image->code().data());
    }
    return std::unique_ptr<const Stubs>(new Stubs(invoke, WTF::move(entries)));
#else
    return nullptr;
#endif
}

#if FFI_STUBS_SUPPORTED

#if !OS(DARWIN)
static bool tryMakeContextWritable(std::span<uint8_t> context)
{
#if OS(WINDOWS)
    // The context lies in a loaded section or in a copy-on-write view of a file, and this is the writable protection both allow.
    DWORD previousProtection;
    return !!VirtualProtect(context.data(), context.size(), PAGE_WRITECOPY, &previousProtection);
#else
    return mmap(context.data(), context.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == context.data();
#endif
}

#endif // !OS(DARWIN)

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

std::optional<Stubs::BlockOfCallbackEntries> Stubs::tryMakeBlockOfCallbackEntries(std::atomic<JSFFICallback*>* callbacks, bool isFirst) const
{
#if FFI_STUBS_SUPPORTED
    if (m_callbackEntries.offsets.isEmpty())
        return std::nullopt;
    std::optional<BlockOfCallbackEntries> block;
#if OS(DARWIN)
    // The kernel can refuse to replace pages of a signed executable mapping, so no entry runs where the image was loaded.
    UNUSED_PARAM(isFirst);
    block = tryMapCallbackEntries();
#else
    if (!isFirst)
        block = tryMapCallbackEntries();
    else if (std::span<uint8_t> context { m_callbackEntries.pages.data() + m_callbackEntries.pages.size(), m_callbackEntries.contextSize }; tryMakeContextWritable(context))
        block = BlockOfCallbackEntries { m_callbackEntries.pages.data(), context };
#endif
    if (!block)
        return std::nullopt;
    *reinterpret_cast<CallbackEntryContext*>(block->context.data()) = CallbackEntryContext::forThisProcess(callbacks);
    makeContextReadOnly(block->context);
    return block;
#else
    UNUSED_PARAM(callbacks);
    UNUSED_PARAM(isFirst);
    return std::nullopt;
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

#endif // ENABLE(FFI)

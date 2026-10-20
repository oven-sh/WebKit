/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageLoading.h"

#if ENABLE(AOT)

#include "AOTFaultTable.h"
#include "AOTHost.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "Options.h"
#include "WasmCodeInImage.h"
#include <wtf/AccessibleAddress.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/threads/Signals.h>

#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD)
#include <errno.h>
#include <sys/mman.h>
#if OS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#elif OS(WINDOWS)
#include <windows.h>
#else
#error "An AOT image is loaded on Darwin, Linux, FreeBSD and Windows only"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool tryReserveLowPages()
{
    static bool s_hasReservedPages;
    if (lowestAccessibleAddress() >= FaultSite::reach || s_hasReservedPages)
        return true;
#if OS(FREEBSD) || (OS(LINUX) && defined(MAP_FIXED_NOREPLACE))
#if OS(FREEBSD)
    constexpr int fixedWithoutReplacing = MAP_FIXED | MAP_EXCL;
    constexpr int outOfRangeError = EINVAL;
    char isResident;
#else
    constexpr int fixedWithoutReplacing = MAP_FIXED_NOREPLACE;
    constexpr int outOfRangeError = EPERM;
    unsigned char isResident;
#endif
    size_t pageBytes = pageSize();
    for (uintptr_t page = 0; page < FaultSite::reach; page += pageBytes) {
        void* mapped = mmap(std::bit_cast<void*>(page), pageBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | fixedWithoutReplacing, -1, 0);
        bool zeroPageWasRelocated = !page && mapped != MAP_FAILED && mapped;
        if (zeroPageWasRelocated)
            munmap(mapped, pageBytes);
        if (mapped == MAP_FAILED || zeroPageWasRelocated) {
            if (!zeroPageWasRelocated && errno != outOfRangeError && errno != EPERM && errno != EACCES && errno != ENOMEM)
                return false;
            if (mincore(std::bit_cast<void*>(page), pageBytes, &isResident) != -1 || errno != ENOMEM)
                return false;
            continue;
        }
        if (mapped != std::bit_cast<void*>(page)) {
            munmap(mapped, pageBytes);
            return false;
        }
    }
    s_hasReservedPages = true;
    return true;
#else
    return false;
#endif
}

#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD)

static void* mapCode(intptr_t descriptor, int64_t fileOffset, size_t size)
{
    int fileDescriptor = static_cast<int>(descriptor);
    void* code = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fileDescriptor, fileOffset);
    if (code == MAP_FAILED && errno == EPERM) {
        code = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fileDescriptor, fileOffset);
        if (code != MAP_FAILED && mprotect(code, size, PROT_READ | PROT_EXEC)) {
            munmap(code, size);
            code = MAP_FAILED;
        }
    }
    if (code == MAP_FAILED)
        return nullptr;
#if OS(DARWIN)
    mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(code), size, true, VM_PROT_READ | VM_PROT_EXECUTE);
#endif
    return code;
}

static void unmapCode(void* code, size_t size)
{
    munmap(code, size);
}

#if OS(LINUX)
static void* copyCodeToAnonymousMemory(std::span<const uint8_t> code)
{
    void* copy = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED)
        return nullptr;
    memcpySpan(std::span { static_cast<uint8_t*>(copy), code.size() }, code);
    if (mprotect(copy, code.size(), PROT_READ | PROT_EXEC)) {
        munmap(copy, code.size());
        return nullptr;
    }
    __builtin___clear_cache(static_cast<char*>(copy), static_cast<char*>(copy) + code.size());
    return copy;
}
#endif

#elif OS(WINDOWS)

static size_t allocationGranularity()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwAllocationGranularity;
}

static void* mapCode(intptr_t descriptor, int64_t fileOffset, size_t size)
{
    // A page of a view can only get a protection that the view was mapped for, and the FFI stubs write their callback context once.
    HANDLE mapping = CreateFileMappingW(reinterpret_cast<HANDLE>(descriptor), nullptr, PAGE_EXECUTE_WRITECOPY, 0, 0, nullptr);
    if (!mapping)
        return nullptr;
    uint64_t start = static_cast<uint64_t>(fileOffset) & ~static_cast<uint64_t>(allocationGranularity() - 1);
    size_t skipped = static_cast<uint64_t>(fileOffset) - start;
    void* view = MapViewOfFile(mapping, FILE_MAP_COPY | FILE_MAP_EXECUTE, static_cast<DWORD>(start >> 32), static_cast<DWORD>(start), size + skipped);
    CloseHandle(mapping);
    if (!view)
        return nullptr;
    DWORD previousProtection;
    if (!VirtualProtect(view, size + skipped, PAGE_EXECUTE_READ, &previousProtection)) {
        UnmapViewOfFile(view);
        return nullptr;
    }
    return static_cast<uint8_t*>(view) + skipped;
}

static void unmapCode(void* code, size_t)
{
    UnmapViewOfFile(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(code) & ~static_cast<uintptr_t>(allocationGranularity() - 1)));
}

#endif

static constexpr bool isOnPageBoundaries(uint64_t position, size_t codeOffset, size_t imageSize, size_t pageSize)
{
    return !(position % pageSize) && !(codeOffset % pageSize) && !(imageSize % pageSize);
}
static_assert(isOnPageBoundaries(3 * AOT::imagePageSize, AOT::imagePageSize, 5 * AOT::imagePageSize, CeilingOnPageSize));

namespace {

class CodeMapping {
public:
    enum class Kind : uint8_t { FileMapping, LoadedSection, AnonymousCopy };

    static std::optional<CodeMapping> mapFile(ImageFile, uint64_t codeOffset, size_t size);
    static std::optional<CodeMapping> protectLoadedSection(std::span<const uint8_t> code);
#if OS(LINUX)
    static std::optional<CodeMapping> copy(std::span<const uint8_t> code);
#endif

    const uint8_t* code() const { return static_cast<const uint8_t*>(m_code); }
    void release();

private:
    CodeMapping(Kind kind, void* code, size_t size)
        : m_kind(kind)
        , m_code(code)
        , m_size(size)
    {
    }

    Kind m_kind;
    void* m_code;
    size_t m_size;
};

std::optional<CodeMapping> CodeMapping::mapFile(ImageFile file, uint64_t codeOffset, size_t size)
{
    void* code = mapCode(file.descriptor, file.offset + codeOffset, size);
    if (!code)
        return std::nullopt;
    return CodeMapping { Kind::FileMapping, code, size };
}

std::optional<CodeMapping> CodeMapping::protectLoadedSection(std::span<const uint8_t> code)
{
    CodeMapping mapping { Kind::LoadedSection, const_cast<uint8_t*>(code.data()), code.size() };
    // On Darwin the section is in a segment that the loader maps executable.
#if OS(LINUX) || OS(FREEBSD)
    if (mprotect(mapping.m_code, mapping.m_size, PROT_READ | PROT_EXEC))
        return std::nullopt;
#elif OS(WINDOWS)
    MEMORY_BASIC_INFORMATION region;
    if (VirtualQuery(mapping.m_code, &region, sizeof(region)) && region.Protect == PAGE_EXECUTE_READ && static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize >= code.data() + code.size())
        return mapping;
    DWORD previousProtection;
    if (!VirtualProtect(mapping.m_code, mapping.m_size, PAGE_EXECUTE_READ, &previousProtection))
        return std::nullopt;
    FlushInstructionCache(GetCurrentProcess(), mapping.m_code, mapping.m_size);
#endif
    return mapping;
}

#if OS(LINUX)
std::optional<CodeMapping> CodeMapping::copy(std::span<const uint8_t> code)
{
    void* anonymousCopy = copyCodeToAnonymousMemory(code);
    if (!anonymousCopy)
        return std::nullopt;
    return CodeMapping { Kind::AnonymousCopy, anonymousCopy, code.size() };
}
#endif

void CodeMapping::release()
{
    switch (m_kind) {
    case Kind::FileMapping:
        unmapCode(m_code, m_size);
        return;
    case Kind::LoadedSection: {
#if OS(LINUX) || OS(FREEBSD)
        mprotect(m_code, m_size, PROT_READ);
#elif OS(WINDOWS)
        DWORD previousProtection;
        VirtualProtect(m_code, m_size, PAGE_READONLY, &previousProtection);
#endif
        return;
    }
    case Kind::AnonymousCopy:
#if OS(LINUX)
        munmap(m_code, m_size);
#endif
        return;
    }
}

} // anonymous namespace

// The loader of an ELF executable maps the payload with write access. The section that holds it in a Mach-O or PE executable has none.
static bool removeWriteAccessFromLoadedSection(std::span<const uint8_t> image)
{
#if OS(LINUX) || OS(FREEBSD)
    return !mprotect(const_cast<uint8_t*>(image.data()), image.size(), PROT_READ);
#else
    UNUSED_PARAM(image);
    return true;
#endif
}

static std::optional<CodeMapping> createCodeMapping(std::span<const uint8_t> bytes, std::optional<ImageFile> file, const ImageHeader& header)
{
    size_t size = header.size - header.codeOffset;
#if OS(LINUX)
    if (Options::useAOTPerfMap()) [[unlikely]]
        return CodeMapping::copy(bytes.subspan(header.codeOffset, size));
#endif
    if (file)
        return CodeMapping::mapFile(*file, header.codeOffset, size);
    return CodeMapping::protectLoadedSection(bytes.subspan(header.codeOffset, size));
}

static bool hasFaultSites(const ImageHeader& header)
{
    return !!header.sections[static_cast<size_t>(ImageSection::FaultSites)].size;
}

static void didPublishImage(const Image& image)
{
#if ENABLE(WEBASSEMBLY_OMGJIT)
    if (auto directory = image.section<uint8_t>(ImageSection::WebAssemblyDirectory); !directory.empty())
        Wasm::CodeInImage::setImage(directory.data(), image.webAssemblyCode());
#endif
    if (hasFaultSites(image.header()))
        activateSignalHandlersFor(Signal::AccessFault);
#if OS(LINUX)
    if (Options::useAOTPerfMap()) [[unlikely]]
        writePerfMap();
#endif
}

Wire::ImageVerdict useImage(std::span<const uint8_t> bytes, std::optional<ImageFile> file, const std::array<uint8_t, 16>& identity)
{
    using Wire::ImageVerdict;

    if (!Options::useAOT())
        return ImageVerdict::CompiledCodeDisabled;
    if (Image::isRegistered())
        return ImageVerdict::AlreadyInUse;
    if (ImageVerdict verdict = Image::check(bytes, identity); verdict != ImageVerdict::Accepted)
        return verdict;

    auto& header = *reinterpret_cast<const ImageHeader*>(bytes.data());
    uint64_t position = file ? static_cast<uint64_t>(file->offset) : reinterpret_cast<uintptr_t>(bytes.data());
    if (!isOnPageBoundaries(position, header.codeOffset, header.size, WTF::pageSize()))
        return ImageVerdict::Misaligned;
    if (hasFaultSites(header) && !tryReserveLowPages())
        return ImageVerdict::LowPagesAccessible;

    if (!file && !removeWriteAccessFromLoadedSection(bytes.first(header.size)))
        return ImageVerdict::CannotMapCode;
    std::optional<CodeMapping> mapping = createCodeMapping(bytes, file, header);
    if (!mapping)
        return ImageVerdict::CannotMapCode;
    if (!Image::publish(Image::create(bytes, mapping->code()))) {
        mapping->release();
        return ImageVerdict::AlreadyInUse;
    }
    didPublishImage(*Image::current());
    return ImageVerdict::Accepted;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#else // ENABLE(AOT)

namespace JSC { namespace AOT {

Wire::ImageVerdict useImage(std::span<const uint8_t>, std::optional<ImageFile>, const std::array<uint8_t, 16>&)
{
    return Wire::ImageVerdict::Unsupported;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

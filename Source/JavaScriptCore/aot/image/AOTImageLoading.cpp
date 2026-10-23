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
#include "AOTImageText.h"
#include "Options.h"
#include "WasmCodeInImage.h"
#include <wtf/AccessibleAddress.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/threads/Signals.h>

#if OS(DARWIN) || OS(LINUX) || OS(FREEBSD)
#include <errno.h>
#include <sys/mman.h>
#if OS(DARWIN)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#if OS(LINUX) || OS(FREEBSD)
#include <fcntl.h>
#include <link.h>
#include <unistd.h>
#if OS(LINUX)
#include <sys/auxv.h>
#include <sys/personality.h>
#endif
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

#if OS(LINUX)
static bool isAddressRandomizationDisabled()
{
    int persona = personality(0xffffffff);
    return persona != -1 && persona & ADDR_NO_RANDOMIZE;
}
#endif

static void* chooseAddressForCode(size_t size)
{
#if OS(LINUX) && CPU(ADDRESS64)
    static constexpr uintptr_t lowestRandomAddress = 64 * GB;
    static constexpr uintptr_t highestRandomAddress = 124 * 1024 * GB;
    uintptr_t numberOfSlots = (highestRandomAddress - lowestRandomAddress - size) / imagePageSize;
    if (!isAddressRandomizationDisabled())
        return std::bit_cast<void*>(lowestRandomAddress + cryptographicallyRandomNumber<uint64_t>() % numberOfSlots * imagePageSize);
#else
    UNUSED_PARAM(size);
#endif
    return nullptr;
}

// The first executableSize bytes of a code part are executed. The rest is data that the code finds by its distance, so it is mapped behind the code, to be read only.
static void* mapCode(intptr_t descriptor, int64_t fileOffset, size_t size, size_t executableSize)
{
    int fileDescriptor = static_cast<int>(descriptor);
    void* code = mmap(chooseAddressForCode(size), size, PROT_READ, MAP_PRIVATE, fileDescriptor, fileOffset);
    if (code == MAP_FAILED)
        return nullptr;
    bool isExecutable = mmap(code, executableSize, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fileDescriptor, fileOffset) != MAP_FAILED;
#if !OS(LINUX)
    if (!isExecutable && errno == EPERM)
        isExecutable = !mprotect(code, executableSize, PROT_READ | PROT_EXEC);
#endif
    if (!isExecutable) {
        munmap(code, size);
        return nullptr;
    }
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
#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

static void* moveLoadedCode(std::span<const uint8_t> code, size_t executableSize)
{
    void* target = mmap(chooseAddressForCode(code.size()), code.size(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (target == MAP_FAILED)
        return nullptr;
    // One call moves one mapping, and the loader made two: they differ in their protection.
    auto move = [&](size_t offset, size_t size) {
        return !size || mremap(const_cast<uint8_t*>(code.data()) + offset, size, size, MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, static_cast<uint8_t*>(target) + offset) != MAP_FAILED;
    };
    if (!move(0, executableSize) || !move(executableSize, code.size() - executableSize)) {
        munmap(target, code.size());
        return nullptr;
    }
    return target;
}

#if !defined(BUN_HARDENED)
static void* copyCodeToAnonymousMemory(std::span<const uint8_t> code, size_t executableSize)
{
    void* copy = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED)
        return nullptr;
    memcpySpan(std::span { static_cast<uint8_t*>(copy), code.size() }, code);
    if (mprotect(copy, code.size(), PROT_READ) || mprotect(copy, executableSize, PROT_READ | PROT_EXEC)) {
        munmap(copy, code.size());
        return nullptr;
    }
    __builtin___clear_cache(static_cast<char*>(copy), static_cast<char*>(copy) + executableSize);
    return copy;
}
#endif
#endif

#elif OS(WINDOWS)

static size_t allocationGranularity()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwAllocationGranularity;
}

static void* mapCode(intptr_t descriptor, int64_t fileOffset, size_t size, size_t executableSize)
{
    // A view can be executed only if its file was opened for that, which whoever wrote the file had no reason to ask for.
    HANDLE file = ReOpenFile(reinterpret_cast<HANDLE>(descriptor), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    // A page of a view can only get a protection that the view was mapped for, and the FFI stubs write their callback context once.
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_EXECUTE_WRITECOPY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping)
        return nullptr;
    uint64_t start = static_cast<uint64_t>(fileOffset) & ~static_cast<uint64_t>(allocationGranularity() - 1);
    size_t skipped = static_cast<uint64_t>(fileOffset) - start;
    void* view = MapViewOfFile(mapping, FILE_MAP_COPY | FILE_MAP_EXECUTE, static_cast<DWORD>(start >> 32), static_cast<DWORD>(start), size + skipped);
    CloseHandle(mapping);
    if (!view)
        return nullptr;
    DWORD previousProtection;
    if (!VirtualProtect(view, executableSize + skipped, PAGE_EXECUTE_READ, &previousProtection)
        || (size > executableSize && !VirtualProtect(static_cast<uint8_t*>(view) + skipped + executableSize, size - executableSize, PAGE_READONLY, &previousProtection))) {
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
    enum class Kind : uint8_t { FileMapping, ExecutableSegment, LoadedSection, AnonymousCopy };

    static std::optional<CodeMapping> mapFile(ImageFile, uint64_t codeOffset, size_t size, size_t executableSize);
    static CodeMapping useExecutableSegment(std::span<const uint8_t> code);
    static std::optional<CodeMapping> protectLoadedSection(std::span<const uint8_t> executableCode);
#if OS(LINUX)
    static std::optional<CodeMapping> moveExecutableSegment(std::span<const uint8_t> code, size_t executableSize);
#if !defined(BUN_HARDENED)
    static std::optional<CodeMapping> copy(std::span<const uint8_t> code, size_t executableSize);
#endif
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

std::optional<CodeMapping> CodeMapping::mapFile(ImageFile file, uint64_t codeOffset, size_t size, size_t executableSize)
{
    void* code = mapCode(file.descriptor, file.offset + codeOffset, size, executableSize);
    if (!code)
        return std::nullopt;
    return CodeMapping { Kind::FileMapping, code, size };
}

CodeMapping CodeMapping::useExecutableSegment(std::span<const uint8_t> code)
{
    return { Kind::ExecutableSegment, const_cast<uint8_t*>(code.data()), code.size() };
}

std::optional<CodeMapping> CodeMapping::protectLoadedSection(std::span<const uint8_t> executableCode)
{
    CodeMapping mapping { Kind::LoadedSection, const_cast<uint8_t*>(executableCode.data()), executableCode.size() };
    // On Darwin the section is in a segment that the loader maps executable.
#if OS(FREEBSD)
    if (mprotect(mapping.m_code, mapping.m_size, PROT_READ | PROT_EXEC))
        return std::nullopt;
#elif OS(WINDOWS)
    MEMORY_BASIC_INFORMATION region;
    if (VirtualQuery(mapping.m_code, &region, sizeof(region)) && region.Protect == PAGE_EXECUTE_READ && static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize >= executableCode.data() + executableCode.size())
        return mapping;
    DWORD previousProtection;
    if (!VirtualProtect(mapping.m_code, mapping.m_size, PAGE_EXECUTE_READ, &previousProtection))
        return std::nullopt;
    FlushInstructionCache(GetCurrentProcess(), mapping.m_code, mapping.m_size);
#endif
    return mapping;
}

#if OS(LINUX)
std::optional<CodeMapping> CodeMapping::moveExecutableSegment(std::span<const uint8_t> code, size_t executableSize)
{
    void* moved = moveLoadedCode(code, executableSize);
    if (!moved)
        return std::nullopt;
    return CodeMapping { Kind::FileMapping, moved, code.size() };
}

#if !defined(BUN_HARDENED)
std::optional<CodeMapping> CodeMapping::copy(std::span<const uint8_t> code, size_t executableSize)
{
    void* anonymousCopy = copyCodeToAnonymousMemory(code, executableSize);
    if (!anonymousCopy)
        return std::nullopt;
    return CodeMapping { Kind::AnonymousCopy, anonymousCopy, code.size() };
}
#endif
#endif

void CodeMapping::release()
{
    switch (m_kind) {
    case Kind::FileMapping:
        unmapCode(m_code, m_size);
        return;
    case Kind::ExecutableSegment:
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

#if OS(LINUX) || OS(FREEBSD)
namespace {

struct PositionInFile {
    int descriptor { -1 };
    int64_t offset { 0 };
};

struct LoadedSegment {
    int64_t offsetInFile;
    uint64_t sizeFromAddress;
    uint32_t flags;
    bool isWhereItWasLinked;
};

struct SegmentSearch {
    uintptr_t address;
    std::optional<LoadedSegment> found;
};

} // anonymous namespace

// Where the image begins in a file that stays open: the file it was loaded from, or the executable whose loader mapped it.
static std::optional<PositionInFile> s_positionOfImage;

static int findSegmentContaining(struct dl_phdr_info* object, size_t, void* data)
{
    auto& search = *static_cast<SegmentSearch*>(data);
    for (unsigned index = 0; index < object->dlpi_phnum; ++index) {
        const auto& header = object->dlpi_phdr[index];
        uintptr_t start = object->dlpi_addr + header.p_vaddr;
        if (header.p_type != PT_LOAD || search.address < start || search.address - start >= header.p_filesz)
            continue;
        search.found = LoadedSegment { static_cast<int64_t>(header.p_offset + (search.address - start)), header.p_filesz - (search.address - start), header.p_flags, !object->dlpi_addr };
        return 1;
    }
    // The executable is the first object, and the image is in no other.
    return 1;
}

static std::optional<LoadedSegment> findLoadedSegment(const uint8_t* address)
{
    SegmentSearch search { reinterpret_cast<uintptr_t>(address), std::nullopt };
    dl_iterate_phdr(findSegmentContaining, &search);
    return search.found;
}

static void rememberPositionOfImage(std::span<const uint8_t> bytes, std::optional<ImageFile> file)
{
    if (file) {
        int descriptor = fcntl(static_cast<int>(file->descriptor), F_DUPFD_CLOEXEC, 0);
        if (descriptor >= 0)
            s_positionOfImage = PositionInFile { descriptor, file->offset };
        return;
    }
    std::optional<LoadedSegment> segment = findLoadedSegment(bytes.data());
    if (!segment)
        return;
    int descriptor = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
#if OS(LINUX)
    if (descriptor < 0) {
        if (auto* path = std::bit_cast<const char*>(getauxval(AT_EXECFN)))
            descriptor = open(path, O_RDONLY | O_CLOEXEC);
        std::array<uint8_t, offsetof(ImageHeader, codeSize) + sizeof(ImageHeader::codeSize)> header;
        if (descriptor >= 0 && (pread(descriptor, header.data(), header.size(), segment->offsetInFile) != static_cast<ssize_t>(header.size()) || memcmp(header.data(), bytes.data(), header.size()))) {
            close(descriptor);
            descriptor = -1;
        }
    }
#endif
    if (descriptor >= 0)
        s_positionOfImage = PositionInFile { descriptor, segment->offsetInFile };
}
#endif

static bool removeWriteAccessFromLoadedSection(std::span<const uint8_t> image)
{
#if OS(LINUX) || OS(FREEBSD)
    if (std::optional<LoadedSegment> segment = findLoadedSegment(image.data()); segment && !(segment->flags & PF_W))
        return true;
    return !mprotect(const_cast<uint8_t*>(image.data()), image.size(), PROT_READ);
#else
    UNUSED_PARAM(image);
    return true;
#endif
}

namespace {

// What the headers of the running executable, which its loader went by, say of the segment that holds an address.
struct SegmentOfExecutable {
    uint64_t sizeFromAddress;
    bool mayBeWritten;
    bool mayBeExecuted;
};

} // anonymous namespace

#if OS(LINUX) || OS(FREEBSD)

static constexpr bool dataHasSegmentsOfItsOwn = true;

static std::optional<SegmentOfExecutable> findSegmentOfExecutable(const uint8_t* address)
{
    std::optional<LoadedSegment> segment = findLoadedSegment(address);
    if (!segment)
        return std::nullopt;
    return SegmentOfExecutable { segment->sizeFromAddress, !!(segment->flags & PF_W), !!(segment->flags & PF_X) };
}

#elif OS(DARWIN)

// The payload of a Mach-O executable is one segment, which has the protection of the code in it.
static constexpr bool dataHasSegmentsOfItsOwn = false;

static std::optional<SegmentOfExecutable> findSegmentOfExecutable(const uint8_t* address)
{
    auto* executable = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    if (!executable || executable->magic != MH_MAGIC_64)
        return std::nullopt;
    uintptr_t slide = _dyld_get_image_vmaddr_slide(0);
    auto* command = reinterpret_cast<const uint8_t*>(executable + 1);
    for (uint32_t index = 0; index < executable->ncmds; ++index) {
        auto& segment = *reinterpret_cast<const segment_command_64*>(command);
        command += segment.cmdsize;
        if (segment.cmd != LC_SEGMENT_64)
            continue;
        uintptr_t start = segment.vmaddr + slide;
        uintptr_t position = reinterpret_cast<uintptr_t>(address);
        if (position < start || position - start >= segment.filesize)
            continue;
        return SegmentOfExecutable { segment.filesize - (position - start), !!((segment.initprot | segment.maxprot) & VM_PROT_WRITE), !!(segment.initprot & VM_PROT_EXECUTE) };
    }
    return std::nullopt;
}

#elif OS(WINDOWS)

// The payload of a PE executable is one section, which has the protection of the code in it.
static constexpr bool dataHasSegmentsOfItsOwn = false;

static std::optional<SegmentOfExecutable> findSegmentOfExecutable(const uint8_t* address)
{
    auto* executable = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!executable)
        return std::nullopt;
    auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(executable + reinterpret_cast<const IMAGE_DOS_HEADER*>(executable)->e_lfanew);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(headers);
    for (unsigned index = 0; index < headers->FileHeader.NumberOfSections; ++index) {
        const IMAGE_SECTION_HEADER& section = sections[index];
        const uint8_t* start = executable + section.VirtualAddress;
        if (address < start || static_cast<uint64_t>(address - start) >= section.Misc.VirtualSize)
            continue;
        return SegmentOfExecutable { section.Misc.VirtualSize - static_cast<uint64_t>(address - start), !!(section.Characteristics & IMAGE_SCN_MEM_WRITE), !!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) };
    }
    return std::nullopt;
}

#endif

static bool liesInOneSegmentOfExecutable(std::span<const uint8_t> range, bool isExecuted)
{
    if (range.empty())
        return !isExecuted;
    std::optional<SegmentOfExecutable> segment = findSegmentOfExecutable(range.data());
    if (!segment || range.size() > segment->sizeFromAddress || segment->mayBeWritten)
        return false;
    return segment->mayBeExecuted == isExecuted || (!isExecuted && !dataHasSegmentsOfItsOwn);
}

// The header of an image gets no more than the headers of the executable give: what it calls code was loaded to be executed, and nothing of it to be written.
static bool isLoadedAsItsHeaderSays(std::span<const uint8_t> bytes, const ImageHeader& header)
{
#if (OS(LINUX) || OS(FREEBSD)) && !defined(BUN_HARDENED)
    // An executable whose table of program headers has room for one more only has its whole payload in one segment, which is not executed.
    if (std::optional<SegmentOfExecutable> segment = findSegmentOfExecutable(bytes.data()); segment && header.size <= segment->sizeFromAddress && !segment->mayBeExecuted)
        return true;
#endif
    size_t endOfExecutedPart = header.codeOffset + header.executableSize;
    return liesInOneSegmentOfExecutable(bytes.first(header.codeOffset), false)
        && liesInOneSegmentOfExecutable(bytes.subspan(header.codeOffset, header.executableSize), true)
        && liesInOneSegmentOfExecutable(bytes.subspan(endOfExecutedPart, header.size - endOfExecutedPart), false);
}

static std::optional<CodeMapping> createCodeMapping(std::span<const uint8_t> bytes, std::optional<ImageFile> file, const ImageHeader& header, Wire::ImageVerdict& refusal)
{
    refusal = Wire::ImageVerdict::CannotMapCode;
    size_t size = header.size - header.codeOffset;
    size_t executableSize = header.executableSize;
#if OS(LINUX) && !defined(BUN_HARDENED)
    if (Options::useAOTPerfMap()) [[unlikely]]
        return CodeMapping::copy(bytes.subspan(header.codeOffset, size), executableSize);
#endif
    if (file)
        return CodeMapping::mapFile(*file, header.codeOffset, size, executableSize);
    std::span<const uint8_t> code = bytes.subspan(header.codeOffset, size);
#if OS(LINUX) || OS(FREEBSD)
    std::optional<LoadedSegment> segment = findLoadedSegment(code.data());
    bool isLoadedExecutable = segment && segment->flags & PF_X;
#if OS(LINUX)
    if (isLoadedExecutable && isAddressRandomizationDisabled())
        return CodeMapping::useExecutableSegment(code);
#endif
    if (s_positionOfImage && (!segment || segment->offsetInFile != s_positionOfImage->offset + static_cast<int64_t>(header.codeOffset))) {
        refusal = Wire::ImageVerdict::OutsideItsSegments;
        return std::nullopt;
    }
    std::optional<CodeMapping> mapping;
    if (s_positionOfImage)
        mapping = CodeMapping::mapFile({ s_positionOfImage->descriptor, s_positionOfImage->offset }, header.codeOffset, size, executableSize);
#if OS(LINUX)
    if (!mapping && isLoadedExecutable)
        mapping = CodeMapping::moveExecutableSegment(code, executableSize);
#endif
    if (mapping) {
        // Compiled code reads the constants in the code part through pointers into the loaded image.
#if defined(BUN_HARDENED)
        if (isLoadedExecutable)
            OSAllocator::protect(const_cast<uint8_t*>(code.data()), code.size(), true, false);
#else
        if (isLoadedExecutable)
            mprotect(const_cast<uint8_t*>(code.data()), code.size(), PROT_READ);
#endif
        return mapping;
    }
#if OS(LINUX)
    if (isLoadedExecutable && segment->isWhereItWasLinked) {
        refusal = Wire::ImageVerdict::CodeAtFixedAddress;
        return std::nullopt;
    }
#endif
    if (isLoadedExecutable)
        return CodeMapping::useExecutableSegment(code);
#endif
#if OS(LINUX)
    return std::nullopt;
#else
    return CodeMapping::protectLoadedSection(code.first(executableSize));
#endif
}

bool mapPrivateCloneOfImagePages(std::span<const uint8_t> pages, void* target)
{
    RELEASE_ASSERT(!(reinterpret_cast<uintptr_t>(pages.data()) % pageSize()) && !(pages.size() % pageSize()) && !(reinterpret_cast<uintptr_t>(target) % pageSize()));
#if OS(DARWIN)
    mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(target);
    vm_prot_t currentProtection;
    vm_prot_t maxProtection;
    kern_return_t result = mach_vm_remap(mach_task_self(), &address, pages.size(), 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
        reinterpret_cast<mach_vm_address_t>(pages.data()), true, &currentProtection, &maxProtection, VM_INHERIT_COPY);
    if (result != KERN_SUCCESS)
        return false;
    // The clone inherits the limits of the section, which nobody may write.
    return mach_vm_protect(mach_task_self(), address, pages.size(), true, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS
        && mach_vm_protect(mach_task_self(), address, pages.size(), false, VM_PROT_READ) == KERN_SUCCESS;
#elif OS(LINUX) || OS(FREEBSD)
    if (!s_positionOfImage)
        return false;
    int64_t offset = s_positionOfImage->offset + (pages.data() - reinterpret_cast<const uint8_t*>(&Image::current()->header()));
    return mmap(target, pages.size(), PROT_READ, MAP_PRIVATE | MAP_FIXED, s_positionOfImage->descriptor, offset) != MAP_FAILED;
#else
    UNUSED_PARAM(target);
    return false;
#endif
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
#else
    UNUSED_PARAM(image);
#endif
    // An image without fault sites still has returns that are diverted to a page without code.
    activateSignalHandlersFor(Signal::AccessFault);
#if OS(LINUX) && !defined(BUN_HARDENED)
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
    if (!file && !isLoadedAsItsHeaderSays(bytes, header))
        return ImageVerdict::OutsideItsSegments;
    if (hasFaultSites(header) && !tryReserveLowPages())
        return ImageVerdict::LowPagesAccessible;

    if (!relocateLoadedImage(bytes.first(header.size)))
        return ImageVerdict::CannotMapCode;
    if (!removeWriteAccessFromLoadedSection(bytes.first(header.size)))
        return ImageVerdict::CannotMapCode;
#if OS(LINUX) || OS(FREEBSD)
    rememberPositionOfImage(bytes, file);
#endif
    ImageVerdict refusal;
    std::optional<CodeMapping> mapping = createCodeMapping(bytes, file, header, refusal);
    if (!mapping)
        return refusal;
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

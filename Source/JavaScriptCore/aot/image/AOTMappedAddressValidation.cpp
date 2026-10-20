/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelocation.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/HexNumber.h>
#include <wtf/MathExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wtf/Scope.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>
#elif OS(WINDOWS)
#include <windows.h>
#elif OS(FREEBSD)
#include <array>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>
#endif

namespace JSC { namespace AOT {

static constexpr uint64_t userAddressSpaceSize = 1ull << 48;

namespace {

struct MappedRange {
    uintptr_t start;
    uintptr_t end;
};

} // anonymous namespace

[[maybe_unused]] static void appendRange(Vector<MappedRange>& ranges, uintptr_t start, uintptr_t end)
{
    if (!ranges.isEmpty() && ranges.last().end == start) {
        ranges.last().end = end;
        return;
    }
    ranges.append({ start, end });
}

#if OS(LINUX)

static Vector<char> readProcessMap()
{
    int descriptor = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return { };
    auto closeDescriptor = makeScopeExit([&] {
        close(descriptor);
    });

    constexpr size_t readSize = 64 * KB;
    Vector<char> text;
    while (true) {
        size_t length = text.size();
        text.grow(length + readSize);
        ssize_t count = read(descriptor, text.mutableSpan().subspan(length).data(), readSize);
        if (count < 0 && errno != EINTR)
            return { };
        text.shrink(length + std::max<ssize_t>(count, 0));
        if (!count)
            return text;
    }
}

static Vector<MappedRange> collectMappedRanges()
{
    constexpr size_t permissionCount = 4;
    Vector<char> text = readProcessMap();
    Vector<MappedRange> ranges;
    for (StringView line : StringView(text.span()).split('\n')) {
        size_t dash = line.find('-');
        size_t blank = line.find(' ');
        if (dash == notFound || blank == notFound || blank < dash || blank + permissionCount >= line.length())
            continue;
        auto start = parseInteger<uintptr_t>(line.left(dash), 16, WTF::ParseIntegerWhitespacePolicy::Disallow);
        auto end = parseInteger<uintptr_t>(line.substring(dash + 1, blank - dash - 1), 16, WTF::ParseIntegerWhitespacePolicy::Disallow);
        if (!start || !end)
            continue;
        if (line[blank + 1] == 'r')
            appendRange(ranges, *start, *end);
    }
    return ranges;
}

#elif OS(WINDOWS)

static Vector<MappedRange> collectMappedRanges()
{
    Vector<MappedRange> ranges;
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) {
        uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        uintptr_t end = start + region.RegionSize;
        if (end <= address)
            break;
        if (region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            appendRange(ranges, start, end);
        address = end;
    }
    return ranges;
}

#elif OS(FREEBSD)

static Vector<MappedRange> collectMappedRanges()
{
    std::array<int, 4> name { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, getpid() };
    size_t size = 0;
    if (sysctl(name.data(), name.size(), nullptr, &size, nullptr, 0) || !size)
        return { };
    Vector<uint8_t> buffer(size + size / 3);
    size = buffer.size();
    if (sysctl(name.data(), name.size(), buffer.mutableSpan().data(), &size, nullptr, 0))
        return { };

    Vector<MappedRange> ranges;
    std::span<const uint8_t> entries = buffer.span().first(size);
    while (!entries.empty()) {
        // The kernel cuts each entry off after the text of kve_path, so an entry is shorter than the struct.
        kinfo_vmentry entry { };
        memcpySpan(asMutableByteSpan(entry), entries.first(std::min(entries.size(), sizeof(entry))));
        if (entry.kve_structsize <= 0 || static_cast<size_t>(entry.kve_structsize) > entries.size())
            break;
        if (entry.kve_protection & KVME_PROT_READ)
            appendRange(ranges, entry.kve_start, entry.kve_end);
        entries = entries.subspan(entry.kve_structsize);
    }
    return ranges;
}

#else

static Vector<MappedRange> collectMappedRanges()
{
    return { };
}

#endif

static bool containsAddress(const Vector<MappedRange>& ranges, uintptr_t address)
{
    auto next = std::ranges::upper_bound(ranges, address, { }, &MappedRange::start);
    return next != ranges.begin() && address < std::prev(next)->end;
}

static String describeWord(uint64_t word, size_t offset)
{
    return makeString("0x"_s, hex(word, Lowercase), " at offset "_s, offset);
}

void validateNoMappedAddress(const LinkBuffer& linkBuffer, BuildContext& context)
{
    if constexpr (!mappedIntegersAreAddresses)
        return;
    auto isCandidate = [](const WideImmediate& immediate) {
        return immediate.value < userAddressSpaceSize;
    };
    const Vector<WideImmediate>& immediates = linkBuffer.wideImmediates();
    if (std::ranges::none_of(immediates, isCandidate))
        return;

    Vector<MappedRange> ranges = collectMappedRanges();
    for (const WideImmediate& immediate : immediates) {
        if (isCandidate(immediate) && containsAddress(ranges, immediate.value))
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode, .count = immediate.offset, .detail = describeWord(immediate.value, immediate.offset) });
    }
}

void validateNoMappedAddress(std::span<const uint8_t> data, BuildContext& context)
{
    if constexpr (!mappedIntegersAreAddresses)
        return;
    std::optional<Vector<MappedRange>> ranges;
    for (size_t offset = 0; offset + sizeof(uint64_t) <= data.size(); offset += sizeof(uint64_t)) {
        uint64_t word = WTF::unalignedLoad<uint64_t>(data.subspan(offset).data());
        if (word <= std::numeric_limits<uint32_t>::max() || word >= userAddressSpaceSize)
            continue;
        if (!ranges)
            ranges = collectMappedRanges();
        if (containsAddress(*ranges, word))
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::WritableSectionInImage, .count = clampTo<uint32_t>(offset), .detail = describeWord(word, offset) });
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

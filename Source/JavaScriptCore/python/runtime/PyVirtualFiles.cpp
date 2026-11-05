/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyVirtualFiles.h"

#include "PyCodeCache.h"
#include "PyStr.h"
#include <wtf/FileSystem.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>

#if !OS(WINDOWS)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

#if OS(LINUX)
#include <sys/syscall.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

static const VirtualFileSystem* s_virtualFileSystem;
// The first part of the root, "$bunfs": a path without it is not looked at any further.
static std::span<const char> s_virtualMarker;

void setVirtualFileSystem(const VirtualFileSystem* fileSystem)
{
    s_virtualFileSystem = fileSystem;
    s_virtualMarker = { };
    if (!fileSystem)
        return;
    auto root = fileSystem->root.span();
    ASSERT(root.size() > 1 && root[0] == '/' && root.back() != '/');
    size_t end = 1;
    while (end < root.size() && root[end] != '/')
        ++end;
    s_virtualMarker = root.subspan(1, end - 1);
}

const VirtualFileSystem* virtualFileSystem()
{
    return s_virtualFileSystem;
}

static constexpr unsigned maxLengthOfVirtualPath = 4095;

bool toVirtualPath(std::span<const char> path, VirtualPath& fromRoot)
{
    if (!s_virtualFileSystem || path.empty() || path[0] != '/')
        return false;
    auto root = s_virtualFileSystem->root.span();
    // A longer path is left to the system, which says of any such that the name is too long.
    if (path.size() < root.size() || path.size() > maxLengthOfVirtualPath || !WTF::contains(path, s_virtualMarker))
        return false;

    Vector<char, 256> normalized;
    bool namesDirectory = false;
    for (size_t start = 0; start <= path.size();) {
        size_t end = start;
        while (end < path.size() && path[end] != '/')
            ++end;
        auto part = path.subspan(start, end - start);
        start = end + 1;
        namesDirectory = true;
        if (part.empty() || (part.size() == 1 && part[0] == '.'))
            continue;
        if (part.size() == 2 && part[0] == '.' && part[1] == '.') {
            while (!normalized.isEmpty() && normalized.takeLast() != '/') { }
            continue;
        }
        namesDirectory = false;
        normalized.append('/');
        normalized.append(part);
    }
    if (normalized.size() < root.size() || !equalSpans(normalized.span().first(root.size()), root))
        return false;
    if (normalized.size() > root.size() && normalized[root.size()] != '/')
        return false;
    fromRoot.shrink(0);
    if (normalized.size() > root.size()) {
        fromRoot.append(normalized.span().subspan(root.size() + 1));
        if (namesDirectory)
            fromRoot.append('/');
    }
    return true;
}

bool toCachePath(std::span<const char> fromRoot, Vector<char, 256>& realPath)
{
    static constexpr auto name = "__pycache__"_span;
    bool isInCache = false;
    for (size_t start = 0; start < fromRoot.size() && !isInCache;) {
        size_t end = start;
        while (end < fromRoot.size() && fromRoot[end] != '/')
            ++end;
        isInCache = equalSpans(fromRoot.subspan(start, end - start), name);
        start = end + 1;
    }
    if (!isInCache || !s_virtualFileSystem->cacheDirectory)
        return false;

    // Empty: there is none.
    static NeverDestroyed<Vector<char, 256>> directoryOfBuild = [] {
        Vector<char, 256> directory;
        if (!s_virtualFileSystem->cacheDirectory(directory) || directory.isEmpty())
            return Vector<char, 256> { };
        directory.append('/');
        directory.append(byteCast<char>(hex(codeCacheStamp(), 16, Lowercase).span()));
        directory.append('/');
        return directory;
    }();
    if (directoryOfBuild->isEmpty())
        return false;
    realPath.shrink(0);
    realPath.appendVector(directoryOfBuild.get());
    realPath.append(fromRoot);
    return true;
}

bool makeParentDirectories(std::span<const char> path)
{
#if OS(WINDOWS)
    UNUSED_PARAM(path);
    return false;
#else
    size_t length = path.size();
    while (length && path[length - 1] == '/')
        --length;
    while (length && path[length - 1] != '/')
        --length;
    while (length > 1 && path[length - 1] == '/')
        --length;
    if (!length)
        return true;
    Vector<char, 256> parent;
    parent.append(path.first(length));
    parent.append('\0');
    if (!mkdir(parent.span().data(), 0777) || errno == EEXIST)
        return true;
    if (errno != ENOENT || length == 1 || !makeParentDirectories(path.first(length)))
        return false;
    return !mkdir(parent.span().data(), 0777) || errno == EEXIST;
#endif
}

// What is at the path, which loses the slash at its end. If nothing is, or not what the slash asks for, errno says so.
static VirtualFileKind lookUp(std::span<const char>& fromRoot, uint64_t& size)
{
    bool namesDirectory = !fromRoot.empty() && fromRoot.back() == '/';
    if (namesDirectory)
        fromRoot = fromRoot.first(fromRoot.size() - 1);
    VirtualFileKind kind = s_virtualFileSystem->kindOf(fromRoot, size);
    if (kind == VirtualFileKind::File && namesDirectory) {
        errno = ENOTDIR;
        return VirtualFileKind::None;
    }
    if (kind != VirtualFileKind::None)
        return kind;
    // Nothing is in a file.
    errno = ENOENT;
    for (auto parent = fromRoot; !parent.empty();) {
        size_t length = parent.size();
        while (length && parent[length - 1] != '/')
            --length;
        parent = parent.first(length ? length - 1 : 0);
        uint64_t sizeOfParent;
        VirtualFileKind kindOfParent = s_virtualFileSystem->kindOf(parent, sizeOfParent);
        if (kindOfParent == VirtualFileKind::None)
            continue;
        if (kindOfParent == VirtualFileKind::File)
            errno = ENOTDIR;
        break;
    }
    return VirtualFileKind::None;
}

bool isVirtualPath(std::span<const char> path)
{
    VirtualPath fromRoot;
    return toVirtualPath(path, fromRoot);
}

// The system refuses a path of so many bytes, as toVirtualPath() does, and each unit of the text takes a byte at least.
bool isTooLongForAPath(unsigned length)
{
#if OS(WINDOWS)
    constexpr unsigned limit = 32768;
#else
    constexpr unsigned limit = std::max<unsigned>(PATH_MAX, maxLengthOfVirtualPath + 1);
#endif
    if (length < limit)
        return false;
    errno = ENAMETOOLONG;
    return true;
}

VirtualFileKind kindOfFile(const String& path)
{
    if (isTooLongForAPath(path.length())) [[unlikely]]
        return VirtualFileKind::None;
    auto bytes = encodeFilesystem(path);
    if (s_virtualFileSystem) {
        VirtualPath fromRoot;
        if (toVirtualPath(byteCast<char>(bytes.span()), fromRoot)) {
            auto virtualPath = fromRoot.span();
            uint64_t size;
            return lookUp(virtualPath, size);
        }
    }
#if !OS(WINDOWS)
    bytes.append(0);
    struct stat status;
    if (stat(byteCast<char>(bytes.span()).data(), &status) < 0)
        return VirtualFileKind::None;
    return S_ISDIR(status.st_mode) ? VirtualFileKind::Directory : VirtualFileKind::File;
#else
    auto type = FileSystem::fileTypeFollowingSymlinks(path);
    if (!type)
        return VirtualFileKind::None;
    return *type == FileSystem::FileType::Directory ? VirtualFileKind::Directory : VirtualFileKind::File;
#endif
}

std::optional<Vector<uint8_t>> readFile(const String& path)
{
    if (isTooLongForAPath(path.length())) [[unlikely]]
        return std::nullopt;
    auto bytes = encodeFilesystem(path);
    if (s_virtualFileSystem) {
        VirtualPath fromRoot;
        if (Vector<char, 256> realPath; toVirtualPath(byteCast<char>(bytes.span()), fromRoot) && toCachePath(fromRoot.span(), realPath))
            return readFile(decodeFilesystem(byteCast<uint8_t>(realPath.span())));
        if (toVirtualPath(byteCast<char>(bytes.span()), fromRoot)) {
            auto virtualPath = fromRoot.span();
            uint64_t size;
            VirtualFileKind kind = lookUp(virtualPath, size);
            Vector<uint8_t> contents;
            if (kind == VirtualFileKind::File && s_virtualFileSystem->read(virtualPath, contents))
                return contents;
            if (kind != VirtualFileKind::None)
                errno = kind == VirtualFileKind::Directory ? EISDIR : EIO;
            return std::nullopt;
        }
    }
#if !OS(WINDOWS)
    // Not FileSystem::readEntireFile(): the name of a file need not be UTF-8.
    bytes.append(0);
    int fd;
    do {
        fd = open(byteCast<char>(bytes.span()).data(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return std::nullopt;
    Vector<uint8_t> contents;
    if (struct stat status; !fstat(fd, &status) && S_ISREG(status.st_mode) && status.st_size > 0)
        contents.reserveInitialCapacity(static_cast<size_t>(status.st_size));
    while (true) {
        uint8_t buffer[64 * KB];
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            int errorNumber = errno;
            close(fd);
            errno = errorNumber;
            return std::nullopt;
        }
        if (!count)
            break;
        contents.append(std::span<const uint8_t> { buffer, static_cast<size_t>(count) });
    }
    close(fd);
    return contents;
#else
    errno = 0;
    auto contents = FileSystem::readEntireFile(path);
    // That answers the same for a file with nothing in it as for one it cannot read.
    if (!contents) {
        int errorNumber = errno;
        if (auto size = FileSystem::fileSize(path); size && !*size)
            return Vector<uint8_t> { };
        errno = errorNumber ? errorNumber : ENOENT;
    }
    return contents;
#endif
}

RefPtr<ThreadSafeRefCountedFixedVector<uint8_t>> readFileToKeep(const String& path, size_t padding)
{
    using Contents = ThreadSafeRefCountedFixedVector<uint8_t>;
    auto make = [&](size_t size) {
        Ref contents = Contents::create(padding + size);
        zeroSpan(contents->span().first(padding));
        return contents;
    };
    if (isTooLongForAPath(path.length())) [[unlikely]]
        return nullptr;
#if !OS(WINDOWS)
    auto bytes = encodeFilesystem(path);
    Vector<char, 256> realPath;
    if (VirtualPath fromRoot; s_virtualFileSystem && toVirtualPath(byteCast<char>(bytes.span()), fromRoot)) {
        bytes.clear();
        if (toCachePath(fromRoot.span(), realPath))
            bytes.append(byteCast<uint8_t>(realPath.span()));
    }
    // (Empty: in the virtual file system itself.)
    if (!bytes.isEmpty()) {
        bytes.append(0);
        int fd;
        do {
            fd = open(byteCast<char>(bytes.span()).data(), O_RDONLY | O_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
            return nullptr;
        // Anything else has no size to go by.
        if (struct stat status; !fstat(fd, &status) && S_ISREG(status.st_mode) && status.st_size >= 0 && static_cast<uint64_t>(status.st_size) < std::numeric_limits<uint32_t>::max() - padding) {
            Ref contents = make(static_cast<size_t>(status.st_size));
            auto rest = contents->span().subspan(padding);
            while (!rest.empty()) {
                ssize_t count = read(fd, rest.data(), rest.size());
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0) {
                    // Shorter than it was a moment ago: somebody is writing it.
                    int errorNumber = count ? errno : EIO;
                    close(fd);
                    errno = errorNumber;
                    return nullptr;
                }
                rest = rest.subspan(count);
            }
            close(fd);
            return contents;
        }
        close(fd);
    }
#endif
    auto read = readFile(path);
    if (!read)
        return nullptr;
    Ref contents = make(read->size());
    memcpySpan(contents->span().subspan(padding), read->span());
    return contents;
}

#if !OS(WINDOWS)

// No other file system has it, or os.path.samefile() could be fooled.
static constexpr dev_t virtualDevice = 0x62756e; // "bun"

static uint64_t virtualInode(std::span<const char> fromRoot)
{
    // FNV-1a
    uint64_t hash = 14695981039346656037ull;
    for (char byte : fromRoot) {
        hash ^= static_cast<uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

int virtualStat(std::span<const char> fromRoot, struct stat& status)
{
    uint64_t size = 0;
    VirtualFileKind kind = lookUp(fromRoot, size);
    if (kind == VirtualFileKind::None)
        return -1;
    zeroBytes(status);
    bool isDirectory = kind == VirtualFileKind::Directory;
    status.st_mode = isDirectory ? S_IFDIR | 0555 : S_IFREG | 0444;
    status.st_nlink = isDirectory ? 2 : 1;
    status.st_size = isDirectory ? 0 : static_cast<off_t>(size);
    status.st_ino = static_cast<ino_t>(virtualInode(fromRoot));
    status.st_dev = virtualDevice;
    status.st_blksize = 4096;
    status.st_blocks = static_cast<blkcnt_t>((status.st_size + 511) / 512);
    time_t time = static_cast<time_t>(s_virtualFileSystem->modificationTime);
    status.st_atime = time;
    status.st_mtime = time;
    status.st_ctime = time;
    return 0;
}

int virtualAccess(std::span<const char> fromRoot, int mode)
{
    uint64_t size;
    VirtualFileKind kind = lookUp(fromRoot, size);
    if (kind == VirtualFileKind::None)
        return -1;
    if (mode & W_OK) {
        errno = EROFS;
        return -1;
    }
    if ((mode & X_OK) && kind == VirtualFileKind::File) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int errorForVirtualChange(std::span<const char> fromRoot, VirtualChange change)
{
    uint64_t size;
    if (lookUp(fromRoot, size) != VirtualFileKind::None)
        return change == VirtualChange::MakesNew ? EEXIST : EROFS;
    if (change == VirtualChange::OfExisting || errno != ENOENT)
        return errno;
    size_t length = fromRoot.size();
    while (length && fromRoot[length - 1] != '/')
        --length;
    return s_virtualFileSystem->kindOf(fromRoot.first(length ? length - 1 : 0), size) == VirtualFileKind::Directory ? EROFS : ENOENT;
}

bool virtualList(std::span<const char> fromRoot, Vector<VirtualDirectoryEntry>& entries)
{
    uint64_t size;
    VirtualFileKind kind = lookUp(fromRoot, size);
    if (kind != VirtualFileKind::Directory) {
        if (kind == VirtualFileKind::File)
            errno = ENOTDIR;
        return false;
    }
    s_virtualFileSystem->list(fromRoot, entries);
    VirtualPath path;
    path.append(fromRoot);
    if (!path.isEmpty())
        path.append('/');
    size_t directoryLength = path.size();
    for (auto& entry : entries) {
        path.shrink(directoryLength);
        path.append(byteCast<char>(entry.name.span()));
        entry.inode = virtualInode(path.span());
    }
    return true;
}

static bool writeAll(int fd, std::span<const uint8_t> bytes)
{
    while (!bytes.empty()) {
        ssize_t written = write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    return true;
}

// What fstat() says of the copy is what stat() says of the file, but for where it is.
static bool fillCopy(int fd, std::span<const uint8_t> contents)
{
    struct timespec time { static_cast<time_t>(s_virtualFileSystem->modificationTime), 0 };
    struct timespec times[2] { time, time };
    return writeAll(fd, contents) && !fchmod(fd, 0444) && !futimens(fd, times);
}

// A descriptor for reading, which children do not inherit, of a file without a name that holds the bytes. A file can only be
// filled through a descriptor for writing, so it is opened a second time.
static int openCopy(std::span<const uint8_t> contents)
{
#if OS(LINUX) && defined(SYS_memfd_create)
    // By number: the function is newer than the oldest C library this runs on. 1 is MFD_CLOEXEC.
    int memoryFile = static_cast<int>(syscall(SYS_memfd_create, "python", 1u));
    if (memoryFile >= 0) {
        if (fillCopy(memoryFile, contents)) {
            char path[32];
            snprintf(path, sizeof(path), "/proc/self/fd/%d", memoryFile);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                close(memoryFile);
                return fd;
            }
            // Where there is no /proc, the descriptor that can write will have to do: the copy is nobody else's.
            if (!lseek(memoryFile, 0, SEEK_SET))
                return memoryFile;
        }
        close(memoryFile);
    }
#endif
    const char* directory = getenv("TMPDIR");
    auto name = makeString(String::fromUTF8(directory && *directory ? directory : "/tmp"), "/python-XXXXXX"_s).utf8();
    Vector<char, 128> path;
    path.append(byteCast<char>(name.spanIncludingNullTerminator()));
    int temporaryFile = mkostemp(path.mutableSpan().data(), O_CLOEXEC);
    if (temporaryFile < 0)
        return -1;
    int fd = fillCopy(temporaryFile, contents) ? open(path.span().data(), O_RDONLY | O_CLOEXEC) : -1;
    int errorNumber = errno;
    unlink(path.span().data());
    close(temporaryFile);
    errno = errorNumber;
    return fd;
}

int virtualOpen(std::span<const char> fromRoot, int flags)
{
    uint64_t size;
    VirtualFileKind kind = lookUp(fromRoot, size);
    if (kind == VirtualFileKind::None) {
        if (errno == ENOENT && (flags & O_CREAT))
            errno = EROFS;
        return -1;
    }
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        errno = EEXIST;
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC)) {
        errno = kind == VirtualFileKind::Directory ? EISDIR : EROFS;
        return -1;
    }
    if (kind == VirtualFileKind::Directory) {
        // FIXME: There are no descriptors of virtual directories, so nothing that takes a dir_fd works on them (os.fwalk()).
        errno = flags & O_DIRECTORY ? ENOTSUP : EISDIR;
        return -1;
    }
    if (flags & O_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }

    Vector<uint8_t> contents;
    if (!s_virtualFileSystem->read(fromRoot, contents)) {
        errno = EIO;
        return -1;
    }
    return openCopy(contents.span());
}

int openFile(const char* path, int flags, mode_t mode)
{
    VirtualPath fromRoot;
    if (toVirtualPath(std::span<const char> { path, strlen(path) }, fromRoot)) {
        Vector<char, 256> realPath;
        if (!toCachePath(fromRoot.span(), realPath))
            return virtualOpen(fromRoot.span(), flags);
        realPath.append('\0');
        return open(realPath.span().data(), flags, mode);
    }
    return open(path, flags, mode);
}

#endif // !OS(WINDOWS)

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

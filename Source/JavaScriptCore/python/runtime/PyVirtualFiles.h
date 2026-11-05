/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/RefCountedFixedVector.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

#if !OS(WINDOWS)
#include <sys/stat.h>
#endif

namespace JSC {
namespace Python {

// Files that are on no disk: the host has them (Bun: the standard library, inside the executable), and says under which
// directory they appear to be. The lowest layer of the runtime serves them (posix, _io.FileIO, the importer), so everything
// above it, which is Python code that has never heard of this, finds ordinary files that cannot be written to.

enum class VirtualFileKind : uint8_t { None, File, Directory };

struct VirtualDirectoryEntry {
    UTF8CString name;
    VirtualFileKind kind;
    uint64_t inode { 0 }; // Not for the host to fill in.
};

// The paths the functions are given are relative to the root and normalized: "python/lib/python3.15/os.py"; "" is the root.
struct VirtualFileSystem {
    // Absolute and normalized: "/$bunfs". Its parent is a real directory, and nothing real is at or under it.
    ASCIILiteral root;
    // sys.prefix, under the root: "/$bunfs/python". The standard library is in <prefix>/lib/python3.15.
    ASCIILiteral prefix;
    // A directory of modules of the host's own, written in Python. It is not on sys.path: they are frozen modules (PyImp.cpp). Null: none.
    ASCIILiteral hostLibrary;
    // When every file was last changed, in seconds since the epoch.
    int64_t modificationTime;
    VirtualFileKind (*kindOf)(std::span<const char> path, uint64_t& size);
    // False: there is no such file.
    bool (*read)(std::span<const char> path, Vector<uint8_t>& contents);
    // What is in a directory.
    void (*list)(std::span<const char> path, Vector<VirtualDirectoryEntry>&);
    // A real directory, this user's, where what was worked out from the files may be kept from one run to the next. Absolute,
    // without a slash at the end; it need not exist yet. False, or no function: there is none.
    bool (*cacheDirectory)(Vector<char, 256>& path) { nullptr };
};

// Before Python first runs. It is never freed.
JS_EXPORT_PRIVATE void setVirtualFileSystem(const VirtualFileSystem*);
const VirtualFileSystem* virtualFileSystem();

using VirtualPath = Vector<char, 128>;
// Whether that is where the path leads, once "//", "." and ".." are out of it, and the path from the root if it is, for the
// functions below. (It keeps a slash at the end, which only the name of a directory may have.)
// A relative path never leads there: no process is in a directory the system does not know.
bool toVirtualPath(std::span<const char> path, VirtualPath& fromRoot);
bool isVirtualPath(std::span<const char> path);

// The one thing that is written next to the files of a library is the compiled code of its modules, in directories named
// __pycache__. Under the root these are real directories, somewhere else: in the host's cache directory, apart for each build.
// Whether the path, which toVirtualPath() made, is such a directory or in one, and where that is.
bool toCachePath(std::span<const char> fromRoot, Vector<char, 256>& realPath);
// mkdir -p of the directory the path is in. False: errno says why.
bool makeParentDirectories(std::span<const char> path);

// ---- For what reads files without going through posix, the host too ----

// Follows symbolic links.
JS_EXPORT_PRIVATE VirtualFileKind kindOfFile(const String& path);
// Nothing: errno says why.
JS_EXPORT_PRIVATE std::optional<Vector<uint8_t>> readFile(const String& path);
// The same into memory that several can hold on to, after `padding` bytes of nothing: for contents that are used where they are, part
// of which has to lie at a certain alignment. A file of the system is read straight into it.
RefPtr<ThreadSafeRefCountedFixedVector<uint8_t>> readFileToKeep(const String& path, size_t padding);

#if !OS(WINDOWS)

// ---- As the system calls of those names: -1, and errno says why ----

int virtualStat(std::span<const char> fromRoot, struct stat&);
int virtualAccess(std::span<const char> fromRoot, int mode);
// A descriptor like any other, on a copy of the contents that belongs to nobody else.
int virtualOpen(std::span<const char> fromRoot, int flags);
// False: it is not a directory.
bool virtualList(std::span<const char> fromRoot, Vector<VirtualDirectoryEntry>&);

// The errno of a call that would change something: for what is wrong with the path, as on a disk, or else EROFS.
enum class VirtualChange : uint8_t { OfExisting, MakesNew, MakesOrReplaces };
int errorForVirtualChange(std::span<const char> fromRoot, VirtualChange);

// open(2), of a path that may be virtual.
int openFile(const char* path, int flags, mode_t);

#endif // !OS(WINDOWS)

} // namespace Python
} // namespace JSC

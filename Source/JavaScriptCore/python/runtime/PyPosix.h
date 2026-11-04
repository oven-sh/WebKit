/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include "PyVirtualFiles.h"

#if !OS(WINDOWS)

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace JSC {
namespace Python {

// What the files of the module posix share with each other, and with the other modules that talk to the operating system:
// pwd, grp, fcntl, resource. (Modules/posixmodule.c in CPython.)

// ---- Errors ----

// OSError(errno, strerror(errno)), or the subclass that goes with the number.
JSValue posixError(JSGlobalObject*);
// The current directory of the process is another: the host is told (setDidChangeDirectoryFunction).
void posixDidChangeDirectory(JSGlobalObject*);
JSValue posixError(JSGlobalObject*, int errorNumber);

// ---- Results ----

template<typename Integer>
JSValue posixMakeInt(JSGlobalObject* globalObject, Integer value)
{
    if constexpr (std::is_signed_v<Integer>)
        return makeInt(globalObject, static_cast<int64_t>(value));
    else
        return makeInt(globalObject, static_cast<uint64_t>(value));
}

// (uid_t)-1, which stands for "leave it as it is", is -1.
JSValue posixFromUid(JSGlobalObject*, uid_t);
JSValue posixFromGid(JSGlobalObject*, gid_t);
JSValue posixFromDev(JSGlobalObject*, dev_t);
// NODEV, without the rest of <sys/param.h>.
static constexpr dev_t posixNoDevice = static_cast<dev_t>(-1);

// What the system calls a file, as a str (os.fsdecode) or as bytes.
JSValue posixDecode(JSGlobalObject*, std::span<const char>);
JSValue posixDecode(JSGlobalObject*, const char* nulTerminated);
JSValue posixFileName(JSGlobalObject*, std::span<const char>, bool asBytes);

// ---- Arguments ----
// All return false having raised an exception.

// What "f() argument 1 must be str, not x" calls the type of the argument: None is "None".
String posixTypeNameOfArgument(JSGlobalObject*, JSValue);

// int; long, which off_t is; long long and Py_ssize_t, which differ from it in their messages.
bool posixToInt(JSGlobalObject*, JSValue, int&);
bool posixToLong(JSGlobalObject*, JSValue, int64_t&);
bool posixToLongLong(JSGlobalObject*, JSValue, int64_t&);
bool posixToSsize(JSGlobalObject*, JSValue, int64_t&);
bool posixToUnsigned(JSGlobalObject*, JSValue, unsigned&);
bool posixToUnsignedLongLong(JSGlobalObject*, JSValue, uint64_t&);
// Flags: the bits that do not fit are dropped, with a DeprecationWarning.
bool posixToUnsignedMask(JSGlobalObject*, JSValue, unsigned&);
inline bool posixToOffset(JSGlobalObject* globalObject, JSValue value, off_t& result)
{
    int64_t number;
    if (!posixToLong(globalObject, value, number))
        return false;
    result = number;
    return true;
}
inline bool posixToPid(JSGlobalObject* globalObject, JSValue value, pid_t& result)
{
    static_assert(sizeof(pid_t) == sizeof(int));
    int number;
    if (!posixToInt(globalObject, value, number))
        return false;
    result = number;
    return true;
}
// A count of bytes that must not be negative: ValueError, "<name> cannot be negative".
bool posixToCount(JSGlobalObject*, JSValue, ASCIILiteral name, int64_t&);
bool posixToUid(JSGlobalObject*, JSValue, uid_t&);
bool posixToGid(JSGlobalObject*, JSValue, gid_t&);
bool posixToDev(JSGlobalObject*, JSValue, dev_t&);
// An int, or anything with a fileno() method. (PyObject_AsFileDescriptor)
bool posixToFileDescriptor(JSGlobalObject*, JSValue, int&);
// dir_fd: absent or None is AT_FDCWD.
bool posixToDirFd(JSGlobalObject*, JSValue, int&);
// An argument that is absent is the default.
inline bool posixToBool(JSGlobalObject* globalObject, JSValue value, bool defaultValue, bool& result)
{
    if (!value) {
        result = defaultValue;
        return true;
    }
    result = isTrue(globalObject, value);
    return !getVM(globalObject).exceptionForInspection();
}

// os.fspath(): a str or bytes. Empty: an exception.
JSValue posixFSPath(JSGlobalObject*, JSValue);
// Bytes to hand to the system as a C string: there is a NUL after them.
class PosixBytes {
public:
    void assign(std::span<const char> bytes)
    {
        m_buffer.shrink(0);
        m_buffer.append(bytes);
        m_buffer.append('\0');
    }
    const char* characters() const { return m_buffer.span().data(); }
    size_t length() const { return m_buffer.isEmpty() ? 0 : m_buffer.size() - 1; }
    std::span<const char> span() const { return m_buffer.span().first(length()); }
    bool contains(char byte) const { return !!memchr(m_buffer.span().data(), byte, length()); }

private:
    Vector<char, 128> m_buffer;
};
// os.fsencode(), without a NUL in it. (PyUnicode_FSConverter)
bool posixToFSBytes(JSGlobalObject*, JSValue, PosixBytes&);
// The same for a str alone, which the caller has checked.
bool posixEncodeStr(JSGlobalObject*, JSString*, PosixBytes&);

// Whether obj[0] may mean something: it has __getitem__ and is not a dict. (PySequence_Check)
bool posixIsSequence(JSGlobalObject*, JSValue);

// Memory that can be written to. `function` and `position` are for the TypeError.
bool posixGetWritableBuffer(JSGlobalObject*, JSValue, ASCIILiteral function, unsigned position, BufferHolder&);

// A path: str, bytes, or what has __fspath__, and for some functions an open file descriptor or None. (path_t)
class PosixPath {
public:
    enum Option : unsigned {
        AllowFd = 1 << 0,
        Nullable = 1 << 1,
        Nonstrict = 1 << 2, // A NUL in it is fine.
        // What becomes of a path into the virtual file system (PyVirtualFiles.h). Unless it is allowed, convert() refuses it as
        // a file system that cannot be written to does: OSError, EROFS if nothing is wrong with the path itself.
        AllowVirtual = 1 << 3, // The function looks at isVirtual(), or does not go to the file system.
        VirtualUnsupported = 1 << 4, // The function changes nothing, but there is no doing it there: ENOTSUP.
        MakesNew = 1 << 5, // Nothing may be at the path,
        MakesOrReplaces = 1 << 6, // or nothing need be: otherwise that is what is wrong with it.
        AsText = 1 << 7, // The function only looks at the characters: a str is not encoded, so whatever is in it is fine. No narrow() then.
    };

    explicit PosixPath(ASCIILiteral function, ASCIILiteral argument = "path"_s, unsigned options = 0)
        : m_function(function)
        , m_argument(argument)
        , m_options(options)
    {
    }
    PosixPath(ASCIILiteral function, unsigned options)
        : PosixPath(function, "path"_s, options)
    {
    }

    bool convert(JSGlobalObject*, JSValue);

    // Null for a file descriptor and for None.
    const char* narrow() const { return m_hasNarrow ? m_narrow.characters() : nullptr; }
    std::span<const char> span() const { return m_narrow.span(); }
    bool isFd() const { return m_isFd; }
    int fd() const { return m_fd; }
    bool isVirtual() const { return m_isVirtual; }
    // From the root of the virtual file system.
    std::span<const char> virtualPath() const { return m_virtualPath.span(); }
    // The str or bytes (of __fspath__(), if that was called), the int, or None.
    JSValue object() const { return m_object; }
    // Whether the results are to be bytes too.
    bool isBytes() const { return PyBytes::is(m_object); }
    ASCIILiteral function() const { return m_function; }

    // OSError from errno, with the path as the filename.
    JSValue error(JSGlobalObject*) const;
    JSValue error(JSGlobalObject*, const PosixPath& second) const;

private:
    ASCIILiteral m_function;
    ASCIILiteral m_argument;
    unsigned m_options;
    bool m_hasNarrow { false };
    bool m_isFd { false };
    bool m_isVirtual { false };
    int m_fd { -1 };
    JSValue m_object;
    PosixBytes m_narrow;
    VirtualPath m_virtualPath;
};

// Combinations of arguments that make no sense. True: a ValueError was raised.
bool posixPathAndDirFdInvalid(JSGlobalObject*, ASCIILiteral function, const PosixPath&, int dirFd);
bool posixDirFdAndFdInvalid(JSGlobalObject*, ASCIILiteral function, int dirFd, int fd);
bool posixFdAndFollowSymlinksInvalid(JSGlobalObject*, ASCIILiteral function, bool isFd, bool followSymlinks);
bool posixDirFdAndFollowSymlinksInvalid(JSGlobalObject*, ASCIILiteral function, int dirFd, bool followSymlinks);

// (seconds, nanoseconds) from an int or a float, rounded down. (_PyTime_ObjectToTimespec)
bool posixToTimespec(JSGlobalObject*, JSValue, time_t& seconds, long& nanoseconds);

// ---- Types ----

// The type made from the definition, the same one each time.
PyType* cachedStructSeqType(JSGlobalObject*, const StructSeqDef&);
// Values the modules keep for themselves, by name.
PyDict* posixState(JSGlobalObject*);

PyType* posixStatResultType(JSGlobalObject*);
JSValue posixStatResult(JSGlobalObject*, const struct stat&);
// resource.struct_rusage
PyType* posixRusageType(JSGlobalObject*);

// ---- File descriptors ----

// False: errno says why.
bool posixSetInheritable(int fd, bool inheritable);
// A system call has failed, and errno says why. If there are too many open files: those that nothing refers to any more (files,
// sockets, directory iterators) are closed only once the collector has noticed, so it is made to look. True: try the call again.
bool posixFreeDescriptors(JSGlobalObject*);
// A copy that children do not inherit. -1: errno says why.
int posixDupNotInheritable(int fd);

// ---- The parts of the module ----

void addPosixConstants(JSGlobalObject*, PyModule*); // PyPosixConstants.cpp
void addPosixFileFunctions(JSGlobalObject*, PyModule*); // PyPosixFiles.cpp
void addPosixDescriptorFunctions(JSGlobalObject*, PyModule*); // PyPosixIO.cpp
void addPosixProcessFunctions(JSGlobalObject*, PyModule*); // PyPosixProcess.cpp
// A child that runs the first of the executables that can be run, with the descriptors, signals and so on that exec() leaves it,
// or else exits with status 127. A null environment is that of this process. -1: there is no child, and errno says why.
pid_t posixSpawnChild(char* const* executables, char* const* arguments, char* const* environment); // PyPosixSubprocess.cpp

struct PosixConstant {
    ASCIILiteral name;
    int64_t value;
};
void addPosixConstants(JSGlobalObject*, PyModule*, std::span<const PosixConstant>);
// {name: value}
PyDict* posixConstantDict(JSGlobalObject*, std::span<const PosixConstant>);

} // namespace Python
} // namespace JSC

#endif // !OS(WINDOWS)

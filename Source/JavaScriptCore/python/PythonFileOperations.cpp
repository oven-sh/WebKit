/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonFileOperations.h"

#if OS(UNIX)
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace JSC { namespace Python {

#if OS(UNIX)

template<typename T>
static T resultOrError(T result)
{
    return result < 0 ? static_cast<T>(-errno) : result;
}

const FileOperations* systemFileOperations()
{
    static constexpr FileOperations operations {
        [] (const CString& path, int flags, int mode) { return resultOrError(::open(path.data(), flags, mode)); },
        [] (int descriptor) { return resultOrError(::close(descriptor)); },
        [] (int descriptor, std::span<uint8_t> buffer) { return resultOrError<int64_t>(::read(descriptor, buffer.data(), buffer.size())); },
        [] (int descriptor, std::span<const uint8_t> bytes) { return resultOrError<int64_t>(::write(descriptor, bytes.data(), bytes.size())); },
        [] (int descriptor, int64_t offset, int whence) { return resultOrError<int64_t>(::lseek(descriptor, offset, whence)); },
        [] (int descriptor, int64_t size) { return resultOrError(::ftruncate(descriptor, size)); },
        [] (int descriptor, FileStatus& status) {
            struct stat result;
            if (::fstat(descriptor, &result) < 0)
                return -errno;
            status.mode = result.st_mode;
            status.size = result.st_size;
            status.blockSize = result.st_blksize;
            return 0;
        },
        [] (int descriptor) { return !!::isatty(descriptor); },
        // set_inheritable() of CPython's Python/fileutils.c
        [] (int descriptor, bool isInheritable) {
            int flags = ::fcntl(descriptor, F_GETFD);
            if (flags < 0)
                return -errno;
            int wanted = isInheritable ? flags & ~FD_CLOEXEC : flags | FD_CLOEXEC;
            if (wanted == flags)
                return 0;
            return resultOrError(::fcntl(descriptor, F_SETFD, wanted));
        },
    };
    return &operations;
}

#else

// FIXME: Windows.
const FileOperations* systemFileOperations()
{
    return nullptr;
}

#endif

} } // namespace JSC::Python

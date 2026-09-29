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


#pragma once

#include <span>
#include <wtf/text/CString.h>

namespace JSC { namespace Python {

// What is asked of the system about open files, by io.FileIO and by the functions of `os` that do the same. Whoever embeds the engine can have it asked of itself instead: Configuration::files. It may want
// what is written to the standard output to go the way that what JavaScript writes there goes, so that the two come out in the order that they were written in.
//
// Each does what the system call of the name does and gives what that gives. If it failed, that is what errno was, negated. None of them tries again when it is interrupted: that is seen to by what calls them,
// which has something to look to first.

struct FileStatus {
    uint32_t mode { 0 }; // st_mode
    int64_t size { 0 }; // st_size
    int64_t blockSize { 0 }; // st_blksize
};

struct FileOperations {
    int (*open)(const CString& path, int flags, int mode);
    int (*close)(int descriptor);
    int64_t (*read)(int descriptor, std::span<uint8_t>);
    int64_t (*write)(int descriptor, std::span<const uint8_t>);
    int64_t (*seek)(int descriptor, int64_t offset, int whence);
    int (*truncate)(int descriptor, int64_t size);
    int (*status)(int descriptor, FileStatus&);
    bool (*isTerminal)(int descriptor);
    int (*setInheritable)(int descriptor, bool);
};

// By asking the system. Null where there is no way written to.
JS_EXPORT_PRIVATE const FileOperations* systemFileOperations();

} } // namespace JSC::Python

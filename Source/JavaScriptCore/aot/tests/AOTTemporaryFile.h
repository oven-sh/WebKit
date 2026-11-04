/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include <span>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class TemporaryFile {
    WTF_MAKE_NONCOPYABLE(TemporaryFile);
public:
    // Only Cocoa's createTemporaryFile() takes a suffix. The file without it keeps the name from anybody else.
    TemporaryFile(ASCIILiteral suffix, std::span<const uint8_t> contents)
        : m_pathWithoutSuffix(FileSystem::createTemporaryFile("testaot"_s, { }))
        , m_path(makeString(m_pathWithoutSuffix, suffix))
    {
        AOT_TEST_CHECK(!m_pathWithoutSuffix.isNull());
        AOT_TEST_CHECK(FileSystem::overwriteEntireFile(m_path, contents) == contents.size());
    }

    ~TemporaryFile()
    {
        FileSystem::deleteFile(m_path);
        FileSystem::deleteFile(m_pathWithoutSuffix);
    }

    const String& path() const { return m_path; }

private:
    String m_pathWithoutSuffix;
    String m_path;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

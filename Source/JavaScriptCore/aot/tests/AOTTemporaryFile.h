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
#include <wtf/SHA1.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class TemporaryFile {
    WTF_MAKE_NONCOPYABLE(TemporaryFile);
public:
    TemporaryFile(ASCIILiteral suffix, std::span<const uint8_t> contents)
        : m_pathWithoutSuffix(FileSystem::createTemporaryFile("testaot"_s, { }))
        , m_ownPath(makeString(m_pathWithoutSuffix, suffix))
        , m_path(pathSharedByTests(suffix, contents))
    {
        AOT_TEST_CHECK(!m_pathWithoutSuffix.isNull());
        if (!m_path.isNull() && FileSystem::fileExists(m_path))
            return;
        AOT_TEST_CHECK(FileSystem::overwriteEntireFile(m_ownPath, contents) == contents.size());
        if (m_path.isNull() || !(FileSystem::hardLink(m_ownPath, m_path) || FileSystem::fileExists(m_path)))
            m_path = m_ownPath;
    }

    ~TemporaryFile()
    {
        FileSystem::deleteFile(m_ownPath);
        FileSystem::deleteFile(m_pathWithoutSuffix);
    }

    const String& path() const { return m_path; }
    const String& ownPath() const { return m_ownPath; }

private:
    static String pathSharedByTests(ASCIILiteral suffix, std::span<const uint8_t> contents)
    {
        const char* folder = getenv("AOT_TEST_IMAGES");
        if (!folder || suffix != ".image"_s)
            return { };
        SHA1 digest;
        digest.addBytes(contents);
        return FileSystem::pathByAppendingComponent(String::fromUTF8(folder), makeString(digest.computeHexDigest(), suffix));
    }

    String m_pathWithoutSuffix;
    String m_ownPath;
    String m_path;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

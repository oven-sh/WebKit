/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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
#include "SourceProvider.h"

#include <numeric>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/ProcessID.h>
#include <wtf/text/MakeString.h>

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(StringSourceProvider);

SourceProvider::SourceProvider(const SourceOrigin& sourceOrigin, String&& sourceURL, String&& preRedirectURL, SourceTaintedOrigin taintedness, const TextPosition& startPosition, SourceProviderSourceType sourceType)
    : m_sourceType(sourceType)
    , m_sourceOrigin(sourceOrigin)
    , m_sourceURL(WTF::move(sourceURL))
    , m_preRedirectURL(WTF::move(preRedirectURL))
    , m_startPosition(startPosition)
    , m_taintedness(taintedness)
{
}

SourceProvider::~SourceProvider() = default;

void SourceProvider::lockUnderlyingBuffer()
{
    if (!m_lockingCount++)
        lockUnderlyingBufferImpl();
}

void SourceProvider::unlockUnderlyingBuffer()
{
    if (!--m_lockingCount)
        unlockUnderlyingBufferImpl();
}

CodeBlockHash SourceProvider::codeBlockHashConcurrently(int startOffset, int endOffset, CodeSpecializationKind kind)
{
    auto entireSourceCode = source();
    return CodeBlockHash { entireSourceCode.substring(startOffset, endOffset - startOffset), entireSourceCode, kind };
}

void SourceProvider::lockUnderlyingBufferImpl() { }

void SourceProvider::unlockUnderlyingBufferImpl() { }

void SourceProvider::getID()
{
    if (!m_id) {
        static std::atomic<SourceID> nextProviderID = nullID;
        m_id = ++nextProviderID;
        RELEASE_ASSERT(m_id);
    }
}

const String& SourceProvider::sourceURLStripped()
{
    if (m_sourceURL.isNull()) [[unlikely]]
        return m_sourceURLStripped;
    if (!m_sourceURLStripped.isNull()) [[likely]]
        return m_sourceURLStripped;
    m_sourceURLStripped = URL(m_sourceURL).strippedForUseAsReport();
    return m_sourceURLStripped;
}

UTF8CString SourceProvider::sourceCodeDumpFilePath(const UTF8CString& dumpDirectory)
{
    if (m_sourceCodeDumped.load(std::memory_order_acquire)) {
        Locker locker { m_sourceCodeDumpLock };
        return m_sourceCodeDumpFilePath;
    }

    Locker locker { m_sourceCodeDumpLock };
    if (m_sourceCodeDumped.load(std::memory_order_relaxed))
        return m_sourceCodeDumpFilePath;

    auto tryExtractLocalPath = [](const String& urlString) -> String {
        if (urlString.isNull())
            return { };
        if (urlString.startsWith('/'))
            return urlString;
        if (urlString.startsWith("file://"_s))
            return URL(urlString).fileSystemPath();
        return { };
    };

    String localPath = tryExtractLocalPath(sourceURL());

    if (!localPath.isNull())
        m_sourceCodeDumpFilePath = FileSystem::fileSystemRepresentation(localPath);
    else {
        auto baseName = makeString("source-"_s, asID(), '-', WTF::getCurrentProcessID());
        String filePath;
        FileSystem::FileHandle handle;
        if (dumpDirectory.isNull()) {
            auto result = FileSystem::openTemporaryFile(baseName, ".js"_s);
            filePath = result.first;
            handle = WTF::move(result.second);
        } else {
            filePath = makeString(dumpDirectory, FileSystem::pathSeparator, baseName, ".js"_s);
            handle = FileSystem::openFile(filePath, FileSystem::FileOpenMode::Truncate);
        }
        if (handle) {
            auto sourceText = source().utf8();
            handle.write(WTF::asByteSpan(sourceText.span()));
            handle.flush();
            m_sourceCodeDumpFilePath = FileSystem::fileSystemRepresentation(filePath);
        }
    }

    m_sourceCodeDumped.store(true, std::memory_order_release);
    return m_sourceCodeDumpFilePath;
}

#if ENABLE(WEBASSEMBLY)
BaseWebAssemblySourceProvider::BaseWebAssemblySourceProvider(const SourceOrigin& sourceOrigin, String&& sourceURL)
    : SourceProvider(sourceOrigin, WTF::move(sourceURL), String(), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::WebAssembly)
{
}
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// Where each line of a text starts, in about one byte per line:
//
//     u32 lineCount
//     { u32 lineStart, u32 streamOffset }   one per block of linesPerBlock lines
//     stream                                for each line but the last of its block: its length, terminator included, as a LEB128
//
// Every number is little-endian and read a byte at a time, so the bytes need no alignment.
class EncodedLineStarts {
public:
    static constexpr unsigned linesPerBlock = 64;

    struct Line {
        unsigned line0Based;
        unsigned start;
    };

    explicit EncodedLineStarts(std::span<const uint8_t> bytes)
        : m_bytes(bytes)
        , m_lineCount(load32(0))
    {
    }

    unsigned lineCount() const { return m_lineCount; }

    // An offset past the end of the text clamps to the last line rather than being refused, because
    // callers reach here from error reporting, where an approximate answer beats none.
    Line lineContaining(unsigned offset) const
    {
        unsigned low = 0;
        unsigned high = blockCount();
        while (high - low > 1) {
            unsigned middle = std::midpoint(low, high);
            if (blockLineStart(middle) <= offset)
                low = middle;
            else
                high = middle;
        }
        return walk(low, [&](Line next) { return next.start <= offset; });
    }

    unsigned startOfLine(unsigned line0Based) const
    {
        ASSERT(line0Based < m_lineCount);
        return walk(line0Based / linesPerBlock, [&](Line next) { return next.line0Based <= line0Based; }).start;
    }

private:
    unsigned load32(size_t at) const
    {
        uint32_t value;
        memcpySpan(asMutableByteSpan(value), m_bytes.subspan(at, sizeof(value)));
        return value;
    }

    unsigned blockCount() const { return (m_lineCount + linesPerBlock - 1) / linesPerBlock; }
    unsigned blockLineStart(unsigned block) const { return load32(sizeof(uint32_t) * (1 + 2 * block)); }
    unsigned blockStreamOffset(unsigned block) const { return load32(sizeof(uint32_t) * (2 + 2 * block)); }

    // The last line of the block that `accepts` takes, from the block's first line on.
    template<typename Functor>
    Line walk(unsigned block, const Functor& accepts) const
    {
        Line line { block * linesPerBlock, blockLineStart(block) };
        unsigned end = std::min(line.line0Based + linesPerBlock, m_lineCount);
        const uint8_t* cursor = m_bytes.data() + sizeof(uint32_t) * (1 + 2 * blockCount()) + blockStreamOffset(block);
        while (line.line0Based + 1 < end) {
            unsigned length = *cursor & 0x7f;
            for (unsigned shift = 7; *cursor++ & 0x80; shift += 7)
                length |= (*cursor & 0x7f) << shift;
            Line next { line.line0Based + 1, line.start + length };
            if (!accepts(next))
                break;
            line = next;
        }
        return line;
    }

    std::span<const uint8_t> m_bytes;
    unsigned m_lineCount;
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

template<typename CharType>
Vector<uint8_t> LineStartTable::build(std::span<const CharType> text)
{
    Vector<uint32_t> header;
    Vector<uint8_t> stream;
    header.append(0); // lineCount, below
    header.append(0);
    header.append(0);

    const CharType* const begin = text.data();
    const CharType* const end = std::to_address(text.end());
    unsigned lineCount = 1;
    size_t index = 0;
    while (index < text.size()) {
        const CharType* found = findLineTerminator(text.subspan(index));
        if (found == end)
            break;
        size_t next = lineStartAfterTerminator(text, static_cast<size_t>(found - begin));
        if (lineCount % EncodedLineStarts::linesPerBlock) {
            unsigned length = static_cast<unsigned>(next - index);
            for (; length >= 0x80; length >>= 7)
                stream.append(static_cast<uint8_t>(length | 0x80));
            stream.append(static_cast<uint8_t>(length));
        } else {
            header.append(static_cast<uint32_t>(next));
            header.append(static_cast<uint32_t>(stream.size()));
        }
        ++lineCount;
        index = next;
    }
    header[0] = lineCount;

    Vector<uint8_t> encoded;
    encoded.reserveInitialCapacity(header.sizeInBytes() + stream.size());
    encoded.append(asByteSpan(header.span()));
    encoded.appendVector(stream);
    return encoded;
}

Vector<uint8_t> LineStartTable::encode(StringView text)
{
    return text.is8Bit() ? build(text.span8()) : build(text.span16());
}

void LineStartTable::setEncoded(std::span<const uint8_t> encoded)
{
    Locker locker { m_lock };
    m_encoded = encoded;
}

std::span<const uint8_t> LineStartTable::ensureBuilt(StringView text)
{
    if (m_encoded.empty()) {
        m_owned = encode(text);
        m_encoded = m_owned.span();
    }
    return m_encoded;
}

static unsigned lineEndFor(StringView text, const EncodedLineStarts& lineStarts, unsigned line0Based)
{
    unsigned length = text.length();
    // A non-final line's end comes from the next line's start, which is past the terminator, so the
    // terminator has to be backed over.
    unsigned lineEnd = (line0Based + 1 < lineStarts.lineCount()) ? lineStarts.startOfLine(line0Based + 1) : length;
    if (lineEnd < length) {
        if (lineEnd >= 2 && isCRLFPair(text[lineEnd - 2], text[lineEnd - 1]))
            lineEnd -= 2;
        else
            lineEnd -= 1;
    }
    return lineEnd;
}

static LineStartTable::PositionInfo positionWithoutLineEnd(const EncodedLineStarts& lineStarts, unsigned offset)
{
    auto line = lineStarts.lineContaining(offset);
    return {
        line.line0Based,
        offset > line.start ? offset - line.start : 0,
        line.start,
        0,
    };
}

LineStartTable::PositionInfo LineStartTable::lineAndColumnForOffset(StringView text, unsigned offset)
{
    Locker locker { m_lock };
    return positionWithoutLineEnd(EncodedLineStarts { ensureBuilt(text) }, offset);
}

LineStartTable::PositionInfo LineStartTable::positionInfoForOffset(StringView text, unsigned offset)
{
    Locker locker { m_lock };
    EncodedLineStarts lineStarts { ensureBuilt(text) };
    auto info = positionWithoutLineEnd(lineStarts, offset);
    info.lineEnd = lineEndFor(text, lineStarts, info.line0Based);
    return info;
}

unsigned LineStartTable::offsetForPosition(StringView text, unsigned line0Based, unsigned column0Based)
{
    Locker locker { m_lock };
    EncodedLineStarts lineStarts { ensureBuilt(text) };

    if (line0Based >= lineStarts.lineCount())
        return text.length();

    return std::min(lineStarts.startOfLine(line0Based) + column0Based, lineEndFor(text, lineStarts, line0Based));
}

} // namespace JSC


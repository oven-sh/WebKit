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

#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/ProcessID.h>
#include <wtf/UnalignedAccess.h>
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

// Where each line of a text starts, in about one byte per line:
//
//     u32 lineCount
//     { u32 lineStart, u32 streamOffset }   one per block of linesPerBlock lines
//     stream                                for each line but the last of its block: its length, terminator included, as a LEB128
//
// The u32s are in the byte order of the machine, and the bytes need no alignment. The bytecode cache holds these bytes as
// they are: a change to the layout comes with a new cachedTypesFormatRevision.
//
// A lookup is what the first stack trace through a call site pays for each of its frames, so this reads the bytes
// directly. With checked spans and WTF::LEBDecoder such a stack trace of 12 frames executed 20% more instructions than
// when the code had its lines and columns, and it executes 8% more with this.
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

class EncodedLineStarts {
public:
    static constexpr unsigned linesPerBlock = 64;

    struct Line {
        unsigned line0Based;
        unsigned start;
    };

    explicit EncodedLineStarts(std::span<const uint8_t> bytes)
        : m_blocks(bytes.data() + sizeof(uint32_t))
        , m_lineCount(WTF::unalignedLoad<uint32_t>(bytes.data()))
    {
    }

    unsigned lineCount() const { return m_lineCount; }

    // An offset past the end of the text clamps to the last line rather than being refused, because
    // callers reach here from error reporting, where an approximate answer beats none.
    Line lineContaining(unsigned offset) const
    {
        unsigned block = 0;
        for (unsigned count = blockCount(); count > 1;) {
            unsigned half = count / 2;
            block += blockLineStart(block + half) <= offset ? half : 0;
            count -= half;
        }
        return walk(block, [&](Line next) { return next.start <= offset; });
    }

    unsigned startOfLine(unsigned line0Based) const
    {
        ASSERT(line0Based < m_lineCount);
        return walk(line0Based / linesPerBlock, [&](Line next) { return next.line0Based <= line0Based; }).start;
    }

private:
    unsigned blockCount() const { return (m_lineCount + linesPerBlock - 1) / linesPerBlock; }
    unsigned blockLineStart(unsigned block) const { return WTF::unalignedLoad<uint32_t>(m_blocks + 2 * sizeof(uint32_t) * block); }
    unsigned blockStreamOffset(unsigned block) const { return WTF::unalignedLoad<uint32_t>(m_blocks + 2 * sizeof(uint32_t) * block + sizeof(uint32_t)); }

    // The last line of the block that `accepts` takes, from the block's first line on.
    template<typename Functor>
    ALWAYS_INLINE Line walk(unsigned block, const Functor& accepts) const
    {
        Line line { block * linesPerBlock, blockLineStart(block) };
        unsigned end = std::min(line.line0Based + linesPerBlock, m_lineCount);
        const uint8_t* cursor = m_blocks + 2 * sizeof(uint32_t) * blockCount() + blockStreamOffset(block);
        while (line.line0Based + 1 < end) {
            unsigned length = *cursor++;
            if (length & 0x80) [[unlikely]] {
                length &= 0x7f;
                for (unsigned shift = 7;; shift += 7) {
                    unsigned byte = *cursor++;
                    length |= (byte & 0x7f) << shift;
                    if (!(byte & 0x80))
                        break;
                }
            }
            Line next { line.line0Based + 1, line.start + length };
            if (!accepts(next))
                break;
            line = next;
        }
        return line;
    }

    const uint8_t* m_blocks;
    unsigned m_lineCount;
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

template<typename CharType>
Vector<unsigned> LineStartTable::build(std::span<const CharType> text)
{
    Vector<unsigned> lineStarts;
    lineStarts.append(0);

    const CharType* const begin = text.data();
    const CharType* const end = std::to_address(text.end());
    size_t index = 0;
    while (index < text.size()) {
        const CharType* found = findLineTerminator(text.subspan(index));
        if (found == end)
            break;
        size_t next = lineStartAfterTerminator(text, static_cast<size_t>(found - begin));
        lineStarts.append(static_cast<unsigned>(next));
        index = next;
    }

    return lineStarts;
}

LineStarts LineStartTable::encode(const Vector<unsigned>& lineStarts)
{
    Vector<uint32_t> header { static_cast<uint32_t>(lineStarts.size()) };
    Vector<uint8_t> stream;
    for (size_t line = 0; line < lineStarts.size(); ++line) {
        if (line % EncodedLineStarts::linesPerBlock) {
            unsigned length = lineStarts[line] - lineStarts[line - 1];
            for (; length >= 0x80; length >>= 7)
                stream.append(static_cast<uint8_t>(length | 0x80));
            stream.append(static_cast<uint8_t>(length));
        } else {
            header.append(lineStarts[line]);
            header.append(static_cast<uint32_t>(stream.size()));
        }
    }

    auto headerBytes = asByteSpan(header.span());
    Ref owner = ThreadSafeRefCountedFixedVector<uint8_t>::create(headerBytes.size() + stream.size());
    memcpySpan(owner->span().first(headerBytes.size()), headerBytes);
    memcpySpan(owner->span().subspan(headerBytes.size()), stream.span());
    return { owner->span(), WTF::move(owner) };
}

const LineStarts& LineStartTable::ensureBuilt(StringView text)
{
    if (!m_lineStarts)
        m_lineStarts = encode(text.is8Bit() ? build(text.span8()) : build(text.span16()));
    return m_lineStarts;
}

LineStarts LineStartTable::lineStarts(StringView text)
{
    Locker locker { m_lock };
    return ensureBuilt(text);
}

LineStarts LineStartTable::lineStartsIfBuilt() const
{
    Locker locker { m_lock };
    return m_lineStarts;
}

void LineStartTable::setLineStarts(LineStarts&& lineStarts)
{
    Locker locker { m_lock };
    if (!m_lineStarts)
        m_lineStarts = WTF::move(lineStarts);
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

LineColumn LineStartTable::lineColumnForOffset(StringView text, unsigned offset)
{
    Locker locker { m_lock };
    auto line = EncodedLineStarts { ensureBuilt(text).bytes }.lineContaining(offset);
    return { line.line0Based, offset - line.start };
}

LineStartTable::PositionInfo LineStartTable::positionInfoForOffset(StringView text, unsigned offset)
{
    Locker locker { m_lock };
    EncodedLineStarts lineStarts { ensureBuilt(text).bytes };
    auto line = lineStarts.lineContaining(offset);
    return {
        line.line0Based,
        offset - line.start,
        line.start,
        lineEndFor(text, lineStarts, line.line0Based),
    };
}

unsigned LineStartTable::offsetForPosition(StringView text, unsigned line0Based, unsigned column0Based)
{
    Locker locker { m_lock };
    EncodedLineStarts lineStarts { ensureBuilt(text).bytes };

    if (line0Based >= lineStarts.lineCount())
        return text.length();

    return std::min(lineStarts.startOfLine(line0Based) + column0Based, lineEndFor(text, lineStarts, line0Based));
}

LineColumn BuiltinsSourceProvider::lineColumnInTextForOffset(unsigned offset)
{
    unsigned start = *(std::ranges::upper_bound(m_starts, offset) - 1);
    auto text = source().span8().subspan(start, offset - start);
    unsigned line = 0;
    size_t lineStart = 0;
    while (lineStart < text.size()) {
        const Latin1Character* found = findLineTerminator(text.subspan(lineStart));
        if (found == std::to_address(text.end()))
            break;
        lineStart = lineStartAfterTerminator(text, static_cast<size_t>(found - text.data()));
        ++line;
    }
    return { line, static_cast<unsigned>(text.size() - lineStart) };
}

} // namespace JSC


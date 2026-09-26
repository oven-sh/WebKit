/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include <JavaScriptCore/CachedBytecode.h>
#include <JavaScriptCore/CodeBlockHash.h>
#include <JavaScriptCore/CodeSpecializationKind.h>
#include <JavaScriptCore/LineColumn.h>
#include <JavaScriptCore/SourceCharacters.h>
#include <JavaScriptCore/SourceOrigin.h>
#include <JavaScriptCore/SourceTaintedOrigin.h>
#include <atomic>
#include <span>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefCountedFixedVector.h>
#include <wtf/Vector.h>
#include <wtf/text/TextPosition.h>
#include <wtf/text/WTFString.h>
#include <JavaScriptCore/ArgList.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

namespace JSC {

class SourceCode;
class SourceCodeKey;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class UnlinkedFunctionCodeBlock;
class VM;

enum class JS_EXPORT_PRIVATE SourceProviderSourceType : uint8_t {
    Program,
    Module,
    WebAssembly,
    JSON,
    Text,
    Synthetic,
    ImportMap,
#if USE(BUN_JSC_ADDITIONS)
    BunTranspiledModule,
#endif
};

using BytecodeCacheGenerator = Function<RefPtr<CachedBytecode>()>;

// What a line start table holds: see EncodedLineStarts. The sources that have one text and the code compiled from it
// share the bytes. Those of code out of a bytecode cache that stays mapped are borrowed from it, and have no owner.
struct LineStarts {
    std::span<const uint8_t> bytes;
    RefPtr<ThreadSafeRefCountedFixedVector<uint8_t>> owner;

    explicit operator bool() const { return !bytes.empty(); }
};

class LineStartTable {
    WTF_MAKE_NONCOPYABLE(LineStartTable);
public:
    LineStartTable() = default;

    // Positions within the provider's own text. An inline <script> starts partway into its
    // document, and that offset is not applied here.
    struct PositionInfo {
        unsigned line0Based { 0 };
        unsigned column0Based { 0 };
        unsigned lineStart { 0 };
        unsigned lineEnd { 0 }; // excludes the line terminator, so [lineStart, lineEnd) is the text
    };

    JS_EXPORT_PRIVATE PositionInfo positionInfoForOffset(StringView text, unsigned offset);
    // Out-of-range input clamps rather than fails: a line past the end gives the end of the text,
    // and a column past the end of its line gives that line's end.
    JS_EXPORT_PRIVATE unsigned offsetForPosition(StringView text, unsigned line0Based, unsigned column0Based);

    // The zero-based line and column of positionInfoForOffset(). Unlike it, this reads none of the text if isBuilt().
    JS_EXPORT_PRIVATE LineColumn lineColumnForOffset(StringView text, unsigned offset);

    static constexpr unsigned linesPerBlock = 64;
    // A table costs an allocation and a few reference counts, whatever its size. That is 3% of what it takes to
    // evaluate 100 characters, and a scan of so few is as cheap, so a short text pays only if it is asked for a position.
    static constexpr unsigned minimumLengthToCollect = 1024;

    // Collects where the lines of a text start, from whoever reads the text: the lexer while it parses, or scan().
    class Builder {
    public:
        Builder()
            : m_header({ 0, 0, 0 })
        {
        }

        unsigned lastLineStart() const { return m_lastLineStart; }
        unsigned lineCount() const { return m_lineCount; }

        // The line after the last one starts at lineStart.
        ALWAYS_INLINE void append(unsigned lineStart)
        {
            ASSERT(lineStart > m_lastLineStart);
            unsigned length = lineStart - m_lastLineStart;
            m_lastLineStart = lineStart;
            if (m_lineCount++ % linesPerBlock && length < 0x80) [[likely]] {
                m_stream.append(static_cast<uint8_t>(length));
                return;
            }
            appendSlow(length);
        }

        // Appends the lines whose terminator is in text between begin and end.
        template<typename CharType> void scan(std::span<const CharType> text, size_t begin, size_t end);
        void scan(StringView text, size_t begin, size_t end);

        // Once the rest of text is scanned: what is before readTo has been read, though the last line may be long.
        LineStarts finish(StringView text, unsigned readTo);

    private:
        JS_EXPORT_PRIVATE void appendSlow(unsigned length);

        // A short text is done without an allocation.
        Vector<uint32_t, 3> m_header;
        Vector<uint8_t, 64> m_stream;
        unsigned m_lineCount { 1 };
        unsigned m_lastLineStart { 0 };
    };

    bool isBuilt() const { return m_isBuilt.load(std::memory_order_acquire); }
    // Empty if !isBuilt().
    LineStarts lineStarts() const { return isBuilt() ? m_lineStarts : LineStarts { }; }
    // Of this table's text. The first ones stay.
    JS_EXPORT_PRIVATE void setLineStarts(LineStarts&&);

private:
    const LineStarts& ensureBuilt(StringView);

    Lock m_lock; // For who sets m_lineStarts, which does not change once m_isBuilt says that it is there.
    std::atomic<bool> m_isBuilt { false };
    LineStarts m_lineStarts;
};

class JS_EXPORT_PRIVATE SourceProvider : public ThreadSafeRefCounted<SourceProvider> {
public:
    static const intptr_t nullID = 1;

    JS_EXPORT_PRIVATE SourceProvider(const SourceOrigin&, String&& sourceURL, String&& preRedirectURL, SourceTaintedOrigin, const TextPosition& startPosition, SourceProviderSourceType);
    JS_EXPORT_PRIVATE virtual ~SourceProvider();

    JS_EXPORT_PRIVATE virtual unsigned hash() const = 0;
    JS_EXPORT_PRIVATE virtual StringView source() const = 0;
    JS_EXPORT_PRIVATE virtual RefPtr<CachedBytecode> cachedBytecode() const { return nullptr; }
    JS_EXPORT_PRIVATE virtual void cacheBytecode(const BytecodeCacheGenerator&) const { }
    JS_EXPORT_PRIVATE virtual void updateCache(const UnlinkedFunctionExecutable*, const SourceCode&, CodeSpecializationKind, const UnlinkedFunctionCodeBlock*) const { }
    JS_EXPORT_PRIVATE virtual void commitCachedBytecode() const { }
#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE virtual size_t memoryCost() const { return 0; }
    JS_EXPORT_PRIVATE virtual void didGenerateUnlinkedCodeBlock(VM&, const SourceCodeKey&, UnlinkedCodeBlock*) const { }
#endif

    StringView getRange(int start, int end) const LIFETIME_BOUND
    {
        return source().substring(start, end - start);
    }

    const SourceOrigin& sourceOrigin() const LIFETIME_BOUND { return m_sourceOrigin; }

    // This is NOT the path that should be used for computing relative paths from a script. Use SourceOrigin's URL for that, the values may or may not be the same...
    const String& sourceURL() const LIFETIME_BOUND { return m_sourceURL; }
    const String& sourceURLStripped();
    const String& preRedirectURL() const LIFETIME_BOUND { return m_preRedirectURL; }
    const String& sourceURLDirective() const LIFETIME_BOUND { return m_sourceURLDirective; }
    const String& sourceMappingURLDirective() const LIFETIME_BOUND { return m_sourceMappingURLDirective; }

    JS_EXPORT_PRIVATE TextPosition startPosition() const { return m_startPosition; }
    JS_EXPORT_PRIVATE SourceProviderSourceType sourceType() const { return m_sourceType; }
    bool isModuleType() const
    {
        switch (m_sourceType) {
        case SourceProviderSourceType::Module:
        case SourceProviderSourceType::JSON:
        case SourceProviderSourceType::Text:
#if USE(BUN_JSC_ADDITIONS)
        case SourceProviderSourceType::BunTranspiledModule:
#endif
            return true;
        default:
            return false;
        }
    }

    SourceID asID()
    {
        if (!m_id)
            getID();
        return m_id;
    }

    void setSourceURLDirective(const String& sourceURLDirective) { m_sourceURLDirective = sourceURLDirective; }
    void setSourceMappingURLDirective(const String& sourceMappingURLDirective) { m_sourceMappingURLDirective = sourceMappingURLDirective; }
    void setSourceTaintedOrigin(SourceTaintedOrigin taintedness) { m_taintedness = taintedness; }

    SourceTaintedOrigin sourceTaintedOrigin() const { return m_taintedness; }
    bool couldBeTainted() const { return m_taintedness != SourceTaintedOrigin::Untainted; }

    JS_EXPORT_PRIVATE void lockUnderlyingBuffer();
    JS_EXPORT_PRIVATE void unlockUnderlyingBuffer();
    JS_EXPORT_PRIVATE virtual CodeBlockHash codeBlockHashConcurrently(int startOffset, int endOffset, CodeSpecializationKind);

    virtual bool isScriptBufferSourceProvider() const { return false; }

    JS_EXPORT_PRIVATE UTF8CString sourceCodeDumpFilePath(const UTF8CString& dumpDirectory);

    LineStartTable::PositionInfo positionInfoForOffset(unsigned offset)
    {
        return m_lineStartTable.positionInfoForOffset(source(), offset);
    }

    unsigned offsetForPosition(unsigned line0Based, unsigned column0Based)
    {
        return m_lineStartTable.offsetForPosition(source(), line0Based, column0Based);
    }

    // Zero-based, in the provider's own text.
    virtual LineColumn lineColumnInTextForOffset(unsigned offset)
    {
        return m_lineStartTable.lineColumnForOffset(source(), offset);
    }

    // An inline <script> shifts every line of its document, but shifts the column only on its first
    // line, since later lines begin where their own line begins.
    LineColumn documentLineColumn(LineColumn inText) const
    {
        return {
            m_startPosition.m_line.oneBasedInt() + inText.line,
            inText.line ? inText.column + 1 : m_startPosition.m_column.oneBasedInt() + inText.column,
        };
    }

    LineColumn documentLineColumnForOffset(unsigned offset)
    {
        return documentLineColumn(lineColumnInTextForOffset(offset));
    }

    LineColumn documentZeroBasedLineColumnForOffset(unsigned offset)
    {
        auto inText = lineColumnInTextForOffset(offset);
        return {
            m_startPosition.m_line.zeroBasedInt() + inText.line,
            inText.line ? inText.column : m_startPosition.m_column.zeroBasedInt() + inText.column,
        };
    }

    bool lineStartTableIsBuilt() const { return m_lineStartTable.isBuilt(); }
    // The code compiled from a text has them too, and gives them to a source that has that text and is not parsed.
    LineStarts lineStarts() const { return m_lineStartTable.lineStarts(); }
    void setLineStarts(LineStarts&& lineStarts) { m_lineStartTable.setLineStarts(WTF::move(lineStarts)); }
    // Whether a parse of the text, or of a part of it, is to collect them. What it does not read is scanned.
    virtual bool wantsLineStarts() const { return !m_lineStartTable.isBuilt() && source().length() >= LineStartTable::minimumLengthToCollect; }

private:
    JS_EXPORT_PRIVATE virtual void lockUnderlyingBufferImpl();
    JS_EXPORT_PRIVATE virtual void unlockUnderlyingBufferImpl();

    JS_EXPORT_PRIVATE void NODELETE getID();

    std::atomic<unsigned> m_lockingCount { 0 };
    SourceProviderSourceType m_sourceType;
    SourceOrigin m_sourceOrigin;
    String m_sourceURL;
    String m_sourceURLStripped;
    String m_preRedirectURL;
    String m_sourceURLDirective;
    String m_sourceMappingURLDirective;
    TextPosition m_startPosition;
    SourceID m_id { 0 };
    SourceTaintedOrigin m_taintedness;

    std::atomic<bool> m_sourceCodeDumped { false };
    Lock m_sourceCodeDumpLock;
    UTF8CString m_sourceCodeDumpFilePath WTF_GUARDED_BY_LOCK(m_sourceCodeDumpLock);

    LineStartTable m_lineStartTable;
};

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(StringSourceProvider);
class StringSourceProvider : public SourceProvider {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(StringSourceProvider, StringSourceProvider);
public:
    static Ref<StringSourceProvider> create(const String& source, const SourceOrigin& sourceOrigin, String sourceURL, SourceTaintedOrigin taintedness, const TextPosition& startPosition = TextPosition(), SourceProviderSourceType sourceType = SourceProviderSourceType::Program)
    {
        return adoptRef(*new StringSourceProvider(source, sourceOrigin, taintedness, WTF::move(sourceURL), startPosition, sourceType));
    }

    unsigned hash() const override
    {
        return m_source.get().hash();
    }

    StringView source() const override
    {
        return m_source.get();
    }

protected:
    StringSourceProvider(const String& source, const SourceOrigin& sourceOrigin, SourceTaintedOrigin taintedness, String&& sourceURL, const TextPosition& startPosition, SourceProviderSourceType sourceType)
        : SourceProvider(sourceOrigin, WTF::move(sourceURL), String(), taintedness, startPosition, sourceType)
        , m_source(source.isNull() ? *StringImpl::empty() : *source.impl())
    {
    }

private:
    const Ref<StringImpl> m_source;
};

// The texts of many builtins, one after the other. None of them knows of the others, so a line and column in one
// counts from where it starts. That takes no table: a builtin is short, and few of them are ever asked about.
class BuiltinsSourceProvider final : public StringSourceProvider {
public:
    // Where each builtin starts, ascending, the first at 0. It has to outlive the provider.
    static Ref<BuiltinsSourceProvider> create(const String& source, std::span<const unsigned> starts)
    {
        return adoptRef(*new BuiltinsSourceProvider(source, starts));
    }

    JS_EXPORT_PRIVATE LineColumn lineColumnInTextForOffset(unsigned offset) final;
    bool wantsLineStarts() const final { return false; }

private:
    BuiltinsSourceProvider(const String& source, std::span<const unsigned> starts)
        : StringSourceProvider(source, { }, SourceTaintedOrigin::Untainted, String(), TextPosition(), SourceProviderSourceType::Program)
        , m_starts(starts)
    {
    }

    std::span<const unsigned> m_starts;
};

    class SyntheticSourceProvider final : public SourceProvider {
    public:
        using SyntheticSourceGenerator = WTF::Function<void(JSGlobalObject*, Identifier, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues)>;
        // Same contract as SyntheticSourceGenerator, except that an export may be declared without a value (an empty
        // JSValue appended to exportValues). Such exports are read from the returned object the first time something
        // binds to them; see SyntheticModuleRecord::tryCreateWithExportNamesAndValues(..., JSObject* lazyExportsSource).
        // The generator returns nullptr when it provided every value.
        using LazySyntheticSourceGenerator = WTF::Function<JSObject*(JSGlobalObject*, Identifier, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues)>;

        static Ref<SyntheticSourceProvider> create(SyntheticSourceGenerator&& generator, const SourceOrigin& sourceOrigin, String sourceURL)
        {
            return adoptRef(*new SyntheticSourceProvider(WTF::move(generator), nullptr, sourceOrigin, WTF::move(sourceURL)));
        }

        static Ref<SyntheticSourceProvider> createWithLazyExports(LazySyntheticSourceGenerator&& generator, const SourceOrigin& sourceOrigin, String sourceURL)
        {
            return adoptRef(*new SyntheticSourceProvider(nullptr, WTF::move(generator), sourceOrigin, WTF::move(sourceURL)));
        }

        // For a generator that evaluates a module written by the user to find out what it exports (a CommonJS module).
        // It does not run when the module record is created, which happens whenever that module's fetch completes, but
        // once the whole graph importing it has loaded, in import order, right before that graph is linked. See
        // AbstractModuleRecord::generateDeferredSyntheticModules().
        static Ref<SyntheticSourceProvider> createDeferred(SyntheticSourceGenerator&& generator, const SourceOrigin& sourceOrigin, String sourceURL)
        {
            Ref provider = create(WTF::move(generator), sourceOrigin, WTF::move(sourceURL));
            provider->m_isDeferred = true;
            return provider;
        }

        bool isDeferred() const { return m_isDeferred; }

        unsigned hash() const final
        {
            return m_source.impl()->hash();
        }

        StringView source() const final
        {
            return m_source;
        }

        // Returns the object that exports declared without a value are read from, or nullptr if there are none.
        JSObject* generate(JSGlobalObject* globalObject, Identifier moduleKey, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues)
        {
            if (m_lazyGenerator)
                return m_lazyGenerator(globalObject, moduleKey, exportNames, exportValues);
            m_generator(globalObject, moduleKey, exportNames, exportValues);
            return nullptr;
        }

    
    private:
        JS_EXPORT_PRIVATE SyntheticSourceProvider(SyntheticSourceGenerator&& generator, LazySyntheticSourceGenerator&& lazyGenerator, const SourceOrigin& sourceOrigin, String&& sourceURL, String&& preRedirectURL = String())
            : SourceProvider(sourceOrigin, WTF::move(sourceURL), WTF::move(preRedirectURL), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::Synthetic)
            , m_source("[native code]"_s)
            , m_generator(WTF::move(generator))
            , m_lazyGenerator(WTF::move(lazyGenerator))
        {
        }

        String m_source;
        SyntheticSourceGenerator m_generator;
        LazySyntheticSourceGenerator m_lazyGenerator;
        bool m_isDeferred { false };
    };

#if ENABLE(WEBASSEMBLY)
class BaseWebAssemblySourceProvider : public SourceProvider {
public:
    virtual const uint8_t* data() = 0;
    virtual size_t size() const = 0;
protected:
    JS_EXPORT_PRIVATE BaseWebAssemblySourceProvider(const SourceOrigin&, String&& sourceURL);
};

class WebAssemblySourceProvider final : public BaseWebAssemblySourceProvider {
public:
    static Ref<WebAssemblySourceProvider> create(Vector<uint8_t>&& data, const SourceOrigin& sourceOrigin, String sourceURL)
    {
        return adoptRef(*new WebAssemblySourceProvider(WTF::move(data), sourceOrigin, WTF::move(sourceURL)));
    }

    unsigned hash() const final
    {
        return m_source.impl()->hash();
    }

    StringView source() const final
    {
        return m_source;
    }

    const uint8_t* data() final
    {
        return m_data.span().data();
    }

    size_t size() const final
    {
        return m_data.size();
    }

    const Vector<uint8_t>& dataVector() const
    {
        return m_data;
    }

private:
    JS_EXPORT_PRIVATE WebAssemblySourceProvider(Vector<uint8_t>&& data, const SourceOrigin& sourceOrigin, String&& sourceURL)
        : BaseWebAssemblySourceProvider(sourceOrigin, WTF::move(sourceURL))
        , m_source("[WebAssembly source]"_s)
        , m_data(WTF::move(data))
    {
    }

    String m_source;
    Vector<uint8_t> m_data;
};

// RAII class for managing a Wasm source provider's underlying buffer.
class WebAssemblySourceProviderBufferGuard {
public:
    explicit WebAssemblySourceProviderBufferGuard(BaseWebAssemblySourceProvider* sourceProvider)
        : m_sourceProvider(sourceProvider)
    {
        if (m_sourceProvider)
            m_sourceProvider->lockUnderlyingBuffer();
    }

    ~WebAssemblySourceProviderBufferGuard()
    {
        if (m_sourceProvider)
            m_sourceProvider->unlockUnderlyingBuffer();
    }

private:
    RefPtr<BaseWebAssemblySourceProvider> m_sourceProvider;
};
#endif

// RAII class for managing a source provider's underlying buffer.
class SourceProviderBufferGuard {
public:
    explicit SourceProviderBufferGuard(SourceProvider* sourceProvider)
        : m_sourceProvider(sourceProvider)
    {
        if (m_sourceProvider)
            m_sourceProvider->lockUnderlyingBuffer();
    }

    ~SourceProviderBufferGuard()
    {
        if (m_sourceProvider)
            m_sourceProvider->unlockUnderlyingBuffer();
    }

    SourceProvider* provider() { return m_sourceProvider; }

private:
    // This must not be RefPtr. It is possible that this is used by the concurrent compiler and
    // we are ensuring that this does not go away with different mechanism. But SourceProvider etc. can have main-thread-only affinity.
    SourceProvider* m_sourceProvider { nullptr };
};

} // namespace JSC
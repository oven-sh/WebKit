/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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

#include "BytecodeIndex.h"
#include "Instruction.h"
#include <wtf/UnalignedAccess.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(InstructionStream);

struct InstructionStreamBufferMalloc final : public InstructionStreamMalloc {
    static constexpr ALWAYS_INLINE size_t nextCapacity(size_t capacity) { return capacity + capacity; }
};

template<typename InstructionType>
class InstructionStream {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(InstructionStream);

    template<typename> friend class InstructionStreamWriter;
    friend class CachedInstructionStream;
public:
    using InstructionBuffer = Vector<uint8_t, 0, UnsafeVectorOverflow, 16, InstructionStreamBufferMalloc>;
#if USE(BUN_JSC_ADDITIONS)
    // Read-only view over instruction bytes. Backed by either m_instructions
    // (owning) or by externally-owned storage that outlives this stream — in
    // practice the mmapped CachedBytecode that the bytecode-cache decoder
    // reads from. view() picks the right one: owned takes precedence so an
    // InstructionStreamWriter mid-generation (whose Vector grows) always
    // reads its current bytes, and a borrowed stream (empty Vector) falls
    // through to m_borrowed.
    using InstructionSpan = std::span<const uint8_t>;

    InstructionSpan view() const
    {
        return m_instructions.isEmpty() ? m_borrowed : InstructionSpan { m_instructions.span() };
    }
#endif

    size_t sizeInBytes() const
    {
#if USE(BUN_JSC_ADDITIONS)
        return view().size();
#else
        return m_instructions.size();
#endif
    }

    using Offset = unsigned;

private:
    template<class InstructionBuffer>
    class BaseRef {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BaseRef);

        template<typename> friend class InstructionStream;

    public:
        BaseRef(const BaseRef<InstructionBuffer>& other)
            : m_instructions(other.m_instructions)
            ,  m_index(other.m_index)
        { }

        void operator=(const BaseRef<InstructionBuffer>& other)
        {
            m_instructions = other.m_instructions;
            m_index = other.m_index;
        }

        inline const InstructionType* operator->() const { return unwrap(); }
        inline const InstructionType* ptr() const { return unwrap(); }

        bool operator==(const BaseRef<InstructionBuffer>& other) const
        {
            return &m_instructions == &other.m_instructions && m_index == other.m_index;
        }

        BaseRef next() const
        {
            return BaseRef { m_instructions, m_index + ptr()->size() };
        }

        inline Offset offset() const { return m_index; }
        inline BytecodeIndex index() const { return BytecodeIndex(offset()); }

        bool isValid() const
        {
            return m_index < m_instructions.size();
        }

    private:
        inline const InstructionType* unwrap() const { return reinterpret_cast<const InstructionType*>(&m_instructions[m_index]); }

    protected:
        BaseRef(InstructionBuffer& instructions, size_t index)
            : m_instructions(instructions)
            , m_index(index)
        { }

        InstructionBuffer& m_instructions;
        Offset m_index;
    };

public:
#if USE(BUN_JSC_ADDITIONS)
    // Ref holds an InstructionSpan by value rather than the original
    // InstructionBuffer& so it can refer either to the owning Vector's
    // current storage or to a borrowed CachedBytecode span. Its public
    // surface (operator->, ptr(), next(), offset(), index(), isValid(),
    // operator==) matches the upstream BaseRef<const InstructionBuffer>.
    class Ref {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Ref);
        template<typename> friend class InstructionStream;

    public:
        Ref(const Ref&) = default;
        Ref& operator=(const Ref&) = default;

        inline const InstructionType* operator->() const { return unwrap(); }
        inline const InstructionType* ptr() const { return unwrap(); }

        bool operator==(const Ref& other) const { return m_span.data() == other.m_span.data() && m_index == other.m_index; }

        Ref next() const { return Ref { m_span, m_index + ptr()->size() }; }

        inline Offset offset() const { return m_index; }
        inline BytecodeIndex index() const { return BytecodeIndex(offset()); }
        bool isValid() const { return m_index < m_span.size(); }

    private:
        inline const InstructionType* unwrap() const { return reinterpret_cast<const InstructionType*>(&m_span[m_index]); }

    protected:
        Ref(InstructionSpan span, size_t index)
            : m_span(span)
            , m_index(index)
        { }

        InstructionSpan m_span;
        Offset m_index;
    };
#else
    using Ref = BaseRef<const InstructionBuffer>;
#endif

    class MutableRef : public BaseRef<InstructionBuffer> {
        template<typename> friend class InstructionStreamWriter;

    protected:
        using BaseRef<InstructionBuffer>::BaseRef;
        using BaseRef<InstructionBuffer>::m_index;
        using BaseRef<InstructionBuffer>::m_instructions;

    public:
#if USE(BUN_JSC_ADDITIONS)
        Ref freeze() const  { return Ref { InstructionSpan { m_instructions.span() }, m_index }; }
#else
        Ref freeze() const  { return Ref { m_instructions, m_index }; }
#endif
        inline InstructionType* operator->() { return unwrap(); }
        inline const InstructionType* operator->() const { return unwrap(); }
        inline InstructionType* ptr() { return unwrap(); }
        inline const InstructionType* ptr() const { return unwrap(); }
        inline operator Ref()
        {
#if USE(BUN_JSC_ADDITIONS)
            return Ref { InstructionSpan { m_instructions.span() }, m_index };
#else
            return Ref { m_instructions, m_index };
#endif
        }

    private:
        inline InstructionType* unwrap() { return reinterpret_cast<InstructionType*>(&m_instructions[m_index]); }
        inline const InstructionType* unwrap() const { return reinterpret_cast<const InstructionType*>(&m_instructions[m_index]); }
    };

private:
    class iterator : public Ref {
        template<typename> friend class InstructionStream;

    public:
        using Ref::Ref;
        using Ref::m_index;

        Ref& operator*()
        {
            return *this;
        }

        iterator& operator+=(size_t size)
        {
            m_index += size;
            return *this;
        }

        iterator& operator++()
        {
            return *this += this->ptr()->size();
        }
    };

public:
#if USE(BUN_JSC_ADDITIONS)
    inline iterator begin() const LIFETIME_BOUND { return iterator { view(), 0 }; }
    inline iterator end() const LIFETIME_BOUND { auto v = view(); return iterator { v, v.size() }; }

    inline const Ref at(BytecodeIndex index) const { return at(index.offset()); }
    inline const Ref at(Offset offset) const
    {
        auto v = view();
        ASSERT(offset < v.size());
        return Ref { v, offset };
    }

    inline size_t size() const { return view().size(); }
    const void* rawPointer() const { return view().data(); }

    bool contains(InstructionType* instruction) const
    {
        auto v = view();
        auto* pointer = std::bit_cast<const uint8_t*>(instruction);
        return !v.empty() && pointer >= v.data() && pointer < v.data() + v.size();
    }

    // Construct a stream that borrows externally-owned instruction bytes
    // (e.g. directly from a mmapped CachedBytecode span). The caller is
    // responsible for the borrowed storage outliving this stream — in the
    // bytecode-cache decode path that lifetime is the CachedBytecode itself,
    // which is held by the Decoder and the SourceProvider for the lifetime
    // of every UnlinkedCodeBlock that came from it.
    static std::unique_ptr<InstructionStream> createBorrowed(InstructionSpan borrowed)
    {
        return std::unique_ptr<InstructionStream> { new InstructionStream(BorrowedTag { }, borrowed) };
    }
#else
    inline iterator begin() const LIFETIME_BOUND
    {
        return iterator { m_instructions, 0 };
    }

    inline iterator end() const LIFETIME_BOUND
    {
        return iterator { m_instructions, m_instructions.size() };
    }

    inline const Ref at(BytecodeIndex index) const { return at(index.offset()); }
    inline const Ref at(Offset offset) const
    {
        ASSERT(offset < m_instructions.size());
        return Ref { m_instructions, offset };
    }

    inline size_t size() const
    {
        return m_instructions.size();
    }

    const void* rawPointer() const
    {
        return m_instructions.span().data();
    }

    bool contains(InstructionType* instruction) const
    {
        auto* pointer = std::bit_cast<const uint8_t*>(instruction);
        return pointer >= m_instructions.begin() && pointer < m_instructions.end();
    }
#endif

protected:
    explicit InstructionStream(InstructionBuffer&& instructions)
        : m_instructions(WTF::move(instructions))
    { }

#if USE(BUN_JSC_ADDITIONS)
    struct BorrowedTag { };
    InstructionStream(BorrowedTag, InstructionSpan borrowed)
        : m_borrowed(borrowed)
    { }
#endif

    InstructionBuffer m_instructions;
#if USE(BUN_JSC_ADDITIONS)
    InstructionSpan m_borrowed;
#endif
};

template<typename InstructionType>
class InstructionStreamWriter : public InstructionStream<InstructionType> {
    friend class BytecodeRewriter;
public:
    using InstructionStream<InstructionType>::InstructionStream;
    using typename InstructionStream<InstructionType>::InstructionBuffer;
    using typename InstructionStream<InstructionType>::MutableRef;
    using typename InstructionStream<InstructionType>::Offset;
    using InstructionStream<InstructionType>::m_instructions;

    InstructionStreamWriter()
        : InstructionStream<InstructionType>({ })
    { }

    void setInstructionBuffer(InstructionBuffer&& buffer)
    {
        RELEASE_ASSERT(!m_instructions.size());
        RELEASE_ASSERT(!buffer.size());
        m_instructions = WTF::move(buffer);
    }

    inline MutableRef ref(Offset offset)
    {
        ASSERT(offset < m_instructions.size());
        return MutableRef { m_instructions, offset };
    }

    void seek(unsigned position)
    {
        ASSERT(position <= m_instructions.size());
        m_position = position;
    }

    unsigned position()
    {
        return m_position;
    }

    template<typename... Args>
        requires (sizeof...(Args) > 0 && (... && std::integral<Args>))
    void write(Args... args)
    {
        constexpr size_t totalSize = (sizeof(Args) + ...);
        auto* p = static_cast<uint8_t*>(reserve<totalSize>());
        ((WTF::unalignedStore(p, args), p += sizeof(args)), ...);
    }

    template<size_t size>
    uint8_t* reserve()
    {
        ASSERT(!m_finalized);
        if ((m_position + size) > m_instructions.size())
            m_instructions.grow(m_position + size);
        auto* result = m_instructions.mutableSpan().data() + m_position;
        m_position += size;
        return result;
    }

    void rewind(MutableRef& ref)
    {
        ASSERT(ref.offset() < m_instructions.size());
        m_instructions.shrink(ref.offset());
        m_position = ref.offset();
    }

    std::unique_ptr<InstructionStream<InstructionType>> finalize()
    {
        m_finalized = true;
        m_instructions.shrinkToFit();
        return std::unique_ptr<InstructionStream<InstructionType>> { new InstructionStream<InstructionType>(WTF::move(m_instructions)) };
    }

    std::unique_ptr<InstructionStream<InstructionType>> finalize(InstructionBuffer& usedBuffer)
    {
        m_finalized = true;

        InstructionBuffer resultBuffer(m_instructions.size());
        RELEASE_ASSERT(m_instructions.sizeInBytes() == resultBuffer.sizeInBytes());
        memcpy(resultBuffer.mutableSpan().data(), m_instructions.span().data(), m_instructions.sizeInBytes());

        usedBuffer = WTF::move(m_instructions);

        return std::unique_ptr<InstructionStream<InstructionType>> { new InstructionStream<InstructionType>(WTF::move(resultBuffer)) };
    }

    MutableRef ref()
    {
        return MutableRef { m_instructions, m_position };
    }

    void swap(InstructionStreamWriter<InstructionType>& other)
    {
        std::swap(m_finalized, other.m_finalized);
        std::swap(m_position, other.m_position);
        m_instructions.swap(other.m_instructions);
    }

private:
    class iterator : public InstructionStream<InstructionType>::MutableRef {
        template<typename> friend class InstructionStreamWriter;

    protected:
        using MutableRef::MutableRef;
        using MutableRef::m_index;

    public:
        MutableRef& operator*()
        {
            return *this;
        }

        iterator& operator+=(size_t size)
        {
            m_index += size;
            return *this;
        }

        iterator& operator++()
        {
            return *this += this->ptr()->size();
        }
    };

public:
    iterator begin()
    {
        return iterator { m_instructions, 0 };
    }

    iterator end()
    {
        return iterator { m_instructions, m_instructions.size() };
    }

private:
    unsigned m_position { 0 };
    bool m_finalized { false };
};

using JSInstructionStream = InstructionStream<JSInstruction>;
using JSInstructionStreamWriter = InstructionStreamWriter<JSInstruction>;

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

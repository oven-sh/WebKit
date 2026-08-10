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
#include <span>

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

    size_t sizeInBytes() const
    {
        return m_bytes.size();
    }
    size_t ownedSizeInBytes() const { return m_isBorrowed ? 0 : m_bytes.size(); } // what this object reports to the GC as its own

    using Offset = unsigned;

private:
    // Refs read through the stream's span (one indirection, exactly like reading through a reference to the buffer), so a
    // ref minted while a writer is still appending keeps seeing the current bytes: the writer refreshes the span after
    // every mutation of its buffer.
    using Bytes = std::span<const uint8_t>;

public:
    class Ref;
private:
    class BaseRef {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BaseRef);

        template<typename> friend class InstructionStream;
        template<typename> friend class InstructionStreamWriter;

    public:
        inline const InstructionType* operator->() const { return unwrap(); }
        inline const InstructionType* ptr() const { return unwrap(); }

        bool operator==(const BaseRef& other) const
        {
            return m_bytes == other.m_bytes && m_index == other.m_index;
        }

        inline Ref next() const;

        inline Offset offset() const { return m_index; }
        inline BytecodeIndex index() const { return BytecodeIndex(offset()); }

        bool isValid() const
        {
            return m_index < m_bytes->size();
        }

    protected:
        BaseRef(const Bytes* bytes, size_t index)
            : m_bytes(bytes)
            , m_index(index)
        { }

        inline const InstructionType* unwrap() const { return reinterpret_cast<const InstructionType*>(m_bytes->data() + m_index); }

        const Bytes* m_bytes;
        Offset m_index;
    };

public:
    class Ref : public BaseRef {
        template<typename> friend class InstructionStream;
        template<typename> friend class InstructionStreamWriter;
        friend class BaseRef;
    protected:
        using BaseRef::BaseRef;
    };

    class MutableRef : public BaseRef {
        template<typename> friend class InstructionStream;
        template<typename> friend class InstructionStreamWriter;

    protected:
        MutableRef(InstructionBuffer& buffer, const Bytes* bytes, size_t index)
            : BaseRef(bytes, index)
            , m_buffer(&buffer)
        { }
        using BaseRef::m_index;

    public:
        Ref freeze() const { return Ref { this->m_bytes, m_index }; }
        inline InstructionType* operator->() { return unwrap(); }
        inline const InstructionType* operator->() const { return BaseRef::unwrap(); }
        inline InstructionType* ptr() { return unwrap(); }
        inline const InstructionType* ptr() const { return BaseRef::unwrap(); }
        inline operator Ref() const { return freeze(); }

    private:
        inline InstructionType* unwrap() { return reinterpret_cast<InstructionType*>(m_buffer->mutableSpan().data() + m_index); }

        InstructionBuffer* m_buffer;
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
    inline iterator begin() const LIFETIME_BOUND
    {
        return iterator { &m_bytes, 0 };
    }

    inline iterator end() const LIFETIME_BOUND
    {
        return iterator { &m_bytes, m_bytes.size() };
    }

    inline const Ref at(BytecodeIndex index) const { return at(index.offset()); }
    inline const Ref at(Offset offset) const
    {
        ASSERT(offset < m_bytes.size());
        return Ref { &m_bytes, offset };
    }

    inline size_t size() const
    {
        return m_bytes.size();
    }

    const void* rawPointer() const
    {
        return m_bytes.data();
    }

    bool contains(InstructionType* instruction) const
    {
        auto* pointer = std::bit_cast<const uint8_t*>(instruction);
        return pointer >= m_bytes.data() && pointer < m_bytes.data() + m_bytes.size();
    }

    // Read an immutable instruction stream that lives elsewhere (inside a bytecode cache mapping) instead of copying it.
    enum BorrowTag { Borrow };
    InstructionStream(Bytes borrowed, BorrowTag)
        : m_bytes(borrowed)
        , m_isBorrowed(true)
    { }
    bool isBorrowed() const { return m_isBorrowed; }

    InstructionStream(InstructionStream&& other)
        : m_instructions(WTF::move(other.m_instructions))
        , m_bytes(other.m_isBorrowed ? other.m_bytes : Bytes(m_instructions.span()))
        , m_isBorrowed(other.m_isBorrowed)
    {
        other.m_bytes = { };
        other.m_isBorrowed = false;
    }
    InstructionStream& operator=(InstructionStream&& other)
    {
        m_instructions = WTF::move(other.m_instructions);
        m_isBorrowed = other.m_isBorrowed;
        m_bytes = m_isBorrowed ? other.m_bytes : Bytes(m_instructions.span());
        other.m_bytes = { };
        other.m_isBorrowed = false;
        return *this;
    }

protected:
    explicit InstructionStream(InstructionBuffer&& instructions)
        : m_instructions(WTF::move(instructions))
        , m_bytes(m_instructions.span())
    { }

    void didMutateBuffer()
    {
        RELEASE_ASSERT(!m_isBorrowed); // the bytes are the cache mapping's; every write path comes through here
        m_bytes = m_instructions.span();
    }

    InstructionBuffer m_instructions;
    Bytes m_bytes;
    bool m_isBorrowed { false };
};

template<typename InstructionType>
inline typename InstructionStream<InstructionType>::Ref InstructionStream<InstructionType>::BaseRef::next() const
{
    return Ref { m_bytes, m_index + ptr()->size() };
}

template<typename InstructionType>
class InstructionStreamWriter : public InstructionStream<InstructionType> {
    friend class BytecodeRewriter;
public:
    using InstructionStream<InstructionType>::InstructionStream;
    using typename InstructionStream<InstructionType>::InstructionBuffer;
    using typename InstructionStream<InstructionType>::MutableRef;
    using typename InstructionStream<InstructionType>::Offset;
    using InstructionStream<InstructionType>::m_instructions;
    using InstructionStream<InstructionType>::m_bytes;
    using InstructionStream<InstructionType>::didMutateBuffer;

    InstructionStreamWriter()
        : InstructionStream<InstructionType>({ })
    { }

    void setInstructionBuffer(InstructionBuffer&& buffer)
    {
        RELEASE_ASSERT(!m_instructions.size());
        RELEASE_ASSERT(!buffer.size());
        m_instructions = WTF::move(buffer);
        didMutateBuffer();
    }

    inline MutableRef ref(Offset offset)
    {
        ASSERT(offset < m_instructions.size());
        return MutableRef { m_instructions, &m_bytes, offset };
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
        if ((m_position + size) > m_instructions.size()) {
            m_instructions.grow(m_position + size);
            didMutateBuffer();
        }
        auto* result = m_instructions.mutableSpan().data() + m_position;
        m_position += size;
        return result;
    }

    void rewind(MutableRef& ref)
    {
        ASSERT(ref.offset() < m_instructions.size());
        m_instructions.shrink(ref.offset());
        didMutateBuffer();
        m_position = ref.offset();
    }

    std::unique_ptr<InstructionStream<InstructionType>> finalize()
    {
        m_finalized = true;
        m_instructions.shrinkToFit();
        auto result = std::unique_ptr<InstructionStream<InstructionType>> { new InstructionStream<InstructionType>(WTF::move(m_instructions)) };
        didMutateBuffer();
        return result;
    }

    std::unique_ptr<InstructionStream<InstructionType>> finalize(InstructionBuffer& usedBuffer)
    {
        m_finalized = true;

        InstructionBuffer resultBuffer(m_instructions.size());
        RELEASE_ASSERT(m_instructions.sizeInBytes() == resultBuffer.sizeInBytes());
        memcpy(resultBuffer.mutableSpan().data(), m_instructions.span().data(), m_instructions.sizeInBytes());

        usedBuffer = WTF::move(m_instructions);
        didMutateBuffer();

        return std::unique_ptr<InstructionStream<InstructionType>> { new InstructionStream<InstructionType>(WTF::move(resultBuffer)) };
    }

    MutableRef ref()
    {
        return MutableRef { m_instructions, &m_bytes, m_position };
    }

    void swap(InstructionStreamWriter<InstructionType>& other)
    {
        std::swap(m_finalized, other.m_finalized);
        std::swap(m_position, other.m_position);
        m_instructions.swap(other.m_instructions);
        didMutateBuffer();
        other.didMutateBuffer();
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
        return iterator { m_instructions, &m_bytes, 0 };
    }

    iterator end()
    {
        return iterator { m_instructions, &m_bytes, m_instructions.size() };
    }

private:
    unsigned m_position { 0 };
    bool m_finalized { false };
};

using JSInstructionStream = InstructionStream<JSInstruction>;
using JSInstructionStreamWriter = InstructionStreamWriter<JSInstruction>;

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

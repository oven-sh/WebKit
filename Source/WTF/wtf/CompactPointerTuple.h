/*
 * Copyright (C) 2018 Yusuke Suzuki <utatane.tea@gmail.com>.
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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

#include <type_traits>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

#if OS(DARWIN)
#include <mach/vm_param.h>
#endif

namespace WTF {

// The goal of this class is folding a pointer and 2 bytes value into 8 bytes in both 32bit and 64bit architectures.
// 32bit architecture just has a pair of byte and pointer, which should be 8 bytes.
// We are assuming 48bit pointers here, which is also assumed in JSValue anyway.
template<typename PointerType, typename Type>
class CompactPointerTuple final {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CompactPointerTuple);
public:
    static_assert(sizeof(Type) <= 2);
    static_assert(std::is_pointer<PointerType>::value);
    static_assert(::allowCompactPointers<PointerType>());
    static_assert(IntegralOrEnum<Type>);
    using UnsignedType = std::make_unsigned_t<std::conditional_t<std::is_same_v<Type, bool>, uint8_t, Type>>;
    static_assert(sizeof(UnsignedType) == sizeof(Type));

    CompactPointerTuple() = default;

    friend bool operator==(const CompactPointerTuple&, const CompactPointerTuple&) = default;

#if CPU(ADDRESS64)
public:
    static constexpr unsigned maxNumberOfBitsInPointer = 48;
    static_assert(OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH) <= maxNumberOfBitsInPointer);

#if CPU(LITTLE_ENDIAN)
    static constexpr ptrdiff_t offsetOfType()
    {
        return maxNumberOfBitsInPointer / 8;
    }
#endif

    static constexpr uint64_t pointerMask = (1ULL << maxNumberOfBitsInPointer) - 1;

    CompactPointerTuple(PointerType pointer, Type type)
        : m_data(encode(pointer, type))
    {
        ASSERT(this->type() == type);
        ASSERT(this->pointer() == pointer);
    }

    template<typename OtherPointerType>
        requires (std::is_pointer_v<PointerType> && std::is_convertible_v<OtherPointerType, PointerType>)
    CompactPointerTuple(CompactPointerTuple<OtherPointerType, Type>&& other)
        : m_data { std::exchange(other.m_data, { }) }
    {
    }

    PointerType pointer() const { return std::bit_cast<PointerType>(m_data & pointerMask); }
    void setPointer(PointerType pointer)
    {
        // The assertions below intentionally validate the locally encoded
        // word, NOT a re-read of m_data: some tuples are racy-by-design
        // profiling words shared between threads (e.g. JSC's
        // ArrayAllocationProfile under shared CodeBlocks, SPEC-jit §5.7
        // racy-profiling tolerance), and a concurrent store between our
        // store and a re-read would fail a re-read assert without any
        // encoding bug. The invariant being checked — the pointer survives
        // the 48-bit encoding — is a property of the encoded value itself.
        uint64_t encoded = encode(pointer, type());
        m_data = encoded;
        ASSERT(std::bit_cast<PointerType>(encoded & pointerMask) == pointer);
    }

    Type type() const { return decodeType(m_data); }
    void setType(Type type)
    {
        uint64_t encoded = encode(pointer(), type);
        m_data = encoded;
        ASSERT(decodeType(encoded) == type);
    }

    uint64_t data() const { return m_data; }

    // Relaxed atomic accessors over the single encoded 64-bit word, for
    // tuples that are racy-by-design profiling state shared between threads
    // (e.g. JSC's ArrayAllocationProfile under shared CodeBlocks,
    // SPEC-ungil/SPEC-jit racy-profiling tolerance). Plain concurrent access
    // to m_data is UB; these make each access one relaxed atomic load/store
    // of the whole word, so readers always see a consistent (pointer, type)
    // pair. No ordering is implied, and the read-modify-write helpers
    // (setPointerRelaxed/setTypeRelaxed) are NOT atomic RMWs — racing
    // updates can be lost. Acceptable for advisory profiling data only.
    uint64_t dataRelaxed() const { return atomicLoad(const_cast<uint64_t*>(&m_data), std::memory_order_relaxed); }
    PointerType pointerRelaxed() const { return std::bit_cast<PointerType>(dataRelaxed() & pointerMask); }
    Type typeRelaxed() const { return decodeType(dataRelaxed()); }
    void setPointerRelaxed(PointerType pointer) { atomicStore(&m_data, encode(pointer, typeRelaxed()), std::memory_order_relaxed); }
    void setTypeRelaxed(Type type) { atomicStore(&m_data, encode(pointerRelaxed(), type), std::memory_order_relaxed); }

    CompactPointerTuple loadTupleRelaxed() const
    {
        CompactPointerTuple result;
        result.m_data = dataRelaxed();
        return result;
    }

    CompactPointerTuple exchangeTupleRelaxed(CompactPointerTuple newValue)
    {
        CompactPointerTuple result;
        result.m_data = atomicExchange(&m_data, newValue.m_data, std::memory_order_relaxed);
        return result;
    }

    void swap(CompactPointerTuple& other)
    {
        std::swap(m_data, other.m_data);
    }

private:
    static constexpr uint64_t encodeType(Type type)
    {
        return static_cast<uint64_t>(static_cast<UnsignedType>(type)) << maxNumberOfBitsInPointer;
    }
    static constexpr Type decodeType(uint64_t value)
    {
        return static_cast<Type>(static_cast<UnsignedType>(value >> maxNumberOfBitsInPointer));
    }

    static uint64_t encode(PointerType pointer, Type type)
    {
        return std::bit_cast<uint64_t>(pointer) | encodeType(type);
    }

    uint64_t m_data { 0 };
#else
public:
    CompactPointerTuple(PointerType pointer, Type type)
        : m_pointer(pointer)
        , m_type(type)
    {
    }

    template<typename OtherPointerType>
        requires (std::is_pointer_v<PointerType> && std::is_convertible_v<OtherPointerType, PointerType>)
    CompactPointerTuple(CompactPointerTuple<OtherPointerType, Type>&& other)
        : m_pointer { std::exchange(other.m_pointer, { }) }
        , m_type { std::exchange(other.m_type, { }) }
    {
    }

    PointerType pointer() const { return m_pointer; }
    void setPointer(PointerType pointer) { m_pointer = pointer; }
    Type type() const { return m_type; }
    void setType(Type type) { m_type = type; }

    // Same API as the 64-bit path, but the pointer and type live in separate
    // words here, so the "tuple" forms are NOT a single atomic word — a
    // racing reader can observe a torn (pointer, type) pair. Acceptable for
    // advisory profiling data only (the only intended user).
    PointerType pointerRelaxed() const { return atomicLoad(const_cast<PointerType*>(&m_pointer), std::memory_order_relaxed); }
    Type typeRelaxed() const { return atomicLoad(const_cast<Type*>(&m_type), std::memory_order_relaxed); }
    void setPointerRelaxed(PointerType pointer) { atomicStore(&m_pointer, pointer, std::memory_order_relaxed); }
    void setTypeRelaxed(Type type) { atomicStore(&m_type, type, std::memory_order_relaxed); }

    CompactPointerTuple loadTupleRelaxed() const { return CompactPointerTuple(pointerRelaxed(), typeRelaxed()); }

    CompactPointerTuple exchangeTupleRelaxed(CompactPointerTuple newValue)
    {
        PointerType oldPointer = atomicExchange(&m_pointer, newValue.m_pointer, std::memory_order_relaxed);
        Type oldType = atomicExchange(&m_type, newValue.m_type, std::memory_order_relaxed);
        return CompactPointerTuple(oldPointer, oldType);
    }

    void swap(CompactPointerTuple& other)
    {
        std::swap(m_pointer, other.m_pointer);
        std::swap(m_type, other.m_type);
    }

private:
    PointerType m_pointer { nullptr };
    Type m_type { 0 };
#endif

    template<typename, typename> friend class CompactPointerTuple;
};

} // namespace WTF

using WTF::CompactPointerTuple;

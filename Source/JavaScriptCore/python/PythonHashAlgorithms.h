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

#include <array>
#include <span>
#include <wtf/StdLibExtras.h>

// The hashes that CPython has of its own, in Modules/_hacl: MD5, SHA-1, SHA-2, SHA-3 and BLAKE2. What comes of each is laid down elsewhere (RFC 1321, FIPS 180-4, FIPS 202, RFC 7693), and these are written from that. They know
// nothing of Python. The modules are in PythonHashModules.cpp.
//
// Each can be given more at any time, and asked at any time what it has come to, which leaves it as it was. To copy one is to copy the object.

namespace JSC { namespace Python {

// What goes through its input a block at a time, and ends it with a 1, zeros, and how long it was.
template<typename Derived, typename Word, size_t stateWords, size_t bytesInBlock>
class BlockHash {
public:
    static constexpr size_t blockSize = bytesInBlock;

    void update(std::span<const uint8_t> data)
    {
        size_t buffered = m_length % blockSize;
        m_length += data.size();
        if (buffered) {
            size_t taken = std::min(blockSize - buffered, data.size());
            memcpySpan(std::span { m_buffer }.subspan(buffered, taken), data.first(taken));
            data = data.subspan(taken);
            if (buffered + taken < blockSize)
                return;
            static_cast<Derived*>(this)->compress(m_buffer.data());
        }
        while (data.size() >= blockSize) {
            static_cast<Derived*>(this)->compress(data.data());
            data = data.subspan(blockSize);
        }
        if (!data.empty())
            memcpySpan(std::span { m_buffer }.first(data.size()), data);
    }

protected:
    // The words of the state once the end has been put on. `lengthBytes` is how much room how long it was is given.
    std::array<Word, stateWords> finished(size_t lengthBytes, bool isBigEndian) const
    {
        Derived copy = *static_cast<const Derived*>(this);
        uint64_t length = m_length;
        size_t buffered = length % blockSize;
        copy.m_buffer[buffered++] = 0x80;
        if (buffered > blockSize - lengthBytes) {
            zeroSpan(std::span { copy.m_buffer }.subspan(buffered));
            copy.compress(copy.m_buffer.data());
            buffered = 0;
        }
        zeroSpan(std::span { copy.m_buffer }.subspan(buffered));
        // In bits, which is three more of them than fit.
        uint64_t low = length << 3;
        uint64_t high = length >> 61;
        for (size_t i = 0; i < 8; ++i) {
            if (isBigEndian) {
                copy.m_buffer[blockSize - 1 - i] = static_cast<uint8_t>(low >> (8 * i));
                if (lengthBytes == 16)
                    copy.m_buffer[blockSize - 9 - i] = static_cast<uint8_t>(high >> (8 * i));
            } else
                copy.m_buffer[blockSize - 8 + i] = static_cast<uint8_t>(low >> (8 * i));
        }
        copy.compress(copy.m_buffer.data());
        return copy.m_state;
    }

    std::array<Word, stateWords> m_state;
    std::array<uint8_t, bytesInBlock> m_buffer;
    uint64_t m_length { 0 };
};

class MD5Hash final : public BlockHash<MD5Hash, uint32_t, 4, 64> {
public:
    static constexpr size_t digestSize = 16;
    MD5Hash();
    void digest(std::span<uint8_t, digestSize>) const;

private:
    friend class BlockHash;
    void compress(const uint8_t*);
};

class SHA1Hash final : public BlockHash<SHA1Hash, uint32_t, 5, 64> {
public:
    static constexpr size_t digestSize = 20;
    SHA1Hash();
    void digest(std::span<uint8_t, digestSize>) const;

private:
    friend class BlockHash;
    void compress(const uint8_t*);
};

// SHA-224 and SHA-256
class SHA256Hash final : public BlockHash<SHA256Hash, uint32_t, 8, 64> {
public:
    static constexpr size_t maxDigestSize = 32;
    explicit SHA256Hash(size_t digestSize);
    size_t digestSize() const { return m_digestSize; }
    void digest(std::span<uint8_t>) const;

private:
    friend class BlockHash;
    void compress(const uint8_t*);
    size_t m_digestSize;
};

// SHA-384 and SHA-512
class SHA512Hash final : public BlockHash<SHA512Hash, uint64_t, 8, 128> {
public:
    static constexpr size_t maxDigestSize = 64;
    explicit SHA512Hash(size_t digestSize);
    size_t digestSize() const { return m_digestSize; }
    void digest(std::span<uint8_t>) const;

private:
    friend class BlockHash;
    void compress(const uint8_t*);
    size_t m_digestSize;
};

// SHA-3, and SHAKE, of which as much comes as is asked for.
class KeccakHash final {
public:
    // `capacityBits` is twice how strong it is. `suffix` is what tells SHA-3 (6) from SHAKE (0x1f), with the first bit of the padding.
    KeccakHash(unsigned capacityBits, uint8_t suffix);
    size_t rate() const { return m_rate; }
    uint8_t suffix() const { return m_suffix; }
    void update(std::span<const uint8_t>);
    void digest(std::span<uint8_t>) const;

private:
    void permute();
    void advance(size_t);

    std::array<uint64_t, 25> m_state { };
    size_t m_rate; // In bytes
    size_t m_position { 0 };
    uint8_t m_suffix;
};

// What a BLAKE2 is begun with. Each is in the block of parameters as RFC 7693 and the BLAKE2 paper lay it out.
struct Blake2Parameters {
    uint8_t digestSize;
    std::span<const uint8_t> key;
    std::span<const uint8_t> salt;
    std::span<const uint8_t> person;
    uint8_t fanout { 1 };
    uint8_t depth { 1 };
    uint32_t leafSize { 0 };
    uint64_t nodeOffset { 0 };
    uint8_t nodeDepth { 0 };
    uint8_t innerSize { 0 };
    bool isLastNode { false };
};

// BLAKE2b, of 64-bit words, and BLAKE2s, of 32-bit ones.
template<typename Word>
class Blake2Hash final {
public:
    static constexpr bool isB = sizeof(Word) == 8;
    static constexpr size_t blockSize = 16 * sizeof(Word);
    static constexpr size_t maxDigestSize = 8 * sizeof(Word);
    static constexpr size_t maxKeySize = maxDigestSize;
    static constexpr size_t saltSize = 2 * sizeof(Word);
    static constexpr size_t personSize = 2 * sizeof(Word);

    explicit Blake2Hash(const Blake2Parameters&);
    size_t digestSize() const { return m_digestSize; }
    void update(std::span<const uint8_t>);
    void digest(std::span<uint8_t>) const;

private:
    void compress(const uint8_t*, bool isLast);

    std::array<Word, 8> m_state;
    std::array<Word, 2> m_count { };
    std::array<uint8_t, blockSize> m_buffer { };
    size_t m_buffered { 0 };
    size_t m_digestSize;
    bool m_isLastNode;
};

using Blake2bHash = Blake2Hash<uint64_t>;
using Blake2sHash = Blake2Hash<uint32_t>;

} } // namespace JSC::Python

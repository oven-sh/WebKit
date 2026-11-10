/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTCodeSites.h"
#include "AOTListOfUnit.h"
#include "AOTRecordTypes.h"
#include "AOTRelocation.h"
#include "AOTStubKey.h"
#include "AOTVector.h"
#include "CCallHelpers.h"
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class BuildContext;
class StubCatalog;
struct CodeBody;

enum class ArgumentSource : uint8_t { Register, Stack32, Stack64, Constant, Stack32OffStackPointer, Stack64OffStackPointer, ThrowWordWithMention, ConstantBelowMention };

using ArgumentMoves = ListOfUnit<uint64_t, 8>;

inline constexpr uint64_t hasImmediate = 1ull << 32;
inline constexpr uint64_t immediateIsMention = 1ull << 33;
inline constexpr uint64_t mentionNamesSiblingOfCallee = 1ull << 34;
inline constexpr size_t numberOfHeaderWords = 2;

inline void appendArgumentMove(ArgumentMoves& moves, GPRReg destination, ArgumentSource source, uint64_t payload)
{
    moves.append(static_cast<uint64_t>(source) | static_cast<uint64_t>(destination) << 8);
    moves.append(payload);
}

inline ArgumentSource sourceOfArgumentMove(std::span<const uint64_t> moves, size_t index)
{
    return static_cast<ArgumentSource>(moves[index] & 0xff);
}

inline void setSourceOfArgumentMove(std::span<uint64_t> moves, size_t index, ArgumentSource source, uint64_t payload)
{
    moves[index] = (moves[index] & ~static_cast<uint64_t>(0xff)) | static_cast<uint64_t>(source);
    moves[index + 1] = payload;
}

inline StubKey calleeOfArgumentMoves(std::span<const uint64_t> moves)
{
    RELEASE_ASSERT(moves.size() >= numberOfHeaderWords);
    return StubKey::fromBits(moves[0]);
}

class ArgumentMovesOfUnit {
public:
    ArgumentMovesOfUnit() = default;
    explicit ArgumentMovesOfUnit(std::span<const ArgumentMoves> lists)
    {
        if (lists.empty())
            return;
        size_t end = numberOfWordsInFrontOfLists(lists.size());
        size_t numberOfWords = end;
        for (const ArgumentMoves& list : lists)
            numberOfWords += list.size();
        m_words.reserveInitialCapacity(numberOfWords);
        m_words.grow(numberOfWords);
        std::span<uint64_t> words = m_words.mutableSpan();
        zeroSpan(words.first(end));
        auto setHalfWord = [&](size_t index, size_t value) {
            words[index / 2] |= static_cast<uint64_t>(value) << (index % 2 * 32);
        };
        setHalfWord(0, lists.size());
        for (size_t index = 0; index < lists.size(); ++index) {
            for (uint64_t word : lists[index])
                words[end++] = word;
            setHalfWord(1 + index, end);
        }
    }

    size_t size() const { return m_words.isEmpty() ? 0 : halfWord(0); }
    bool isEmpty() const { return m_words.isEmpty(); }
    std::span<const uint64_t> operator[](size_t index) const
    {
        size_t begin = beginOfList(index);
        return m_words.span().subspan(begin, endOfList(index) - begin);
    }
    std::span<uint64_t> operator[](size_t index)
    {
        size_t begin = beginOfList(index);
        return m_words.mutableSpan().subspan(begin, endOfList(index) - begin);
    }
    ListOfUnit<uint64_t>& list() { return m_words; }

    friend bool operator==(const ArgumentMovesOfUnit&, const ArgumentMovesOfUnit&) = default;

private:
    static size_t numberOfWordsInFrontOfLists(size_t numberOfLists) { return (1 + numberOfLists + 1) / 2; }

    uint32_t halfWord(size_t index) const { return static_cast<uint32_t>(m_words[index / 2] >> (index % 2 * 32)); }
    size_t endOfList(size_t index) const { return halfWord(1 + index); }
    size_t beginOfList(size_t index) const
    {
        size_t numberOfLists = size();
        RELEASE_ASSERT(index < numberOfLists);
        return index ? endOfList(index - 1) : numberOfWordsInFrontOfLists(numberOfLists);
    }

    ListOfUnit<uint64_t> m_words;
};

class KeptArgumentMoves {
public:
    KeptArgumentMoves() = default;
    KeptArgumentMoves(std::span<const uint64_t> words)
        : m_words(words.data())
        , m_size(words.size())
    {
    }

    std::span<const uint64_t> span() const { return unsafeMakeSpan(m_words, m_size); }

private:
    const uint64_t* m_words;
    size_t m_size;
};

inline constexpr uint64_t maxConstantInArgumentThunk = 255;

struct ArgumentThunks {
    RelocatableCode code;
    Vector<uint32_t> offsets;
    Vector<StubKey> stubKeys;
};

std::expected<ArgumentThunks, LinkFailure> generateArgumentThunks(BuildContext&, const StubCatalog&, const Vector<KeptArgumentMoves>&);

struct NumberedImmediate {
    uint32_t mention;
};

struct RecordedCall {
    uint32_t relocation;
    OptionalIndex site;
    OptionalIndex firstReadSite;
    std::optional<CCallHelpers::Label> returnAddress { };
};

struct RememberedStubKey {
    uint64_t bits;
    bool bindsImmediate;
    bool bindsOperationOffset;
    bool knowsImmediateGPR;
    GPRReg immediateGPR;
};

struct RememberedStubKeys {
    static constexpr unsigned logOfNumberOfKeys = 8;

    std::array<RememberedStubKey, 1u << logOfNumberOfKeys> keys;
};

class StubCalls {
public:
    StubCalls(const StubCatalog&, RelocationRecorder&, RememberedStubKeys* = nullptr);

    uint32_t call(CCallHelpers&, StubKey, OptionalIndex site);
    uint32_t call(CCallHelpers&, StubKey, uint32_t immediate, OptionalIndex site, OptionalIndex firstReadSite = { });
    uint32_t call(CCallHelpers&, StubKey, NumberedImmediate, OptionalIndex site, OptionalIndex firstReadSite = { });
    uint32_t call(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&, OptionalIndex firstReadSite = { });
    uint32_t callWithImmediateOfSite(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, OptionalIndex site, const ArgumentMoves&);
    uint32_t call(CCallHelpers&, StubKey, NumberedImmediate, OptionalIndex site, const ArgumentMoves&, OptionalIndex firstReadSite = { });
    void enterNextStubInFront(Reg filledByCallee, Reg movedIntoResultByCallee)
    {
        RELEASE_ASSERT(filledByCallee);
        m_registerFilledInFrontOfNextCall = filledByCallee;
        m_registerMovedIntoResultInFrontOfNextCall = movedIntoResultByCallee;
    }
    void callForTwoReads(CCallHelpers&, StubKey, OptionalIndex firstRead, OptionalIndex secondRead);
    void tailCall(CCallHelpers&, StubKey);
    void tailCall(CCallHelpers&, StubKey, uint32_t immediate);
    void tailCall(CCallHelpers&, StubKey, NumberedImmediate);
    void callFunction(CCallHelpers&, Typed::CodeUnit callee, uint16_t entry, OptionalIndex site);
    uint32_t callRegister(CCallHelpers&, GPRReg address, OptionalIndex site);
    void jumpToFunction(CCallHelpers&, Typed::CodeUnit callee, uint16_t entry);
    void moveAddressOfFunction(Typed::CodeUnit callee, uint16_t entry, GPRReg destination);
    void moveAddressOfStub(CCallHelpers&, StubKey, std::optional<uint32_t> immediate, GPRReg destination);

    void routeTransfersThroughEnd();
    void describeCalls(std::span<const Relocation>, CodeBody&, LinkBuffer* = nullptr);

private:
    struct ResolvedKey {
        StubKey key;
        std::optional<uint32_t> immediateToMove;
    };

    ResolvedKey resolveKey(StubKey, std::optional<uint32_t> immediate) const;
    StubKey prepare(CCallHelpers&, StubKey, std::optional<uint32_t> immediate);
    CodeSymbol prepare(StubKey, NumberedImmediate);
    uint32_t indexOf(StubKey, std::optional<uint32_t> mentionOfImmediate = std::nullopt);
    uint32_t indexOf(Typed::CodeUnit);
    uint32_t emitCall(CCallHelpers&, StubKey called, CodeSymbol, const RecordedCall&);
    Reg m_registerFilledInFrontOfNextCall;
    Reg m_registerMovedIntoResultInFrontOfNextCall;
    void emitJump(CodeSymbol);

    const StubCatalog& m_catalog;
    RelocationRecorder& m_relocations;
    std::unique_ptr<RememberedStubKeys> m_ownRememberedKeys;
    RememberedStubKeys& m_rememberedKeys;
    Vector<RecordedCall, 32> m_calls;
    Vector<StubKey> m_stubKeys;
    Vector<std::optional<uint32_t>, 16> m_mentionsOfImmediates;
    Vector<uint32_t> m_placesOfStubKeys;
    Vector<Typed::CodeUnit> m_callees;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_indexesOfCallees;
    ListOfUnit<ArgumentMoves, 16> m_argumentMoves;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

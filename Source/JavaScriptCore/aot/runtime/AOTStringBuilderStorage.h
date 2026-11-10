/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesStringKeys.h"
#include "AOTStringBuilderWords.h"
#include "JSString.h"
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/MathExtras.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

struct Instance;

class StringBuilderStorage final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;

    static constexpr uint32_t minCapacity = 4096;
    static constexpr uint32_t maxRetainedCapacity = 262144;
    static constexpr uint32_t minLengthAppendedByReference = 65536;
    static constexpr uint32_t minLengthAdoptedByFinish = 4096;
    static constexpr uint32_t minLengthShared = minCapacity / 2;
    static constexpr uint32_t maxSegments = 1024;
    static constexpr uint32_t maxLengthOfWideStringCopied = 64;
    static constexpr uint32_t maxUnits = 1 << 20;
    static constexpr uint32_t maxRetainedUnits = 4096;
    static constexpr uint32_t maxPooledBuilders = 8;

    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.aotStringBuilderStorageSpace<mode>();
    }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static StringBuilderStorage* acquire(Instance&);
    void release(Instance&);

    enum class AppendResult : uint8_t {
        Failed,
        Appended,
    };

    AppendResult append(JSGlobalObject*, std::span<const EncodedJSValue> primitives);
    AppendResult append(JSGlobalObject*, JSValue primitive);
    AppendResult append(JSGlobalObject*, JSString*);
    AppendResult append(JSGlobalObject*, std::span<const Latin1Character>);
    AppendResult append(JSGlobalObject*, std::span<const char16_t>);
    bool tryAppendShortString(JSString*);

    Latin1Character* reserve(JSGlobalObject*, size_t length);
    Latin1Character* tryReserve(size_t length);
    struct Reservation {
        size_t length;
        size_t maxLengthOfRest { 0 };
    };
    Latin1Character* reserve(JSGlobalObject*, const Reservation&);
    Latin1Character* tryReserve(const Reservation&);
    void commit(const Latin1Character* cursor);
    bool hasRoomAt(const Latin1Character* cursor, size_t size) const
    {
        ASSERT(m_characters <= cursor && cursor <= m_characters + m_limit);
        return size <= static_cast<size_t>(m_characters + m_limit - cursor);
    }
    std::span<Latin1Character> charactersUpToLimit() LIFETIME_BOUND { return { m_characters, m_limit }; }

    uint32_t length() const { return m_position + m_lengthOutsideBuffer; }
    void rollBack(uint32_t length);

    enum class StringUse : uint8_t {
        Unknown,
        LikelyKey,
        LikelyName,
    };

    JSString* toString(JSGlobalObject*, Instance&, StringUse);
    JSString* finish(JSGlobalObject*, Instance&, StringUse = StringUse::Unknown);
    std::optional<StringKeyCache::Lookup> tryFinishWithCachedKey(Instance&, JSString*& key, unsigned& hash);
    JSString* finishWithNewKey(JSGlobalObject*, Instance&, unsigned hash);

    static constexpr ptrdiff_t offsetOfCharacters() { return OBJECT_OFFSETOF(StringBuilderStorage, m_characters); }
    static constexpr ptrdiff_t offsetOfPosition() { return OBJECT_OFFSETOF(StringBuilderStorage, m_position); }
    static constexpr ptrdiff_t offsetOfLimit() { return OBJECT_OFFSETOF(StringBuilderStorage, m_limit); }
    static constexpr ptrdiff_t offsetOfLengthOutsideBuffer() { return OBJECT_OFFSETOF(StringBuilderStorage, m_lengthOutsideBuffer); }
    static constexpr ptrdiff_t offsetOfNextFree() { return OBJECT_OFFSETOF(StringBuilderStorage, m_nextFree); }
    static constexpr ptrdiff_t offsetOfLastString() { return OBJECT_OFFSETOF(StringBuilderStorage, m_lastString); }
    static constexpr ptrdiff_t offsetOfLengthOfLastString() { return OBJECT_OFFSETOF(StringBuilderStorage, m_lengthOfLastString); }

private:
    friend struct StackStringBuilder;

    struct Segment {
        uint32_t offsetInBuffer;
        uint32_t length;
        uint32_t startOfUnits;
        WriteBarrier<JSString> string;
    };

    struct Piece {
        bool is8Bit() const { return !units && (!string || string->is8Bit()); }

        JSString* string { nullptr };
        const char16_t* units { nullptr };
        uint32_t start { 0 };
        uint32_t length { 0 };
    };

    StringBuilderStorage(VM&, Structure*, StringBuilderWords&);
    static StringBuilderStorage* create(Instance&);
    void reset();
    void returnToPool(Instance&);

    uint32_t capacity() const { return m_buffer ? m_buffer->length() : 0; }
    bool wouldOverwriteSharedCharacters() const { return m_position < m_sharedLength && !m_buffer->hasOneRef(); }
    JSString* validLastString() const { return m_lengthOfLastString == length() ? m_lastString.get() : nullptr; }
    bool holdsOnlyShortTextInOwnBuffer() const
    {
        return m_position > 1 && m_position < minLengthAdoptedByFinish && m_limit <= maxRetainedCapacity && !m_sharedLength && !m_lastString && m_segments.isEmpty();
    }
    void updateLimit();

    bool tryEnsureRoom(size_t size) { return tryEnsureRoom(size, size); }
    bool tryEnsureRoom(size_t length, size_t bytesWritten);
    bool tryMakeRoom(size_t length, size_t bytesWritten);
    bool tryReplaceBuffer(size_t newCapacity);
    void dropBuffer();
    void dropLargeBuffer(size_t longestTextInBuffer);
    bool ensureRoom(JSGlobalObject*, size_t size);
    void throwOutOfMemory(JSGlobalObject*);
    bool canGrowBy(size_t added) const;

    AppendResult appendCharactersOf(JSGlobalObject*, JSString*);
    AppendResult addSegment(JSGlobalObject*, JSString*);
    template<typename CopyUnits> AppendResult addUnits(JSGlobalObject*, uint32_t length, const CopyUnits&);
    bool hasRoomForUnits(uint32_t length) const { return length <= maxLengthOfWideStringCopied && m_units.size() + length <= maxUnits; }
    bool collapseSegments(JSGlobalObject*);
    void joinPiecesInto(VM&, JSString* all);

    template<typename Functor> void forEachPiece(const Functor&) const;
    JSString* createString(JSGlobalObject*, Instance&, StringUse, bool mayGiveBufferAway);
    JSString* createStringFromBuffer(JSGlobalObject*, Instance&, StringUse, bool mayGiveBufferAway);
    JSString* copyBufferToString(JSGlobalObject*);
    JSString* createStringFromPieces(JSGlobalObject*);
    JSString* createFlatRun(JSGlobalObject*, std::span<const Piece>);
    RefPtr<StringImpl> tryShareOrGiveAwayBuffer(bool mayGiveBufferAway);

    Latin1Character* m_characters { nullptr };
    uint32_t m_position { 0 };
    uint32_t m_limit { 0 };
    uint32_t m_lengthOutsideBuffer { 0 };
    uint32_t m_sharedLength { 0 };
    uint32_t m_positionWhenSegmentsWereJoined { 0 };
    uint32_t m_positionJoinedIntoFirstSegment { 0 };
    uint32_t m_capacityOfNextBuffer { 0 };
    StringBuilderWords& m_words;
    WriteBarrier<StringBuilderStorage> m_nextFree;
    WriteBarrier<JSString> m_lastString;
    uint32_t m_lengthOfLastString { 0 };
    uint8_t m_poolDepth { 0 };
    static_assert(maxPooledBuilders <= std::numeric_limits<uint8_t>::max());
    RefPtr<StringImpl> m_buffer;
    Vector<Segment> m_segments;
    Vector<char16_t> m_units;
};

static_assert(StringBuilderStorage::offsetOfCharacters() == sizeof(JSCell), "bytes 12 to 15 of the cell are the upper half of a user-space pointer, so the compiled-object bit of the shape word is clear");

struct StackStringBuilder {
    static constexpr size_t capacity = 128;
    static constexpr size_t maxBytesWrittenPastLimit = 16;
    static constexpr uint32_t minLengthAppendedByReference = StringBuilderStorage::minLengthAppendedByReference;

    bool isOnStack() const { return limit == buffer + capacity; }
    bool holdsOneString() const { return limit == buffer; }
    std::span<const Latin1Character> charactersOnStack(const Latin1Character* cursor) const LIFETIME_BOUND { return { buffer, cursor }; }

    StringBuilderStorage* moveToStorage(JSGlobalObject*, Instance&, const Latin1Character* cursor);
    Latin1Character* didChangeStorage(JSGlobalObject*, StringBuilderStorage&);
    void abandonChangeOfStorage(Instance&, StringBuilderStorage&);
    Latin1Character* holdOneString(JSString*);
    JSString* toString(JSGlobalObject*, Instance&, const Latin1Character* cursor);
    JSString* finish(JSGlobalObject*, Instance&, const Latin1Character* cursor, StringBuilderStorage::StringUse);

    Latin1Character* savedCursor;
    Latin1Character* limit;
    Latin1Character* lengthOrigin;
    union {
        StringBuilderStorage* storage;
        JSString* oneString;
    };
    Latin1Character buffer[capacity + maxBytesWrittenPastLimit];
};

static_assert(std::is_trivially_default_constructible_v<StackStringBuilder> && !(sizeof(StackStringBuilder) % sizeof(void*)));
static_assert(StackStringBuilder::capacity + StackStringBuilder::maxBytesWrittenPastLimit >= StringKeyCache::minReadableBytes);

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

ALWAYS_INLINE bool StringBuilderStorage::tryAppendShortString(JSString* string)
{
    if (!m_position)
        return false;
    std::span<const Latin1Character> characters;
    if (const StringImpl* impl = string->tryGetValueImpl()) {
        if (!impl->is8Bit())
            return false;
        characters = impl->span8();
    } else {
        if (!string->isSubstring())
            return false;
        auto* substring = uncheckedDowncast<JSRopeString>(string);
        const StringImpl* base = substring->substringBase()->tryGetValueImpl();
        if (!base->is8Bit())
            return false;
        characters = base->span8().subspan(substring->substringOffset(), substring->length());
    }
    if (characters.size() >= minLengthAppendedByReference || characters.size() > m_limit - m_position)
        return false;
    StringImpl::copyCharacters(std::span { m_characters + m_position, characters.size() }, characters);
    ensureStillAliveHere(string);
    m_position += static_cast<uint32_t>(characters.size());
    return true;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

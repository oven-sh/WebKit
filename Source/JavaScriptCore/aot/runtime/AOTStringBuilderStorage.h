/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesStringKeys.h"
#include "AOTStringBuilderWords.h"
#include "AOTWatchedJSONLevels.h"
#include "JSString.h"
#include "StackManager.h"
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
    static constexpr uint32_t maxSegments = 1024;
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

    // All or nothing: if one part fails, the parts appended before it are rolled back.
    AppendResult append(JSGlobalObject*, std::span<const EncodedJSValue> primitives);
    AppendResult append(JSGlobalObject*, JSValue primitive);
    AppendResult append(JSGlobalObject*, JSString*);
    AppendResult append(JSGlobalObject*, std::span<const Latin1Character>);
    AppendResult append(JSGlobalObject*, std::span<const char16_t>);
    // A short 8-bit string, or a short piece of one, is copied into the room that is there. It neither allocates nor throws.
    // Not into an empty builder, whose first string may be the whole result.
    bool tryAppendShortString(JSString*);

    Latin1Character* reserve(JSGlobalObject*, size_t length);
    Latin1Character* tryReserve(size_t length);
    struct Reservation {
        size_t length;
        // Bytes that may be written behind them. What is committed of them is held against the longest string when the string is made, or more room is asked for.
        size_t maxLengthOfRest { 0 };
    };
    Latin1Character* reserve(JSGlobalObject*, const Reservation&);
    Latin1Character* tryReserve(const Reservation&);
    void commit(const Latin1Character* cursor);
    // Gives back the room that a Reservation left behind what was committed. Throws and returns false if the text is longer than a string can be.
    bool checkLength(JSGlobalObject*);
    // For a cursor that reserve() has handed out and that nothing has been committed behind.
    bool hasRoomAt(const Latin1Character* cursor, size_t size) const
    {
        ASSERT(m_characters <= cursor && cursor <= m_characters + m_limit);
        return size <= static_cast<size_t>(m_characters + m_limit - cursor);
    }
    // The buffer up to its limit, for whoever counts positions in it. Whatever makes room may replace it, and keeps what is committed at its positions.
    std::span<Latin1Character> charactersUpToLimit() LIFETIME_BOUND { return { m_characters, m_limit }; }

    // In characters. A length is also a checkpoint: rollBack() drops everything after it.
    uint32_t length() const { return m_position + m_lengthOutsideBuffer; }
    void rollBack(uint32_t length);
    // For a function that writes an object as JSON and has found its frame below Instance::jsonLevels. False: the holder is being written inside itself, or the stack has no room for more.
    bool enterWatchedLevel(EncodedJSValue holder, void* frame);

    // What compiled code passes as isLikelyKey.
    enum class StringUse : uint8_t {
        Unknown,
        LikelyKey,
        // A key of properties alone. What is made for it is a name at once, and is not made one by the first access.
        LikelyName,
    };

    // The builder stays usable. Later appends and rollbacks do not change the returned string.
    JSString* toString(JSGlobalObject*, Instance&, StringUse);
    // Returns the builder to the pool, even when it throws.
    JSString* finish(JSGlobalObject*, Instance&, StringUse = StringUse::Unknown);
    // No answer: the text is not one that the cache of keys holds. Found: the builder is back in the pool. Otherwise it is as it was.
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
        // Less than the string's length if the end of the segment was rolled back.
        uint32_t length;
        WriteBarrier<JSString> string;
    };

    struct Piece {
        JSString* string { nullptr };
        uint32_t start { 0 };
        uint32_t length { 0 };
    };

    StringBuilderStorage(VM&, Structure*, StackManager::Mirror& levelLimit, StringBuilderWords&);
    static StringBuilderStorage* create(Instance&);
    void reset();
    void returnToPool(Instance&);

    uint32_t capacity() const { return m_buffer ? m_buffer->length() : 0; }
    bool wouldOverwriteSharedCharacters() const { return m_position < m_sharedLength && !m_buffer->hasOneRef(); }
    JSString* validLastString() const { return m_lengthOfLastString == length() ? m_lastString.get() : nullptr; }
    // Emptying such a builder takes no more than moving the position back: only toString() shares the buffer, and without text outside an unshared buffer the limit is the capacity.
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
    void watchLevelsAfterGrowth(size_t numberOfCharacters);
    void setLevelLimit(void*);
    void stopWatchingLevels();
    bool canGrowBy(size_t added) const;

    AppendResult appendCharactersOf(JSGlobalObject*, JSString*);
    AppendResult addSegment(JSGlobalObject*, JSString*);
    bool collapseSegments(JSGlobalObject*);

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
    // The number of characters at the front of the buffer that strings returned by toString() share.
    uint32_t m_sharedLength { 0 };
    uint32_t m_positionWhenSegmentsWereJoined { 0 };
    uint32_t m_capacityOfNextBuffer { 0 };
    uint32_t m_lengthWhenWatchBegan { 0 };
    WatchedJSONLevels m_watchedLevels;
    StackManager::Mirror& m_levelLimit;
    StringBuilderWords& m_words;
    WriteBarrier<StringBuilderStorage> m_nextFree;
    WriteBarrier<JSString> m_lastString;
    uint32_t m_lengthOfLastString { 0 };
    uint8_t m_poolDepth { 0 };
    static_assert(maxPooledBuilders <= std::numeric_limits<uint8_t>::max());
    RefPtr<StringImpl> m_buffer;
    Vector<Segment> m_segments;
};

static_assert(StringBuilderStorage::offsetOfCharacters() == sizeof(JSCell), "bytes 12 to 15 of the cell are the upper half of a user-space pointer, so the compiled-object bit of the shape word is clear");

// A builder in the frame of compiled code, which hands the cursor from one append to the next in a register and compares it with the limit after each write.
// A text that outgrows the buffer moves to a StringBuilderStorage, and the cursor goes on in the buffer of that. The stack is scanned conservatively, so the cell that the record names stays alive.
// Behind an append that has thrown nobody uses the record: its buffer may be another by then. Compiled code appends to no such builder under a handler that is handed the text.
struct StackStringBuilder {
    static constexpr size_t capacity = 128;
    static constexpr size_t maxBytesWrittenPastLimit = 16;
    static constexpr uint32_t minLengthAppendedByReference = StringBuilderStorage::minLengthAppendedByReference;

    bool isOnStack() const { return limit == buffer + capacity; }
    // One string and nothing behind it. It is the text unless something follows, and there is no room, so whatever follows comes by a call.
    bool holdsOneString() const { return limit == buffer; }
    std::span<const Latin1Character> charactersOnStack(const Latin1Character* cursor) const LIFETIME_BOUND { return { buffer, cursor }; }

    // These return null after throwing.
    // The storage with the text in it: the record's own, or one from the pool that the record does not name until didChangeStorage() has taken it.
    StringBuilderStorage* moveToStorage(JSGlobalObject*, Instance&, const Latin1Character* cursor);
    // The cursor to go on with.
    Latin1Character* didChangeStorage(JSGlobalObject*, StringBuilderStorage&);
    // For whoever has failed between the two: a storage that the record does not name goes back to the pool.
    void abandonChangeOfStorage(Instance&, StringBuilderStorage&);
    Latin1Character* holdOneString(JSString*);
    // The cursor stays good: the same call may be made again with it, and so may an append.
    JSString* toString(JSGlobalObject*, Instance&, const Latin1Character* cursor);
    JSString* finish(JSGlobalObject*, Instance&, const Latin1Character* cursor, StringBuilderStorage::StringUse);

    // The last of a row of appends stores it, for whoever comes by a way on which no register holds it.
    Latin1Character* savedCursor;
    Latin1Character* limit;
    // The cursor less this is the length of the text.
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

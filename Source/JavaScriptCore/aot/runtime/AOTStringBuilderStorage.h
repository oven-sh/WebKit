/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTStringBuilderWords.h"
#include "AOTVector.h"
#include "JSString.h"
#include "StackManager.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

struct Instance;

// An object or an array that compiled code is writing as JSON. The record is in the frame of that code, and holds what that code puts back when it has written the holder.
struct JSONHolderRecord {
    JSONHolderRecord* outer;
    EncodedJSValue holder;
};

// Finds an object that is written as JSON inside itself, for a writer that keeps no list of what it is inside of.
// The text of such an object has no end, so the buffer of its writer grows again and again. Each time the writer shows as many of the objects that follow as cost little beside the copy.
class WatchedJSONLevels {
public:
    bool isWatching() const { return m_numberOfLevelsLeft; }
    bool isWatchingAfterGrowthTo(size_t numberOfCharacters);
    void stop() { m_numberOfLevelsLeft = 0; }
    void forgetLevels();
    // False: the holder is inside itself. Nothing says when a holder has been written: a frame at the same address or above it does.
    bool enter(EncodedJSValue holder, void* frame);

private:
    struct Level {
        void* frame;
        EncodedJSValue holder;
    };

    // What has been shown since the watch began and is not written yet, the outermost first.
    struct Levels {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Levels);
        Vector<Level, 16> path;
        UncheckedKeyHashSet<EncodedJSValue> holders;
    };

    uint32_t m_numberOfLevelsLeft { 0 };
    std::unique_ptr<Levels> m_levels;
};

class StringBuilderStorage final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;

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
    // A short 8-bit piece of another string is copied out of that string into the room that is there. Not into an empty builder, whose first string may be the whole result.
    ALWAYS_INLINE bool tryAppendShortSubstring(JSString* string)
    {
        if (!m_position || !string->isSubstring())
            return false;
        auto* rope = uncheckedDowncast<JSRopeString>(string);
        uint32_t length = rope->length();
        const StringImpl* base = rope->substringBase()->tryGetValueImpl();
        if (length >= m_limits.minLengthAppendedByReference || length > m_limit - m_position || !base->is8Bit())
            return false;
        StringImpl::copyCharacters(std::span<Latin1Character> { m_characters + m_position, length }, base->span8().subspan(rope->substringOffset(), length));
        ensureStillAliveHere(string);
        m_position += length;
        return true;
    }

    Latin1Character* reserve(JSGlobalObject*, size_t size);
    void commit(const Latin1Character* cursor);
    bool hasRoomAt(const Latin1Character* cursor, size_t size) const
    {
        ASSERT(m_characters <= cursor && cursor <= m_characters + m_limit);
        return size <= static_cast<size_t>(m_characters + m_limit - cursor);
    }

    uint32_t length() const { return m_position + m_lengthOutsideBuffer; }
    void rollBack(uint32_t length);
    const JSONHolderRecord* jsonHolders() const { return m_jsonHolders; }
    static bool hasRepeatedHolder(const JSONHolderRecord* innermost);
    // False: the holder is being written inside itself, or the stack has no room for more.
    bool enterWatchedLevel(EncodedJSValue holder, void* frame);
    uint32_t minLengthAppendedByReference() const { return m_limits.minLengthAppendedByReference; }

    enum class StringUse : uint8_t {
        Unknown,
        LikelyKey,
    };

    JSString* toString(JSGlobalObject*, Instance&, StringUse);
    JSString* finish(JSGlobalObject*, Instance&, StringUse);
    std::optional<StringKeyCache::Lookup> tryFinishWithCachedKey(Instance&, JSString*& key, unsigned& hash);
    JSString* finishWithNewKey(Instance&, unsigned hash);
    static JSString* concatenate(JSGlobalObject*, Instance&, std::span<const EncodedJSValue> primitives, StringUse);
    static JSString* tryFindKeyByParts(Instance&, std::span<const EncodedJSValue> primitives);

    static constexpr ptrdiff_t offsetOfCharacters() { return OBJECT_OFFSETOF(StringBuilderStorage, m_characters); }
    static constexpr ptrdiff_t offsetOfPosition() { return OBJECT_OFFSETOF(StringBuilderStorage, m_position); }
    static constexpr ptrdiff_t offsetOfLimit() { return OBJECT_OFFSETOF(StringBuilderStorage, m_limit); }
    static constexpr ptrdiff_t offsetOfLengthOutsideBuffer() { return OBJECT_OFFSETOF(StringBuilderStorage, m_lengthOutsideBuffer); }
    static constexpr ptrdiff_t offsetOfJSONHolders() { return OBJECT_OFFSETOF(StringBuilderStorage, m_jsonHolders); }
    static constexpr ptrdiff_t offsetOfNextFree() { return OBJECT_OFFSETOF(StringBuilderStorage, m_nextFree); }
    static constexpr ptrdiff_t offsetOfLastString() { return OBJECT_OFFSETOF(StringBuilderStorage, m_lastString); }
    static constexpr ptrdiff_t offsetOfLengthOfLastString() { return OBJECT_OFFSETOF(StringBuilderStorage, m_lengthOfLastString); }
    static constexpr ptrdiff_t offsetOfMinLengthAppendedByReference() { return OBJECT_OFFSETOF(StringBuilderStorage, m_limits) + OBJECT_OFFSETOF(Limits, minLengthAppendedByReference); }
    static constexpr ptrdiff_t offsetOfMinLengthAdoptedByFinish() { return OBJECT_OFFSETOF(StringBuilderStorage, m_limits) + OBJECT_OFFSETOF(Limits, minLengthAdoptedByFinish); }

private:
    friend struct StackStringBuilder;

    struct Segment {
        uint32_t offsetInBuffer;
        uint32_t length;
        WriteBarrier<JSString> string;
    };

    struct Piece {
        JSString* string { nullptr };
        uint32_t start { 0 };
        uint32_t length { 0 };
    };

    struct Limits {
        explicit Limits(VM&);

        uint32_t minCapacity;
        uint32_t maxRetainedCapacity;
        uint32_t minLengthAppendedByReference;
        uint32_t minLengthAdoptedByFinish;
        uint32_t maxSegments;
        uint32_t maxPooledBuilders;
    };

    StringBuilderStorage(VM&, Structure*, StackManager::Mirror& levelLimit);
    static StringBuilderStorage* create(Instance&);
    void reset();
    void returnToPool(Instance&);

    uint32_t capacity() const { return m_buffer ? m_buffer->length() : 0; }
    bool wouldOverwriteSharedCharacters() const { return m_position < m_sharedLength && !m_buffer->hasOneRef(); }
    JSString* validLastString() const { return m_lengthOfLastString == length() ? m_lastString.get() : nullptr; }
    // Emptying such a builder takes no more than moving the position back: only toString() shares the buffer, and without text outside an unshared buffer the limit is the capacity.
    bool holdsOnlyShortTextInOwnBuffer() const
    {
        return m_position > 1 && m_position < m_limits.minLengthAdoptedByFinish && m_limit <= m_limits.maxRetainedCapacity && !m_sharedLength && !m_lastString && m_segments.isEmpty();
    }
    void updateLimit();

    bool tryEnsureRoom(size_t size) { return tryEnsureRoom(size, size); }
    bool tryEnsureRoom(size_t length, size_t bytesWritten);
    bool tryReplaceBuffer(size_t newCapacity);
    void dropBuffer();
    bool ensureRoom(JSGlobalObject*, size_t size);
    void throwOutOfMemory(JSGlobalObject*);
    static bool holderInTheMiddleRepeats(const JSONHolderRecord* innermost);
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
    uint32_t m_sharedLength { 0 };
    // The innermost first. A frame that an exception ends takes nothing off the list: the handler that catches the exception puts back the list of its own frame.
    JSONHolderRecord* m_jsonHolders { nullptr };
    WatchedJSONLevels m_watchedLevels;
    uint32_t m_lengthWhenWatchBegan { 0 };
    StackManager::Mirror& m_levelLimit;
    WriteBarrier<StringBuilderStorage> m_nextFree;
    WriteBarrier<JSString> m_lastString;
    uint32_t m_lengthOfLastString { 0 };
    uint8_t m_poolDepth { 0 };
    const Limits m_limits;
    RefPtr<StringImpl> m_buffer;
    Vector<Segment> m_segments;
};

static_assert(StringBuilderStorage::offsetOfCharacters() == sizeof(JSCell), "bytes 12 to 15 of the cell are the upper half of a user-space pointer, so the compiled-object bit of the shape word is clear");

// Compiled code keeps the cursor in a register and compares it with the limit after each write.
struct StackStringBuilder {
    static constexpr size_t capacity = 128;
    static constexpr size_t maxBytesWrittenPastLimit = 16;
    static constexpr uint32_t minLengthAppendedByReference = defaultLimitValue(LimitID::MinLengthAppendedByReference, hostArchitecture());

    bool isOnStack() const { return limit == buffer + capacity; }
    std::span<const Latin1Character> charactersOnStack(const Latin1Character* cursor) const LIFETIME_BOUND { return { buffer, cursor }; }

    // These return null after throwing.
    StringBuilderStorage* moveToStorage(JSGlobalObject*, Instance&, const Latin1Character* cursor);
    Latin1Character* didChangeStorage(JSGlobalObject*);
    JSString* finish(JSGlobalObject*, Instance&, const Latin1Character* cursor, StringBuilderStorage::StringUse);

    Latin1Character* limit;
    Latin1Character* lengthOrigin;
    StringBuilderStorage* storage;
    Latin1Character buffer[capacity + maxBytesWrittenPastLimit];
};

static_assert(std::is_trivially_default_constructible_v<StackStringBuilder> && !(sizeof(StackStringBuilder) % sizeof(void*)));

struct ShortKeyCharacters {
    std::array<Latin1Character, defaultLimitValue(LimitID::MaxCachedKeyLength, hostArchitecture())> characters;
    size_t length { 0 };
    bool hasRopePart { false };

    std::span<const Latin1Character> span() const LIFETIME_BOUND { return std::span { characters }.first(length); }
};

// Writes eight bytes unless the value has one digit.
ALWAYS_INLINE size_t writeUpToEightDigits(uint32_t value, Latin1Character* to)
{
    ASSERT(value < 100'000'000);
    if (value < 10) {
        *to = '0' + value;
        return 1;
    }
    uint64_t upperHalf = value * 0xd1b71759ull >> 45;
    uint64_t halves = upperHalf | (value - upperHalf * 10000) << 32;
    uint64_t upperPairs = halves * 10486 >> 20 & (0x7full << 32 | 0x7f);
    uint64_t pairs = ((halves - upperPairs * 100) << 16) + upperPairs;
    uint64_t tens = pairs * 103 >> 10 & 0x000f000f000f000full;
    uint64_t digits = tens + ((pairs - tens * 10) << 8);
    unsigned leadingZeroBits = ctz(digits) & 0x38;
    WTF::unalignedStore<uint64_t>(to, (digits + 0x3030303030303030ull) >> leadingZeroBits);
    return sizeof(uint64_t) - leadingZeroBits / 8;
}

bool tryWriteShortKey(std::span<const EncodedJSValue> primitives, ShortKeyCharacters&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

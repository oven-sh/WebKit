/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringBuilderWords.h"
#include "JSString.h"
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

struct Instance;

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

    Latin1Character* reserve(JSGlobalObject*, size_t size);
    Latin1Character* tryReserve(size_t size);
    void commit(const Latin1Character* cursor);
    bool hasRoomAt(const Latin1Character* cursor, size_t size) const
    {
        ASSERT(m_characters <= cursor && cursor <= m_characters + m_limit);
        return size <= static_cast<size_t>(m_characters + m_limit - cursor);
    }

    uint32_t length() const { return m_position + m_lengthOutsideBuffer; }
    void rollBack(uint32_t length);
    uint32_t minLengthAppendedByReference() const { return m_limits.minLengthAppendedByReference; }

    enum class StringUse : uint8_t {
        Unknown,
        LikelyKey,
    };

    JSString* toString(JSGlobalObject*, Instance&, StringUse);
    JSString* finish(JSGlobalObject*, Instance&, StringUse);
    static JSString* concatenate(JSGlobalObject*, Instance&, std::span<const EncodedJSValue> primitives, StringUse);

    static constexpr ptrdiff_t offsetOfCharacters() { return OBJECT_OFFSETOF(StringBuilderStorage, m_characters); }
    static constexpr ptrdiff_t offsetOfPosition() { return OBJECT_OFFSETOF(StringBuilderStorage, m_position); }
    static constexpr ptrdiff_t offsetOfLimit() { return OBJECT_OFFSETOF(StringBuilderStorage, m_limit); }
    static constexpr ptrdiff_t offsetOfLengthOutsideBuffer() { return OBJECT_OFFSETOF(StringBuilderStorage, m_lengthOutsideBuffer); }
    static constexpr ptrdiff_t offsetOfNextFree() { return OBJECT_OFFSETOF(StringBuilderStorage, m_nextFree); }

private:
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

    StringBuilderStorage(VM&, Structure*);
    static StringBuilderStorage* create(VM&);
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

    bool tryEnsureRoom(size_t size);
    bool tryReplaceBuffer(size_t newCapacity);
    void dropBuffer();
    bool ensureRoom(JSGlobalObject*, size_t size);
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
    WriteBarrier<StringBuilderStorage> m_nextFree;
    WriteBarrier<JSString> m_lastString;
    uint32_t m_lengthOfLastString { 0 };
    uint8_t m_poolDepth { 0 };
    const Limits m_limits;
    RefPtr<StringImpl> m_buffer;
    Vector<Segment> m_segments;
};

static_assert(StringBuilderStorage::offsetOfCharacters() == sizeof(JSCell), "bytes 12 to 15 of the cell are the upper half of a user-space pointer, so the compiled-object bit of the shape word is clear");

extern "C" JS_EXPORT_PRIVATE uint8_t* JSC__AOT__StringBuilderStorage__tryReserve(StringBuilderStorage*, size_t maxLength);
extern "C" JS_EXPORT_PRIVATE void JSC__AOT__StringBuilderStorage__commit(StringBuilderStorage*, size_t written);
extern "C" JS_EXPORT_PRIVATE bool JSC__AOT__StringBuilderStorage__appendUTF16(StringBuilderStorage*, JSGlobalObject*, const char16_t*, size_t length);

using HostAppendForm = bool (*)(JSGlobalObject*, StringBuilderStorage*, EncodedJSValue thisValue, const EncodedJSValue* arguments, size_t count);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTSectionWriter.h"
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class TableWriter {
    WTF_MAKE_NONCOPYABLE(TableWriter);
public:
    explicit TableWriter(BuildContext& build)
        : m_build(build)
    {
    }

    uint32_t allocate(size_t size, size_t alignment = 8)
    {
        if (std::optional<uint32_t> offset = tryAllocate(size, alignment))
            return *offset;
        RELEASE_ASSERT(size <= defaultLimitValue(LimitID::VectorBytes, hostArchitecture()));
        if (size > m_bytes.size())
            growSectionTo(m_bytes, size);
        return 0;
    }

    template<typename T>
    T& at(uint32_t offset)
    {
        ASSERT(!(offset % alignof(T)));
        return *reinterpret_cast<T*>(m_bytes.mutableSpan().subspan(offset, sizeof(T)).data());
    }

    uint32_t append(std::span<const uint8_t> bytes, size_t alignment = 8)
    {
        auto& candidates = m_appended.add(hashOf(bytes), Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
        for (auto [offset, size] : candidates) {
            if (size == bytes.size() && !(offset % alignment) && equalSpans(m_bytes.span().subspan(offset, size), bytes))
                return offset;
        }
        std::optional<uint32_t> offset = tryAllocate(bytes.size(), alignment);
        if (!offset)
            return 0;
        memcpySpan(m_bytes.mutableSpan().subspan(*offset), bytes);
        candidates.append({ *offset, static_cast<uint32_t>(bytes.size()) });
        return *offset;
    }

    template<typename T>
    uint32_t append(std::span<const T> items)
    {
        static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
        return append(asBytes(items), sizeof(T) == 1 ? 8 : alignof(T));
    }

    template<typename T, size_t inlineCapacity>
    uint32_t append(const Vector<T, inlineCapacity>& items) { return append(items.span()); }

    void appendByte(uint8_t byte) { appendUnshared(singleElementSpan(byte)); }

    uint32_t appendUnshared(std::span<const uint8_t> bytes, size_t alignment = 1)
    {
        std::optional<uint32_t> offset = tryAllocate(bytes.size(), alignment);
        if (!offset)
            return 0;
        memcpySpan(m_bytes.mutableSpan().subspan(*offset), bytes);
        return *offset;
    }

    uint8_t lastByte() const { return m_bytes.last(); }
    size_t size() const { return m_bytes.size(); }
    std::span<const uint8_t> bytes() const LIFETIME_BOUND { return m_bytes.span(); }

    Vector<uint8_t> take()
    {
        m_appended.clear();
        if (m_didOverflow)
            return { };
        return std::exchange(m_bytes, { });
    }

private:
    std::optional<uint32_t> tryAllocate(size_t size, size_t alignment)
    {
        size_t end = m_bytes.size();
        size_t offset = WTF::roundUpToMultipleOf(alignment, end);
        if (!m_didOverflow)
            m_didOverflow = !checkSectionSize(offset + size, m_build);
        if (m_didOverflow)
            return std::nullopt;
        growSectionTo(m_bytes, offset + size);
        zeroSpan(m_bytes.mutableSpan().subspan(end));
        return static_cast<uint32_t>(offset);
    }

    static uint64_t hashOf(std::span<const uint8_t> bytes)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t byte : bytes)
            hash = (hash ^ byte) * 1099511628211ull;
        return hash >> 1 | 1;
    }

    BuildContext& m_build;
    Vector<uint8_t> m_bytes;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_appended;
    bool m_didOverflow { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

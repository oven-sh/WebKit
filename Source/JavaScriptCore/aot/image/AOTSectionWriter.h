/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <type_traits>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

template<typename T>
Vector<uint8_t> bytesOf(const Vector<T>& values)
{
    static_assert(std::has_unique_object_representations_v<T>, "padding would make the bytes of the image differ from build to build");
    return Vector<uint8_t>(asBytes(values.span()));
}

[[nodiscard]] bool checkSectionSize(uint64_t size, BuildContext&);
void growSectionTo(Vector<uint8_t>& bytes, size_t newSize);

class SectionWriter {
    WTF_MAKE_NONCOPYABLE(SectionWriter);
public:
    explicit SectionWriter(BuildContext&);

    uint32_t append(std::span<const uint8_t>, size_t alignment);
    Vector<uint32_t> appendAll(std::span<const Vector<uint8_t>> streams);
    size_t size() const { return m_bytes.size(); }
    bool didOverflow() const { return m_didOverflow; }
    Vector<uint8_t> takeBytes();

private:
    bool canGrowTo(size_t newSize);

    BuildContext& m_context;
    Vector<uint8_t> m_bytes;
    bool m_didOverflow { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

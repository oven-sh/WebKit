/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPrefixCode.h"
#include "AOTVector.h"
#include <cstdint>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class ParallelLoops;

class CodedTexts {
public:
    static constexpr unsigned textsInGroup = 8;

    CodedTexts() = default;
    JS_EXPORT_PRIVATE CodedTexts(std::span<const uint8_t> bytes, uint32_t count);

    uint32_t count() const { return m_count; }
    JS_EXPORT_PRIVATE Vector<uint8_t, 64> at(uint32_t number) const;
    JS_EXPORT_PRIVATE std::span<const uint8_t> bytesOfGroup(uint32_t group) const;

    static bool canHold(std::span<const uint8_t> text);
    JS_EXPORT_PRIVATE static Vector<uint8_t> encode(std::span<const std::span<const uint8_t>> ascendingTexts, const ParallelLoops&);

    class Cursor {
    public:
        explicit Cursor(const CodedTexts& texts)
            : m_texts(texts)
        {
        }

        JS_EXPORT_PRIVATE std::span<const uint8_t> at(uint32_t number);

    private:
        friend class CodedTexts;

        const CodedTexts& m_texts;
        std::optional<PrefixCodeReader> m_reader;
        uint32_t m_next { 0 };
        Vector<uint8_t, 64> m_text;
    };

private:
    std::span<const uint8_t> m_bytes;
    uint32_t m_count { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

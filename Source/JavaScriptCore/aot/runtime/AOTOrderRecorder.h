/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <span>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE bool recordsOrder();
JS_EXPORT_PRIVATE ASCIILiteral startOrderRecording(const char* path);
JS_EXPORT_PRIVATE void finishOrderRecording();
JS_EXPORT_PRIVATE bool isRecordingOrder();

class HashOfNames {
public:
    void add(std::span<const uint8_t> name)
    {
        for (uint8_t byte : name)
            m_hash = (m_hash ^ byte) * 1099511628211ull;
        m_hash = (m_hash ^ '\n') * 1099511628211ull;
        m_isEmpty = false;
    }

    bool isEmpty() const { return m_isEmpty; }
    uint32_t value() const { return static_cast<uint32_t>(m_hash % 1000000000); }

private:
    uint64_t m_hash { 14695981039346656037ull };
    bool m_isEmpty { true };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

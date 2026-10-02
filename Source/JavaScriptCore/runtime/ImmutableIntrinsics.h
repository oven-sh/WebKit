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

#include "JSCJSValue.h"
#include "JSType.h"
#include <wtf/HashMap.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class VM;

class ImmutableIntrinsics {
    WTF_MAKE_TZONE_ALLOCATED(ImmutableIntrinsics);
    WTF_MAKE_NONCOPYABLE(ImmutableIntrinsics);
public:
    static constexpr unsigned maximumCount = 1024;
    static constexpr unsigned globalObject = 0;

    struct Entry {
        String name;
        EncodedJSValue primitive { 0 };
        uint16_t holder { 0 };
        uint16_t canonical { 0 };
        JSType type { CellType };
        bool isCell { false };
        uint16_t builtinCode { 0 };
    };

    JS_EXPORT_PRIVATE static const ImmutableIntrinsics* NODELETE shared();

    unsigned count() const { return m_entries.size(); }
    const Entry& at(unsigned number) const { return m_entries[number]; }
    JS_EXPORT_PRIVATE unsigned find(unsigned holder, const StringImpl& name) const;
    uint32_t hash() const { return m_hash; }

    JS_EXPORT_PRIVATE static void ensureShared(VM&);

    static Vector<EncodedJSValue> describe(JSGlobalObject*, std::span<const ASCIILiteral> variableNames);

private:
    ImmutableIntrinsics() = default;

    Vector<Entry> m_entries;
    UncheckedKeyHashMap<uint64_t, uint16_t> m_numbers;
    uint32_t m_hash { 0 };
};

} // namespace JSC

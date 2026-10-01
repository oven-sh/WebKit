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

// A numbering of what JSGlobalObject::makeIntrinsicsImmutable() has made immutable: the variables of the global object, the plain
// properties of the objects that they hold, and so on recursively. The numbers are the same in every realm of every process that
// runs the same engine, so code that was compiled elsewhere can use them. This class describes each intrinsic without referring to
// any heap object, so that compiler threads can use it. The values themselves belong to the realm
// (JSGlobalObject::immutableIntrinsics()).
class ImmutableIntrinsics {
    WTF_MAKE_TZONE_ALLOCATED(ImmutableIntrinsics);
    WTF_MAKE_NONCOPYABLE(ImmutableIntrinsics);
public:
    static constexpr unsigned maximumCount = 1024;
    static constexpr unsigned globalObject = 0; // The first is the global object itself.

    struct Entry {
        String name;
        EncodedJSValue primitive { 0 }; // The value, if it is not a cell.
        uint16_t holder { 0 }; // The number of the object that has this property.
        uint16_t canonical { 0 }; // The canonical number of the object, which may be reachable through more than one property.
        JSType type { CellType }; // If it is a cell.
        bool isCell { false };
        // For a builtin that is written in JavaScript: its BuiltinCodeIndex, plus one. Only set if the realm was created in the
        // ordinary way, as it is in a process that compiles.
        uint16_t builtinCode { 0 };
    };

    // Null until a realm has been made with VM::useImmutableIntrinsics.
    JS_EXPORT_PRIVATE static const ImmutableIntrinsics* NODELETE shared();

    unsigned count() const { return m_entries.size(); }
    const Entry& at(unsigned number) const { return m_entries[number]; }
    // The property, or the variable, of that name of the object that `holder` is. Zero: none that is fixed.
    JS_EXPORT_PRIVATE unsigned find(unsigned holder, const StringImpl& name) const;
    uint32_t hash() const { return m_hash; }

    // For a compiler in a process that has had no use for a realm.
    JS_EXPORT_PRIVATE static void ensureShared(VM&);

    // For the realm, when it has fixed them: the values, by number.
    static Vector<EncodedJSValue> describe(JSGlobalObject*, std::span<const ASCIILiteral> namesOfVariables);

private:
    ImmutableIntrinsics() = default;

    Vector<Entry> m_entries;
    UncheckedKeyHashMap<uint64_t, uint16_t> m_numbers; // By holder and the hash of the name.
    uint32_t m_hash { 0 };
};

} // namespace JSC

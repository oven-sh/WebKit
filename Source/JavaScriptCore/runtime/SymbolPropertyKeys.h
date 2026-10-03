/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "Weak.h"
#include "WeakHandleOwner.h"
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/SymbolImpl.h>

#if USE(BUN_JSC_ADDITIONS)

namespace JSC {

class Symbol;
class VM;

// A Structure holds a symbol property key as its SymbolImpl, not as the Symbol cell, so the
// collector sees no edge from an object to the Symbol of one of its keys. Without one, the cell
// dies while the key is still there, and Object.getOwnPropertySymbols hands out a new cell that
// no WeakMap, WeakSet, WeakRef or FinalizationRegistry knows.
//
// The cells that have become a property key get a weak handle here. A Structure adds the
// SymbolImpl of the key of its transition, and of each symbol key of the table it pins, as an
// opaque root when it is visited; a handle whose SymbolImpl is an opaque root marks its cell. So
// a keyed Symbol lives exactly as long as a live Structure holds its key, like an ArrayBuffer's
// wrapper lives as long as a live view holds the buffer.
//
// Only symbols that can be held weakly and that nothing else keeps are tracked: not registered
// symbols (Symbol.for recreates their cell by design and a WeakMap rejects them), not private
// names (their cell lives in the scope that declares them), not static symbols (the well-known
// ones, held by the Symbol constructor).
class SymbolPropertyKeys final : private WeakHandleOwner {
    WTF_MAKE_TZONE_ALLOCATED(SymbolPropertyKeys);
public:
    SymbolPropertyKeys() = default;

    static bool isTracked(SymbolImpl& uid)
    {
        return !uid.isStatic() && !uid.isRegistered() && !uid.isPrivate();
    }

    // The SymbolImpl has just been added to a PropertyTable as a key.
    void noteKey(VM&, SymbolImpl&);

private:
    bool isReachableFromOpaqueRoots(Handle<Unknown>, void* context, AbstractSlotVisitor&, ASCIILiteral* reason) final;
    void finalize(Handle<Unknown>, void* context) final;

    UncheckedKeyHashMap<SymbolImpl*, Weak<Symbol>> m_handles;
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)

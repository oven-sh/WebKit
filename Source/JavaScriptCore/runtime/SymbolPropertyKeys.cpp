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

#include "config.h"
#include "SymbolPropertyKeys.h"

#include "AbstractSlotVisitorInlines.h"
#include "Symbol.h"
#include "WeakInlines.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SymbolPropertyKeys);

void SymbolPropertyKeys::noteKey(VM& vm, SymbolImpl& uid)
{
    if (!isTracked(uid))
        return;
    if (m_handles.get(&uid))
        return;
    // A PropertyName made from a bare SymbolImpl has no cell. Then there is nothing to keep alive.
    Symbol* symbol = vm.symbolImplToSymbolMap.get(&uid);
    if (!symbol)
        return;
    weakAdd(m_handles, &uid, Weak<Symbol>(symbol, this, &uid));
}

bool SymbolPropertyKeys::isReachableFromOpaqueRoots(Handle<Unknown>, void* context, AbstractSlotVisitor& visitor, ASCIILiteral* reason)
{
    if (reason) [[unlikely]]
        *reason = "Symbol is a property key of a live Structure"_s;
    return visitor.containsOpaqueRoot(context);
}

void SymbolPropertyKeys::finalize(Handle<Unknown> handle, void* context)
{
    weakRemove(m_handles, static_cast<SymbolImpl*>(context), static_cast<Symbol*>(handle.get().asCell()));
}

} // namespace JSC

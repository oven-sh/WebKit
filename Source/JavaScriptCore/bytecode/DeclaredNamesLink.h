/*
 * Copyright (C) 2026 Anthropic PBC.
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

#include "Identifier.h"
#include <wtf/RefCounted.h>

namespace JSC {

// The names lexically declared by the scopes enclosing a function at the point where its executable is created
// (one link per enclosing function/program, innermost first). Only built while generating bytecode for a cache
// image; the bytecode optimizer uses it to tell free variables that resolve to an environment record (stable for the
// lifetime of an activation) from ones that fall through to the global object.
class DeclaredNamesLink : public RefCounted<DeclaredNamesLink> {
public:
    static Ref<DeclaredNamesLink> create(IdentifierSet&& names, bool isDynamicBarrier, RefPtr<DeclaredNamesLink> parent)
    {
        return adoptRef(*new DeclaredNamesLink(WTF::move(names), isDynamicBarrier, WTF::move(parent)));
    }

    // True if some enclosing scope declares |name| before the lookup would have to cross a scope whose contents can
    // change at run time (sloppy direct eval, with).
    bool isStablyDeclared(UniquedStringImpl* name) const
    {
        for (const DeclaredNamesLink* link = this; link; link = link->m_parent.get()) {
            if (link->m_names.contains(name))
                return true;
            if (link->m_isDynamicBarrier)
                return false;
        }
        return false;
    }

    DeclaredNamesLink* parent() const { return m_parent.get(); }

private:
    DeclaredNamesLink(IdentifierSet&& names, bool isDynamicBarrier, RefPtr<DeclaredNamesLink> parent)
        : m_names(WTF::move(names))
        , m_parent(WTF::move(parent))
        , m_isDynamicBarrier(isDynamicBarrier)
    {
    }

    IdentifierSet m_names;
    RefPtr<DeclaredNamesLink> m_parent;
    bool m_isDynamicBarrier;
};

} // namespace JSC

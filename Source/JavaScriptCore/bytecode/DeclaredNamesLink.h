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
    // One environment record that is on the scope chain at the creation site, innermost first: the names that
    // live in it and their slots. A barrier frame stands for a `with` object (anything past it is dynamic).
    struct Frame {
        bool isBarrier { false };
        UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, unsigned, IdentifierRepHash> slots; // name -> ScopeOffset
    };

    static Ref<DeclaredNamesLink> create(IdentifierSet&& names, Vector<Frame>&& frames, bool isDynamicBarrier, RefPtr<DeclaredNamesLink> parent)
    {
        return adoptRef(*new DeclaredNamesLink(WTF::move(names), WTF::move(frames), isDynamicBarrier, WTF::move(parent)));
    }

    struct Resolution {
        enum Kind : uint8_t {
            Dynamic, // may resolve differently at run time (globals, eval/with in the way): leave alone
            Stable, // always the same binding for a given starting scope, but no static slot (e.g. an import)
            Slot, // lives |hops| environment records out from the function's own scope, at |offset|
        };
        Kind kind { Dynamic };
        unsigned hops { 0 };
        unsigned offset { 0 };
    };

    // Resolve |name| as seen from a function created at this point (i.e. starting from that function's [[Scope]]).
    Resolution resolve(UniquedStringImpl* name) const
    {
        unsigned hops = 0;
        for (const DeclaredNamesLink* link = this; link; link = link->m_parent.get()) {
            for (auto& frame : link->m_frames) {
                if (frame.isBarrier)
                    return { };
                auto it = frame.slots.find(name);
                if (it != frame.slots.end())
                    return { Resolution::Slot, hops, it->value };
                ++hops;
            }
            if (link->m_names.contains(name))
                return { Resolution::Stable, 0, 0 };
            if (link->m_isDynamicBarrier)
                return { };
        }
        return { };
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
    DeclaredNamesLink(IdentifierSet&& names, Vector<Frame>&& frames, bool isDynamicBarrier, RefPtr<DeclaredNamesLink> parent)
        : m_names(WTF::move(names))
        , m_frames(WTF::move(frames))
        , m_parent(WTF::move(parent))
        , m_isDynamicBarrier(isDynamicBarrier)
    {
    }

    IdentifierSet m_names;
    Vector<Frame> m_frames;
    RefPtr<DeclaredNamesLink> m_parent;
    bool m_isDynamicBarrier;
};

} // namespace JSC

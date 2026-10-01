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

// The scopes enclosing a function at the point where its executable is created: one link per enclosing
// function/program (innermost first), each with the names it declares and the environment records it has on the
// scope chain right then. Only built while generating with OptimizeBytecode::Yes; the bytecode optimizer uses it to
// locate free variables statically and to tell environment-record bindings (stable for the lifetime of an
// activation) from names that fall through to the global object.
// `const f = function () { }`, `g = () => { }`, `class C { }`: the statement stores the function that it creates in the variable.
// Recorded by the bytecode generator, from the syntax tree.
struct FunctionAssignment {
    unsigned identifier { 0 }; // Of the variable: UnlinkedCodeBlock::identifier().
    unsigned functionExpr { 0 }; // UnlinkedCodeBlock::functionExpr(). Of a class, its constructor.
    // If it is a variable of the code's own: the constant that has the symbol table of its scope, and where it is in that.
    bool isOwn { false };
    int symbolTableConstantIndex { 0 };
    unsigned scopeOffset { 0 };
};

class DeclaredNamesLink : public RefCounted<DeclaredNamesLink> {
public:
    // Names an enclosing module binds stably without giving them an environment slot of its own (imports). Every
    // other name a nested function can see is captured and therefore has a slot in some Frame.
    struct Names : public RefCounted<Names> {
        static Ref<Names> create(IdentifierSet&& names) { return adoptRef(*new Names { WTF::move(names) }); }
        IdentifierSet names;

    private:
        explicit Names(IdentifierSet&& names)
            : names(WTF::move(names))
        {
        }
    };
    // One environment record that is on the scope chain at the creation site (innermost first): the names that live
    // in it and their slots. A barrier frame stands for a `with` object or a sloppy eval's var scope: anything at or
    // past it is dynamic.
    struct Frame : public RefCounted<Frame> {
        using Slots = UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, unsigned, IdentifierRepHash>; // name -> ScopeOffset | lazyFunctionSlotFlag
        static constexpr unsigned lazyFunctionSlotFlag = 1u << 31; // a module's function declaration: read it with ResolvedLazyClosureVar
        static constexpr unsigned readOnlySlotFlag = 1u << 30; // storing to it, other than to initialize it, throws
        static Ref<Frame> create(bool isBarrier, Slots&& slots, RefPtr<Frame> next, const void* identity = nullptr) { return adoptRef(*new Frame { isBarrier, WTF::move(slots), WTF::move(next), identity }); }
        bool isBarrier;
        Slots slots;
        RefPtr<Frame> next;
        // Which scope of the source the record is for: the same for every Frame there is for it (there is another whenever a name has
        // been added to it), and for the code that makes the record: it is the SymbolTable among its constants that it makes it from.
        const void* identity;

    private:
        Frame(bool isBarrier, Slots&& slots, RefPtr<Frame> next, const void* identity)
            : isBarrier(isBarrier)
            , slots(WTF::move(slots))
            , next(WTF::move(next))
            , identity(identity)
        {
        }
    };

    // isOutermost: for the code of a module or a program, which is only enclosed by the global scopes. (A function's link may lack
    // a parent simply because none was provided.)
    static Ref<DeclaredNamesLink> create(RefPtr<Names> names, RefPtr<Frame> frames, bool isDynamicBarrier, bool isOutermost, RefPtr<DeclaredNamesLink> parent)
    {
        return adoptRef(*new DeclaredNamesLink(WTF::move(names), WTF::move(frames), isDynamicBarrier, isOutermost, WTF::move(parent)));
    }

    struct Resolution {
        enum Kind : uint8_t {
            Dynamic, // may resolve differently at run time (eval/with in the way): leave alone
            Stable, // always the same binding for a given starting scope, but no static slot (an import of the module whose environment is |hops| records out)
            Slot, // lives |hops| environment records out from the function's own scope, at |offset|
            Global, // in none of the environment records of the code around the function: which global scope has it, if any, may change
        };
        Kind kind { Dynamic };
        unsigned hops { 0 };
        unsigned offset { 0 };
        bool isLazyFunctionSlot { false };
        bool isInOutermostEnvironment { false }; // Slot: it is a variable of the module (or the program) itself.
        const void* scope { nullptr }; // Slot: Frame::identity of the record.
        bool isReadOnly { false };
    };

    // Resolve |name| as seen from a function created at this point (i.e. starting from that function's [[Scope]]).
    Resolution resolve(UniquedStringImpl* name) const
    {
        unsigned hops = 0;
        for (const DeclaredNamesLink* link = this; link; link = link->m_parent.get()) {
            for (const Frame* frame = link->m_frames.get(); frame; frame = frame->next.get()) {
                if (frame->isBarrier)
                    return { };
                auto it = frame->slots.find(name);
                if (it != frame->slots.end())
                    return { Resolution::Slot, hops, it->value & ~(Frame::lazyFunctionSlotFlag | Frame::readOnlySlotFlag), !!(it->value & Frame::lazyFunctionSlotFlag), link->m_isOutermost && !frame->next, frame->identity, !!(it->value & Frame::readOnlySlotFlag) };
                ++hops;
            }
            // The link that has names is a module's, and the last of its frames the module's environment.
            if (link->m_names && link->m_names->names.contains(name))
                return { Resolution::Stable, hops ? hops - 1 : 0, 0 };
            if (link->m_isDynamicBarrier)
                return { };
            if (link->m_isOutermost)
                return { Resolution::Global, hops, 0 };
        }
        return { };
    }

    // Frame::identity of the record that is that many out from the [[Scope]] of a function created at this point. Null: there is no telling.
    const void* identityOfScope(unsigned hops) const
    {
        for (const DeclaredNamesLink* link = this; link; link = link->m_parent.get()) {
            for (const Frame* frame = link->m_frames.get(); frame; frame = frame->next.get()) {
                if (frame->isBarrier)
                    return nullptr;
                if (!hops--)
                    return frame->identity;
            }
            if (link->m_isDynamicBarrier)
                return nullptr;
        }
        return nullptr;
    }
    template<typename Functor> void forEachScope(const Functor& functor) const
    {
        for (const DeclaredNamesLink* link = this; link; link = link->m_parent.get()) {
            for (const Frame* frame = link->m_frames.get(); frame; frame = frame->next.get()) {
                if (frame->identity)
                    functor(frame->identity);
            }
        }
    }

    // Whether the [[Scope]] of a function created at this point is the environment of the module (or the program) itself.
    bool scopeIsOutermostEnvironment() const { return m_isOutermost && m_frames && !m_frames->isBarrier && !m_frames->next; }

    Names* names() const { return m_names.get(); }
    Frame* frames() const { return m_frames.get(); }

private:
    DeclaredNamesLink(RefPtr<Names> names, RefPtr<Frame> frames, bool isDynamicBarrier, bool isOutermost, RefPtr<DeclaredNamesLink> parent)
        : m_names(WTF::move(names))
        , m_frames(WTF::move(frames))
        , m_parent(WTF::move(parent))
        , m_isDynamicBarrier(isDynamicBarrier)
        , m_isOutermost(isOutermost)
    {
    }

    RefPtr<Names> m_names;
    RefPtr<Frame> m_frames;
    RefPtr<DeclaredNamesLink> m_parent;
    bool m_isDynamicBarrier;
    bool m_isOutermost;
};

} // namespace JSC

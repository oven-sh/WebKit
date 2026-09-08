/*
 * Copyright (C) 2012-2021 Apple Inc. All rights reserved.
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

#include "GetPutInfo.h"
#include "JSObject.h"
#include "VariableEnvironment.h"
#include <array>

namespace JSC {

class ScopeChainIterator;
class SymbolTable;
class WatchpointSet;

using TDZEnvironment = UncheckedKeyHashSet<RefPtr<UniquedStringImpl>, IdentifierRepHash>;

// Options::useGlobalResolveMemo(): per-realm, mutator-only memo of what JSScope::abstractResolve() decides at the global
// lexical environment / global object, which is the same for every scope chain of the realm. Symbol-table-decided
// entries (GlobalVar, GlobalLexicalVar, read-only -> Dynamic) live until JSGlobalObject::invalidateGlobalResolveMemo()
// (global symbol table / lexical binding added); structure-decided ones (GlobalProperty, UnresolvedProperty) also die
// when the global object's structure, maxOffset or dictionary kind moves on (JSGlobalObject::globalResolveMemoFor-
// Resolve(), which keeps that Structure alive so a recycled one cannot validate them). Whatever else an entry stands for
// is re-checked when the linked instruction runs, as for a CodeBlock linked earlier. Entries reference no GC cell.
class GlobalResolveMemo {
    WTF_MAKE_NONCOPYABLE(GlobalResolveMemo);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(GlobalResolveMemo);
public:
    GlobalResolveMemo() = default;
    enum class Depth : uint8_t { Zero, GlobalLexicalEnvironment, GlobalObject }; // ResolveOp::depth is 0 / the global lexical environment's / one more
    struct Entry {
        ResolveType type;
        Depth depth;
        bool hasStructure; // ResolveOp::structure is the global object's (memo-wide) structure
        InlineWatchpointSet* watchpointSet;
        uintptr_t operand;
        bool isStructureDecided() const { return type == GlobalProperty || type == GlobalPropertyWithVarInjectionChecks || type == UnresolvedProperty || type == UnresolvedPropertyWithVarInjectionChecks; }
    };
    using Map = UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, Entry, IdentifierRepHash>;
    Map& map(GetOrPut getOrPut, bool needsVarInjectionChecks) { return m_maps[(getOrPut == Put ? 2 : 0) | (needsVarInjectionChecks ? 1 : 0)]; }
    void add(Map& map, UniquedStringImpl* ident, const Entry& entry)
    {
        map.add(ident, entry);
        m_hasStructureDecidedEntries |= entry.isStructureDecided();
    }
    void clear()
    {
        for (auto& map : m_maps)
            map.clear();
        m_hasStructureDecidedEntries = false;
    }
    void clearStructureDecidedEntries()
    {
        if (!m_hasStructureDecidedEntries)
            return;
        for (auto& map : m_maps)
            map.removeIf([](auto& keyValue) { return keyValue.value.isStructureDecided(); });
        m_hasStructureDecidedEntries = false;
    }
private:
    std::array<Map, 4> m_maps;
    bool m_hasStructureDecidedEntries { false };
};

class JSScope : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    template<typename, SubspaceAccess>
    static void subspaceFor(VM&)
    {
        RELEASE_ASSERT_NOT_REACHED();
    }

    DECLARE_EXPORT_INFO;

    friend class LLIntOffsetsExtractor;
    static size_t offsetOfNext();

    static JSObject* NODELETE objectAtScope(JSScope*);

    static JSObject* resolve(JSGlobalObject*, JSScope*, const Identifier&);
    static JSValue resolveScopeForHoistingFuncDeclInEval(JSGlobalObject*, JSScope*, const Identifier&);
    static ResolveOp abstractResolve(JSGlobalObject*, size_t depthOffset, JSScope*, const Identifier&, GetOrPut, ResolveType, InitializationMode);

    static bool hasConstantScope(ResolveType);
    static JSScope* NODELETE constantScopeForCodeBlock(ResolveType, CodeBlock*);

    static void collectClosureVariablesUnderTDZ(JSScope*, TDZEnvironment& result, PrivateNameEnvironment&);

    DECLARE_VISIT_CHILDREN;

    bool NODELETE isVarScope();
    bool NODELETE isLexicalScope();
    bool NODELETE isModuleScope();
    bool NODELETE isCatchScope();
    bool NODELETE isCatchScopeWithSimpleParameter();
    bool NODELETE isFunctionNameScopeObject();

    bool NODELETE isNestedLexicalScope();

    ScopeChainIterator begin();
    ScopeChainIterator end();
    JSScope* next();

    JSObject* globalThis();

    SymbolTable* NODELETE symbolTable();

protected:
    JSScope(VM&, Structure*, JSScope* next);

    template<typename ReturnPredicateFunctor, typename SkipPredicateFunctor>
    static JSObject* resolve(JSGlobalObject*, JSScope*, const Identifier&, ReturnPredicateFunctor, SkipPredicateFunctor);

private:
    WriteBarrier<JSScope> m_next;
};

inline JSScope::JSScope(VM& vm, Structure* structure, JSScope* next)
    : Base(vm, structure)
    , m_next(next, WriteBarrierEarlyInit)
{
}

class ScopeChainIterator {
public:
    ScopeChainIterator(JSScope* node)
        : m_node(node)
    {
    }

    JSObject* get() const;
    JSObject* operator->() const;
    JSScope* scope() const { return m_node; }

    ScopeChainIterator& operator++() { m_node = m_node->next(); return *this; }

    // postfix ++ intentionally omitted

    friend bool operator==(const ScopeChainIterator&, const ScopeChainIterator&) = default;

private:
    JSScope* m_node;
};

inline ScopeChainIterator JSScope::begin()
{
    return ScopeChainIterator(this); 
}

inline ScopeChainIterator JSScope::end()
{ 
    return ScopeChainIterator(nullptr); 
}

inline JSScope* JSScope::next()
{ 
    return m_next.get();
}

inline size_t JSScope::offsetOfNext()
{
    return OBJECT_OFFSETOF(JSScope, m_next);
}

} // namespace JSC

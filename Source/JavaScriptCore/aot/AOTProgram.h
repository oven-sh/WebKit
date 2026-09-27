/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTStubs.h"
#include "DeclaredNamesLink.h"
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSGlobalObject;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

namespace AOT {

// A function that the compiler has in front of it while it compiles a call.
struct KnownFunction {
    UnlinkedFunctionExecutable* executable { nullptr };
    UnlinkedFunctionCodeBlock* forCall { nullptr }; // Either may be missing.
    UnlinkedFunctionCodeBlock* forConstruct { nullptr };
    ImageKey key; // Of the code for a call. That for construction is the same but for the bit that says so.

    ImageKey keyFor(bool isConstruct) const
    {
        ImageKey result = key;
        result.kind |= isConstruct;
        return result;
    }
};

// What a variable that is called probably holds. A program's functions are nearly all declared once, at the top of a module, and
// never assigned to; but that takes the whole program to prove, and a debugger or an eval to undo. So nothing rests on it. It is a
// reason to compile a call for that callee, behind a check that it is the callee (see Lowering::lowerCallToKnownFunction()).
class CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(CalleeHints);
    WTF_MAKE_NONCOPYABLE(CalleeHints);
public:
    CalleeHints() = default;
    virtual ~CalleeHints();

    // scopeOffset: where the variable is in the scope it was resolved to. None: it was not resolved, so it may be a global.
    virtual const KnownFunction* find(UniquedStringImpl* name, std::optional<unsigned> scopeOffset) const = 0;
};

// From the code of a module: its function declarations, and the variables it initializes with a function or a class.
class ModuleHints final : public CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(ModuleHints);
public:
    // What there is to know about a function of the module. False: nothing.
    using Describe = Function<bool(UnlinkedFunctionExecutable*, KnownFunction&)>;
    ModuleHints(UnlinkedCodeBlock* codeOfModule, const Describe&);
    ~ModuleHints() final;

    const KnownFunction* find(UniquedStringImpl*, std::optional<unsigned> scopeOffset) const final;

private:
    struct Variable {
        unsigned scopeOffset { 0 };
        bool isAmbiguous { false };
        KnownFunction function;
    };
    void add(UniquedStringImpl*, unsigned scopeOffset, UnlinkedFunctionExecutable*, const Describe&);

    UncheckedKeyHashMap<UniquedStringImpl*, Variable> m_variables;
};

// An import that whoever linked the program's modules resolved to a variable of another of them.
struct StaticImport {
    unsigned slot { 0 }; // JSModuleEnvironment::importSlot(), of the environment of the module that imports.
    unsigned scopeOffsetOfSlot { 0 }; // JSModuleEnvironment::importSlotScopeOffset() of that.
    unsigned scopeOffset { 0 }; // Of the variable, in the environment of the module that has it.
};

// What is known of a module because the whole program is there when it is compiled. Unlike a hint, code rests on it with no check
// of its own: it is only run for a module whose record has been seen to be linked that way (JSModuleRecord::isLinkedAsInImage()).
class ModuleLinkage {
    WTF_MAKE_TZONE_ALLOCATED(ModuleLinkage);
    WTF_MAKE_NONCOPYABLE(ModuleLinkage);
public:
    ModuleLinkage() = default;

    void addImport(UniquedStringImpl* localName, StaticImport import) { m_imports.add(localName, import); }
    const StaticImport* findImport(UniquedStringImpl* localName) const
    {
        auto it = m_imports.find(localName);
        return it == m_imports.end() ? nullptr : &it->value;
    }

private:
    UncheckedKeyHashMap<UniquedStringImpl*, StaticImport> m_imports;
};

// Options::resolveAllScopeSlotsStatically(): what the code around a function declares, kept from when the function's bytecode was
// generated until it is compiled. For the code of a module itself: as its own functions see it.
void noteDeclaredNames(UnlinkedCodeBlock*, RefPtr<DeclaredNamesLink>&&);
const DeclaredNamesLink* declaredNamesFor(UnlinkedCodeBlock*); // Any thread. Good until forgetDeclaredNames().
void forgetDeclaredNames();

// Options::aotUseLiveCalleeHints(): from what the global variables hold when the caller is compiled. For testing what the hints
// are used for on programs that are not modules.
class LiveHints final : public CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(LiveHints);
public:
    explicit LiveHints(JSGlobalObject*);
    ~LiveHints() final;

    const KnownFunction* find(UniquedStringImpl*, std::optional<unsigned> scopeOffset) const final;

private:
    JSGlobalObject* m_globalObject;
    mutable Vector<std::unique_ptr<KnownFunction>> m_functions;
};

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

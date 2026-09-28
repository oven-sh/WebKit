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
    // The variable it was found in holds a closure of it from when it is initialized, and never anything else: see
    // ModuleHints::prove(). Then a call of what is read from that variable needs no check that this is the callee.
    bool isProven { false };
    // If so: everything that a call of it can return. It is worked out for all of them together, from nothing up
    // (inferReturnTypeForImage()), and means what it says once that has come to an end.
    mutable std::atomic<uint32_t> returnType { 0 };

    KnownFunction() = default;
    KnownFunction(const KnownFunction& other) { *this = other; }
    KnownFunction& operator=(const KnownFunction& other)
    {
        executable = other.executable;
        forCall = other.forCall;
        forConstruct = other.forConstruct;
        key = other.key;
        isProven = other.isProven;
        returnType = other.returnType.load(std::memory_order_relaxed);
        return *this;
    }

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

    // What is a hint until the whole of the module has been looked at. Nobody but the module's own code can store to its variables.
    void noteStoresIn(UnlinkedCodeBlock* functionOfModule); // Every function there is in the module, however deep.
    void prove(); // After that.
    unsigned numberOfVariables() const { return m_variables.size(); }
    unsigned numberProven() const;
    template<typename Functor> void forEachProven(const Functor& functor) const
    {
        for (auto& entry : m_variables) {
            if (entry.value.function.isProven)
                functor(entry.value.function);
        }
    }

private:
    struct Variable {
        unsigned scopeOffset { 0 };
        bool isAmbiguous { false };
        bool isInitializedInPlainSight { false }; // Declared as a function, or stored once, with a function made just before, in a straight line.
        bool isStoredToOtherwise { false };
        KnownFunction function;
    };
    void noteStore(UniquedStringImpl*);
    bool m_hasEval { false };
    void add(UniquedStringImpl*, unsigned scopeOffset, UnlinkedFunctionExecutable*, const Describe&);

    UncheckedKeyHashMap<UniquedStringImpl*, Variable> m_variables;
};

// An import that whoever linked the program's modules resolved to a variable of another of them.
struct StaticImport {
    unsigned slot { 0 }; // JSModuleEnvironment::importSlot(), of the environment of the module that imports.
    unsigned scopeOffsetOfSlot { 0 }; // JSModuleEnvironment::importSlotScopeOffset() of that.
    unsigned scopeOffset { 0 }; // Of the variable, in the environment of the module that has it.
    const KnownFunction* function { nullptr }; // What that module's code puts in the variable, if it is a function (ModuleHints).
    uint32_t distanceOfEnvironment { 0 }; // Of the module that has it: ImageEnvironment::distance.
};

// What is known of a module because the whole program is there when it is compiled. Unlike a hint, code rests on it with no check
// of its own: it is only run for a module whose record has been seen to be linked that way (JSModuleRecord::isLinkedAsInImage()).
class ModuleLinkage {
    WTF_MAKE_TZONE_ALLOCATED(ModuleLinkage);
    WTF_MAKE_NONCOPYABLE(ModuleLinkage);
public:
    ModuleLinkage() = default;

    uint32_t distanceOfEnvironment { 0 }; // Of the module itself: ImageEnvironment::distance.

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

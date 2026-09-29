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

// What the whole of the program says of a function that is proven (KnownFunction::isProven). Every piece of code there is has its say,
// on any thread, and what is said only ever adds to what has been said. It means what it says once they have all had it.
struct ProgramFacts {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(ProgramFacts);

    // The function gets somewhere as a value: it is stored, passed, compared, constructed with, asked for a property; or it is called
    // in a way that is not a call of this function and no other. If not, whoever calls it is known, all of them.
    std::atomic<bool> valueIsUsed { false };
    std::atomic<uint32_t> directCalls { 0 };

    // Once that is settled. Nobody gets to call it but the calls that are calls of this function and no other.
    bool isClosed { false };
    // If closed: everything that is passed for each parameter, `this` being the first, from nothing up (see KnownFunction::returnType).
    // One that there are more of than this has nothing said of the rest.
    static constexpr unsigned mostParameters = 12;
    std::array<std::atomic<uint32_t>, mostParameters> parameterTypes { };
};
// (One for the code of a function, however many variables hold it.)
using FactsOfExecutables = UncheckedKeyHashMap<UnlinkedFunctionExecutable*, ProgramFacts*>;

// A variable that lives in an environment record: of a module, or of a function whose inner functions use it.
struct Variable {
    const void* scope { nullptr }; // DeclaredNamesLink::Frame::identity
    unsigned offset { 0 };
    // In place of an offset: what every variable of the scope holds when the record is made.
    static constexpr unsigned initialValue = std::numeric_limits<unsigned>::max();
    explicit operator bool() const { return !!scope; }
};

// Everything that is ever put in each variable, from nothing up. Nothing gets to write one but the code of the program, which is all
// there to be looked at; where that cannot tell which variable it writes, or something else can write it, nothing is said of it.
class VariableFacts {
    WTF_MAKE_TZONE_ALLOCATED(VariableFacts);
    WTF_MAKE_NONCOPYABLE(VariableFacts);
public:
    VariableFacts() = default;

    // Before any of the rest: any thread.
    void giveUpOnName(UniquedStringImpl*);
    void giveUpOnScope(const void*);

    // Any thread. reader: told of by takeReadersOfWhatGrew() if there turns out to be more to it. TAll: nothing is known.
    static constexpr unsigned nobody = std::numeric_limits<unsigned>::max();
    uint32_t read(Variable, UniquedStringImpl* name, unsigned reader);
    void join(Variable, uint32_t type);

    // Not while any of that is going on.
    Vector<unsigned> takeReadersOfWhatGrew();
    template<typename Functor> void forEach(const Functor& functor) const
    {
        for (auto& shard : m_shards) {
            for (auto& entry : shard.cells)
                functor(Variable { entry.key.first, entry.key.second }, entry.value->type.load(std::memory_order_relaxed));
        }
    }
    bool hasGivenUpOn(Variable, UniquedStringImpl* name) const;

private:
    using SetOfReaders = UncheckedKeyHashSet<unsigned, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>>;
    struct Cell {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(Cell);
        std::atomic<uint32_t> type { 0 };
        bool grew { false };
        SetOfReaders readers;
    };
    struct Shard {
        Lock lock;
        UncheckedKeyHashMap<std::pair<const void*, unsigned>, std::unique_ptr<Cell>> cells;
    };
    static constexpr unsigned numberOfShards = 64;
    Shard& shardFor(Variable variable) { return m_shards[(std::bit_cast<uintptr_t>(variable.scope) >> 4 ^ variable.offset) % numberOfShards]; }

    std::array<Shard, numberOfShards> m_shards;
    Lock m_givenUpLock;
    UncheckedKeyHashSet<UniquedStringImpl*> m_namesGivenUpOn;
    UncheckedKeyHashSet<const void*> m_scopesGivenUpOn;
};

// A function that the compiler has in front of it while it compiles a call.
struct KnownFunction {
    UnlinkedFunctionExecutable* executable { nullptr };
    UnlinkedFunctionCodeBlock* forCall { nullptr }; // Either may be missing.
    UnlinkedFunctionCodeBlock* forConstruct { nullptr };
    ImageKey key; // Of the code for a call. That for construction is the same but for the bit that says so.
    // The variable it was found in holds a closure of it from when it is initialized, and never anything else: see
    // ModuleHints::prove(). Then a call of what is read from that variable needs no check that this is the callee, once it is seen
    // to have been initialized.
    bool isProven { false };
    bool isDeclaration { false }; // The variable is initialized before any code of the module runs.
    // If proven: the code for a call makes no use of the object it is called as (needsFunctionObject()), and a call passes none.
    // (Whoever compiles the program takes it back if it turns out that there is no code to call: BytecodeLinkEncoder.)
    mutable std::atomic<bool> needsNoFunctionObject { false };
    // If proven: everything that a call of it can return. It is worked out for all of them together, from nothing up
    // (inferReturnTypeForImage()), and means what it says once that has come to an end.
    mutable std::atomic<uint32_t> returnType { 0 };
    mutable ProgramFacts* facts { nullptr }; // If proven, and whoever compiles the program keeps them.

    KnownFunction() = default;
    KnownFunction(const KnownFunction& other) { *this = other; }
    KnownFunction& operator=(const KnownFunction& other)
    {
        executable = other.executable;
        forCall = other.forCall;
        forConstruct = other.forConstruct;
        key = other.key;
        isProven = other.isProven;
        isDeclaration = other.isDeclaration;
        needsNoFunctionObject = other.needsNoFunctionObject.load(std::memory_order_relaxed);
        returnType = other.returnType.load(std::memory_order_relaxed);
        facts = other.facts;
        return *this;
    }

    ImageKey keyFor(bool isConstruct) const
    {
        ImageKey result = key;
        result.kind |= isConstruct;
        return result;
    }
};

// Whether code can tell which object it was called as, other than by way of the scope that has, if that is the environment of the
// module: which is where it is (ModuleLinkage::distanceOfEnvironment). From the bytecode alone, so that the function and whoever
// calls it come to the same answer.
bool needsFunctionObject(UnlinkedCodeBlock*);

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
    bool m_isModule { false };
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
    const void* scope { nullptr }; // Variable::scope of the environment of the module that has it.
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

// The code of a function says which name it means by a number. That is where the name is among the function's own identifiers, unless
// the whole program's identifiers have been numbered: then there is one table of them for all of its functions. That takes something
// that makes the table (StaticHeap), which is the only way there is then of getting the code to run.
using NumbersOfIdentifiers = UncheckedKeyHashMap<UniquedStringImpl*, uint32_t>;
JS_EXPORT_PRIVATE void setNumbersOfIdentifiersOfProgram(const NumbersOfIdentifiers*); // Not while anything is being compiled.
const NumbersOfIdentifiers* numbersOfIdentifiersOfProgram();

// Likewise the constants of functions whose constants are of no realm: one table of them for the program, in which what is the same
// (a string that says the same, a number) is there once. For each such function, the number of each of its constants, or
// notAConstantOfProgram for one that is empty.
static constexpr uint32_t notAConstantOfProgram = std::numeric_limits<uint32_t>::max();
using NumbersOfConstants = UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<uint32_t>>;
JS_EXPORT_PRIVATE void setNumbersOfConstantsOfProgram(const NumbersOfConstants*); // Not while anything is being compiled.
const Vector<uint32_t>* numbersOfConstantsOfProgramFor(UnlinkedCodeBlock*); // Null: the function has its own.

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

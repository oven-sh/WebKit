/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTStubs.h"
#include "AOTType.h"
#include "DeclaredNamesLink.h"
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSGlobalObject;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class UnlinkedModuleProgramCodeBlock;
class VM;

namespace AOT {

// What the whole of the program says of a function that is proven (KnownFunction::isExact). Every piece of code there is has its say,
// on any thread, and what is said only ever adds to what has been said. It means what it says once they have all had it.
struct FunctionSummary {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(FunctionSummary);

    // The function gets somewhere as a value: it is stored, passed, compared, constructed with, asked for a property; or it is called
    // in a way that is not a call of this function and no other. If not, whoever calls it is known, all of them.
    std::atomic<bool> valueIsUsed { false };
    // The first thing that was seen to make that so, for the log. See Graph::recordUsesOfKnownFunctions().
    enum ValueUseReason : uint32_t { NotSaid, AtCreationSite, CalleeWithInexactRead, CalleeWithInexactCall, Operand };
    std::atomic<uint32_t> valueUseReason { 0 }; // ValueUseReason | what uses it (an opcode, or 1000 + a kind of node) << 8
    std::atomic<uint32_t> directCalls { 0 };
    // Called from inside a loop, or handed to something of the language's that calls what it is handed once for each of many things. That is all that is known of how often it runs.
    std::atomic<bool> isUsedInLoop { false };

    // Once that is settled. Nobody gets to call it but the calls that are calls of this function and no other.
    bool isNonEscaping { false };
    // The function has got somewhere that is not reckoned with: from there anybody may call it, with anything, and
    // does who knows what with what it returns. If that never happens it is closed. (While that is being worked out isClosed is
    // set for all of them: one that turns out to be exposed is passed anything, which is as good as saying nothing.)
    mutable std::atomic<bool> escapes { false };
    uint32_t number { 0 }; // FunctionsOfProgram
    // The first thing that was seen to make that so, for the log.
    enum EscapeReason : uint32_t {
        DoesNotEscape, ReportedByBundler, CreationSiteUnknown, ReferencesItself, ExternalFunction, NotCallable,
        UsedBy, // | an opcode << 8, or 1000 + a kind of node
        PassedToUnknownCallee, PassedAsThis, PassedAsExtraArgument, CalledIndirectly, ReturnedToUnknownCaller,
        MergedInPhi, MergedInFrameRegister, MergedInVariable, MergedInParameter, MergedInReturn, LostThroughAlias,
        StoredInModuleVariable, StoredInUntrackedVariable, StoredToUnknownLocation, StoredInDynamicallyReadVariable, ReadInexactly,
    };
    mutable std::atomic<uint32_t> escapeReason { 0 };
    // Whether that is news. hadBeenPassed: told what each parameter had been passed. It is one of all the things that the parameter may be passed now, and the code of the function
    // no longer knows what it is: so what could be told apart until now has got somewhere that is not reckoned with, too.
    template<typename Functor>
    bool markEscaping(uint32_t why, const Functor& hadBeenPassed)
    {
        if (escapes.exchange(true, std::memory_order_relaxed))
            return false;
        escapeReason.store(why, std::memory_order_relaxed);
        for (auto& type : parameterTypes)
            hadBeenPassed(type.join(TTop));
        hadBeenPassed(thisType.join(TTop));
        return true;
    }
    // (Before anything has been passed to anything.)
    bool markEscaping(uint32_t why) { return markEscaping(why, [](Type) { }); }
    // If closed: everything that it is called on. (parameterTypes[0] says whether it is reached at all.)
    AtomicType thisType;
    bool isReached() const { return !isNonEscaping || escapes.load(std::memory_order_relaxed) || parameterTypes[0].load(); }
    // If closed: everything that is passed for each parameter, `this` being the first, from nothing up (see KnownFunction::returnType).
    // One that there are more of than this has nothing said of the rest.
    static constexpr unsigned mostParameters = 12;
    std::array<AtomicType, mostParameters> parameterTypes { };
    // KnownFunction::returnType, where the function itself finds it.
    mutable AtomicType returnType;
    // What it returns is, every time, an object that a literal has just made with properties of the same names (MultiValueReturnTable). If it is closed, and nobody who calls it does
    // anything with the object but read those, there is no object: what it would have been made with is handed back as it is, each thing in a register, as a function in Go hands back several.
    static constexpr unsigned maxReturnValues = 8;
    mutable std::array<AtomicType, maxReturnValues> returnValueTypes { }; // From nothing up, like the rest.
    // Somebody who calls it does something else with what it returns. From false to true, and never back.
    mutable std::atomic<bool> needsReturnObject { false };
    // One or the other has changed since whoever goes round asking last asked.
    mutable std::atomic<bool> returnValueTypesChanged { false };
    // What the function is passed that something may still be able to get at when it has returned: a bit for each parameter, `this` being
    // the first. It goes by the code of the function and of what that calls, whoever calls it: from nothing up, like the rest.
    // It takes for granted that reading and writing properties of what was passed runs nobody's code.
    static constexpr unsigned maxTrackedEscapingParameters = 31;
    static constexpr uint32_t whatIsPassedBeyondParametersEscapes = 1u << 31;
    mutable std::atomic<uint32_t> escapingParameters { 0 };
};
// (One for the code of a function, however many variables hold it.)
using FunctionSummaryMap = UncheckedKeyHashMap<UnlinkedFunctionExecutable*, FunctionSummary*>;

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
class VariableSummaries {
    WTF_MAKE_TZONE_ALLOCATED(VariableSummaries);
    WTF_MAKE_NONCOPYABLE(VariableSummaries);
public:
    VariableSummaries() = default;

    // Before any of the rest: any thread.
    void giveUpOnName(UniquedStringImpl*);
    void giveUpOnScope(const void*);
    // Something reads a variable of that name, and there is no telling which. What it gets is anything, as far as it knows: so whatever is in a
    // variable of that name has got somewhere nobody keeps track of.
    void recordDynamicReadOfName(UniquedStringImpl*);
    bool isDynamicallyRead(UniquedStringImpl* name) const { return m_dynamicallyReadNames.contains(name); }
    void noteScopeOfModule(const void* scope) { m_scopesOfModules.add(scope); } // One thread.
    bool isScopeOfModule(const void* scope) const { return m_scopesOfModules.contains(scope); }

    // Any thread. reader: told of by takeReadersOfWidenedVariables() if there turns out to be more to it. TAll: nothing is known.
    static constexpr unsigned nobody = std::numeric_limits<unsigned>::max();
    Type read(Variable, UniquedStringImpl* name, unsigned reader);
    Type join(Variable, Type); // What was there before.

    // Not while any of that is going on.
    Vector<unsigned> takeReadersOfWidenedVariables();
    // A variable that is read holds what its scope was made with, if nothing else. One that nothing at all is known to be in is one
    // whose scope is not the one it was taken for, or is only read by code that nothing gets to. Nothing is said of those any more.
    Vector<unsigned> untrackVariablesReadButNeverWritten(unsigned& count);
    template<typename Functor> void forEach(const Functor& functor) const
    {
        for (auto& shard : m_shards) {
            for (auto& entry : shard.cells)
                functor(Variable { entry.key.first, entry.key.second }, entry.value->type.load());
        }
    }
    bool isUntracked(Variable, UniquedStringImpl* name) const;

private:
    using SetOfReaders = UncheckedKeyHashSet<unsigned, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>>;
    struct Cell {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(Cell);
        AtomicType type;
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
    UncheckedKeyHashSet<UniquedStringImpl*> m_untrackedNames;
    UncheckedKeyHashSet<UniquedStringImpl*> m_dynamicallyReadNames;
    UncheckedKeyHashSet<const void*> m_untrackedScopes;
    UncheckedKeyHashSet<const void*> m_scopesOfModules;
};

// A function that the compiler has in front of it while it compiles a call.
struct KnownFunction {
    UnlinkedFunctionExecutable* executable { nullptr };
    UnlinkedFunctionCodeBlock* forCall { nullptr }; // Either may be missing.
    UnlinkedFunctionCodeBlock* forConstruct { nullptr };
    ImageKey key; // Of the code for a call. That for construction is the same but for the bit that says so.
    Convention conventionForCall; // conventionOf() each of them.
    Convention conventionForConstruct;
    // The variable it was found in holds a closure of it from when it is initialized, and never anything else: the bundler, which
    // has seen every use there is of the variable, says so (ModuleHints::prove()). Then a call of what is read from that variable needs no check that this is the callee, once it is seen
    // to have been initialized.
    bool isExact { false };
    bool isDeclaration { false }; // The variable is initialized before any code of the module runs.
    // What the variable holds can get somewhere other than into a call of it: PrelinkedModuleGraph::Binding::Escapes.
    bool escapes { true };
    // ...by way of something that is not a read of the variable by the code of the program: PrelinkedModuleGraph::Binding::IsVisibleFromOutside.
    bool isVisibleFromOutside { true };
    // If proven: the code for a call makes no use of the object it is called as (needsFunctionObject()), and a call passes none.
    // (Whoever compiles the program takes it back if it turns out that there is no code to call: BytecodeLinkEncoder.)
    mutable std::atomic<bool> needsNoFunctionObject { false };
    // If proven: everything that a call of it can return. It is worked out for all of them together, from nothing up
    // (inferReturnTypeForImage()), and means what it says once that has come to an end.
    mutable AtomicType returnType;
    mutable FunctionSummary* facts { nullptr }; // If proven, and whoever compiles the program keeps them.

    KnownFunction() = default;
    KnownFunction(const KnownFunction& other) { *this = other; }
    KnownFunction& operator=(const KnownFunction& other)
    {
        executable = other.executable;
        forCall = other.forCall;
        forConstruct = other.forConstruct;
        key = other.key;
        conventionForCall = other.conventionForCall;
        conventionForConstruct = other.conventionForConstruct;
        isExact = other.isExact;
        isDeclaration = other.isDeclaration;
        escapes = other.escapes;
        isVisibleFromOutside = other.isVisibleFromOutside;
        needsNoFunctionObject = other.needsNoFunctionObject.load(std::memory_order_relaxed);
        returnType = other.returnType;
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

// Whether the code reads the object it is called as, other than to get at the scope.
bool readsCallee(UnlinkedCodeBlock*);
// Whoever is called is told what it was called as. Most code makes nothing of that.
JS_EXPORT_PRIVATE bool mayReferenceItself(UnlinkedCodeBlock*);

// Every function of the program, by the number it goes by in a type (typeOfFunction()).
class FunctionsOfProgram {
    WTF_MAKE_TZONE_ALLOCATED(FunctionsOfProgram);
    WTF_MAKE_NONCOPYABLE(FunctionsOfProgram);
public:
    FunctionsOfProgram() = default;

    // While it is being put together.
    uint32_t add(const KnownFunction& function)
    {
        m_functions.append(makeUniqueWithoutFastMallocCheck<KnownFunction>(function));
        RELEASE_ASSERT(m_functions.size() < (1u << bitsOfFunctionNumber));
        return m_functions.size();
    }
    // (What is inside a function that has code both for a call and for `new` is there twice.)
    void isAlso(uint32_t number, UnlinkedFunctionExecutable* executable) { m_numbers.add(executable, number); }

    // After that: any thread.
    uint32_t numberOf(UnlinkedFunctionExecutable* executable) const { return m_numbers.get(executable); } // Zero: it is not one of them.
    const KnownFunction* function(uint32_t number) const { return number && number <= m_functions.size() ? m_functions[number - 1].get() : nullptr; }
    unsigned size() const { return m_functions.size(); }

private:
    Vector<std::unique_ptr<KnownFunction>> m_functions;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t> m_numbers;
};
JS_EXPORT_PRIVATE void setFunctionsOfProgram(const FunctionsOfProgram*); // Not while anything is being compiled.
const FunctionsOfProgram* functionsOfProgram();

// With structs (TypeTable::tableHasTypedFields()): what the definitions of the program's classes come to. A class that the table of types says something of says so where it is defined
// (@noteClass), and there its constructor and its methods are in plain sight. Functions go by their numbers (FunctionsOfProgram).
class ClassesOfProgram {
    WTF_MAKE_TZONE_ALLOCATED(ClassesOfProgram);
    WTF_MAKE_NONCOPYABLE(ClassesOfProgram);
public:
    ClassesOfProgram() = default;

    // While every piece of code has its say (Graph::noteClassesDefined()): any thread.
    JS_EXPORT_PRIVATE void recordNonEscapingMethod(uint32_t classType, UniquedStringImpl* name, uint32_t function);
    JS_EXPORT_PRIVATE void noteThisIn(UnlinkedCodeBlock*, uint16_t layoutID);

    // After that: any thread.
    uint32_t closedMethod(uint32_t classType, UniquedStringImpl* name) const { return m_methods.get({ classType, name }); } // Zero: none.
    bool isNonEscapingMethod(uint32_t function) const { return function && m_nonEscapingMethods.contains(function); }
    uint16_t layoutIDOfThisIn(UnlinkedCodeBlock* code) const { return m_layoutIDOfThis.get(code); } // Zero: there is no telling.
    template<typename Functor> void forEachNonEscapingMethod(const Functor& functor) const
    {
        for (uint32_t function : m_nonEscapingMethods)
            functor(function);
    }
    unsigned numberOfNonEscapingMethods() const { return m_nonEscapingMethods.size(); }

private:
    Lock m_lock;
    UncheckedKeyHashMap<std::pair<uint32_t, UniquedStringImpl*>, uint32_t> m_methods;
    UncheckedKeyHashSet<uint32_t> m_nonEscapingMethods;
    UncheckedKeyHashMap<UnlinkedCodeBlock*, uint16_t> m_layoutIDOfThis;
};
JS_EXPORT_PRIVATE void setClassesOfProgram(ClassesOfProgram*); // Not while anything is being compiled.
ClassesOfProgram* classesOfProgram();

// The functions that return, every time, an object that a literal has just made and nothing else has seen, with properties of the same names in the same order. From the bytecode alone.
class MultiValueReturnTable {
    WTF_MAKE_TZONE_ALLOCATED(MultiValueReturnTable);
    WTF_MAKE_NONCOPYABLE(MultiValueReturnTable);
public:
    MultiValueReturnTable() = default;
    using Names = Vector<UniquedStringImpl*, 8>;

    // While every piece of code has its say (recordReturnedLiterals()): any thread.
    JS_EXPORT_PRIVATE void note(UnlinkedCodeBlock*, Names&&);

    // After that: any thread.
    const Names* returnValueNamesOf(UnlinkedCodeBlock* code) const
    {
        auto it = m_names.find(code);
        return it == m_names.end() ? nullptr : &it->value;
    }
    unsigned size() const { return m_names.size(); }

private:
    Lock m_lock;
    UncheckedKeyHashMap<UnlinkedCodeBlock*, Names> m_names;
};
JS_EXPORT_PRIVATE void setMultiValueReturnTable(MultiValueReturnTable*); // Not while anything is being compiled.
MultiValueReturnTable* multiValueReturnTable();
// If that is how it hands them back, as things stand.
JS_EXPORT_PRIVATE const MultiValueReturnTable::Names* registerReturnValuesOf(UnlinkedCodeBlock*, const FunctionSummary*);

class CalleeHints;
struct ModuleLinkage;
// What whoever compiles the program has for each piece of its code, for making one part of another.
class CodeOfProgram {
public:
    virtual ~CodeOfProgram() = default;
    struct About {
        const CalleeHints* hints { nullptr };
        const ModuleLinkage* linkage { nullptr };
        const FunctionSummary* facts { nullptr };
        ImageKey key;
    };
    virtual std::optional<About> about(UnlinkedCodeBlock*) const = 0; // Any thread.
    // The code for a call of one of the engine's own functions, by BuiltinCodeIndex. Null if there is none.
    virtual UnlinkedFunctionCodeBlock* codeOfBuiltin(unsigned) const = 0;
};

// Whoever calls a closed function knows what it is calling, and it is known what all of them pass and what comes back. So what is a number
// or a boolean every time goes as that: an int32 or a boolean in the register the parameter has anyway, a double in the floating point
// register of the same number. Likewise what is returned. Plain from the facts, so the function and whoever calls it agree.
struct HowValuesArePassed {
    std::array<Rep, numberOfArgumentGPRs> parameters;
    Rep result { Rep::JSValue };
    HowValuesArePassed() { parameters.fill(Rep::JSValue); }
};
HowValuesArePassed howValuesArePassed(const FunctionSummary*, Convention);
// Likewise the things that are returned in registers: the first in the first register that a parameter would have, and so on.
Vector<Rep, 8> returnValueReps(const FunctionSummary*, unsigned count);

// A constant that is whatever the realm has for it (SourceCodeRepresentation::LinkTimeConstant): the number of that among what cannot be
// changed (ImmutableIntrinsics), if it is one of those. Then code gets it from there, and has no use for the constant.
JS_EXPORT_PRIVATE std::optional<unsigned> intrinsicForLinkTimeConstant(JSValue constant);

// Whether code can tell which object it was called as, other than by way of the scope that has, if that is the environment of the
// module: which is where it is (ModuleLinkage::distanceOfEnvironment). From the bytecode alone, so that the function and whoever
// calls it come to the same answer.
bool needsFunctionObject(UnlinkedCodeBlock*);

// What a variable that is called holds, or probably holds. Only what is proven makes a difference to how it is called.
class CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(CalleeHints);
    WTF_MAKE_NONCOPYABLE(CalleeHints);
public:
    CalleeHints() = default;
    virtual ~CalleeHints();

    // scopeOffset: where the variable is in scopeOfVariables(), which whoever asks has seen to that it is a variable of. None: it was
    // not resolved, so it may be a global.
    virtual const KnownFunction* find(UniquedStringImpl* name, std::optional<unsigned> scopeOffset) const = 0;
    virtual const void* scopeOfVariables() const { return nullptr; } // As Variable::scope.
};

// The variables at the top of a module that a declaration gives a function or a class. Which those are is in the syntax tree
// (function declarations, FunctionAssignment). What becomes of them after that takes the whole program to know: which is what the
// bundler had in hand.
class ModuleHints final : public CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(ModuleHints);
public:
    // What there is to know about a function of the module. False: nothing.
    using Describe = Function<bool(UnlinkedFunctionExecutable*, KnownFunction&)>;
    // PrelinkedModuleGraph::Binding, of the variable that is at scopeOffset in the environment of the module.
    struct Binding {
        unsigned scopeOffset { 0 };
        bool keepsDeclaredValue { false };
        bool escapes { true };
        bool isVisibleFromOutside { true };
    };
    ModuleHints(UnlinkedCodeBlock* codeOfModule, std::span<const Binding>, const Describe&);
    ~ModuleHints() final;

    const KnownFunction* find(UniquedStringImpl*, std::optional<unsigned> scopeOffset) const final;
    const void* scopeOfVariables() const final;

    void recordFunctionAssignmentsIn(UnlinkedCodeBlock*, const Describe&); // The code of the module, and every function there is in it, however deep.
    void prove(); // After that.
    void noteEscape(unsigned scopeOffset); // After that.
    unsigned numberOfVariables() const { return m_variables.size(); }
    unsigned numberOfSingleFunctionVariables() const;
    template<typename Functor> void forEachSingleFunctionVariable(const Functor& functor) const
    {
        for (auto& entry : m_variables) {
            if (entry.value.function.isExact)
                functor(entry.value.function);
        }
    }

private:
    struct Variable {
        Binding binding;
        unsigned numberOfFunctions { 0 }; // That some declaration or statement gives it.
        bool isDescribed { false };
        KnownFunction function; // The first of them.
    };
    void add(unsigned scopeOffset, UnlinkedFunctionExecutable*, const Describe&);

    UnlinkedModuleProgramCodeBlock* m_module { nullptr };
    UncheckedKeyHashMap<unsigned, Variable, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_variables;
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
void recordFunctionAssignments(UnlinkedCodeBlock*, Vector<FunctionAssignment>&&);
Vector<FunctionAssignment> functionAssignmentsIn(UnlinkedCodeBlock*); // Any thread.

// EXPERIMENT: Options::aotFacts(). All are noted before any is asked for.
void noteBodyOfFact(uint32_t body, const KnownFunction&);
const KnownFunction* bodyOfFact(uint32_t body);
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

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

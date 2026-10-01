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

// Interprocedural summary of one function whose identity is known exactly (KnownFunction::isExact). Every function in the program
// contributes to it, from any thread. All fields are monotonic (they only widen), and the summary is valid once the fixpoint has
// been reached.
struct FunctionSummary {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(FunctionSummary);

    // The function is used as a value: stored, passed, compared, used with `new`, has a property read, or is called through
    // something other than a direct call. If false, all of its callers are known.
    std::atomic<bool> valueIsUsed { false };
    std::atomic<uint32_t> directCalls { 0 };
    // The function is likely to run many times: it is called in a loop, it is passed to a method that calls its argument for each
    // element of a collection, or a function of which that is true calls it. Besides loops, this is the only indication of which
    // code is hot that is available ahead of time. The inliner treats a call in such a function like a call in a loop.
    std::atomic<bool> isCalledRepeatedly { false };
    // The functions that this one calls directly outside its loops (Graph::recordUsesOfKnownFunctions()). (Two code blocks may share
    // a summary.)
    mutable Lock directCalleesLock;
    mutable Vector<FunctionSummary*> directCallees WTF_GUARDED_BY_LOCK(directCalleesLock);

    // Valid once valueIsUsed is final. Only direct calls can reach this function.
    bool isNonEscaping { false };
    // The function flowed somewhere the analysis does not track, so it may be called from anywhere with any arguments, and its
    // result may go anywhere. During the fixpoint isNonEscaping is set optimistically for every function. One that turns out to
    // escape has all its parameter types widened to top, which is equivalent to knowing nothing.
    mutable std::atomic<bool> escapes { false };
    uint32_t number { 0 }; // Index in FunctionsOfProgram.
    // The first reason it escaped, for logging.
    enum EscapeReason : uint32_t {
        DoesNotEscape, ReportedByBundler, CreationSiteUnknown, ReferencesItself, ExternalFunction, NotCallable, NumberDoesNotFitInTypes,
        UsedBy, // | (opcode, or 1000 + NodeKind) << 8
        PassedToUnknownCallee, PassedAsThis, PassedAsExtraArgument, CalledIndirectly, ReturnedToUnknownCaller,
        MergedInPhi, MergedInFrameRegister, MergedInVariable, MergedInParameter, MergedInReturn, LostThroughAlias,
        StoredInModuleVariable, StoredInUntrackedVariable, StoredToUnknownLocation, StoredInDynamicallyReadVariable, ReadInexactly,
    };
    mutable std::atomic<uint32_t> escapeReason { 0 };
    // Returns true the first time. Widens every parameter type to top and calls hadBeenPassed with each previous type: function
    // values that were tracked precisely through those parameters are no longer distinguishable, so they escape too.
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
    // For use before any argument types have been recorded.
    bool markEscaping(uint32_t why) { return markEscaping(why, [](Type) { }); }
    // If non-escaping: the union of all `this` values. (parameterTypes[0] tells whether the function is reached at all.)
    AtomicType thisType;
    bool isReached() const { return !isNonEscaping || escapes.load(std::memory_order_relaxed) || parameterTypes[0].load(); }
    // If non-escaping: the union of all arguments passed for each parameter, with `this` first. Starts at bottom (see
    // KnownFunction::returnType). Parameters beyond mostParameters are not tracked.
    static constexpr unsigned mostParameters = 12;
    std::array<AtomicType, mostParameters> parameterTypes { };
    // Same as KnownFunction::returnType, reachable from the function's own compilation.
    mutable AtomicType returnType;
    // A tail call hands the callee's result to this function's caller as it is, so the two have to return it in the same representation.
    // The functions this one is known to call in tail position, as of the last time its types were inferred.
    mutable Vector<const FunctionSummary*> knownTailCallees;
    // Set once the fixpoint is reached. The result is boxed whatever its type, because the function at the other end of a tail call
    // returns it boxed.
    mutable bool returnsBoxed { false };
    // Multi-value return. The function always returns a fresh object literal with the same property names (MultiValueReturnTable).
    // If it is non-escaping and every caller only reads those properties, the object is never allocated: each property value is
    // returned in a register.
    static constexpr unsigned maxReturnValues = 8;
    mutable std::array<AtomicType, maxReturnValues> returnValueTypes { }; // Starts at bottom.
    // Some caller uses the returned object for something other than reading those properties. Only goes from false to true.
    mutable std::atomic<bool> needsReturnObject { false };
    // returnValueTypes or needsReturnObject changed since the fixpoint driver last checked.
    mutable std::atomic<bool> returnValueTypesChanged { false };
    // Which arguments may still be reachable after the function returns: one bit per parameter, with `this` first. Derived from the
    // function's code and its callees, independent of callers. Starts at zero. Assumes that property reads and writes on the
    // arguments do not run user code.
    static constexpr unsigned maxTrackedEscapingParameters = 31;
    static constexpr uint32_t extraArgumentsEscape = 1u << 31;
    mutable std::atomic<uint32_t> escapingParameters { 0 };
};
// One summary per function, however many variables hold it.
using FunctionSummaryMap = UncheckedKeyHashMap<UnlinkedFunctionExecutable*, FunctionSummary*>;

// A variable stored in an environment record: a module variable, or a local captured by an inner function.
struct Variable {
    const void* scope { nullptr }; // DeclaredNamesLink::Frame::identity
    unsigned offset { 0 };
    // Used in place of an offset: the value every variable of the scope has when the environment is created.
    static constexpr unsigned initialValue = std::numeric_limits<unsigned>::max();
    explicit operator bool() const { return !!scope; }
};

// The union of all values ever stored in each variable, starting at bottom. Only program code can write these variables, and all of
// it is analyzed. A variable is untracked if some write cannot be attributed to a specific variable or something outside the
// program can write it.
class VariableSummaries {
    WTF_MAKE_TZONE_ALLOCATED(VariableSummaries);
    WTF_MAKE_NONCOPYABLE(VariableSummaries);
public:
    VariableSummaries() = default;

    // Setup phase, before any read() or join(). Any thread.
    void giveUpOnName(UniquedStringImpl*);
    void giveUpOnScope(const void*);
    // Some code reads a variable with this name but the analysis cannot tell which one. The reader treats the value as unknown, so
    // anything stored in a variable with this name escapes.
    void recordDynamicReadOfName(UniquedStringImpl*);
    bool isDynamicallyRead(UniquedStringImpl* name) const { return m_dynamicallyReadNames.contains(name); }
    void noteScopeOfModule(const void* scope) { m_scopesOfModules.add(scope); } // Single-threaded.
    bool isScopeOfModule(const void* scope) const { return m_scopesOfModules.contains(scope); }

    // Any thread. `reader` is reported by takeReadersOfWidenedVariables() if the variable's type later widens. Returns TAll for an
    // untracked variable.
    static constexpr unsigned nobody = std::numeric_limits<unsigned>::max();
    Type read(Variable, UniquedStringImpl* name, unsigned reader);
    Type join(Variable, Type); // Returns the previous type.

    // Not concurrently with read() or join().
    Vector<unsigned> takeReadersOfWidenedVariables();
    // A variable that is read holds at least its scope's initial value. One with no known value at all was attributed to the wrong
    // scope or is only read by unreachable code. Stops tracking those.
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

// A function whose code is available to the compiler while it compiles a call.
struct KnownFunction {
    UnlinkedFunctionExecutable* executable { nullptr };
    UnlinkedFunctionCodeBlock* forCall { nullptr }; // Either code block may be null.
    UnlinkedFunctionCodeBlock* forConstruct { nullptr };
    ImageKey key; // For the call code block. The construct key differs only in the kind bit.
    Convention conventionForCall; // conventionOf() the matching code block.
    Convention conventionForConstruct;
    // The variable this was found in holds a closure of this function from initialization onward and is never reassigned. The
    // bundler has seen every use of the variable and guarantees this (ModuleHints::prove()). A call through the variable then needs
    // no callee check, provided the variable is known to be initialized.
    bool isExact { false };
    bool isDeclaration { false }; // The variable is initialized before any module code runs.
    // The variable's value may be used for something other than a direct call. PrelinkedModuleGraph::Binding::Escapes.
    bool escapes { true };
    // The variable is reachable other than through reads in program code. PrelinkedModuleGraph::Binding::IsVisibleFromOutside.
    bool isVisibleFromOutside { true };
    // If exact: the call code block never uses its callee (needsFunctionObject()), so calls do not pass one. BytecodeLinkEncoder
    // clears this if there turns out to be no code to call.
    mutable std::atomic<bool> needsNoFunctionObject { false };
    // If exact: the union of everything a call can return. Computed for all functions together, starting at bottom
    // (inferReturnTypeForImage()). Valid once the fixpoint has been reached.
    mutable AtomicType returnType;
    mutable FunctionSummary* summary { nullptr }; // If exact, and the driver keeps summaries.

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
        summary = other.summary;
        return *this;
    }

    ImageKey keyFor(bool isConstruct) const
    {
        ImageKey result = key;
        result.kind |= isConstruct;
        return result;
    }
};

// Whether the code reads its callee for anything other than the scope.
bool readsCallee(UnlinkedCodeBlock*);
// Whether the code can obtain a reference to its own function object.
JS_EXPORT_PRIVATE bool mayReferenceItself(UnlinkedCodeBlock*);

// Every function in the program, indexed by the number that represents it in a Type (typeOfFunction()), if it is small enough to.
class FunctionsOfProgram {
    WTF_MAKE_TZONE_ALLOCATED(FunctionsOfProgram);
    WTF_MAKE_NONCOPYABLE(FunctionsOfProgram);
public:
    FunctionsOfProgram() = default;

    // Construction phase.
    uint32_t add(const KnownFunction& function)
    {
        m_functions.append(makeUniqueWithoutFastMallocCheck<KnownFunction>(function));
        return m_functions.size();
    }
    // An inner function of a function with both call and construct code blocks has two executables.
    void isAlso(uint32_t number, UnlinkedFunctionExecutable* executable) { m_numbers.add(executable, number); }

    // After construction. Any thread.
    uint32_t numberOf(UnlinkedFunctionExecutable* executable) const { return m_numbers.get(executable); } // Zero if not in the table.
    const KnownFunction* function(uint32_t number) const { return number && number <= m_functions.size() ? m_functions[number - 1].get() : nullptr; }
    unsigned size() const { return m_functions.size(); }

private:
    Vector<std::unique_ptr<KnownFunction>> m_functions;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t> m_numbers;
};
JS_EXPORT_PRIVATE void setFunctionsOfProgram(const FunctionsOfProgram*); // Not during compilation.
const FunctionsOfProgram* functionsOfProgram();

// With typed fields (TypeTable::tableHasTypedFields()): what the program's class definitions declare. A class that the type table
// describes is marked at its definition (@noteClass), where its constructor and methods are visible. Functions are identified by
// their FunctionsOfProgram number.
class ClassesOfProgram {
    WTF_MAKE_TZONE_ALLOCATED(ClassesOfProgram);
    WTF_MAKE_NONCOPYABLE(ClassesOfProgram);
public:
    ClassesOfProgram() = default;

    // Collection phase (Graph::noteClassesDefined()). Any thread.
    JS_EXPORT_PRIVATE void recordNonEscapingMethod(uint32_t classType, UniquedStringImpl* name, uint32_t function);
    JS_EXPORT_PRIVATE void noteThisIn(UnlinkedCodeBlock*, uint16_t layoutID);

    // After collection. Any thread.
    uint32_t closedMethod(uint32_t classType, UniquedStringImpl* name) const { return m_methods.get({ classType, name }); } // Zero if none.
    bool isNonEscapingMethod(uint32_t function) const { return function && m_nonEscapingMethods.contains(function); }
    uint16_t layoutIDOfThisIn(UnlinkedCodeBlock* code) const { return m_layoutIDOfThis.get(code); } // Zero if unknown.
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
JS_EXPORT_PRIVATE void setClassesOfProgram(ClassesOfProgram*); // Not during compilation.
ClassesOfProgram* classesOfProgram();

// Functions whose every return statement returns a fresh, otherwise unobserved object literal with the same property names in the
// same order. Determined from bytecode alone.
class MultiValueReturnTable {
    WTF_MAKE_TZONE_ALLOCATED(MultiValueReturnTable);
    WTF_MAKE_NONCOPYABLE(MultiValueReturnTable);
public:
    MultiValueReturnTable() = default;
    using Names = Vector<UniquedStringImpl*, 8>;

    // Collection phase (recordReturnedLiterals()). Any thread.
    JS_EXPORT_PRIVATE void note(UnlinkedCodeBlock*, Names&&);

    // After collection. Any thread.
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
JS_EXPORT_PRIVATE void setMultiValueReturnTable(MultiValueReturnTable*); // Not during compilation.
MultiValueReturnTable* multiValueReturnTable();
// Non-null if the function currently qualifies for multi-value return.
JS_EXPORT_PRIVATE const MultiValueReturnTable::Names* registerReturnValuesOf(UnlinkedCodeBlock*, const FunctionSummary*);

class CalleeHints;
class ModuleLinkage;
// Lets the inliner look up what the driver knows about another function's code.
class CodeOfProgram {
public:
    virtual ~CodeOfProgram() = default;
    struct About {
        const CalleeHints* hints { nullptr };
        const ModuleLinkage* linkage { nullptr };
        const FunctionSummary* summary { nullptr };
        ImageKey key;
    };
    virtual std::optional<About> about(UnlinkedCodeBlock*) const = 0; // Any thread.
    // The call code block of a built-in, by BuiltinCodeIndex. Null if there is none.
    virtual UnlinkedFunctionCodeBlock* codeOfBuiltin(unsigned) const = 0;
};

// Unboxed calling convention. All callers of a non-escaping function are known, as are the types they pass and the type returned. A
// parameter that is always an int32 or a boolean is passed unboxed in its usual register; one that is always a double is passed in
// the FPR with the same index. The same applies to the result. Derived from the summary alone, so caller and callee agree.
struct ValueRepresentations {
    std::array<Rep, numberOfArgumentGPRs> parameters;
    Rep result { Rep::JSValue };
    ValueRepresentations() { parameters.fill(Rep::JSValue); }
};
ValueRepresentations valueRepresentations(const FunctionSummary*, Convention);
// The same for multi-value returns. The first value uses the first argument register, and so on.
Vector<Rep, 8> returnValueReps(const FunctionSummary*, unsigned count);

// If a link-time constant (SourceCodeRepresentation::LinkTimeConstant) is one of the ImmutableIntrinsics, returns its index there.
// Code then loads it from the Instance instead of the constant pool.
JS_EXPORT_PRIVATE std::optional<unsigned> intrinsicForLinkTimeConstant(JSValue constant);

// Whether the code needs its callee, other than to reach the scope when the scope is the module environment (which is at a known
// location, ModuleLinkage::distanceOfEnvironment). Determined from bytecode alone, so caller and callee agree.
bool needsFunctionObject(UnlinkedCodeBlock*);

// What function a called variable holds, or probably holds. Only exact entries change how the call is compiled.
class CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(CalleeHints);
    WTF_MAKE_NONCOPYABLE(CalleeHints);
public:
    CalleeHints() = default;
    virtual ~CalleeHints();

    // scopeOffset: the variable's offset in scopeOfVariables(); the caller has verified that it belongs to that scope. Nullopt if
    // the variable was not resolved and may be a global.
    virtual const KnownFunction* find(UniquedStringImpl* name, std::optional<unsigned> scopeOffset) const = 0;
    virtual const void* scopeOfVariables() const { return nullptr; } // As Variable::scope.
};

// The top-level variables of a module that are initialized with a function or a class. The syntax tree says which those are
// (function declarations, FunctionAssignment). Whether they are ever reassigned or escape requires whole-program knowledge, which
// comes from the bundler.
class ModuleHints final : public CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(ModuleHints);
public:
    // Fills in the KnownFunction for one of the module's functions. Returns false if nothing is known.
    using Describe = Function<bool(UnlinkedFunctionExecutable*, KnownFunction&)>;
    // PrelinkedModuleGraph::Binding for the variable at scopeOffset in the module environment.
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

    void recordFunctionAssignmentsIn(UnlinkedCodeBlock*, const Describe&); // Call for the module code and for every function nested in it.
    void prove(); // Call after recordFunctionAssignmentsIn().
    void noteEscape(unsigned scopeOffset); // Call after prove().
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
        unsigned numberOfFunctions { 0 }; // Number of declarations or assignments that store a function in it.
        bool isDescribed { false };
        KnownFunction function; // The first function stored.
    };
    void add(unsigned scopeOffset, UnlinkedFunctionExecutable*, const Describe&);

    UnlinkedModuleProgramCodeBlock* m_module { nullptr };
    UncheckedKeyHashMap<unsigned, Variable, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_variables;
};

// An import that was resolved at link time to a variable of another module.
struct StaticImport {
    unsigned slot { 0 }; // JSModuleEnvironment::importSlot() in the importing module's environment.
    unsigned scopeOffsetOfSlot { 0 }; // JSModuleEnvironment::importSlotScopeOffset() for that slot.
    unsigned scopeOffset { 0 }; // Offset of the variable in the exporting module's environment.
    const KnownFunction* function { nullptr }; // The function the exporting module stores there, if any (ModuleHints).
    uint32_t distanceOfEnvironment { 0 }; // ImageEnvironment::distance of the exporting module.
    const void* scope { nullptr }; // Variable::scope of the exporting module's environment.
};

// Properties of a module that hold because the whole program is linked at compile time. Unlike hints, compiled code relies on these
// without checks. It only runs for a module whose record was verified to be linked the same way
// (JSModuleRecord::isLinkedAsInImage()).
class ModuleLinkage {
    WTF_MAKE_TZONE_ALLOCATED(ModuleLinkage);
    WTF_MAKE_NONCOPYABLE(ModuleLinkage);
public:
    ModuleLinkage() = default;

    uint32_t distanceOfEnvironment { 0 }; // ImageEnvironment::distance of this module.

    void addImport(UniquedStringImpl* localName, StaticImport import) { m_imports.add(localName, import); }
    const StaticImport* findImport(UniquedStringImpl* localName) const
    {
        auto it = m_imports.find(localName);
        return it == m_imports.end() ? nullptr : &it->value;
    }

private:
    UncheckedKeyHashMap<UniquedStringImpl*, StaticImport> m_imports;
};

// With Options::resolveAllScopeSlotsStatically(): the names declared in the scopes enclosing a function, kept from bytecode
// generation until compilation. For module code: as seen by the module's own functions.
void noteDeclaredNames(UnlinkedCodeBlock*, RefPtr<DeclaredNamesLink>&&);
void recordFunctionAssignments(UnlinkedCodeBlock*, Vector<FunctionAssignment>&&);
Vector<FunctionAssignment> functionAssignmentsIn(UnlinkedCodeBlock*); // Any thread.
const DeclaredNamesLink* declaredNamesFor(UnlinkedCodeBlock*); // Any thread. Valid until forgetDeclaredNames().
void forgetDeclaredNames();

// Bytecode refers to an identifier by index. Normally that is an index into the function's own identifier table. If the whole
// program's identifiers have been numbered, all functions share one table. The shared table is built by StaticHeap, so such code
// can only run with a static heap.
using NumbersOfIdentifiers = UncheckedKeyHashMap<UniquedStringImpl*, uint32_t>;
JS_EXPORT_PRIVATE void setNumbersOfIdentifiersOfProgram(const NumbersOfIdentifiers*); // Not during compilation.
const NumbersOfIdentifiers* numbersOfIdentifiersOfProgram();

// The same for constants of functions whose constants are realm-independent: one deduplicated table for the program. Maps each such
// function to the table index of each of its constants, or notAConstantOfProgram for an empty constant.
static constexpr uint32_t notAConstantOfProgram = std::numeric_limits<uint32_t>::max();
using NumbersOfConstants = UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<uint32_t>>;
JS_EXPORT_PRIVATE void setNumbersOfConstantsOfProgram(const NumbersOfConstants*); // Not during compilation.
const Vector<uint32_t>* numbersOfConstantsOfProgramFor(UnlinkedCodeBlock*); // Null if the function uses its own constant pool.

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

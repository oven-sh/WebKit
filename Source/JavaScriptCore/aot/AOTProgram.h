/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubs.h"
#include "AOTType.h"
#include "DeclaredNamesLink.h"
#include <span>
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

struct FunctionSummary {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(FunctionSummary);

    std::atomic<bool> valueIsUsed { false };
    std::atomic<uint32_t> directCalls { 0 };
    std::atomic<bool> isCalledRepeatedly { false };
    mutable Lock directCalleesLock;
    mutable Vector<FunctionSummary*> directCallees WTF_GUARDED_BY_LOCK(directCalleesLock);

    bool isNonEscaping { false };
    mutable std::atomic<bool> escapes { false };
    uint32_t number { 0 };
    enum EscapeReason : uint32_t {
        DoesNotEscape, ReportedByBundler, CreationSiteUnknown, ReferencesItself, ExternalFunction, NotCallable, FunctionNumberOverflow,
        UsedBy,
        PassedToUnknownCallee, PassedAsThis, PassedAsExtraArgument, CalledIndirectly, ReturnedToUnknownCaller,
        MergedInPhi, MergedInFrameRegister, MergedInVariable, MergedInParameter, MergedInReturn, LostThroughAlias,
        StoredInModuleVariable, StoredInUntrackedVariable, StoredToUnknownLocation, StoredInDynamicallyReadVariable, ReadInexactly,
    };
    mutable std::atomic<uint32_t> escapeReason { 0 };
    template<typename Functor>
    bool markEscaping(uint32_t why, const Functor& wasPassed)
    {
        if (escapes.exchange(true, std::memory_order_relaxed))
            return false;
        escapeReason.store(why, std::memory_order_relaxed);
        for (auto& type : parameterTypes)
            wasPassed(type.join(TTop));
        wasPassed(thisType.join(TTop));
        return true;
    }
    bool markEscaping(uint32_t why) { return markEscaping(why, [](Type) { }); }
    AtomicType thisType;
    bool isReached() const { return !isNonEscaping || escapes.load(std::memory_order_relaxed) || parameterTypes[0].load(); }
    static constexpr unsigned maxParameters = 12;
    std::array<AtomicType, maxParameters> parameterTypes { };
    mutable AtomicType returnType;
    mutable Vector<const FunctionSummary*> knownTailCallees;
    mutable bool returnsBoxed { false };
    static constexpr unsigned maxReturnValues = 8;
    mutable std::array<AtomicType, maxReturnValues> returnValueTypes { };
    mutable std::atomic<bool> needsReturnObject { false };
    mutable std::atomic<bool> returnValueTypesChanged { false };
    static constexpr unsigned maxTrackedEscapingParameters = 31;
    static constexpr uint32_t extraArgumentsEscape = 1u << 31;
    mutable std::atomic<uint32_t> escapingParameters { 0 };
    struct PropertyEffects {
        static constexpr unsigned maxStoredNames = 24;
        bool isArbitrary { true };
        Vector<UniquedStringImpl*, 2> storedNames;
        Vector<const FunctionSummary*, 2> callees;
    };
    mutable PropertyEffects propertyEffects;
};
JS_EXPORT_PRIVATE void closePropertyEffects(std::span<const std::unique_ptr<FunctionSummary>>);
using FunctionSummaryMap = UncheckedKeyHashMap<UnlinkedFunctionExecutable*, FunctionSummary*>;

struct Variable {
    const void* scope { nullptr };
    unsigned offset { 0 };
    static constexpr unsigned initialValue = std::numeric_limits<unsigned>::max();
    explicit operator bool() const { return !!scope; }
};

class VariableSummaries {
    WTF_MAKE_TZONE_ALLOCATED(VariableSummaries);
    WTF_MAKE_NONCOPYABLE(VariableSummaries);
public:
    VariableSummaries() = default;

    void giveUpOnName(UniquedStringImpl*);
    void giveUpOnScope(const void*);
    void giveUpOnAllScopes() { m_hasGivenUpOnAllScopes.store(true, std::memory_order_relaxed); }
    void recordDynamicNameRead(UniquedStringImpl*);
    bool isDynamicallyRead(UniquedStringImpl* name) const { return m_dynamicallyReadNames.contains(name); }
    void noteModuleScope(const void* scope) { m_moduleScopes.add(scope); }
    bool isModuleScope(const void* scope) const { return m_moduleScopes.contains(scope); }

    static constexpr unsigned nobody = std::numeric_limits<unsigned>::max();
    Type read(Variable, UniquedStringImpl* name, unsigned reader);
    Type join(Variable, Type);

    Vector<unsigned> takeWidenedVariableReaders();
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
    using ReaderSet = UncheckedKeyHashSet<unsigned, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>>;
    struct Cell {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(Cell);
        AtomicType type;
        bool grew { false };
        ReaderSet readers;
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
    std::atomic<bool> m_hasGivenUpOnAllScopes { false };
    UncheckedKeyHashSet<const void*> m_moduleScopes;
};

struct KnownFunction {
    UnlinkedFunctionExecutable* executable { nullptr };
    UnlinkedFunctionCodeBlock* forCall { nullptr };
    UnlinkedFunctionCodeBlock* forConstruct { nullptr };
    ImageKey key;
    Convention conventionForCall;
    Convention conventionForConstruct;
    bool isExact { false };
    bool isDeclaration { false };
    bool escapes { true };
    bool isExternallyVisible { true };
    mutable std::atomic<bool> needsNoFunctionObject { false };
    mutable AtomicType returnType;
    mutable FunctionSummary* summary { nullptr };

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
        isExternallyVisible = other.isExternallyVisible;
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

bool readsCallee(UnlinkedCodeBlock*);
JS_EXPORT_PRIVATE bool mayReferenceItself(UnlinkedCodeBlock*);

class ProgramFunctions {
    WTF_MAKE_TZONE_ALLOCATED(ProgramFunctions);
    WTF_MAKE_NONCOPYABLE(ProgramFunctions);
public:
    ProgramFunctions() = default;

    uint32_t add(const KnownFunction& function)
    {
        m_functions.append(makeUniqueWithoutFastMallocCheck<KnownFunction>(function));
        return m_functions.size();
    }
    void addAlias(uint32_t number, UnlinkedFunctionExecutable* executable) { m_numbers.add(executable, number); }

    uint32_t numberOf(UnlinkedFunctionExecutable* executable) const { return m_numbers.get(executable); }
    const KnownFunction* function(uint32_t number) const { return number && number <= m_functions.size() ? m_functions[number - 1].get() : nullptr; }
    unsigned size() const { return m_functions.size(); }

private:
    Vector<std::unique_ptr<KnownFunction>> m_functions;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t> m_numbers;
};
JS_EXPORT_PRIVATE void setProgramFunctions(const ProgramFunctions*);
const ProgramFunctions* programFunctions();

class ProgramClasses {
    WTF_MAKE_TZONE_ALLOCATED(ProgramClasses);
    WTF_MAKE_NONCOPYABLE(ProgramClasses);
public:
    ProgramClasses() = default;

    JS_EXPORT_PRIVATE void recordNonEscapingMethod(uint32_t classType, UniquedStringImpl* name, uint32_t function);
    JS_EXPORT_PRIVATE void noteThisIn(UnlinkedCodeBlock*, uint16_t layoutID);

    uint32_t closedMethod(uint32_t classType, UniquedStringImpl* name) const { return m_methods.get({ classType, name }); }
    bool isNonEscapingMethod(uint32_t function) const { return function && m_nonEscapingMethods.contains(function); }
    uint16_t thisLayoutIDIn(UnlinkedCodeBlock* code) const { return m_thisLayoutID.get(code); }
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
    UncheckedKeyHashMap<UnlinkedCodeBlock*, uint16_t> m_thisLayoutID;
};
JS_EXPORT_PRIVATE void setProgramClasses(ProgramClasses*);
ProgramClasses* programClasses();

class MultiValueReturnTable {
    WTF_MAKE_TZONE_ALLOCATED(MultiValueReturnTable);
    WTF_MAKE_NONCOPYABLE(MultiValueReturnTable);
public:
    MultiValueReturnTable() = default;
    using Names = Vector<UniquedStringImpl*, 8>;

    JS_EXPORT_PRIVATE void note(UnlinkedCodeBlock*, Names&&);

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
JS_EXPORT_PRIVATE void setMultiValueReturnTable(MultiValueReturnTable*);
MultiValueReturnTable* multiValueReturnTable();
JS_EXPORT_PRIVATE const MultiValueReturnTable::Names* registerReturnValuesOf(UnlinkedCodeBlock*, const FunctionSummary*);

class CalleeHints;
class ModuleLinkage;

struct CoveredOperation {
    static constexpr uint8_t isInGenericCopy = 1;
    static constexpr uint8_t isRarelyExecuted = 2;
    static constexpr uint8_t isInLoop = 4;
    static constexpr uint8_t isInlined = 8;
    static constexpr uint8_t isElided = 16;
    static constexpr uint8_t hasTypeTag = 32;
    static constexpr uint8_t isNotInGraph = 64;
    static constexpr uint32_t noReason = UINT32_MAX;
    static constexpr uint32_t noCounter = UINT32_MAX;

    UnlinkedCodeBlock* codeBlock { nullptr };
    ImageKey function;
    uint32_t bytecodeOffset { 0 };
    uint32_t divot { 0 };
    uint32_t reason { noReason };
    uint32_t counter { noCounter };
    uint16_t opcode { 0 };
    uint8_t flags { 0 };
    String property;
    Vector<String, 2> outcomes;
};

class ProgramCode {
public:
    virtual ~ProgramCode() = default;
    struct About {
        const CalleeHints* hints { nullptr };
        const ModuleLinkage* linkage { nullptr };
        const FunctionSummary* summary { nullptr };
        ImageKey key;
        uint32_t firstTypeCoverageCounter { 0 };
    };
    virtual std::optional<About> about(UnlinkedCodeBlock*) const = 0;
    virtual UnlinkedFunctionCodeBlock* codeForBuiltin(unsigned) const = 0;
};

struct ValueRepresentations {
    std::array<Rep, numberOfArgumentGPRs> parameters;
    Rep result { Rep::JSValue };
    ValueRepresentations() { parameters.fill(Rep::JSValue); }
};
ValueRepresentations valueRepresentations(const FunctionSummary*, Convention);
Vector<Rep, 8> returnValueReps(const FunctionSummary*, unsigned count);

JS_EXPORT_PRIVATE std::optional<unsigned> intrinsicForLinkTimeConstant(JSValue constant);

bool needsFunctionObject(UnlinkedCodeBlock*);

class CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(CalleeHints);
    WTF_MAKE_NONCOPYABLE(CalleeHints);
public:
    CalleeHints() = default;
    virtual ~CalleeHints();

    virtual const KnownFunction* find(UniquedStringImpl* name, std::optional<unsigned> scopeOffset) const = 0;
    virtual const void* variableScope() const { return nullptr; }
};

class ModuleHints final : public CalleeHints {
    WTF_MAKE_TZONE_ALLOCATED(ModuleHints);
public:
    using Describe = Function<bool(UnlinkedFunctionExecutable*, KnownFunction&)>;
    struct Binding {
        unsigned scopeOffset { 0 };
        bool keepsDeclaredValue { false };
        bool escapes { true };
        bool isExternallyVisible { true };
    };
    ModuleHints(UnlinkedCodeBlock* moduleCode, std::span<const Binding>, const Describe&);
    ~ModuleHints() final;

    const KnownFunction* find(UniquedStringImpl*, std::optional<unsigned> scopeOffset) const final;
    const void* variableScope() const final;

    void recordFunctionAssignmentsIn(UnlinkedCodeBlock*, const Describe&);
    void prove();
    void noteEscape(unsigned scopeOffset);
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
        unsigned numberOfFunctions { 0 };
        bool isDescribed { false };
        KnownFunction function;
    };
    void add(unsigned scopeOffset, UnlinkedFunctionExecutable*, const Describe&);

    UnlinkedModuleProgramCodeBlock* m_module { nullptr };
    UncheckedKeyHashMap<unsigned, Variable, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_variables;
};

struct StaticImport {
    unsigned slot { 0 };
    unsigned slotScopeOffset { 0 };
    unsigned scopeOffset { 0 };
    const KnownFunction* function { nullptr };
    uint32_t environmentDepth { 0 };
    const void* scope { nullptr };
};

class ModuleLinkage {
    WTF_MAKE_TZONE_ALLOCATED(ModuleLinkage);
    WTF_MAKE_NONCOPYABLE(ModuleLinkage);
public:
    ModuleLinkage() = default;

    uint32_t environmentDepth { 0 };

    void addImport(UniquedStringImpl* localName, StaticImport import) { m_imports.add(localName, import); }
    const StaticImport* findImport(UniquedStringImpl* localName) const
    {
        auto it = m_imports.find(localName);
        return it == m_imports.end() ? nullptr : &it->value;
    }

private:
    UncheckedKeyHashMap<UniquedStringImpl*, StaticImport> m_imports;
};

void noteDeclaredNames(UnlinkedCodeBlock*, RefPtr<DeclaredNamesLink>&&);
void recordFunctionAssignments(UnlinkedCodeBlock*, Vector<FunctionAssignment>&&);
Vector<FunctionAssignment> functionAssignmentsIn(UnlinkedCodeBlock*);
const DeclaredNamesLink* declaredNamesFor(UnlinkedCodeBlock*);
void forgetDeclaredNames();

using IdentifierIndices = UncheckedKeyHashMap<UniquedStringImpl*, uint32_t>;
JS_EXPORT_PRIVATE void setProgramIdentifierIndices(const IdentifierIndices*);
const IdentifierIndices* programIdentifierIndices();

static constexpr uint32_t invalidConstantIndex = std::numeric_limits<uint32_t>::max();
using ConstantIndices = UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<uint32_t>>;
JS_EXPORT_PRIVATE void setProgramConstantIndices(const ConstantIndices*);
const Vector<uint32_t>* programConstantIndicesFor(UnlinkedCodeBlock*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

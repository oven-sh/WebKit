/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgram.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTType.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "LinkTimeConstant.h"
#include "PreciseJumpTargets.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VariableSummaries);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(VariableSummaries::Cell);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(FunctionSummary);
WTF_MAKE_TZONE_ALLOCATED_IMPL(CalleeHints);
WTF_MAKE_TZONE_ALLOCATED_IMPL(ModuleHints);
WTF_MAKE_TZONE_ALLOCATED_IMPL(ModuleLinkage);

static Lock s_declaredNamesLock;
static UncheckedKeyHashMap<UnlinkedCodeBlock*, RefPtr<DeclaredNamesLink>>& declaredNames() WTF_REQUIRES_LOCK(s_declaredNamesLock)
{
    static NeverDestroyed<UncheckedKeyHashMap<UnlinkedCodeBlock*, RefPtr<DeclaredNamesLink>>> map;
    return map;
}

void noteDeclaredNames(UnlinkedCodeBlock* codeBlock, RefPtr<DeclaredNamesLink>&& names)
{
    if (!names)
        return;
    Locker locker { s_declaredNamesLock };
    declaredNames().set(codeBlock, WTF::move(names));
}

const DeclaredNamesLink* declaredNamesFor(UnlinkedCodeBlock* codeBlock)
{
    Locker locker { s_declaredNamesLock };
    auto it = declaredNames().find(codeBlock);
    return it == declaredNames().end() ? nullptr : it->value.get();
}

static UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<FunctionAssignment>>& functionAssignments() WTF_REQUIRES_LOCK(s_declaredNamesLock)
{
    static NeverDestroyed<UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<FunctionAssignment>>> map;
    return map;
}

void recordFunctionAssignments(UnlinkedCodeBlock* codeBlock, Vector<FunctionAssignment>&& functions)
{
    Locker locker { s_declaredNamesLock };
    functionAssignments().set(codeBlock, WTF::move(functions));
}

Vector<FunctionAssignment> functionAssignmentsIn(UnlinkedCodeBlock* codeBlock)
{
    Locker locker { s_declaredNamesLock };
    return functionAssignments().get(codeBlock);
}

static const IdentifierIndices* s_programIdentifierIndices;

void setProgramIdentifierIndices(const IdentifierIndices* numbers)
{
    s_programIdentifierIndices = numbers;
}

const IdentifierIndices* programIdentifierIndices()
{
    return s_programIdentifierIndices;
}

static const ConstantIndices* s_programConstantIndices;

void setProgramConstantIndices(const ConstantIndices* numbers)
{
    s_programConstantIndices = numbers;
}

const Vector<uint32_t>* programConstantIndicesFor(UnlinkedCodeBlock* codeBlock)
{
    if (!s_programConstantIndices)
        return nullptr;
    auto it = s_programConstantIndices->find(codeBlock);
    return it == s_programConstantIndices->end() ? nullptr : &it->value;
}

void forgetDeclaredNames()
{
    Locker locker { s_declaredNamesLock };
    declaredNames().clear();
    functionAssignments().clear();
}

void VariableSummaries::giveUpOnName(UniquedStringImpl* name)
{
    Locker locker { m_givenUpLock };
    m_untrackedNames.add(name);
}

void VariableSummaries::recordDynamicNameRead(UniquedStringImpl* name)
{
    Locker locker { m_givenUpLock };
    m_dynamicallyReadNames.add(name);
}

void VariableSummaries::giveUpOnScope(const void* scope)
{
    Locker locker { m_givenUpLock };
    m_untrackedScopes.add(scope);
}

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(VariableSummaries::ObjectLiteral);

void VariableSummaries::noteObjectsInVariables(StoresToVariables&& stores, const EscapingVariables& escaping, const NamesReadOfVariables& namesRead, std::span<const Variable> needingObject, std::span<UniquedStringImpl* const> lookedUpFromUnknownScopes)
{
    Locker locker { m_objectsInVariablesLock };
    for (auto& [variable, literal] : stores) {
        auto& entry = m_objectsInVariables.add({ variable.scope, variable.offset }, ObjectInVariable { }).iterator->value;
        entry.numberOfStores++;
        entry.literal = WTF::move(literal);
    }
    for (auto [variable, how] : escaping)
        m_objectsInVariables.add({ variable.scope, variable.offset }, ObjectInVariable { }).iterator->value.howItEscapes = how;
    for (auto [variable, name] : namesRead)
        m_objectsInVariables.add({ variable.scope, variable.offset }, ObjectInVariable { }).iterator->value.namesRead.add(name);
    for (Variable variable : needingObject)
        m_objectsInVariables.add({ variable.scope, variable.offset }, ObjectInVariable { }).iterator->value.needsObject = true;
    for (UniquedStringImpl* name : lookedUpFromUnknownScopes)
        m_namesLookedUpFromUnknownScopes.add(name);
}

void VariableSummaries::noteObjectsEscapeIn(const void* scope, ASCIILiteral how)
{
    Locker locker { m_objectsInVariablesLock };
    for (auto& entry : m_objectsInVariables) {
        if (entry.key.first == scope)
            entry.value.howItEscapes = how;
    }
}

void VariableSummaries::noteScopes(const FunctionSummary* maker, std::span<const void* const> made, std::span<const std::pair<const void*, WhyMade>> mustExist, std::span<const Variable> readFromInside, Vector<ClosureMade>&& closures)
{
    Locker locker { m_scopesLock };
    for (const void* scope : made) {
        if (!m_makersOfScopes.add(scope, maker).isNewEntry || !maker)
            m_scopesThatMustExist.add(scope, maker ? WhyMade::MadeTwice : WhyMade::MakerCannotPromote);
    }
    for (auto [scope, why] : mustExist) {
        m_scopesThatMustExist.add(scope, why);
        if (why == WhyMade::UnknownAccess || why == WhyMade::Evaluated)
            m_scopesSearchedByName.add(scope);
    }
    for (Variable variable : readFromInside) {
        auto& readers = m_readersFromInside.add({ variable.scope, variable.offset }, Vector<const FunctionSummary*, 2> { }).iterator->value;
        if (!readers.contains(maker))
            readers.append(maker);
    }
    m_closuresMade.appendVector(WTF::move(closures));
}

unsigned VariableSummaries::dissolveScopes(unsigned& closuresWithCaptures)
{
    constexpr unsigned maximumNumberOfCaptures = 8;
    UncheckedKeyHashMap<const FunctionSummary*, const ClosureMade*> whereMade;
    UncheckedKeyHashSet<const FunctionSummary*> madeInSeveralPlaces;
    for (auto& closure : m_closuresMade) {
        if (!whereMade.add(closure.made, &closure).isNewEntry)
            madeInSeveralPlaces.add(closure.made);
    }
    UncheckedKeyHashSet<const void*> candidates;
    std::array<unsigned, static_cast<size_t>(WhyMade::Count)> tally { };
    auto count = [&](WhyMade why) { ++tally[static_cast<size_t>(why)]; };
    for (auto& [scope, maker] : m_makersOfScopes) {
        if (auto it = m_scopesThatMustExist.find(scope); it != m_scopesThatMustExist.end())
            count(it->value);
        else if (!maker)
            count(WhyMade::MakerCannotPromote);
        else if (m_untrackedScopes.contains(scope) || m_hasGivenUpOnAllScopes.load(std::memory_order_relaxed))
            count(WhyMade::Untracked);
        else
            candidates.add(scope);
    }
    using Captured = std::pair<const void*, unsigned>;
    UncheckedKeyHashMap<const FunctionSummary*, Vector<Captured, 4>> held;
    for (bool changed = true; changed;) {
        changed = false;
        held.clear();
        for (auto& [variable, readers] : m_readersFromInside) {
            if (!candidates.contains(variable.first))
                continue;
            const FunctionSummary* makerOfScope = m_makersOfScopes.get(variable.first);
            bool works = variable.second < 64;
            WhyMade why = WhyMade::TooLarge;
            auto require = [&](bool condition, WhyMade otherwise) {
                if (works && !condition) {
                    works = false;
                    why = otherwise;
                }
            };
            for (const FunctionSummary* reader : readers) {
                for (const FunctionSummary* holder = reader; works; ) {
                    auto it = holder ? whereMade.find(holder) : whereMade.end();
                    require(holder, WhyMade::ReadByUnknownCode);
                    require(holder && holder->canHoldCaptures, WhyMade::ReaderCannotHold);
                    require(it != whereMade.end(), WhyMade::ReaderNotMade);
                    require(!madeInSeveralPlaces.contains(holder), WhyMade::ReaderMadeTwice);
                    if (!works)
                        break;
                    auto& captures = held.add(holder, Vector<Captured, 4> { }).iterator->value;
                    if (!captures.contains(variable))
                        captures.append(variable);
                    require(captures.size() <= maximumNumberOfCaptures, WhyMade::TooManyCaptures);
                    if (it->value->maker != makerOfScope) {
                        holder = it->value->maker;
                        continue;
                    }
                    bool isInChain = false;
                    for (auto& [scope, storedLater] : it->value->scopesAndWhatIsStoredLater) {
                        if (scope == variable.first) {
                            isInChain = true;
                            require(!(storedLater & (1ull << variable.second)), WhyMade::StoredLater);
                        }
                    }
                    require(isInChain, WhyMade::NotInChain);
                    break;
                }
                if (!works)
                    break;
            }
            if (!works) {
                count(why);
                candidates.remove(variable.first);
                changed = true;
            }
        }
    }
    m_dissolvedScopes = WTF::move(candidates);
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        static constexpr ASCIILiteral names[] = { "made twice in one function (split loop)"_s, "its function cannot promote (catch, generator, async, top level)"_s, "more than 32 variables"_s, "an access that is not resolved"_s, "the scope is used in another way"_s, "written by an inner function"_s, "read by code without a summary (construct code)"_s, "a closure without a summary is made in it"_s, "eval, with, arguments"_s, "a reader of a kind that holds no captures"_s, "a reader that is made in two places"_s, "a reader whose creation was not seen"_s, "stored to after the closure is made"_s, "not in the scope chain of the closure"_s, "more than 8 captures"_s, "untracked"_s, "left out by the bisection"_s, "made by top-level code"_s, "made by code without a summary (construct code, class constructors)"_s, "made by the body of an async function or a generator"_s, "made by the wrapper of an async function or a generator"_s, "made by a function with try/catch or registers in its frame"_s, "made by a function of another kind"_s };
        dataLogLn("AOT: of ", m_makersOfScopes.size(), " scopes, ", m_dissolvedScopes.size(), " are never made. The others:");
        for (size_t i = 0; i < tally.size(); ++i) {
            if (tally[i])
                dataLogLn("AOT:   ", tally[i], " ", names[i]);
        }
    }
    if (Options::logAOTTypeInference()) [[unlikely]] {
        for (const void* scope : m_dissolvedScopes)
            dataLogLn("AOT inference: scope ", RawPointer(scope), " is never made");
    }
    for (const void* scope : m_dissolvedScopes)
        m_makersOfScopes.get(scope)->makesDissolvedScopes = true;
    closuresWithCaptures = 0;
    for (auto& [holder, captures] : held) {
        for (auto& variable : captures) {
            if (m_dissolvedScopes.contains(variable.first))
                holder->captures.append(variable);
        }
        std::ranges::sort(holder->captures);
        closuresWithCaptures += !holder->captures.isEmpty();
    }
    return m_dissolvedScopes.size();
}

unsigned VariableSummaries::finishFindingConstantObjects(unsigned& neverAllocated)
{
    m_objectsInVariables.removeIf([&](auto& entry) {
        auto& object = entry.value;
        if (!object.literal)
            return true;
        ASCIILiteral why = object.howItEscapes;
        if (object.numberOfStores != 1)
            why = "is-assigned-more-than-once"_s;
        else if (m_hasGivenUpOnAllScopes.load(std::memory_order_relaxed) || m_untrackedScopes.contains(entry.key.first) || m_namesLookedUpFromUnknownScopes.contains(object.literal->variableName))
            why = "may-be-looked-up-by-name"_s;
        if (!why.isNull() && Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: the object literal in ", StringView(object.literal->variableName), " (", object.literal->names.size(), " properties, ", object.namesRead.size(), " read by name) is not constant: ", why);
        return !why.isNull();
    });
    neverAllocated = 0;
    for (auto& object : m_objectsInVariables.values()) {
        object.literal->isNeverAllocated = !object.needsObject && std::ranges::all_of(object.namesRead, [&](UniquedStringImpl* name) {
            size_t index = object.literal->names.find(name);
            return index == notFound ? isAbsentFromObjectPrototype(name) : object.literal->values[index].isKnown();
        });
        neverAllocated += object.literal->isNeverAllocated;
    }
    return m_objectsInVariables.size();
}

bool VariableSummaries::isUntracked(Variable variable, UniquedStringImpl* name) const
{
    return m_hasGivenUpOnAllScopes.load(std::memory_order_relaxed) || m_untrackedScopes.contains(variable.scope) || m_untrackedNames.contains(name);
}

void VariableSummaries::noteInitialValueIsNeverRead(const void* scope)
{
    Locker locker { m_scopesWhoseInitialValueIsNeverReadLock };
    m_scopesWhoseInitialValueIsNeverRead.add(scope);
}

Type VariableSummaries::read(Variable variable, UniquedStringImpl* name, unsigned reader)
{
    if (isUntracked(variable, name))
        return TAll;
    bool initialValueIsNeverRead;
    {
        Locker locker { m_scopesWhoseInitialValueIsNeverReadLock };
        initialValueIsNeverRead = m_scopesWhoseInitialValueIsNeverRead.contains(variable.scope);
    }
    Type type = 0;
    for (unsigned offset : { variable.offset, Variable::initialValue }) {
        if (offset == Variable::initialValue && initialValueIsNeverRead)
            continue;
        Variable which { variable.scope, offset };
        Shard& shard = shardFor(which);
        Locker locker { shard.lock };
        auto& cell = shard.cells.ensure({ which.scope, which.offset }, [] { return makeUnique<Cell>(); }).iterator->value;
        if (reader != nobody)
            cell->readers.add(reader);
        type |= cell->type.load();
    }
    return type;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramFunctions);
WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramClasses);

WTF_MAKE_TZONE_ALLOCATED_IMPL(MultiValueReturnTable);

static MultiValueReturnTable* s_multiValueReturnTable;
void setMultiValueReturnTable(MultiValueReturnTable* things) { s_multiValueReturnTable = things; }
MultiValueReturnTable* multiValueReturnTable() { return s_multiValueReturnTable; }

void MultiValueReturnTable::note(UnlinkedCodeBlock* code, Names&& names)
{
    Locker locker { m_lock };
    m_names.set(code, WTF::move(names));
}

const MultiValueReturnTable::Names* registerReturnValuesOf(UnlinkedCodeBlock* code, const FunctionSummary* summary)
{
    if (!s_multiValueReturnTable || !summary || !summary->isNonEscaping || summary->escapes.load(std::memory_order_relaxed) || summary->needsReturnObject.load(std::memory_order_relaxed))
        return nullptr;
    return s_multiValueReturnTable->returnValueNamesOf(code);
}

static ProgramClasses* s_programClasses;
void setProgramClasses(ProgramClasses* classes) { s_programClasses = classes; }
ProgramClasses* programClasses() { return s_programClasses; }

void ProgramClasses::recordNonEscapingMethod(uint32_t classType, UniquedStringImpl* name, uint32_t function)
{
    Locker locker { m_lock };
    auto result = m_methods.add({ classType, name }, function);
    if (!result.isNewEntry && result.iterator->value != function) {
        m_nonEscapingMethods.remove(result.iterator->value);
        result.iterator->value = 0;
        return;
    }
    if (result.iterator->value)
        m_nonEscapingMethods.add(function);
}

void ProgramClasses::noteThisIn(UnlinkedCodeBlock* code, uint16_t layoutID)
{
    if (!code)
        return;
    Locker locker { m_lock };
    auto result = m_thisLayoutID.add(code, layoutID);
    if (!result.isNewEntry && result.iterator->value != layoutID)
        result.iterator->value = 0;
}

void closePropertyEffects(std::span<const std::unique_ptr<FunctionSummary>> summaries)
{
    for (bool changed = true; std::exchange(changed, false);) {
        for (auto& summary : summaries) {
            auto& effects = summary->propertyEffects;
            if (effects.isArbitrary)
                continue;
            for (const FunctionSummary* callee : effects.callees) {
                auto& calleeEffects = callee->propertyEffects;
                if (calleeEffects.isArbitrary) {
                    effects.isArbitrary = true;
                    break;
                }
                for (UniquedStringImpl* name : calleeEffects.storedNames) {
                    if (!effects.storedNames.contains(name)) {
                        effects.storedNames.append(name);
                        changed = true;
                    }
                }
            }
            if (effects.storedNames.size() > FunctionSummary::PropertyEffects::maxStoredNames)
                effects.isArbitrary = true;
            changed |= effects.isArbitrary;
        }
    }
}

static const ProgramFunctions* s_programFunctions;
void setProgramFunctions(const ProgramFunctions* functions) { s_programFunctions = functions; }
const ProgramFunctions* programFunctions() { return s_programFunctions; }

Type VariableSummaries::join(Variable variable, Type type)
{
    Shard& shard = shardFor(variable);
    Locker locker { shard.lock };
    auto& cell = shard.cells.ensure({ variable.scope, variable.offset }, [] { return makeUnique<Cell>(); }).iterator->value;
    Type before = cell->type.load();
    if ((before | type) == before)
        return before;
    cell->type.store(before | type);
    cell->grew = true;
    return before;
}

Vector<unsigned> VariableSummaries::untrackVariablesReadButNeverWritten(unsigned& count)
{
    UncheckedKeyHashSet<const void*> made;
    for (auto& shard : m_shards) {
        for (auto& entry : shard.cells) {
            if (entry.key.second == Variable::initialValue && entry.value->type.load())
                made.add(entry.key.first);
        }
    }
    ReaderSet result;
    for (auto& shard : m_shards) {
        for (auto& entry : shard.cells) {
            if (made.contains(entry.key.first) || m_untrackedScopes.contains(entry.key.first) || entry.value->readers.isEmpty())
                continue;
            for (unsigned reader : entry.value->readers)
                result.add(reader);
        }
    }
    for (auto& shard : m_shards) {
        for (auto& entry : shard.cells) {
            if (!made.contains(entry.key.first) && !entry.value->readers.isEmpty() && m_untrackedScopes.add(entry.key.first).isNewEntry)
                ++count;
        }
    }
    return copyToVector(result);
}

Vector<unsigned> VariableSummaries::takeWidenedVariableReaders()
{
    ReaderSet result;
    for (auto& shard : m_shards) {
        for (auto& entry : shard.cells) {
            if (!std::exchange(entry.value->grew, false))
                continue;
            for (unsigned reader : entry.value->readers)
                result.add(reader);
        }
    }
    return copyToVector(result);
}

CalleeHints::~CalleeHints() = default;
ModuleHints::~ModuleHints() = default;

void ModuleHints::add(unsigned scopeOffset, UnlinkedFunctionExecutable* executable, const Describe& describe)
{
    Variable& variable = m_variables.add(scopeOffset, Variable { }).iterator->value;
    if (variable.numberOfFunctions++)
        return;
    variable.function.executable = executable;
    variable.isDescribed = describe(executable, variable.function);
}

ModuleHints::ModuleHints(UnlinkedCodeBlock* codeBlock, std::span<const Binding> bindings, const Describe& describe)
    : m_module(dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock))
{
    if (!m_module)
        return;
    for (auto& binding : bindings)
        m_variables.add(binding.scopeOffset, Variable { }).iterator->value.binding = binding;
    if (auto* slots = m_module->heapAllocatedFunctionDeclSlots(); slots && !slots->hasDecodeSource()) {
        for (unsigned i = 0; i < slots->size() && i < m_module->numberOfFunctionDecls(); ++i) {
            add(slots->at(i).offset(), m_module->functionDecl(i), describe);
            m_variables.find(slots->at(i).offset())->value.function.isDeclaration = true;
        }
    }
}

void ModuleHints::recordFunctionAssignmentsIn(UnlinkedCodeBlock* codeBlock, const Describe& describe)
{
    if (!m_module)
        return;
    const DeclaredNamesLink* declaredNames = nullptr;
    for (auto& note : functionAssignmentsIn(codeBlock)) {
        if (note.functionExpr >= codeBlock->numberOfFunctionExprs())
            continue;
        if (note.isOwn) {
            if (codeBlock == m_module && note.symbolTableConstantIndex == m_module->moduleEnvironmentSymbolTableConstantRegisterOffset())
                add(note.scopeOffset, codeBlock->functionExpr(note.functionExpr), describe);
            continue;
        }
        if (!declaredNames)
            declaredNames = declaredNamesFor(codeBlock);
        if (!declaredNames)
            return;
        auto resolution = declaredNames->resolve(codeBlock->identifier(note.identifier).impl());
        if (resolution.kind == DeclaredNamesLink::Resolution::Slot && resolution.isInOutermostEnvironment)
            add(resolution.offset, codeBlock->functionExpr(note.functionExpr), describe);
    }
}

void ModuleHints::prove()
{
    for (auto& entry : m_variables) {
        Variable& variable = entry.value;
        variable.function.isExact = variable.binding.keepsDeclaredValue && variable.numberOfFunctions == 1 && variable.isDescribed;
        variable.function.escapes = variable.binding.escapes;
        variable.function.isExternallyVisible = variable.binding.isExternallyVisible;
        variable.function.needsNoFunctionObject = variable.function.isExact && variable.function.forCall && !needsFunctionObject(variable.function.forCall);
    }
}

const void* ModuleHints::variableScope() const
{
    return m_module ? m_module->getConstant(VirtualRegister(m_module->moduleEnvironmentSymbolTableConstantRegisterOffset())).asCell() : nullptr;
}

void ModuleHints::noteEscape(unsigned scopeOffset)
{
    if (auto it = m_variables.find(scopeOffset); it != m_variables.end())
        it->value.function.escapes = it->value.function.isExternallyVisible = true;
}

unsigned KnownShape::inlineCapacityFor(unsigned numberOfProperties)
{
    unsigned capacity = std::min(std::max(numberOfProperties, 1u), JSFinalObject::maxInlineCapacity);
    size_t size = JSFinalObject::allocationSize(capacity);
    capacity += (MarkedSpace::optimalSizeFor(size) - size) / sizeof(WriteBarrier<Unknown>);
    return std::min(capacity, JSFinalObject::maxInlineCapacity);
}

std::optional<unsigned> intrinsicForLinkTimeConstant(JSValue constant)
{
    if (static_cast<LinkTimeConstant>(constant.asInt32AsAnyInt()) != LinkTimeConstant::arrayProtoValues)
        return std::nullopt;
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    if (!intrinsics)
        return std::nullopt;
    static unsigned number;
    static std::once_flag once;
    std::call_once(once, [&] {
        unsigned holder = ImmutableIntrinsics::globalObject;
        for (ASCIILiteral name : { "Array"_s, "prototype"_s, "values"_s }) {
            String string { name };
            unsigned found = intrinsics->find(holder, *string.impl());
            if (!found)
                return;
            holder = intrinsics->at(found).canonical;
        }
        number = holder;
    });
    return number ? std::optional { number } : std::nullopt;
}

ValueRepresentations valueRepresentations(const FunctionSummary* summary, Convention convention)
{
    ValueRepresentations result;
    if (!summary || !summary->isNonEscaping || convention.signature != Signature::Registers || Options::validateAOTInferredTypes())
        return result;
    auto repFor = [](Type type) {
        Rep rep = type ? repForType(type) : Rep::JSValue;
        return rep == Rep::Int32 || rep == Rep::Double || rep == Rep::Boolean ? rep : Rep::JSValue;
    };
    static_assert(numberOfArgumentGPRs < FunctionSummary::maxParameters);
    for (unsigned i = 0; i < convention.numberOfParameters; ++i)
        result.parameters[i] = repFor(summary->parameterTypes[i + 1].load());
    if (!summary->returnsBoxed)
        result.result = repFor(summary->returnType.load());
    return result;
}

Vector<Rep, 8> returnValueReps(const FunctionSummary* summary, unsigned count)
{
    bool skipsValidation = !Options::validateAOTInferredTypes();
    Vector<Rep, 8> result;
    for (unsigned i = 0; i < count; ++i) {
        Type type = summary->returnValueTypes[i].load();
        Rep rep = skipsValidation && type ? repForType(type) : Rep::JSValue;
        result.append(rep == Rep::Int32 || rep == Rep::Double || rep == Rep::Boolean ? rep : Rep::JSValue);
    }
    return result;
}

Convention conventionOf(UnlinkedCodeBlock* codeBlock)
{
    Convention result;
    unsigned numberOfParameters = codeBlock->numParameters() - 1;
    bool takesList = false;
    result.usesThis = codeBlock->isConstructor();
    for (const auto& instruction : codeBlock->instructions()) {
        switch (instruction->opcodeID()) {
        case op_create_direct_arguments:
        case op_create_scoped_arguments:
        case op_create_cloned_arguments:
        case op_create_rest:
        case op_argument_count:
            takesList = true;
            break;
        case op_get_argument:
            numberOfParameters = std::max<unsigned>(numberOfParameters, instruction->as<OpGetArgument>().m_index);
            break;
        default:
            break;
        }
        for (unsigned checkpoint = 0; checkpoint < instruction->numberOfCheckpoints() && !result.usesThis; ++checkpoint) {
            computeUsesForBytecodeIndexImpl(instruction.ptr(), checkpoint, [&](VirtualRegister reg) {
                result.usesThis |= reg == virtualRegisterForArgumentIncludingThis(0);
            });
        }
    }
    takesList |= numberOfParameters > numberOfArgumentGPRs;
    result.signature = takesList ? Signature::List : Signature::Registers;
    result.numberOfParameters = takesList ? 0 : numberOfParameters;
    return result;
}

bool needsFunctionObject(UnlinkedCodeBlock* codeBlock)
{
    if (codeBlock->codeType() != FunctionCode || codeBlock->isConstructor())
        return true;
    const DeclaredNamesLink* declaredNames = declaredNamesFor(codeBlock);
    if (!declaredNames || !declaredNames->scopeIsOutermostEnvironment())
        return true;
    return readsCallee(codeBlock);
}

bool readsCallee(UnlinkedCodeBlock* codeBlock)
{
    Vector<VirtualRegister, 2> copies;
    bool result = false;
    for (const auto& instruction : codeBlock->instructions()) {
        switch (instruction->opcodeID()) {
        case op_create_direct_arguments:
        case op_create_scoped_arguments:
        case op_create_cloned_arguments:
        case op_call_direct_eval:
            return true;
        case op_mov:
            if (auto bytecode = instruction->as<OpMov>(); bytecode.m_src == VirtualRegister(CallFrameSlot::callee)) {
                copies.append(bytecode.m_dst);
                continue;
            }
            break;
        default:
            break;
        }
        for (unsigned checkpoint = 0; checkpoint < instruction->numberOfCheckpoints(); ++checkpoint) {
            computeUsesForBytecodeIndexImpl(instruction.ptr(), checkpoint, [&](VirtualRegister reg) {
                result |= reg == VirtualRegister(CallFrameSlot::callee);
            });
        }
        if (result)
            return true;
    }
    if (copies.isEmpty())
        return false;
    for (const auto& instruction : codeBlock->instructions()) {
        for (unsigned checkpoint = 0; checkpoint < instruction->numberOfCheckpoints(); ++checkpoint) {
            computeUsesForBytecodeIndexImpl(instruction.ptr(), checkpoint, [&](VirtualRegister reg) {
                result |= copies.contains(reg);
            });
        }
        if (result)
            return true;
    }
    return false;
}

unsigned ModuleHints::numberOfSingleFunctionVariables() const
{
    unsigned result = 0;
    for (auto& entry : m_variables)
        result += entry.value.function.isExact;
    return result;
}

const KnownFunction* ModuleHints::find(UniquedStringImpl*, std::optional<unsigned> scopeOffset) const
{
    if (!scopeOffset)
        return nullptr;
    auto it = m_variables.find(*scopeOffset);
    if (it == m_variables.end() || it->value.numberOfFunctions != 1 || !it->value.isDescribed)
        return nullptr;
    return &it->value.function;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

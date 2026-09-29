/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgram.h"

#if ENABLE(FTL_JIT)

#include "AOTImage.h"
#include "AOTType.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "PreciseJumpTargets.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VariableFacts);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(VariableFacts::Cell);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(ProgramFacts);
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

static UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<FunctionPutInVariable>>& functionsPutInVariables() WTF_REQUIRES_LOCK(s_declaredNamesLock)
{
    static NeverDestroyed<UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<FunctionPutInVariable>>> map;
    return map;
}

void noteFunctionsPutInVariables(UnlinkedCodeBlock* codeBlock, Vector<FunctionPutInVariable>&& functions)
{
    Locker locker { s_declaredNamesLock };
    functionsPutInVariables().set(codeBlock, WTF::move(functions));
}

static UncheckedKeyHashMap<uint32_t, std::unique_ptr<KnownFunction>>& bodiesOfFacts()
{
    static NeverDestroyed<UncheckedKeyHashMap<uint32_t, std::unique_ptr<KnownFunction>>> bodies;
    return bodies;
}

void noteBodyOfFact(uint32_t body, const KnownFunction& known)
{
    // Twice: the text was copied, and there is no saying which copy is meant.
    auto result = bodiesOfFacts().add(body, nullptr);
    result.iterator->value = result.isNewEntry ? makeUniqueWithoutFastMallocCheck<KnownFunction>(known) : nullptr;
}

const KnownFunction* bodyOfFact(uint32_t body)
{
    auto it = bodiesOfFacts().find(body);
    return it == bodiesOfFacts().end() ? nullptr : it->value.get();
}

Vector<FunctionPutInVariable> functionsPutInVariablesBy(UnlinkedCodeBlock* codeBlock)
{
    Locker locker { s_declaredNamesLock };
    return functionsPutInVariables().get(codeBlock);
}

static const NumbersOfIdentifiers* s_numbersOfIdentifiersOfProgram;

void setNumbersOfIdentifiersOfProgram(const NumbersOfIdentifiers* numbers)
{
    s_numbersOfIdentifiersOfProgram = numbers;
}

const NumbersOfIdentifiers* numbersOfIdentifiersOfProgram()
{
    return s_numbersOfIdentifiersOfProgram;
}

static const NumbersOfConstants* s_numbersOfConstantsOfProgram;

void setNumbersOfConstantsOfProgram(const NumbersOfConstants* numbers)
{
    s_numbersOfConstantsOfProgram = numbers;
}

const Vector<uint32_t>* numbersOfConstantsOfProgramFor(UnlinkedCodeBlock* codeBlock)
{
    if (!s_numbersOfConstantsOfProgram)
        return nullptr;
    auto it = s_numbersOfConstantsOfProgram->find(codeBlock);
    return it == s_numbersOfConstantsOfProgram->end() ? nullptr : &it->value;
}

void forgetDeclaredNames()
{
    Locker locker { s_declaredNamesLock };
    declaredNames().clear();
    functionsPutInVariables().clear();
}

void VariableFacts::giveUpOnName(UniquedStringImpl* name)
{
    Locker locker { m_givenUpLock };
    m_namesGivenUpOn.add(name);
}

void VariableFacts::giveUpOnScope(const void* scope)
{
    Locker locker { m_givenUpLock };
    m_scopesGivenUpOn.add(scope);
}

bool VariableFacts::hasGivenUpOn(Variable variable, UniquedStringImpl* name) const
{
    return m_scopesGivenUpOn.contains(variable.scope) || m_namesGivenUpOn.contains(name);
}

uint64_t VariableFacts::read(Variable variable, UniquedStringImpl* name, unsigned reader)
{
    if (hasGivenUpOn(variable, name))
        return TAll;
    uint64_t type = 0;
    for (unsigned offset : { variable.offset, Variable::initialValue }) {
        Variable which { variable.scope, offset };
        Shard& shard = shardFor(which);
        Locker locker { shard.lock };
        auto& cell = shard.cells.ensure({ which.scope, which.offset }, [] { return makeUnique<Cell>(); }).iterator->value;
        if (reader != nobody)
            cell->readers.add(reader);
        type |= cell->type.load(std::memory_order_relaxed);
    }
    return type;
}

void VariableFacts::join(Variable variable, uint64_t type)
{
    Shard& shard = shardFor(variable);
    Locker locker { shard.lock };
    auto& cell = shard.cells.ensure({ variable.scope, variable.offset }, [] { return makeUnique<Cell>(); }).iterator->value;
    uint64_t before = cell->type.load(std::memory_order_relaxed);
    if ((before | type) == before)
        return;
    cell->type.store(before | type, std::memory_order_relaxed);
    cell->grew = true;
}

Vector<unsigned> VariableFacts::giveUpOnWhatIsReadAndNeverMade(unsigned& count)
{
    UncheckedKeyHashSet<const void*> made;
    for (auto& shard : m_shards) {
        for (auto& entry : shard.cells) {
            if (entry.key.second == Variable::initialValue && entry.value->type.load(std::memory_order_relaxed))
                made.add(entry.key.first);
        }
    }
    SetOfReaders result;
    for (auto& shard : m_shards) {
        for (auto& entry : shard.cells) {
            if (made.contains(entry.key.first) || m_scopesGivenUpOn.contains(entry.key.first) || entry.value->readers.isEmpty())
                continue;
            for (unsigned reader : entry.value->readers)
                result.add(reader);
        }
    }
    for (auto& shard : m_shards) {
        for (auto& entry : shard.cells) {
            if (!made.contains(entry.key.first) && !entry.value->readers.isEmpty() && m_scopesGivenUpOn.add(entry.key.first).isNewEntry)
                ++count;
        }
    }
    return copyToVector(result);
}

Vector<unsigned> VariableFacts::takeReadersOfWhatGrew()
{
    SetOfReaders result;
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

void ModuleHints::noteFunctionsPutInVariablesBy(UnlinkedCodeBlock* codeBlock, const Describe& describe)
{
    if (!m_module)
        return;
    const DeclaredNamesLink* declaredNames = nullptr;
    for (auto& note : functionsPutInVariablesBy(codeBlock)) {
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
        variable.function.isProven = variable.binding.holdsWhatItWasDeclaredWith && variable.numberOfFunctions == 1 && variable.isDescribed;
        variable.function.escapes = variable.binding.escapes;
        // (It is strict code, which nobody can ask what it was called as. Function.prototype.caller can ask the other kind.)
        variable.function.needsNoFunctionObject = variable.function.isProven && variable.function.forCall && !needsFunctionObject(variable.function.forCall);
    }
}

const void* ModuleHints::scopeOfVariables() const
{
    return m_module ? m_module->getConstant(VirtualRegister(m_module->moduleEnvironmentSymbolTableConstantRegisterOffset())).asCell() : nullptr;
}

void ModuleHints::noteEscape(unsigned scopeOffset)
{
    if (auto it = m_variables.find(scopeOffset); it != m_variables.end())
        it->value.function.escapes = true;
}

unsigned KnownShape::inlineCapacityFor(unsigned numberOfProperties)
{
    unsigned capacity = std::min(std::max(numberOfProperties, 1u), JSFinalObject::maxInlineCapacity);
    // With whatever else there is room for in a cell of the size it takes.
    size_t size = JSFinalObject::allocationSize(capacity);
    capacity += (MarkedSpace::optimalSizeFor(size) - size) / sizeof(WriteBarrier<Unknown>);
    return std::min(capacity, JSFinalObject::maxInlineCapacity);
}

Convention conventionOf(UnlinkedCodeBlock* codeBlock)
{
    Convention result;
    unsigned numberOfParameters = codeBlock->numParameters() - 1;
    bool takesList = numberOfParameters > numberOfArgumentGPRs;
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
            // (It counts `this`.)
            takesList |= static_cast<unsigned>(instruction->as<OpGetArgument>().m_index) > numberOfParameters;
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
    bool result = false;
    for (const auto& instruction : codeBlock->instructions()) {
        switch (instruction->opcodeID()) {
        case op_create_direct_arguments:
        case op_create_scoped_arguments:
        case op_create_cloned_arguments:
        case op_call_direct_eval:
            return true;
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
    return false;
}

unsigned ModuleHints::numberProven() const
{
    unsigned result = 0;
    for (auto& entry : m_variables)
        result += entry.value.function.isProven;
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

#endif // ENABLE(FTL_JIT)

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
WTF_MAKE_TZONE_ALLOCATED_IMPL(LiveHints);
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

uint32_t VariableFacts::read(Variable variable, UniquedStringImpl* name, unsigned reader)
{
    if (hasGivenUpOn(variable, name))
        return TAll;
    uint32_t type = 0;
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

void VariableFacts::join(Variable variable, uint32_t type)
{
    Shard& shard = shardFor(variable);
    Locker locker { shard.lock };
    auto& cell = shard.cells.ensure({ variable.scope, variable.offset }, [] { return makeUnique<Cell>(); }).iterator->value;
    uint32_t before = cell->type.load(std::memory_order_relaxed);
    if ((before | type) == before)
        return;
    cell->type.store(before | type, std::memory_order_relaxed);
    cell->grew = true;
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
LiveHints::~LiveHints() = default;

void ModuleHints::add(UniquedStringImpl* name, unsigned scopeOffset, UnlinkedFunctionExecutable* executable, const Describe& describe)
{
    auto result = m_variables.add(name, Variable { });
    Variable& variable = result.iterator->value;
    if (!result.isNewEntry) {
        // Two variables of the same name, in scopes that cannot be told apart from here, or one that is given two functions.
        if (variable.function.executable != executable || variable.scopeOffset != scopeOffset)
            variable.isAmbiguous = true;
        return;
    }
    variable.scopeOffset = scopeOffset;
    variable.function.executable = executable;
    if (!describe(executable, variable.function))
        variable.isAmbiguous = true;
}

ModuleHints::ModuleHints(UnlinkedCodeBlock* codeBlock, const Describe& describe)
{
    if (auto* module = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock)) {
        m_isModule = true;
        if (auto* slots = module->heapAllocatedFunctionDeclSlots(); slots && !slots->hasDecodeSource()) {
            for (unsigned i = 0; i < slots->size() && i < module->numberOfFunctionDecls(); ++i) {
                UnlinkedFunctionExecutable* executable = module->functionDecl(i);
                add(executable->name().impl(), slots->at(i).offset(), executable, describe);
                Variable& variable = m_variables.find(executable->name().impl())->value;
                variable.isInitializedInPlainSight = true;
                variable.function.isDeclaration = true;
            }
        }
    }

    // const f = function () { }, const g = () => { }, class C { }: a function is made and, sooner or later, put in a variable.
    using Registers = UncheckedKeyHashMap<int, unsigned, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>>;
    Registers functionInRegister; // Probably.
    Registers functionCertainlyInRegister; // Nothing else has been written there since, and there is no other way to get here.
    Vector<JSInstructionStream::Offset, 32> jumpTargets;
    computePreciseJumpTargets(codeBlock, jumpTargets);
    unsigned nextJumpTarget = 0;
    for (const auto& instruction : codeBlock->instructions()) {
        while (nextJumpTarget < jumpTargets.size() && jumpTargets[nextJumpTarget] < instruction.offset())
            ++nextJumpTarget;
        if (nextJumpTarget < jumpTargets.size() && jumpTargets[nextJumpTarget] == instruction.offset())
            functionCertainlyInRegister.clear();

        OpcodeID opcode = instruction->opcodeID();
        if (opcode == op_call_direct_eval)
            m_hasEval = true;
        std::optional<std::pair<int, unsigned>> certain;
        switch (opcode) {
        case op_new_func_exp: {
            auto bytecode = instruction->as<OpNewFuncExp>();
            functionInRegister.set(bytecode.m_dst.offset(), bytecode.m_functionDecl);
            certain = { bytecode.m_dst.offset(), bytecode.m_functionDecl };
            break;
        }
        case op_mov: {
            auto bytecode = instruction->as<OpMov>();
            auto it = functionInRegister.find(bytecode.m_src.offset());
            if (it != functionInRegister.end()) {
                unsigned function = it->value;
                functionInRegister.set(bytecode.m_dst.offset(), function);
            } else
                functionInRegister.remove(bytecode.m_dst.offset());
            if (auto known = functionCertainlyInRegister.find(bytecode.m_src.offset()); known != functionCertainlyInRegister.end())
                certain = { bytecode.m_dst.offset(), known->value };
            break;
        }
        case op_put_to_scope: {
            auto bytecode = instruction->as<OpPutToScope>();
            UniquedStringImpl* name = codeBlock->identifier(bytecode.m_var).impl();
            if (bytecode.m_getPutInfo.resolveType() != ResolvedClosureVar) {
                noteStore(name);
                break;
            }
            auto it = functionInRegister.find(bytecode.m_value.offset());
            bool isNew = !m_variables.contains(name);
            if (it != functionInRegister.end() && it->value < codeBlock->numberOfFunctionExprs()) {
                add(name, bytecode.m_offset, codeBlock->functionExpr(it->value), describe);
                auto known = functionCertainlyInRegister.find(bytecode.m_value.offset());
                if (isNew && known != functionCertainlyInRegister.end() && known->value == it->value)
                    m_variables.find(name)->value.isInitializedInPlainSight = true;
                else
                    noteStore(name);
            } else {
                if (auto variable = m_variables.find(name); variable != m_variables.end() && variable->value.scopeOffset == bytecode.m_offset)
                    variable->value.isAmbiguous = true;
                noteStore(name);
            }
            break;
        }
        default:
            break;
        }

        for (unsigned checkpoint = 0; checkpoint < instruction->numberOfCheckpoints(); ++checkpoint) {
            computeDefsForBytecodeIndexImpl(codeBlock->numVars(), instruction.ptr(), checkpoint, [&](VirtualRegister reg) {
                functionCertainlyInRegister.remove(reg.offset());
            });
        }
        if (certain)
            functionCertainlyInRegister.set(certain->first, certain->second);
        if (isBranch(opcode) || isTerminal(opcode) || isThrow(opcode))
            functionCertainlyInRegister.clear();
    }
}

// A store that comes before the variable is known to be one of these is remembered all the same.
void ModuleHints::noteStore(UniquedStringImpl* name)
{
    auto result = m_variables.add(name, Variable { });
    if (result.isNewEntry)
        result.iterator->value.isAmbiguous = true;
    result.iterator->value.isStoredToOtherwise = true;
}

void ModuleHints::noteStoresIn(UnlinkedCodeBlock* codeBlock)
{
    const DeclaredNamesLink* declaredNames = declaredNamesFor(codeBlock);
    for (const auto& instruction : codeBlock->instructions()) {
        if (instruction->opcodeID() == op_call_direct_eval)
            m_hasEval = true;
        if (instruction->opcodeID() != op_put_to_scope)
            continue;
        auto bytecode = instruction->as<OpPutToScope>();
        // (It may be a variable of the function's own that has the same name. Then this errs on the safe side.)
        UniquedStringImpl* name = codeBlock->identifier(bytecode.m_var).impl();
        if (declaredNames) {
            auto resolution = declaredNames->resolve(name);
            switch (resolution.kind) {
            case DeclaredNamesLink::Resolution::Slot:
                if (!resolution.isInOutermostEnvironment)
                    continue; // Something nearer has the name.
                break;
            case DeclaredNamesLink::Resolution::Stable: // Storing to an import throws.
            case DeclaredNamesLink::Resolution::Global:
                continue;
            case DeclaredNamesLink::Resolution::Dynamic:
                break;
            }
        }
        noteStore(name);
    }
}

void ModuleHints::prove()
{
    if (m_hasEval)
        return;
    for (auto& entry : m_variables) {
        Variable& variable = entry.value;
        variable.function.isProven = !variable.isAmbiguous && variable.isInitializedInPlainSight && !variable.isStoredToOtherwise;
        // (Of a module: then it is strict code, which nobody can ask what it was called as. Function.prototype.caller can ask the other kind.)
        variable.function.needsNoFunctionObject = m_isModule && variable.function.isProven && variable.function.forCall && !needsFunctionObject(variable.function.forCall);
    }
}

unsigned KnownShape::inlineCapacityFor(unsigned numberOfProperties)
{
    unsigned capacity = std::min(std::max(numberOfProperties, 1u), JSFinalObject::maxInlineCapacity);
    // With whatever else there is room for in a cell of the size it takes.
    size_t size = JSFinalObject::allocationSize(capacity);
    capacity += (MarkedSpace::optimalSizeFor(size) - size) / sizeof(WriteBarrier<Unknown>);
    return std::min(capacity, JSFinalObject::maxInlineCapacity);
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

const KnownFunction* ModuleHints::find(UniquedStringImpl* name, std::optional<unsigned> scopeOffset) const
{
    if (!scopeOffset)
        return nullptr;
    auto it = m_variables.find(name);
    if (it == m_variables.end() || it->value.isAmbiguous || it->value.scopeOffset != *scopeOffset)
        return nullptr;
    return &it->value.function;
}

LiveHints::LiveHints(JSGlobalObject* globalObject)
    : m_globalObject(globalObject)
{
}

const KnownFunction* LiveHints::find(UniquedStringImpl* name, std::optional<unsigned> scopeOffset) const
{
    if (scopeOffset)
        return nullptr;
    VM& vm = m_globalObject->vm();
    JSValue value;
    // Looking, and nothing else: no getter is called and nothing is reified.
    if (auto entry = m_globalObject->globalLexicalEnvironment()->symbolTable()->get(name); !entry.isNull())
        value = m_globalObject->globalLexicalEnvironment()->variableAt(entry.scopeOffset()).get();
    else if (auto entry = m_globalObject->symbolTable()->get(name); !entry.isNull())
        value = m_globalObject->variableAt(entry.scopeOffset()).get();
    else
        value = m_globalObject->getDirect(vm, PropertyName(Identifier::fromUid(vm, name)));
    auto* function = value ? dynamicDowncast<JSFunction>(value) : nullptr;
    if (!function || function->isHostOrBuiltinFunction())
        return nullptr;
    FunctionExecutable* executable = function->jsExecutable();
    auto key = imageKeyFor(executable, CodeSpecializationKind::CodeForCall);
    if (!key)
        return nullptr;
    auto known = makeUniqueWithoutFastMallocCheck<KnownFunction>();
    known->executable = executable->unlinkedExecutable();
    known->key = *key;
    auto [forCall, forConstruct] = known->executable->codeBlocksDecodingCached(vm);
    known->forCall = forCall;
    known->forConstruct = forConstruct;
    m_functions.append(WTF::move(known));
    return m_functions.last().get();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

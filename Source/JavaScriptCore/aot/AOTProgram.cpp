/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgram.h"

#if ENABLE(FTL_JIT)

#include "AOTImage.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

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

void forgetDeclaredNames()
{
    Locker locker { s_declaredNamesLock };
    declaredNames().clear();
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
        if (auto* slots = module->heapAllocatedFunctionDeclSlots(); slots && !slots->hasDecodeSource()) {
            for (unsigned i = 0; i < slots->size() && i < module->numberOfFunctionDecls(); ++i) {
                UnlinkedFunctionExecutable* executable = module->functionDecl(i);
                add(executable->name().impl(), slots->at(i).offset(), executable, describe);
            }
        }
    }

    // const f = function () { }, const g = () => { }, class C { }: a function is made and, sooner or later, put in a variable.
    UncheckedKeyHashMap<int, unsigned, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>> functionInRegister;
    for (const auto& instruction : codeBlock->instructions()) {
        switch (instruction->opcodeID()) {
        case op_new_func_exp: {
            auto bytecode = instruction->as<OpNewFuncExp>();
            functionInRegister.set(bytecode.m_dst.offset(), bytecode.m_functionDecl);
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
            break;
        }
        case op_put_to_scope: {
            auto bytecode = instruction->as<OpPutToScope>();
            if (bytecode.m_getPutInfo.resolveType() != ResolvedClosureVar)
                break;
            UniquedStringImpl* name = codeBlock->identifier(bytecode.m_var).impl();
            auto it = functionInRegister.find(bytecode.m_value.offset());
            if (it != functionInRegister.end() && it->value < codeBlock->numberOfFunctionExprs())
                add(name, bytecode.m_offset, codeBlock->functionExpr(it->value), describe);
            else if (auto variable = m_variables.find(name); variable != m_variables.end() && variable->value.scopeOffset == bytecode.m_offset)
                variable->value.isAmbiguous = true;
            break;
        }
        default:
            break;
        }
    }
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

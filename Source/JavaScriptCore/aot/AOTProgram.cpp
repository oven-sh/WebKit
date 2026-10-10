/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgram.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTOpcodeTraits.h"
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


unsigned KnownShape::inlineCapacityFor(unsigned numberOfProperties)
{
    unsigned capacity = std::min(std::max(numberOfProperties, 1u), JSFinalObject::maxInlineCapacity);
    size_t size = JSFinalObject::allocationSize(capacity);
    capacity += (MarkedSpace::optimalSizeFor(size) - size) / sizeof(WriteBarrier<Unknown>);
    return std::min(capacity, JSFinalObject::maxInlineCapacity);
}

Convention conventionOf(UnlinkedCodeBlock* codeBlock)
{
    Convention result;
    unsigned numberOfParameters = codeBlock->numParameters() - 1;
    bool takesList = false;
    result.usesThis = codeBlock->isConstructor();
    for (const auto& instruction : codeBlock->instructions()) {
        takesList |= !!(traitsOf(instruction->opcodeID()) & OpcodeTraits::ReadsArgumentList);
        if (instruction->opcodeID() == op_get_argument)
            numberOfParameters = std::max<unsigned>(numberOfParameters, instruction->as<OpGetArgument>().m_index);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

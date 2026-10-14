/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgram.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTOpcodeTraits.h"
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

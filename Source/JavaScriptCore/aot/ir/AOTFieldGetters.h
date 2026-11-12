/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

inline PropertyKeyID fieldReturnedByGetter(const Program& program, const Function& getter)
{
    if (getter.kind != FunctionKind::Getter || !getter.hasBody() || getter.isCoroutine() || getter.body.numberOfBlocks() != 1)
        return { };
    BodyBlock block = getter.body.entry();
    if (block.hasHandler() || block.numberOfValues() != 1)
        return { };
    BodyValue read = *block.values().begin();
    if (read.opcode() != Opcode::PropGet || read.isFreeName())
        return { };
    std::optional<uint32_t> parameter = block.indexOfParameter(read.objectOperand());
    if (!parameter || *parameter >= getter.numberOfEntryParameters() || getter.entryParameterKind(*parameter) != EntryParameter::This)
        return { };
    BodyTerminator terminator = block.terminator();
    if (terminator.kind() != TerminatorKind::Return || terminator.as<TerminatorKind::Return>().valueOperand() != read.id())
        return { };
    return program.at(read.key()).kind == PropertyKeyKind::PrivateName ? PropertyKeyID() : read.key();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

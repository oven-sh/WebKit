/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTIR.h"
#include "AOTOperand.h"
#include <optional>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class CodeGenerator;
class ExceptionEmitter;

class OperationContext {
    WTF_MAKE_NONCOPYABLE(OperationContext);
public:
    BlockID block() const { return m_block; }
    ValueID valueID() const { return m_valueID; }
    const Instruction& instruction() const LIFETIME_BOUND { return m_instruction; }
    Origin origin() const { return m_instruction.origin(); }
    const EffectsOnEdges& effects() const LIFETIME_BOUND { return m_effects; }
    CodeShape shape() const { return m_shape; }

protected:
    OperationContext(BlockID blockID, const Block& block, ValueID valueID, const Instruction& instruction, const EffectsOnEdges& effects)
        : m_block(blockID)
        , m_valueID(valueID)
        , m_instruction(instruction)
        , m_effects(effects)
        , m_handler(block.handler && effects.mayThrow() ? &*block.handler : nullptr)
        , m_shape(block.shape)
    {
    }

private:
    friend class ExceptionEmitter;

    const Target* handler() const { return m_handler; }

    BlockID m_block;
    ValueID m_valueID;
    const Instruction& m_instruction;
    const EffectsOnEdges& m_effects;
    const Target* m_handler;
    CodeShape m_shape;
};

class TerminatorContext final : public OperationContext {
public:
    const Terminator& terminator() const LIFETIME_BOUND { return m_terminator; }
    const Terminator* operator->() const LIFETIME_BOUND { return &m_terminator; }
    TerminatorKind kind() const { return m_terminator.kind; }

private:
    friend class CodeGenerator;

    TerminatorContext(BlockID blockID, const Block& block, const EffectsOnEdges& effects)
        : OperationContext(blockID, block, ValueID(), block.terminator, effects)
        , m_terminator(block.terminator)
    {
    }

    const Terminator& m_terminator;
};

class ValueContext final : public OperationContext {
public:
    ValueID id() const { return valueID(); }
    const Value& value() const LIFETIME_BOUND { return m_value; }
    const Value* operator->() const LIFETIME_BOUND { return &m_value; }
    Opcode opcode() const { return m_value.opcode; }
    Representation representation() const { return m_value.representation; }

    bool isBranchCondition() const { return m_branchTargets.has_value(); }
    const BranchTargets& branchTargets() const LIFETIME_BOUND
    {
        RELEASE_ASSERT(m_branchTargets);
        return *m_branchTargets;
    }

    const TerminatorContext* returnCall() const { return m_returnCall; }

private:
    friend class CodeGenerator;

    ValueContext(BlockID blockID, const Block& block, ValueID id, const Value& value, const EffectsOnEdges& effects)
        : OperationContext(blockID, block, id, value, effects)
        , m_value(value)
    {
    }

    const Value& m_value;
    std::optional<BranchTargets> m_branchTargets;
    const TerminatorContext* m_returnCall { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

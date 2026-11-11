/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTOperand.h"
#include <optional>
#include <span>
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

protected:
    OperationContext(BlockID block, ValueID valueID, const Instruction& instruction, const Edge* handler)
        : m_block(block)
        , m_valueID(valueID)
        , m_instruction(instruction)
        , m_handler(handler)
    {
    }

private:
    friend class CodeGenerator;
    friend class ExceptionEmitter;

    const Edge* handler() const { return m_handler; }

    BlockID m_block;
    ValueID m_valueID;
    const Instruction& m_instruction;
    const Edge* m_handler;
    mutable bool m_hasEffects { false };
    union {
        mutable Effects m_effects;
    };
};

class TerminatorContext final : public OperationContext {
public:
    const Terminator& terminator() const LIFETIME_BOUND { return m_terminator; }
    const Terminator* operator->() const LIFETIME_BOUND { return &m_terminator; }
    TerminatorKind kind() const { return m_terminator.kind; }
    uint32_t numberOfTargets() const { return m_terminator.numberOfEdges; }
    bool generatesCondition() const { return m_generatesCondition; }

private:
    friend class CodeGenerator;

    TerminatorContext(BlockID block, const Terminator& terminator, const Edge* handler)
        : OperationContext(block, ValueID(), terminator, handler)
        , m_terminator(terminator)
    {
    }

    const Terminator& m_terminator;
    bool m_generatesCondition { false };
};

class ValueContext final : public OperationContext {
public:
    ValueID id() const { return valueID(); }
    const Operation& value() const LIFETIME_BOUND { return m_value; }
    const Operation* operator->() const LIFETIME_BOUND { return &m_value; }
    Opcode opcode() const { return m_value.opcode; }
    Representation representation() const { return m_value.representation; }

    bool isBranchCondition() const { return m_branchTargets.has_value(); }
    const BranchTargets& branchTargets() const LIFETIME_BOUND
    {
        RELEASE_ASSERT(m_branchTargets);
        return *m_branchTargets;
    }

    const TerminatorContext* returnCall() const { return m_returnCall; }
    bool isAwaitedAtOnce() const { return m_isAwaitedAtOnce; }
    std::span<const ValueID> operationsBehind() const { return m_operationsBehind; }

private:
    friend class CodeGenerator;

    ValueContext(BlockID block, ValueID id, const Operation& value, const Edge* handler)
        : OperationContext(block, id, value, handler)
        , m_value(value)
    {
    }

    const Operation& m_value;
    std::optional<BranchTargets> m_branchTargets;
    const TerminatorContext* m_returnCall { nullptr };
    bool m_isAwaitedAtOnce { false };
    std::span<const ValueID> m_operationsBehind;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include <optional>
#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet readAccesses { Opcode::PropGet, Opcode::ElemGet };
static constexpr OpcodeSet writeAccesses { Opcode::PropSet, Opcode::ElemSet };
static constexpr OpcodeSet iterationAccesses { Opcode::IteratorOpen };

static constexpr OpcodeSet namedAccesses { Opcode::PropGet, Opcode::PropSet };
static constexpr OpcodeSet keyedAccesses { Opcode::ElemGet, Opcode::ElemSet };

static constexpr uint32_t receiverOperand = OperandIndex::PropGet::object;
static constexpr uint32_t keyOperand = OperandIndex::ElemGet::property;

static_assert(OperandIndex::PropSet::object == receiverOperand);
static_assert(OperandIndex::ElemGet::object == receiverOperand);
static_assert(OperandIndex::ElemSet::object == receiverOperand);
static_assert(OperandIndex::IteratorOpen::iterable == receiverOperand);
static_assert(OperandIndex::ElemSet::property == keyOperand);

static std::optional<NullishAccess> nullishAccessOf(Opcode opcode)
{
    if (readAccesses.contains(opcode))
        return NullishAccess::Read;
    if (writeAccesses.contains(opcode))
        return NullishAccess::Write;
    if (iterationAccesses.contains(opcode))
        return NullishAccess::Iterate;
    return std::nullopt;
}

static bool needsCheck(const Type& type)
{
    return type.mayBeUndefinedOrNull() && !type.anyObject && !type.isUndefinedOrNull();
}

static bool isNarrowedByCheck(const Type& type)
{
    return type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull();
}

namespace {

using CheckID = ID<struct CheckIDTag>;

struct Check {
    ProgramPoint point;
    ValueID access;
    ValueID receiver;
    NullishAccess kind;
    StringID name;
    ValueID result;
};

struct Rewrite {
    Use use;
    CheckID check;
};

class CheckInsertion {
public:
    explicit CheckInsertion(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
    {
    }

    PassOutcome run()
    {
        if (!requiresAnyCheck())
            return PassOutcome::Unchanged;
        collectChecks();
        if (m_rewrites.isEmpty())
            return PassOutcome::Unchanged;

        insertChecks();
        for (const Rewrite& rewrite : m_rewrites)
            replaceUse(m_function, rewrite.use, m_checks[rewrite.check.index()].result);
        m_context.analyses.didChangeValues();

        Vector<ValueID, 8> results;
        for (const Check& check : m_checks)
            results.append(check.result);
        narrowValueTypes(TypePropagationInputs { m_program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build }, results.span());
        return PassOutcome::Changed;
    }

private:
    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }

    std::optional<NullishAccess> requiredCheckOf(const Value& access) const
    {
        std::optional<NullishAccess> kind = nullishAccessOf(access.opcode);
        if (!kind || !needsCheck(typeOf(access.operand(receiverOperand))))
            return std::nullopt;
        if (namedAccesses.contains(access.opcode) && access.isFreeName())
            return std::nullopt;
        if (keyedAccesses.contains(access.opcode) && typeOf(access.operand(keyOperand)).hasObjects())
            return std::nullopt;
        return kind;
    }

    // Another pass puts such a check where a read it resolved threw.
    bool isNarrowingCheck(const Value& value) const
    {
        return value.opcode == Opcode::RequireObjectCoercible && isNarrowedByCheck(typeOf(value.valueOperand()));
    }

    bool requiresAnyCheck() const
    {
        for (const Block& block : m_function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = m_function.at(id);
                if (requiredCheckOf(value) || isNarrowingCheck(value))
                    return true;
            }
        }
        return false;
    }

    void collectChecks()
    {
        m_activeChecks.fill(CheckID(), m_function.values.size());
        auto enter = [&](BlockID block) {
            m_scopes.append(m_checkedReceivers.size());
            visitBlock(block);
        };
        auto leave = [&](BlockID) {
            size_t numberOfOuterReceivers = m_scopes.takeLast();
            while (m_checkedReceivers.size() > numberOfOuterReceivers) {
                auto [receiver, replacedCheck] = m_checkedReceivers.takeLast();
                m_activeChecks[receiver.index()] = replacedCheck;
            }
        };
        m_context.analyses.dominators().walkTree(enter, leave);
    }

    // A covered block dominates its handler and whatever the handler leads to, and its checks do not.
    bool isAvailableIn(const Check& check, BlockID block) const
    {
        return check.point.block == block || m_context.analyses.dominators().terminatorDominates(check.point.block, block);
    }

    CheckID activeCheckOf(ValueID value, BlockID block) const
    {
        CheckID check = m_activeChecks[value.index()];
        if (!check || !isAvailableIn(m_checks[check.index()], block))
            return { };
        return check;
    }

    void visitBlock(BlockID blockID)
    {
        const Block& block = m_function.at(blockID);
        if (block.handler)
            visitArguments(Use::Kind::HandlerArgument, blockID, *block.handler, 0);
        if (block.subject)
            visitUse(block.subject, Use { Use::Kind::BlockSubject, blockID, { }, 0, 0 });

        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            const Value& value = m_function.at(id);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                visitUse(operand, Use { Use::Kind::Operand, blockID, id, operandIndex, 0 });
            });
            if (isNarrowingCheck(value)) {
                if (!activeCheckOf(value.valueOperand(), blockID))
                    activate(Check { { blockID, index }, id, value.valueOperand(), value.nullishAccess(), value.name(), id });
                continue;
            }
            std::optional<NullishAccess> kind = requiredCheckOf(value);
            if (kind && !activeCheckOf(value.operand(receiverOperand), blockID))
                addCheck({ blockID, index }, id, *kind);
        }

        const Terminator& terminator = block.terminator;
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            visitUse(operand, Use { Use::Kind::TerminatorOperand, blockID, { }, operandIndex, 0 });
        });
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex)
            visitArguments(Use::Kind::EdgeArgument, blockID, terminator.targets[targetIndex], targetIndex);
    }

    void visitArguments(Use::Kind kind, BlockID block, const Target& target, uint32_t targetIndex)
    {
        const Arguments& arguments = target.arguments;
        for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
            if (ValueID argument = arguments[argumentIndex])
                visitUse(argument, Use { kind, block, { }, argumentIndex, targetIndex });
        }
    }

    void visitUse(ValueID used, const Use& use)
    {
        if (CheckID check = activeCheckOf(used, use.block))
            m_rewrites.append({ use, check });
    }

    StringID accessedNameOf(const Value& access) const
    {
        if (!namedAccesses.contains(access.opcode))
            return { };
        const PropertyKey& key = m_program.at(access.key());
        return key.kind == PropertyKeyKind::Text ? key.text : StringID();
    }

    CheckID activate(Check&& check)
    {
        ValueID receiver = check.receiver;
        CheckID id(m_checks.size());
        m_checks.append(WTF::move(check));
        CheckID replacedCheck = std::exchange(m_activeChecks[receiver.index()], id);
        m_checkedReceivers.append(std::pair { receiver, replacedCheck });
        return id;
    }

    void addCheck(ProgramPoint point, ValueID id, NullishAccess kind)
    {
        const Value& access = m_function.at(id);
        CheckID check = activate(Check { point, id, access.operand(receiverOperand), kind, accessedNameOf(access), { } });
        m_rewrites.append({ Use { Use::Kind::Operand, point.block, id, receiverOperand, 0 }, check });
    }

    static Value createCheck(const Check& check)
    {
        Value value = IRBuilder::make(Opcode::RequireObjectCoercible, { check.receiver });
        value.setNullishAccess(check.kind);
        value.setName(check.name);
        return value;
    }

    void insertChecks()
    {
        IRBuilder builder(m_context.analyses.context(), m_function, m_context.types, &m_context.analyses);
        for (Check& check : m_checks) {
            if (check.result)
                continue;
            builder.setOrigin(m_function.at(check.access).origin());
            check.result = builder.insertBefore(check.point, createCheck(check));
        }
        builder.applyInsertions();
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    Vector<Check, 8> m_checks;
    Vector<Rewrite, 16> m_rewrites;
    Vector<CheckID> m_activeChecks;
    Vector<std::pair<ValueID, CheckID>, 8> m_checkedReceivers;
    Vector<size_t, 16> m_scopes;
};

} // anonymous namespace

PassOutcome insertObjectCoercibleChecks(FunctionPassContext& context)
{
    return CheckInsertion(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

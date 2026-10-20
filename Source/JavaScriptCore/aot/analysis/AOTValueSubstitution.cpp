/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueSubstitution.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

void replaceUse(Function& function, const Use& use, ValueID replacement)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        function.at(use.user).setOperand(use.operandIndex, replacement);
        return;
    case Use::Kind::TerminatorOperand:
        function.at(use.block).terminator.setOperand(use.operandIndex, replacement);
        return;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        targetOf(function, use.edge()).arguments[use.operandIndex] = replacement;
        return;
    case Use::Kind::BlockSubject:
        function.at(use.block).subject = replacement;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An allocation is the guard of its own shape, and a cast of it is not.
static bool acceptsCast(OperandKind kind)
{
    return kind != OperandKind::Guard && !mustBeDefinition(kind);
}

bool acceptsCastOfValue(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return acceptsCast(function.at(use.user).operandKind(use.operandIndex));
    case Use::Kind::TerminatorOperand: {
        const Terminator& terminator = function.at(use.block).terminator;
        return terminator.kind != TerminatorKind::ReturnCall && acceptsCast(terminator.operandKind(use.operandIndex));
    }
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return true;
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void becomeUpcast(Function& function, ValueID id, ValueID operand)
{
    Value& value = function.at(id);
    value.become(Opcode::Upcast);
    value.setValueOperand(operand);
    value.setCastType(value.type);
}

void becomeUndefined(Function& function, ValueID id)
{
    Value& value = function.at(id);
    value.become(Opcode::Undefined);
    value.type = TypeTable::undefined;
    value.setWritten(TypeID());
    value.setPrediction(TypeID());
}

ValueSubstitution::ValueSubstitution(const Function& function)
{
    ensureSize(function.values.size());
}

void ValueSubstitution::ensureSize(size_t size)
{
    size_t oldSize = m_replacements.size();
    if (size <= oldSize)
        return;
    m_replacements.grow(size);
    for (size_t i = oldSize; i < size; ++i)
        m_replacements[i] = ValueID(i);
}

void ValueSubstitution::replace(ValueID from, ValueID to)
{
    RELEASE_ASSERT(from && to);
    ensureSize(static_cast<size_t>(std::max(from, to).index()) + 1);
    RELEASE_ASSERT(m_replacements[from.index()] == from);
    ValueID end = resolve(to);
    RELEASE_ASSERT(end != from);
    m_replacements[from.index()] = end;
    ++m_numberOfReplacements;
}

ValueID ValueSubstitution::resolve(ValueID value) const
{
    if (!value || value.index() >= m_replacements.size())
        return value;
    ValueID end = value;
    while (m_replacements[end.index()] != end)
        end = m_replacements[end.index()];
    while (value != end)
        value = std::exchange(m_replacements[value.index()], end);
    return end;
}

void ValueSubstitution::apply(Function& function, FunctionAnalyses* analyses)
{
    RELEASE_ASSERT(!analyses || &analyses->function() == &function);
    if (isEmpty())
        return;

    auto rewrite = [&](ValueID& value) {
        value = resolve(value);
    };
    for (auto& value : function.values)
        value.forEachChild(rewrite);
    for (auto& block : function.blocks)
        block.forEachChildOutsideValues(rewrite);
    if (analyses)
        analyses->didChangeValues();

    m_replacements.shrink(0);
    ensureSize(function.values.size());
    m_numberOfReplacements = 0;
}

void replaceReadWithValue(const Program& program, Function& function, ValueSubstitution& substitution, ValueID read, ValueID replacement)
{
    Value& value = function.at(read);
    if (!program.at(value.type).isJavaScriptValue()) {
        substitution.replace(read, replacement);
        becomeUndefined(function, read);
        return;
    }
    becomeUpcast(function, read, replacement);
}

void keepReadForValidation(IRBuilder& builder, ProgramPoint read, ValueID replacement)
{
    const AnalysisContext& context = builder.context();
    if (!context.build.validates())
        return;
    Value kept = builder.function().at(builder.function().at(read.block).values[read.index]);
    if (!context.program.at(kept.type).isJavaScriptValue())
        return;
    builder.setOrigin(kept.origin());
    ValueID keptRead = builder.insertBefore(read, WTF::move(kept));
    builder.insertBefore(read, IRBuilder::make(Opcode::ValidateSameValue, { keptRead, replacement }));
}

void appendKeptReadForValidation(IRBuilder& builder, BlockValues& values, ValueID movedRead)
{
    const AnalysisContext& context = builder.context();
    if (!context.build.validates())
        return;
    Value kept = builder.function().at(movedRead);
    if (!context.program.at(kept.type).isJavaScriptValue())
        return;
    builder.setOrigin(kept.origin());
    ValueID keptRead = builder.addDetached(WTF::move(kept));
    values.append(keptRead);
    values.append(builder.addDetached(IRBuilder::make(Opcode::ValidateSameValue, { keptRead, movedRead })));
}

BitVector findReadsKeptForValidation(const Function& function)
{
    BitVector keptReads;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode == Opcode::ValidateSameValue)
                keptReads.set(function.at(id).keptOperand().index());
        }
    }
    return keptReads;
}

bool isTestedByValidation(const Function& function, const UseLists& useLists, ValueID id)
{
    return std::ranges::any_of(useLists.usesOf(id), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && familyOf(function.at(use.user).opcode) == OperationFamily::RuntimeValidation;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

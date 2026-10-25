/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueSubstitution.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool hasTypesOf(const Function& function, ValueID replaced, ValueID replacement)
{
    const Value& from = function.at(replaced);
    const Value& to = function.at(replacement);
    if (from.type != to.type || from.expectedForTyping() != to.expectedForTyping() || opcodesWithIndirectUsers.contains(from.opcode) || opcodesWithIndirectUsers.contains(to.opcode))
        return false;
    // The user may still be typed from the type that the replaced value had before.
    const ChangeLog* log = function.changeLog();
    return !log || !log->hasUnpropagatedType(replaced);
}

void replaceUse(Function& function, const Use& use, ValueID replacement)
{
    bool hasSameTypes = hasTypesOf(function, usedValueOf(function, use), replacement);
    auto editsOfBlock = [&](BlockEdit edit) {
        OptionSet<BlockEdit> edits { edit };
        if (hasSameTypes)
            edits.add(BlockEdit::OperandsOfSameTypes);
        return edits;
    };
    switch (use.kind) {
    case Use::Kind::Operand:
        function.edit(use.user, hasSameTypes ? OptionSet<ValueEdit> { ValueEdit::Definition, ValueEdit::OperandsOfSameTypes } : OptionSet<ValueEdit> { ValueEdit::Definition }).setOperand(use.operandIndex, replacement);
        return;
    case Use::Kind::TerminatorOperand:
        function.edit(use.block, editsOfBlock(BlockEdit::Terminator)).terminator.setOperand(use.operandIndex, replacement);
        return;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        editTarget(function, use.edge(), editsOfBlock(BlockEdit::Arguments)).arguments[use.operandIndex] = replacement;
        return;
    case Use::Kind::BlockSubject:
        function.edit(use.block, BlockEdit::Rest).subject = replacement;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
    Value& value = function.edit(id, ValueEdit::Definition);
    value.become(Opcode::Upcast);
    value.setValueOperand(operand);
    value.setCastType(value.type);
}

void becomeUndefined(Function& function, ValueID id)
{
    Value& value = function.edit(id);
    value.become(Opcode::Undefined);
    value.type = TypeTable::undefined;
    value.setExpected(TypeID());
}

void ValueSubstitution::replace(ValueID from, ValueID to)
{
    RELEASE_ASSERT(from && to);
    size_t oldSize = m_replacements.size();
    if (size_t size = std::max(m_numberOfValues, static_cast<size_t>(std::max(from, to).index()) + 1); size > oldSize) {
        m_replacements.grow(size);
        for (size_t index = oldSize; index < size; ++index)
            m_replacements[index] = ValueID(index);
    }
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
    bool namesReplacedValue = false;
    bool getsOtherTypes = false;
    std::span<const ValueID> replacements = m_replacements.span();
    auto look = [&](ValueID value) ALWAYS_INLINE_LAMBDA {
        if (value.index() >= replacements.size() || replacements[value.index()] == value) [[likely]]
            return;
        namesReplacedValue = true;
        getsOtherTypes |= !hasTypesOf(function, value, resolve(value));
    };
    uint32_t index = 0;
    for (const Value& value : function.values) {
        namesReplacedValue = false;
        getsOtherTypes = false;
        value.forEachChild(look);
        if (namesReplacedValue) [[unlikely]]
            function.edit(ValueID(index), getsOtherTypes ? OptionSet<ValueEdit> { ValueEdit::Definition } : OptionSet<ValueEdit> { ValueEdit::Definition, ValueEdit::OperandsOfSameTypes }).forEachChild(rewrite);
        ++index;
    }
    index = 0;
    for (const Block& block : function.blocks) {
        namesReplacedValue = false;
        getsOtherTypes = false;
        block.forEachChildOutsideValues(look);
        if (namesReplacedValue) [[unlikely]] {
            OptionSet<BlockEdit> edits { BlockEdit::Terminator, BlockEdit::Arguments, BlockEdit::Rest };
            if (!getsOtherTypes)
                edits.add(BlockEdit::OperandsOfSameTypes);
            function.edit(BlockID(index), edits).forEachChildOutsideValues(rewrite);
        }
        ++index;
    }
    if (analyses) {
        for (size_t index = 0; index < m_replacements.size(); ++index) {
            if (ValueID from(index); m_replacements[index] != from)
                analyses->didReplaceUses(from, resolve(from));
        }
    }

    m_replacements.shrink(0);
    m_numberOfValues = function.values.size();
    m_numberOfReplacements = 0;
}

void replaceReadWithValue(const Program& program, Function& function, ValueSubstitution& substitution, ValueID read, ValueID replacement)
{
    const Value& value = function.at(read);
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

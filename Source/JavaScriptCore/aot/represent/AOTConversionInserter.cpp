/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConversions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTFunctionAnalyses.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <utility>
#include <wtf/StringPrintStream.h>

#if ASSERT_ENABLED
#include "AOTIRPrinter.h"
#endif

namespace JSC { namespace AOT { namespace Typed {

static std::optional<Value> makeOnlyValueOf(const Type& type)
{
    if (type.isInteger() && type.integers.isSingleValue()) {
        Value constant = IRBuilder::make(Opcode::Integer);
        constant.setInteger(type.integers.minimum);
        return constant;
    }
    if (type.isBoolean() && type.isSingleValue()) {
        Value constant = IRBuilder::make(Opcode::Boolean);
        constant.setBoolean(type.primitives & Primitive::True);
        return constant;
    }
    return std::nullopt;
}

ConversionInserter::ConversionInserter(FunctionPassContext& pass, IRBuilder& builder)
    : m_pass(pass)
    , m_builder(builder)
{
    RELEASE_ASSERT(&builder.function() == &pass.function);
}

void ConversionInserter::convertOperand(ValueID user, uint32_t operandIndex, Representation wanted, ProgramPoint pointOfUser)
{
    Function& function = m_pass.function;
    m_builder.setOrigin(function.at(user).origin());
    ValueID converted = convert(function.at(user).operand(operandIndex), wanted, pointOfUser);
    function.edit(user, ValueEdit::Definition).setOperand(operandIndex, converted);
}

void ConversionInserter::convertTerminatorOperand(BlockID block, uint32_t operandIndex, Representation wanted)
{
    Function& function = m_pass.function;
    Terminator& terminator = function.edit(block, BlockEdit::Terminator).terminator;
    RELEASE_ASSERT(terminator.kind != TerminatorKind::TestAndBranch && terminator.kind != TerminatorKind::ReturnCall);
    m_builder.setOrigin(terminator.origin());
    ValueID converted = convert(terminator.operand(operandIndex), wanted, pointBeforeTerminatorOf(function, block));
    terminator.setOperand(operandIndex, converted);
}

void ConversionInserter::finish()
{
    m_builder.applyInsertions();
}

ValueID ConversionInserter::convert(ValueID source, Representation wanted, ProgramPoint before)
{
    Function& function = m_pass.function;
    Representation from = function.at(source).representation;
    if (from == wanted)
        return source;

    const Type& type = m_pass.program.typeOf(function, source);
    if (std::optional<Value> constant = makeOnlyValueOf(type); constant && isLegalRepresentation(type, wanted)) {
        ValueID made = m_builder.insertBefore(before, WTF::move(*constant));
        function.edit(made, ValueEdit::Type).representation = wanted;
        ++m_numberOfConversions;
        return made;
    }

    std::optional<ConversionPath> path = findConversion(type, from, wanted);
    if (!path) {
        reportUnsupportedConversion(source, wanted, before.block);
        return source;
    }

    ValueID current = source;
    for (ConversionStep step : *path) {
        current = m_builder.insertBefore(before, IRBuilder::make(step.opcode, { current }));
        function.edit(current, ValueEdit::Type).representation = step.to;
        ++m_numberOfConversions;
    }
    return current;
}

void ConversionInserter::reportUnsupportedConversion(ValueID source, Representation wanted, BlockID block)
{
    StringPrintStream detail;
    detail.print("a value"_s);
#if ASSERT_ENABLED
    detail.print(" of type "_s);
    printType(detail, m_pass.program, m_pass.function.at(source).type);
#endif
    detail.print(" kept as "_s, nameOf(m_pass.function.at(source).representation), " is wanted as "_s, nameOf(wanted));
    m_pass.build.diagnostics().add(InternalError { .kind = InternalErrorKind::UnsupportedConversion, .function = m_pass.id, .block = block, .value = source, .detail = detail.toString() });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

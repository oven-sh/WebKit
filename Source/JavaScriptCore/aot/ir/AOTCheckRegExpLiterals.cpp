/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTStringArena.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include "YarrErrorCode.h"
#include "YarrFlags.h"
#include <array>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static const RegExpSummary* summarizeLiteral(FunctionPassContext& context, const Value& literal, Vector<char16_t>& patternBuffer)
{
    OptionSet<Yarr::Flags> flags = toYarrFlags(literal.regExpFlags());
    Yarr::FlagsString flagCharacters = Yarr::flagsString(flags);
    if (!Yarr::parseFlags(StringView { flagCharacters.span() }))
        return nullptr;
    StringView pattern = context.program.strings.viewOf(literal.pattern(), patternBuffer);
    const RegExpSummary& summary = context.build.regExpSummaries().summaryOf(literal.pattern(), flags, pattern);
    return summary.isValid() ? &summary : nullptr;
}

static void emitThrowSyntaxError(IRBuilder& builder, StringID message)
{
    Value constructor = IRBuilder::make(Opcode::Builtin);
    constructor.setBuiltin(Builtins::SyntaxError);
    Value text = IRBuilder::make(Opcode::String);
    text.setString(message);
    Value error = IRBuilder::make(Opcode::Construct);
    error.setCalleeOperand(builder.append(WTF::move(constructor)));
    error.appendVariadicOperand(builder.append(WTF::move(text)));
    builder.emitThrow(builder.append(WTF::move(error)));
}

// How deep a pattern can be nested depends on the stack of the thread that builds it, so the literal is created by a call of RegExp where it is evaluated.
static void replaceWithConstructorCall(FunctionPassContext& context, IRBuilder& builder, ProgramPoint point)
{
    Function& function = context.function;
    ValueID literal = function.at(point.block).values[point.index];
    StringID pattern = function.at(literal).pattern();
    Yarr::FlagsString flagCharacters = Yarr::flagsString(toYarrFlags(function.at(literal).regExpFlags()));

    BlockID creation = builder.splitBlock(point.block, std::array { point.index, point.index + 1 }).first();
    builder.setHandler(function.at(point.block).handler);
    BlockID failed = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
    builder.setHandler(std::nullopt);
    function.edit(creation, { BlockEdit::Edges, BlockEdit::Arguments }).handler = Target { failed, { }, { } };

    Value constructor = IRBuilder::make(Opcode::Builtin);
    constructor.setBuiltin(Builtins::RegExp);
    Value patternText = IRBuilder::make(Opcode::String);
    patternText.setString(pattern);
    Value flagsText = IRBuilder::make(Opcode::String);
    flagsText.setString(context.strings.intern(StringView { flagCharacters.span() }));
    Value call = IRBuilder::make(Opcode::Construct);
    call.setOrigin(builder.origin());
    call.setCalleeOperand(builder.insertBefore(ProgramPoint::atHeadOf(creation), WTF::move(constructor)));
    call.appendVariadicOperand(builder.insertBefore(ProgramPoint::atHeadOf(creation), WTF::move(patternText)));
    call.appendVariadicOperand(builder.insertBefore(ProgramPoint::atHeadOf(creation), WTF::move(flagsText)));
    builder.applyInsertions();
    function.edit(literal) = WTF::move(call);

    builder.moveToEndOf(failed);
    builder.append(Opcode::CatchValue, { });
    emitThrowSyntaxError(builder, context.strings.intern(StringView { Yarr::errorMessage(Yarr::ErrorCode::TooManyDisjunctions) }));
}

// NewRegExp does not throw: the run time creates the object from what the build knows about the pattern. A literal with valid syntax whose pattern cannot be built throws a SyntaxError
// where it is evaluated.
PassOutcome checkRegExpLiterals(FunctionPassContext& context)
{
    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<char16_t> patternBuffer;
    bool didChange = false;
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (uint32_t index = 0; index < function.at(block).values.size(); ++index) {
            ValueID id = function.at(block).values[index];
            const Value& value = function.at(id);
            if (value.opcode != Opcode::NewRegExp)
                continue;
            const RegExpSummary* summary = summarizeLiteral(context, value, patternBuffer);
            if (!summary) {
                context.build.diagnostics().add(UserDiagnostic {
                    .kind = UserDiagnosticKind::InvalidRegExpLiteral,
                    .function = context.id,
                    .block = block,
                    .value = id,
                    .location = function.locationOf(id),
                });
                continue;
            }
            if (summary->canBeCreated())
                continue;
            didChange = true;
            builder.setOrigin(value.origin());
            // What stands behind the literal is in a new block, which the loop comes to.
            if (!Yarr::hasError(summary->constructionError)) {
                replaceWithConstructorCall(context, builder, { block, index });
                break;
            }
            Block& cut = function.edit(block);
            cut.values.shrink(index);
            cut.terminator.become(TerminatorKind::Unreachable);
            builder.moveToEndOf(block);
            emitThrowSyntaxError(builder, context.strings.intern(StringView { Yarr::errorMessage(summary->constructionError) }));
            break;
        }
    }
    if (!didChange)
        return PassOutcome::Unchanged;
    compactFunction(function);
    context.analyses.didChangeControlFlow();
    typeValuesFromNever(TypePropagationInputs { context.program, context.id, function, context.analyses, context.types, context.build, &context.typingMemo });
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTValidationPlanText.h"

namespace JSC { namespace AOT { namespace Typed {

static void endExpansionWithResult(IRBuilder& builder, const IRBuilder::Expansion& expansion, ValueID result)
{
    DeclaredType replacedType { .type = builder.function().at(expansion.results[0]).type };
    builder.endExpansion(expansion, convertTo(builder, result, replacedType));
}

static void recomputeTypes(const TypingContext& typing, Value& value)
{
    value.type = resultTypeOf(typing.program, typing.function, value, typing.types);
    value.setWritten(writtenTypeOf(typing.program, typing.function, value, typing.types));
    value.setPrediction(predictionOf(typing.program, typing.function, value, typing.types));
}

static std::optional<Value> tryMakeBuiltinCall(const TypingContext& typing, BuiltinID callee, const BuiltinCallOperands& operands)
{
    BuiltinOperandTypes types = operandTypesOf(typing, operands.receiver, operands.arguments.span());
    OverloadMatch match = matchOverload(OverloadQuery { typing, callee, BuiltinCallMode::Call, types, types });
    if (!match.overload)
        return std::nullopt;

    Value call = makeBuiltinCall(match.overload, BuiltinCallForm::Whole, operands, { });
    bool isMatched = match.state == BuiltinMatchState::Proven && match.requirements.isEmpty() && overloadTakesOperands(typing.program, typing.function, call);
    if (!isMatched)
        call.setBuiltinCallForm(BuiltinCallForm::AnyReceiver);
    return call;
}

bool canCallBodyBytesForm(const TypingContext& typing, BuiltinID bodyBytesForm, ValueID body)
{
    return tryMakeBuiltinCall(typing, bodyBytesForm, { body, { } }).has_value();
}

static BuiltinID bodyBytesFormOf(const BuiltinTable& builtins, const Value& bodyCall)
{
    const BuiltinForms* forms = builtins.formsOf(builtins.canonicalID(builtins.at(bodyCall.overload()).builtin));
    RELEASE_ASSERT(forms && forms->bodyBytesForm);
    return forms->bodyBytesForm;
}

void TypedResultExpander::expand(const TypedResultSite& site)
{
    m_originalCall = builder().function().at(site.call);
    switch (site.form) {
    case TypedResultForm::ParsedText:
        expandParsedText(site);
        return;
    case TypedResultForm::ParsedBody:
        if (site.root.isValidated())
            expandValidatedCall(site);
        else
            expandParsedBody(site);
        return;
    case TypedResultForm::Rows:
        if (site.root.isValidated())
            expandValidatedCall(site);
        else
            expandRows(site);
        return;
    case TypedResultForm::ValidatedValue:
        expandValidatedValue(site);
        return;
    case TypedResultForm::None:
        expandValidatedCall(site);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID TypedResultExpander::appendOriginalCall(const TypedResultSite&)
{
    Value call = m_originalCall;
    call.setExpectedResult({ });
    return builder().append(WTF::move(call));
}

ValueID TypedResultExpander::emitParseOf(const TypedResultSite& site, ValueID textOrBytes, BlockID generic)
{
    ValueID open = builder().append(Opcode::JSONSourceOpen, { textOrBytes });
    IRBuilder::ProvenBranch opened = builder().emitBranchWithProof(open, 0, BlockFrequency::Normal, BlockFrequency::Rare);
    builder().moveToEndOf(opened.notTaken);
    builder().emitJump(generic);
    builder().moveToEndOf(opened.taken);
    ValueID source = opened.provenOnTaken;

    ValueID start = builder().append(Opcode::JSONSourceStart, { source });
    ValueID first = builder().append(Opcode::JSONSkipWhitespace, { start });
    ValueID results = appendCallOf(inputs().functions.ensure(parserKeyOf(site.root)), { first });
    ValueID outcome = appendResultAt(results, 0);
    ValueID value = appendResultAt(results, 1);
    ValueID next = appendResultAt(results, 2);

    BlockID parsed = builder().addBlock();
    BlockID abandon = builder().addBlock(BlockFrequency::Rare);
    emitBranchOnOutcome(outcome, parsed, abandon, abandon);

    builder().moveToEndOf(abandon);
    builder().append(Opcode::JSONSourceAbandon, { source });
    builder().emitJump(generic);

    builder().moveToEndOf(parsed);
    emitJumpUnless(builder().append(Opcode::JSONSourceClose, { source, next }), generic);
    return value;
}

void TypedResultExpander::expandParsedText(const TypedResultSite& site)
{
    IRBuilder::Expansion expansion = builder().beginExpansion({ site.block, site.index });
    BlockID generic = builder().addBlock(BlockFrequency::Rare);
    endExpansionWithResult(builder(), expansion, emitParseOf(site, site.source, generic));

    builder().moveToEndOf(generic);
    ValueID value = appendOriginalCall(site);
    if (site.root.isValidated())
        value = emitGeneralValidation(site, value);
    endExpansionWithResult(builder(), expansion, value);
}

static uint32_t awaitedOperandIndexOf(const Value& await)
{
    if (await.opcode == Opcode::Await)
        return OperandIndex::Await::value;
    RELEASE_ASSERT(await.opcode == Opcode::AwaitBySavingFrame);
    return OperandIndex::AwaitBySavingFrame::awaited;
}

// What the casts between the call and its await say holds for the promise of json(), not for the promise of bytes().
static void removeCastsOfAwaitedPromise(Function& function, const TypedResultSite& site)
{
    Value& await = function.at(site.await);
    uint32_t awaitedIndex = awaitedOperandIndexOf(await);

    BitVector casts;
    for (ValueID promise = await.operand(awaitedIndex); promise != site.call; promise = function.at(promise).operand(0)) {
        RELEASE_ASSERT(isCast(function.at(promise).opcode));
        casts.set(promise.index());
    }

    await.setOperand(awaitedIndex, site.call);
    function.at(site.block).values.removeAllMatching([&](ValueID id) {
        return casts.get(id.index());
    });
}

void TypedResultExpander::expandParsedBody(const TypedResultSite& site)
{
    Function& function = builder().function();
    TypingContext typing { program(), function, inputs().types };
    std::optional<Value> bytesCall = tryMakeBuiltinCall(typing, bodyBytesFormOf(program().builtins(), m_originalCall), { site.source, { } });
    RELEASE_ASSERT(bytesCall);

    bytesCall->setOrigin(m_originalCall.origin());
    function.at(site.call) = WTF::move(*bytesCall);
    recomputeTypes(typing, function.at(site.call));
    removeCastsOfAwaitedPromise(function, site);
    m_pass.analyses.didChangeValues();

    size_t awaitIndex = function.at(site.block).values.find(site.await);
    RELEASE_ASSERT(awaitIndex != notFound);
    Value await = function.at(site.await);
    await.setWritten({ });
    await.setPrediction({ });

    IRBuilder::Expansion expansion = builder().beginExpansion({ site.block, static_cast<uint32_t>(awaitIndex) });
    BlockID generic = builder().addBlock(BlockFrequency::Rare);
    ValueID bytes = builder().append(WTF::move(await));
    endExpansionWithResult(builder(), expansion, emitParseOf(site, bytes, generic));

    builder().moveToEndOf(generic);
    ValueID text = builder().append(Opcode::JSONDecodeBytes, { bytes });
    std::optional<Value> parseCall = tryMakeBuiltinCall(typing, Builtins::JSONParse, { { }, { text } });
    RELEASE_ASSERT(parseCall);
    endExpansionWithResult(builder(), expansion, builder().append(WTF::move(*parseCall)));
}

void TypedResultExpander::expandRows(const TypedResultSite& site)
{
    Value& call = builder().function().at(site.call);
    call.setExpectedResult({ });
    builder().setOrigin(call.origin());

    Value expectation = IRBuilder::make(Opcode::ExpectRows, { site.call });
    expectation.setLayout(site.rowLayout);
    builder().insertBefore({ site.block, site.index + 1 }, WTF::move(expectation));
    builder().applyInsertions();
}

void TypedResultExpander::expandValidatedValue(const TypedResultSite& site)
{
    IRBuilder::Expansion expansion = builder().beginExpansion({ site.block, site.index });
    endExpansionWithResult(builder(), expansion, emitValidationOf(site, site.source));
}

void TypedResultExpander::expandValidatedCall(const TypedResultSite& site)
{
    const Terminator& terminator = builder().function().at(site.block).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == site.call) {
        builder().demoteReturnCall(site.block);
        m_originalCall = builder().function().at(site.call);
    }

    IRBuilder::Expansion expansion = builder().beginExpansion({ site.block, site.index });
    endExpansionWithResult(builder(), expansion, emitValidationOf(site, appendOriginalCall(site)));
}

ValueID TypedResultExpander::emitValidationOf(const TypedResultSite& site, ValueID value)
{
    NodeDecisionInputs decisionInputs { inputs().program, inputs().flow, inputs().build };
    ExpectedResultID expectedResult = site.root.expectedResult();
    for (size_t index : nodesReachableFrom(program(), expectedResult, site.root.part)) {
        ResultNodeRef node = ResultNodeRef::forValidation(expectedResult, static_cast<uint32_t>(index));
        NodeDecision decision = decideNodeCode(decisionInputs, node, NodeUse::CopiedFromValue);
        if (decision.code != NodeCode::Interpreted)
            continue;
        addInterpretedNodeDecline(decisionInputs, firstLayoutOf(program(), node), decision.cause);
        return emitGeneralValidation(site, value);
    }

    ValueID results = appendCallOf(inputs().functions.ensure(validatorKeyOf(site.root)), { value });
    ValueID outcome = appendResultAt(results, 0);
    ValueID copy = appendResultAt(results, 1);
    DeclaredType validatedType { .type = builder().function().at(copy).type };

    BlockID accepted = builder().addBlock();
    BlockID general = builder().addBlock(BlockFrequency::Rare);
    BlockID merge = builder().addBlock();
    ValueID validated = builder().addParameter(merge, validatedType.type);
    emitBranchOnOutcome(outcome, accepted, general, general);

    builder().moveToEndOf(accepted);
    builder().emitJump(merge, { copy });

    builder().moveToEndOf(general);
    ValueID generalCopy = convertTo(builder(), emitGeneralValidation(site, value), validatedType);
    builder().emitJump(merge, { generalCopy });

    builder().moveToEndOf(merge);
    return validated;
}

ValueID TypedResultExpander::emitGeneralValidation(const TypedResultSite& site, ValueID value)
{
    StringID plan = validationPlanText(program(), inputs().strings, site.root.expectedResult(), site.root.part);
    return appendHelperCall(Builtins::CompilerHelpersValidateByPlan, { value, appendString(plan) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

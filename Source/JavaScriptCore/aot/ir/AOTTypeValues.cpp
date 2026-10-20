/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Resolution {
    enum class Cast : uint8_t { Nothing, Upcast, Claim };

    Cast cast { Cast::Nothing };
    TypeID castType;
    TypeID reported;
    TypeID predicted;
    TypeSiteID site;
    CheckCause cause { CheckCause::Unknown };
};

struct RequestContext {
    TypingContext typing;
    MismatchPolicy policy { MismatchPolicy::Throw };
};

struct RequestEdits {
    explicit RequestEdits(FunctionPassContext& context)
        : builder(context.analyses.context(), context.function, context.types, &context.analyses)
    {
    }

    IRBuilder builder;
    std::optional<ValueSubstitution> substitution;
    BitVector erasedValues;
    Vector<BlockID, 8> blocksWithErasedValues;
};

} // anonymous namespace

static Resolution::Cast resolveCast(const TypingContext& typing, const Type& source, const Type& target, const Type& cast)
{
    if (!source.isSubsetOf(target, typing.program))
        return Resolution::Cast::Claim;
    return cast == source ? Resolution::Cast::Nothing : Resolution::Cast::Upcast;
}

static TypeID resolvePrediction(const TypingContext& typing, const Type& proven, TypeID expected)
{
    Type met = proven.castTo(typing.types.at(expected), typing.types);
    if (met.isNever() || met == proven)
        return { };
    return expected;
}

static TypeID resolveReport(const TypingContext& typing, const Type& value, TypeID written)
{
    Type expected = typing.types.at(written).withoutProofs();
    if (value.isSubsetOf(expected, typing.program))
        return { };
    return typing.types.intern(expected);
}

// In a build that reports, the written types say what the types would be in one that throws: a value inside them has been reported where it entered them.
static Resolution resolveClaimDeclared(const RequestContext& context, const Type& source, const Type& writtenSource, const DeclaredType& declared)
{
    const TypingContext& typing = context.typing;
    const Type& target = typing.types.at(declared.type);
    Type cast = source.castTo(target, typing.types);

    Resolution resolution;
    resolution.cast = resolveCast(typing, source, target, cast);
    resolution.castType = declared.type;
    resolution.site = declared.site;
    resolution.cause = CheckCause::MayFail;
    if (context.policy == MismatchPolicy::Report && declared.written)
        resolution.reported = resolveReport(typing, writtenSource.castTo(target, typing.types), declared.written);
    TypeID expected = typing.predicting().typeIDOf(declared);
    if (expected != declared.type)
        resolution.predicted = resolvePrediction(typing, cast, expected);
    return resolution;
}

static CheckCause causeOf(ClaimSource::Kind kind)
{
    switch (kind) {
    case ClaimSource::Kind::None:
        break;
    case ClaimSource::Kind::Field:
        return CheckCause::UnknownObject;
    case ClaimSource::Kind::Contents:
        return CheckCause::UnprovenContents;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Resolution resolveClaimRead(const RequestContext& context, const Value& request)
{
    const TypingContext& typing = context.typing;
    RequestBound bound = boundOfClaimRead(typing, request);
    const Type& source = typing.typeOf(request.valueOperand());

    Resolution resolution;
    switch (bound.source) {
    case RequestBound::Source::NotFound:
        return resolution;
    case RequestBound::Source::Prediction:
        resolution.predicted = resolvePrediction(typing, source, bound.type);
        return resolution;
    case RequestBound::Source::Written:
    case RequestBound::Source::Type:
        break;
    }

    resolution.site = request.site();
    if (context.policy == MismatchPolicy::Report) {
        resolution.predicted = resolvePrediction(typing, source, bound.type);
        resolution.reported = resolveReport(typing, source, bound.type);
        return resolution;
    }

    Type target = typing.types.at(bound.type).withoutProofs();
    Type cast = source.castTo(target, typing.types);
    resolution.cast = resolveCast(typing, source, target, cast);
    resolution.castType = typing.types.intern(target);
    resolution.cause = causeOf(request.source().kind());
    if (resolution.castType != bound.type)
        resolution.predicted = resolvePrediction(typing, cast, bound.type);
    return resolution;
}

static Resolution resolveRequest(const RequestContext& context, const Value& request)
{
    if (request.opcode == Opcode::ClaimRead)
        return resolveClaimRead(context, request);
    ValueID operand = request.valueOperand();
    return resolveClaimDeclared(context, context.typing.typeOf(operand), context.typing.atLevel(TypeLevel::Written).typeOf(operand), request.declared());
}

static StatisticID statisticOf(Resolution::Cast cast)
{
    switch (cast) {
    case Resolution::Cast::Nothing:
        return StatisticID::RequestsResolvedToNothing;
    case Resolution::Cast::Upcast:
        return StatisticID::RequestsResolvedToUpcast;
    case Resolution::Cast::Claim:
        return StatisticID::RequestsResolvedToClaim;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void becomeCast(Value& value, ValueID operand, const Resolution& resolution)
{
    switch (resolution.cast) {
    case Resolution::Cast::Nothing:
        RELEASE_ASSERT_NOT_REACHED();
    case Resolution::Cast::Upcast:
        value.become(Opcode::Upcast);
        break;
    case Resolution::Cast::Claim:
        value.become(Opcode::Claim);
        value.setSite(resolution.site);
        value.setCause(resolution.cause);
        break;
    }
    value.setValueOperand(operand);
    value.setCastType(resolution.castType);
}

static void insertReport(FunctionPassContext& context, IRBuilder& builder, ProgramPoint point, ValueID operand, const Resolution& resolution)
{
    Value report = IRBuilder::make(Opcode::ReportMismatch, { operand });
    report.setExpectedType(resolution.reported);
    report.setSite(resolution.site);
    builder.insertBefore(point, WTF::move(report));
    context.build.statistics().count(StatisticID::RequestsResolvedToReport);
}

static void replaceWithOperand(FunctionPassContext& context, RequestEdits& edits, BlockID block, ValueID request)
{
    if (!edits.substitution)
        edits.substitution.emplace(context.function);
    edits.substitution->replace(request, context.function.at(request).valueOperand());
    edits.erasedValues.set(request.index());
    if (edits.blocksWithErasedValues.isEmpty() || edits.blocksWithErasedValues.last() != block)
        edits.blocksWithErasedValues.append(block);
}

static void applyResolution(FunctionPassContext& context, RequestEdits& edits, ProgramPoint point, const Resolution& resolution)
{
    Function& function = context.function;
    Statistics& statistics = context.build.statistics();
    ValueID id = function.at(point.block).values[point.index];
    ValueID operand = function.at(id).valueOperand();
    bool hasCast = resolution.cast != Resolution::Cast::Nothing;

    if (resolution.reported) {
        edits.builder.setOrigin(function.at(id).origin());
        insertReport(context, edits.builder, point, operand, resolution);
    }
    if (hasCast || (!resolution.predicted && !resolution.reported))
        statistics.count(statisticOf(resolution.cast));
    if (!resolution.predicted) {
        if (hasCast)
            becomeCast(function.at(id), operand, resolution);
        else
            replaceWithOperand(context, edits, point.block, id);
        return;
    }

    statistics.count(StatisticID::RequestsResolvedToPrediction);
    if (hasCast) {
        Value cast;
        becomeCast(cast, operand, resolution);
        edits.builder.setOrigin(function.at(id).origin());
        operand = edits.builder.insertBefore(point, WTF::move(cast));
    }
    Value& request = function.at(id);
    request.become(Opcode::Predict);
    request.setValueOperand(operand);
    request.setPredicted(resolution.predicted);
}

static void applyEdits(FunctionPassContext& context, RequestEdits& edits)
{
    Function& function = context.function;
    edits.builder.applyInsertions();
    if (!edits.substitution)
        return;

    for (BlockID block : edits.blocksWithErasedValues) {
        const BlockValues& values = function.at(block).values;
        BitVector indexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (edits.erasedValues.get(values[index].index()))
                indexes.set(index);
        }
        edits.builder.eraseValues(block, indexes);
    }
    edits.substitution->apply(function, &context.analyses);
    compactFunction(function);
    context.analyses.didChangeControlFlow();
}

static bool isResolvedByTypeValues(const Value& value)
{
    if (value.opcode == Opcode::ClaimRead)
        return true;
    return value.opcode == Opcode::ClaimDeclared && !value.declared().flags.contains(PositionFlags::IsInt32Promise);
}

static Vector<ProgramPoint, 16> collectRequests(const Function& function)
{
    Vector<ProgramPoint, 16> requests;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (isResolvedByTypeValues(function.at(values[index])))
                requests.append(ProgramPoint { BlockID(blockIndex), index });
        }
    }
    return requests;
}

static bool isTestedOnEntry(EntryParameter kind)
{
    switch (kind) {
    case EntryParameter::This:
    case EntryParameter::Parameter:
        return true;
    case EntryParameter::Capture:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// No caller converts an argument to a type that promises nothing, so the function reports what enters it.
static void reportEntryParameters(FunctionPassContext& context, const RequestContext& requestContext, RequestEdits& edits)
{
    const Function& function = context.function;
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    unsigned count = std::min<unsigned>(function.numberOfEntryParameters(), parameters.size());
    for (unsigned index = 0; index < count; ++index) {
        if (!isTestedOnEntry(function.entryParameterKind(index)))
            continue;
        const DeclaredType& declared = *function.declaredEntryParameter(index);
        if (!declared.site)
            continue;
        const Type& entered = requestContext.typing.typeOf(parameters[index]);
        Resolution resolution = resolveClaimDeclared(requestContext, entered, entered, declared);
        if (!resolution.reported)
            continue;
        edits.builder.setOrigin(Origin { context.program.at(declared.site).position, 0 });
        insertReport(context, edits.builder, ProgramPoint::atHeadOf(entryBlock), parameters[index], resolution);
    }
}

static void resolveRequests(FunctionPassContext& context, MismatchPolicy policy)
{
    const Function& function = context.function;
    Vector<ProgramPoint, 16> requests = collectRequests(function);
    if (requests.isEmpty() && policy == MismatchPolicy::Throw)
        return;

    RequestContext requestContext { { context.program, function, context.types }, policy };
    RequestEdits edits(context);
    if (policy == MismatchPolicy::Report)
        reportEntryParameters(context, requestContext, edits);
    for (ProgramPoint point : requests) {
        Resolution resolution = resolveRequest(requestContext, function.at(function.at(point.block).values[point.index]));
        applyResolution(context, edits, point, resolution);
    }
    applyEdits(context, edits);
}

static CastResolution tailCastResolutionOf(const Resolution& resolution)
{
    switch (resolution.cast) {
    case Resolution::Cast::Nothing:
        return resolution.predicted ? CastResolution::Prediction : CastResolution::Nothing;
    case Resolution::Cast::Upcast:
        return CastResolution::Upcast;
    case Resolution::Cast::Claim:
        return CastResolution::Claim;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A report runs after the call, which a tail call has no frame for.
static void demoteReportedTailCall(FunctionPassContext& context, BlockID block, std::span<const Resolution> reports)
{
    const Terminator& terminator = context.function.at(block).terminator;
    ValueID call = terminator.callOperand();
    Origin origin = terminator.origin();

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    builder.demoteReturnCall(block);
    builder.setOrigin(origin);
    for (const Resolution& report : reports)
        insertReport(context, builder, pointBeforeTerminatorOf(context.function, block), call, report);
    builder.applyInsertions();
    context.build.statistics().count(StatisticID::ReturnCallsDemoted);
}

static void resolveTailCasts(FunctionPassContext& context, MismatchPolicy policy)
{
    Function& function = context.function;
    RequestContext requestContext { { context.program, function, context.types }, policy };
    const TypingContext& typing = requestContext.typing;
    TypingContext written = typing.atLevel(TypeLevel::Written);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        Type source = typing.typeOf(terminator.callOperand());
        Type writtenSource = written.typeOf(terminator.callOperand());
        Vector<TailCast, 4> casts;
        Vector<Resolution, 2> reports;
        for (TailCast cast : terminator.casts()) {
            Resolution resolution = resolveClaimDeclared(requestContext, source, writtenSource, cast.declared);
            cast.resolution = tailCastResolutionOf(resolution);
            if (resolution.reported)
                reports.append(resolution);
            source = source.castTo(typing.types.at(cast.declared.type), typing.types);
            writtenSource = writtenSource.castTo(written.typeOf(cast.declared), typing.types);
            casts.append(cast);
        }
        terminator.setCasts(casts.span());
        if (!reports.isEmpty())
            demoteReportedTailCall(context, block, reports.span());
    }
}

PassOutcome typeValues(FunctionPassContext& context)
{
    if (!context.function.hasBody())
        return PassOutcome::Unchanged;

    MismatchPolicy policy = context.build.options().recorded.policy();
    TypePropagationInputs inputs { context.program, context.id, context.function, context.analyses, context.types, context.build };
    if (hasTypedValues(context.function))
        finishTypedValues(inputs);
    else
        typeValuesFromNever(inputs);
    resolveRequests(context, policy);
    resolveTailCasts(context, policy);
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

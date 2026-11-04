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
#include "AOTInt32Proof.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RequestCounts {
    unsigned erased { 0 };
    unsigned kept { 0 };

    bool isEmpty() const { return !erased && !kept; }
};

struct Flow {
    DeclaredType position;
    Wire::SourceLocation location;
    bool isConstant { false };
};

} // anonymous namespace

static bool isPromise(const DeclaredType& declared)
{
    return declared.has(PositionFlags::IsInt32Promise);
}

static bool hasOnlyInt32sAmongNumbers(const Type& type)
{
    return !(type.primitives & Primitive::Double) && type.integers.isSubsetOf(Range::int32());
}

static bool isNumberConstant(const Function& function, ValueID value)
{
    Opcode opcode = function.at(sameObjectThroughCasts(function, value)).opcode;
    return opcode == Opcode::Integer || opcode == Opcode::Double;
}

static Int32ProofFailure failureOf(const NumberBounds& bounds)
{
    switch (bounds.kind) {
    case BoundsKind::Unknown:
        return bounds.failure;
    case BoundsKind::None:
        return Int32ProofFailure::Unknown;
    case BoundsKind::Integers:
        return Int32ProofFailure::OutOfRange;
    case BoundsKind::Reals:
        return Int32ProofFailure::Quotient;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportUnprovenFlow(FunctionPassContext& context, const Flow& flow, const NumberBounds& bounds)
{
    bool isSingleInteger = bounds.kind == BoundsKind::Integers && bounds.range.isSingleValue();
    UserDiagnosticKind kind = flow.isConstant || isSingleInteger ? UserDiagnosticKind::ConstantNotInt32 : UserDiagnosticKind::UnprovenInt32;
    if (flow.position.has(PositionFlags::IsDeclaredByDependency))
        kind = UserDiagnosticKind::Int32OfDependencyNotProven;
    UserDiagnostic diagnostic {
        .kind = kind,
        .function = context.id,
        .count = static_cast<uint32_t>(failureOf(bounds)),
        .location = flow.location,
    };
    if (flow.position.site) {
        const TypeSite& site = context.program.at(flow.position.site);
        diagnostic.origin = { site.module.index(), site.position };
        diagnostic.siteKind = site.kind;
    }
    if (bounds.kind == BoundsKind::Integers) {
        diagnostic.minimum = static_cast<double>(bounds.range.minimum);
        diagnostic.maximum = static_cast<double>(bounds.range.maximum);
    }
    context.build.diagnostics().add(diagnostic);
}

enum class Judge : uint8_t {
    FlowsThatTypesDoNotProve,
    FlowsThatTypesProve,
};

static bool judgeRequests(FunctionPassContext& context, Int32Prover& prover, Judge judge)
{
    const Function& function = context.function;
    TypingContext typing { context.program, function, context.types };
    bool hasReported = false;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& request = function.at(id);
            if (request.opcode != Opcode::Convert)
                continue;

            DeclaredType declared = request.declared();
            RELEASE_ASSERT(isPromise(declared));
            if (isAssertionOfDependency(context.program, declared))
                continue;
            ValueID value = request.valueOperand();
            const Type& known = typing.typeOf(value);
            bool isProvenByTypes = hasOnlyInt32sAmongNumbers(known) && (known.isNever() || known.overlaps(typing.typeOf(declared), context.program));
            if (!isEnteredAt(siteKindOf(context.program, declared)) || isProvenByTypes != (judge == Judge::FlowsThatTypesProve))
                continue;
            bool isFromLooseType = request.illTypedCause() == IllTypedCause::FromLooseType;
            NumberBounds bounds = isFromLooseType && !isProvenByTypes ? boundsOfType(known) : prover.boundsOf(value);
            if (bounds.isInt32OrNullish() || (isProvenByTypes && !prover.reliesOnUnprovenFlow()))
                continue;
            reportUnprovenFlow(context, { declared, function.locationOf(id), isNumberConstant(function, value) }, bounds);
            hasReported = true;
        }
    }
    return hasReported;
}

static bool hasPromisedTailCast(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return false;
    for (TailCast cast : terminator.casts()) {
        if (isPromise(cast.declared))
            return true;
    }
    return false;
}

static bool judgeTailCasts(FunctionPassContext& context, Int32Prover& prover)
{
    const Function& function = context.function;
    TypingContext expected { context.program, function, context.types, TypeLevel::Expected };
    bool hasReported = false;
    for (const Block& block : function.blocks) {
        const Terminator& terminator = block.terminator;
        if (!hasPromisedTailCast(terminator))
            continue;

        NumberBounds bounds = prover.boundsOf(terminator.callOperand());
        for (TailCast cast : terminator.casts()) {
            if (!isPromise(cast.declared)) {
                bounds = narrowToType(bounds, expected.typeOf(cast.declared));
                continue;
            }
            if (bounds.isInt32OrNullish())
                continue;
            if (isEnteredAt(siteKindOf(context.program, cast.declared)) && !isAssertionOfDependency(context.program, cast.declared)) {
                reportUnprovenFlow(context, { cast.declared, function.locationOf(terminator.origin()) }, bounds);
                hasReported = true;
            }
            bounds = NumberBounds::integers(Range::int32());
        }
    }
    return hasReported;
}

static bool hasPromises(const Function& function)
{
    for (const Block& block : function.blocks) {
        if (hasPromisedTailCast(block.terminator))
            return true;
        for (ValueID id : block.values) {
            if (function.at(id).opcode == Opcode::Convert)
                return true;
        }
    }
    return false;
}

static RequestCounts resolveInt32Requests(FunctionPassContext& context)
{
    Function& function = context.function;
    TypingContext typing { context.program, function, context.types };
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    std::optional<ValueSubstitution> substitution;
    RequestCounts counts;

    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            const Value& request = function.at(id);
            if (request.opcode != Opcode::Convert)
                continue;

            ValueID value = request.valueOperand();
            if (becomeResolved(function, id, resolveConvert(typing, typing.typeOf(value), request.declared()))) {
                ++counts.kept;
                continue;
            }

            if (!substitution)
                substitution.emplace(function);
            substitution->replace(id, value);
            erasedIndexes.set(index);
            ++counts.erased;
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(block, erasedIndexes);
    }

    if (substitution)
        substitution->apply(function, &context.analyses);
    if (counts.kept)
        context.analyses.didChangeValues();
    return counts;
}

PassOutcome checkInt32Promises(FunctionPassContext& context)
{
    if (!context.function.hasBody() || !hasPromises(context.function))
        return PassOutcome::Unchanged;

    Int32Prover prover({ context.program, context.function, context.analyses, context.types, context.build });
    bool hasReported = judgeRequests(context, prover, Judge::FlowsThatTypesDoNotProve);
    hasReported = judgeTailCasts(context, prover) || hasReported;
    if (hasReported)
        judgeRequests(context, prover, Judge::FlowsThatTypesProve);

    RequestCounts counts = resolveInt32Requests(context);
    if (counts.erased) {
        compactFunction(context.function);
        context.analyses.didChangeControlFlow();
    }
    if (counts.isEmpty())
        return PassOutcome::Unchanged;
    finishTypedValues(TypePropagationInputs { context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo });
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramPasses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowApply.h"
#include "AOTObservableOperations.h"
#include "AOTPredictedValues.h"
#include "AOTResultRules.h"
#include "AOTRewrite.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet repeatableCasts { Opcode::Check, Opcode::CheckInitialized, Opcode::RequireObjectCoercible, Opcode::RequireObject };

static_assert((simplifiableCasts | repeatableCasts) == simplifiableCasts);
static_assert(simplifiableCasts.contains(Opcode::AssumeType));

namespace {

struct CastAtHead {
    BlockID block;
    uint32_t parameterIndex;
    ValueID cast;
    bool hasTest;
};

// Every step tells the analyses what it has changed, so the frame only lends its reads and its builder.
struct CastSimplification : Rewrite {
    using Rewrite::Rewrite;

    unsigned run()
    {
        unsigned changed = static_cast<unsigned>(moveCastsToEdges());
        for (;;) {
            changed += removeCasts();
            // What has gone may have been the last way from a check to such a claim.
            while (std::exchange(hasTakenValuesFromBehindChecksInThisTurn, false) && hasKeptClaimThatNoCheckHolds())
                changed += removeCasts();
            // removeCasts() judges each cast by what it leaves behind, so it finds nothing more until removeRepeatedCasts() has changed something.
            unsigned repeated = removeRepeatedCasts();
            if (!repeated) {
                RELEASE_ASSERT(!context.build.options().verifiesAfterEveryPass || (!removeCasts() && !removeRepeatedCasts()));
                return changed;
            }
            changed += repeated;
        }
    }

    std::optional<CastAtHead> findCastOfParameter(const CFG& cfg, BlockID id)
    {
        const Block& block = at(id);
        if (block.isHandlerBlock() || block.parameters.isEmpty() || block.values.isEmpty())
            return std::nullopt;

        // In a validating build the test of an assumed type stands before the cast and goes to the edges with it.
        const Value& test = at(block.values[0]);
        bool hasTest = test.opcode == Opcode::ValidateType && block.values.size() > 1;
        ValueID castID = block.values[hasTest];
        const Value& cast = at(castID);
        if (cast.opcode != Opcode::Check && cast.opcode != Opcode::AssumeType)
            return std::nullopt;

        ValueID parameter = cast.operand(0);
        if (saysMoreThanTypeOnCredit(cast, parameter))
            return std::nullopt;
        if (hasTest && (cast.opcode != Opcode::AssumeType || test.valueOperand() != parameter || test.validatedType() != cast.provenType()))
            return std::nullopt;
        size_t parameterIndex = block.parameters.find(parameter);
        if (parameterIndex == notFound || parameterIndex >= numberOfSharedExplicitParameters(function, cfg, id))
            return std::nullopt;
        if (uses().useCount(parameter) != 1u + hasTest || !context.types.at(at(parameter).type).isJavaScriptValue())
            return std::nullopt;
        return CastAtHead { id, static_cast<uint32_t>(parameterIndex), castID, hasTest };
    }

    bool edgeNeedsCast(const CastAtHead& candidate, Edge edge)
    {
        ValueID definition = analyses.definitionThroughUpcasts(targetOf(function, edge).arguments[candidate.parameterIndex]);
        return !context.types.at(at(definition).type).isSubsetOf(context.types.at(at(candidate.cast).type), program);
    }

    bool canMoveCast(const CFG& cfg, const CastAtHead& candidate)
    {
        std::span<const Edge> edges = cfg.incomingEdges(candidate.block);
        bool castHasCode = at(candidate.cast).opcode == Opcode::Check;
        size_t numberOfEdgesNeedingCast = 0;
        for (Edge edge : edges) {
            if (edge.isHandlerEdge() || edge.from == candidate.block || at(edge.from).terminator.kind != TerminatorKind::Jump)
                return false;
            if (!edgeNeedsCast(candidate, edge))
                continue;
            if (castHasCode && at(edge.from).handler != at(candidate.block).handler)
                return false;
            ++numberOfEdgesNeedingCast;
        }
        if (castHasCode && numberOfEdgesNeedingCast > 1)
            return false;
        return numberOfEdgesNeedingCast < edges.size();
    }

    size_t moveCastsToEdges()
    {
        const CFG& cfg = this->cfg();
        Vector<CastAtHead, 4> candidates;
        for (BlockID block : cfg.reversePostOrder()) {
            if (block == cfg.root())
                continue;
            if (auto candidate = findCastOfParameter(cfg, block))
                candidates.append(*candidate);
        }

        Vector<ValueID, 4> movedParameters;
        bool didChangeEdge = false;
        for (const CastAtHead& candidate : candidates) {
            if (!canMoveCast(cfg, candidate))
                continue;
            const Value cast = at(candidate.cast);
            bool joinSaysLessThanCast = false;
            builder().setOrigin(cast.origin());
            for (Edge edge : cfg.incomingEdges(candidate.block)) {
                ValueID& argument = editTarget(function, edge).arguments[candidate.parameterIndex];
                ValueID passed = analyses.definitionThroughUpcasts(argument);
                if (edgeNeedsCast(candidate, edge)) {
                    if (candidate.hasTest)
                        builder().insertBefore(pointBeforeTerminatorOf(function, edge.from), makeValidateType(argument, cast.typeNamedByCast()));
                    Value castOnEdge = cast;
                    castOnEdge.setOperand(0, argument);
                    castOnEdge.setExpected({ });
                    // A check that nothing on its edge passes has the whole type that it asks for.
                    joinSaysLessThanCast |= !context.types.at(resultTypeOf(program, function, castOnEdge, context.types, &context.typingMemo)).isSubsetOf(context.types.at(cast.type), program);
                    passed = builder().insertBefore(pointBeforeTerminatorOf(function, edge.from), WTF::move(castOnEdge));
                }
                didChangeEdge |= passed != argument;
                argument = passed;
            }
            if (joinSaysLessThanCast)
                edit(candidate.cast, ValueEdit::Definition).becomeAssumeType(cast.operand(0), Proven::byRulesBehindChecks(cast.type));
            movedParameters.append(cast.operand(0));
        }
        if (movedParameters.isEmpty())
            return 0;
        mayHaveTakenValuesFromBehindChecks = true;

        builder().applyInsertions();
        predicted.reset();
        // The arguments of the edges were written past the builder.
        analyses.didChangeValues();
        // A join may widen, so the cast at the head stays until the type of its parameter is inside it.
        TypePropagationOutcome outcome = narrowValueTypes(TypePropagationInputs { program, context.id, function, analyses, context.types, context.build, &context.typingMemo }, movedParameters.span());
        return didChangeEdge || outcome.didChange() ? movedParameters.size() : 0;
    }

    // isSubsetOf() does not compare what a type only declares: contents that are not proven, a signature.
    bool isUnchangedByCastTo(const Type& type, const Type& castType)
    {
        return &type == &castType || type.castTo(castType, context.types) == type;
    }

    bool expectsNarrowerTypeThan(const Value& cast, ValueID operand)
    {
        TypeID expected = cast.expected().predicted();
        if (!expected || expected == at(operand).expected().predicted())
            return false;
        return !typeOf(operand).isSubsetOf(program.at(expected), program);
    }

    void eraseValue(BlockID block, uint32_t index)
    {
        if (erasedValues.isEmpty() || erasedValues.last().first != block)
            erasedValues.append({ block, BitVector() });
        erasedValues.last().second.set(index);
    }

    unsigned finishStep(unsigned changed)
    {
        if (!changed)
            return 0;
        for (auto& [block, indexes] : std::exchange(erasedValues, { }))
            builder().eraseValues(block, indexes);
        substitution.apply(function, &analyses);
        analyses.didChangeValues();
        predicted.reset();
        return changed;
    }

    unsigned removeCasts()
    {
        unsigned changed = 0;
        holdsRepeatableCast = false;
        claimsBehindChecks.shrink(0);
        claimsKeptBehindChecks.shrink(0);
        for (BlockID block : blocks()) {
            const BlockValues& values = at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (simplifiableCasts.contains(at(values[index]).opcode))
                    changed += simplifyCast(block, index);
            }
        }
        return finishStep(changed);
    }

    unsigned simplifyCast(BlockID block, uint32_t index)
    {
        ValueID id = at(block).values[index];
        const Value& cast = at(id);
        ValueID operand = substitution.resolve(cast.operand(0));

        ValueID narrowest = operand;
        while (!isCastThatProvesNothing(cast.opcode) && isCastThatProvesNothing(at(narrowest).opcode)) {
            ValueID inner = substitution.resolve(at(narrowest).operand(0));
            if (expectsNarrowerTypeThan(at(narrowest), inner) || !isUnchangedByCastTo(typeOf(inner), typeOf(narrowest)))
                break;
            narrowest = inner;
        }
        unsigned changed = narrowest != operand;
        if (changed)
            edit(id, ValueEdit::Definition).setOperand(0, narrowest);
        operand = narrowest;
        if (!typeOf(operand).isSubsetOf(typeOf(id), program)) {
            noteCastThatStays(id);
            return changed;
        }
        if (saysMoreThanTypeOnCredit(cast, operand)) {
            claimsKeptBehindChecks.append(id);
            return changed;
        }
        if (isTestOfLanguageAnsweredOnCredit(cast, operand)) {
            noteCastThatStays(id);
            return changed;
        }
        // A cast stands behind the checks in front of every operand, and what takes its place behind those in front of the first.
        bool standsBehindMoreChecksThanOperand = cast.opcode == Opcode::Check || cast.numberOfOperands() > 1;

        if (!expectsNarrowerTypeThan(cast, operand) && typeOf(operand).hasLiteralStrings() == typeOf(id).hasLiteralStrings() && isUnchangedByCastTo(typeOf(operand), typeOf(id))) {
            substitution.replace(id, operand);
            eraseValue(block, index);
        } else if (repeatableCasts.contains(cast.opcode))
            becomeUpcast(function, id, operand);
        else {
            noteCastThatStays(id);
            return changed;
        }
        hasTakenValuesFromBehindChecksInThisTurn |= standsBehindMoreChecksThanOperand;
        mayHaveTakenValuesFromBehindChecks |= standsBehindMoreChecksThanOperand;
        return changed + 1;
    }

    // Nothing else makes one more turn find what the last one has left.
    bool hasKeptClaimThatNoCheckHolds()
    {
        return std::ranges::any_of(claimsKeptBehindChecks, [&](ValueID claim) {
            return !isBehindCheck(at(claim).operand(0));
        });
    }

    void noteCastThatStays(ValueID id)
    {
        const Value& cast = at(id);
        holdsRepeatableCast |= repeatableCasts.contains(cast.opcode);
        if (cast.opcode == Opcode::AssumeType && cast.isBehindChecks())
            claimsBehindChecks.append(id);
    }

    // What is assumed in every version says more than a type that is owed to a check: the check may go.
    bool saysMoreThanTypeOnCredit(const Value& cast, ValueID operand)
    {
        return cast.opcode == Opcode::AssumeType && !cast.isBehindChecks() && isBehindCheck(operand);
    }

    // What the language asks of a value it asks in every version of the function. A type that is owed to a check answers in the typed version alone, so the test stays until the function has its generic version.
    bool isTestOfLanguageAnsweredOnCredit(const Value& cast, ValueID operand)
    {
        return cast.opcode != Opcode::Check && repeatableCasts.contains(cast.opcode) && isBehindCheck(operand);
    }

    bool isBehindCheck(ValueID value)
    {
        return marksOfChecks() && predicted->isBehindCheck(value);
    }

    const PredictedValues* marksOfChecks()
    {
        if (!hasChecks) {
            hasChecks = context.opcodesWhereRunBegins ? context.opcodesWhereRunBegins->contains(Opcode::Check)
                : std::ranges::any_of(function.values, [](const Value& candidate) { return candidate.opcode == Opcode::Check; });
        }
        if (!*hasChecks)
            return nullptr;
        if (!predicted)
            predicted.emplace(function, analyses.cfg(), PredictedValues::Options { .holdsCheck = true });
        return &*predicted;
    }

    // A check that the type of its operand answers is gone.
    void settleProofs()
    {
        if (mayHaveTakenValuesFromBehindChecks && !claimsBehindChecks.isEmpty())
            settleProofsBehindAnsweredChecks(function, claimsBehindChecks.span(), marksOfChecks());
    }

    std::tuple<ValueID, TypeID, uint32_t> keyOf(ValueID id)
    {
        const Value& cast = at(id);
        return { substitution.resolve(cast.operand(0)), cast.type, static_cast<uint32_t>(cast.opcode) };
    }

    uint64_t bindingOf(const Value& access)
    {
        if (access.opcode == Opcode::GetModuleVariable || access.opcode == Opcode::PutModuleVariable)
            return 1ull << 63 | access.variable().index();
        if (access.opcode != Opcode::GetEnvironmentField && access.opcode != Opcode::PutEnvironmentField)
            return 0;
        ValueID environment = analyses.sameObjectThroughCasts(substitution.resolve(access.environmentOperand())).valueInFrontOfFacts();
        return access.field() < 1u << 24 ? 1ull << 62 | static_cast<uint64_t>(environment.index()) << 24 | access.field() : 0;
    }

    // A binding never goes back into its dead zone: it has left it where a store has run, where the test for it has passed, and where a cast stands for that test.
    uint64_t bindingPastDeadZoneBehind(const Value& value)
    {
        if (value.opcode == Opcode::PutModuleVariable || value.opcode == Opcode::PutEnvironmentField)
            return typeOf(value.valueOperand()).primitives & Primitive::Absent ? 0 : bindingOf(value);
        if (!leavesDeadZoneBehind(program, function, value))
            return 0;
        return bindingOf(at(definitionThroughUpcasts(function, substitution.resolve(value.valueOperand()))));
    }

    unsigned repointOperand(ValueID id, uint32_t operandIndex, const Vector<ValueID>& narrowestCast)
    {
        ValueID current = at(id).operand(operandIndex);
        ValueID cast = narrowestCast[analyses.sameObjectThroughCasts(current).valueInFrontOfFacts().index()];
        if (!cast || typeOf(cast) == typeOf(current) || !isUnchangedByCastTo(typeOf(cast), typeOf(current)))
            return 0;
        if (!program.expectedOrTypeOf(function, cast).predicted().isSubsetOf(program.expectedOrTypeOf(function, current).predicted(), program))
            return 0;
        Value& user = edit(id, ValueEdit::Definition);
        user.setOperand(operandIndex, cast);
        // A check that nothing passes is typed by its cast type, as behind a check that passed, and that is more than it let through of the wider operand.
        if (user.opcode == Opcode::Check && !computeResultType(TypingContext { program, function, context.types }, user).isSubsetOf(typeOf(id), program)) {
            user.setOperand(operandIndex, current);
            return 0;
        }
        mayHaveTakenValuesFromBehindChecks = true;
        return 1;
    }

    unsigned removeRepeatedCasts()
    {
        if (!holdsRepeatableCast)
            return 0;

        // Of a block that adds casts or bindings, or hides some. Any other block leaves the tables as they are.
        struct Scope {
            BlockID block;
            size_t numberOfAddedCasts;
            size_t numberOfReplacedNarrowestCasts;
            size_t numberOfBindingsPastDeadZone;
            Vector<ValueID> hiddenCasts;
            Vector<uint64_t> hiddenBindingsPastDeadZone;
        };
        const Dominators& dominators = analyses.dominators();
        HashMap<std::tuple<ValueID, TypeID, uint32_t>, ValueID> passed;
        // A number stands for a variable, or for a field of the environment that a value of the function holds. In the order in which they were found: one that is found again further down is not listed twice.
        Vector<uint64_t> bindingsPastDeadZone;
        HashSet<uint64_t> isPastDeadZone;
        Vector<ValueID> addedCasts;
        Vector<ValueID> narrowestCast(function.values.size());
        Vector<std::pair<ValueID, ValueID>> replacedNarrowestCasts;
        Vector<Scope> scopes;
        unsigned changed = 0;

        auto addDominatingCast = [&](ValueID id) {
            passed.add(keyOf(id), id);
            addedCasts.append(id);
            ValueID object = analyses.sameObjectThroughCasts(id).valueInFrontOfFacts();
            ValueID before = narrowestCast[object.index()];
            if (before && typeOf(before).isSubsetOf(typeOf(id), program))
                return;
            replacedNarrowestCasts.append({ object, before });
            narrowestCast[object.index()] = id;
        };
        auto notePastDeadZone = [&](uint64_t binding) {
            if (!isPastDeadZone.add(binding).isNewEntry)
                return false;
            bindingsPastDeadZone.append(binding);
            return true;
        };
        auto forgetWhatWasAddedIn = [&](const Scope& scope) {
            while (addedCasts.size() > scope.numberOfAddedCasts)
                passed.remove(keyOf(addedCasts.takeLast()));
            while (replacedNarrowestCasts.size() > scope.numberOfReplacedNarrowestCasts) {
                auto [object, before] = replacedNarrowestCasts.takeLast();
                narrowestCast[object.index()] = before;
            }
            while (bindingsPastDeadZone.size() > scope.numberOfBindingsPastDeadZone)
                isPastDeadZone.remove(bindingsPastDeadZone.takeLast());
        };
        auto hasScope = [&](BlockID block) {
            return !scopes.isEmpty() && scopes.last().block == block;
        };
        auto enterScope = [&](BlockID block, Vector<ValueID>&& hiddenCasts, Vector<uint64_t>&& hiddenBindings) {
            scopes.append(Scope { block, addedCasts.size(), replacedNarrowestCasts.size(), bindingsPastDeadZone.size(), WTF::move(hiddenCasts), WTF::move(hiddenBindings) });
        };
        auto ensureScope = [&](BlockID block) {
            if (!hasScope(block))
                enterScope(block, { }, { });
        };
        // A covered block dominates its handler and whatever the handler leads to, and its casts do not.
        auto hideCastsAndBindingsOfDominator = [&](BlockID block) {
            if (scopes.isEmpty())
                return;
            const Scope& innermost = scopes.last();
            if (addedCasts.size() == innermost.numberOfAddedCasts && bindingsPastDeadZone.size() == innermost.numberOfBindingsPastDeadZone)
                return;
            if (innermost.block != dominators.idom(block) || dominators.terminatorDominates(innermost.block, block))
                return;
            Vector<ValueID> hiddenCasts(addedCasts.subspan(innermost.numberOfAddedCasts));
            Vector<uint64_t> hiddenBindings(bindingsPastDeadZone.subspan(innermost.numberOfBindingsPastDeadZone));
            forgetWhatWasAddedIn(innermost);
            enterScope(block, WTF::move(hiddenCasts), WTF::move(hiddenBindings));
        };

        dominators.walkTree([&](BlockID block) {
            hideCastsAndBindingsOfDominator(block);
            const BlockValues& values = at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                ValueID id = values[index];
                Opcode opcode = at(id).opcode;
                if (accessClassOf(opcode) == AccessClass::ByName) {
                    if (std::optional<uint8_t> receiver = receiverOf(opcode))
                        changed += repointOperand(id, *receiver, narrowestCast);
                    continue;
                }
                uint64_t binding = bindingPastDeadZoneBehind(at(id));
                if (binding)
                    ensureScope(block);
                bool wasPastDeadZone = binding && !notePastDeadZone(binding);
                if (!repeatableCasts.contains(opcode))
                    continue;
                if (ValueID earlier = passed.get(keyOf(id))) {
                    substitution.replace(id, earlier);
                    mayHaveTakenValuesFromBehindChecks |= opcode == Opcode::Check;
                    explainOperationThatGoes(function, id, InPlaceOfOperation::AnEarlierOneThatCompleted);
                    eraseValue(block, index);
                    ++changed;
                } else if (wasPastDeadZone && opcode == Opcode::CheckInitialized) {
                    Value& test = edit(id);
                    test.becomeAssumeType(test.valueOperand(), Proven::byConstruction(TypeTable::anything));
                    ++changed;
                } else {
                    // A cast of what an earlier cast has let through asks only for what that one left open, and the next round removes it where that is nothing.
                    changed += repointOperand(id, 0, narrowestCast);
                    ensureScope(block);
                    addDominatingCast(id);
                }
            }
        }, [&](BlockID block) {
            if (!hasScope(block))
                return;
            Scope scope = scopes.takeLast();
            forgetWhatWasAddedIn(scope);
            for (ValueID cast : scope.hiddenCasts)
                addDominatingCast(cast);
            for (uint64_t binding : scope.hiddenBindingsPastDeadZone)
                notePastDeadZone(binding);
        });
        return finishStep(changed);
    }

    ValueSubstitution substitution { function };
    bool hasTakenValuesFromBehindChecksInThisTurn { false };
    // A check has gone, or a cast that read something else beside what it let through, or an operation has got another operand than a cast of the one that it had.
    bool mayHaveTakenValuesFromBehindChecks { false };
    // The pass makes no check.
    std::optional<bool> hasChecks;
    std::optional<PredictedValues> predicted;
    // Of the casts that the last removeCasts() has left.
    bool holdsRepeatableCast { false };
    Vector<ValueID, 8> claimsBehindChecks;
    Vector<ValueID, 8> claimsKeptBehindChecks;
    Vector<std::pair<BlockID, BitVector>, 4> erasedValues;
};

} // anonymous namespace

PassOutcome simplifyCasts(FunctionPassContext& context)
{
    CastSimplification simplification(context);
    if (!simplification.run())
        return PassOutcome::Unchanged;
    simplification.settleProofs();
    // Every step drops what its edits have made stale, so what is held at the end was made behind the last edit.
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

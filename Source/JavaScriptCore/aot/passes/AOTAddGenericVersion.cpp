/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAddGenericVersion.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTGrouping.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTOperatorForms.h"
#include "AOTPredictedValues.h"
#include "AOTProgramCaches.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <tuple>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet typeValidations { Opcode::ValidateType, Opcode::ValidateCallee };
static constexpr OpcodeSet castsBehindChecks { Opcode::Upcast, Opcode::AssumeType };

static TypeID anyValueType(TypeArena& types)
{
    return types.withPrimitives(TypeTable::anything, Primitive::Absent);
}

static void becomeUpcast(Value& cast, ValueID operand, TypeID castType)
{
    cast.become(Opcode::Upcast);
    cast.setValueOperand(operand);
    cast.setCastType(castType);
}

// The parameter that takes what is handed on along an edge. Null where the target has none for it.
static ValueID parameterTaking(const Function& function, const Use& use)
{
    const CompactList<ValueID>& parameters = function.at(targetOf(function, use.edge()).block).parameters;
    return use.operandIndex < parameters.size() ? parameters[use.operandIndex] : ValueID();
}

static void forEachEdgeResultParameter(const Function& function, BlockID from, NOESCAPE const Invocable<void(ValueID, EdgeResultPosition)> auto& functor)
{
    const TargetList& targets = function.at(from).terminator.targets;
    for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        const CompactList<ValueID>& parameters = function.at(targets[targetIndex].block).parameters;
        uint32_t first = firstEdgeResultParameter(function, from, targetIndex);
        uint32_t numberOfResults = numberOfEdgeResults(function, from, targetIndex);
        for (uint32_t resultIndex = 0; resultIndex < numberOfResults && first + resultIndex < parameters.size(); ++resultIndex)
            functor(parameters[first + resultIndex], EdgeResultPosition { from, targetIndex, resultIndex });
    }
}

static bool isBefore(ProgramPoint a, ProgramPoint b)
{
    return std::tie(a.block, a.index) < std::tie(b.block, b.index);
}

static Value makeMismatchReport(ValueID value, ExpectedType expectedType, TypeSiteID site)
{
    Value report = IRBuilder::make(Opcode::ReportMismatch, { value });
    report.setExpectedType(expectedType);
    report.setSite(site);
    return report;
}

namespace {


struct CheckSite {
    ValueID check;
};

struct GenericVersionBuilder {
    FunctionPassContext& context;
    const Program& program { context.program };
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    TypeArena& types { context.types };
    IRBuilder builder { analyses.context(), function, types, &analyses };
    TypeID anyValue { anyValueType(types) };
    bool reportsMismatches { context.build.recordedOptions().policy() == MismatchPolicy::Report };
    Vector<CheckSite, 8> sites { };

    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return program.typeOf(function, value); }
    TypePropagationInputs typingInputs() { return { program, context.id, function, analyses, types, context.build, &context.typingMemo }; }

    void typeAgain()
    {
        typeValuesWithoutCutting(typingInputs());
        analyses.didChangeTypes();
    }

    PassOutcome run()
    {
        // Everything here starts from a check.
        if (!analyses.opcodesOfValues().contains(Opcode::Check))
            return PassOutcome::Unchanged;

        bool changed = removeChecksInUnreachableBlocks();
        collectChecks();
        changed |= removeRedundantChecks();
        bool hasDroppedChecks = dropChecksNobodyReliesOn();
        hasDroppedChecks |= dropChecksThatTurnAwayWhatArrives();
        hasDroppedChecks |= guardElementsThatOneOperatorTakes();
        if (sites.isEmpty()) {
            if (hasDroppedChecks)
                finishTyping();
            else
                settleProofsBehindAnsweredChecks(function, analyses.cfg());
            return changed || hasDroppedChecks ? PassOutcome::Changed : PassOutcome::Unchanged;
        }

        Vector<ValueID, 8> checks(sites.size(), [&](size_t index) { return sites[index].check; });
        replaceChecksWithGuards(checks.span());
        finishTyping();
        return PassOutcome::Changed;
    }

    // Follows the checked value through the casts that are named and the parameters that take it. False once one of its other uses is not accepted.
    bool acceptsEveryUseOfCheckedValue(ValueID check, bool (*isFollowedCast)(Opcode), NOESCAPE const auto& accepts)
    {
        const UseLists& useLists = analyses.useLists();
        BitVector isSameValue(function.values.size());
        Vector<ValueID, 8> worklist;
        auto add = [&](ValueID value) {
            if (isSameValue.add(value.index()))
                worklist.append(value);
        };
        add(check);
        while (!worklist.isEmpty()) {
            for (const Use& use : useLists.usesOf(worklist.takeLast())) {
                switch (use.kind) {
                case Use::Kind::Operand:
                    if (isFollowedCast(function.at(use.user).opcode)) {
                        add(use.user);
                        break;
                    }
                    [[fallthrough]];
                case Use::Kind::TerminatorOperand:
                    if (!accepts(use, isSameValue, add))
                        return false;
                    break;
                case Use::Kind::EdgeArgument:
                case Use::Kind::HandlerArgument:
                    if (ValueID parameter = parameterTaking(function, use))
                        add(parameter);
                    else
                        return false;
                    break;
                case Use::Kind::BlockSubject:
                    return false;
                }
            }
        }
        return true;
    }

    // Whether an operation looks at the checked value, or a position of some type takes it.
    bool isReliedOn(ValueID check)
    {
        TypingContext typing { program, function, types };
        auto takesAnything = [](TypeID required) { return !required || required == TypeTable::anything; };
        // A store into a typed field tests a value that is not proven. One store outside every loop tests as often as the check. What the library is handed is anybody's, and its copy runs.
        unsigned numberOfStores = 0;
        auto isOnlyStore = [&](ValueID store) {
            return !isLibraryCode(program, function) && function.at(store).opcode == Opcode::PropSet && !analyses.isInCycle(analyses.useLists().locationOf(store).block) && !numberOfStores++;
        };
        return !acceptsEveryUseOfCheckedValue(check, [](Opcode opcode) { return castsBehindChecks.contains(opcode); }, [&](const Use& use, auto&...) {
            if (use.kind == Use::Kind::TerminatorOperand) {
                const Terminator& terminator = function.at(use.block).terminator;
                return terminator.kind == TerminatorKind::Return && takesAnything(requiredTypeOfOperand(typing, terminator, use.operandIndex));
            }
            const Value& user = function.at(use.user);
            if (user.operandUses(use.operandIndex) != OperandUses { OperandUse::Retains })
                return false;
            // Only calls and that store count among the operations that take any value.
            if (user.operandKind(use.operandIndex) == OperandKind::Any)
                return effectRuleOf(user.opcode) == EffectRule::Call || isOnlyStore(use.user);
            // The storage of an array literal that names none follows the types of its elements.
            if (holderOfRequiredTypes(program, user) == RequiredTypeHolder::ContentsOfLiteral)
                return false;
            return user.operandKind(use.operandIndex) == OperandKind::Declared && takesAnything(requiredTypeOfOperand(typing, user, use.operandIndex));
        });
    }

    // What a test has left of the value goes on as a parameter of the block behind the test.
    void addValuesNarrowedBehind(BlockID from, const BitVector& isSameValue, NOESCAPE const auto& add)
    {
        forEachEdgeResultParameter(function, from, [&](ValueID parameter, EdgeResultPosition position) {
            ValueID narrowed = operandNarrowedOnEdge(function, position);
            if (narrowed && isSameValue.get(narrowed.index()))
                add(parameter);
        });
    }

    void addValuesNarrowedByAnswer(ValueID answer, const BitVector& isSameValue, NOESCAPE const auto& add)
    {
        for (const Use& useOfAnswer : analyses.useLists().usesOf(answer)) {
            if (useOfAnswer.kind == Use::Kind::TerminatorOperand)
                addValuesNarrowedBehind(useOfAnswer.block, isSameValue, add);
        }
    }

    // `fiber === cursor.current`: beside what is no number, no string and no bigint the bits are compared, whatever the checked value is.
    bool comparesBits(const Use& use)
    {
        if (use.kind != Use::Kind::Operand || function.at(use.user).opcode != Opcode::StrictEq)
            return false;
        // The other side must not owe its type to a check that goes for the same reason.
        ValueID other = function.at(use.user).operand(1 - use.operandIndex);
        return function.at(other).opcode != Opcode::Check && canCompareBits(typeOf(other), types.at(TypeTable::anything));
    }

    bool isComparedByBitsAtEachUse(ValueID check)
    {
        std::span<const Use> uses = analyses.useLists().usesOf(check);
        return !uses.empty() && std::ranges::all_of(uses, [&](const Use& use) { return comparesBits(use); });
    }

    // `level === "warn"`: a text is compared with a listed text as it is. To find which of the listed texts it is takes a search, which costs more than the comparisons.
    bool isListedTextThatIsOnlyCompared(ValueID check)
    {
        const Type& checked = types.at(function.at(check).expectedType().predicted());
        if (!checked.hasLiteralStrings() || !checked.hasOnlyPrimitives(Primitive::LiteralString | Primitive::UndefinedOrNull))
            return false;
        return acceptsEveryUseOfCheckedValue(check, isCastWithoutTest, [&](const Use& use, const BitVector& isSameValue, const auto& add) {
            if (use.kind == Use::Kind::TerminatorOperand) {
                if (function.at(use.block).terminator.kind != TerminatorKind::SwitchString)
                    return false;
                addValuesNarrowedBehind(use.block, isSameValue, add);
                return true;
            }
            const Value& user = function.at(use.user);
            // What flow says of the value behind the check goes with the check.
            if (typeValidations.contains(user.opcode))
                return true;
            // An operation that takes any value, only looks at it and answers yes or no.
            if (user.operandKind(use.operandIndex) != OperandKind::Any || user.operandUses(use.operandIndex) != OperandUses { OperandUse::Inspects } || !typeOf(use.user).isBoolean())
                return false;
            addValuesNarrowedByAnswer(use.user, isSameValue, add);
            return true;
        });
    }

    // A value that is only handed on, as an argument of a call by value or into a position of any type, is the same value in both versions of the function.
    bool dropChecksNobodyReliesOn()
    {
        // A build that reports mismatches reports them where the check stands.
        if (reportsMismatches)
            return false;
        Vector<ValueID, 8> dropped;
        const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
        ParametersLeftToBody leftToBody(program, function);
        for (const CheckSite& site : sites) {
            const Value& check = function.at(site.check);
            auto isParameterThatNeedNotPass = [&] {
                size_t parameter = entryParameters.find(sameObjectThroughCasts(function, check.valueOperand()).valueInFrontOfFacts());
                if (parameter == notFound)
                    return false;
                return !isReliedOn(site.check) || (check.cause() != CheckCause::MayFail && leftToBody.contains(parameter, types.at(check.expectedType().predicted())));
            };
            bool leavesCodeAsItIs = check.cause() != CheckCause::MayFail && !changesCodeWhereProven(types.at(check.expectedType().predicted()));
            if (leavesCodeAsItIs || isComparedByBitsAtEachUse(site.check) || isListedTextThatIsOnlyCompared(site.check) || isParameterThatNeedNotPass())
                dropped.append(site.check);
        }
        if (dropped.isEmpty())
            return false;
        replaceChecksWithGuards(dropped.span());
        typeAgain();
        sites.removeAllMatching([&](const CheckSite& site) { return dropped.contains(site.check); });
        return true;
    }

    // Flow has seen an ordinary object arrive that the program only calls by the type. The test would fail in ordinary runs and send them into the generic version, so the value goes on as one that has no type.
    bool dropChecksThatTurnAwayWhatArrives()
    {
        Vector<ValueID, 8> dropped;
        Vector<ValueID, 8> primitivesStillExpected;
        for (const CheckSite& site : sites) {
            const Value& check = function.at(site.check);
            if (check.cause() == CheckCause::MayFail)
                (types.at(check.expectedType().predicted()).hasObjects() ? dropped : primitivesStillExpected).append(site.check);
        }
        if (dropped.isEmpty() && primitivesStillExpected.isEmpty())
            return false;
        // What is read from such an object has its type from the type that does not hold.
        BitVector isReadFromSuchObject = valuesBehind(function, analyses.cfg(), dropped.span());
        for (const CheckSite& site : sites) {
            if (isReadFromSuchObject.get(site.check.index()))
                dropped.appendIfNotContains(site.check);
        }
        primitivesStillExpected.removeAllMatching([&](ValueID check) { return dropped.contains(check); });
        replaceChecksWithGuards(dropped.span(), ExpectedTypes::Dropped);
        replaceChecksWithGuards(primitivesStillExpected.span(), ExpectedTypes::Kept);
        // A report that was put in front of a check has moved what stands behind it.
        collectChecks();
        return true;
    }

    // `sum += list[index]`: an array and an iterator hand out undefined whatever their elements are declared to be, and the operator has a form for it. An exit would make all that the generic version reads.
    bool guardElementsThatOneOperatorTakes()
    {
        Vector<ValueID, 8> guarded;
        for (const CheckSite& site : sites) {
            const Value& check = function.at(site.check);
            Opcode source = function.at(sameObjectThroughCasts(function, check.valueOperand())).opcode;
            std::span<const Use> uses = analyses.useLists().usesOf(site.check);
            // A cast hands the value on to whoever takes it.
            while (uses.size() == 1 && uses[0].kind == Use::Kind::Operand && castsBehindChecks.contains(function.at(uses[0].user).opcode))
                uses = analyses.useLists().usesOf(uses[0].user);
            if ((source != Opcode::ElemGet && source != Opcode::ResultAt) || uses.size() != 1)
                continue;
            // A switch over texts tells them apart itself.
            if (uses[0].kind == Use::Kind::TerminatorOperand && function.at(uses[0].block).terminator.kind == TerminatorKind::SwitchString && !typeOf(check.valueOperand()).hasObjects()) {
                guarded.append(site.check);
                continue;
            }
            if (uses[0].kind != Use::Kind::Operand)
                continue;
            const Value& user = function.at(uses[0].user);
            // What is compared with another value is mostly read again behind the comparison, and that read is checked.
            bool isOnlyLookedAt = user.operandKind(uses[0].operandIndex) == OperandKind::Any && !user.operandUses(uses[0].operandIndex).contains(OperandUse::Retains) && (user.numberOfOperands() == 1 || !typeOf(uses[0].user).isBoolean());
            if (isOnlyLookedAt && !types.at(check.expectedType().predicted()).hasObjects() && !typeOf(check.valueOperand()).hasObjects())
                guarded.append(site.check);
        }
        if (guarded.isEmpty())
            return false;
        replaceChecksWithGuards(guarded.span());
        collectChecks();
        return true;
    }

    bool removeChecksInUnreachableBlocks()
    {
        const CFG& cfg = analyses.cfg();
        bool changed = false;
        for (uint32_t index = 0; index < function.blocks.size(); ++index) {
            if (cfg.isReachable(BlockID(index)))
                continue;
            for (ValueID id : function.blocks[index].values) {
                if (function.at(id).opcode != Opcode::Check)
                    continue;
                becomeUpcast(function, id, function.at(id).valueOperand());
                changed = true;
            }
        }
        if (changed)
            analyses.didChangeValues();
        return changed;
    }

    void collectChecks()
    {
        sites.shrink(0);
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            const BlockValues& values = function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (function.at(values[index]).opcode == Opcode::Check)
                    sites.append(CheckSite { .check = values[index] });
            }
        }
    }

    bool removeRedundantChecks()
    {
        const UseLists& useLists = analyses.useLists();
        unsigned numberOfRemovedChecks = sites.removeAllMatching([&](const CheckSite& site) {
            ValueID operand = function.at(site.check).valueOperand();
            TypeID castType = function.at(site.check).expectedType().predicted();
            if (typeOf(operand).isSubsetOf(types.at(castType), program)) {
                becomeUpcast(function.edit(site.check, ValueEdit::Definition), operand, castType);
                return true;
            }
            if (useLists.useCount(site.check))
                return false;
            Value& check = function.edit(site.check);
            becomeUpcast(check, operand, anyValue);
            check.type = function.at(operand).type;
            check.setExpected({ });
            return true;
        });
        if (numberOfRemovedChecks)
            didChangeCastsInPlace();
        return numberOfRemovedChecks;
    }

    // A cast was replaced by another cast of the same operand, or a payload changed. Use lists, identities and liveness depend only on operands and on which opcodes are casts.
    void didChangeCastsInPlace()
    {
        static_assert(isCast(Opcode::Check) && isCast(Opcode::AssumeType) && isCast(Opcode::Upcast) && isCast(Opcode::Expect));
        static_assert(OperandIndex::Check::value == OperandIndex::Upcast::value && OperandIndex::AssumeType::value == OperandIndex::Upcast::value && OperandIndex::Expect::value == OperandIndex::Upcast::value);
        analyses.invalidate(valueAnalyses - analysesThatReadNoTypes);
    }

    void didForgetProofs(const ForgottenProofs& forgotten)
    {
        // A validation that is erased lets go of its operand.
        if (forgotten.hasErasedValidation)
            analyses.didChangeValues();
        else
            didChangeCastsInPlace();
    }

    void replaceChecksWithGuards(std::span<const ValueID> checks, ExpectedTypes expectedTypes = ExpectedTypes::Kept)
    {
        if (checks.empty())
            return;
        if (reportsMismatches) {
            const UseLists& useLists = analyses.useLists();
            Vector<ProgramPoint, 8> points(checks.size(), [&](size_t index) { return useLists.pointOf(checks[index]); });
            for (size_t index = 0; index < checks.size(); ++index) {
                const Value& check = function.at(checks[index]);
                builder.setOrigin(check.origin());
                builder.insertBefore(points[index], makeMismatchReport(check.valueOperand(), check.expectedType(), check.site()));
            }
            builder.applyInsertions();
        }

        didForgetProofs(expectInsteadOfChecking(function, analyses.cfg(), types, checks, expectedTypes));
        context.build.statistics().count(StatisticID::ChecksReplacedByGuards, checks.size());
    }

    void finishTyping()
    {
        // Every check is a test by now or is gone.
        settleProofsBehindAnsweredChecks(function, analyses.cfg());
        typeAgain();
        // Whatever it stores behind an allocation, it puts a value in for.
        size_t numberOfValuesBefore = function.values.size();
        storeUnprovenInitialValues(program, context.id, function, types, context.build);
        if (function.values.size() != numberOfValuesBefore)
            analyses.didChangeValues();
        finishTypedValues(typingInputs());
    }
};

} // anonymous namespace

PassOutcome addGenericVersion(FunctionPassContext& context)
{
    if (!context.function.hasBody())
        return PassOutcome::Unchanged;
    // The generic version takes over what is alive where a check fails, and an object that it takes over has to be made.
    PassOutcome outcome = leaveURLsUnbuilt(context) ? PassOutcome::Changed : PassOutcome::Unchanged;
    outcome |= GenericVersionBuilder { context }.run();
    // The analyses were told of every edit when it was made.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

namespace {

struct Position {
    enum class Kind : uint8_t { Variable, Field, EntryParameter, Result };

    Kind kind { Kind::Variable };
    uint32_t owner { 0 };
    uint32_t index { 0 };

    friend auto operator<=>(const Position&, const Position&) = default;
};

struct TypedStore {
    ValueID stored;
    TypeID required;
    const Value* operation { nullptr };
    uint32_t operandIndex { 0 };
};

} // anonymous namespace

static Position positionOfField(FieldReference field)
{
    return { Position::Kind::Field, field.layout.index(), field.field };
}

static const DeclaredType* findDeclaredType(const Program& program, Position position)
{
    switch (position.kind) {
    case Position::Kind::Variable:
        return &program.variables[position.owner].declared;
    case Position::Kind::Field:
        return &program.layouts[position.owner].fields[position.index].declared;
    case Position::Kind::EntryParameter:
        return program.functions[position.owner].declaredEntryParameter(position.index);
    case Position::Kind::Result:
        return &program.functions[position.owner].results[position.index];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool markExpected(Program& program, Position position)
{
    DeclaredType* declared = const_cast<DeclaredType*>(findDeclaredType(program, position));
    if (!declared || declared->isExpected() || !program.at(declared->written()).isJavaScriptValue())
        return false;
    declared->expect(declared->expected());
    declared->remove(PositionFlags::IsInt32Promise);
    return true;
}

static bool canBeAssignedByEvaluatedText(const EvalBinding& binding)
{
    switch (binding.kind) {
    case EvalBindingKind::Variable:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::PrivateName:
    case EvalBindingKind::SimpleCatchParameter:
        return true;
    case EvalBindingKind::This:
        return binding.function.has_value();
    case EvalBindingKind::Constant:
    case EvalBindingKind::FunctionName:
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::HomeObject:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::PrivateMethod:
    case EvalBindingKind::PrivateAccessor:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
    case EvalBindingKind::MappedArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Evaluated text is one more writer that no type holds back.
static Vector<Position> findPositionsAssignedByEvaluatedText(const Program& program)
{
    Vector<Position> positions;
    auto add = [&](Position position) {
        if (findDeclaredType(program, position)->written() != TypeTable::anything)
            positions.append(position);
    };
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!canBeAssignedByEvaluatedText(binding))
                continue;
            if (binding.variable)
                add({ Position::Kind::Variable, binding.variable.index(), 0 });
            else if (binding.layout && binding.field && program.at(binding.layout).isEnvironment())
                add({ Position::Kind::Field, binding.layout.index(), *binding.field });
        }
    }
    return positions;
}

static bool markNamespaceFields(Program& program, std::span<const Position> marked)
{
    if (marked.empty() || marked.front().kind != Position::Kind::Variable)
        return false;
    bool changed = false;
    for (const Module& module : program.modules) {
        for (uint32_t index = 0; module.namespaceLayout && index < module.exports.size(); ++index) {
            const ModuleExport& entry = module.exports[index];
            if (entry.kind == ModuleExportKind::Variable && entry.variable && std::ranges::binary_search(marked, Position { Position::Kind::Variable, entry.variable.index(), 0 }))
                changed |= markExpected(program, { Position::Kind::Field, module.namespaceLayout.index(), index });
        }
    }
    return changed;
}

static std::optional<Position> storedPosition(const Program& program, FunctionID id, const TypedStore& store)
{
    static_assert(!static_cast<unsigned>(EntryParameter::Capture));
    if (!store.operation)
        return Position { Position::Kind::Result, id.index(), 0 };
    const Value& operation = *store.operation;
    auto variadicIndex = [&] { return *operation.variadicOperandIndex(store.operandIndex); };
    switch (operation.opcode) {
    case Opcode::PutModuleVariable:
        return Position { Position::Kind::Variable, operation.variable().index(), 0 };
    case Opcode::PutEnvironmentField:
        return Position { Position::Kind::Field, operation.layout().index(), operation.field() };
    case Opcode::NewObject: {
        FieldReference field = declarationOf(program, { operation.layout(), operation.fields()[variadicIndex()] });
        bool staysInAllocation = fieldAt(program, field).flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
        if (!program.at(operation.layout()).isEnvironment() && !staysInAllocation && canStoreInitialValueBehindAllocation(program, operation, variadicIndex()))
            return std::nullopt;
        return positionOfField(field);
    }
    case Opcode::NewClosure:
    case Opcode::CallDirect:
        return Position { Position::Kind::EntryParameter, operation.function().index(), variadicIndex() };
    case Opcode::Results:
        return Position { Position::Kind::Result, id.index(), variadicIndex() };
    default:
        return std::nullopt;
    }
}

static ALWAYS_INLINE std::optional<Position> variableReadBy(const Value& read)
{
    if (read.opcode == Opcode::GetModuleVariable)
        return Position { Position::Kind::Variable, read.variable().index(), 0 };
    if (read.opcode == Opcode::GetEnvironmentField)
        return Position { Position::Kind::Field, read.layout().index(), read.field() };
    return std::nullopt;
}

static ALWAYS_INLINE std::optional<Position> readPosition(const Program& program, const Function& function, const Value& read)
{
    FunctionID callee;
    uint32_t resultIndex = 0;
    switch (read.opcode) {
    case Opcode::CallDirect:
        if (program.at(read.function()).results.size() == 1)
            callee = read.function();
        break;
    case Opcode::Call:
        callee = onlyCalleeOf(program, program.typeOf(function, read.operand(OperandIndex::Call::callee)));
        break;
    case Opcode::ResultAt:
        if (function.at(read.resultsOperand()).opcode == Opcode::CallDirect)
            callee = function.at(read.resultsOperand()).function();
        resultIndex = read.index();
        break;
    default:
        return variableReadBy(read);
    }
    if (!callee)
        return std::nullopt;
    return Position { Position::Kind::Result, callee.index(), resultIndex };
}

// After flow a read by name through a proven receiver has the type of the fields that it may find. None for any other operation.
static ALWAYS_INLINE std::optional<PropertyKeyID> keyOfTypedReadByName(const Program& program, const Value& read)
{
    if ((read.opcode != Opcode::PropGet && read.opcode != Opcode::PrivateGet) || program.at(read.type).isAnything())
        return std::nullopt;
    return read.opcode == Opcode::PropGet ? read.key() : program.at(read.layout()).fields[read.field()].key;
}

// What is told apart by the value of a field leaves out the layouts whose field cannot hold that value: it reads the types of their fields, and of no other.
static ALWAYS_INLINE bool leavesOutLayoutsByField(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::NarrowByField && program.at(value.type).layouts != program.typeOf(function, value.valueOperand()).layouts;
}

static ALWAYS_INLINE bool isTypedFromFieldsUnderName(const Program& program, const Function& function, const Value& value)
{
    return keyOfTypedReadByName(program, value) || leavesOutLayoutsByField(program, function, value);
}

static bool hasMarkedField(std::span<const Position> marked)
{
    auto firstField = std::ranges::lower_bound(marked, Position { Position::Kind::Field, 0, 0 });
    return firstField != marked.end() && firstField->kind == Position::Kind::Field;
}

static bool readsMarkedFieldByName(const Program& program, const Function& function, const Value& read, std::span<const Position> marked)
{
    if (!hasMarkedField(marked))
        return false;
    auto hasMarkedFieldUnder = [&](LayoutID member, PropertyKeyID key) {
        std::optional<FieldReference> field = findField(program, member, key, BaseChain::Proven);
        return field && std::ranges::binary_search(marked, positionOfField(declarationOf(program, *field)));
    };
    if (leavesOutLayoutsByField(program, function, read)) {
        std::span<const LayoutID> kept = membersOf(program, program.at(read.type));
        return std::ranges::any_of(membersOf(program, program.typeOf(function, read.valueOperand())), [&](LayoutID member) {
            return !std::ranges::binary_search(kept, member) && hasMarkedFieldUnder(member, read.key());
        });
    }
    std::optional<PropertyKeyID> key = keyOfTypedReadByName(program, read);
    if (!key)
        return false;
    return std::ranges::any_of(membersOf(program, program.typeOf(function, read.objectOperand())), [&](LayoutID member) {
        return hasMarkedFieldUnder(member, *key);
    });
}

static Vector<TypedStore, 16> findTypedStores(const TypingContext& typing, const CFG& cfg)
{
    const Function& function = typing.function;
    Vector<TypedStore, 16> stores;
    auto visit = [&](const TypedStore& store) {
        if (store.stored && store.required && store.required != TypeTable::anything)
            stores.append(store);
    };
    TypeID requiredResult;
    if (function.results.size() == 1 && !function.isUnsplitCoroutine())
        requiredResult = typing.typeIDOf(function.result());
    const OpcodeSet& opcodesWithRequiredTypes = opcodesWithRequiredOperandTypes();
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (!opcodesWithRequiredTypes.contains(value.opcode))
                continue;
            // The contents of a literal are cut by no check, so they take nothing that a check turns away.
            if (holderOfRequiredTypes(typing.program, value) == RequiredTypeHolder::ContentsOfLiteral)
                continue;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Declared)
                    visit({ operand, requiredTypeOfOperand(typing, value, operandIndex), &value, operandIndex });
            });
        }
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind == TerminatorKind::Return)
            visit({ terminator.valueOperand(), requiredResult });
        else if (terminator.kind == TerminatorKind::ReturnCall)
            visit({ terminator.callOperand(), requiredResult });
    }
    return stores;
}

static void appendIllTypedStores(const TypingContext& typing, FunctionID id, std::span<const TypedStore> stores, const BitVector* storedValues, Vector<Position>& positions)
{
    for (const TypedStore& store : stores) {
        if ((storedValues && !storedValues->get(store.stored.index())) || typing.typeOf(store.stored).isSubsetOf(typing.types.at(store.required), typing.program))
            continue;
        if (std::optional<Position> position = storedPosition(typing.program, id, store))
            positions.append(*position);
    }
}

static ValueID definitionThrough(const OpcodeSet& casts, const Function& function, ValueID value)
{
    while (casts.contains(function.at(value).opcode))
        value = function.at(value).valueOperand();
    return value;
}

// What passes a check is the value that went in.
static std::optional<Position> variableReadThroughCastsAndChecks(const Function& function, ValueID value)
{
    auto isCastOrCheck = [](Opcode opcode) { return opcode == Opcode::Upcast || opcode == Opcode::AssumeType || opcode == Opcode::Check || opcode == Opcode::CheckInitialized; };
    while (isCastOrCheck(function.at(value).opcode))
        value = function.at(value).valueOperand();
    return variableReadBy(function.at(value));
}

// AddGenericVersion drops a check that has nothing left to test, so no generic version comes of it.
bool mayTurnValueAway(const Program& program, const Function& function, const Value& check)
{
    return !program.typeOf(function, check.valueOperand()).isSubsetOf(program.at(check.expectedType().predicted()), program);
}

bool changesCodeWhereProven(const Type& checked)
{
    return !checked.anyObject;
}

bool isMetByOtherObjects(const Program& program, const Type& checked)
{
    std::span<const LayoutID> layouts = membersOf(program, checked);
    return !layouts.empty() && std::ranges::all_of(layouts, [&](LayoutID layout) { return program.at(layout).kind == LayoutKind::Record; });
}

bool ParametersLeftToBody::contains(size_t entryParameter, const Type& checked)
{
    if (m_function.thisParameterIndex() == entryParameter || !isMetByOtherObjects(m_program, checked))
        return false;
    if (!m_isFirstValueOfLoopVariable)
        findFirstValuesOfLoopVariables();
    return m_isFirstValueOfLoopVariable->get(entryParameter);
}

void ParametersLeftToBody::findFirstValuesOfLoopVariables()
{
    const CompactList<ValueID>& entryParameters = m_function.at(entryBlock).parameters;
    m_isFirstValueOfLoopVariable.emplace(entryParameters.size());
    CFG cfg(m_function);
    if (!cfg.hasCycle())
        return;
    auto inFrontOfChecks = [&](ValueID value) {
        for (;;) {
            value = sameObjectThroughCasts(m_function, value).valueInFrontOfFacts();
            if (m_function.at(value).opcode != Opcode::Check)
                return value;
            value = m_function.at(value).valueOperand();
        }
    };
    for (BlockID head : cfg.reversePostOrder()) {
        std::span<const Edge> incomingEdges = cfg.incomingEdges(head);
        auto comesAround = [&](Edge edge) { return cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(head); };
        if (std::ranges::none_of(incomingEdges, comesAround) || std::ranges::any_of(incomingEdges, &Edge::isHandlerEdge))
            continue;
        for (uint32_t index = 0; index < m_function.at(head).parameters.size(); ++index) {
            auto argumentOn = [&](Edge edge) { return inFrontOfChecks(targetOf(m_function, edge).arguments[index]); };
            // Two of the values that come around tell of any value whether another one does.
            ValueID comingAround;
            ValueID anotherComingAround;
            for (Edge edge : incomingEdges) {
                ValueID argument = comesAround(edge) ? argumentOn(edge) : ValueID();
                if (!argument || argument == m_function.at(head).parameters[index] || argument == comingAround)
                    continue;
                (comingAround ? anotherComingAround : comingAround) = argument;
                if (anotherComingAround)
                    break;
            }
            if (!comingAround)
                continue;
            for (Edge edge : incomingEdges) {
                size_t entryParameter = comesAround(edge) ? notFound : entryParameters.find(argumentOn(edge));
                if (entryParameter != notFound && (anotherComingAround || comingAround != entryParameters[entryParameter]))
                    m_isFirstValueOfLoopVariable->set(entryParameter);
            }
        }
    }
}

namespace {

// In flow every read of a property may answer undefined, also the read of a field that flow's results then call always filled.
struct PositionsProvenByTypes {
    struct Arrival {
        Position position;
        ValueID value;
    };
    // The target holds as long as the source does.
    struct Link {
        Position target;
        Position source;
    };
    struct TestedCheck {
        ValueID check;
        bool passesWhileSourcesHold { false };
        Vector<Position, 2> sources { };
        Vector<Position, 1> sourcesWithoutStart { };
    };
    struct Body {
        FunctionID function;
        Vector<Arrival> arrivals { };
        Vector<Link> links { };
        Vector<TestedCheck> checks { };
        Vector<BlockID, 2> tailCallsProvenByCallees { };
        Vector<Position> positionsGivenUp { };
        // The target is not undefined as long as the source is not: the stored value was read from the source.
        Vector<Link> copies { };
        Vector<Position> variablesThatMayHoldUndefined { };
        Vector<Position> variablesNamed { };
        size_t numberOfChecksThatMayFail { 0 };
    };

    Program& program;
    const FlowResults& flow;
    BuildContext& build;
    Vector<Position> assumed { };
    Vector<Position> assumedWithoutStart { };
    // The same candidates by number: a body asks about every argument, store and test that it has, in every round.
    BitVector numbersOfAssumed { };
    BitVector numbersOfAssumedWithoutStart { };
    // Each with one entry more than there are owners.
    Vector<uint32_t> firstField { };
    Vector<uint32_t> firstEntryParameter { };
    Vector<uint32_t> firstResult { };
    ParallelArray<Body> bodies { };

    ~PositionsProvenByTypes() { build.destroyLater(WTF::move(bodies)); }

    // The variables, then the fields of the layouts, the entry parameters and the results of the functions. None for a position that its owner does not have.
    std::optional<uint32_t> numberOf(Position position) const
    {
        auto numberIn = [&](const Vector<uint32_t>& firstOfOwner) -> std::optional<uint32_t> {
            uint32_t first = firstOfOwner[position.owner];
            if (position.index >= firstOfOwner[position.owner + 1] - first)
                return std::nullopt;
            return first + position.index;
        };
        switch (position.kind) {
        case Position::Kind::Variable:
            return position.owner;
        case Position::Kind::Field:
            return numberIn(firstField);
        case Position::Kind::EntryParameter:
            return numberIn(firstEntryParameter);
        case Position::Kind::Result:
            return numberIn(firstResult);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    bool isAmong(const BitVector& numbers, Position position) const
    {
        std::optional<uint32_t> number = numberOf(position);
        return number && numbers.quickGet(*number);
    }
    bool hasCandidates() const { return !assumed.isEmpty() || !assumedWithoutStart.isEmpty(); }
    bool isAssumed(Position position) const { return isAmong(numbersOfAssumed, position); }
    bool isAssumedWithoutStart(Position position) const { return isAmong(numbersOfAssumedWithoutStart, position); }
    bool isCandidate(Position position) const { return isAssumed(position) || isAssumedWithoutStart(position); }
    bool holdsDeclaredType(Position position) const { return isAssumed(position) || !findDeclaredType(program, position)->isExpected(); }
    Expected<const Type&> declaredTypeOf(Position position) const { return program.at(findDeclaredType(program, position)->expected().predicted()); }
    bool isInside(const Type& type, TypeID other) const { return type.isSubsetOf(program.at(other), program); }

    Expected<Type> typeWithoutStartOf(Position position) const
    {
        Type type = declaredTypeOf(position).predicted();
        type.primitives = type.primitives & ~Primitive::Undefined;
        return type;
    }

    void numberPositions()
    {
        uint32_t next = program.variables.size();
        auto number = [&](Vector<uint32_t>& firstOfOwner, size_t numberOfOwners, const auto& numberOfPositionsOf) {
            firstOfOwner = Vector<uint32_t>(numberOfOwners + 1, [&](size_t owner) {
                uint32_t first = next;
                next += owner < numberOfOwners ? numberOfPositionsOf(owner) : 0;
                return first;
            });
        };
        number(firstField, program.layouts.size(), [&](size_t index) { return program.layouts[index].fields.size(); });
        number(firstEntryParameter, program.functions.size(), [&](size_t index) { return program.functions[index].numberOfEntryParameters(); });
        number(firstResult, program.functions.size(), [&](size_t index) { return program.functions[index].results.size(); });
        numbersOfAssumed = BitVector(next);
        numbersOfAssumedWithoutStart = BitVector(next);
    }

    void noteCandidates()
    {
        numbersOfAssumed.clearAll();
        numbersOfAssumedWithoutStart.clearAll();
        for (Position position : assumed)
            numbersOfAssumed.quickSet(*numberOf(position));
        for (Position position : assumedWithoutStart)
            numbersOfAssumedWithoutStart.quickSet(*numberOf(position));
    }

    // `this`, where it is asked for and the function has it, and the positional parameters.
    void forEachParameterOf(FunctionID id, bool alsoThis, NOESCAPE const Invocable<void(Position)> auto& functor) const
    {
        const Function& function = program.at(id);
        std::optional<unsigned> thisValue = function.thisParameterIndex();
        if (alsoThis && thisValue)
            functor(Position { Position::Kind::EntryParameter, id.index(), *thisValue });
        unsigned first = function.firstParameterIndex();
        for (unsigned parameter = first; parameter < first + function.numberOfPositionalParameters(); ++parameter)
            functor(Position { Position::Kind::EntryParameter, id.index(), parameter });
    }

    void findCandidates()
    {
        numberPositions();
        // Of an indirect call nobody here sees the arguments.
        BitVector isEnteredOtherwise(program.functions.size());
        for (const FlowIndirectCall& call : flow.indirectCalls())
            isEnteredOtherwise.set(call.callee.index());
        assumed = collectInParallel<Position>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Position>& candidates) {
            const Function& function = program.functions[index];
            if (!function.hasBody() || function.isModuleBody())
                return;
            auto addIfExpected = [&](Position position) {
                const DeclaredType* declared = findDeclaredType(program, position);
                if (declared && declared->isExpected() && program.at(declared->written()).isJavaScriptValue())
                    candidates.append(position);
            };
            if (callerStateOf(flow, FunctionID(index)) == CallerState::AllKnown && !isEnteredOtherwise.get(index))
                forEachParameterOf(FunctionID(index), true, addIfExpected);
            if (function.results.size() == 1 && !function.isUnsplitCoroutine())
                addIfExpected({ Position::Kind::Result, index, 0 });
        });

        Vector<Position> assignedByEvaluatedText = findPositionsAssignedByEvaluatedText(program);
        AOT::sort(assignedByEvaluatedText);
        for (uint32_t index = 0; index < program.variables.size(); ++index) {
            if (program.variables[index].initialization != VariableInitialization::Function)
                addVariableIfOpen({ Position::Kind::Variable, index, 0 }, program.variables[index].flow, assignedByEvaluatedText.span());
        }
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            const Layout& layout = program.layouts[index];
            for (uint32_t field = 0; layout.isEnvironment() && field < layout.fields.size(); ++field)
                addVariableIfOpen({ Position::Kind::Field, index, field }, layout.fields[field].flow.location, assignedByEvaluatedText.span());
        }
        AOT::sortInLine(assumed);
        AOT::sortInLine(assumedWithoutStart);
        noteCandidates();
    }

    void addVariableIfOpen(Position position, const LocationFlow& location, std::span<const Position> assignedByEvaluatedText)
    {
        if (location.writers != WriterState::AllKnown || location.storeNaming != StoreNaming::EveryStoreNamesLocation || std::ranges::binary_search(assignedByEvaluatedText, position))
            return;
        const DeclaredType& declared = *findDeclaredType(program, position);
        const Type& type = program.at(declared.written());
        if (!type.isJavaScriptValue() || type.isAnything())
            return;
        // What a variable starts as arrives there too, unless nobody can read it. An allocation lists what it sets.
        bool startsUndefined = position.kind == Position::Kind::Variable && program.variables[position.owner].initialization == VariableInitialization::Undefined;
        if (location.initialization == InitializationState::Unknown && !(type.primitives & (startsUndefined ? Primitive::Undefined : Primitive::Absent)))
            return;
        if (declared.isExpected())
            assumed.append(position);
        if (location.initialization == InitializationState::StoredBeforeEveryRead && !!(type.primitives & Primitive::Undefined) && type != program.at(TypeTable::undefined))
            assumedWithoutStart.append(position);
    }

    void scanStoresIntoVariables(Body& body, const Function& function, const Value& store) const
    {
        bool isAllocation = store.opcode == Opcode::NewObject;
        if (isAllocation && !program.at(store.layout()).isEnvironment())
            return;
        store.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (store.operandKind(operandIndex) != OperandKind::Declared)
                return;
            std::optional<Position> position = storedPosition(program, body.function, { operand, { }, &store, operandIndex });
            if (!position || !isCandidate(*position))
                return;
            body.variablesNamed.append(*position);
            const Type& arriving = program.typeOf(function, operand);
            if (!arriving.isSubsetOf(declaredTypeOf(*position).predicted(), program)) {
                // Of a position that flow has proven the types of the values may know less.
                (isAssumed(*position) ? body.positionsGivenUp : body.variablesThatMayHoldUndefined).append(*position);
                return;
            }
            // A store comes before every read, so nobody reads what the allocation lists for a `var`.
            if (isAllocation && function.at(operand).opcode == Opcode::Undefined && isAssumedWithoutStart(*position))
                return;
            body.arrivals.append({ *position, operand });
            if (!isAssumedWithoutStart(*position) || !(arriving.primitives & Primitive::Undefined))
                return;
            std::optional<Position> source = variableReadThroughCastsAndChecks(function, operand);
            if (source && isAssumedWithoutStart(*source))
                body.copies.append({ *position, *source });
            else
                body.variablesThatMayHoldUndefined.append(*position);
        });
    }

    void giveUp(Body& body, Position position) const
    {
        if (isAssumed(position))
            body.positionsGivenUp.append(position);
    }

    void arrive(Body& body, const Function& function, Position position, ValueID value) const
    {
        if (!isAssumed(position))
            return;
        if (!(value ? program.typeOf(function, value) : program.at(TypeTable::undefined)).isSubsetOf(declaredTypeOf(position).predicted(), program))
            body.positionsGivenUp.append(position);
        else if (value)
            body.arrivals.append({ position, value });
    }

    void scanCall(Body& body, const Function& function, const Value& call) const
    {
        static_assert(OperandIndex::Call::arguments == 2 && OperandIndex::Construct::arguments == 2 && OperandIndex::SuperConstruct::arguments == 2);
        static_assert(OperandIndex::Call::thisValue == 1 && OperandIndex::CallList::thisValue == 1);
        if (call.opcode == Opcode::CallDirect) {
            for (uint32_t index = 0; index < call.numberOfOperands(); ++index)
                arrive(body, function, { Position::Kind::EntryParameter, call.function().index(), index }, call.operand(index));
            return;
        }
        CalleeSetID stamp = call.callees();
        if (!stamp)
            return;
        auto giveUpParameter = [&](Position position) { giveUp(body, position); };
        bool passesThis = call.opcode == Opcode::Call || call.opcode == Opcode::CallList;
        bool passesList = call.opcode != Opcode::Call && call.opcode != Opcode::Construct && call.opcode != Opcode::SuperConstruct;
        // The bound arguments come first, and the bound `this` takes the place of the operand.
        bool mayCallBoundFunction = call.accessSite() && flow.mayCallBoundFunction(call.accessSite());
        for (FunctionID callee : program.at(stamp).followed().value()) {
            if (mayCallBoundFunction) {
                forEachParameterOf(callee, true, giveUpParameter);
                continue;
            }
            const Function& target = program.at(callee);
            if (std::optional<unsigned> thisValue = target.thisParameterIndex()) {
                Position position { Position::Kind::EntryParameter, callee.index(), *thisValue };
                if (passesThis)
                    arrive(body, function, position, call.operand(OperandIndex::Call::thisValue));
                else
                    giveUp(body, position);
            }
            if (passesList) {
                forEachParameterOf(callee, false, giveUpParameter);
                continue;
            }
            unsigned first = target.firstParameterIndex();
            uint32_t numberOfArguments = call.numberOfOperands() - OperandIndex::Call::arguments;
            for (unsigned index = 0; index < target.numberOfPositionalParameters(); ++index)
                arrive(body, function, { Position::Kind::EntryParameter, callee.index(), first + index }, index < numberOfArguments ? call.operand(OperandIndex::Call::arguments + index) : ValueID());
        }
    }

    // False unless whatever the call answers has the tested type as long as the results that are appended hold.
    bool appendResultsTestedBy(const Function& function, ValueID callID, TypeID testedType, Vector<Position, 2>& sources) const
    {
        auto append = [&](FunctionID callee) {
            const Function& target = program.at(callee);
            if (target.results.size() != 1 || target.isUnsplitCoroutine() || !target.hasBody() || !isInside(program.at(target.result().expected().predicted()), testedType))
                return false;
            if (!target.result().isExpected())
                return true;
            Position position { Position::Kind::Result, callee.index(), 0 };
            if (!isAssumed(position))
                return false;
            sources.append(position);
            return true;
        };
        const Value& call = function.at(callID);
        if (call.opcode == Opcode::CallDirect)
            return append(call.function());
        if ((call.opcode != Opcode::Call && call.opcode != Opcode::CallList) || !call.callees())
            return false;
        std::optional<ProvenValue<Callees>> callees = program.at(call.callees()).all();
        return callees && callees->value().builtins.empty() && !callees->value().functions.empty() && std::ranges::all_of(callees->value().functions, append);
    }

    // False unless the check passes as long as the positions that are appended hold.
    bool appendPositionsTestedBy(FunctionID id, const Function& function, const Value& check, TestedCheck& tested) const
    {
        if (std::optional<Position> variable = variableReadThroughCastsAndChecks(function, check.valueOperand())) {
            if (!holdsDeclaredType(*variable))
                return false;
            if (!isInside(declaredTypeOf(*variable).predicted(), check.expectedType().predicted())) {
                if (!isAssumedWithoutStart(*variable) || !isInside(typeWithoutStartOf(*variable).predicted(), check.expectedType().predicted()))
                    return false;
                tested.sourcesWithoutStart.append(*variable);
            }
            if (isAssumed(*variable))
                tested.sources.append(*variable);
            return true;
        }
        ValueID definition = definitionThrough(castsBehindChecks, function, check.valueOperand());
        size_t parameter = function.at(entryBlock).parameters.find(definition);
        if (parameter == notFound || parameter >= function.numberOfEntryParameters())
            return appendResultsTestedBy(function, definition, check.expectedType().predicted(), tested.sources);
        Position position { Position::Kind::EntryParameter, id.index(), static_cast<uint32_t>(parameter) };
        if (!isAssumed(position) || !isInside(declaredTypeOf(position).predicted(), check.expectedType().predicted()))
            return false;
        tested.sources.append(position);
        return true;
    }

    // True while the candidates that are left hold.
    bool passes(FunctionID id, const Function& function, const Value& check) const
    {
        TestedCheck tested;
        return appendPositionsTestedBy(id, function, check, tested);
    }

    bool passes(const TestedCheck& tested) const
    {
        return tested.passesWhileSourcesHold && std::ranges::all_of(tested.sources, [&](Position source) { return isAssumed(source); })
            && std::ranges::all_of(tested.sourcesWithoutStart, [&](Position source) { return isAssumedWithoutStart(source); });
    }

    void scanReturns(Body& body, const Function& function, TypeArena& types) const
    {
        Position result { Position::Kind::Result, body.function.index(), 0 };
        if (!isAssumed(result))
            return;
        TypingContext typing { program, function, types };
        for (uint32_t index = 0; index < function.blocks.size(); ++index) {
            const Terminator& terminator = function.blocks[index].terminator;
            if (terminator.kind == TerminatorKind::Return)
                arrive(body, function, result, terminator.valueOperand());
            if (terminator.kind != TerminatorKind::ReturnCall)
                continue;
            ValueID call = terminator.callOperand();
            Type answered = typing.typeOf(call);
            for (TailCast cast : terminator.casts()) {
                if (cast.resolution == CastResolution::Assume)
                    answered = typeOfConvert(typing, answered, answered, cast.declared);
            }
            Vector<Position, 2> sources;
            if (answered.isSubsetOf(declaredTypeOf(result).predicted(), program))
                body.arrivals.append({ result, call });
            else if (!appendResultsTestedBy(function, call, function.result().expected().predicted(), sources))
                body.positionsGivenUp.append(result);
            else {
                for (Position source : sources)
                    body.links.append({ result, source });
                body.tailCallsProvenByCallees.append(BlockID(index));
            }
        }
    }

    void scan(Body& body) const
    {
        const Function& function = program.at(body.function);
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                switch (value.opcode) {
                case Opcode::Call:
                case Opcode::CallList:
                case Opcode::Construct:
                case Opcode::ConstructList:
                case Opcode::SuperConstruct:
                case Opcode::SuperConstructList:
                case Opcode::CallDirect:
                    scanCall(body, function, value);
                    break;
                case Opcode::Check: {
                    if (!mayTurnValueAway(program, function, value))
                        break;
                    TestedCheck tested { id };
                    tested.passesWhileSourcesHold = appendPositionsTestedBy(body.function, function, value, tested);
                    body.checks.append(WTF::move(tested));
                    break;
                }
                case Opcode::GetModuleVariable:
                case Opcode::GetEnvironmentField:
                    if (Position variable = *variableReadBy(value); isCandidate(variable))
                        body.variablesNamed.append(variable);
                    break;
                case Opcode::PutModuleVariable:
                case Opcode::PutEnvironmentField:
                case Opcode::NewObject:
                    scanStoresIntoVariables(body, function, value);
                    break;
                default:
                    break;
                }
            }
        }
        ScratchTypeArena scratch(program.types);
        scanReturns(body, function, scratch.arena());
    }

    void collectPositionsGivenUp(Body& body) const
    {
        for (const Link& link : body.links) {
            if (!isAssumed(link.source))
                giveUp(body, link.target);
        }
        for (const Link& copy : body.copies) {
            if (isAssumedWithoutStart(copy.target) && !isAssumedWithoutStart(copy.source))
                body.variablesThatMayHoldUndefined.append(copy.target);
        }
        Vector<ValueID, 16> checksThatMayFail;
        for (const TestedCheck& tested : body.checks) {
            if (!passes(tested))
                checksThatMayFail.append(tested.check);
        }
        if (std::exchange(body.numberOfChecksThatMayFail, checksThatMayFail.size()) == checksThatMayFail.size() || body.arrivals.isEmpty())
            return;
        const Function& function = program.at(body.function);
        BitVector isTainted = valuesBehind(function, CFG(function), checksThatMayFail.span());
        for (const Arrival& arrival : body.arrivals) {
            if (!isTainted.get(arrival.value.index()))
                continue;
            if (isAssumed(arrival.position))
                body.positionsGivenUp.append(arrival.position);
            else if (isAssumedWithoutStart(arrival.position))
                body.variablesThatMayHoldUndefined.append(arrival.position);
        }
    }

    // Gives up the candidates that do not hold in the program as it is now. True if it has given one up.
    bool solve()
    {
        size_t numberOfCandidates = assumed.size() + assumedWithoutStart.size();
        // The run in between may have taken back a variable that flow had proven.
        assumedWithoutStart.removeAllMatching([&](Position position) { return !holdsDeclaredType(position); });
        noteCandidates();
        const ParallelLoops& loops = build.parallelLoops();
        build.destroyLater(std::exchange(bodies, { }));
        bodies = collectIntoArrayInParallel<Body>(loops, program.functions.size(), { }, [&](unsigned index, Vector<Body>& found) {
            if (!program.functions[index].hasBody())
                return;
            Body body { FunctionID(index) };
            scan(body);
            bool saysNothing = body.arrivals.isEmpty() && body.links.isEmpty() && body.checks.isEmpty() && body.positionsGivenUp.isEmpty() && body.tailCallsProvenByCallees.isEmpty()
                && body.variablesThatMayHoldUndefined.isEmpty() && body.variablesNamed.isEmpty();
            if (!saysNothing)
                found.append(WTF::move(body));
        });

        // What a body gives up hangs on the candidates that its links, its copies and its checks rest on, and on nothing else. A candidate is twice the number of the position, and one more for the candidate without its start.
        using BodyRestingOnCandidate = std::pair<uint32_t, uint32_t>;
        Vector<BodyRestingOnCandidate> bodiesRestingOnCandidates = collectInParallel<BodyRestingOnCandidate>(loops, bodies.size(), [&](unsigned index, Vector<BodyRestingOnCandidate>& found) {
            const Body& body = bodies[index];
            auto add = [&](Position source, uint32_t isWithoutStart) {
                if (std::optional<uint32_t> number = numberOf(source))
                    found.append({ 2 * *number + isWithoutStart, index });
            };
            for (const Link& link : body.links)
                add(link.source, 0);
            for (const Link& copy : body.copies)
                add(copy.source, 1);
            for (const TestedCheck& tested : body.checks) {
                for (Position source : tested.sources)
                    add(source, 0);
                for (Position source : tested.sourcesWithoutStart)
                    add(source, 1);
            }
        });
        sortAndRemoveDuplicates(loops, bodiesRestingOnCandidates);

        Vector<uint32_t> bodiesToLookAt(bodies.size(), [](size_t index) { return static_cast<uint32_t>(index); });
        bool hasLookedAtEveryBody = false;
        while (hasCandidates()) {
            auto takeInOrder = [&](Vector<Position> Body::* list) {
                Vector<Position> taken = collectInParallel<Position>(loops, bodiesToLookAt.size(), [&](unsigned index, Vector<Position>& found) {
                    found.appendVector(std::exchange(bodies[bodiesToLookAt[index]].*list, { }));
                });
                AOT::sortInLine(taken);
                return taken;
            };
            Vector<Position> givenUp = takeInOrder(&Body::positionsGivenUp);
            Vector<Position> mayHoldUndefined = takeInOrder(&Body::variablesThatMayHoldUndefined);
            Vector<uint32_t> candidatesGivenUp;
            assumed.removeAllMatching([&](Position position) {
                if (!std::ranges::binary_search(givenUp, position))
                    return false;
                candidatesGivenUp.append(2 * *numberOf(position));
                return true;
            });
            assumedWithoutStart.removeAllMatching([&](Position position) {
                if (!std::ranges::binary_search(givenUp, position) && !std::ranges::binary_search(mayHoldUndefined, position))
                    return false;
                candidatesGivenUp.append(2 * *numberOf(position) + 1);
                return true;
            });
            noteCandidates();

            // In the first round every body says which of its checks may fail whatever holds.
            if (std::exchange(hasLookedAtEveryBody, true)) {
                bodiesToLookAt.shrink(0);
                for (uint32_t candidate : candidatesGivenUp) {
                    auto first = std::ranges::lower_bound(bodiesRestingOnCandidates, BodyRestingOnCandidate { candidate, 0 });
                    for (; first != bodiesRestingOnCandidates.end() && first->first == candidate; ++first)
                        bodiesToLookAt.append(first->second);
                }
                AOT::sort(bodiesToLookAt);
                removeRepeatedElements(bodiesToLookAt);
            }

            std::atomic<bool> givesUpMore { false };
            forEachIndexInParallel(loops, bodiesToLookAt.size(), [&](unsigned index) {
                Body& body = bodies[bodiesToLookAt[index]];
                collectPositionsGivenUp(body);
                if (!body.positionsGivenUp.isEmpty() || !body.variablesThatMayHoldUndefined.isEmpty())
                    givesUpMore.store(true, std::memory_order_relaxed);
            });
            if (!givesUpMore.load(std::memory_order_relaxed))
                break;
        }
        return assumed.size() + assumedWithoutStart.size() != numberOfCandidates;
    }

    void stopListingStartsThatNobodyReads(Function& function) const
    {
        for (Value& allocation : function.editAllValues()) {
            if (allocation.opcode != Opcode::NewObject || !program.at(allocation.layout()).isEnvironment())
                continue;
            std::span<const uint32_t> fields = allocation.fields();
            std::span<const ValueID> values = allocation.variadicOperands();
            Vector<uint32_t, 16> remainingFields;
            Vector<ValueID, 16> remainingValues;
            for (size_t index = 0; index < fields.size(); ++index) {
                if (function.at(values[index]).opcode == Opcode::Undefined && isAssumedWithoutStart({ Position::Kind::Field, allocation.layout().index(), fields[index] }))
                    continue;
                remainingFields.append(fields[index]);
                remainingValues.append(values[index]);
            }
            if (remainingFields.size() == fields.size())
                continue;
            allocation.setFields(remainingFields.span());
            allocation.setVariadicOperands(remainingValues.span());
        }
    }

    bool applyTo(Function& function, const Body& body, std::atomic<uint64_t>& numberOfChecks) const
    {
        bool hasAnsweredCheck = false;
        for (const TestedCheck& tested : body.checks) {
            if (!passes(tested))
                continue;
            Value& check = function.edit(tested.check);
            check.becomeAssumeType(check.valueOperand(), Proven::byEveryArrival(check.expectedType()));
            numberOfChecks.fetch_add(1, std::memory_order_relaxed);
            hasAnsweredCheck = true;
        }
        settleProofsBehindAnsweredChecks(function);
        if (!body.variablesNamed.isEmpty())
            stopListingStartsThatNobodyReads(function);
        if (!isAssumed({ Position::Kind::Result, body.function.index(), 0 }))
            return hasAnsweredCheck;
        for (BlockID block : body.tailCallsProvenByCallees) {
            Terminator& terminator = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
            Vector<TailCast, 4> casts;
            for (TailCast cast : terminator.casts())
                casts.append(cast);
            casts.append(TailCast { DeclaredType(Proven::byEveryArrival(function.result().expected())), CastResolution::Assume });
            terminator.setCasts(casts.span());
        }
        return hasAnsweredCheck;
    }

    DeclaredType& edit(Position position) { return *const_cast<DeclaredType*>(findDeclaredType(program, position)); }

    // Only right behind solve(): it names values by their numbers.
    void apply()
    {
        for (Position position : assumed)
            edit(position).establish(Proven::byEveryArrival(edit(position).expected()));
        for (Position position : assumedWithoutStart) {
            ExpectedType withoutStart(program.types.intern(typeWithoutStartOf(position).predicted()));
            edit(position).changeType(Proven::byEveryArrival(withoutStart));
        }

        // Whoever has a proven parameter or result, tests one, or hands a value to one is typed anew. So is whoever names a proven variable.
        BitVector isChanged(program.functions.size());
        for (Position position : assumed) {
            if (position.kind == Position::Kind::EntryParameter || position.kind == Position::Kind::Result)
                isChanged.set(position.owner);
        }
        Vector<const Body*> bodyOfFunction(FillWith { }, program.functions.size(), nullptr);
        for (const Body& body : bodies.span()) {
            bodyOfFunction[body.function.index()] = &body;
            if (std::ranges::any_of(body.variablesNamed, [&](Position variable) { return isCandidate(variable); })
                || std::ranges::any_of(body.checks, [&](const TestedCheck& tested) { return passes(tested); })
                || std::ranges::any_of(body.arrivals, [&](const Arrival& arrival) { return isAssumed(arrival.position); }))
                isChanged.set(body.function.index());
        }
        Vector<FunctionID> changed;
        for (size_t index : isChanged)
            changed.append(FunctionID(index));
        sortLargestFirst(program, build.parallelLoops(), changed);

        std::atomic<uint64_t> numberOfChecks { 0 };
        ProgramEditor editor(program);
        editFunctionsInParallel(editor, build, BehindEdit::Fit, changed.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            const Body* body = bodyOfFunction[id.index()];
            bool hasAnsweredCheck = body && applyTo(function, *body, numberOfChecks);
            {
                AnalysisContext context { build, program, nullptr, nullptr, id };
                FunctionAnalyses analyses(context, function);
                typeValuesFromNever(TypePropagationInputs { program, id, function, analyses, arenas.types, build });
            }
            // What a store took may have rested on a check of that very value.
            if (hasAnsweredCheck)
                assumeTypesOfPositionsAtStores(program, id, function, arenas.types, build);
        });
        build.statistics().count(StatisticID::PositionsProvenByTypes, assumed.size());
        build.statistics().count(StatisticID::ChecksProvenByTypes, numberOfChecks.load(std::memory_order_relaxed));
    }
};

} // anonymous namespace

static Vector<ValueID, 16> findChecks(const Program& program, FunctionID id, const PositionsProvenByTypes* proofs)
{
    const Function& function = program.at(id);
    Vector<ValueID, 16> checks;
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values) {
            const Value& check = function.at(value);
            if (check.opcode == Opcode::Check && mayTurnValueAway(program, function, check) && !(proofs && proofs->passes(id, function, check)))
                checks.append(value);
        }
    }
    return checks;
}

// The rule of such a value reads its own words and the program, and the type of no operand. As read a value has the type that its rule gives, so no check that is erased moves it.
static bool isTypedWithoutOperands(const Function& function, const Value& value)
{
    if (value.opcode == Opcode::ResultAt)
        return function.at(value.resultsOperand()).opcode == Opcode::CallDirect;
    return value.opcode == Opcode::CallDirect || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewObject;
}

static bool tellsObjectsApartByLayout(const Type& type)
{
    return type.layouts && !type.anyObject;
}

static void appendFieldsGivenObjectsThatChecksTurnAway(const Program& program, const Function& function, const CFG& cfg, std::span<const ValueID> checks, Vector<Position>& positions)
{
    Vector<ValueID, 8> checksOfLayouts;
    Vector<ValueID, 8> checksOfLayoutsThatMayFail;
    for (ValueID id : checks) {
        if (!tellsObjectsApartByLayout(program.at(function.at(id).expectedType().predicted())))
            continue;
        checksOfLayouts.append(id);
        if (function.at(id).cause() == CheckCause::MayFail)
            checksOfLayoutsThatMayFail.append(id);
    }
    if (checksOfLayouts.isEmpty())
        return;
    BitVector mayBeTurnedAway = valuesThatAre(function, cfg, checksOfLayoutsThatMayFail.span());
    std::optional<BitVector> isAnsweredForByCheckAlone;
    auto append = [&](FieldReference field) {
        FieldReference declaration = declarationOf(program, field);
        if (tellsObjectsApartByLayout(program.at(fieldAt(program, declaration).declared.expected().predicted())))
            positions.append(positionOfField(declaration));
    };
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& store = function.at(id);
            if (store.opcode == Opcode::NewObject) {
                std::span<const ValueID> initialValues = store.variadicOperands();
                if (program.at(store.layout()).isEnvironment() || store.fields().size() != initialValues.size())
                    continue;
                for (uint32_t index = 0; index < initialValues.size(); ++index) {
                    if (mayBeTurnedAway.get(initialValues[index].index()))
                        append({ store.layout(), store.fields()[index] });
                }
            } else if (store.opcode == Opcode::CopyDataProperties) {
                if (!isAnsweredForByCheckAlone)
                    isAnsweredForByCheckAlone = valuesThatAre(function, cfg, checksOfLayouts.span());
                if (!isAnsweredForByCheckAlone->get(store.sourceOperand().index()))
                    continue;
                for (LayoutID member : membersOf(program, program.typeOf(function, store.targetOperand()))) {
                    for (uint32_t field = 0; field < program.at(member).fields.size(); ++field)
                        append({ member, field });
                }
            } else if ((store.opcode == Opcode::PropDefine || store.opcode == Opcode::PropSet) && mayBeTurnedAway.get(store.valueOperand().index())) {
                for (LayoutID member : membersOf(program, program.typeOf(function, store.objectOperand()))) {
                    if (std::optional<FieldReference> field = findField(program, member, store.key(), BaseChain::Proven))
                        append(*field);
                }
            }
        }
    }
}

// After flow only the very value that a check has turned away counts. The body is left as it was.
static void collectTaintedStores(const Program& program, FunctionID id, Function& body, BuildContext& build, TaintedPositionsRun run, const PositionsProvenByTypes* proofs, Vector<Position>& positions)
{
    const Function& function = body;
    ScratchTypeArena scratch(program.types);
    TypeArena& types = scratch.arena();
    AnalysisContext context { build, program, nullptr, nullptr, id };
    bool isAfterFlow = run == TaintedPositionsRun::AfterFlow;
    Vector<ValueID, 16> checks;
    if (isAfterFlow) {
        checks = findChecks(program, id, proofs);
        if (checks.isEmpty())
            return;
    }

    FunctionAnalyses analyses(context, function);
    if (isAfterFlow)
        appendFieldsGivenObjectsThatChecksTurnAway(program, function, analyses.cfg(), checks.span(), positions);
    TypingContext typing { program, function, types };
    // Forgetting a check changes neither which operands are stores nor what they require.
    Vector<TypedStore, 16> stores = findTypedStores(typing, analyses.cfg());
    if (stores.isEmpty())
        return;
    if (!isAfterFlow)
        checks = findChecks(program, id, proofs);

    // Past form AsRead the type of a value need not be what its rule gives.
    bool isAsRead = program.form() == IRForm::AsRead;
    Vector<ValueID, 16> valuesThatMayHaveOtherTypeWithoutChecks;
    for (const TypedStore& store : stores) {
        if (!checks.isEmpty() && (!isAsRead || (!isTypedWithoutOperands(function, function.at(store.stored)) && storedPosition(program, id, store))))
            valuesThatMayHaveOtherTypeWithoutChecks.append(store.stored);
    }
    BitVector storedValuesThatCount;
    if (!valuesThatMayHaveOtherTypeWithoutChecks.isEmpty()) {
        storedValuesThatCount = isAfterFlow ? valuesThatAre(function, analyses.cfg(), checks.span()) : valuesBehind(function, analyses.cfg(), checks.span());
        valuesThatMayHaveOtherTypeWithoutChecks.removeAllMatching([&](ValueID stored) { return !storedValuesThatCount.get(stored.index()); });
    }
    if (valuesThatMayHaveOtherTypeWithoutChecks.isEmpty()) {
        if (!isAfterFlow)
            appendIllTypedStores(typing, id, stores.span(), nullptr, positions);
        return;
    }

    BitVector valuesBehindChecksAfterFlow;
    if (isAfterFlow)
        valuesBehindChecksAfterFlow = valuesBehind(function, analyses.cfg(), checks.span());
    const BitVector& isBehindCheck = isAfterFlow ? valuesBehindChecksAfterFlow : storedValuesThatCount;
    bool valuesAreTypedAsDefined = function.valuesAreTypedAsDefined();
    UndoLog undoLog;
    {
        // A cast becomes another cast of the same operand: the graph and the use lists of the body are those of the body without its checks.
        TypePropagation propagation(TypePropagationInputs { program, id, body, analyses, types, build });
        propagation.restartSourcesOf(valuesThatMayHaveOtherTypeWithoutChecks.span(), isBehindCheck);
        const BitVector& isTypedAnew = propagation.restartedValues();
        // A check answers a claim of flow that says no more than the check does, whatever the claim rests on, and the claim is gone when the check goes.
        for (size_t index : isTypedAnew) {
            const Value& claim = function.values[index];
            if (claim.opcode != Opcode::AssumeType || claim.isWordOfFrontEnd() || claim.isBehindChecks())
                continue;
            undoLog.willEdit(function, ValueID(index));
            body.edit(ValueID(index)).setIsBehindChecks(true);
        }
        // A check that has nothing left to test in the typed code is dropped in front of the generic version, so it holds nothing back there.
        forgetProofsOf(body, types, isTypedAnew, isTypedAnew, ExpectedTypes::Dropped, &undoLog);
        propagation.solveTypes();
        appendIllTypedStores(typing, id, stores.span(), isAfterFlow ? &storedValuesThatCount : nullptr, positions);
        undoLog.undo(body);
        propagation.restoreTypes();
    }
    if (valuesAreTypedAsDefined)
        body.setValuesAreTypedAsDefined();
}

bool mayReturnTaintedValue(const Program& program, FunctionID id, Function& body, BuildContext& build)
{
    Vector<Position> positions;
    collectTaintedStores(program, id, body, build, TaintedPositionsRun::BeforeFlow, nullptr, positions);
    return std::ranges::any_of(positions, [&](Position position) { return position.kind == Position::Kind::Result && position.owner == id.index(); });
}

namespace {

struct GuardedRead {
    ValueID value;
    ProgramPoint insertionPoint;
    DeclaredType declared;
    Vector<Use, 4> uses;
};

// What the operations of a body read, by their own words and the types of their operands. It holds until the body is edited.
struct ReadsOfBody {
    bool isListed { false };
    // Ascending, each once.
    Vector<Position> positions;
    Vector<ValueID> typedReadsByName;
};

// What a round has to know of a body without touching its list.
enum class BodyToAsk : uint8_t {
    // The table does not hold all that the body reads: its list is not made, or names an owner that the list before it did not name.
    IsOutsideTable = 1 << 0,
    HasTypedReadsByName = 1 << 1,
};

// Which bodies read a position of an owner: a variable, the fields of a layout, the results of a function.
struct ReadersByOwner {
    uint32_t firstLayout;
    uint32_t firstFunction;
    Vector<uint32_t> firstReader { };
    Vector<FunctionID> readers { };

    ReadersByOwner(const Program& program, const ParallelLoops& loops, std::span<const ReadsOfBody> readsOfBodies)
        : firstLayout(program.variables.size())
        , firstFunction(firstLayout + program.layouts.size())
    {
        size_t numberOfOwners = firstFunction + program.functions.size();
        firstReader.fill(0, numberOfOwners + 1);
        auto forEachOwnerAndBody = [&](const auto& functor) {
            forEachIndexInParallel(loops, readsOfBodies.size(), [&](unsigned index) {
                // The positions of one owner stand together.
                std::optional<uint32_t> last;
                for (Position position : readsOfBodies[index].positions) {
                    std::optional<uint32_t> owner = ownerOf(position);
                    if (owner && owner != last)
                        functor(*owner, FunctionID(index));
                    last = owner;
                }
            });
        };
        forEachOwnerAndBody([&](uint32_t owner, FunctionID) { AtomicRef(firstReader[owner + 1]).fetch_add(1, std::memory_order_relaxed); });
        for (size_t owner = 0; owner < numberOfOwners; ++owner)
            firstReader[owner + 1] += firstReader[owner];
        readers.grow(firstReader[numberOfOwners]);
        Vector<uint32_t> next = firstReader;
        // The order inside a row is that of arrival. Nobody reads it: a reader is a mark on a body.
        forEachOwnerAndBody([&](uint32_t owner, FunctionID body) { readers[AtomicRef(next[owner]).fetch_add(1, std::memory_order_relaxed)] = body; });
    }

    // None: nobody reads an entry parameter but its own function.
    std::optional<uint32_t> ownerOf(Position position) const
    {
        switch (position.kind) {
        case Position::Kind::Variable:
            return position.owner;
        case Position::Kind::Field:
            return firstLayout + position.owner;
        case Position::Kind::Result:
            return firstFunction + position.owner;
        case Position::Kind::EntryParameter:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
};

} // anonymous namespace

static ReadsOfBody listReads(const Program& program, const Function& function)
{
    ReadsOfBody reads { .isListed = true };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (std::optional<Position> position = readPosition(program, function, function.at(id)))
                reads.positions.append(*position);
            else if (isTypedFromFieldsUnderName(program, function, function.at(id)))
                reads.typedReadsByName.append(id);
        }
    }
    AOT::sortInLine(reads.positions);
    removeRepeatedElements(reads.positions);
    reads.positions.shrinkToFit();
    reads.typedReadsByName.shrinkToFit();
    return reads;
}

static bool readsMarkedPosition(const Program& program, FunctionID id, const ReadsOfBody& reads, std::span<const Position> marked)
{
    const Function& function = program.at(id);
    // The positions of one kind and one owner stand together, by index.
    auto hasMarkedPositionOfItsOwn = [&](Position::Kind kind, size_t numberOfPositions) {
        auto first = std::ranges::lower_bound(marked, Position { kind, id.index(), 0 });
        return first != marked.end() && first->kind == kind && first->owner == id.index() && first->index < numberOfPositions;
    };
    if (hasMarkedPositionOfItsOwn(Position::Kind::EntryParameter, function.numberOfEntryParameters()) || hasMarkedPositionOfItsOwn(Position::Kind::Result, function.results.size()))
        return true;
    std::span<const Position> fewer = reads.positions.span();
    std::span<const Position> more = marked;
    if (fewer.size() > more.size())
        std::swap(fewer, more);
    if (std::ranges::any_of(fewer, [&](Position position) { return std::ranges::binary_search(more, position); }))
        return true;
    return !reads.typedReadsByName.isEmpty() && hasMarkedField(marked) && std::ranges::any_of(reads.typedReadsByName, [&](ValueID value) { return readsMarkedFieldByName(program, function, function.at(value), marked); });
}

static bool namesOwnerThatIsNotIn(std::span<const Position> ascendingPositions, std::span<const Position> otherAscendingPositions)
{
    auto isBefore = [](Position a, Position b) { return std::tie(a.kind, a.owner) < std::tie(b.kind, b.owner); };
    size_t other = 0;
    for (Position position : ascendingPositions) {
        while (other < otherAscendingPositions.size() && isBefore(otherAscendingPositions[other], position))
            ++other;
        if (other == otherAscendingPositions.size() || isBefore(position, otherAscendingPositions[other]))
            return true;
    }
    return false;
}

static Vector<GuardedRead> collectGuardedReads(const Program& program, FunctionID id, const Function& function, FunctionAnalyses& analyses, std::span<const Position> marked, Vector<ValueID, 16>& valuesReadingMarkedPositions)
{
    const UseLists& useLists = analyses.useLists();
    Vector<GuardedRead> reads;
    auto addRead = [&](ValueID value, const DeclaredType& declared) {
        valuesReadingMarkedPositions.append(value);
        GuardedRead read { value, useLists.pointAfter(value), declared, { } };
        const BlockValues& values = function.at(read.insertionPoint.block).values;
        while (read.insertionPoint.index < values.size() && function.at(values[read.insertionPoint.index]).opcode == Opcode::ResultAt)
            ++read.insertionPoint.index;
        for (const Use& use : useLists.usesOf(value)) {
            if (acceptsCastOfValue(function, use))
                read.uses.append(use);
        }
        if (!read.uses.isEmpty())
            reads.append(WTF::move(read));
    };
    auto addReadIfMarked = [&](ValueID value, Position position) {
        if (std::ranges::binary_search(marked, position))
            addRead(value, *findDeclaredType(program, position));
    };
    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
    uint32_t numberOfEntryParameters = std::min<uint32_t>(function.numberOfEntryParameters(), entryParameters.size());
    for (uint32_t index = 0; index < numberOfEntryParameters; ++index)
        addReadIfMarked(entryParameters[index], { Position::Kind::EntryParameter, id.index(), index });
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (std::optional<Position> position = readPosition(program, function, function.at(value)))
                addReadIfMarked(value, *position);
            else if (leavesOutLayoutsByField(program, function, function.at(value))) {
                // It is typed anew and lets those layouts through.
                if (readsMarkedFieldByName(program, function, function.at(value), marked))
                    valuesReadingMarkedPositions.append(value);
            } else if (keyOfTypedReadByName(program, function.at(value)) && readsMarkedFieldByName(program, function, function.at(value), marked))
                addRead(value, DeclaredType(ExpectedType(function.at(value).type)));
        }
    }
    return reads;
}

static void resolveTailCasts(const TypingContext& typing, Function& function)
{
    bool hasExpectedResult = function.results.size() == 1 && function.result().isExpected();
    for (Block& block : function.editAllBlocks()) {
        Terminator& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;
        Type source = typing.typeOf(terminator.callOperand());
        Vector<TailCast, 4> casts;
        for (TailCast cast : terminator.casts()) {
            // In the run after flow: what flow has proven of the call stays proven. Resolved again it would be a check, which ends the tail call.
            bool isProvenByFlow = cast.resolution == CastResolution::Assume;
            if (hasExpectedResult && !isProvenByFlow) {
                cast.declared.expect(cast.declared.expected());
                cast.declared.remove(PositionFlags::IsInt32Promise);
            }
            if (!isProvenByFlow)
                cast.resolution = resolveConvert(typing, source, cast.declared).cast;
            source = typeOfConvert(typing, source, source, cast.declared);
            casts.append(cast);
        }
        terminator.setCasts(casts.span());
    }
}

static uint32_t insertReadChecks(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build, std::span<const Position> marked)
{
    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    TypingMemo typingMemo;
    analyses.setTypingMemo(&typingMemo);
    TypingContext typing { program, function, types };
    uint32_t numberOfChecks = 0;
    IRBuilder builder(context, function, types, &analyses);
    Vector<ValueID, 16> changedValues;
    Vector<ValueID, 8> checks;
    for (const GuardedRead& read : collectGuardedReads(program, id, function, analyses, marked, changedValues)) {
        DeclaredType checked = read.declared;
        checked.establish(Proven::byConversion(read.declared.expected()));
        ConvertResolution resolution = resolveConvert(typing, typing.typeOf(read.declared), checked);
        if (resolution.cast != CastResolution::Check)
            continue;
        Value check = IRBuilder::make(Opcode::Check, { read.value });
        check.setExpectedType(ExpectedType(resolution.castType));
        check.setSite(checked.site);
        check.setCause(CheckCause::AddedByPass);
        builder.setOrigin(function.at(read.value).origin());
        ValueID checkID = builder.insertBefore(read.insertionPoint, WTF::move(check));
        for (const Use& use : read.uses)
            replaceUse(function, use, checkID);
        changedValues.append(checkID);
        checks.append(checkID);
        ++numberOfChecks;
    }
    builder.applyInsertions();
    // A position that is taken back takes along what flow has said of the value that is read from it.
    auto isClaimInEveryVersion = [](const Value& claim) { return claim.opcode == Opcode::AssumeType && !claim.isBehindChecks() && !claim.isWordOfFrontEnd(); };
    if (std::ranges::any_of(function.values, isClaimInEveryVersion)) {
        for (size_t index : valuesThatAre(function, analyses.cfg(), checks.span())) {
            if (isClaimInEveryVersion(function.values[index]))
                function.edit(ValueID(index), ValueEdit::Definition).setIsBehindChecks(true);
        }
    }

    // A check has the type that its read had, so most of what is typed from the read keeps its types, and nothing behind such a value needs a look.
    if (!analyses.cfg().hasCycle()) {
        BitVector isTypedFromChangedValue = valuesBehind(function, analyses.cfg(), changedValues.span());
        for (ValueID changed : changedValues)
            isTypedFromChangedValue.quickClear(changed.index());
        for (size_t index : isTypedFromChangedValue) {
            if (function.values[index].opcode == Opcode::Upcast)
                changedValues.append(ValueID(index));
        }
    }
    narrowValueTypesBehindChanges(TypePropagationInputs { program, id, function, analyses, types, build, &typingMemo }, changedValues.span());
    storeUnprovenInitialValues(program, id, function, types, build);
    resolveTailCasts(typing, function);
    return numberOfChecks;
}

void expectFields(Program& program, std::span<const FieldReference> fields, BuildContext& build)
{
    for (FieldReference field : fields)
        markExpected(program, positionOfField(field));
    markEnforcedFields(program, build);
}

static uint64_t guardTaintedPositions(Program& program, BuildContext& build, TaintedPositionsRun run, const PositionsProvenByTypes* proofs, std::span<const FieldReference> fieldsTakenBack = { })
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    const ParallelLoops& loops = build.parallelLoops();
    ProgramEditor editor(program);
    Vector<FunctionID> worklist = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& functionsWithBody) {
        if (program.functions[index].hasBody())
            functionsWithBody.append(FunctionID(index));
    });
    sortLargestFirst(program, loops, worklist);

    uint64_t numberOfPositions = 0;
    std::atomic<uint64_t> numberOfChecks { 0 };
    bool markedObjectField = false;
    // Flow has looked at the text that may be evaluated.
    Vector<Position> assignedByEvaluatedText;
    if (run == TaintedPositionsRun::BeforeFlow)
        assignedByEvaluatedText = findPositionsAssignedByEvaluatedText(program);
    Vector<ReadsOfBody> readsOfBodies(program.functions.size());
    Vector<OptionSet<BodyToAsk>> bodiesToAsk(FillWith { }, program.functions.size(), OptionSet<BodyToAsk> { BodyToAsk::IsOutsideTable });
    std::optional<ReadersByOwner> readers;
    // A body that was edited keeps its list until the new one is made, which is held against it.
    auto listReadsOf = [&](FunctionID id) {
        ReadsOfBody& reads = readsOfBodies[id.index()];
        if (reads.isListed)
            return;
        ReadsOfBody listed = listReads(program, program.at(id));
        OptionSet<BodyToAsk>& toAsk = bodiesToAsk[id.index()];
        if (!readers)
            toAsk.remove(BodyToAsk::IsOutsideTable);
        else if (namesOwnerThatIsNotIn(listed.positions.span(), reads.positions.span()))
            toAsk.add(BodyToAsk::IsOutsideTable);
        toAsk.set(BodyToAsk::HasTypedReadsByName, !listed.typedReadsByName.isEmpty());
        reads = WTF::move(listed);
    };
    while (!worklist.isEmpty()) {
        Vector<Vector<Position>> storesByFunction(worklist.size());
        build.setRecordsLimitHits(false);
        {
            ProgramCaches caches(program, build);
            // No task reads the values of another task's body: typing a call reads the record of the callee.
            forEachIndexInParallel(loops, worklist.size(), [&](unsigned index) {
                collectTaintedStores(program, worklist[index], program.at(worklist[index]), build, run, proofs, storesByFunction[index]);
                listReadsOf(worklist[index]);
            });
        }
        build.setRecordsLimitHits(true);

        Vector<Position> marked = std::exchange(assignedByEvaluatedText, { });
        for (Vector<Position>& stores : storesByFunction)
            marked.appendVector(WTF::move(stores));
        AOT::sortInLine(marked);
        removeRepeatedElements(marked);
        marked.removeAllMatching([&](Position position) { return !markExpected(program, position); });
        for (FieldReference field : std::exchange(fieldsTakenBack, { }))
            marked.append(positionOfField(field));
        AOT::sortInLine(marked);
        if (marked.isEmpty())
            break;
        numberOfPositions += marked.size();
        bool markedObjectFieldInRound = std::ranges::any_of(marked, [&](Position position) { return position.kind == Position::Kind::Field && !program.layouts[position.owner].isEnvironment(); });
        if (markedObjectFieldInRound)
            markEnforcedFields(program, build);
        markedObjectField |= markedObjectFieldInRound;
        markedObjectField |= markNamespaceFields(program, marked.span());

        if (!readers)
            readers.emplace(program, loops, readsOfBodies.span());
        Vector<bool> mayReadMarkedPosition(FillWith { }, program.functions.size(), false);
        std::optional<uint32_t> lastOwner;
        for (Position position : marked) {
            if (position.kind == Position::Kind::EntryParameter || position.kind == Position::Kind::Result)
                mayReadMarkedPosition[position.owner] = true;
            std::optional<uint32_t> owner = readers->ownerOf(position);
            if (!owner || owner == lastOwner)
                continue;
            lastOwner = owner;
            for (FunctionID reader : entriesOfGroup(readers->firstReader, readers->readers, *owner))
                mayReadMarkedPosition[reader.index()] = true;
        }
        OptionSet<BodyToAsk> askedWhatever { BodyToAsk::IsOutsideTable };
        if (hasMarkedField(marked.span()))
            askedWhatever.add(BodyToAsk::HasTypedReadsByName);
        Vector<FunctionID> bodiesToLookAt;
        for (unsigned index = 0; index < program.functions.size(); ++index) {
            if (mayReadMarkedPosition[index] || bodiesToAsk[index].containsAny(askedWhatever))
                bodiesToLookAt.append(FunctionID(index));
        }
        worklist = collectInParallel<FunctionID>(loops, bodiesToLookAt.size(), [&](unsigned index, Vector<FunctionID>& found) {
            FunctionID id = bodiesToLookAt[index];
            if (!program.at(id).hasBody())
                return;
            listReadsOf(id);
            if (readsMarkedPosition(program, id, readsOfBodies[id.index()], marked.span()))
                found.append(id);
        });
        sortLargestFirst(program, loops, worklist);
        editFunctionsInParallel(editor, build, BehindEdit::Fit, worklist.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            numberOfChecks.fetch_add(insertReadChecks(program, id, function, arenas.types, build, marked.span()), std::memory_order_relaxed);
            if (run == TaintedPositionsRun::AfterFlow)
                assumeTypesOfPositionsAtStores(program, id, function, arenas.types, build);
            readsOfBodies[id.index()].isListed = false;
        });
    }
    build.destroyLater(WTF::move(readsOfBodies));

    if (markedObjectField)
        program.indexNames(loops);
    if (run == TaintedPositionsRun::BeforeFlow && !program.hasGuardedReads()) {
        program.setHasGuardedReads();
        Vector<FunctionID> functionsWithoutChecks = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
            if (program.functions[index].hasBody() && !waitsForGenericVersion(program, program.functions[index]))
                found.append(FunctionID(index));
        });
        sortLargestFirst(program, loops, functionsWithoutChecks);
        editFunctionsInParallel(editor, build, BehindEdit::Fit, functionsWithoutChecks.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            AnalysisContext context { build, program, nullptr, nullptr, id };
            FunctionAnalyses analyses(context, function);
            finishTypedValues(TypePropagationInputs { program, id, function, analyses, arenas.types, build });
        });
    }
    build.statistics().count(StatisticID::TaintedPositions, numberOfPositions);
    build.statistics().count(StatisticID::ChecksOnTaintedPositions, numberOfChecks.load(std::memory_order_relaxed));
    return numberOfPositions;
}

void guardTaintedPositions(Program& program, BuildContext& build, TaintedPositionsRun run)
{
    guardTaintedPositions(program, build, run, nullptr);
}

// A position that is taken back gets checks at its reads, which may fail. So the proofs are looked at again behind each run that has taken one back, and nothing is applied before both agree.
void guardTaintedPositionsAfterFlow(Program& program, const FlowResults& flow, BuildContext& build)
{
    PositionsProvenByTypes proofs { program, flow, build };
    proofs.findCandidates();
    std::span<const FieldReference> fieldsTakenBack = flow.fieldsTakenBack();
    if (!proofs.hasCandidates()) {
        guardTaintedPositions(program, build, TaintedPositionsRun::AfterFlow, nullptr, fieldsTakenBack);
        return;
    }
    proofs.solve();
    while (guardTaintedPositions(program, build, TaintedPositionsRun::AfterFlow, &proofs, std::exchange(fieldsTakenBack, { })) && proofs.solve()) { }
    if (proofs.hasCandidates())
        proofs.apply();
}

// The typed body that stands behind the tests at the entrance of a function has a generic version of the whole function beside it.
static FunctionID findGenericVersionBeside(const Program& program, FunctionID id, const Function& function)
{
    if (function.role != FunctionRole::Source || !function.derivation.source || function.results.size() != 1 || !function.hasOrdinaryFrame())
        return { };
    const auto* record = program.at(function.derivation.source).creator.tryAs<CreatorRecord::EntryChecks>();
    return record && record->body == id && program.at(record->fallback).hasBody() ? record->fallback : FunctionID();
}

BitVector findBlocksThatEndBeforeAnyEffect(FunctionPassContext& pass)
{
    const CFG& cfg = pass.analyses.cfg();
    // Nobody can tell that it ran, so nobody can tell that it ran twice.
    auto canRunTwice = [&](const Effects& effects) {
        return (effects.writes.containsOnly(heapsWhoseWritesNobodySees) && !effects.mayRunOtherCode()) || pass.build.isSabotaged(Sabotage::WritesMayRunTwice);
    };
    auto canRunTwiceOnAnyEdge = [&](const EffectsOnEdges& effects) { return canRunTwice(effects.normal) && canRunTwice(effects.handler); };
    // Once a tail call has jumped back here, the parameters of the entry block belong to an earlier call.
    auto beginsNextCall = [&](BlockID block) {
        return pass.analyses.isInCycle(block) && std::ranges::any_of(pass.function.at(block).values, [&](ValueID id) {
            const Value& value = pass.function.at(id);
            return value.opcode == Opcode::LoopHint && value.replacesTailCalls();
        });
    };
    BitVector endsBeforeAnyEffect(pass.function.blocks.size());
    for (BlockID block : cfg.reversePostOrder()) {
        if (!beginsNextCall(block) && canRunTwiceOnAnyEdge(pass.analyses.effectsOfTerminator(block)) && std::ranges::all_of(pass.function.at(block).values, [&](ValueID id) { return canRunTwiceOnAnyEdge(pass.analyses.effectsOf(id)); }))
            endsBeforeAnyEffect.quickSet(block.index());
    }
    for (bool changed = true; std::exchange(changed, false);) {
        for (BlockID block : cfg.reversePostOrder()) {
            if (endsBeforeAnyEffect.quickGet(block.index()) && std::ranges::any_of(cfg.predecessors(block), [&](BlockID predecessor) { return !endsBeforeAnyEffect.quickGet(predecessor.index()); })) {
                endsBeforeAnyEffect.quickClear(block.index());
                changed = true;
            }
        }
    }
    return endsBeforeAnyEffect;
}

static BlockID emitCallOfGenericVersion(IRBuilder& builder, FunctionID genericVersion)
{
    BlockID block = builder.addBlock(BlockFrequency::Rare);
    builder.moveToEndOf(block);
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(genericVersion);
    call.setArgumentsOperands(builder.function().at(entryBlock).parameters.span());
    call.setIsNeverInlined(true);
    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { builder.append(WTF::move(call)) });
    terminator.setRequirement(tailCallRequirementOf(builder.context().program.at(genericVersion)));
    builder.emitTerminator(WTF::move(terminator));
    return block;
}

// The callers are compiled by the row that the function has now. The text of the generic version says more than the code that the call stands for.
static BlockID emitCallOfGenericVersion(FunctionPassContext& pass, IRBuilder& builder, FunctionID genericVersion)
{
    if (!pass.function.effectsBeforeRestart && pass.inputs.effects.contains(pass.id))
        pass.function.effectsBeforeRestart = std::make_shared<const EffectsOnEdges>(pass.inputs.effects.of(pass.id).onCall());
    return emitCallOfGenericVersion(builder, genericVersion);
}

static BlockID findCallOfGenericVersion(FunctionPassContext& pass, FunctionID genericVersion)
{
    const Function& function = pass.function;
    for (BlockID id : pass.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        if (block.frequency.predicted() != BlockFrequency::Rare || block.values.size() != 1 || !block.parameters.isEmpty() || block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        const Value& call = function.at(block.values[0]);
        if (call.opcode == Opcode::CallDirect && call.function() == genericVersion && call.isNeverInlined() && std::ranges::equal(call.variadicOperands(), function.at(entryBlock).parameters.span()))
            return id;
    }
    return { };
}

static Vector<Edge, 16> findFailingEdgesBeforeAnyEffect(FunctionPassContext& pass, BlockID restart)
{
    const Function& function = pass.function;
    BitVector endsBeforeAnyEffect = findBlocksThatEndBeforeAnyEffect(pass);
    Vector<Edge, 16> failingEdges;
    for (BlockID id : pass.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        if (!endsBeforeAnyEffect.quickGet(id.index()) || block.frequency.predicted() != BlockFrequency::Normal || block.handler || !block.terminator.isConditionalBranch())
            continue;
        // What fails without being asked goes where the test behind it would send it.
        const Terminator& branch = function.at(block.terminator.conditionOperand()).opcode == Opcode::ShouldFailPrediction ? function.at(block.terminator.notTaken().block).terminator : block.terminator;
        if (!branch.isConditionalBranch())
            continue;
        const Value& test = function.at(branch.conditionOperand());
        // A test of a type may be a guess, or tell the members of a union apart: the side on which it fails is then an ordinary way through the function, which is worth its own code.
        bool testsState = provesForStretch(provesOf(test.opcode)) && provesOf(test.opcode) != Proof::ExactFunction;
        // What fails such a test is not what its annotation says, as what fails a test at the entrance, or it is undefined or null where the program goes on as if it had an object: it is about to throw.
        if (!testsState && (test.opcode != Opcode::TypeTest || !pass.program.expectedOrTypeOf(function, test.valueOperand()).predicted().without(pass.program.at(test.testedType()), pass.types).hasOnlyPrimitives(Primitive::UndefinedOrNull)))
            continue;
        for (uint32_t index = 0; index < block.terminator.targets.size(); ++index) {
            BlockID target = block.terminator.targets[index].block;
            Expected<BlockFrequency> frequency = function.at(target).frequency;
            if (target != restart && (testsState ? frequency.predicted() != BlockFrequency::Normal : frequency.predicted() == BlockFrequency::Rare))
                failingEdges.append(Edge { id, index });
        }
    }
    return failingEdges;
}

// What a call that begins again has made is made a second time. So it is made behind the test: from the first value of the block that allocates on, whatever the test does not ask for.
// Nothing that may throw is passed on the way, a result stays with the operation that it is a result of, and what takes the caught value stays at the head of its handler.
static bool moveAllocationsBehindTestsThatRestart(FunctionPassContext& pass, BlockID restart)
{
    Function& function = pass.function;
    const CFG& cfg = pass.analyses.cfg();
    BitVector endsBeforeAnyEffect = findBlocksThatEndBeforeAnyEffect(pass);
    BitVector isAskedFor(function.values.size());
    BitVector isMoved(function.values.size());
    Vector<ValueID, 16> moved;
    bool hasMoved = false;
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        if (!endsBeforeAnyEffect.quickGet(id.index()) || block.handler || !block.terminator.isConditionalBranch())
            continue;
        const Target& taken = block.terminator.taken();
        const Target& notTaken = block.terminator.notTaken();
        if ((taken.block == restart) == (notTaken.block == restart))
            continue;
        BlockID passing = taken.block == restart ? notTaken.block : taken.block;
        if (cfg.incomingEdges(passing).size() != 1 || function.at(passing).handler || function.at(passing).isHandlerBlock())
            continue;

        block.terminator.forEachChild([&](ValueID operand) { isAskedFor.quickSet(operand.index()); });
        for (const Target& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                isAskedFor.quickSet(argument.index());
        }
        uint32_t firstThatMayMove = 0;
        for (uint32_t index = block.values.size(); index--;) {
            ValueID value = block.values[index];
            if (!isAskedFor.quickGet(value.index()))
                continue;
            function.at(value).forEachChild([&](ValueID operand) { isAskedFor.quickSet(operand.index()); });
            if (!firstThatMayMove && pass.analyses.effectsOf(value).normal.mayExitSideways())
                firstThatMayMove = index + 1;
        }
        firstThatMayMove = std::max(firstThatMayMove, indexAfterCaughtValues(function, block));
        for (ValueID value : std::exchange(moved, { }))
            isMoved.quickClear(value.index());
        bool everyValueCompletes = true;
        for (uint32_t index = firstThatMayMove; index < block.values.size() && everyValueCompletes; ++index) {
            ValueID value = block.values[index];
            if (isAskedFor.quickGet(value.index()))
                continue;
            const Effects& effects = pass.analyses.effectsOf(value).normal;
            everyValueCompletes = effects.alwaysCompletes();
            if (moved.isEmpty() && !effects.mayCollect)
                continue;
            if (function.at(value).opcode == Opcode::ResultAt && !isMoved.quickGet(function.at(value).resultsOperand().index()))
                continue;
            isMoved.quickSet(value.index());
            moved.append(value);
        }
        if (moved.isEmpty() || !everyValueCompletes)
            continue;
        function.edit(id, BlockEdit::Values).values.removeAllMatching([&](ValueID value) { return isMoved.quickGet(value.index()); });
        function.edit(passing, BlockEdit::Values).values.insertVector(0, moved);
        hasMoved = true;
    }
    return hasMoved;
}

FunctionID findGenericVersionToRestartIn(FunctionPassContext& pass)
{
    const Program& program = pass.program;
    const Function& function = pass.function;
    FunctionID genericVersion = findGenericVersionBeside(program, pass.id, function);
    if (!genericVersion || function.at(entryBlock).parameters.size() != program.at(genericVersion).at(entryBlock).parameters.size())
        return { };
    const Function& generic = program.at(genericVersion);
    TypingContext typing { program, function, pass.types };
    if (!typing.typeOf(generic.result()).isSubsetOf(typing.typeOf(function.result()), program))
        return { };
    for (unsigned index = 0; index < generic.numberOfEntryParameters(); ++index) {
        if (!program.typeOf(function, function.at(entryBlock).parameters[index]).isSubsetOf(program.at(declaredEntryTypeOf(typing, generic, index)), program))
            return { };
    }
    return genericVersion;
}

BlockID ensureCallOfGenericVersion(FunctionPassContext& pass, IRBuilder& builder, FunctionID genericVersion)
{
    if (BlockID restart = findCallOfGenericVersion(pass, genericVersion))
        return restart;
    return emitCallOfGenericVersion(pass, builder, genericVersion);
}

// A test of the state of an object that fails leads into generic code for what follows it, which the function holds for itself.
PassOutcome restartInGenericVersion(FunctionPassContext& pass)
{
    Function& function = pass.function;
    FunctionID genericVersion = findGenericVersionToRestartIn(pass);
    if (!genericVersion)
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), function, pass.types, &pass.analyses);
    builder.setOrigin({ function.position, 0 });
    BlockID restart = findCallOfGenericVersion(pass, genericVersion);
    size_t numberOfRestarts = 0;
    // Generic code that has gone no longer joins the code behind it, which may then stand in front of every effect too.
    while (true) {
        Vector<Edge, 16> failingEdges = findFailingEdgesBeforeAnyEffect(pass, restart);
        if (failingEdges.isEmpty())
            break;
        if (!restart)
            restart = emitCallOfGenericVersion(pass, builder, genericVersion);
        for (Edge edge : failingEdges)
            builder.redirectEdge(edge, Target { restart, { }, { } });
        builder.emptyUnreachableBlocks();
        numberOfRestarts += failingEdges.size();
    }
    bool hasMoved = restart && moveAllocationsBehindTestsThatRestart(pass, restart);
    if (!numberOfRestarts && !hasMoved)
        return PassOutcome::Unchanged;
    pass.build.statistics().count(StatisticID::RestartsInGenericVersion, numberOfRestarts);
    // The graph is that of the last search, which has edited nothing.
    pass.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

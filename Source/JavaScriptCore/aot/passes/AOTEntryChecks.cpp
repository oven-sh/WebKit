/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSplitBehindEntryChecks.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCheckTypesAtEntry.h"
#include "AOTCreateEntrypointsInternal.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectsCheck.h"
#include "AOTErrorMessages.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTPredictedValues.h"
#include "AOTProgramPasses.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isPassedByCaller(const Function& function, unsigned entryParameterIndex)
{
    switch (function.entryParameterKind(entryParameterIndex)) {
    case EntryParameter::This:
        return true;
    case EntryParameter::Parameter:
        return entryParameterIndex - function.firstParameterIndex() < function.numberOfPositionalParameters();
    case EntryParameter::Capture:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const DeclaredType* findTypeCheckedAtEntry(const TypeTable& types, const Function& function, unsigned index)
{
    if (!isPassedByCaller(function, index))
        return nullptr;
    const DeclaredType* declared = function.declaredEntryParameter(index);
    if (!declared->written() || declared->has(PositionFlags::IsFromCompiler) || declared->isExpected())
        return nullptr;
    const Type& type = types.at(*declared->established());
    return type.isJavaScriptValue() && !type.isAnything() ? declared : nullptr;
}

static bool hasTypeCheckedAtEntry(const Program& program, const BuildContext& build, FunctionID id, const Function& function, bool hasBody)
{
    bool isApplicationOrLibraryCode = function.role == FunctionRole::Source || function.role == FunctionRole::Library;
    if (!hasBody || !isApplicationOrLibraryCode || !build.isEnabled(PassID::CheckTypesAtEntry, findFunctionAsRead(&program, id)))
        return false;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        const DeclaredType* declared = findTypeCheckedAtEntry(program.types, function, index);
        if (declared && declared->site)
            return true;
    }
    return false;
}

static BlockID findFirstBlockWithValues(const Function& function)
{
    BlockID block = entryBlock;
    for (size_t steps = 0; steps < function.blocks.size(); ++steps) {
        const Block& current = function.at(block);
        if (!current.values.isEmpty() || current.handler || current.terminator.kind != TerminatorKind::Jump || !current.terminator.targets[0].arguments.isEmpty())
            return block;
        block = current.terminator.targets[0].block;
    }
    return entryBlock;
}

static ProgramPoint findPointBehindOwnAllocation(const Function& function)
{
    ProgramPoint head = ProgramPoint::atHeadOf(entryBlock);
    std::optional<unsigned> newTargetIndex = function.newTargetParameterIndex();
    if (function.kind != FunctionKind::BaseConstructor || !newTargetIndex)
        return head;
    BlockID block = findFirstBlockWithValues(function);
    const BlockValues& values = function.at(block).values;
    if (values.size() < 2)
        return head;
    const Value& prototype = function.at(values[0]);
    if (prototype.opcode != Opcode::GetConstructPrototype || prototype.newTargetOperand() != function.at(entryBlock).parameters[*newTargetIndex])
        return head;
    const Value& allocation = function.at(values[1]);
    if (allocation.opcode != Opcode::NewObject || allocation.numberOfOperands() != 1 || allocation.operand(0) != values[0])
        return head;
    return { block, 2 };
}

static Vector<unsigned, 8> findCheckedParameters(const Program& program, const Function& function)
{
    Vector<unsigned, 8> checkedParameters;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (findTypeCheckedAtEntry(program.types, function, index))
            checkedParameters.append(index);
    }
    return checkedParameters;
}

Vector<unsigned, 8> findParametersCheckedAtEntry(const Program& program, const BuildContext& build, FunctionID id, const Function& record, bool hasBody)
{
    if (!hasTypeCheckedAtEntry(program, build, id, record, hasBody))
        return { };
    return findCheckedParameters(program, record);
}

void appendChecksAtEntry(const Program& program, BuildContext& build, Function& function, std::span<const unsigned> checkedParameters)
{
    ProgramPoint point = findPointBehindOwnAllocation(function);
    BlockValues checks;
    function.editAllValues().reserveCapacity(function.values.size() + checkedParameters.size());
    for (unsigned index : checkedParameters) {
        DeclaredType& declared = *function.declaredEntryParameter(index);
        Value convert = IRBuilder::make(Opcode::Convert, { function.at(entryBlock).parameters[index] });
        convert.setDeclared(DeclaredType(Proven::byConversion(declared.expected()), declared.site));
        convert.setIllTypedCause(IllTypedCause::UnseenCaller);
        convert.setOrigin({ declared.site ? program.at(declared.site).position : function.position, 0 });
        declared.expect(declared.expected());
        declared.remove(PositionFlags::IsInt32Promise);

        checks.append(ValueID(function.values.size()));
        function.add(WTF::move(convert));
    }
    function.edit(point.block, BlockEdit::Values).values.insertVector(point.index, checks);
    build.statistics().count(StatisticID::TypesCheckedAtEntry, checkedParameters.size());
}

void checkTypesAtEntry(ProgramEditor& editor, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> functions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return hasTypeCheckedAtEntry(program, build, FunctionID(index), program.functions[index], program.functions[index].hasBody());
    });
    forEachIndexInParallel(loops, functions.size(), [&](unsigned position) {
        Function& function = editor.at(FunctionID(functions[position]));
        Vector<unsigned, 8> checkedParameters = findCheckedParameters(program, function);

        // Every use of a checked parameter becomes a use of its check. The parameters of the entry have the lowest numbers, so the table is short.
        Vector<ValueID, 16> checkOfValue;
        uint32_t numberOfValues = function.values.size();
        for (unsigned checked = 0; checked < checkedParameters.size(); ++checked) {
            ValueID parameter = function.at(entryBlock).parameters[checkedParameters[checked]];
            if (checkOfValue.size() <= parameter.index())
                checkOfValue.grow(parameter.index() + 1);
            checkOfValue[parameter.index()] = ValueID(numberOfValues + checked);
        }
        auto rewrite = [&](ValueID& value) {
            if (value.index() < checkOfValue.size() && checkOfValue[value.index()])
                value = checkOfValue[value.index()];
        };
        for (Value& value : function.editAllValues())
            value.forEachChild(rewrite);
        for (Block& block : function.editAllBlocks())
            block.forEachChildOutsideValues(rewrite);
        appendChecksAtEntry(program, build, function, checkedParameters.span());
    });
    build.statistics().count(StatisticID::FunctionsWithTypesCheckedAtEntry, functions.size());
}

namespace {

struct EntryCheck {
    ValueID check;
    unsigned entryParameter { 0 };
    TypeID type;
    TypeSiteID site;
    // A check of the same parameter stands in front of it in the list, and the test of that one serves both.
    bool sharesTest { false };
};

// All that the body does with the value of a parameter at the entrance is to check it for one type.
struct CheckedEntryValue {
    unsigned entryParameter { 0 };
    TypeID type;
    // In the list of the checks at the entrance.
    std::optional<size_t> entryCheck;
    Vector<ValueID, 4> checks;
    bool hasOtherUse { false };
};

struct Split {
    FunctionID id;
    Vector<EntryCheck, 4> checks;
    Vector<CheckedEntryValue, 2> checkedEntryValues;
};

struct EntryTest {
    uint32_t entryParameter;
    TypeID type;
};

// The tests of an entrance in their order, and which entry parameter each operand of its two calls is.
struct Entrance {
    Vector<EntryTest, 4> tests;
    Vector<uint32_t, 8> operandsOfBody;
    Vector<uint32_t, 8> operandsOfFallback;
};

struct ShorterEntrance {
    FunctionID entrance;
    uint32_t answered;

    friend auto operator<=>(const ShorterEntrance&, const ShorterEntrance&) = default;
};

struct AnsweredCall {
    FunctionID caller;
    ValueID call;
    ShorterEntrance entered;
};

} // anonymous namespace

static constexpr OptionSet<FunctionCapabilities> capabilitiesBehindEntrance { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };

static bool canBeSplit(const Program& program, const BuildContext& build, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.hasBody() || function.role != FunctionRole::Source)
        return false;
    if (!function.mayBeCloned() || !function.mayChangeSignature() || function.results.size() != 1)
        return false;
    if (!function.hasOrdinaryFrame() && !function.isUnsplitCoroutine())
        return false;
    if (function.constructedClass || function.has(FunctionFlags::HasNewTarget))
        return false;
    if (function.blocks.size() > build.limit(LimitID::MaxBlocksInGenericVersion))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return build.isEnabled(PassID::SplitBehindEntryChecks, id);
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findValuesUsedAsOperands(const Function& function)
{
    BitVector isUsed(function.values.size());
    auto note = [&](ValueID operand) {
        if (operand)
            isUsed.quickSet(operand.index());
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            function.at(id).forEachChild(note);
        block.terminator.forEachChild(note);
    }
    return isUsed;
}

static bool narrowsWithoutTest(Opcode opcode)
{
    return opcode == Opcode::AssumeType || isCastThatProvesNothing(opcode);
}

static Vector<EntryCheck, 4> findEntryChecksThatMayFail(const Program& program, const Function& function)
{
    Vector<EntryCheck, 4> checks;
    const Block& entry = function.at(entryBlock);
    if (entry.handler)
        return checks;
    for (ValueID id : entry.values) {
        const Value& value = function.at(id);
        if (value.opcode == Opcode::ValidateType || makesFact(value.opcode))
            continue;
        if (value.opcode != Opcode::Check && !narrowsWithoutTest(value.opcode))
            break;
        ValueID source = value.valueOperand();
        while (narrowsWithoutTest(function.at(source).opcode))
            source = function.at(source).valueOperand();
        size_t entryParameter = entry.parameters.find(source);
        if (entryParameter == notFound)
            break;
        // A test at the entrance would send ordinary calls into the generic version.
        if (value.opcode != Opcode::Check || value.cause() == CheckCause::MayFail)
            continue;
        const Type& given = program.typeOf(function, value.valueOperand());
        const Type& checked = program.at(value.expectedType().predicted());
        if (given.isSubsetOf(checked, program))
            continue;
        if (!given.overlaps(checked, program))
            return { };
        if (!changesCodeWhereProven(checked))
            continue;
        checks.append(EntryCheck { id, static_cast<unsigned>(entryParameter), value.expectedType().predicted(), value.site() });
    }
    if (checks.isEmpty())
        return checks;

    BitVector isUsed = findValuesUsedAsOperands(function);
    checks.removeAllMatching([&](const EntryCheck& check) {
        return !isUsed.quickGet(check.check.index());
    });
    return checks;
}

// The declared type of a parameter belongs to the variable, which the body may assign to.
static Vector<CheckedEntryValue, 2> findCheckedEntryValues(const Program& program, const Function& function, std::span<const EntryCheck> entryChecks)
{
    Vector<CheckedEntryValue, 8> entryValues;
    // Each name of the value of a parameter, with the position of the parameter in the list above.
    Vector<std::pair<ValueID, unsigned>, 8> sameValues;
    auto find = [&](ValueID value) -> CheckedEntryValue* {
        size_t index = sameValues.findIf([&](const auto& same) {
            return same.first == value;
        });
        return index == notFound ? nullptr : &entryValues[sameValues[index].second];
    };

    const Block& entry = function.at(entryBlock);
    if (entry.handler)
        return { };
    unsigned first = function.firstParameterIndex();
    for (unsigned index = first; index < first + function.numberOfPositionalParameters() && index < entry.parameters.size(); ++index) {
        std::optional<size_t> entryCheck;
        for (size_t i = 0; i < entryChecks.size(); ++i) {
            if (entryChecks[i].entryParameter == index)
                entryCheck = i;
        }
        if (entryCheck && function.at(entryChecks[*entryCheck].check).valueOperand() != entry.parameters[index])
            continue;
        sameValues.append({ entryCheck ? entryChecks[*entryCheck].check : entry.parameters[index], static_cast<unsigned>(entryValues.size()) });
        entryValues.append(CheckedEntryValue { index, { }, entryCheck, { } });
    }
    if (entryValues.isEmpty())
        return { };

    bool hasCheck = false;
    for (bool foundCast = true; std::exchange(foundCast, false);) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (value.opcode == Opcode::Check)
                    hasCheck |= !!find(value.valueOperand());
                if (!narrowsWithoutTest(value.opcode) || find(id))
                    continue;
                if (CheckedEntryValue* same = find(value.valueOperand())) {
                    sameValues.append({ id, static_cast<unsigned>(same - entryValues.begin()) });
                    foundCast = true;
                }
            }
        }
    }
    if (!hasCheck)
        return { };

    BitVector isUsed = findValuesUsedAsOperands(function);
    auto noteOtherUse = [&](ValueID operand) {
        if (CheckedEntryValue* entryValue = operand ? find(operand) : nullptr)
            entryValue->hasOtherUse = true;
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if ((narrowsWithoutTest(value.opcode) && find(id)) || value.opcode == Opcode::ValidateType)
                continue;
            if (CheckedEntryValue* checked = value.opcode == Opcode::Check && value.cause() != CheckCause::MayFail ? find(value.valueOperand()) : nullptr) {
                // Nobody wants what a check without a use lets through.
                if (isUsed.quickGet(id.index()))
                    checked->checks.append(id);
            } else
                value.forEachChild(noteOtherUse);
        }
        block.terminator.forEachChild(noteOtherUse);
        if (block.handler) {
            for (ValueID argument : block.handler->arguments)
                noteOtherUse(argument);
        }
    }

    Vector<CheckedEntryValue, 2> result;
    for (CheckedEntryValue& entryValue : entryValues) {
        if (entryValue.hasOtherUse || entryValue.checks.isEmpty())
            continue;
        entryValue.type = function.at(entryValue.checks[0]).expectedType().predicted();
        bool isOneType = std::ranges::all_of(entryValue.checks, [&](ValueID check) {
            return function.at(check).expectedType().predicted() == entryValue.type;
        });
        if (!isOneType)
            continue;
        const Type& given = program.typeOf(function, entry.parameters[entryValue.entryParameter]);
        const Type& checked = program.at(entryValue.type);
        if (given.isSubsetOf(checked, program) || !given.overlaps(checked, program) || !changesCodeWhereProven(checked))
            continue;
        if (entryValue.entryCheck && !checked.isSubsetOf(program.at(entryChecks[*entryValue.entryCheck].type), program))
            continue;
        result.append(WTF::move(entryValue));
    }
    return result;
}

// The type of a parameter that holds a reference lists `undefined` and `null` whatever the program has written.
static void leaveOutOmittedNullish(const Program& program, const Function& function, std::span<const EntryCheck> entryChecks, Vector<CheckedEntryValue, 2>& checkedEntryValues)
{
    for (size_t index = 0; index < entryChecks.size(); ++index) {
        const EntryCheck& check = entryChecks[index];
        const DeclaredType* declared = function.declaredEntryParameter(check.entryParameter);
        if (!declared || function.at(check.check).valueOperand() != function.at(entryBlock).parameters[check.entryParameter])
            continue;
        uint16_t omitted = 0;
        if (declared->has(PositionFlags::OmitsNull))
            omitted |= Primitive::Null;
        if (declared->has(PositionFlags::OmitsUndefined))
            omitted |= Primitive::Undefined;
        if (!omitted || std::ranges::contains(checkedEntryValues, check.entryParameter, &CheckedEntryValue::entryParameter))
            continue;
        const Type& listed = program.at(check.type);
        Type wanted = listed.withoutPrimitives(omitted);
        if (wanted == listed || wanted.isNever())
            continue;
        if (std::optional<TypeID> type = program.types.find(wanted))
            checkedEntryValues.append(CheckedEntryValue { check.entryParameter, *type, index, { } });
    }
}

// A member of the declared type that some caller is seen to pass is an ordinary value of the parameter, and a test that turns it away would send ordinary calls into the generic version.
static void keepWhereEveryKnownArrivalFits(const Program& program, const ParallelLoops& loops, Vector<Split>& splits)
{
    static_assert(OperandIndex::Call::arguments == 2);
    BitVector hasCheckedEntryValue(program.functions.size());
    for (const Split& split : splits) {
        if (!split.checkedEntryValues.isEmpty())
            hasCheckedEntryValue.set(split.id.index());
    }
    if (hasCheckedEntryValue.isEmpty())
        return;
    auto splitOf = [&](FunctionID id) -> const Split& {
        return *std::ranges::lower_bound(splits, id, { }, &Split::id);
    };

    using Misfit = std::pair<FunctionID, unsigned>;
    Vector<Misfit> misfits = collectInParallel<Misfit>(loops, program.functions.size(), [&](unsigned index, Vector<Misfit>& found) {
        const Function& caller = program.functions[index];
        auto arrive = [&](FunctionID callee, const CheckedEntryValue& checked, ValueID argument) {
            const Type& arriving = argument ? program.typeOf(caller, argument) : program.at(TypeTable::undefined);
            if (arriving != program.at(TypeTable::anything) && !arriving.isSubsetOf(program.at(checked.type), program))
                found.append(Misfit { callee, checked.entryParameter });
        };
        auto isCheckedAtEntry = [&](NameKind kind, uint32_t callee) {
            return kind == NameKind::Function && hasCheckedEntryValue.get(callee);
        };
        forEachListedValueNaming(program, FunctionID(index), { Opcode::CallDirect }, isCheckedAtEntry, [&](BlockID, ValueID, const Value& call) {
            for (const CheckedEntryValue& checked : splitOf(call.function()).checkedEntryValues)
                arrive(call.function(), checked, checked.entryParameter < call.numberOfOperands() ? call.operand(checked.entryParameter) : ValueID());
        });
        forEachListedValue(program, FunctionID(index), { Opcode::Call }, [&](BlockID, uint32_t, ValueID, const Value& call) {
            if (!call.callees())
                return;
            uint32_t numberOfArguments = call.numberOfOperands() - OperandIndex::Call::arguments;
            for (FunctionID callee : program.at(call.callees()).seen().predicted().functions) {
                if (!hasCheckedEntryValue.get(callee.index()))
                    continue;
                unsigned first = program.at(callee).firstParameterIndex();
                for (const CheckedEntryValue& checked : splitOf(callee).checkedEntryValues) {
                    uint32_t position = checked.entryParameter - first;
                    arrive(callee, checked, position < numberOfArguments ? call.operand(OperandIndex::Call::arguments + position) : ValueID());
                }
            }
        });
    });
    AOT::sortInLine(misfits);
    for (Split& split : splits) {
        split.checkedEntryValues.removeAllMatching([&](const CheckedEntryValue& checked) {
            return std::ranges::binary_search(misfits, Misfit { split.id, checked.entryParameter });
        });
    }
}

static Function createTypedBody(const Program& program, Function&& body, std::span<const EntryCheck> checks)
{
    for (const EntryCheck& check : checks) {
        ValueID parameter = body.at(entryBlock).parameters[check.entryParameter];
        if (!check.sharesTest) {
            Value& parameterValue = body.edit(parameter);
            parameterValue.type = check.type;
            // An expected type is set where it says more than the type does.
            TypeID expected = parameterValue.expected().predicted();
            if (expected && (program.at(expected) == program.at(check.type) || !program.at(expected).isSubsetOf(program.at(check.type), program)))
                parameterValue.setExpected({ });
            *body.declaredEntryParameter(check.entryParameter) = DeclaredType(Proven::byPosition(check.type), check.site);
        }

        Value& proven = body.edit(check.check);
        ValueID operand = proven.valueOperand();
        // The type of a check is what is left of the given type. A check that was taken in from the body has narrowed the one at the entrance.
        bool isAllOfTestedType = program.at(check.type).isSubsetOf(program.at(proven.type), program);
        TypeID provenType = isAllOfTestedType ? check.type : proven.type;
        // The entrance has made the test.
        if (operand == parameter && isAllOfTestedType) {
            proven.become(Opcode::Upcast);
            proven.setValueOperand(operand);
            proven.setCastType(provenType);
        } else
            proven.becomeAssumeType(operand, Proven::byPosition(provenType));
        proven.type = provenType;
    }
    settleProofsBehindAnsweredChecks(body);
    body.results[0].establish(Proven::asCandidate(body.results[0].expected()));
    body.capabilities = initialCapabilitiesOf(body) | capabilitiesBehindEntrance;
    return WTF::move(body);
}

// A cast between a parameter and its check holds what is left of the type that the parameter has behind the tests.
static void typeCastsAheadOfChecks(const Program& program, Function& body, TypeArena& types, std::span<const EntryCheck> checks)
{
    TypingContext typing { program, body, types };
    for (const EntryCheck& check : checks) {
        ValueID parameter = body.at(entryBlock).parameters[check.entryParameter];
        Vector<ValueID, 4> casts;
        for (ValueID cast = body.at(check.check).valueOperand(); cast != parameter; cast = body.at(cast).valueOperand())
            casts.append(cast);
        for (ValueID cast : casts | std::views::reverse) {
            Value& value = body.edit(cast);
            value.type = computeResultTypeID(typing, value);
            // An expected type is set where it says more than the type does.
            TypeID expected = value.expected().predicted();
            if (expected && (types.at(expected) == types.at(value.type) || !types.at(expected).isSubsetOf(types.at(value.type), program)))
                value.setExpected({ });
        }
    }
}

static Derivation derivationWithSameParameters(FunctionID source, const Function& function)
{
    return {
        .source = source,
        .parameterMap = Vector<ParameterOrigin>(function.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };
}

// The typed body and the fallback have the parameters of the function that is split, one for one. A function that was derived itself hands on what it says of its own.
static Function withParametersOf(FunctionID original, Function&& function)
{
    if (!function.derivation.source)
        function.derivation = derivationWithSameParameters(original, function);
    return WTF::move(function);
}

static Function createFallback(const Function& original, FunctionID typedBody)
{
    Function fallback = original;
    fallback.role = FunctionRole::GenericVersion;
    fallback.creator = CreatorRecord::Version { .provenVersion = typedBody };
    fallback.capabilities = initialCapabilitiesOf(fallback) | capabilitiesBehindEntrance;
    for (Block& block : fallback.editAllBlocks())
        block.frequency = block.frequency.predicted() == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
    return fallback;
}

// A row of tests of entry parameters, each with the proof as the one parameter of the block that follows, all failing into one block.
static void emitEntrance(const AnalysisContext& context, TypeArena& types, Function& function, const Entrance& entrance, FunctionID body, FunctionID fallback)
{
    IRBuilder builder(context, function, types);
    builder.setOrigin({ function.position, 0 });
    BlockID entry = builder.addBlock();
    builder.moveToEndOf(entry);

    TypingContext typing { context.program, function, types };
    Vector<ValueID, 8> arguments;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index)
        arguments.append(builder.addParameter(entry, declaredEntryTypeOf(typing, function, index)));

    BlockID failed = entrance.tests.isEmpty() ? BlockID() : builder.addBlock(BlockFrequency::Unlikely);
    if (failed)
        builder.emitFailureOfPredictions(Target { failed, { }, { } });
    Vector<ValueID, 8> testedArguments = arguments;
    for (const EntryTest& check : entrance.tests) {
        ValueID test = builder.appendTypeTest(arguments[check.entryParameter], check.type);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        testedArguments[check.entryParameter] = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(failed);
        builder.moveToEndOf(branch.taken);
    }
    auto emitCall = [&](FunctionID callee, const Vector<uint32_t, 8>& entryParameters, const Vector<ValueID, 8>& values) {
        Value call = IRBuilder::make(Opcode::CallDirect);
        call.setFunction(callee);
        call.setArgumentsOperands(WTF::map<8>(entryParameters, [&](uint32_t entryParameter) {
            return values[entryParameter];
        }).span());
        Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { builder.append(WTF::move(call)) });
        terminator.setRequirement(tailCallRequirementOf(context.program.at(callee)));
        builder.emitTerminator(WTF::move(terminator));
    };
    emitCall(body, entrance.operandsOfBody, testedArguments);
    if (failed) {
        builder.moveToEndOf(failed);
        emitCall(fallback, entrance.operandsOfFallback, arguments);
    }
    compactFunction(function);
}

static Function createChecks(const AnalysisContext& context, TypeArena& types, const Function& header, std::span<const EntryCheck> checks, FunctionID typedBody, FunctionID fallback)
{
    Function tests = header;
    tests.inlineFrames.clear();
    tests.role = FunctionRole::EntryChecks;
    tests.creator = CreatorRecord::EntryChecks { typedBody, fallback };
    tests.capabilities = capabilitiesInPlaceOf(tests, header.capabilities, FunctionCapabilities::IsHiddenInStackTraces);
    if (header.isUnsplitCoroutine())
        tests.flags.add(FunctionFlags::IsHiddenInStackTraces);

    Entrance entrance;
    for (const EntryCheck& check : checks) {
        if (!check.sharesTest)
            entrance.tests.append({ check.entryParameter, check.type });
    }
    for (uint32_t index = 0; index < tests.numberOfEntryParameters(); ++index)
        entrance.operandsOfBody.append(index);
    entrance.operandsOfFallback = entrance.operandsOfBody;
    emitEntrance(context, types, tests, entrance, typedBody, fallback);
    return tests;
}

void splitBehindEntryChecks(ProgramEditor& editor, const Heat* heat, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    Vector<Split> splits = collectInParallel<Split>(loops, program.functions.size(), [&](unsigned index, Vector<Split>& found) {
        FunctionID id(index);
        // Tests, a typed body and a fallback are three functions for one. They pay where callers that have proven their arguments enter the typed body often.
        if (!mayListValueWith(program, id, { Opcode::Check }) || !canBeSplit(program, build, id) || (heat && isCold(heat->of(program, id)).predicted()))
            return;
        Vector<EntryCheck, 4> checks = findEntryChecksThatMayFail(program, program.at(id));
        Vector<CheckedEntryValue, 2> checkedEntryValues;
        // A build that reports mismatches reports them where the check stands.
        if (build.recordedOptions().policy() != MismatchPolicy::Report) {
            checkedEntryValues = findCheckedEntryValues(program, program.at(id), checks.span());
            leaveOutOmittedNullish(program, program.at(id), checks.span(), checkedEntryValues);
        }
        if (!checks.isEmpty() || !checkedEntryValues.isEmpty())
            found.append(Split { id, WTF::move(checks), WTF::move(checkedEntryValues) });
    });
    AOT::sortInLine(splits, { }, &Split::id);
    keepWhereEveryKnownArrivalFits(program, loops, splits);
    // The test at the entrance is for the type that the body checks for. The typed body holds the value proven, hands it on proven, and enters its callees behind their own tests.
    for (Split& split : splits) {
        for (const CheckedEntryValue& checked : split.checkedEntryValues) {
            if (checked.entryCheck)
                split.checks[*checked.entryCheck].type = checked.type;
            for (ValueID check : checked.checks)
                split.checks.append(EntryCheck { check, checked.entryParameter, checked.type, program.at(split.id).at(check).site(), checked.entryCheck || check != checked.checks[0] });
        }
    }
    splits.removeAllMatching([](const Split& split) {
        return split.checks.isEmpty();
    });
    if (splits.isEmpty())
        return;

    unsigned count = static_cast<unsigned>(splits.size());
    FunctionID firstAdded = editor.reserveFunctions(2 * count);
    Vector<FunctionID> ids(count, [&](size_t index) {
        return splits[index].id;
    });
    // The typed bodies, then the fallbacks.
    Vector<FunctionID> added(2 * count, [&](size_t index) {
        return FunctionID(firstAdded.index() + index);
    });

    Vector<Function> headers(count);
    std::optional<ProgramEditor::ReplacedFunctions> replacedFunctions;
    replacedFunctions.emplace(editor, std::initializer_list<std::span<const FunctionID>> { ids.span(), added.span() });
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        FunctionID id = ids[index];
        Function& function = editor.at(id);
        static_cast<FunctionRecord&>(headers[index]) = function;
        editor.replaceFunctionLike(added[count + index], id, withParametersOf(id, createFallback(function, added[index])));
        editor.replaceFunctionLike(added[index], id, withParametersOf(id, createTypedBody(program, WTF::move(function), splits[index].checks.span())));
        function = headers[index];
    });
    replacedFunctions.reset();

    std::atomic<unsigned> numberOfProvenResults { 0 };
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (!headers[index].result().isExpected())
            return;
        // What a coroutine returns is the value inside its promise or generator. A call of it gives the promise or the generator, of which the body proves nothing.
        if (headers[index].isUnsplitCoroutine() || mayReturnTaintedValue(program, added[index], editor.at(added[index]), build))
            editor.at(added[index]).results[0].expect(program.at(added[index]).results[0].expected());
        else
            numberOfProvenResults.fetch_add(1, std::memory_order_relaxed);
    });

    // Typing a call reads the record of the callee. So the bodies are typed while no record changes, and the functions that test, which take the place of records that callers name, are written behind that.
    editFunctionsInParallel(editor, build, added.span().first(count), [&](unsigned index, FunctionID id, Function& body, TaskArenas& arenas) {
        AnalysisContext context { build, program, nullptr, nullptr, id };
        if (build.validates()) {
            IRBuilder builder(context, body, arenas.types);
            builder.setOrigin({ body.position, 0 });
            for (const EntryCheck& check : splits[index].checks) {
                if (!check.sharesTest)
                    builder.insertBefore(ProgramPoint::atHeadOf(entryBlock), makeValidateType(body.at(entryBlock).parameters[check.entryParameter], check.type));
            }
            builder.applyInsertions();
        }
        // A parameter that is tested for less than it was declared with has another type, and so has every cast that stands between it and the checks that were taken in.
        if (!splits[index].checkedEntryValues.isEmpty()) {
            FunctionAnalyses analyses(context, body);
            typeValuesFromNever(TypePropagationInputs { program, id, body, analyses, arenas.types, build });
        } else
            typeCastsAheadOfChecks(program, body, arenas.types, splits[index].checks.span());
        // While the tests at its head were checks, the function waited for its generic version and kept what stands behind a value of type never. A body without a check waits for nothing.
        if (!waitsForGenericVersion(program, body)) {
            FunctionAnalyses analyses(context, body);
            finishTypedValues(TypePropagationInputs { program, id, body, analyses, arenas.types, build });
        }
    });
    replacedFunctions.emplace(editor, std::initializer_list<std::span<const FunctionID>> { ids.span() });
    editFunctionsInParallel(editor, build, ids.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        function = createChecks(AnalysisContext { build, program, nullptr, nullptr, id }, arenas.types, headers[index], splits[index].checks.span(), added[index], added[count + index]);
    });
    replacedFunctions.reset();

    build.statistics().count(StatisticID::FunctionsSplitBehindEntryChecks, count);
    build.statistics().count(StatisticID::ResultsProvenBehindEntryChecks, numberOfProvenResults.load(std::memory_order_relaxed));
}

static bool readCallOfEntrance(const Function& function, BlockID id, FunctionID callee, std::span<const uint32_t> entryParameterOf, Vector<uint32_t, 8>& operands)
{
    const Block& block = function.at(id);
    if (block.values.size() != 1 || block.terminator.kind != TerminatorKind::ReturnCall)
        return false;
    const Value& call = function.at(block.values[0]);
    if (call.opcode != Opcode::CallDirect || call.function() != callee)
        return false;
    for (uint32_t index = 0; index < call.numberOfOperands(); ++index) {
        ValueID operand = call.operand(index);
        if (!operand || entryParameterOf[operand.index()] == UINT32_MAX)
            return false;
        operands.append(entryParameterOf[operand.index()]);
    }
    return true;
}

// As emitEntrance() lays them.
static std::optional<Entrance> readEntrance(const Function& function)
{
    const auto* record = function.role == FunctionRole::EntryChecks ? function.creator.tryAs<CreatorRecord::EntryChecks>() : nullptr;
    if (!record || !function.hasBody())
        return std::nullopt;

    Vector<uint32_t> entryParameterOf;
    entryParameterOf.fill(UINT32_MAX, function.values.size());
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
    for (uint32_t index = 0; index < parameters.size(); ++index)
        entryParameterOf[parameters[index].index()] = index;

    Entrance entrance;
    BlockID block = entryBlock;
    BlockID failed;
    if (const Block& first = function.at(block); first.terminator.kind == TerminatorKind::Branch && function.at(first.terminator.conditionOperand()).opcode == Opcode::ShouldFailPrediction) {
        failed = first.terminator.taken().block;
        block = first.terminator.notTaken().block;
    }
    while (function.at(block).terminator.kind == TerminatorKind::Branch) {
        const Terminator& branch = function.at(block).terminator;
        if (function.at(block).values.size() != 1 || entrance.tests.size() == 32)
            return std::nullopt;
        const Value& test = function.at(function.at(block).values[0]);
        if (test.opcode != Opcode::TypeTest || !std::ranges::contains(parameters, test.valueOperand()))
            return std::nullopt;
        BlockID passed = branch.taken().block;
        if (function.at(passed).parameters.size() != 1 || (failed && failed != branch.notTaken().block))
            return std::nullopt;
        failed = branch.notTaken().block;
        uint32_t entryParameter = entryParameterOf[test.valueOperand().index()];
        entryParameterOf[function.at(passed).parameters[0].index()] = entryParameter;
        entrance.tests.append({ entryParameter, test.testedType() });
        block = passed;
    }
    if (!failed || !function.at(failed).parameters.isEmpty())
        return std::nullopt;
    if (!readCallOfEntrance(function, block, record->body, entryParameterOf.span(), entrance.operandsOfBody) || !readCallOfEntrance(function, failed, record->fallback, entryParameterOf.span(), entrance.operandsOfFallback))
        return std::nullopt;
    return entrance;
}

static uint32_t testsAnsweredBy(const Program& program, const Function& caller, const Value& call, const Entrance& entrance)
{
    uint32_t answered = 0;
    for (uint32_t index = 0; index < entrance.tests.size(); ++index) {
        const EntryTest& test = entrance.tests[index];
        ValueID argument = test.entryParameter < call.numberOfOperands() ? call.operand(test.entryParameter) : ValueID();
        if (argument && program.typeOf(caller, argument).isSubsetOf(program.at(test.type), program))
            answered |= 1u << index;
    }
    return answered;
}

static Function createShorterEntrance(const AnalysisContext& context, TypeArena& types, const Function& header, const Entrance& entrance, ShorterEntrance key)
{
    const auto& record = header.creator.as<CreatorRecord::EntryChecks>();
    Function tests = header;
    tests.role = FunctionRole::CallSiteVersion;
    // The entrance of a coroutine only calls what makes the promise or the generator. Under a role that has the body of the source the two flags would say that this is the coroutine itself.
    tests.flags.remove({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
    tests.creator = CreatorRecord::Version { .provenVersion = key.entrance };
    // It has the parameters of the entrance, one for one: what flow says of them goes for a caller that enters here too.
    tests.derivation = derivationWithSameParameters(key.entrance, tests);
    tests.capabilities = initialCapabilitiesOf(tests) | capabilitiesBehindEntrance;
    tests.flags.add(FunctionFlags::IsHiddenInStackTraces);

    Entrance shorter { { }, entrance.operandsOfBody, entrance.operandsOfFallback };
    for (uint32_t index = 0; index < entrance.tests.size(); ++index) {
        const EntryTest& test = entrance.tests[index];
        if (key.answered & 1u << index) {
            // The fallback takes the parameter as the entrance declares it.
            DeclaredType& parameter = *tests.declaredEntryParameter(test.entryParameter);
            parameter = DeclaredType(Proven::byPosition(types.intersectionOf(context.program.provenTypeOf(parameter), test.type)));
        } else
            shorter.tests.append(test);
    }
    emitEntrance(context, types, tests, shorter, record.body, record.fallback);
    return tests;
}

void skipAnsweredEntryChecks(ProgramEditor& editor, const Heat& heat, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();

    using EntranceOfFunction = std::pair<uint32_t, Entrance>;
    Vector<EntranceOfFunction> entrances = collectInParallel<EntranceOfFunction>(loops, program.functions.size(), [&](unsigned index, Vector<EntranceOfFunction>& found) {
        if (std::optional<Entrance> entrance = readEntrance(program.functions[index]))
            found.append({ index, WTF::move(*entrance) });
    });
    constexpr uint32_t noEntrance = UINT32_MAX;
    Vector<uint32_t> positionOfEntrance(FillWith { }, program.functions.size(), noEntrance);
    for (uint32_t position = 0; position < entrances.size(); ++position)
        positionOfEntrance[entrances[position].first] = position;
    auto entranceOf = [&](FunctionID id) -> const Entrance* {
        uint32_t position = positionOfEntrance[id.index()];
        return position == noEntrance ? nullptr : &entrances[position].second;
    };

    // A shorter entrance is a function more. It pays where the call is expected to run.
    Vector<AnsweredCall> calls = collectInParallel<AnsweredCall>(loops, program.functions.size(), [&](unsigned index, Vector<AnsweredCall>& found) {
        FunctionID id(index);
        const Function& caller = program.functions[index];
        if (!mayListValueWith(program, id, { Opcode::CallDirect }) || isCold(heat.of(program, id)).predicted())
            return;
        auto isEntrance = [&](NameKind kind, uint32_t callee) {
            return kind == NameKind::Function && entranceOf(FunctionID(callee));
        };
        forEachListedValueNaming(program, id, { Opcode::CallDirect }, isEntrance, [&](BlockID block, ValueID value, const Value& call) {
            if (caller.at(block).frequency.predicted() != BlockFrequency::Normal || isCold(heat.of(program, call.function())).predicted())
                return;
            // The typed body promises another result than its entrance, so a call that named it would have another type, and so would all that is computed from it.
            if (uint32_t answered = testsAnsweredBy(program, caller, call, *entranceOf(call.function())))
                found.append({ id, value, { call.function(), answered } });
        });
    });
    if (calls.isEmpty())
        return;

    Vector<ShorterEntrance> keys = WTF::map(calls, [](const AnsweredCall& call) {
        return call.entered;
    });
    AOT::sortInLine(keys);
    removeRepeatedElements(keys);

    unsigned count = static_cast<unsigned>(keys.size());
    FunctionID firstAdded = editor.reserveFunctions(count);
    Vector<FunctionID> added(count, [&](size_t index) {
        return FunctionID(firstAdded.index() + index);
    });
    Vector<Function> headers(count);
    {
        ProgramEditor::ReplacedFunctions replacedFunctions(editor, { added.span() });
        for (unsigned index = 0; index < count; ++index) {
            static_cast<FunctionRecord&>(headers[index]) = program.at(keys[index].entrance);
            headers[index].inlineFrames.clear();
            editor.replaceFunctionLike(added[index], keys[index].entrance, Function(headers[index]));
        }
        editFunctionsInParallel(editor, build, added.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
            function = createShorterEntrance(AnalysisContext { build, program, nullptr, nullptr, id }, arenas.types, headers[index], *entranceOf(keys[index].entrance), keys[index]);
        });
    }

    Vector<FunctionID> callers = WTF::map(calls, [](const AnsweredCall& call) {
        return call.caller;
    });
    removeRepeatedElements(callers);
    editFunctionsInParallel(editor, build, callers.span(), [&](unsigned, FunctionID id, Function& caller, TaskArenas&) {
        auto [first, last] = std::ranges::equal_range(calls, id, { }, &AnsweredCall::caller);
        for (; first != last; ++first)
            caller.edit(first->call, ValueEdit::Definition).setFunction(added[std::ranges::lower_bound(keys, first->entered) - keys.begin()]);
    });

    build.statistics().count(StatisticID::ShorterEntrancesCreated, count);
    build.statistics().count(StatisticID::CallsThatSkipAnsweredEntryChecks, calls.size());
}

static bool runsAsCoroutine(const Function& target)
{
    return target.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || target.role == FunctionRole::CoroutineRamp;
}

bool hasAllocatingConstructEntrypoint(const Program& program, const Function& target)
{
    // A second entrypoint keeps the body out of the first. A long constructor spends its time in the body, and gains little by an object that is made in line.
    if (target.has(FunctionCapabilities::IsLongConstructor))
        return false;
    // Only a construction whose new.target is the function itself enters such an entrypoint.
    if (target.has(FunctionCapabilities::MakesNoTailCallIfConstructed))
        return false;
    // The constructor functions of the runtime's own modules are constructed by name.
    if (target.role == FunctionRole::Library || target.enclosingRuntimeFunction || (target.module && program.at(target.module).kind == ModuleKind::RuntimeSource))
        return false;
    if (target.has(FunctionFlags::HasNewTarget) || runsAsCoroutine(target) || target.results.size() != 1)
        return false;
    return isConstructorFunction(program, target) && program.at(target.constructedClass).flow.allocation != AllocationState::NeverAllocated;
}

EntrypointVariants entrypointVariantsOf(const Program& program, const Function& target)
{
    if (!target.has(FunctionFlags::HasNewTarget)) {
        bool keepsFrame = target.has(FunctionCapabilities::MakesNoTailCallIfConstructed) && !runsAsCoroutine(target);
        return keepsFrame || hasAllocatingConstructEntrypoint(program, target) ? EntrypointVariants::Both : EntrypointVariants::WithoutNewTarget;
    }
    switch (target.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return EntrypointVariants::WithNewTarget;
    case FunctionKind::Plain:
        return runsAsCoroutine(target) ? EntrypointVariants::WithoutNewTarget : EntrypointVariants::Both;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return EntrypointVariants::WithoutNewTarget;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Function createEntryWithoutBody(const Program& program, TypeArena& types, const Function& target, const GenericEntrypointRequest& request)
{
    const DeclaredType anything(Proven::byPosition(TypeTable::anything));
    bool readsArgumentList = target.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount });
    ArgumentPassing passing = readsArgumentList ? ArgumentPassing::Buffer : ArgumentPassing::Registers;
    uint32_t keyParameter = callEntrypointParameter;
    if (request.takesNewTarget && entrypointVariantsOf(program, target) == EntrypointVariants::Both)
        keyParameter = request.allocatesThis ? allocatingConstructEntrypointParameter : constructEntrypointParameter;

    Function entry = createDerivedHeader(target, request.target, FunctionRole::GenericEntrypoint, keyParameter);
    entry.flags.add(FunctionFlags::HasThis);
    entry.thisType = anything;
    // The run time enters the bodies of all modules with one closure, which it made for the first of them: Modules::callBody().
    bool isBodyOfModule = target.kind == FunctionKind::Module || target.kind == FunctionKind::CommonJsModule;
    entry.captures.append(DeclaredType(Proven::byPosition(isBodyOfModule ? TypeTable::function : types.intern(Type::forFunction(request.target)))));
    entry.results.append(anything);
    switch (passing) {
    case ArgumentPassing::Registers:
        if (request.takesNewTarget)
            entry.flags.add(FunctionFlags::HasNewTarget);
        entry.parameters.fill(anything, target.numberOfPositionalParameters());
        break;
    case ArgumentPassing::Buffer:
        break;
    }
    entry.creator = CreatorRecord::Entrypoint {
        .target = request.target,
        .arguments = passing,
        .takesNewTarget = request.takesNewTarget,
        .constructEntrypoint = request.constructEntrypoint,
    };
    if (request.isEnteredByHost)
        entry.capabilities.remove(FunctionCapabilities::IsNeverCalledByHost);
    return entry;
}

static String messageOfEntryThatNothingEnters(const AnalysisContext& context, const Function& target)
{
    const StringTable& strings = context.program.strings;
    // An obfuscated executable says nothing about its sources.
    bool mayNameSources = !context.build.options().obfuscates();
    if (mayNameSources && target.name && target.name != strings.emptyString())
        return functionNotCompiledMessage(strings.toWTFString(target.name));
    if (mayNameSources && target.module)
        return functionNotCompiledMessage(strings.toWTFString(context.program.pathForLineColumns(target.module)), context.program.lineColumnFor(target.module, target.position).line);
    return functionNotCompiledMessage();
}

// No call that flow follows comes here.
static void emitEntryThatNothingEnters(IRBuilder& builder, const Function& target, ValueID closure, StringArena& strings)
{
    const AnalysisContext& context = builder.context();
    bool canBeEntered = !context.build.validates() || !context.flow || context.flow->unseenCodeCanRun() || target.role == FunctionRole::Library;
    // Reading a property does not throw. Calling what was read does.
    if (canBeEntered && target.kind == FunctionKind::Getter) {
        Value function = IRBuilder::make(Opcode::NewFunctionThatThrows);
        function.setGetterOperand(closure);
        function.setName(context.build.options().obfuscates() || !target.name ? context.program.strings.emptyString() : target.name);
        builder.emitReturn(builder.append(WTF::move(function)));
        return;
    }
    if (canBeEntered) {
        Value error = IRBuilder::make(Opcode::ThrowNewError);
        error.setErrorKind(ErrorKind::Error);
        error.setMessage(strings.intern(StringView(messageOfEntryThatNothingEnters(context, target))));
        builder.append(WTF::move(error));
    }
    builder.emitUnreachable();
}

Function createGenericEntrypoint(const AnalysisContext& context, TypeArena& types, StringArena& strings, const GenericEntrypointRequest& request)
{
    AOT_THRESHOLD(size_t, mostValuesOfCopiedConstructor, 64);
    const Function& target = context.program.at(request.target);
    Function entry = createEntryWithoutBody(context.program, types, target, request);
    IRBuilder builder(context, entry, types);
    builder.setOrigin({ target.position, 0 });
    BlockID block = builder.addBlock();
    builder.moveToEndOf(block);

    uint32_t nextArgumentIndex = 0;
    auto addArgument = [&] {
        switch (entry.creator.as<CreatorRecord::Entrypoint>().arguments) {
        case ArgumentPassing::Registers:
            return builder.addParameter(block, TypeTable::anything);
        case ArgumentPassing::Buffer: {
            Value argument = IRBuilder::make(Opcode::GetArgument);
            argument.setArgumentIndex(nextArgumentIndex++);
            return builder.append(WTF::move(argument));
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto readArgumentListIf = [&](FunctionFlags flag, Opcode opcode, uint32_t firstArgument) {
        if (!target.has(flag))
            return ValueID();
        Value read = IRBuilder::make(opcode);
        read.setFirstArgument(firstArgument);
        return builder.append(WTF::move(read));
    };
    ValueID closure = builder.addParameter(block, context.program.provenTypeOf(entry.captures[0]));
    ValueID thisValue = builder.addParameter(block, context.program.provenTypeOf(entry.thisType));
    ValueID newTarget = request.takesNewTarget ? addArgument() : ValueID();
    uint32_t firstSourceArgument = request.takesNewTarget;
    Vector<ValueID, 8> fixedArguments(target.numberOfPositionalParameters(), [&](size_t) { return addArgument(); });
    uint32_t firstRestArgument = firstSourceArgument + fixedArguments.size();
    ValueID restArray = readArgumentListIf(FunctionFlags::HasRestParameter, Opcode::CreateRest, firstRestArgument);
    ValueID hiddenRest = readArgumentListIf(FunctionFlags::HasHiddenRest, Opcode::CreateRest, firstRestArgument);
    ValueID argumentCount = readArgumentListIf(FunctionFlags::HasArgumentCount, Opcode::GetRestLength, firstSourceArgument);

    // A body is removed once nothing can enter it. The closure may still be created, and its identity is the entrypoint.
    if (!target.hasBody()) {
        emitEntryThatNothingEnters(builder, target, closure, strings);
        return entry;
    }
    // new.target is the closure here: whoever enters has seen to that.
    ValueID object = request.allocatesThis ? appendThisOfConstructorFunction(builder, request.target, closure) : ValueID();
    // Flow narrows `this` from every call that reaches the function. `function Base() {}` that is only called with the objects of others, by Base.call(this) and by super(), never sees an object of its own layout.
    TypingContext typing { context.program, entry, types };
    if (object && typing.typeOf(object).castTo(typing.typeOf(target.thisType), types).isNever())
        emitEntryThatNothingEnters(builder, target, closure, strings);
    else {
        if (target.capabilities.contains(FunctionCapabilities::RecordsCallee) && !target.capabilities.contains(FunctionCapabilities::RecordsCalleeOnceRenamed)) {
            Value record = IRBuilder::make(Opcode::RecordCallee);
            record.setClosureOperand(closure);
            builder.append(WTF::move(record));
        }
        Vector<ValueID, 8> captures = appendCaptures(builder, request.target, closure);
        // The copy of the body in the construct entrypoint makes no tail calls.
        bool copiesBody = request.takesNewTarget && target.has(FunctionCapabilities::MakesNoTailCallIfConstructed) && entrypointVariantsOf(context.program, target) == EntrypointVariants::Both;
        ValueID call = appendDirectCall(builder, {
            .callee = request.target,
            .captures = captures.span(),
            .thisValue = object ? object : thisValue,
            .calleeValue = closure,
            .newTarget = object ? ValueID() : newTarget,
            .arguments = fixedArguments.span(),
            .restArray = restArray,
            .argumentCount = argumentCount,
            .hiddenRest = hiddenRest,
            .isConstruction = object || copiesBody,
            .isNeverInlined = object && target.values.size() > mostValuesOfCopiedConstructor,
            .isAlwaysInlined = copiesBody,
        });
        if (object)
            emitReturnOfConstruction(builder, call, object);
        else if (copiesBody)
            emitReturnOfCall(builder, call);
        else
            emitReturnOfEntrypointCall(builder, call);
    }
    if (context.build.validates())
        checkCallByValueCoversEntrypoint(context.program, request.target, entry, context.build.diagnostics());
    return entry;
}

Function buildGenericEntrypoint(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Function& target = inputs.program.at(key.target);
    EntrypointVariants variants = entrypointVariantsOf(inputs.program, target);
    RELEASE_ASSERT(canBeCalledByValueOf(target.role));
    bool allocatesThis = hasAllocatingConstructEntrypoint(inputs.program, target);

    GenericEntrypointRequest request { .target = key.target };
    if (key.parameter == constructEntrypointParameter || key.parameter == allocatingConstructEntrypointParameter) {
        RELEASE_ASSERT(variants == EntrypointVariants::Both && allocatesThis == (key.parameter == allocatingConstructEntrypointParameter));
        request.takesNewTarget = true;
        request.allocatesThis = allocatesThis;
    } else {
        RELEASE_ASSERT(key.parameter == callEntrypointParameter);
        request.takesNewTarget = variants == EntrypointVariants::WithNewTarget;
        if (variants == EntrypointVariants::Both)
            request.constructEntrypoint = inputs.derivedFunctions.ensure(derivedFunctionKey(FunctionRole::GenericEntrypoint, key.target, allocatesThis ? allocatingConstructEntrypointParameter : constructEntrypointParameter));
    }
    return createGenericEntrypoint({ inputs.build, inputs.program, &inputs.flow, nullptr }, inputs.types, inputs.strings, request);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

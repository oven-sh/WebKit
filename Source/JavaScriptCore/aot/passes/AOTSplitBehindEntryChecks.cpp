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
#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

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
};

struct Split {
    FunctionID id;
    Vector<EntryCheck, 4> checks;
    Vector<CheckedEntryValue, 2> checkedEntryValues;
};

} // anonymous namespace

static constexpr OptionSet<FunctionCapabilities> inheritedCapabilities {
    FunctionCapabilities::HasOnlyDirectCallers,
    FunctionCapabilities::IsNeverCalledByHost,
    FunctionCapabilities::IsHiddenInStackTraces,
};

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

static BitVector findUsedValues(const Function& function)
{
    BitVector isUsed(function.values.size());
    auto note = [&](ValueID operand, uint32_t) {
        if (operand)
            isUsed.quickSet(operand.index());
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            function.at(id).forEachOperand(note);
        block.terminator.forEachOperand(note);
        for (const Target& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                note(argument, 0);
        }
    }
    return isUsed;
}

static bool narrowsWithoutTest(Opcode opcode)
{
    return opcode == Opcode::AssumeType || opcode == Opcode::Upcast;
}

static std::optional<unsigned> findEntryParameterBehindCasts(const Function& function, ValueID id)
{
    while (narrowsWithoutTest(function.at(id).opcode))
        id = function.at(id).valueOperand();
    size_t index = function.at(entryBlock).parameters.find(id);
    if (index == notFound)
        return std::nullopt;
    return static_cast<unsigned>(index);
}

static Vector<EntryCheck, 4> findEntryChecksThatMayFail(const Program& program, const Function& function)
{
    Vector<EntryCheck, 4> checks;
    const Block& entry = function.at(entryBlock);
    if (entry.handler)
        return checks;
    for (ValueID id : entry.values) {
        const Value& value = function.at(id);
        if (value.opcode != Opcode::Check && !narrowsWithoutTest(value.opcode))
            break;
        std::optional<unsigned> entryParameter = findEntryParameterBehindCasts(function, value.valueOperand());
        if (!entryParameter)
            break;
        if (value.opcode != Opcode::Check)
            continue;
        const Type& given = program.typeOf(function, value.valueOperand());
        const Type& checked = program.at(value.castType());
        if (given.isSubsetOf(checked, program))
            continue;
        if (!given.overlaps(checked, program))
            return { };
        checks.append(EntryCheck { id, *entryParameter, value.castType(), value.site() });
    }
    if (checks.isEmpty())
        return checks;

    BitVector isUsed = findUsedValues(function);
    checks.removeAllMatching([&](const EntryCheck& check) {
        return !isUsed.quickGet(check.check.index());
    });
    return checks;
}

// The declared type of a parameter belongs to the variable, which the body may assign to. The value at the entrance is what callers pass.
// Where all that the body does with that value is to check it for one type, a narrower one than is declared, the value is wanted with that type or not at all.
static Vector<CheckedEntryValue, 2> findCheckedEntryValues(const Program& program, const Function& function, std::span<const EntryCheck> entryChecks)
{
    struct EntryValue {
        unsigned entryParameter { 0 };
        std::optional<size_t> entryCheck;
        Vector<ValueID, 4> checks;
        bool hasOtherUse { false };
    };
    struct SameValue {
        ValueID value;
        unsigned entryValue { 0 };
    };
    Vector<EntryValue, 8> entryValues;
    Vector<SameValue, 8> sameValues;
    auto find = [&](ValueID value) -> EntryValue* {
        size_t index = sameValues.findIf([&](const SameValue& same) {
            return same.value == value;
        });
        return index == notFound ? nullptr : &entryValues[sameValues[index].entryValue];
    };

    const Block& entry = function.at(entryBlock);
    if (entry.handler)
        return { };
    unsigned first = function.firstParameterIndex();
    for (unsigned index = first; index < first + function.numberOfPositionalParameters() && index < entry.parameters.size(); ++index) {
        size_t entryCheck = entryChecks.size();
        for (size_t i = 0; i < entryChecks.size(); ++i) {
            if (entryChecks[i].entryParameter == index)
                entryCheck = i;
        }
        if (entryCheck == entryChecks.size())
            sameValues.append(SameValue { entry.parameters[index], static_cast<unsigned>(entryValues.size()) });
        else if (function.at(entryChecks[entryCheck].check).valueOperand() == entry.parameters[index])
            sameValues.append(SameValue { entryChecks[entryCheck].check, static_cast<unsigned>(entryValues.size()) });
        else
            continue;
        entryValues.append(EntryValue { index, entryCheck == entryChecks.size() ? std::nullopt : std::optional<size_t> { entryCheck }, { }, false });
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
                if (EntryValue* same = find(value.valueOperand())) {
                    sameValues.append(SameValue { id, static_cast<unsigned>(same - entryValues.begin()) });
                    foundCast = true;
                }
            }
        }
    }
    if (!hasCheck)
        return { };

    BitVector isUsed = findUsedValues(function);
    auto noteOtherUse = [&](ValueID operand, uint32_t = 0) {
        if (EntryValue* entryValue = operand ? find(operand) : nullptr)
            entryValue->hasOtherUse = true;
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (narrowsWithoutTest(value.opcode) && find(id))
                continue;
            if (EntryValue* checked = value.opcode == Opcode::Check ? find(value.valueOperand()) : nullptr) {
                // Nobody wants what a check without a use lets through.
                if (isUsed.quickGet(id.index()))
                    checked->checks.append(id);
            } else
                value.forEachOperand(noteOtherUse);
        }
        block.terminator.forEachOperand(noteOtherUse);
        for (const Target& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                noteOtherUse(argument);
        }
        if (block.handler) {
            for (ValueID argument : block.handler->arguments)
                noteOtherUse(argument);
        }
    }

    Vector<CheckedEntryValue, 2> result;
    for (EntryValue& entryValue : entryValues) {
        if (entryValue.hasOtherUse || entryValue.checks.isEmpty())
            continue;
        TypeID type = function.at(entryValue.checks[0]).castType();
        bool isOneType = std::ranges::all_of(entryValue.checks, [&](ValueID check) {
            return function.at(check).castType() == type;
        });
        if (!isOneType)
            continue;
        const Type& given = program.typeOf(function, entry.parameters[entryValue.entryParameter]);
        const Type& checked = program.at(type);
        if (given.isSubsetOf(checked, program) || !given.overlaps(checked, program))
            continue;
        if (entryValue.entryCheck && !checked.isSubsetOf(program.at(entryChecks[*entryValue.entryCheck].type), program))
            continue;
        result.append(CheckedEntryValue { entryValue.entryParameter, type, entryValue.entryCheck, WTF::move(entryValue.checks) });
    }
    return result;
}

// The type of a parameter that holds a reference lists `undefined` and `null` whatever the program has written. What the program has left out goes to the generic version,
// and the typed body holds a reference.
static void leaveOutOmittedNullish(const Program& program, const Function& function, std::span<const EntryCheck> entryChecks, Vector<CheckedEntryValue, 2>& checkedEntryValues)
{
    for (size_t index = 0; index < entryChecks.size(); ++index) {
        const EntryCheck& check = entryChecks[index];
        const DeclaredType* declared = function.declaredEntryParameter(check.entryParameter);
        if (!declared || function.at(check.check).valueOperand() != function.at(entryBlock).parameters[check.entryParameter])
            continue;
        uint16_t omitted = 0;
        if (declared->flags.contains(PositionFlags::OmitsNull))
            omitted |= Primitive::Null;
        if (declared->flags.contains(PositionFlags::OmitsUndefined))
            omitted |= Primitive::Undefined;
        bool isCheckedInBody = std::ranges::any_of(checkedEntryValues, [&](const CheckedEntryValue& checked) {
            return checked.entryParameter == check.entryParameter;
        });
        if (!omitted || isCheckedInBody)
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
// Of a value of any type, of a list of arguments and of a caller that nobody sees, nothing is known: those are what the test is for.
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

    struct Misfit {
        FunctionID function;
        unsigned entryParameter { 0 };
        auto operator<=>(const Misfit&) const = default;
    };
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
            for (FunctionID callee : program.at(call.callees()).functions) {
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
    std::ranges::sort(misfits);
    for (Split& split : splits) {
        split.checkedEntryValues.removeAllMatching([&](const CheckedEntryValue& checked) {
            return std::ranges::binary_search(misfits, Misfit { split.id, checked.entryParameter });
        });
    }
}

// The test at the entrance is for the type that the body checks for. The typed body holds the value proven, hands it on proven, and enters its callees behind their own tests.
static void takeInChecksOfEntryValues(const Function& function, Split& split)
{
    for (const CheckedEntryValue& checked : split.checkedEntryValues) {
        if (checked.entryCheck)
            split.checks[*checked.entryCheck].type = checked.type;
        for (ValueID check : checked.checks)
            split.checks.append(EntryCheck { check, checked.entryParameter, checked.type, function.at(check).site(), checked.entryCheck || check != checked.checks[0] });
    }
}

static void emitCallOfBody(IRBuilder& builder, FunctionID body, std::span<const ValueID> arguments)
{
    const Function& callee = builder.context().program.at(body);
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(body);
    call.setArgumentsOperands(arguments);
    ValueID result = builder.append(WTF::move(call));

    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { result });
    terminator.setRequirement(tailCallRequirementOf(callee));
    builder.emitTerminator(WTF::move(terminator));
}

static Function createTypedBody(const Program& program, Function&& body, std::span<const EntryCheck> checks)
{
    for (const EntryCheck& check : checks) {
        ValueID parameter = body.at(entryBlock).parameters[check.entryParameter];
        if (!check.sharesTest) {
            Value& parameterValue = body.edit(parameter);
            parameterValue.type = check.type;
            // An expected type is set where it says more than the type does.
            TypeID expected = parameterValue.expected();
            if (expected && (program.at(expected) == program.at(check.type) || !program.at(expected).isSubsetOf(program.at(check.type), program)))
                parameterValue.setExpected({ });
            *body.declaredEntryParameter(check.entryParameter) = DeclaredType { .type = check.type, .site = check.site };
        }

        Value& proven = body.edit(check.check);
        ValueID operand = proven.valueOperand();
        // The type of a check is what is left of the given type. A check that was taken in from the body has narrowed the one at the entrance.
        bool isAllOfTestedType = program.at(check.type).isSubsetOf(program.at(proven.type), program);
        TypeID provenType = isAllOfTestedType ? check.type : proven.type;
        proven.become(operand == parameter && isAllOfTestedType ? Opcode::Upcast : Opcode::AssumeType);
        proven.setValueOperand(operand);
        proven.setCastType(provenType);
        proven.type = provenType;
    }
    body.results[0].flags.remove(PositionFlags::IsExpected);
    body.capabilities = initialCapabilitiesOf(body) | OptionSet<FunctionCapabilities> { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };
    return WTF::move(body);
}

static void validateTypesAtEntry(const AnalysisContext& context, TypeArena& types, Function& body, std::span<const EntryCheck> checks)
{
    IRBuilder builder(context, body, types);
    builder.setOrigin({ body.position, 0 });
    for (const EntryCheck& check : checks) {
        if (!check.sharesTest)
            builder.insertBefore(ProgramPoint::atHeadOf(entryBlock), makeValidateType(body.at(entryBlock).parameters[check.entryParameter], check.type));
    }
    builder.applyInsertions();
}

// While the tests at its head were checks, the function waited for its generic version and kept what stands behind a value of type never. A body without a check waits for nothing.
static void cutBehindValuesThatNeverComplete(const AnalysisContext& context, TypeArena& types, Function& body)
{
    if (waitsForGenericVersion(context.program, body))
        return;
    FunctionAnalyses analyses(context, body);
    finishTypedValues(TypePropagationInputs { context.program, context.id, body, analyses, types, context.build });
}

// A parameter that is tested for less than it was declared with has another type, and so has every cast that stands between it and the checks that were taken in.
static void typeValuesBehindNarrowedParameters(const AnalysisContext& context, TypeArena& types, Function& body)
{
    FunctionAnalyses analyses(context, body);
    typeValuesFromNever(TypePropagationInputs { context.program, context.id, body, analyses, types, context.build });
}

// The typed body and the fallback have the parameters of the function that is split, one for one. A function that was derived itself hands on what it says of its own.
static Function withParametersOf(FunctionID original, Function&& function)
{
    if (!function.derivation.source) {
        function.derivation = Derivation {
            .source = original,
            .parameterMap = Vector<ParameterOrigin>(function.numberOfEntryParameters(), [](size_t index) {
                return ParameterOrigin { static_cast<uint32_t>(index) };
            }),
        };
    }
    return WTF::move(function);
}

static Function createFallback(const Function& original, FunctionID typedBody)
{
    Function fallback = original;
    fallback.role = FunctionRole::GenericVersion;
    fallback.creator = CreatorRecord::Version { .provenVersion = typedBody };
    fallback.capabilities = initialCapabilitiesOf(fallback) | OptionSet<FunctionCapabilities> { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };
    for (Block& block : fallback.editAllBlocks())
        block.frequency = block.frequency == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
    return fallback;
}

static Function createChecks(const AnalysisContext& context, TypeArena& types, const Function& header, std::span<const EntryCheck> checks, FunctionID typedBody, FunctionID fallback)
{
    Function tests = header;
    tests.inlineFrames.clear();
    tests.role = FunctionRole::EntryChecks;
    tests.creator = CreatorRecord::EntryChecks { typedBody, fallback };
    tests.capabilities = (initialCapabilitiesOf(tests) - inheritedCapabilities) | (header.capabilities & inheritedCapabilities);
    if (header.isUnsplitCoroutine())
        tests.flags.add(FunctionFlags::IsHiddenInStackTraces);

    IRBuilder builder(context, tests, types);
    builder.setOrigin({ header.position, 0 });
    BlockID entry = builder.addBlock();
    builder.moveToEndOf(entry);

    TypingContext typing { context.program, tests, types };
    Vector<ValueID, 8> arguments;
    for (unsigned index = 0; index < tests.numberOfEntryParameters(); ++index)
        arguments.append(builder.addParameter(entry, declaredEntryTypeOf(typing, tests, index)));

    BlockID failed = builder.addBlock(BlockFrequency::Unlikely);
    Vector<ValueID, 8> testedArguments = arguments;
    for (const EntryCheck& check : checks) {
        if (check.sharesTest)
            continue;
        ValueID test = builder.appendTypeTest(arguments[check.entryParameter], check.type);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        testedArguments[check.entryParameter] = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(failed);
        builder.moveToEndOf(branch.taken);
    }
    emitCallOfBody(builder, typedBody, testedArguments.span());
    builder.moveToEndOf(failed);
    emitCallOfBody(builder, fallback, arguments.span());
    compactFunction(tests);
    return tests;
}

void splitBehindEntryChecks(ProgramEditor& editor, const Heat* heat, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    Vector<Split> splits = collectInParallel<Split>(loops, program.functions.size(), [&](unsigned index, Vector<Split>& found) {
        FunctionID id(index);
        // Tests, a typed body and a fallback are three functions for one. They pay where callers that have proven their arguments enter the typed body often.
        if (!mayListValueWith(program, id, { Opcode::Check }) || !canBeSplit(program, build, id) || (heat && isCold(heat->of(program, id))))
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
    std::ranges::sort(splits, { }, &Split::id);
    keepWhereEveryKnownArrivalFits(program, loops, splits);
    for (Split& split : splits)
        takeInChecksOfEntryValues(program.at(split.id), split);
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
    Vector<FunctionID> added(2 * count, [&](size_t index) {
        return FunctionID(firstAdded.index() + index);
    });
    auto typedBodyOf = [&](unsigned index) {
        return added[index];
    };
    auto fallbackOf = [&](unsigned index) {
        return added[count + index];
    };

    Vector<Function> headers(count);
    std::optional<ProgramEditor::ReplacedFunctions> replacedFunctions;
    replacedFunctions.emplace(editor, std::initializer_list<std::span<const FunctionID>> { ids.span(), added.span() });
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        FunctionID id = ids[index];
        Function& function = editor.at(id);
        static_cast<FunctionRecord&>(headers[index]) = function;
        editor.replaceFunctionLike(fallbackOf(index), id, withParametersOf(id, createFallback(function, typedBodyOf(index))));
        editor.replaceFunctionLike(typedBodyOf(index), id, withParametersOf(id, createTypedBody(program, WTF::move(function), splits[index].checks.span())));
        function = headers[index];
    });
    replacedFunctions.reset();

    std::atomic<unsigned> numberOfProvenResults { 0 };
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (!headers[index].result().flags.contains(PositionFlags::IsExpected))
            return;
        // What a coroutine returns is the value inside its promise or generator. A call of it gives the promise or the generator, of which the body proves nothing.
        if (headers[index].isUnsplitCoroutine() || mayReturnTaintedValue(program, typedBodyOf(index), build))
            editor.at(typedBodyOf(index)).results[0].flags.add(PositionFlags::IsExpected);
        else
            numberOfProvenResults.fetch_add(1, std::memory_order_relaxed);
    });

    // Typing a call reads the record of the callee. So the bodies are typed while no record changes, and the functions that test, which take the place of records that callers name, are written behind that.
    editFunctionsInParallel(editor, build, added.span().first(count), [&](unsigned index, FunctionID id, Function& body, TaskArenas& arenas) {
        AnalysisContext context { build, program, nullptr, nullptr, id };
        if (build.validates())
            validateTypesAtEntry(context, arenas.types, body, splits[index].checks.span());
        if (!splits[index].checkedEntryValues.isEmpty())
            typeValuesBehindNarrowedParameters(context, arenas.types, body);
        cutBehindValuesThatNeverComplete(context, arenas.types, body);
    });
    replacedFunctions.emplace(editor, std::initializer_list<std::span<const FunctionID>> { ids.span() });
    editFunctionsInParallel(editor, build, ids.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        function = createChecks(AnalysisContext { build, program, nullptr, nullptr, id }, arenas.types, headers[index], splits[index].checks.span(), typedBodyOf(index), fallbackOf(index));
    });
    replacedFunctions.reset();

    Statistics& statistics = build.statistics();
    statistics.count(StatisticID::FunctionsSplitBehindEntryChecks, count);
    statistics.count(StatisticID::ResultsProvenBehindEntryChecks, numberOfProvenResults.load(std::memory_order_relaxed));
}

namespace {

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

struct AnsweredCall {
    FunctionID caller;
    ValueID call;
    FunctionID entrance;
    uint32_t answered;
};

struct ShorterEntrance {
    FunctionID entrance;
    uint32_t answered;

    friend bool operator==(const ShorterEntrance&, const ShorterEntrance&) = default;
    friend auto operator<=>(const ShorterEntrance&, const ShorterEntrance&) = default;
};

} // anonymous namespace

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

// As createChecks() lays them: a row of tests of entry parameters, each with the proof as the one parameter of the block that follows, all failing into one block.
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
    tests.creator = CreatorRecord::Version { .provenVersion = key.entrance };
    // It has the parameters of the entrance, one for one: what flow says of them goes for a caller that enters here too.
    tests.derivation = Derivation {
        .source = key.entrance,
        .parameterMap = Vector<ParameterOrigin>(tests.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };
    tests.capabilities = initialCapabilitiesOf(tests) | OptionSet<FunctionCapabilities> { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };
    tests.flags.add(FunctionFlags::IsHiddenInStackTraces);
    for (uint32_t index = 0; index < entrance.tests.size(); ++index) {
        if (key.answered & 1u << index)
            *tests.declaredEntryParameter(entrance.tests[index].entryParameter) = DeclaredType { .type = entrance.tests[index].type };
    }

    IRBuilder builder(context, tests, types);
    builder.setOrigin({ header.position, 0 });
    BlockID entry = builder.addBlock();
    builder.moveToEndOf(entry);

    TypingContext typing { context.program, tests, types };
    Vector<ValueID, 8> arguments;
    for (unsigned index = 0; index < tests.numberOfEntryParameters(); ++index)
        arguments.append(builder.addParameter(entry, declaredEntryTypeOf(typing, tests, index)));

    bool hasTests = key.answered != (uint64_t { 1 } << entrance.tests.size()) - 1;
    BlockID failed = hasTests ? builder.addBlock(BlockFrequency::Unlikely) : BlockID();
    Vector<ValueID, 8> testedArguments = arguments;
    for (uint32_t index = 0; index < entrance.tests.size(); ++index) {
        if (key.answered & 1u << index)
            continue;
        const EntryTest& check = entrance.tests[index];
        ValueID test = builder.appendTypeTest(arguments[check.entryParameter], check.type);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        testedArguments[check.entryParameter] = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(failed);
        builder.moveToEndOf(branch.taken);
    }
    auto operandsFrom = [](std::span<const uint32_t> entryParameters, std::span<const ValueID> values) {
        return WTF::map<8>(entryParameters, [&](uint32_t entryParameter) {
            return values[entryParameter];
        });
    };
    emitCallOfBody(builder, record.body, operandsFrom(entrance.operandsOfBody.span(), testedArguments.span()).span());
    if (hasTests) {
        builder.moveToEndOf(failed);
        emitCallOfBody(builder, record.fallback, operandsFrom(entrance.operandsOfFallback.span(), arguments.span()).span());
    }
    compactFunction(tests);
    return tests;
}

void skipAnsweredEntryChecks(ProgramEditor& editor, const Heat& heat, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();

    struct EntranceOfFunction {
        uint32_t function;
        Entrance entrance;
    };
    Vector<EntranceOfFunction> entrances = collectInParallel<EntranceOfFunction>(loops, program.functions.size(), [&](unsigned index, Vector<EntranceOfFunction>& found) {
        if (std::optional<Entrance> entrance = readEntrance(program.functions[index]))
            found.append({ index, WTF::move(*entrance) });
    });
    constexpr uint32_t noEntrance = UINT32_MAX;
    Vector<uint32_t> positionOfEntrance(FillWith { }, program.functions.size(), noEntrance);
    for (uint32_t position = 0; position < entrances.size(); ++position)
        positionOfEntrance[entrances[position].function] = position;
    auto entranceOf = [&](FunctionID id) -> const Entrance* {
        uint32_t position = positionOfEntrance[id.index()];
        return position == noEntrance ? nullptr : &entrances[position].entrance;
    };

    // A shorter entrance is a function more. It pays where the call is expected to run.
    Vector<AnsweredCall> calls = collectInParallel<AnsweredCall>(loops, program.functions.size(), [&](unsigned index, Vector<AnsweredCall>& found) {
        FunctionID id(index);
        const Function& caller = program.functions[index];
        if (!mayListValueWith(program, id, { Opcode::CallDirect }) || isCold(heat.of(program, id)))
            return;
        auto isEntrance = [&](NameKind kind, uint32_t callee) {
            return kind == NameKind::Function && entranceOf(FunctionID(callee));
        };
        forEachListedValueNaming(program, id, { Opcode::CallDirect }, isEntrance, [&](BlockID block, ValueID value, const Value& call) {
            if (caller.at(block).frequency != BlockFrequency::Normal || isCold(heat.of(program, call.function())))
                return;
            // The typed body promises another result than its entrance, so a call that named it would have another type, and so would all that is computed from it.
            // A caller that answers every test enters an entrance without tests, which is a jump that the linker threads.
            if (uint32_t answered = testsAnsweredBy(program, caller, call, *entranceOf(call.function())))
                found.append({ id, value, call.function(), answered });
        });
    });
    if (calls.isEmpty())
        return;

    Vector<ShorterEntrance> keys = WTF::map(calls, [](const AnsweredCall& call) {
        return ShorterEntrance { call.entrance, call.answered };
    });
    std::ranges::sort(keys);
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
            caller.edit(first->call).setFunction(added[std::ranges::lower_bound(keys, ShorterEntrance { first->entrance, first->answered }) - keys.begin()]);
    });

    Statistics& statistics = build.statistics();
    statistics.count(StatisticID::ShorterEntrancesCreated, count);
    statistics.count(StatisticID::CallsThatSkipAnsweredEntryChecks, calls.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

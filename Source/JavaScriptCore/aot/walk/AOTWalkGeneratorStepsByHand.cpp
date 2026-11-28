/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkLoopsOverGenerators.h"

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
bool canWaitForStepOfAsyncGenerator(Walker&);
Reference buildStepOfAsyncGenerator(Walker&, Reference generator, Reference sent, std::span<const Reference> held, BuiltinID awaitedCallee, Reference goesByDriver, Origin);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);


namespace {

enum ParameterOfMeetingInStepsByHand : uint32_t { Whole, Answer, IsAnswered, Value, Done, numberOfParametersOfMeetingInStepsByHand };

} // anonymous namespace

static Reference buildInStepsByHand(Walker& walker, Draft&& operation)
{
    return walker.isUnreachable() ? Reference() : walker.build(WTF::move(operation));
}

static Reference buildResultAtInStepsByHand(Walker& walker, Reference step, StepGeneratorResult result, Origin origin)
{
    Draft read = draftOfCoroutineCalls(walker, Opcode::ResultAt, origin);
    read.setResultsOperand(step);
    read.setIndex(static_cast<uint32_t>(result));
    return buildInStepsByHand(walker, WTF::move(read));
}

template<typename Status>
static Reference buildTestOfStatusInStepsByHand(Walker& walker, Reference status, Status asked, Origin origin)
{
    if (walker.isUnreachable())
        return { };
    Draft written(Opcode::Integer, origin);
    written.setInteger(static_cast<int64_t>(asked));
    Draft test = draftOfCoroutineCalls(walker, Opcode::StrictEq, origin);
    test.setOperand(0, status);
    test.setOperand(1, walker.constant(WTF::move(written)));
    test.setForm(operatorFormFor(walker, test));
    return boxNumberIfNeeded(walker, walker.build(WTF::move(test)), origin);
}

Reference buildResultOfStep(Walker& walker, Reference value, Reference done, Origin origin)
{
    if (walker.isUnreachable())
        return { };
    const Program& program = walker.program();
    LayoutID layoutID = program.layoutOf(BuiltinLayouts::IteratorResult);
    const Layout& layout = program.at(layoutID);
    std::optional<uint32_t> valueField = layout.findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);
    Draft object(Opcode::NewObject, origin);
    object.setLayout(layoutID);
    object.setFields(std::array { *valueField, *doneField });
    object.setValuesOperands(std::array { value, done });
    walker.chooseConservativeFields(object);
    return walker.build(WTF::move(object));
}

static void jumpToInStepsByHand(Walker& walker, BlockID meeting, std::span<const Reference> arguments, bool& someWayArrives)
{
    if (walker.isUnreachable())
        return;
    walker.jumpTo(meeting, arguments);
    someWayArrives = true;
}

static Reference booleanInStepsByHand(Walker& walker, bool written, Origin origin)
{
    Draft constant(Opcode::Boolean, origin);
    constant.setBoolean(written);
    return walker.constant(WTF::move(constant));
}

static bool isPlainPrimitiveInStepsByHand(Walker& walker, const BodyIndex& index, ValueID value, ValueID read)
{
    static constexpr uint16_t plainPrimitives = Primitive::Number | Primitive::AnyString | Primitive::Boolean | Primitive::UndefinedOrNull;
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    static constexpr OpcodeSet written { Opcode::Integer, Opcode::Double, Opcode::String, Opcode::Boolean, Opcode::Undefined, Opcode::Null };
    value = index.stripCasts(value);
    bool followsRead = value && !index.isParameter(value) && index.blockOf(value) == index.blockOf(read) && value.index() > read.index();
    if (!followsRead) {
        Reference built = value ? walker.referenceOf(value) : Reference();
        return built && walker.typeOf(built) && walker.types().at(walker.typeOf(built)).hasOnlyPrimitives(plainPrimitives);
    }
    Opcode opcode = index.opcodeOf(value);
    if (written.contains(opcode))
        return true;
    if (!operators.contains(opcode))
        return false;
    bool isPlain = true;
    index.value(value).forEachOperand([&](ValueID operand, uint32_t) {
        isPlain = isPlain && isPlainPrimitiveInStepsByHand(walker, index, operand, read);
    });
    return isPlain;
}

static bool nothingSeenHappensBetweenInStepsByHand(Walker& walker, const BodyIndex& index, ValueID read, ValueID call)
{
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    if (index.blockOf(read) != index.blockOf(call) || read.index() >= call.index())
        return false;
    for (uint32_t between = read.index() + 1; between < call.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (declaredBoundInTable(opcode).mayBeRunAgainUnseen() && !operandsMayRunCode(opcode))
            continue;
        if (!operators.contains(opcode) || !isPlainPrimitiveInStepsByHand(walker, index, ValueID(between), read))
            return false;
    }
    return true;
}

static PropertyKeyID nameCalled(Walker& walker, const BodyValue& call)
{
    if (PropertyKeyID key = call.calleeKey())
        return key;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(call.calleeOperand());
    return !index.isParameter(read) && index.opcodeOf(read) == Opcode::ElemGet && walker.armHoldsKeyOf(read) ? keyThatAccessGoesBy(walker, index.value(read)) : PropertyKeyID();
}

static bool readsEngineNext(Walker& walker, const BodyValue& call)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID next = table.canonicalID(Builtins::GeneratorPrototypeNext);
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(call.calleeOperand());
    if (index.isParameter(read) || (index.opcodeOf(read) != Opcode::PropGet && index.opcodeOf(read) != Opcode::ElemGet))
        return false;
    BodyValue access = index.value(read);
    PropertyKeyID key = program.keyOf(BuiltinNames::next);
    bool readsNext = access.opcode() == Opcode::PropGet ? !access.isFreeName() && access.key() == key : nameCalled(walker, call) == key;
    if (!readsNext || index.stripCasts(access.objectOperand()) != index.stripCasts(call.thisValueOperand()))
        return false;
    if (!nothingSeenHappensBetweenInStepsByHand(walker, index, read, call.id()))
        return false;

    BuiltinID prototype = table.at(BuiltinClasses::Generator).prototype;
    if (!engineAnswersForGenerators(walker) || !prototype)
        return false;
    BuiltinID found = findConstantMemberFrom(program, walker.facts(), prototype, key);
    return found && table.canonicalID(found) == next;
}

static bool calleeIsNextOfEngineInStepsByHand(Walker& walker, const BodyValue& call)
{
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.calleeOperand());
    Type typeOfCallee = callee ? types.at(walker.typeOf(callee)) : Type();
    if (BuiltinID named = callee && typeOfCallee.isObject() && typeOfCallee.isJavaScriptValue() ? singleBuiltinOf(types, typeOfCallee) : BuiltinID()) {
        const BuiltinTable& table = walker.program().builtins();
        return table.canonicalID(named) == table.canonicalID(Builtins::GeneratorPrototypeNext);
    }
    return readsEngineNext(walker, call);
}

enum class ResultOfNextInStepsByHand : uint8_t { IsWhatCallGives, IsWhatAwaitOfCallGives };

using OnlyReadSteps = LoopsOverGeneratorsOfUnit::OnlyReadSteps;

static const OnlyReadSteps& findOnlyReadSteps(Walker& walker, ResultOfNextInStepsByHand result = ResultOfNextInStepsByHand::IsWhatCallGives)
{
    bool isAwaited = result == ResultOfNextInStepsByHand::IsWhatAwaitOfCallGives;
    auto& found = isAwaited ? walker.loopsOverGenerators().onlyReadAwaitedSteps : walker.loopsOverGenerators().onlyReadSteps;
    auto added = found.add(walker.currentFunction().index(), OnlyReadSteps());
    OnlyReadSteps& steps = added.iterator->value;
    if (!added.isNewEntry)
        return steps;

    const BodyIndex& index = walker.currentBodyIndex();
    const Program& program = walker.program();
    PropertyKeyID next = program.keyOf(BuiltinNames::next);
    PropertyKeyID done = program.keyOf(BuiltinNames::done);
    PropertyKeyID value = program.keyOf(BuiltinNames::value);
    BitVector& onlyRead = steps.values;
    onlyRead.ensureSize(index.numberOfValues());
    Vector<uint32_t> sources;
    auto sourceOf = [&](ValueID name) {
        return name ? sources[name.index()] : UINT32_MAX;
    };
    auto callsComputedMember = [&](const BodyValue& user) {
        ValueID read = user.calleeKey() || !user.thisValueOperand() ? ValueID() : index.stripCasts(user.calleeOperand());
        return read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::ElemGet && index.stripCasts(index.value(read).objectOperand()) == index.stripCasts(user.thisValueOperand());
    };
    auto isCallOfNext = [&](const BodyValue& user) {
        return user.opcode() == Opcode::Call && (user.calleeKey() == next || callsComputedMember(user));
    };
    bool someCallIsOfNext = false;
    for (uint32_t blockIndex = 0; blockIndex < index.numberOfBlocks() && !someCallIsOfNext; ++blockIndex)
        someCallIsOfNext = std::ranges::any_of(index.block(BlockID(blockIndex)).values(), isCallOfNext);
    if (!someCallIsOfNext)
        return steps;
    sources.fill(UINT32_MAX, index.numberOfValues());
    for (bool hasFoundMore = true; hasFoundMore;) {
        hasFoundMore = false;
        for (uint32_t blockIndex = 0; blockIndex < index.numberOfBlocks(); ++blockIndex) {
            BodyBlock block = index.block(BlockID(blockIndex));
            for (BodyValue user : block.values()) {
                uint32_t source = UINT32_MAX;
                if (isCallOfNext(user))
                    source = user.id().index();
                else if (opcodesThatStateTypes.contains(user.opcode()))
                    source = sourceOf(user.operand(0));
                else if (isAwaited && user.opcode() == Opcode::Await) {
                    ValueID awaited = index.stripCasts(user.valueOperand());
                    source = awaited && sourceOf(awaited) == awaited.index() && !index.isParameter(awaited) ? awaited.index() : UINT32_MAX;
                }
                if (source == UINT32_MAX)
                    continue;
                sources[user.id().index()] = source;
                if (isCallOfNext(user))
                    onlyRead.quickSet(source);
            }
            index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& handed) {
                uint32_t source = sourceOf(handed.subject);
                if (source == UINT32_MAX || sources[handed.parameter.index()] == source || index.valueNarrowedInto(handed.parameter) != handed.subject)
                    return;
                sources[handed.parameter.index()] = source;
                hasFoundMore |= index.blockOf(handed.parameter).index() <= blockIndex;
            });
            if (isAwaited)
                continue;
            block.forEachOutgoingEdge([&](BodyTarget target, bool isHandler) {
                std::span<const ValueID> arguments = target.arguments();
                for (uint32_t position = 0; !isHandler && position < arguments.size(); ++position) {
                    ValueID parameter = index.block(target.block()).parameter(position);
                    if (sourceOf(arguments[position]) == UINT32_MAX || sources[parameter.index()] == parameter.index() || index.isUnused(parameter))
                        continue;
                    sources[parameter.index()] = parameter.index();
                    onlyRead.quickSet(parameter.index());
                    hasFoundMore |= target.block().index() <= blockIndex;
                }
            });
        }
    }

    auto refuse = [&](ValueID mentioned) {
        if (uint32_t source = sourceOf(mentioned); source != UINT32_MAX)
            onlyRead.quickClear(source);
    };
    Vector<std::pair<uint32_t, uint32_t>, 4> passes;
    for (uint32_t blockIndex = 0; blockIndex < index.numberOfBlocks(); ++blockIndex) {
        BodyBlock block = index.block(BlockID(blockIndex));
        refuse(block.subject());
        for (BodyValue user : block.values()) {
            Opcode opcode = user.opcode();
            if (opcodesThatStateTypes.contains(opcode)) {
            } else if (isAwaited && opcode == Opcode::Await && sourceOf(user.id()) != UINT32_MAX) {
                for (ValueID held : user.heldOperands())
                    refuse(held);
            } else if (opcode == Opcode::TypeTest && walker.types().isSubset(user.as<Opcode::TypeTest>().testedType(), TypeTable::nullish)) {
            } else if (opcode == Opcode::PropGet && !user.isFreeName() && (user.key() == done || user.key() == value)) {
            } else {
                user.forEachOperand([&](ValueID operand, uint32_t) {
                    refuse(operand);
                });
            }
        }
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            refuse(operand);
        });
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& handed) {
            if (sourceOf(handed.subject) != UINT32_MAX && sourceOf(handed.parameter) != sourceOf(handed.subject))
                refuse(handed.subject);
        });
        block.forEachOutgoingEdge([&](BodyTarget target, bool isHandler) {
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t position = 0; position < arguments.size(); ++position) {
                ValueID parameter = index.block(target.block()).parameter(position);
                uint32_t source = sourceOf(arguments[position]);
                if (!isHandler && index.isUnused(parameter))
                    continue;
                if (source == UINT32_MAX || isHandler || sourceOf(parameter) != parameter.index())
                    refuse(arguments[position]);
                else
                    passes.append({ source, parameter.index() });
            }
        });
    }
    for (bool hasRefusedMore = true; hasRefusedMore;) {
        hasRefusedMore = false;
        for (auto [source, parameter] : passes) {
            if (onlyRead.quickGet(source) == onlyRead.quickGet(parameter))
                continue;
            onlyRead.quickClear(source);
            onlyRead.quickClear(parameter);
            hasRefusedMore = true;
        }
    }
    for (uint32_t blockIndex = 0; blockIndex < index.numberOfBlocks(); ++blockIndex) {
        for (BodyValue user : index.block(BlockID(blockIndex)).values()) {
            if (isCallOfNext(user) && onlyRead.quickGet(user.id().index()))
                steps.positionsOfCalls.append(user.origin().position);
        }
    }
    return steps;
}

bool isOnlyReadStep(Walker& walker, ValueID step)
{
    return step.index() < walker.currentBodyIndex().numberOfValues() && findOnlyReadSteps(walker).values.quickGet(step.index());
}

static Reference continueIfHasType(Walker&, Reference value, TypeID tested, BlockID otherwise, bool& someWayTurnsAway, Origin);

static Reference buildFirstReadOfNextInStepsByHand(Walker& walker, Reference generator, BlockID byCall, Origin origin)
{
    bool someWayTurnsAway = false;
    generator = continueIfHasType(walker, generator, walker.types().builtinClass(BuiltinClasses::Generator), byCall, someWayTurnsAway, origin);
    Draft test = draftOfCoroutineCalls(walker, Opcode::IsUnmodifiedGenerator, origin);
    test.setValueOperand(generator);
    Reference passes = buildInStepsByHand(walker, WTF::move(test));
    if (!passes)
        return { };
    BlockID passed = walker.addBlock();
    walker.branch(passes, passed, byCall);
    walker.continueIn(passed);
    buildFirstReadsThatOpenOfGeneratorLeavesOut(walker, origin, false);
    return generator;
}

static bool buildStepInStepsByHand(Walker& walker, const BodyValue& call, Reference generator, Reference sent, bool resultIsOnlyRead)
{
    bool readsNextAfterStep = !walker.referenceOf(call.calleeOperand());
    if (readsNextAfterStep)
        walker.bind(call.calleeOperand(), generator);
    Draft written = walker.draftOf(call);
    if (readsNextAfterStep)
        walker.bind(call.calleeOperand(), Reference());
    Origin origin = written.origin();
    origin.isTailCall = false;
    BlockID byCall;
    if (readsNextAfterStep && mustReadGeneratorNext(walker)) {
        byCall = walker.addBlock(BlockFrequency::Rare);
        generator = buildFirstReadOfNextInStepsByHand(walker, generator, byCall, origin);
    }
    Reference stepped = walker.isUnreachable() ? Reference() : buildStepOfGenerator(walker, generator, ResumeMode::Next, sent, checkLeftToStepOf(walker, generator), origin);
    Reference value = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Value, origin);
    Reference status = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Status, origin);
    Reference isNotStepped = buildTestOfStatusInStepsByHand(walker, status, GeneratorStepStatus::NotStepped, origin);
    if (walker.hasFailed() || (!isNotStepped && !byCall))
        return true;

    if (!byCall)
        byCall = walker.addBlock(BlockFrequency::Rare);
    BlockID isStepped = walker.addBlock();
    BlockID meeting = walker.addBlock();
    Reference whole;
    uint32_t numberOfParameters = resultIsOnlyRead ? static_cast<uint32_t>(numberOfParametersOfMeetingInStepsByHand) : 1;
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        bool isTruth = parameter == IsAnswered || parameter == Done;
        Reference added = isTruth ? walker.addParameter(meeting, TypeTable::boolean, Representation::Boolean) : walker.addParameter(meeting, { }, Representation::JSValue);
        if (parameter == Whole)
            whole = added;
    }
    if (resultIsOnlyRead)
        walker.code().markMeetingOfStepWrittenByHand(meeting);
    bool someWayArrives = false;
    if (isNotStepped)
        walker.branch(isNotStepped, byCall, isStepped);

    walker.continueIn(byCall);
    written.setOrigin(origin);
    written.resultIsReturnedAtOnce = false;
    written.inputValueGiven = { };
    if (readsNextAfterStep) {
        Draft read = walker.draftOf(walker.currentBodyIndex().value(call.calleeOperand()));
        read.inputValueGiven = { };
        walker.chooseConservativeFields(read);
        written.setCalleeOperand(buildInStepsByHand(walker, WTF::move(read)));
    }
    Reference answer;
    if (!walker.isUnreachable()) {
        walker.chooseConservativeFields(written);
        answer = walker.build(WTF::move(written));
    }
    std::array answered { answer, answer, booleanInStepsByHand(walker, true, origin), answer, booleanInStepsByHand(walker, false, origin) };
    jumpToInStepsByHand(walker, meeting, std::span<const Reference>(answered).first(numberOfParameters), someWayArrives);

    walker.continueIn(isStepped);
    Reference isCompleted = buildTestOfStatusInStepsByHand(walker, status, GeneratorStepStatus::Completed, origin);
    Reference made = buildResultOfStep(walker, value, isCompleted, origin);
    std::array atHand { made, value, booleanInStepsByHand(walker, false, origin), value, isCompleted };
    jumpToInStepsByHand(walker, meeting, std::span<const Reference>(atHand).first(numberOfParameters), someWayArrives);
    if (!someWayArrives || walker.hasFailed())
        return true;

    walker.continueIn(meeting);
    walker.bind(call.id(), whole);
    if (resultIsOnlyRead)
        walker.loopsOverGenerators().hasStepsWrittenByHand = true;
    else
        walker.rememberedExpectationOf(call.id()) = { walker.types().forLayout(walker.program().layoutOf(BuiltinLayouts::IteratorResult)), false, false, true };
    return true;
}

static bool isResultOfLibraryStep(Walker& walker, const Operation& made)
{
    const Program& program = walker.program();
    std::span<const InlineFrame> frames = walker.code().inlineFrames();
    if (made.opcode != Opcode::NewObject || made.layout() != program.layoutOf(BuiltinLayouts::IteratorResult) || !made.inlineFrame)
        return false;
    const InlineFrame& frame = frames[made.inlineFrame];
    return frame.parent == walker.inlineFrameAtWork() && frame.callee && program.at(frame.callee).role == FunctionRole::Library && findOnlyReadSteps(walker).positionsOfCalls.contains(frame.callPosition);
}

static std::optional<StepParts> findStepParts(Walker& walker, Reference name)
{
    const Code& code = walker.code();
    while (name && name.isValue() && !code.isConstant(name.id()) && cellPreservingOpcodes.contains(code.at(name.id()).opcode)) {
        ValueID record = code.at(name.id()).operand(0);
        name = code.isConstant(record) ? Reference::constant(record) : Reference::value(record);
    }
    if (!name || !name.isValue() || code.isConstant(name.id()))
        return std::nullopt;
    const Operation& made = code.at(name.id());
    if (made.opcode != Opcode::Parameter) {
        if (!isResultOfLibraryStep(walker, made))
            return std::nullopt;
        std::span<const ValueID> values = made.valuesOperands();
        auto partAt = [&](ValueID part) { return code.isConstant(part) ? Reference::constant(part) : Reference::value(part); };
        return StepParts { partAt(values[0]), booleanInStepsByHand(walker, false, made.origin()), partAt(values[0]), partAt(values[1]), true };
    }
    const auto& partsOfParameters = walker.loopsOverGenerators().partsOfParameters;
    if (auto parts = partsOfParameters.find(name.id().index()); parts != partsOfParameters.end()) {
        const auto& [references, isRarelyAnswered] = parts->value;
        return StepParts { Reference::fromBits(references[0]), Reference::fromBits(references[1]), Reference::fromBits(references[2]), Reference::fromBits(references[3]), isRarelyAnswered };
    }
    const Block& meeting = code.at(code.blockOf(name.id()));
    std::span<const ValueID> parameters = meeting.parameters();
    if (!meeting.meetsStepWrittenByHand || parameters.size() != numberOfParametersOfMeetingInStepsByHand || parameters[Whole] != name.id())
        return std::nullopt;
    return StepParts { Reference::value(parameters[Answer]), Reference::value(parameters[IsAnswered]), Reference::value(parameters[Value]), Reference::value(parameters[Done]) };
}

bool hasStepParts(Walker& walker, Reference result)
{
    return findStepParts(walker, result).has_value();
}

void appendStepParts(Walker& walker, Reference result, Vector<Reference, 8>& arguments)
{
    std::optional<StepParts> parts = findStepParts(walker, result);
    if (!parts)
        parts = StepParts { result, booleanInStepsByHand(walker, true, { }), result, booleanInStepsByHand(walker, false, { }) };
    arguments.append(parts->answer);
    arguments.append(parts->isAnswered);
    arguments.append(parts->value);
    arguments.append(parts->done);
}

static TypeID typeOfStepPart(uint32_t part)
{
    return part % 2 ? TypeID(TypeTable::boolean) : TypeID(TypeTable::anything);
}

static Representation formOfStepPart(uint32_t part)
{
    return part % 2 ? Representation::Boolean : Representation::JSValue;
}

static bool carriesSteps(Walker& walker)
{
    return walker.loopsOverGenerators().hasStepsWrittenByHand && !walker.isInGeneratorConsumer();
}

bool isUnlikelyStepResult(Walker& walker, Reference result)
{
    std::optional<StepParts> parts = findStepParts(walker, result);
    return parts && parts->isRarelyAnswered;
}

void addStepParameters(Walker& walker, BlockID block, Reference whole, bool isRarelyAnswered, Vector<ValueID, 8>& parameters, Origin origin)
{
    LoopsOverGeneratorsOfUnit::CarriedParts parts { { }, isRarelyAnswered };
    for (uint32_t part = 0; part < LoopsOverGeneratorsOfUnit::numberOfParts; ++part) {
        parameters.append(walker.code().appendParameter(block, typeOfStepPart(part), formOfStepPart(part), origin));
        parts.references[part] = Reference::value(parameters.last()).bits();
    }
    walker.loopsOverGenerators().partsOfParameters.set(whole.id().index(), parts);
}

void carryStepParts(Walker& walker, BlockID inputHead, BlockID header, Vector<Reference, 8>& entering, Vector<ValueID, 8>& parameters, Origin origin)
{
    if (!carriesSteps(walker))
        return;
    BodyBlock input = walker.currentBodyIndex().block(inputHead);
    Vector<uint32_t, 2> positions;
    for (uint32_t position = 0; position < input.numberOfParameters(); ++position) {
        if (!isOnlyReadStep(walker, input.parameter(position)) || !hasStepParts(walker, entering[position]))
            continue;
        bool isRarelyAnswered = isUnlikelyStepResult(walker, entering[position]);
        appendStepParts(walker, entering[position], entering);
        addStepParameters(walker, header, Reference::value(parameters[position]), isRarelyAnswered, parameters, origin);
        positions.append(position);
    }
    if (!positions.isEmpty())
        walker.loopsOverGenerators().stepPositionsOfHeaders.set(header.index(), WTF::move(positions));
}

static bool isFalseConstant(const Walker& walker, Reference truth)
{
    const Code& code = walker.code();
    return truth && truth.isRecord() && code.isConstant(truth.id()) && code.at(truth.id()).opcode == Opcode::Boolean && !code.at(truth.id()).boolean();
}

void addStepEntryParameters(Walker& walker, BlockID inputHead, BlockID entry, std::span<const Reference> entering, std::span<const Reference> results, Vector<Reference, 8>& arguments)
{
    if (!carriesSteps(walker))
        return;
    BodyBlock input = walker.currentBodyIndex().block(inputHead);
    for (uint32_t position = 0; position < input.numberOfParameters() && position < results.size(); ++position) {
        if (!isOnlyReadStep(walker, input.parameter(position)) || !hasStepParts(walker, entering[position]))
            continue;
        LoopsOverGeneratorsOfUnit::CarriedParts parts { { }, isUnlikelyStepResult(walker, entering[position]) };
        for (uint32_t part = 0; part < LoopsOverGeneratorsOfUnit::numberOfParts; ++part)
            parts.references[part] = walker.addParameter(entry, typeOfStepPart(part), formOfStepPart(part)).bits();
        walker.loopsOverGenerators().partsOfParameters.set(results[position].id().index(), parts);
        appendStepParts(walker, entering[position], arguments);
    }
}

void appendHeldSteps(Walker& walker, BlockID header, Vector<Reference, 8>& held)
{
    Vector<Reference, 8> results = held;
    appendCarriedSteps(walker, header, results.span(), held);
}

void appendCarriedSteps(Walker& walker, BlockID header, std::span<const Reference> results, Vector<Reference, 8>& arguments)
{
    const auto& positions = walker.loopsOverGenerators().stepPositionsOfHeaders;
    auto carried = positions.find(header.index());
    if (carried == positions.end())
        return;
    for (uint32_t position : carried->value)
        appendStepParts(walker, results[position], arguments);
}

bool answerFromStepWrittenByHandOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    bool isTest = value.opcode() == Opcode::TypeTest;
    if (!isTest && value.isFreeName())
        return false;
    ValueID subject = isTest ? value.operand(0) : value.objectOperand();
    std::optional<StepParts> step = subject ? findStepParts(walker, walker.referenceBoundTo(subject)) : std::nullopt;
    if (!step)
        return false;

    const Program& program = walker.program();
    Draft written = walker.draftOf(value);
    Reference atHand;
    if (isTest) {
        if (!walker.types().isSubset(value.as<Opcode::TypeTest>().testedType(), TypeTable::nullish))
            return false;
        atHand = booleanInStepsByHand(walker, false, written.origin());
    } else if (value.key() == program.keyOf(BuiltinNames::done))
        atHand = step->done;
    else if (value.key() == program.keyOf(BuiltinNames::value))
        atHand = step->value;
    else
        return false;

    if (isFalseConstant(walker, step->isAnswered)) {
        walker.bind(value.id(), atHand);
        return true;
    }

    ValueID onlyUser = isTest ? ValueID() : walker.currentBodyIndex().onlyUsingValue(value.id());
    bool isTruthAlone = onlyUser && atHand == step->done && walker.currentBodyIndex().opcodeOf(onlyUser) == Opcode::ToBoolean;
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID ofStep = walker.addBlock();
    BlockID meeting = walker.addBlock();
    Reference read = isTest || isTruthAlone ? walker.addParameter(meeting, TypeTable::boolean, Representation::Boolean) : walker.addParameter(meeting, { }, Representation::JSValue);
    bool someWayArrives = false;
    walker.branch(step->isAnswered, asWritten, ofStep);

    walker.continueIn(asWritten);
    {
        std::optional<Walker::ClosedArm> closedArm;
        if (step->isRarelyAnswered)
            closedArm.emplace(walker, true);
        Origin origin = written.origin();
        written.setOperand(0, step->answer);
        walker.chooseConservativeFields(written);
        Reference offAnswer = buildInStepsByHand(walker, WTF::move(written));
        if (isTruthAlone && !walker.isUnreachable()) {
            Draft truth(Opcode::ToBoolean, origin);
            truth.setValueOperand(offAnswer);
            walker.chooseConservativeFields(truth);
            offAnswer = walker.build(WTF::move(truth));
        }
        jumpToInStepsByHand(walker, meeting, singleElementSpan(offAnswer), someWayArrives);
    }

    walker.continueIn(ofStep);
    jumpToInStepsByHand(walker, meeting, singleElementSpan(atHand), someWayArrives);
    if (!someWayArrives || walker.hasFailed())
        return true;
    walker.continueIn(meeting);
    walker.bind(value.id(), read);
    return true;
}

static bool isGeneratorStep(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::BuiltinCalls) || !walker.opensGuesses())
        return false;
    const Program& program = walker.program();
    if (!call.thisValueOperand() || call.argumentsOperands().size() > 1 || nameCalled(walker, call) != program.keyOf(BuiltinNames::next))
        return false;

    Reference generator = walker.referenceOf(call.thisValueOperand());
    if (!generator || !generator.isValue())
        return false;
    TypeArena& types = walker.types();
    TypeID typeOfGenerator = walker.typeOf(generator);
    if (!types.at(typeOfGenerator).isObject() || !types.isSubset(typeOfGenerator, types.builtinClass(BuiltinClasses::Generator))) {
        TypeID expected = walker.has(Mechanism::Guesses) ? findExpectation(walker, call.thisValueOperand()).type : TypeID();
        TypeID present = expected ? types.withoutNullish(expected) : TypeID();
        bool nothingIsKnown = walker.has(Mechanism::Guesses) && walker.isInLoop() && walker.fileHoldsGenerators() && types.isUninformative(typeOfGenerator) && (!present || types.isUninformative(present));
        if (!nothingIsKnown && (!present || isNeverType(present) || !types.isSubset(present, types.builtinClass(BuiltinClasses::Generator))))
            return false;
    }
    return walker.isInLoop() || findOnlyReadSteps(walker).values.quickGet(call.id().index());
}

bool callStepsGeneratorOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    return isGeneratorStep(walker, call) && readsEngineNext(walker, call);
}

bool leaveNextToStepOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::next))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID taker = index.onlyUsingValue(read.id());
    if (!taker || index.opcodeOf(taker) != Opcode::Call || index.blockOf(taker) != index.blockOf(read.id()))
        return false;
    BodyValue call = index.value(taker);
    if (call.calleeOperand() != read.id() || taker.index() - index.block(index.blockOf(taker)).firstValue().index() >= walker.endOfStretchBeingWalked())
        return false;
    if (!callStepsGenerator(walker, call))
        return false;
    walker.bind(read.id(), Reference());
    return true;
}

bool buildStepWrittenByHand(Walker& walker, const BodyValue& call)
{
    if (!isGeneratorStep(walker, call) || !calleeIsNextOfEngineInStepsByHand(walker, call))
        return false;
    bool resultIsOnlyRead = findOnlyReadSteps(walker).values.quickGet(call.id().index());
    Reference sent = call.argumentsOperands().empty() ? undefinedOfCoroutineCalls(walker) : walker.referenceOf(call.argumentsOperands()[0]);
    return sent && buildStepInStepsByHand(walker, call, walker.referenceOf(call.thisValueOperand()), sent, resultIsOnlyRead);
}

static Reference continueIfHasType(Walker& walker, Reference value, TypeID tested, BlockID otherwise, bool& someWayTurnsAway, Origin origin)
{
    if (walker.isUnreachable() || walker.types().isSubset(walker.typeOf(value), tested))
        return value;
    Draft test = draftOfCoroutineCalls(walker, Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(tested);
    Reference passes = walker.build(WTF::move(test));
    if (!passes)
        return { };
    BlockID passed = walker.addBlock();
    walker.branch(passes, passed, otherwise);
    someWayTurnsAway = true;
    walker.continueIn(passed);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(value);
    proof.setTestOperand(passes);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    return buildInStepsByHand(walker, WTF::move(proof));
}

static Reference buildAwaitedStepInStepsByHand(Walker& walker, const BodyValue& call, Reference callee, Reference receiver, Reference sent, std::span<const Reference> held, bool resultIsOnlyRead, Origin originOfAwait)
{
    TypeArena& types = walker.types();
    BuiltinID next = walker.program().builtins().canonicalID(Builtins::AsyncGeneratorPrototypeNext);
    Draft written = walker.draftOf(call);
    Origin origin = written.origin();
    origin.isTailCall = false;

    BlockID byCall = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    Reference whole;
    for (uint32_t parameter = 0; parameter < numberOfParametersOfMeetingInStepsByHand; ++parameter) {
        bool isTruth = parameter == IsAnswered || parameter == Done;
        Reference added = isTruth ? walker.addParameter(meeting, TypeTable::boolean, Representation::Boolean) : walker.addParameter(meeting, { }, Representation::JSValue);
        if (parameter == Whole)
            whole = added;
    }
    if (resultIsOnlyRead)
        walker.code().markMeetingOfStepWrittenByHand(meeting);
    bool someWayArrives = false;
    bool someWayTurnsAway = false;

    continueIfHasType(walker, callee, types.forBuiltin(next), byCall, someWayTurnsAway, origin);
    Reference generator = continueIfHasType(walker, receiver, types.builtinClass(BuiltinClasses::AsyncGenerator), byCall, someWayTurnsAway, origin);
    if (walker.shouldFailSpeculations() && !walker.isUnreachable()) [[unlikely]] {
        walker.letGuessFailUnasked(byCall, origin);
        someWayTurnsAway = true;
    }
    walker.recordSpeculation();
    Reference stepped = walker.isUnreachable() ? Reference() : buildStepOfAsyncGenerator(walker, generator, sent, held, next, { }, origin);
    Reference value = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Value, origin);
    Reference status = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Status, origin);
    if (Reference isAnsweredByObject = buildTestOfStatusInStepsByHand(walker, status, AsyncGeneratorStepStatus::Result, origin)) {
        BlockID ofEngine = walker.addBlock(BlockFrequency::Rare);
        BlockID ofStep = walker.addBlock();
        walker.branch(isAnsweredByObject, ofEngine, ofStep);
        walker.continueIn(ofEngine);
        jumpToInStepsByHand(walker, meeting, std::array { value, value, booleanInStepsByHand(walker, true, origin), value, booleanInStepsByHand(walker, false, origin) }, someWayArrives);
        walker.continueIn(ofStep);
        Reference isCompleted = buildTestOfStatusInStepsByHand(walker, status, AsyncGeneratorStepStatus::Completed, origin);
        Reference made = buildResultOfStep(walker, value, isCompleted, origin);
        jumpToInStepsByHand(walker, meeting, std::array { made, value, booleanInStepsByHand(walker, false, origin), value, isCompleted }, someWayArrives);
    }

    if (someWayTurnsAway && !walker.hasFailed()) {
        walker.continueIn(byCall);
        written.setOrigin(origin);
        written.resultIsReturnedAtOnce = false;
        written.inputValueGiven = { };
        walker.chooseConservativeFields(written);
        Reference promised = walker.build(WTF::move(written));
        Reference answer = promised ? buildAwaitBySavingFrame(walker, promised, held, originOfAwait) : Reference();
        if (answer)
            jumpToInStepsByHand(walker, meeting, std::array { answer, answer, booleanInStepsByHand(walker, true, origin), answer, booleanInStepsByHand(walker, false, origin) }, someWayArrives);
    }
    if (!someWayArrives || walker.hasFailed())
        return { };
    walker.continueIn(meeting);
    walker.loopsOverGenerators().hasStepsWrittenByHand = true;
    return whole;
}

bool buildAwaitedStepWrittenByHand(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    const Program& program = walker.program();
    if (!call.thisValueOperand() || call.argumentsOperands().size() > 1 || call.calleeKey() != program.keyOf(BuiltinNames::next) || !canWaitForStepOfAsyncGenerator(walker) || program.at(walker.unit().function).isModuleBody())
        return false;

    Reference receiver = walker.referenceOf(call.thisValueOperand());
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!receiver || !receiver.isValue() || !callee)
        return false;
    TypeArena& types = walker.types();
    TypeID generators = types.builtinClass(BuiltinClasses::AsyncGenerator);
    TypeID typeOfReceiver = walker.typeOf(receiver);
    if (!types.at(typeOfReceiver).isObject() || !types.isSubset(typeOfReceiver, generators)) {
        TypeID expected = findExpectation(walker, call.thisValueOperand()).type;
        TypeID present = expected ? types.withoutNullish(expected) : TypeID();
        if (!present || isNeverType(present) || !types.isSubset(present, generators))
            return false;
    }

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    std::optional<WaitOfCallInCoroutine> wait = findWaitOfCallInCoroutine(walker, index, block, call.id());
    if (!wait)
        return false;
    Reference sent = call.argumentsOperands().empty() ? undefinedOfCoroutineCalls(walker) : walker.referenceOf(call.argumentsOperands()[0]);
    if (!sent)
        return false;

    Vector<Reference, 4> held;
    if (!walkUpToAwaitOfCall(walker, index, block, *wait, held))
        return true;
    bool resultIsOnlyRead = findOnlyReadSteps(walker, ResultOfNextInStepsByHand::IsWhatAwaitOfCallGives).values.quickGet(call.id().index());
    if (Reference whole = buildAwaitedStepInStepsByHand(walker, call, callee, receiver, sent, held.span(), resultIsOnlyRead, walker.originOf(index.value(wait->await))))
        finishWaitOfCall(walker, block, *wait, whole);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

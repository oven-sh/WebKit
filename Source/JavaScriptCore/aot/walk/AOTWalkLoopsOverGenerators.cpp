/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkLoopsOverGenerators.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTWalkLoopsOverGeneratorsBuilder.h"

namespace JSC { namespace AOT { namespace Typed {

bool stepsOfPatternStandInRow(const BodyIndex&, const BodyValue& open);

static constexpr uint32_t closedOperationsOfProtocolForGenerators = 3;
static constexpr uint32_t mostStepsOfPatternOverGenerator = 4;

std::optional<LoopOverIterator> findLoopOverIterator(const BodyIndex& index, ValueID open)
{
    LoopOverIterator loop;
    Vector<BlockID, 4> blocksOfCloses;
    ValueID lastUser;
    for (const BodyMentionOfIteration& mention : index.mentionsOfIteration(open)) {
        if (std::exchange(lastUser, mention.user) == mention.user)
            continue;
        BodyValue operation = index.value(mention.user);
        if (!index.isReachable(mention.block))
            continue;
        if (operation.opcode() == Opcode::IteratorClose) {
            blocksOfCloses.append(mention.block);
            continue;
        }
        if (operation.opcode() != Opcode::IteratorResume || loop.step)
            return std::nullopt;
        auto step = operation.as<Opcode::IteratorResume>();
        if (step.resumeMode() != ResumeMode::Next || step.sentOperand() || step.readsValueWhenDone())
            return std::nullopt;
        loop.step = mention.user;
        loop.blockOfStep = mention.block;
    }
    if (!loop.step)
        return std::nullopt;

    LoopID loopOfOpen = index.loopOf(index.blockOf(open));
    if (LoopID loopOfStep = index.loopOf(loop.blockOfStep); loopOfStep != loopOfOpen) {
        if (!loopOfStep || index.loop(loopOfStep).parent != loopOfOpen || index.loop(loopOfStep).head != loop.blockOfStep)
            return std::nullopt;
        loop.stepStandsInHeadOfLoop = true;
    }

    BodyTerminator terminator = index.terminator(loop.blockOfStep);
    if (terminator.kind() != TerminatorKind::Branch)
        return std::nullopt;
    ValueID condition = terminator.operand(0);
    if (index.isParameter(condition) || index.opcodeOf(condition) != Opcode::ResultAt)
        return std::nullopt;
    auto read = index.value(condition).as<Opcode::ResultAt>();
    if (read.resultsOperand() != loop.step || read.index() != static_cast<uint32_t>(IteratorResumeResult::Done))
        return std::nullopt;
    BlockID turn = terminator.notTaken().block();
    if (index.numberOfIncomingEdges(turn) != 1)
        return std::nullopt;
    for (BlockID block : blocksOfCloses) {
        if (!index.dominates(turn, block))
            return std::nullopt;
    }
    return loop;
}

static bool isConstantLocationForGenerators(const Walker& walker, BuiltinID member)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    if (table.has(BuiltinMark::MayBeAbsent, member) || program.isUnlocked(member))
        return false;
    if (table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member))
        return true;
    return !program.isUnlocked(table.canonicalID(member)) && walker.facts().builtinLocation(member) == BuiltinLocationState::Constant;
}

std::optional<BuiltinID> findMemberAboveForIteration(const Walker& walker, BuiltinID start, BuiltinName name, MembersBehindWatch watch)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    PropertyKeyID key = program.keyOf(name);
    if (!start || !key)
        return std::nullopt;
    MembersUnderKey members(program, key);
    BuiltinID holder = table.canonicalID(start);
    while (true) {
        if (BuiltinID member = members.findOwnMember(holder)) {
            bool liesOnHolder = !table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
            bool isBelieved = isConstantLocationForGenerators(walker, member) || (watch == MembersBehindWatch::AreAsBuilt && walker.facts().isBehindWatch(member));
            return liesOnHolder && isBelieved ? std::optional(member) : std::nullopt;
        }
        auto opened = watch == MembersBehindWatch::AreAsBuilt ? MembersUnderKey::NamesOpenedForTestRun::AreAbsent : MembersUnderKey::NamesOpenedForTestRun::MayLieOnHolder;
        if (!listsEveryMemberUnder(program, holder, key) || !members.isClosedKey(holder, opened) || !table.hasLockedPrototypeLink(holder))
            return std::nullopt;
        switch (table.at(holder).prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return std::nullopt;
        case BuiltinPrototypeLink::Null:
            return BuiltinID();
        case BuiltinPrototypeLink::Builtin:
            holder = table.canonicalID(table.at(holder).inheritsFrom);
            break;
        }
    }
}

static bool findsEngineAnswersForGenerators(const Walker& walker)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID prototype = table.at(BuiltinClasses::Generator).prototype;
    auto finds = [&](BuiltinName name, BuiltinID expected) {
        std::optional<BuiltinID> member = findMemberAboveForIteration(walker, prototype, name);
        return member && *member && table.canonicalID(*member) == table.canonicalID(expected);
    };
    if (!finds(BuiltinNames::next, Builtins::GeneratorPrototypeNext) || !finds(BuiltinNames::returnKeyword, Builtins::GeneratorPrototypeReturn))
        return false;

    std::optional<BuiltinID> method = findMemberAboveForIteration(walker, prototype, table.nameOf(WellKnownSymbol::Iterator));
    if (!method || !*method)
        return false;
    bool returnsReceiver = false;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(*method))) {
        if (overload.mode != BuiltinCallMode::Call || overload.code.implementation.inlineBuiltin != InlineBuiltin::ReturnThis)
            return false;
        returnsReceiver = true;
    }
    return returnsReceiver;
}

bool engineAnswersForGenerators(Walker& walker)
{
    std::optional<bool>& answer = walker.loopsOverGenerators().engineAnswers;
    if (!answer)
        answer = findsEngineAnswersForGenerators(walker);
    return *answer;
}

static bool firstReadIsSeenForGenerators(const Walker& walker, BuiltinID location)
{
    const BuiltinTable& table = walker.program().builtins();
    if (isMadeWithCodeOfLibrary(table, location))
        return true;
    const Builtin& entry = table.at(location);
    if (!entry.isAddedAtFirstRead || !entry.holder)
        return false;
    return walker.facts().listsKeysOf(table.canonicalID(entry.holder));
}

static void buildFirstReadForGenerators(Walker& walker, BuiltinName name, Origin origin)
{
    if (walker.isUnreachable())
        return;
    std::optional<BuiltinID> location = findMemberAboveForIteration(walker, walker.program().builtins().at(BuiltinClasses::Generator).prototype, name);
    if (!location || !*location || !firstReadIsSeenForGenerators(walker, *location))
        return;
    if (walker.has(Mechanism::Folding) && walker.state().hasReadBuiltin(*location))
        return;
    origin.isTailCall = false;
    Draft initialization(opcodeOfFirstReadOffHolder(walker.program().builtins(), *location), origin);
    walker.chooseConservativeFields(initialization);
    initialization.setBuiltin(*location);
    walker.build(WTF::move(initialization));
    if (!walker.isUnreachable())
        walker.state().noteReadOfBuiltin(*location);
}

void buildFirstReadsThatOpenOfGeneratorLeavesOut(Walker& walker, Origin origin, bool asksGeneratorForIterator)
{
    if (asksGeneratorForIterator)
        buildFirstReadForGenerators(walker, walker.program().builtins().nameOf(WellKnownSymbol::Iterator), origin);
    buildFirstReadForGenerators(walker, BuiltinNames::next, origin);
}

void buildFirstReadThatCloseOfGeneratorLeavesOut(Walker& walker, Origin origin)
{
    buildFirstReadForGenerators(walker, BuiltinNames::returnKeyword, origin);
}


static Reference buildStepOfGenerator(Walker& walker, BuilderForGenerators& builder, Reference generator, ResumeMode mode, GeneratorStepCheck check)
{
    RELEASE_ASSERT(check != GeneratorStepCheck::None);
    if (walker.isUnreachable())
        return { };
    Draft step = builder.draft(Opcode::StepGenerator);
    step.setGeneratorOperand(generator);
    step.setResumeMode(mode);
    step.setValueOperand(builder.undefined());
    step.setCheck(check);
    return builder.build(WTF::move(step));
}

static Reference buildNextStepOfGeneratorAskedAtOpen(Walker& walker, BuilderForGenerators& builder, Reference generator, const PassedTest&)
{
    if (walker.isUnreachable())
        return { };
    Draft step = builder.draft(Opcode::StepGenerator);
    step.setGeneratorOperand(generator);
    step.setResumeMode(ResumeMode::Next);
    step.setValueOperand(builder.undefined());
    step.setCheck(GeneratorStepCheck::None);
    return builder.build(WTF::move(step));
}

static void leaveAnythingForCalleeListedAtForGenerators(Walker& walker, const BodyValue& operation)
{
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()))
        walker.leaveAnythingAt(listed);
}

static TypeID typeOfGenerators(Walker& walker)
{
    return walker.types().builtinClass(BuiltinClasses::Generator);
}

static bool isStateOfLoopOverGenerator(Walker& walker, Reference state)
{
    return state && state.isValue() && walker.loopsOverGenerators().statesOpened.contains(state.bits());
}

static Reference listOfWordsMadeForState(Walker& walker, Reference state)
{
    return Reference::fromBits(walker.loopsOverGenerators().statesOpened.get(state.bits()).listOfWords);
}

GeneratorStepCheck checkLeftToStepOf(Walker& walker, Reference generator)
{
    TypeID proven = generator && generator.isValue() ? walker.typeOf(generator) : TypeID();
    return proven && walker.types().isSubset(proven, typeOfGenerators(walker)) ? GeneratorStepCheck::IsUnmodified : GeneratorStepCheck::IsUnmodifiedGenerator;
}

Reference buildStepOfGenerator(Walker& walker, Reference generator, ResumeMode mode, Reference sent, GeneratorStepCheck check, Origin origin)
{
    RELEASE_ASSERT(check != GeneratorStepCheck::None);
    if (walker.isUnreachable())
        return { };
    BuilderForGenerators builder(walker, origin);
    Draft step = builder.draft(Opcode::StepGenerator);
    step.setGeneratorOperand(generator);
    step.setResumeMode(mode);
    step.setValueOperand(sent ? sent : builder.undefined());
    step.setCheck(check);
    return builder.build(WTF::move(step));
}

static bool openLoopOverGenerator(Walker& walker, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    if (written.iteratorKind() != IteratorKind::Sync || written.isDelegating() || written.isDestructuring())
        return false;
    Reference iterable = walker.referenceOf(written.iterableOperand());
    TypeID proven = iterable && iterable.isValue() ? walker.typeOf(iterable) : TypeID();
    if (!proven)
        return false;

    TypeArena& types = walker.types();
    TypeID generators = typeOfGenerators(walker);
    bool classIsProven = types.isSubset(proven, generators);
    bool isDeclared = true;
    if (!classIsProven) {
        ExpectationOfValue expectation = findExpectation(walker, written.iterableOperand());
        TypeID expected = expectation.type ? types.withoutNullish(expectation.type) : TypeID();
        const ClassPart* onlyClass = expected && types.at(expected).isObject() ? singleBuiltinClassOf(types, types.at(expected)) : nullptr;
        if (!onlyClass || onlyClass->builtinClass != BuiltinClasses::Generator || !types.overlap(proven, generators))
            return false;
        isDeclared = expectation.isDeclared;
    }
    std::optional<LoopOverIterator> loop = findLoopOverIterator(walker.currentBodyIndex(), open.id());
    if (!loop || !engineAnswersForGenerators(walker))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Folding) || !walker.opensGuesses() || !hasInstanceTest(walker.program().builtins(), BuiltinClasses::Generator))
        return false;
    if (!loop->stepStandsInHeadOfLoop && !shouldOpenGuess(walker, iterable, generators, isDeclared, closedOperationsOfProtocolForGenerators))
        return false;

    BuilderForGenerators builder(walker, walker.originOf(open));
    BlockID byProtocol = builder.addBlock(BlockFrequency::Unlikely);
    BlockID opened = builder.addBlock();
    Reference state = walker.addParameter(opened, { }, Representation::JSValue);

    Reference generator = classIsProven ? iterable : builder.continueIfTypeMatches(iterable, generators, byProtocol);
    Draft test = builder.draft(Opcode::IsUnmodifiedGenerator);
    test.setValueOperand(generator);
    std::optional<PassedTest> generatorIsAsMade = builder.continueIfCheckPasses(builder.build(WTF::move(test)), byProtocol);
    if (generatorIsAsMade) {
        buildFirstReadsThatOpenOfGeneratorLeavesOut(walker, builder.origin());
        builder.jumpTo(opened, { generator });
    } else if (!walker.isUnreachable())
        builder.jumpTo(byProtocol);

    builder.continueIn(byProtocol);
    Reference words = builder.openByProtocol(iterable);
    builder.jumpTo(opened, { words });

    walker.recordSpeculation();
    if (walker.isUnreachable())
        return true;
    walker.loopsOverGenerators().statesOpened.add(state.bits(), LoopsOverGeneratorsOfUnit::StateOpened { words.bits(), generatorIsAsMade });
    walker.bind(open.id(), state);
    return true;
}

static void stepGeneratorInPlace(Walker& walker, BuilderForGenerators& builder, Reference generator, const std::optional<PassedTest>& generatorWasAsMade, const BuilderForGenerators::MeetingOfStep& after)
{
    Reference stepped = generatorWasAsMade ? buildNextStepOfGeneratorAskedAtOpen(walker, builder, generator, *generatorWasAsMade) : buildStepOfGenerator(walker, builder, generator, ResumeMode::Next, GeneratorStepCheck::IsUnmodified);
    Reference value = builder.resultAt(stepped, static_cast<uint32_t>(StepGeneratorResult::Value));
    Reference status = builder.resultAt(stepped, static_cast<uint32_t>(StepGeneratorResult::Status));

    BlockID byMethod = builder.addBlock(BlockFrequency::Rare);
    BlockID isStepped = builder.addBlock();
    builder.branch(builder.isInteger(status, static_cast<int64_t>(GeneratorStepStatus::NotStepped)), byMethod, isStepped);
    builder.continueIn(byMethod);
    Draft resumption = builder.draft(Opcode::ResumeGenerator);
    resumption.setGeneratorOperand(generator);
    resumption.setResumeMode(ResumeMode::Next);
    resumption.setValueOperand(builder.undefined());
    builder.readAnswerOfStep(builder.build(WTF::move(resumption)), after);

    builder.continueIn(isStepped);
    builder.endStep(builder.isInteger(status, static_cast<int64_t>(GeneratorStepStatus::Completed)), value, after);
}

static bool stepLoopOverGenerator(Walker& walker, const BodyValue& step)
{
    Reference state = walker.referenceOf(step.operand(0));
    if (!isStateOfLoopOverGenerator(walker, state))
        return false;

    BuilderForGenerators builder(walker, walker.originOf(step));
    BuilderForGenerators::MeetingOfStep after = builder.openMeetingOfStep();
    BlockID byProtocol = builder.addBlock(BlockFrequency::Unlikely);
    Reference generator = builder.continueIfTypeMatches(state, typeOfGenerators(walker), byProtocol);
    std::optional<PassedTest> generatorWasAsMade = walker.loopsOverGenerators().statesOpened.get(state.bits()).generatorWasAsMade;
    stepGeneratorInPlace(walker, builder, generator, generatorWasAsMade, after);

    builder.continueIn(byProtocol);
    Reference words = builder.takeWordsOfProtocol(state, listOfWordsMadeForState(walker, state));
    Reference iterator = builder.iteratorIn(words);
    builder.stepByProtocol(iterator, builder.nextIn(words), after);
    if (Reference results = builder.resultsOf(after.done, after.value))
        walker.bind(step.id(), results);
    leaveAnythingForCalleeListedAtForGenerators(walker, step);
    return true;
}

static void closeGenerator(Walker& walker, BuilderForGenerators& builder, Reference generator, IteratorCompletion completion)
{
    if (walker.isUnreachable())
        return;

    BlockID after = builder.addBlock();
    bool dropsWhatIsThrown = completion == IteratorCompletion::Throw;
    if (dropsWhatIsThrown)
        builder.beginToDropWhatIsThrown();
    Reference stepped = buildStepOfGenerator(walker, builder, generator, ResumeMode::Return, GeneratorStepCheck::IsUnmodified);
    Reference status = builder.resultAt(stepped, static_cast<uint32_t>(StepGeneratorResult::Status));
    BlockID isNotStepped = builder.addBlock(BlockFrequency::Rare);
    BlockID isStepped = builder.addBlock();
    BlockID isClosed = builder.addBlock();
    builder.branch(builder.isInteger(status, static_cast<int64_t>(GeneratorStepStatus::NotStepped)), isNotStepped, isStepped);
    builder.continueIn(isStepped);
    buildFirstReadThatCloseOfGeneratorLeavesOut(walker, builder.origin());
    builder.jumpTo(isClosed);
    builder.continueIn(isNotStepped);
    builder.closeByProtocol(generator, IteratorCompletion::Normal);
    builder.jumpTo(isClosed);
    if (dropsWhatIsThrown)
        builder.dropWhatWasThrown(after);
    else
        builder.jumpTo(after);
}

static bool closeLoopOverGenerator(Walker& walker, const BodyValue& close)
{
    Reference state = walker.referenceOf(close.operand(0));
    if (!isStateOfLoopOverGenerator(walker, state))
        return false;

    IteratorCompletion completion = close.as<Opcode::IteratorClose>().completion();
    BuilderForGenerators builder(walker, walker.originOf(close));
    BlockID byProtocol = builder.addBlock(BlockFrequency::Unlikely);
    BlockID after = builder.addBlock();
    Reference generator = builder.continueIfTypeMatches(state, typeOfGenerators(walker), byProtocol);
    closeGenerator(walker, builder, generator, completion);
    builder.jumpTo(after);

    builder.continueIn(byProtocol);
    builder.closeByProtocol(builder.iteratorIn(builder.takeWordsOfProtocol(state, listOfWordsMadeForState(walker, state))), completion);
    builder.jumpTo(after);
    leaveAnythingForCalleeListedAtForGenerators(walker, close);
    return true;
}

static bool takePatternOverGenerator(Walker& walker, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    if (!written.isDestructuring() || written.iteratorKind() != IteratorKind::Sync || written.isDelegating() || !walker.isInLoop())
        return false;
    Reference generator = walker.referenceOf(written.iterableOperand());
    TypeID proven = generator && generator.isValue() ? walker.typeOf(generator) : TypeID();
    if (!proven || !walker.types().isSubset(proven, typeOfGenerators(walker)) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    uint32_t numberOfSteps = written.elementCount().value_or(0);
    if (!numberOfSteps || numberOfSteps > mostStepsOfPatternOverGenerator || !engineAnswersForGenerators(walker) || !stepsOfPatternStandInRow(walker.currentBodyIndex(), open))
        return false;

    BuilderForGenerators builder(walker, walker.originOf(open));
    BlockID byRunTime = builder.addBlock(BlockFrequency::Rare);
    BlockID meeting = builder.addBlock();
    Walker::IndexedIteration pattern;
    pattern.hasPrecomputedSteps = true;
    for (uint32_t step = 0; step < numberOfSteps; ++step)
        pattern.precomputedSteps.append(walker.addParameter(meeting, { }, Representation::JSValue));
    auto handOn = [&](std::span<const Reference> taken) {
        if (walker.isUnreachable())
            return;
        Vector<Reference, 4> arguments(taken);
        while (arguments.size() < numberOfSteps)
            arguments.append(builder.undefined());
        walker.jumpTo(meeting, arguments.span());
    };

    Draft test = builder.draft(Opcode::IsUnmodifiedGenerator);
    test.setValueOperand(generator);
    std::optional<PassedTest> isAsMade = builder.continueIfCheckPasses(builder.build(WTF::move(test)), byRunTime);
    if (isAsMade) {
        buildFirstReadsThatOpenOfGeneratorLeavesOut(walker, builder.origin());
        Vector<Reference, 4> taken;
        for (uint32_t step = 0; step < numberOfSteps; ++step) {
            BuilderForGenerators::MeetingOfStep after = builder.openMeetingOfStep();
            stepGeneratorInPlace(walker, builder, generator, isAsMade, after);
            builder.continueIn(after.block);
            BlockID last = builder.addBlock();
            BlockID further = builder.addBlock();
            builder.branch(after.done, last, further);
            builder.continueIn(last);
            handOn(taken.span());
            builder.continueIn(further);
            taken.append(after.value);
        }
        closeGenerator(walker, builder, generator, IteratorCompletion::Normal);
        handOn(taken.span());
    } else if (!walker.isUnreachable())
        builder.jumpTo(byRunTime);

    builder.continueIn(byRunTime);
    Draft steps = builder.draft(Opcode::DestructureIterable);
    steps.setIterableOperand(generator);
    steps.setCount(numberOfSteps);
    steps.setReadsListsByIndex(isArrayIteratorProtocolIntact(walker));
    Reference results = builder.build(WTF::move(steps));
    if (!walker.isUnreachable())
        walker.state().invalidateHeap();
    Vector<Reference, 4> taken;
    for (uint32_t step = 0; step < numberOfSteps; ++step)
        taken.append(builder.resultAt(results, step));
    handOn(taken.span());

    builder.continueIn(meeting);
    walker.recordSpeculation();
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(pattern)));
    return true;
}

bool buildLoopOverGenerator(Walker& walker, const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::IteratorOpen:
        return openLoopOverGenerator(walker, value) || takePatternOverGenerator(walker, value);
    case Opcode::IteratorResume:
        return stepLoopOverGenerator(walker, value);
    case Opcode::IteratorClose:
        return closeLoopOverGenerator(walker, value);
    default:
        return false;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

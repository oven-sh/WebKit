/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Inlining: a callee is walked in the place of its call. It is decided from the callee's thin summary before anything is built, and never taken back.
// A suspension: a callee that suspends is never copied, so a copy holds none. In the body of a coroutine a copy is code of that body like any other.
// A throw: Walker::leadHandlerEdge() goes up the frames, so a block of the copy without a handler of its own stands under the handler of the call. The inline frame names the callee for the stack trace.
// Withheld: the call goes on down the chain.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLimits.h"
#include "AOTLinkTables.h"
#include "AOTThinSummary.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkKnownJoins.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// What a call costs where it stands, in operations as the scan counts them, each operand that it hands over added.
static constexpr uint32_t inlineLimitOutsideLoops = 6;
static constexpr uint32_t inlineLimitInLoops = 40;
// All that copies in copies may add at one call that the unit's own body writes.
static constexpr uint32_t inlineWindowOfWrittenCall = 40;
// By LinkedFunction::weightAsLeaf.
static constexpr uint32_t inlineLimitOfLeaves = 14;
// At a call that a function writes which recursion may enter again.
static constexpr uint32_t inlineLimitInRecursion = 10;
// A helper that calls what it is handed in a loop, and what such helpers and their callbacks may add at one written call.
static constexpr uint32_t inlineLimitOfHelpers = 40;
static constexpr uint32_t inlineWindowOfHelpers = 120;

// Instruction::inlineFrame has no more bits.
static constexpr uint64_t mostInlineFramesAtInlining = defaultLimitValue(LimitID::InlineFramesPerFunction, hostArchitecture());

static constexpr OpcodeSet opcodesOfInlining { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallDirect };
static constexpr OpcodeSet constructionsOfInlining { Opcode::Construct, Opcode::SuperConstruct };
static constexpr OpcodeSet callsOfListsAtInlining { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };

struct CallOfInlining {
    FunctionID callee;
    // Null: a CallDirect. Whoever wrote it listed an operand for every entry parameter, and nothing is made for it.
    Reference closure;
    Reference thisValue;
    Reference newTarget;
    // As written.
    std::span<const Reference> arguments;
};

// Of a Call, a Construct or a SuperConstruct.
static Reference closureCalledAtInlining(const Draft& site)
{
    return site.opcode == Opcode::SuperConstruct ? site.parentOperand() : site.calleeOperand();
}

static bool endsInReturnAtInlining(BodyTerminator end)
{
    return end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall;
}

// No function on the stack of frames is copied, so a body of the unit's function is the unit's own.
// The allowance of a loop is for the calls written there: granted again in every copy, it would let a unit grow by the depth of the calls. In a copy a callee is walked only where it is no larger than its call.
// A call that the body writes stays a call, or something no larger stands in its place.
static bool isNoLargerThanCallWithItsOwnCallsAtInlining(const ThinSummary& summary, size_t numberOfOperands)
{
    size_t size = summary.numberOfOperations;
    for (const SummaryCall& call : summary.calls) {
        if (call.callee != SummaryCall::Callee::Builtin && !call.isMadeByProtocol)
            size += inlineLimitOutsideLoops + call.numberOfArguments;
    }
    return size <= inlineLimitOutsideLoops + numberOfOperands;
}

static bool copyPaysAtInlining(const Walker& walker, FunctionID callee, size_t numberOfOperands, const PlaceOfCall& place)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (summary.numberOfOperations <= inlineLimitOutsideLoops + numberOfOperands
        && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= inlineWindowOfWrittenCall || isNoLargerThanCallWithItsOwnCallsAtInlining(summary, numberOfOperands)))
        return true;
    // A leaf brings nothing along. What it calls are leaves, which the same line lets in.
    if (uint32_t weight = walker.facts().linked().function(callee).weightAsLeaf; weight <= inlineLimitOfLeaves + numberOfOperands
        && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + weight <= inlineWindowOfWrittenCall)
        // A copy that adds code pays where it is repeated: in a loop of the unit, or in a function that may be entered again from below itself (LinkedFunction::isNamedInItsCycle): words on the text, no heat.
        // Elsewhere leaves cost the application 21 G of build for each G that TypeScript 6 ran less, here 5. So the body of a module, too, is the same code in an executable and in a test run.
        && (place.isInLoop || walker.facts().linked().function(walker.unit().function).isNamedInItsCycle)
        // TODO(filled-fields, 11-03 23:18): an object that the unit's own code makes outside of loops and hands to a call stays in sight of the collector from the unit's frame while the callee runs; what a call answers does not.
        // `parameter(make())`, test/aot/parallel/gc/: until that is mended where it is, a callee that answers with an object of its own making is called.
        && (place.isInLoop || !walker.facts().linked().function(callee).resultIsFresh))
        return true;
    // Only the constructor of its class calls an initializer, behind the allocation or behind `super()`, and only the evaluation of the class that of its static members:
    // the copy stands in the place of the body, and the stores go to an object that the walk has just seen made.
    if (walker.program().at(callee).kind == FunctionKind::Initializer)
        return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
    if (place.caller == walker.unit().function && !place.isInLoop && walker.facts().linked().function(place.caller).isNamedInItsCycle)
        return summary.numberOfOperations <= inlineLimitInRecursion && !summary.has(SummaryTrait::HasLoops) && !walker.facts().linked().function(callee).resultIsFresh;
    // `measured(box)` > `checked(box, 2)` > `area(box)`, in a turn that may begin again: a call that stays a call runs other code by Link's word, which speaks for every caller, and with it the whole loop does.
    if (place.caller != walker.unit().function && place.isInLoop && walker.standsInTurnThatLooksCountOn())
        return walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= inlineWindowOfWrittenCall && !summary.has(SummaryTrait::HasLoops);
    if (place.caller != walker.unit().function || !place.isInLoop)
        return false;
    return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
}

// `new Node(kind, pos, end)`, `new (NodeConstructor || (NodeConstructor = allocator.getNodeConstructor()))(kind, -1, -1)`: the walk has made the object in front of the call (walkConstructOfFunctionInWalkCalls()).
// As with an initializer the copy stands in the place of the body, and its stores go to an object that the walk has just seen made: each is one store, no test stands in front of them, and the birth ends in sight.
// Called, the body is handed a `this` of which it knows the layout alone, because a plain call may hand it any object.
// IN A COPY ONLY WITHIN THE WINDOW OF THE CALL THAT THE UNIT's OWN BODY WRITES, as every copy in a copy: constructors that construct in turn, eight times each, were 8 to the n copies.
static bool copyFillsObjectMadeForItAtInlining(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    if (site.opcode != Opcode::CallDirect || !site.isConstruction() || site.isNeverInlined())
        return false;
    const Function& function = walker.program().at(callee);
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfWrittenCall)
        return false;
    return function.takesThisFromConstruction() && function.constructedClass && summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
}

// `each(list, x => ..)`, `list.map(x => ..)`. One body for all callers knows neither the function that its loop calls nor what the list holds. In a copy the callee of that call is one function,
// so it is a direct call or a copy in turn, the list has the type that the caller knows, and what the closure is handed has the type of the elements.
// By entry parameter, up to 64: a call in a loop of the body calls it.
static uint64_t entryParametersCalledInLoopsAtInlining(const ThinSummary& summary)
{
    return summary.entryParametersCalledInLoops;
}

static bool isClosureOfOneFunctionAtInlining(const Walker& walker, Reference value)
{
    TypeID proven = value ? walker.typeOf(value) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    return type.isObject() && type.isJavaScriptValue() && singleFunctionOf(walker.types(), type);
}

// The call is written in a loop of the unit's own body. Copied at every call that hands over a closure, the code of a large application grew by a sixth, and the TypeScript compiler ran 1% fewer instructions.
// `handedOver`: null: whatever the operands are.
static bool copyPaysForClosureHandedOverAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* handedOver)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitOfHelpers)
        return false;
    // Where the call runs once, the loop of the helper is all that a copy could speed up, and a body for all callers has that loop too.
    if (!place.isInLoop && walker.facts().linked().function(walker.unit().function).runsAtMostOnce)
        return false;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    uint64_t called = entryParametersCalledInLoopsAtInlining(summary);
    if (!called || !handedOver)
        return called;
    for (size_t index = 0; index < std::min<size_t>(handedOver->size(), 64); ++index) {
        if ((called >> index & 1) && isClosureOfOneFunctionAtInlining(walker, (*handedOver)[index]))
            return true;
    }
    return false;
}

// The call stands in a loop of such a copy, and calls what the unit's own body has handed to the copy: it has the allowance of a call that is written in a loop of that body.
static bool copyOfClosureHandedOverPaysAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place)
{
    if (!place.isInLoop || !walker.depthOfCopies() || place.caller != walker.currentFunction() || !walker.currentBodyIndex().hasLoops())
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
}

// The Body of a function that enters files begins with a test that fills the words of its group, and a copy has no such test. Where a body of the same group runs those words are filled.
static bool staticsAreFilledAtInlining(const Walker& walker, FunctionID callee, FunctionID caller)
{
    const Plan& plan = walker.plan();
    if (!plan.entersFile(callee))
        return true;
    if (!walksInputBody(walker.unit().role))
        return false;
    const StaticsGroups& groups = plan.staticsGroups();
    std::optional<uint32_t> group = groups.groupFilledWhenEntered(walker.program().at(callee).module);
    return group && group == groups.groupFilledWhenEntered(walker.program().at(caller).module);
}

// All that hangs neither on the operands, nor on the size, nor on the frames. It builds nothing.
static bool mayBeWalkedAtInlining(const Walker& walker, FunctionID callee, FunctionID caller)
{
    const Program& program = walker.program();
    const Function& function = program.at(callee);
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (function.isCoroutine() || function.results.size() != 1)
        return false;
    FlagsOfSummaryForInlining flags {
        .suspends = summary.has(SummaryTrait::Suspends),
        .hasDirectEval = summary.has(SummaryTrait::HasDirectEval),
        .holdsOperationsBoundToFrame = body && body->holdsAnyOf(opcodesThatBindBodyToFrame),
    };
    if (whyNoCallInlines(program, callee, PartOfCallee::WholeBody, &flags, body) != NotInlinedCause::None)
        return false;

    // The inline frame names the Body of the callee for the stack trace.
    if (!walker.plan().conventions().exists({ callee, CodeRole::Body }) || !staticsAreFilledAtInlining(walker, callee, caller))
        return false;
    if (walker.code().inlineFrames().size() >= mostInlineFramesAtInlining)
        return false;

    // The entry goes on in the block of Code that holds the call, with each parameter standing for one operand.
    BodyBlock entry = body->block(entryBlock);
    if (body->numberOfIncomingEdges(entryBlock) || entry.hasHandler() || entry.numberOfParameters() != function.numberOfEntryParameters())
        return false;

    bool returns = false;
    for (uint32_t block = 0; block < body->numberOfBlocks() && !returns; ++block)
        returns = body->isReachable(BlockID(block)) && endsInReturnAtInlining(body->terminator(BlockID(block)));
    // A copy mentions what the callee names. A callee that was found under a name behind a guess, or that is lost to Link, has no referrer (LinkedFunction::isInQueueFromStart): it is called, unless it names nothing that a mention could bring.
    const LinkTables& link = walker.facts().linked();
    return returns && (link.function(callee).namesOnlyWhatEveryFunctionLeadsTo || link.leadsTo(walker.unit().function, callee));
}

// ---- The callee

static bool isEnteredAtInlining(const Function& function, bool isConstruction)
{
    if (!canBeCalledByValueOf(function.role) || function.has(FunctionCapabilities::RecordsCallee))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        // Whoever constructs with a plain function makes `this` and looks at what comes back: that stays a call.
        return !isConstruction;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        // It makes its own object, and what it returns is what was constructed.
        return isConstruction && !function.has(FunctionFlags::HasThis) && function.constructedClass;
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- The operands

static bool takesListAtInlining(const Function& callee, unsigned entryParameter)
{
    EntryParameter kind = callee.entryParameterKind(entryParameter);
    return kind == EntryParameter::HiddenRest || (kind == EntryParameter::Parameter && entryParameter - callee.firstParameterIndex() >= callee.numberOfPositionalParameters());
}

static bool tellsAbsentFromUndefinedAtInlining(Walker& walker, const Function& callee, unsigned entryParameter)
{
    if (callee.entryParameterKind(entryParameter) != EntryParameter::Parameter)
        return false;
    TypeID written = callee.parameters[entryParameter - callee.firstParameterIndex()].written();
    return written && (walker.types().at(written).primitives & Primitive::Absent);
}

// Null: the call has no value for the parameter, and walkInline() makes one.
static Reference operandHandedOverAtInlining(const CallOfInlining& call, const Function& callee, unsigned entryParameter)
{
    switch (callee.entryParameterKind(entryParameter)) {
    case EntryParameter::This:
        return call.thisValue;
    case EntryParameter::Callee:
        return call.closure;
    case EntryParameter::NewTarget:
        return call.newTarget;
    case EntryParameter::Parameter: {
        size_t parameter = entryParameter - callee.firstParameterIndex();
        return parameter < callee.numberOfPositionalParameters() && parameter < call.arguments.size() ? call.arguments[parameter] : Reference();
    }
    case EntryParameter::Capture:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Nothing tests an established position: the operations of the body have no form for a value of another type.
static bool fitsParameterAtInlining(Walker& walker, const Function& callee, unsigned entryParameter, TypeID arriving)
{
    const DeclaredType* declared = callee.declaredEntryParameter(entryParameter);
    if (!declared || !declared->isEstablished())
        return true;
    TypeID established = walker.program().provenTypeOf(*declared);
    return walker.types().isSubset(arriving, established);
}

// `handedOver`: by entry parameter. It speaks of what buildEntryArgumentsAtInlining() makes as well, and builds nothing.
static bool operandsFitAtInlining(Walker& walker, const CallOfInlining& call, const Function& callee, std::span<const Reference> handedOver)
{
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    if (handedOver.size() != numberOfEntryParameters)
        return false;
    bool takesList = callee.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    if (takesList && call.arguments.size() > callee.numberOfPositionalParameters() + maxComputedElementsOfArrayLiteral)
        return false;

    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        if (handedOver[index]) {
            TypeID arriving = walker.typeOf(handedOver[index]);
            if (!arriving || !fitsParameterAtInlining(walker, callee, index, arriving))
                return false;
            continue;
        }

        EntryParameter kind = callee.entryParameterKind(index);
        if (!call.closure || kind == EntryParameter::Callee)
            return false;
        // The compiler lays down both what a closure holds and what the position takes, and a count is a count.
        if (kind == EntryParameter::Capture || kind == EntryParameter::ArgumentCount)
            continue;
        if (takesListAtInlining(callee, index)) {
            // buildListOfOtherArgumentsAtInlining() makes what the entry of the callee makes: a list.
            if (!fitsParameterAtInlining(walker, callee, index, walker.types().builtinClass(BuiltinClasses::Array)))
                return false;
            continue;
        }
        if (!fitsParameterAtInlining(walker, callee, index, tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? TypeTable::absent : TypeTable::undefined))
            return false;
    }
    return true;
}

// False: what the call hands over surely fails the guard of the callee, so no run would come to the copy. It builds nothing.
static bool mayPassGuardAtInlining(Walker& walker, const Function& callee, FunctionID id, std::span<const Reference> handedOver)
{
    const CallingConventions& conventions = walker.plan().conventions();
    if (!conventions.hasParameterGuard(id))
        return true;
    for (unsigned index = 0; index < handedOver.size(); ++index) {
        TypeID tested = conventions.typeBehindGuard(id, index);
        if (!tested)
            continue;
        TypeID arriving = handedOver[index] ? walker.typeOf(handedOver[index]) : tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? TypeID(TypeTable::absent) : TypeID(TypeTable::undefined);
        if (!walker.types().overlap(arriving, tested))
            return false;
    }
    return true;
}

// What a closure has captured never changes, so where its creation is in sight nothing is read off it.
static Reference buildCaptureAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, size_t numberOfCaptures, uint32_t index)
{
    const Code& code = walker.code();
    if (call.closure.isValue()) {
        const Operation& creation = code.at(call.closure.id());
        if (creation.opcode == Opcode::NewClosure && creation.function() == call.callee && creation.numberOfVariadicOperands() == numberOfCaptures) {
            ValueID captured = creation.variadicOperands()[index];
            return code.isConstant(captured) ? Reference::constant(captured) : Reference::value(captured);
        }
    }
    if (Reference scope = findScopeOfClosureReadOffObjectInSight(walker, call.callee, call.closure))
        return scope;
    if (Reference atHand = findCaptureAtHand(walker, call.callee, index, call.closure))
        return atHand;
    Draft capture(Opcode::GetCapture, origin);
    walker.chooseConservativeFields(capture);
    capture.setFunction(call.callee);
    capture.setIndex(index);
    capture.setClosureOperand(call.closure);
    return walker.build(WTF::move(capture));
}

static Reference buildListOfOtherArgumentsAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, const Function& callee)
{
    Draft list(Opcode::NewArray, origin);
    walker.chooseConservativeFields(list);
    list.standsForNoValueOfInput = true;
    list.setContents(TypeTable::anything);
    std::span<const Reference> listed = call.arguments.subspan(std::min<size_t>(callee.numberOfPositionalParameters(), call.arguments.size()));
    list.setVariadicOperands(listed);
    Reference built = walker.build(WTF::move(list));
    // No operation of the input stands behind it, so buildFreshObject() never comes to it.
    noteFreshList(walker, built);
    return built;
}

// operandsFitAtInlining() has passed.
static Vector<Reference, 8> buildEntryArgumentsAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, const Function& callee, std::span<const Reference> handedOver)
{
    Vector<Reference, 8> operands;
    uint32_t numberOfCapturesPassed = 0;
    for (unsigned index = 0; index < handedOver.size(); ++index) {
        EntryParameter kind = callee.entryParameterKind(index);
        uint32_t capture = kind == EntryParameter::Capture ? numberOfCapturesPassed++ : 0;
        if (handedOver[index]) {
            operands.append(handedOver[index]);
            continue;
        }
        switch (kind) {
        case EntryParameter::Capture:
            operands.append(buildCaptureAtInlining(walker, origin, call, callee.captures.size(), capture));
            break;
        case EntryParameter::This:
        case EntryParameter::NewTarget:
            operands.append(walker.constant(Draft(Opcode::Undefined, origin)));
            break;
        case EntryParameter::ArgumentCount: {
            Draft count(Opcode::Integer, origin);
            count.setInteger(static_cast<int64_t>(call.arguments.size()));
            operands.append(walker.constant(WTF::move(count)));
            break;
        }
        case EntryParameter::Parameter:
            if (takesListAtInlining(callee, index))
                operands.append(buildListOfOtherArgumentsAtInlining(walker, origin, call, callee));
            else
                operands.append(walker.constant(Draft(tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? Opcode::Absent : Opcode::Undefined, origin)));
            break;
        case EntryParameter::HiddenRest:
            operands.append(buildListOfOtherArgumentsAtInlining(walker, origin, call, callee));
            break;
        case EntryParameter::Callee:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    return operands;
}

namespace {

// What the walk that stands at a call sees of it.
struct OperandsInSightAtInlining {
    std::span<const Reference> handedOver;
    bool callsParameterOfCopy { false };
    // What is called is a built-in, and the callee the function of the library behind it. The call by its rows has the types of the rows, so a copy takes its place only for the sake of a closure.
    bool calleeStandsBehindBuiltin { false };
};

} // anonymous namespace

// `sum(a, 1, 2)` for `function sum(...values) { for (let i = 0; i < values.length; i++) s += values[i]; .. }`. Called, the list is made, the loop reads boxed values out of it, and nothing is known of any.
// In a copy the walk writes the turns out (Walker::writeOutTurnsOfLoop()): each element is the argument, with its type, and the list is never made. It goes by the call as it is written, so a look at a loop says the same.
std::optional<TurnsOfCopyWrittenOut> turnsOfCopyWrittenOutAt(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    if (site.opcode != Opcode::Call || !walker.has(Mechanism::EscapeAnalysis) || !walker.has(Mechanism::Folding) || !walker.has(Mechanism::LoopInvariants))
        return std::nullopt;
    if (!walker.facts().summaries().at(callee).has(SummaryTrait::HasLoops))
        return std::nullopt;
    const Function& function = walker.program().at(callee);
    Vector<std::optional<int64_t>, 8> integers;
    Vector<std::optional<uint32_t>, 8> lengths;
    integers.fill(std::nullopt, function.numberOfEntryParameters());
    lengths.fill(std::nullopt, function.numberOfEntryParameters());
    size_t numberOfArguments = site.argumentsOperands().size();
    // The arguments as the body of the caller writes them. A look at a loop and the walk read the same text.
    std::span<const ValueID> written;
    if (place.body && place.call && place.body->opcodeOf(place.call) == Opcode::Call)
        written = place.body->value(place.call).as<Opcode::Call>().argumentsOperands();
    if (written.size() != numberOfArguments)
        written = { };
    TurnsOfCopyWrittenOut answer;
    answer.firstArgumentListed = static_cast<uint32_t>(std::min<size_t>(function.numberOfPositionalParameters(), numberOfArguments));
    unsigned numberOfLists = 0;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (function.entryParameterKind(index) == EntryParameter::ArgumentCount)
            integers[index] = static_cast<int64_t>(numberOfArguments);
        if (takesListAtInlining(function, index)) {
            ++numberOfLists;
            answer.entryParameterOfList = index;
            lengths[index] = static_cast<uint32_t>(numberOfArguments) - answer.firstArgumentListed;
            continue;
        }
        if (function.entryParameterKind(index) != EntryParameter::Parameter || index - function.firstParameterIndex() >= written.size())
            continue;
        ValueID argument = place.body->valueBehindCasts(written[index - function.firstParameterIndex()]);
        if (argument && !place.body->isParameter(argument) && place.body->opcodeOf(argument) == Opcode::Integer)
            integers[index] = place.body->value(argument).integer();
    }
    if (numberOfLists > 1)
        return std::nullopt;
    // A callee whose turns are known whoever calls it writes them out in its own body, and a copy brings nothing.
    Vector<std::optional<int64_t>, 8> noIntegers;
    Vector<std::optional<uint32_t>, 8> noLengths;
    noIntegers.fill(std::nullopt, function.numberOfEntryParameters());
    noLengths.fill(std::nullopt, function.numberOfEntryParameters());
    if (turnsOfLoopsOfCopyWrittenOut(walker, callee, noIntegers.span(), noLengths.span()))
        return std::nullopt;
    std::optional<TextOfCopyRead> text = turnsOfLoopsOfCopyWrittenOut(walker, callee, integers.span(), lengths.span());
    if (!text)
        return std::nullopt;
    answer.text = WTF::move(*text);
    return answer;
}

// What the call says as it is written, and whether a copy pays. `inSight`: null: it looks at no type, and says yes wherever some operands would make it say yes.
static bool callAdmitsCopyAtInlining(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place, const OperandsInSightAtInlining* inSight)
{
    // However it is written, a call answers a promise or a generator: `get(k)`, `get.call(o, k)`, a getter that a descriptor lays. Whoever builds the ramp at the call copies the body, and asks with a CallDirect of the CoroutineBody: AOTWalkCoroutineCalls.cpp.
    if (walker.program().at(callee).isCoroutine() && (site.opcode != Opcode::CallDirect || site.role() != CodeRole::CoroutineBody))
        return false;
    auto copyPays = [&](size_t numberOfOperands) {
        return ((!inSight || !inSight->calleeStandsBehindBuiltin) && copyPaysAtInlining(walker, callee, numberOfOperands, place))
            || copyPaysForClosureHandedOverAtInlining(walker, callee, place, inSight ? &inSight->handedOver : nullptr)
            || ((!inSight || inSight->callsParameterOfCopy) && copyOfClosureHandedOverPaysAtInlining(walker, callee, place))
            // At a call that the unit's own body writes: in a copy each turn would walk such a call again, and its copy the calls of its turns.
            || ((!inSight || !walker.hasTextsInStringBuilders()) && place.caller == walker.unit().function && turnsOfCopyWrittenOutAt(walker, site, callee, place));
    };
    if (copyFillsObjectMadeForItAtInlining(walker, site, callee, place))
        return true;
    if (site.opcode == Opcode::CallDirect)
        return !site.isNeverInlined() && copyPays(site.numberOfVariadicOperands());

    bool isConstruction = constructionsOfInlining.contains(site.opcode);
    // Whether another new.target can construct is for the closed form to ask. That of `super(..)` has constructed to get there.
    if (site.opcode == Opcode::Construct && site.newTargetOperand() && site.newTargetOperand() != site.calleeOperand())
        return false;
    if (site.opcode == Opcode::Call && site.isNeverInlined())
        return false;
    return isEnteredAtInlining(walker.program().at(callee), isConstruction) && copyPays(site.argumentsOperands().size());
}

// The questions that inlineCallee() and walkInline() ask before they look at an operand or at the state.
bool mayCopyCallee(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    return walker.has(Mechanism::Inlining) && opcodesOfInlining.contains(site.opcode) && callAdmitsCopyAtInlining(walker, site, callee, place, nullptr)
        && !walker.isBeingWalked(callee) && mayBeWalkedAtInlining(walker, callee, place.caller);
}

bool shouldCopyCallee(const Walker& walker, const Draft& site, FunctionID callee, std::span<const Reference> handedOver, bool callsParameterOfCopy)
{
    OperandsInSightAtInlining inSight { handedOver, callsParameterOfCopy, false };
    PlaceOfCall place { walker.currentFunction(), walker.isInLoop() };
    return walker.has(Mechanism::Inlining) && opcodesOfInlining.contains(site.opcode) && callAdmitsCopyAtInlining(walker, site, callee, place, &inSight)
        && !walker.isBeingWalked(callee) && mayBeWalkedAtInlining(walker, callee, place.caller);
}

Vector<Reference, 8> operandsHandedToCopy(const Draft& site, const Function& callee)
{
    if (site.opcode == Opcode::CallDirect)
        return Vector<Reference, 8>(site.variadicOperands());

    CallOfInlining request;
    request.closure = closureCalledAtInlining(site);
    request.arguments = site.argumentsOperands();
    if (site.opcode == Opcode::Construct)
        request.newTarget = request.closure;
    else if (site.opcode == Opcode::SuperConstruct)
        request.newTarget = site.newTargetOperand();
    else
        request.thisValue = site.thisValueOperand();

    Vector<Reference, 8> handedOver;
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    for (unsigned index = 0; index < numberOfEntryParameters; ++index)
        handedOver.append(operandHandedOverAtInlining(request, callee, index));
    return handedOver;
}

// Who is entered, and whether a copy pays. It builds nothing: what the frames and the operands say, and all that is made, is walkInline()'s.
static_assert(opcodesThatInlineCalleeTakes == (opcodesOfInlining | callsOfListsAtInlining));

// `super(...args)` in a copy of the constructor that the language gives a class, `inner(...args)` in a copy of `function outer(...args)`, `f(...[a, b])`:
// the state knows what the list holds, so the call is what it would be with the elements written as its arguments.
static bool inlineCalleeOfListWrittenOut(Walker& walker, const BodyValue& call)
{
    Reference closure = walker.referenceOf(call.opcode() == Opcode::SuperConstructList ? call.parentOperand() : call.calleeOperand());
    FunctionID callee = closure ? findOnlyCallee(walker, call, closure) : FunctionID();
    if (!callee)
        return false;
    std::optional<Draft> site = draftOfCallWithListWrittenOut(walker, call);
    if (!site)
        return false;
    Vector<Reference, 8> handedOver = operandsHandedToCopy(*site, walker.program().at(callee));
    OperandsInSightAtInlining inSight { handedOver.span(), false, false };
    if (!callAdmitsCopyAtInlining(walker, *site, callee, { walker.currentFunction(), walker.isInLoop() }, &inSight))
        return false;
    return walker.walkInline(call, callee, handedOver.span(), &*site);
}

bool inlineCalleeOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (callsOfListsAtInlining.contains(call.opcode()))
        return inlineCalleeOfListWrittenOut(walker, call);
    if (!opcodesOfInlining.contains(call.opcode()))
        return false;
    // Who is called is read off the type of the callee. The question costs less than the Draft of the site.
    if (call.opcode() == Opcode::Call && walker.nothingIsProvenOf(walker.referenceOf(call.calleeOperand())))
        return false;

    Reference closure = call.opcode() == Opcode::CallDirect ? Reference() : walker.referenceOf(call.opcode() == Opcode::SuperConstruct ? call.parentOperand() : call.calleeOperand());
    FunctionID callee = call.opcode() == Opcode::CallDirect ? call.function() : findOnlyCallee(walker, call, closure);
    // All that is called is one built-in, behind which stands a function of the library.
    bool calleeStandsBehindBuiltin = !callee && call.opcode() == Opcode::Call && !call.isDoneInPlace() && closure;
    if (calleeStandsBehindBuiltin)
        callee = findLibraryFunctionBehindBuiltin(walker, walker.typeOf(closure));
    if (!callee)
        return false;

    Draft site = walker.draftOf(call);
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<Reference, 8> handedOver = operandsHandedToCopy(site, walker.program().at(callee));
    bool callsParameterOfCopy = call.opcode() == Opcode::Call && walker.depthOfCopies() && index.block(entryBlock).indexOfParameter(index.valueBehindCasts(call.as<Opcode::Call>().calleeOperand()));
    OperandsInSightAtInlining inSight { handedOver.span(), callsParameterOfCopy, calleeStandsBehindBuiltin };
    if (!callAdmitsCopyAtInlining(walker, site, callee, { walker.currentFunction(), walker.isInLoop(), &index, call.id() }, &inSight))
        return false;
    return walker.walkInline(call, callee, handedOver.span());
}

// ---- The copy

// `arguments`: one for each entry parameter of the callee. Null, of a Call or a Construct: the call hands nothing over, and what the callee then finds is made here.
// False: the callee is not walked here, and nothing was built.
// A closure that the block of the call makes and that the call alone is handed: one cell for each run of the call, which nobody else can have seen.
struct ClosureMadeForCallAtInlining {
    Reference made;
    ValueID written;
};

static Vector<ClosureMadeForCallAtInlining, 2> collectClosuresMadeForCallAtInlining(Walker& walker, const BodyValue& call)
{
    Vector<ClosureMadeForCallAtInlining, 2> closures;
    const BodyIndex& index = walker.currentBodyIndex();
    call.forEachOperand([&](ValueID operand, uint32_t) {
        ValueID written = operand;
        while (written && !index.isParameter(written) && index.hasOneUse(written) && inputOperationsThatOnlyHandOn.contains(index.opcodeOf(written)))
            written = index.value(written).operand(0);
        if (!written || index.isParameter(written) || !index.hasOneUse(written) || index.opcodeOf(written) != Opcode::NewClosure || index.blockOf(written) != index.blockOf(call.id()))
            return;
        Reference made = walker.referenceOf(operand);
        if (made && made.isValue() && walker.code().at(made.id()).opcode == Opcode::NewClosure)
            closures.append({ made, written });
    });
    return closures;
}

bool Walker::walkInline(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft* writtenOut)
{
    // The typed run has the copy. The closed arm of an opened guess calls.
    if (!m_opensGuesses || isBeingWalked(callee))
        return false;

    Draft site = writtenOut ? *writtenOut : draftOf(call);
    Vector<Reference, 2> heldByCallAlone = collectWhatCopyAloneHolds(*this, call);
    Vector<ClosureMadeForCallAtInlining, 2> closuresMadeForCall = collectClosuresMadeForCallAtInlining(*this, call);
    const Function& function = program().at(callee);
    bool isConstruction = constructionsOfInlining.contains(site.opcode);
    CallOfInlining written;
    written.callee = callee;
    if (isConstruction || site.opcode == Opcode::Call) {
        written.closure = closureCalledAtInlining(site);
        written.arguments = site.argumentsOperands();
    }
    if (!mayBeWalkedAtInlining(*this, callee, currentFunction()) || !operandsFitAtInlining(*this, written, function, arguments))
        return false;
    if (!depthOfCopies())
        m_operationsCopiedAtWrittenCall = 0;
    m_operationsCopiedAtWrittenCall += facts().summaries().at(callee).numberOfOperations;
    if (!mayPassGuardAtInlining(*this, function, callee, arguments))
        return false;

    Origin originOfCall = site.origin();
    originOfCall.isTailCall = false;
    checkProofOfOnlyCallee(*this, written.closure, callee, originOfCall);
    Vector<Reference, 8> entryArguments = buildEntryArgumentsAtInlining(*this, originOfCall, written, function, arguments);
    if (isUnreachable())
        return true;

    // The copy is one of the fast version. What the caller has not proven the tests of the guard prove in front of it, once, and where one fails the slow version is called.
    Vector<Reference, 8> argumentsAsTheyCame(entryArguments);
    BlockID whereGuardFails;
    const CallingConventions& conventions = plan().conventions();
    for (uint32_t index = 0; index < entryArguments.size() && !isUnreachable(); ++index) {
        TypeID tested = conventions.typeBehindGuard(callee, index);
        if (!tested || m_types.isSubset(ownTypeOf(entryArguments[index]), tested))
            continue;
        Draft test(Opcode::TypeTest, originOfCall);
        test.setValueOperand(entryArguments[index]);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));
        if (!passes)
            return true;
        BlockID whereTestPasses = addBlock();
        if (!whereGuardFails) {
            whereGuardFails = addBlock(BlockFrequency::Rare);
            noteGuessOpened();
        }
        branch(passes, whereTestPasses, whereGuardFails);
        continueIn(whereTestPasses);

        Draft narrowed(Opcode::Narrowed, originOfCall);
        narrowed.setValueOperand(entryArguments[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        entryArguments[index] = build(WTF::move(narrowed));
    }
    // What Link has proven of a parameter holds of whatever a call hands over, and the Body of the callee is typed by it. The walk's own type of the operand may say less: it has come out of a closed read.
    // Code has no way to state a type. So the type is tested for, and the failing side traps: it is no guess and has no closed arm, and only a false claim of Link takes it.
    const LinkTables& link = facts().linked();
    const LinkedFunction& linked = link.function(callee);
    // The copy goes by what the caller knows of its operands, and leans on less of Link's word than the Body of the callee does. The word is held against what arrives here all the same: it is the same word.
    for (uint32_t index = 0; index < entryArguments.size() && checksProofs() && linked.everyCallerIsKnown && !isUnreachable(); ++index) {
        if (TypeID proven = index < linked.numberOfParameters ? link.parameters[linked.firstParameter + index].proven : TypeID())
            checkProofOfCopy(entryArguments[index], proven, ProofSource::EntryParameter, originOfCall);
    }
    for (uint32_t index = 0; index < entryArguments.size() && has(Mechanism::Arrivals) && linked.everyCallerIsKnown && !isUnreachable(); ++index) {
        TypeID proven = index < linked.numberOfParameters ? link.parameters[linked.firstParameter + index].proven : TypeID();
        // Primitives, one layout, or both: one look at the value, and no question here goes over a set. A family or a set of closures grows with the program, and so would each question and each test, at every copy.
        // The closures of ONE function are one look too, and the copy may not do without it: `function hop(operation, value) { return operation(value); }`. Link lists that call with its one callee, so a ring of calls in tail position
        // that goes through it may return raw values (LinkedFunction::ringsOfTailCallsAreListed). A copy that called the value instead would come back to unbox, and each turn of the ring would leave a frame.
        bool isOneLook = proven && (!m_types.at(proven).hasObjects() || singleFunctionOf(m_types, m_types.at(proven)) || (m_types.at(proven).hasOnlyLayouts() && holdsOneMember(m_types.at(proven).layouts)));
        if (proven && !isOneLook)
            continue;
        TypeID tested = proven && entryArguments[index].isValue() ? m_types.testablePartOf(proven) : TypeID();
        TypeID arriving = tested ? ownTypeOf(entryArguments[index]) : TypeID();
        if (!arriving || !m_types.at(arriving).isJavaScriptValue() || m_types.at(tested).isAnything() || m_types.isSubset(arriving, tested) || !m_types.overlap(arriving, tested))
            continue;
        Draft test(Opcode::TypeTest, originOfCall);
        test.setValueOperand(entryArguments[index]);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));
        if (!passes)
            return true;
        BlockID whereClaimHolds = addBlock();
        BlockID whereClaimFails = addBlock(BlockFrequency::Rare);
        branch(passes, whereClaimHolds, whereClaimFails);
        continueIn(whereClaimFails);
        m_code.end(whereClaimFails, TerminatorDraft(TerminatorKind::Unreachable, originOfCall), 0);
        continueIn(whereClaimHolds);

        Draft narrowed(Opcode::Narrowed, originOfCall);
        narrowed.setValueOperand(entryArguments[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        entryArguments[index] = build(WTF::move(narrowed));
    }
    if (m_hasFailed)
        return true;
    bool copyIsReached = !isUnreachable();
    bool returnsInPlaceOfUnit = unitReturnsAtOnce(site);

    const BodyIndex& body = facts().bodies().at(callee);
    const BodyIndex& bodyOfCaller = currentBodyIndex();

    Frame frame;
    frame.function = callee;
    frame.body = &body.body();
    frame.index = &body;
    frame.references.fill(Reference(), body.numberOfValues());
    frame.loopDepthAtCall = m_frames.last().loopDepthAtCall + bodyOfCaller.loopDepthOf(bodyOfCaller.blockOf(call.id()));
    // The casts of a ReturnCall build nothing.
    frame.returnsInPlaceOfUnit = returnsInPlaceOfUnit;
    frame.callsAreCountedByPlan = callsOfCopyAreCountedForStackTests(call, callee);
    frame.inlineFrame = m_code.addInlineFrame({
        .callee = callee,
        .callPosition = site.position,
        .parent = m_frames.last().inlineFrame,
        .isTailCall = site.isTailCall,
        .isConstruction = isConstruction,
        .callIsPartOfConstruction = site.isPartOfConstruction,
        .module = function.module,
        // No stack trace lists such a callee. A site in a frame that names a built-in is recorded at the call, under the name of the built-in.
        .calledBuiltin = !isConstruction && function.isHiddenInStackTraces() ? site.calledBuiltin() : BuiltinID(),
    });
    m_frames.append(WTF::move(frame));

    struct ReturnOfCopy {
        BlockID block;
        Reference value;
        WalkState state;
        bool isUnderHandlerOfCallee;
    };
    Vector<ReturnOfCopy, 2> returns;

    BodyBlock entry = body.block(entryBlock);
    for (uint32_t index = 0; index < entryArguments.size() && copyIsReached; ++index)
        bind(entry.parameter(index), entryArguments[index]);
    for (uint32_t number = 0; number < body.numberOfBlocks() && !m_hasFailed && copyIsReached; ++number) {
        BodyBlock block = body.block(BlockID(number));
        walkBlock(block.id());
        // walkTerminator() leaves the walk nowhere, but at a return of a callee: there the block stays open.
        if (isUnreachable() || !endsInReturnAtInlining(block.terminator()))
            continue;
        ValueID returned = block.terminator().operand(0);
        Reference value = returned ? referenceOf(returned) : constant(Draft(Opcode::Undefined, originOfCall));
        if (!value) {
            fail("a return hands on a value of the body that nothing was built for"_s);
            break;
        }
        returns.append(ReturnOfCopy { m_block, value, WTF::move(m_state), block.hasHandler() });
        m_block = { };
        m_state.clear();
    }

    endBlocksInFrontOfLoops(m_frames.size() - 1, { });
    m_frames.removeLast();
    m_origin = originOfCall;
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;

    if (whereGuardFails && !m_hasFailed) {
        m_block = { };
        m_state.clear();
        continueIn(whereGuardFails);
        // The frame of the copy is gone: the operation is read in the body of the caller again.
        for (Reference& argument : argumentsAsTheyCame) {
            for (const ClosureMadeForCallAtInlining& closure : closuresMadeForCall) {
                if (argument != closure.made || isUnreachable())
                    continue;
                Draft again = draftOf(currentBodyIndex().value(closure.written));
                chooseConservativeFields(again);
                if (Reference madeHere = build(WTF::move(again)))
                    argument = madeHere;
            }
        }
        Reference answer = callSlowVersion(*this, site, callee, argumentsAsTheyCame.span(), isConstruction || (call.opcode() == Opcode::CallDirect && site.isConstruction()));
        if (answer && returnsInPlaceOfUnit)
            returnAtOnce(answer);
        else if (answer)
            returns.append(ReturnOfCopy { m_block, answer, WTF::move(m_state), false });
        m_block = { };
        m_state.clear();
    }

    // What the arguments decide may leave no way through the callee that returns, and a callee that returns in place of the unit has ended each. Nothing behind the call is reached.
    if (m_hasFailed || returns.isEmpty()) {
        m_block = { };
        m_state.clear();
        return true;
    }

    // The caller goes on where the one return stands, with all that holds there: an object that the callee has made is still fresh.
    if (returns.size() == 1 && !returns[0].isUnderHandlerOfCallee) {
        m_block = returns[0].block;
        m_state = WTF::move(returns[0].state);
        if (checksProofs() && !function.isCoroutine() && !isConstruction)
            checkProofOfResultOfCopy(returns[0].value, linked.result, originOfCall);
        letGoWhatCopyHeld(*this, heldByCallAlone.span(), originOfCall);
        bind(call.id(), returns[0].value);
        return true;
    }

    // jumpTo() gives the parameter the join of what arrives.
    TypeID type;
    Vector<Representation, 2> forms;
    for (const ReturnOfCopy& returned : returns) {
        TypeID arriving = m_code.at(returned.value.id()).type;
        type = type ? joinTypes(type, arriving) : arriving;
        forms.append(representationOf(returned.value));
    }
    BlockID behindCall = addBlock();
    Reference result = addParameter(behindCall, type, formOfParameter(type, forms.span()));
    WalkState stateBehindCall;
    for (size_t index = 0; index < returns.size() && !m_hasFailed; ++index) {
        m_block = returns[index].block;
        m_state = WTF::move(returns[index].state);
        jumpTo(behindCall, singleElementSpan(returns[index].value));
        if (index)
            stateBehindCall.meetWith(m_state);
        else
            stateBehindCall = WTF::move(m_state);
    }
    m_state = WTF::move(stateBehindCall);
    if (checksProofs() && !function.isCoroutine() && !isConstruction)
        checkProofOfResultOfCopy(result, linked.result, originOfCall);
    letGoWhatCopyHeld(*this, heldByCallAlone.span(), originOfCall);
    bind(call.id(), result);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

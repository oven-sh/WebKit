/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileUnit.h"

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTDiagnostics.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsNet.h"
#include "AOTWalkKnown.h"
#include "AOTMachineArena.h"
#include "AOTNumbering.h"
#include "AOTPhaseTimes.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTScratchRealm.h"
#include "AOTStatistics.h"
#include "AOTTypeArena.h"
#include "AOTTypeIDs.h"
#include "AOTTypeText.h"
#include "AOTBodyIndex.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFacts.h"
#include "AOTOpcodeSets.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

WalkOptions walkOptionsOf(const BuildContext& build)
{
#if ASSERT_ENABLED || ASAN_ENABLED
    constexpr bool isCheckingBuild = true;
#else
    constexpr bool isCheckingBuild = false;
#endif
    const AOT::CompileOptions& options = build.options();
    return {
        .takesNotes = AOT::hasFlag(options.reports, Wire::ReportSet::Flow) || AOT::hasFlag(options.reports, Wire::ReportSet::AccessCounts) || AOT::hasFlag(options.reports, Wire::ReportSet::Allocations),
        .verifiesCode = isCheckingBuild || build.validates(),
        .hidesNames = options.obfuscates(),
        .checksProofs = !!sourcesOfProofsCheckedUnder(options.testing),
        .sourcesOfProofsChecked = sourcesOfProofsCheckedUnder(options.testing),
        .testsEveryPosition = AOT::hasFlag(options.testing, Wire::TestingSwitch::TestsEveryPosition),
        .readsStorageWhereItIsUsed = readsStorageWhereItIsUsedUnder(options.testing),
        .takesPositionsToLieInside = AOT::hasFlag(options.testing, Wire::TestingSwitch::PositionsAreTakenToLieInside),
        .takesCalleesToKeepNothing = AOT::hasFlag(options.testing, Wire::TestingSwitch::NoCalleeKeepsWhatItIsHanded),
        .writesBodiesStraight = AOT::hasFlag(options.testing, Wire::TestingSwitch::WritesBodiesStraight),
        .checksEffects = AOT::hasFlag(options.testing, Wire::TestingSwitch::CheckEffectsAtRunTime),
        .checksWalk = AOT::hasFlag(options.testing, Wire::TestingSwitch::CheckWalkAtRunTime),
        .walksLastWalksOfLoopsAlone = options.walksLastWalksOfLoopsAlone,
        .walksLoopsThatFitAgain = options.walksLoopsThatFitAgain,
        .undoLeavesExpectations = AOT::hasFlag(options.testing, Wire::TestingSwitch::UndoLeavesExpectations),
        .laterUnitsOpenNoGuesses = AOT::hasFlag(options.testing, Wire::TestingSwitch::LaterUnitsOpenNoGuesses),
        .rowThatSaysLess = options.rowThatSaysLess,
        .ignoresBranchHints = AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreBranchHints),
        .shufflesBlockFrequencies = AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleBlockFrequencies),
        .ignoresExpectedTypes = AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreExpectedTypes),
        .shufflesExpectedTypes = AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleExpectedTypes),
        .failsGuesses = AOT::hasFlag(options.testing, Wire::TestingSwitch::FailGuesses),
        .shufflesChoices = AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleChoices),
        .judgesInt32Promises = !AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreExpectedTypes) && !AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleExpectedTypes) && !AOT::hasFlag(options.testing, Wire::TestingSwitch::GiveLinkMore),
        .seedOfShuffles = options.seedOfShuffles,
    };
}

static std::span<const uint32_t> arenaIDsInUnitMentionKey(NumberSpace space, std::span<const uint32_t> key)
{
    switch (space) {
    case NumberSpace::Name:
    case NumberSpace::KeyCell:
    case NumberSpace::PropertyRun:
    case NumberSpace::Type:
        return key;
    case NumberSpace::GlobalBinding:
        return key.first(1);
    case NumberSpace::SiteCache:
        return key.size() < 2 ? std::span<const uint32_t>() : key.subspan(key.size() - 2, 1);
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::RequireEdge:
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
    case NumberSpace::CardOfWord:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassGuard:
    case NumberSpace::ClassRank:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::AtomOfComparedText:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::MethodSelector:
    case NumberSpace::RegExp:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool unitMentionsThreadArenaID(const CodeBody& body)
{
    for (const Mention& mention : body.mentions) {
        std::span<const uint32_t> key = AOT::keyOf(mention, body.mentionWords.span());
        for (uint32_t word : arenaIDsInUnitMentionKey(mention.space, key)) {
            if (liesInThread(TypeID::fromBits(word)))
                return true;
        }
    }
    return false;
}

static void reportFailureOfCompileUnit(BuildContext& build, CodeUnit unit, InternalErrorKind kind, const String& detail)
{
    build.diagnostics().add(InternalError { .kind = kind, .function = unit.function, .detail = detail });
}

static std::optional<uint64_t> instructionsOfCompileThreadSoFar(const BuildContext& build)
{
    if (!build.options().logging.logsPhaseTimes) [[likely]]
        return std::nullopt;
    return InstructionCounts::ofCurrentThread();
}

static bool isAllocationOfUnitForEffectsAgainstLink(const Code& code, ValueID value, Vector<ValueID, 8>& parametersAsked)
{
    if (!value)
        return false;
    value = rootOfCellInSweptCode(code, value);
    const Operation& made = code.at(value);
    if (made.opcode != Opcode::Parameter)
        return resultRuleOf(made.opcode) == ResultRule::Allocation || (!made.numberOfOperands() && declaredBoundInTable(made.opcode).writes.contains(AbstractHeap::AllocatorState));
    const Block& block = code.at(made.block());
    if (block.isEntry || block.isHandlerBlock() || !block.firstEdgeIn())
        return false;
    if (parametersAsked.contains(value))
        return true;
    parametersAsked.append(value);
    size_t place = std::ranges::find(block.parameters(), value) - block.parameters().begin();
    for (EdgeID edge = block.firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
        std::span<const ValueID> arguments = code.at(edge).arguments();
        if (place >= arguments.size() || !isAllocationOfUnitForEffectsAgainstLink(code, arguments[place], parametersAsked))
            return false;
    }
    return true;
}

static String findPartThatLinkHasNotCounted(const Code& code, const Operation& operation, const Effects& counted)
{
    EffectTraits traits = operation.traits();
    if (traits.mayThrowByRule() && !counted.mayThrowByRule() && effectRuleOf(operation.opcode) != EffectRule::Operator && !code.at(operation.block()).handler)
        return "may throw"_s;
    if (traits.mayCollect() && !counted.mayCollect)
        return "may collect"_s;
    if (!traits.mayWriteWhatIsSeen())
        return { };
    Vector<ValueID, 8> parametersAsked;
    bool fillsWhatUnitMakes = operation.origin().isPartOfConstruction || !operation.numberOfOperands() || familyOf(operation.opcode) == OperationFamily::Hint || isAllocationOfUnitForEffectsAgainstLink(code, operation.operand(0), parametersAsked);
    const Effects& declared = declaredBoundInTable(operation.opcode);
    if (declared.mayCallUnknownCode) {
        EffectTraits traitsCounted(counted);
        if (traits.mayWriteOutsideObjects() && !traitsCounted.mayWriteOutsideObjects())
            return "may write outside of objects"_s;
        return traitsCounted.mayWriteWhatIsSeen() || fillsWhatUnitMakes ? String() : String("may write what is seen"_s);
    }
    for (AbstractHeap heap : allMembersOf<AbstractHeap>) {
        if (!observableHeaps.contains(heap) || !declared.writes.contains(heap) || counted.writes.contains(heap))
            continue;
        if (!fillsWhatUnitMakes || !perObjectHeaps.contains(heap))
            return makeString("writes "_s, nameOf(heap));
    }
    return { };
}

struct ClosuresThatCalledParametersHold {
    bool run(const Operation& operation) const { return frames.get(operation.origin().inlineFrame) || (operation.opcode == Opcode::CallDirect && functions.contains(operation.function())); }

    Vector<FunctionID, 4> functions;
    BitVector frames;
};

static ClosuresThatCalledParametersHold findClosuresThatCalledParametersHold(const WalkInputs& inputs, TypeArena& types, const Code& code, uint32_t parametersCalled)
{
    ClosuresThatCalledParametersHold held;
    auto note = [&](TypeID type) {
        if (FunctionID function = type ? singleFunctionOf(types, types.at(type)) : FunctionID())
            held.functions.append(function);
    };
    std::span<const LinkedParameter> parameters = inputs.facts.linked().parametersOf(code.unit().function);
    for (uint32_t parameter = 0; parameter < parameters.size() && parameter < 32; ++parameter) {
        if (parametersCalled >> parameter & 1)
            note(parameters[parameter].proven);
    }
    auto isCalledParameter = [&](ValueID value) {
        const Operation& operation = code.at(value);
        if (operation.opcode != Opcode::Parameter || !operation.block() || !code.at(operation.block()).isEntry)
            return false;
        size_t place = std::ranges::find(code.at(operation.block()).parameters(), value) - code.at(operation.block()).parameters().begin();
        return place < 32 && (parametersCalled >> place & 1);
    };
    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        const Operation& operation = code.at(ValueID(index));
        if (isCalledParameter(ValueID(index)))
            note(operation.type);
        else if (operation.opcode == Opcode::TypeTest && operation.block() && isCalledParameter(rootOfCellInSweptCode(code, operation.operand(0))))
            note(operation.testedType());
    }

    std::span<const InlineFrame> copied = code.inlineFrames();
    for (uint32_t frame = 1; frame < copied.size(); ++frame) {
        if ((!copied[frame].parent && held.functions.contains(copied[frame].callee)) || held.frames.get(copied[frame].parent))
            held.frames.set(frame);
    }
    return held;
}

static String findOperationThatLinkHasNotCounted(const WalkInputs& inputs, TypeArena& types, const Code& code)
{
    CodeUnit unit = code.unit();
    if ((unit.role != CodeRole::Body && !code.isEntryThatWalksBody()) || unitDoesMoreThanBodyForKnownEffects(inputs.plan, unit.function))
        return { };
    const LinkTables& tables = inputs.facts.linked();
    const LinkedFunction& linked = tables.function(unit.function);
    Effects counted = linked.effectsOnReturn();
    counted.merge(linked.effectsOnThrow());
    const LinkedEffectsGivenArguments* given = code.isEntryThatWalksBody() ? nullptr : tables.known.findEffectsGivenArguments(unit.function);
    Effects countedGivenArguments = given ? given->onReturn : Effects::none();
    if (given)
        countedGivenArguments.merge(given->onThrow);
    if (inputs.options.rowThatSaysLess) [[unlikely]] {
        RowThatSaysLess row = RowThatSaysLess::decode(inputs.options.rowThatSaysLess);
        row.cut(KindOfEffectsRow::Summary, unit.function.index(), given ? countedGivenArguments : counted);
    }
    if (inputs.options.ignoresBranchHints || inputs.options.shufflesBlockFrequencies) [[unlikely]]
        given = nullptr;
    ClosuresThatCalledParametersHold leftToCaller;
    if (given && given->parametersCalled)
        leftToCaller = findClosuresThatCalledParametersHold(inputs, types, code, given->parametersCalled);
    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        const Operation& operation = code.at(ValueID(index));
        if (!operation.block() || operation.traits().mayCallUnknownCode() || (code.isEntryThatWalksBody() && operation.opcode == Opcode::CreateRest))
            continue;
        String part = findPartThatLinkHasNotCounted(code, operation, counted);
        ASCIILiteral summary = "Link's effects of the function"_s;
        if (part.isNull() && given && code.at(operation.block()).frequency == BlockFrequency::Normal && !leftToCaller.run(operation)) {
            part = findPartThatLinkHasNotCounted(code, operation, countedGivenArguments);
            summary = "Link's effects of the function given its arguments"_s;
        }
        if (part.isNull())
            continue;
        return makeString('f', unit.function.index(), " v"_s, index, " ("_s, nameOf(operation.opcode), ") "_s, part, ", and "_s, summary, " say that it does not"_s);
    }
    return { };
}

namespace {

class JudgeOfCalls {
public:
    JudgeOfCalls(const WalkInputs& inputs, TypeArena& types, const Code& code, std::span<const Walker::ArrivalHeld> held)
        : m_inputs(inputs)
        , m_types(types)
        , m_code(code)
        , m_held(held)
    {
    }

    String run()
    {
        for (uint32_t index = 0; index < m_code.numberOfValues(); ++index) {
            const Operation& call = m_code.at(ValueID(index));
            if (call.opcode != Opcode::CallDirect || !call.block() || call.numberOfBoundArguments())
                continue;
            bool entersUnitThatBelievesCallers = call.role() == CodeRole::Body || call.role() == CodeRole::ParameterGuard || call.role() == CodeRole::CoroutineBody;
            if (!entersUnitThatBelievesCallers || !m_inputs.arrivals.takesWhatWasLeft(call.function()))
                continue;
            if (String refusal = judge(ValueID(index), call); !refusal.isNull())
                return refusal;
        }
        return { };
    }

private:
    TypeID leftByThisUnitAt(FunctionID callee, uint32_t place, bool isPositional) const
    {
        TypeID left;
        for (const Walker::ArrivalHeld& held : m_held) {
            if (held.callee != callee)
                continue;
            TypeID here;
            switch (held.kind) {
            case Walker::ArrivalHeld::Kind::Anything:
                return TypeTable::anything;
            case Walker::ArrivalHeld::Kind::Proven:
                if (held.entryParameter == place)
                    here = held.proven;
                break;
            case Walker::ArrivalHeld::Kind::UndefinedFrom:
                if (held.entryParameter <= place)
                    here = isPositional ? TypeID(TypeTable::undefined) : TypeID(TypeTable::anything);
                break;
            }
            if (here)
                left = left ? m_types.unionOf(left, here) : here;
        }
        return left;
    }

    TypeID typeThatChecksInFrontOfCallAskOf(ValueID value, ValueID call) const
    {
        BlockID block = m_code.at(value).block();
        TypeID asked = m_code.at(value).type;
        auto askChecksBackFrom = [&](ValueID last) {
            for (ValueID id = last; id && id != value; id = m_code.at(id).previous) {
                const Operation& check = m_code.at(id);
                if (check.opcode != Opcode::CheckProof)
                    continue;
                ValueID checked = check.valueOperand();
                while (checked != value && !m_code.isConstant(checked) && conversionsOfForm.contains(m_code.at(checked).opcode))
                    checked = m_code.at(checked).operand(0);
                if (checked == value)
                    asked = m_types.intersectionOf(asked, check.checkedType());
            }
        };
        askChecksBackFrom(m_code.at(call).previous);
        if (m_code.at(call).block() != block)
            askChecksBackFrom(m_code.lastOperationOf(block));
        return asked;
    }

    bool liesWithin(ValueID value, TypeID bound, ValueID call)
    {
        const Operation& record = m_code.at(value);
        if (!record.type)
            return false;
        if (m_types.isSubset(record.type, bound))
            return true;
        if (m_code.isConstant(value) || !m_types.at(record.type).isJavaScriptValue())
            return false;
        if (m_types.isSubset(typeThatChecksInFrontOfCallAskOf(value, call), bound))
            return true;
        if (conversionsOfForm.contains(record.opcode))
            return liesWithin(record.operand(0), bound, call);
        if (record.opcode != Opcode::Parameter || m_code.at(record.block()).isEntry || m_code.at(record.block()).isHandlerBlock())
            return false;
        if (!m_parametersBeingAsked.add(value.index()).isNewEntry)
            return true;
        std::span<const ValueID> parameters = m_code.at(record.block()).parameters();
        size_t place = std::ranges::find(parameters, value) - parameters.begin();
        bool everyArrivalLiesWithin = true;
        bool hasArrival = false;
        m_code.forEachEdgeInto(record.block(), [&](const Edge& edge) {
            hasArrival = true;
            everyArrivalLiesWithin = everyArrivalLiesWithin && place < edge.arguments().size() && edge.arguments()[place] && liesWithin(edge.arguments()[place], bound, call);
        });
        return hasArrival && everyArrivalLiesWithin;
    }

    String judge(ValueID id, const Operation& call)
    {
        FunctionID callee = call.function();
        const Function& function = m_inputs.program.at(callee);
        bool isUnitOfCallee = callee == m_code.function();
        std::span<const ValueID> handedOver = call.variadicOperands();
        uint32_t firstPositional = function.firstParameterIndex();
        uint32_t endOfPositional = firstPositional + function.numberOfPositionalParameters();
        for (uint32_t place = 0; place < function.numberOfEntryParameters(); ++place) {
            bool isPositional = place >= firstPositional && place < endOfPositional;
            TypeID left = isUnitOfCallee ? m_inputs.arrivals.leftAt(m_types, callee, place) : leftByThisUnitAt(callee, place, isPositional);
            if (left == TypeTable::anything || (isUnitOfCallee && !left))
                continue;
            ValueID operand = place < handedOver.size() ? handedOver[place] : ValueID();
            if (!left)
                return sentence(id, callee, place, operand, "and its unit has left nothing there"_s, { });
            if (operand && m_code.at(operand).type == left)
                continue;
            if (isUnitOfCallee && operand && m_code.at(operand).opcode == Opcode::Parameter) {
                const Block& entry = m_code.at(m_code.at(operand).block());
                if (entry.isEntry && place < entry.parameters().size() && entry.parameters()[place] == operand)
                    continue;
            }
            TypeID believed = m_inputs.facts.typeBelievedOfEntryParameter(m_types, callee, place, left);
            if (!m_types.at(believed).isJavaScriptValue())
                continue;
            if (TypeID provenByLink = isUnitOfCallee ? TypeID() : m_inputs.facts.typeOfEntryParameter(callee, place).proven; provenByLink && m_types.isSubset(m_inputs.facts.typeBelievedOfEntryParameter(m_types, callee, place, provenByLink), believed))
                continue;
            if (TypeID tested = call.role() == CodeRole::Body ? m_inputs.plan.conventions().typeBehindGuard(callee, place) : TypeID()) {
                TypeID passed = m_types.intersectionOf(believed, tested);
                believed = isNeverType(passed) ? tested : passed;
            }
            if (isUnitOfCallee)
                believed = m_types.testablePartOf(believed);
            if (!operand) {
                if (place >= handedOver.size() && isPositional && m_types.isSubset(TypeTable::undefined, believed))
                    continue;
                return sentence(id, callee, place, operand, "by what was left there the callee believes"_s, believed);
            }
            m_parametersBeingAsked.clear();
            if (!liesWithin(operand, believed, id))
                return sentence(id, callee, place, operand, "by what was left there the callee believes"_s, believed);
        }
        return { };
    }

    String sentence(ValueID call, FunctionID callee, uint32_t place, ValueID operand, ASCIILiteral what, TypeID believed) const
    {
        StringPrintStream out;
        out.print("f"_s, m_code.function().index(), " "_s, nameOf(m_code.role()), " v"_s, call.index(), " (CallDirect) hands "_s);
        if (operand)
            out.print(TypeInText { m_types, m_code.at(operand).type });
        else
            out.print("nothing"_s);
        out.print(" to entry parameter "_s, place, " of f"_s, callee.index(), ", which goes by what its callers leave, "_s, what);
        if (believed)
            out.print(" "_s, TypeInText { m_types, believed });
        return out.toString();
    }

    const WalkInputs& m_inputs;
    TypeArena& m_types;
    const Code& m_code;
    std::span<const Walker::ArrivalHeld> m_held;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_parametersBeingAsked;
};

} // anonymous namespace

String findCallThatHandsOverWhatItsUnitHasNotLeft(const WalkInputs& inputs, TypeArena& types, const Code& code, std::span<const Walker::ArrivalHeld> arrivalsHeld)
{
    return JudgeOfCalls(inputs, types, code, arrivalsHeld).run();
}

static String findStoreOfWhatCallerMayLend(const WalkInputs& inputs, const Code& code)
{
    CodeUnit unit = code.unit();
    if (!walksInputBody(unit.role) || !(code.opcodesOfValues().contains(Opcode::PutModuleVariable) || code.opcodesOfValues().contains(Opcode::PutEnvironmentField)))
        return { };
    if (inputs.options.takesCalleesToKeepNothing)
        return { };
    const LinkTables& tables = inputs.facts.linked();
    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        const Operation& operation = code.at(ValueID(index));
        if (!operation.block() || (operation.opcode != Opcode::PutModuleVariable && operation.opcode != Opcode::PutEnvironmentField) || code.isConstant(operation.valueOperand()))
            continue;
        ValueID stored = rootOfCellInSweptCode(code, operation.valueOperand());
        const Operation& made = code.at(stored);
        if (made.opcode != Opcode::Parameter || !code.at(made.block()).isEntry)
            continue;
        std::span<const ValueID> parameters = code.at(made.block()).parameters();
        size_t entryParameter = std::ranges::find(parameters, stored) - parameters.begin();
        if (tables.onlyCallsWhatArrivesAt(unit.function, static_cast<unsigned>(entryParameter)))
            return makeString('f', unit.function.index(), " v"_s, index, " ("_s, nameOf(operation.opcode), ") keeps entry parameter "_s, entryParameter, ", of which Link says that it is only called"_s);
    }
    return { };
}

static bool verifyCodeOfCompileUnit(const WalkInputs& inputs, BuildContext& build, TypeArena& types, const PositionTypes& positions, const Code& code, std::span<const Walker::ArrivalHeld> arrivalsHeld)
{
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::OpcodeNotInForm };
    String brokenRule = verifyCode(CodeTextContext { inputs.program, types, &inputs.plan, &inputs.facts }, code, &kindOfBrokenRule);
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::TypeOutOfDate;
        brokenRule = verifyTypesOfCode(TypingContext { inputs.program, types, positions }, inputs.plan.conventions(), inputs.arrivals, code);
    }
    if (brokenRule.isNull())
        brokenRule = findCallThatHandsOverWhatItsUnitHasNotLeft(inputs, types, code, arrivalsHeld);
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::EffectsNotCounted;
        brokenRule = findOperationThatLinkHasNotCounted(inputs, types, code);
    }
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::OpcodeNotInForm;
        brokenRule = findStoreOfWhatCallerMayLend(inputs, code);
    }
    if (brokenRule.isNull())
        return true;
    reportFailureOfCompileUnit(build, code.unit(), kindOfBrokenRule, brokenRule);
    return false;
}

static bool verifyFrameCellsOfCompileUnit(const WalkInputs& inputs, BuildContext& build, TypeArena& types, const Code& code)
{
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::OpcodeNotInForm };
    String brokenRule = verifyFrameCellsOfSweptCode(CodeTextContext { inputs.program, types, &inputs.plan, &inputs.facts }, code, &kindOfBrokenRule);
    if (brokenRule.isNull())
        return true;
    reportFailureOfCompileUnit(build, code.unit(), kindOfBrokenRule, brokenRule);
    return false;
}

static bool notesAreTakenOfCompileUnit(CodeRole role)
{
    switch (role) {
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
    case CodeRole::ParameterGuard:
        return true;
    case CodeRole::SlowBody:
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::ModuleStart:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WalkInputs withRealmOfCompileWorker(const WalkInputs& inputs, ScratchRealm* realm)
{
    WalkInputs ofWorker = inputs;
    ofWorker.scratchRealm = realm;
    return ofWorker;
}

CompileWorker::CompileWorker(const CompileInputs& inputs, BuildContext& build, TypeTable& types)
    : m_inputs(inputs)
    , m_build(build)
    , m_types(types, ArenaKeeps::UntilReset)
    , m_arena(makeUniqueWithoutFastMallocCheck<Machine::Arena>())
    , m_scratchRealm(makeUniqueWithoutFastMallocCheck<ScratchRealm>())
    , m_walkInputs(withRealmOfCompileWorker(inputs.walk, m_scratchRealm.get()))
    , m_keptBetweenUnits(makeUnique<KeptBetweenUnits>(m_walkInputs, m_types))
    , m_walker(makeWalker(RoomOfFormerWalker::IsLeft))
{
}

CompileWorker::~CompileWorker() = default;

WalkerInRoom CompileWorker::makeWalker(RoomOfFormerWalker room)
{
    WalkerInRoom walker = m_keptBetweenUnits->makeWalker(m_walkInputs, m_types, m_inputs.walk.arrivals);
    if (room == RoomOfFormerWalker::IsTaken)
        walker->takeRoomOf(*m_walker);
    return walker;
}

bool CompileWorker::walkAndSweep(CodeUnit unit)
{
    const WalkInputs& inputs = m_inputs.walk;
    CurrentWorkScope work(inputs.program, unit.function, "walk"_s);

    m_types.reset();
    if (m_walker->hasBegunUnit())
        m_walker = makeWalker(RoomOfFormerWalker::IsTaken);
    bool isWalked = m_walker->walk(unit);
    if (isWalked && m_walker->hasLastWalksOfLoopsToWalkAlone()) {
        WalkerInRoom first = std::exchange(m_walker, makeWalker(RoomOfFormerWalker::IsLeft));
        isWalked = m_walker->walkLastWalksOfLoopsAlone(*first);
    }
    Code& code = m_walker->code();
    if (!isWalked) {
        m_build.diagnostics().append(m_walker->takeReports());
        reportFailureOfCompileUnit(m_build, unit, m_walker->kindOfFailure(), m_walker->failure());
        return false;
    }
    RELEASE_ASSERT(code.unit() == unit);

    if (inputs.options.verifiesCode) {
        work.setStep("verify"_s);
        if (!verifyCodeOfCompileUnit(inputs, m_build, m_types, m_walker->positions(), code, m_walker->arrivalsHeld())) {
            m_build.diagnostics().append(m_walker->takeReports());
            return false;
        }
    }

    work.setStep("sweep"_s);
    SweepTotals swept = Sweep(m_roomsOfSweep).run(SweepInputs { inputs.program, inputs.facts, inputs.plan, m_types }, code);

    if (inputs.options.verifiesCode && code.holdsFrameCells()) {
        work.setStep("verify"_s);
        if (!verifyFrameCellsOfCompileUnit(inputs, m_build, m_types, code)) {
            m_build.diagnostics().append(m_walker->takeReports());
            return false;
        }
    }

    m_walker->handOverArrivals();
    m_build.diagnostics().append(m_walker->takeReports());
    m_build.statistics().count(StatisticID::OperationsSwept, swept.operationsSwept);
    m_build.statistics().count(StatisticID::ValuesWalked, m_walker->numberOfValuesWalked());
    m_build.statistics().count(StatisticID::ValuesWalkedAndKept, m_walker->numberOfValuesWalkedAndKept());
    m_build.statistics().count(StatisticID::WalksOfLoops, m_walker->numberOfWalksOfLoops());
    m_build.statistics().count(StatisticID::WalksOfLoopsUndone, m_walker->numberOfWalksOfLoopsUndone());
    m_build.statistics().count(StatisticID::PurchasesOfLoopsThatFailed, m_walker->numberOfPurchasesOfLoopsThatFailed());
    m_build.statistics().count(StatisticID::OperationsBuilt, code.numberOfValues());
    m_build.statistics().count(StatisticID::CallsInlined, code.inlineFrames().size() - 1);
    if (uint32_t guesses = m_walker->numberOfGuessesOpened()) {
        m_build.statistics().count(StatisticID::TestsThatRejoin, guesses);
        m_build.statistics().count(StatisticID::LoopsBuiltTwice, m_walker->numberOfLoopsBuiltTwice());
        m_build.statistics().count(StatisticID::TurnsThatBeginAgain, m_walker->numberOfTurnsThatBeginAgain());
        m_build.statistics().count(StatisticID::EmptySlotsTakenAsCases, m_walker->numberOfEmptySlotsTakenAsCases());
    }
    if (uint32_t assignments = m_walker->numberOfAssignmentsWrittenOut())
        m_build.statistics().count(StatisticID::AssignmentsWrittenOutFieldByField, assignments);
    if (uint32_t joins = m_walker->numberOfJoinsWalkedForEachWay())
        m_build.statistics().count(StatisticID::JoinsWalkedOnceForEachWay, joins);
    if (uint32_t objects = m_walker->numberOfObjectsMetAsOne())
        m_build.statistics().count(StatisticID::ObjectsOfSeveralWaysMetAsOne, objects);
    static_assert(static_cast<unsigned>(StatisticID::ProofsThatNoTestTells) - static_cast<unsigned>(StatisticID::ProofsCheckedOfEntryParameters) == numberOfEnumerators<ProofSource>);
    for (unsigned source = 0; source < numberOfEnumerators<ProofSource>; ++source) {
        if (uint32_t checked = m_walker->numberOfProofsChecked(static_cast<ProofSource>(source)))
            m_build.statistics().count(static_cast<StatisticID>(static_cast<unsigned>(StatisticID::ProofsCheckedOfEntryParameters) + source), checked);
    }
    if (inputs.options.checksProofs) {
        m_build.statistics().count(StatisticID::ProofsThatNoTestTells, m_walker->numberOfProofsThatNoTestTells());
        m_build.statistics().count(StatisticID::FreshCellsLeftAloneAtReturns, m_walker->numberOfFreshCellsLeftAloneAtReturns());
    }
    if (uint32_t answered = m_walker->numberOfChecksThatWalkAnswers())
        m_build.statistics().count(StatisticID::ChecksThatWalkAnswers, answered);
    if (uint32_t told = m_walker->numberOfProofsThatFormTells())
        m_build.statistics().count(StatisticID::ProofsThatFormTells, told);
    if (uint32_t doors = m_walker->numberOfDoorsThatCanNeverPass())
        m_build.statistics().count(StatisticID::DoorsThatCanNeverPass, doors);

    if (inputs.options.takesNotes && notesAreTakenOfCompileUnit(unit.role)) {
        WalkNotes notes = notesOfSweptCode(m_types, code, AOT::hasFlag(m_build.options().reports, Wire::ReportSet::AccessCounts));
        if (unit.role != CodeRole::ParameterGuard)
            m_build.statistics().count(StatisticID::ValuesInDouble, notes.numberOfValuesInDouble);
        if (!notes.notes.isEmpty())
            m_notes.append(WTF::move(notes));
    }
    return true;
}

static bool storesOnlyWhatFitsByItsMaking(const WalkInputs& inputs, TypeArena& types, const BodyIndex& index)
{
    static constexpr OpcodeSet operationsThatStoreIntoPlaces { Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::NewObject, Opcode::NewClosure, Opcode::TryReExportFields };
    if (!index.opcodes().intersects(operationsThatStoreIntoPlaces))
        return true;
    const LinkTables& linked = inputs.facts.linked();
    auto heldIn = [&](LayoutID layout, uint32_t field) -> TypeID {
        const LinkedCell* cell = linked.findCellOfField({ layout, field });
        return cell && cell->everyWriterIsKnown ? cell->proven : TypeID();
    };
    auto fits = [&](ValueID value, TypeID held) {
        if (!value || !held)
            return true;
        Type type = types.at(held);
        if (!type.isJavaScriptValue() || type.isAnything())
            return true;
        ValueID made = index.valueBehindCasts(value);
        while (made && !index.isParameter(made) && (index.value(made).opcode() == Opcode::FinishClass || index.value(made).opcode() == Opcode::NewClass)) {
            BodyValue handsOn = index.value(made);
            made = index.valueBehindCasts(handsOn.opcode() == Opcode::FinishClass ? handsOn.as<Opcode::FinishClass>().classObjectOperand() : handsOn.as<Opcode::NewClass>().constructorOperand());
        }
        if (!made || index.isParameter(made))
            return false;
        BodyValue definition = index.value(made);
        switch (definition.opcode()) {
        case Opcode::String:
            return !!(type.primitives & Primitive::String);
        case Opcode::BigInt:
            return !!(type.primitives & Primitive::BigInt);
        case Opcode::Double:
            return (type.primitives & Primitive::Number) == Primitive::Number && type.integers == Range::full() && type.boundsOfFractions.isEmpty();
        case Opcode::Undefined:
            return !!(type.primitives & Primitive::Undefined);
        case Opcode::Null:
            return !!(type.primitives & Primitive::Null);
        case Opcode::Absent:
            return true;
        case Opcode::Boolean:
            return !!(type.primitives & (definition.boolean() ? Primitive::True : Primitive::False));
        case Opcode::Integer:
            return type.primitives & Primitive::Integer && Range::one(definition.integer()).isSubsetOf(type.integers);
        case Opcode::NewClosure:
            return types.isSubset(types.forFunction(definition.as<Opcode::NewClosure>().function()), held);
        default:
            return false;
        }
    };
    bool allFit = true;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            if (!allFit || !operationsThatStoreIntoPlaces.contains(value.opcode()))
                continue;
            if (value.opcode() == Opcode::PutModuleVariable) {
                VariableID variable = value.variable();
                bool isTyped = variable.index() < linked.variables.size() && linked.variable(variable).everyWriterIsKnown && !inputs.plan.runTimeMayFill(variable);
                allFit = fits(value.valueOperand(), isTyped ? linked.variable(variable).proven : TypeID());
                continue;
            }
            if (value.opcode() == Opcode::PutEnvironmentField) {
                allFit = fits(value.valueOperand(), heldIn(value.layout(), value.field()));
                continue;
            }
            if (value.opcode() == Opcode::TryReExportFields) {
                auto reExport = value.as<Opcode::TryReExportFields>();
                uint32_t place = reExport.keyIsFirstCapture() ? 1 : 0;
                bool isTyped = reExport.function().index() + 1 < linked.firstCaptureOfFunction.size() && place < linked.firstCaptureOfFunction[reExport.function().index() + 1] - linked.firstCaptureOfFunction[reExport.function().index()];
                if (isTyped && linked.capture(reExport.function(), place).everyWriterIsKnown)
                    allFit = fits(reExport.sourceOperand(), linked.capture(reExport.function(), place).proven);
                continue;
            }
            if (value.opcode() == Opcode::NewClosure) {
                FunctionID function = value.as<Opcode::NewClosure>().function();
                uint32_t numberTyped = function.index() + 1 < linked.firstCaptureOfFunction.size() ? linked.firstCaptureOfFunction[function.index() + 1] - linked.firstCaptureOfFunction[function.index()] : 0;
                uint32_t place = 0;
                for (ValueID captured : value.as<Opcode::NewClosure>().capturesOperands()) {
                    if (place < numberTyped && linked.capture(function, place).everyWriterIsKnown)
                        allFit = allFit && fits(captured, linked.capture(function, place).proven);
                    ++place;
                }
                continue;
            }
            auto allocation = value.as<Opcode::NewObject>();
            if (!allocation.layout() || inputs.program.at(allocation.layout()).kind != LayoutKind::Environment)
                continue;
            std::span<const ValueID> values = value.variadicOperands();
            size_t place = 0;
            for (uint32_t field : allocation.fields()) {
                ValueID listed = place < values.size() ? index.valueBehindCasts(values[place]) : ValueID();
                if (listed && (index.isParameter(listed) || index.value(listed).opcode() != Opcode::Undefined))
                    allFit = allFit && fits(listed, heldIn(allocation.layout(), field));
                ++place;
            }
        }
    });
    return allFit;
}

static bool entryBlockStoresVariablesInFrontOfTheirFirstReaders(const WalkInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    std::span<const LinkedVariableStoredFirst> claims = inputs.facts.linked().variablesStoredFirstIn(unit.function);
    if (claims.empty())
        return true;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> stored;
    Vector<FunctionID, 4> used;
    bool isShown = true;
    bool isEntryBlock = true;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsFoundShown;
    auto look = [&](VariableID storedByOperation) {
        for (FunctionID function : used) {
            if (!isShown || functionsFoundShown.contains(function.index()))
                continue;
            bool isPassedBy = false;
            for (const LinkedVariableStoredFirst& claim : std::ranges::equal_range(claims, function, { }, &LinkedVariableStoredFirst::functionWhoseUseCounts)) {
                isPassedBy |= claim.variable == storedByOperation;
                isShown &= claim.variable == storedByOperation || stored.contains(claim.variable.index()) || inputs.plan.deferredDefinitions().findDeferred(claim.variable);
            }
            if (isShown && !isPassedBy)
                functionsFoundShown.add(function.index());
        }
    };
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            listFunctionsUsedByOperation(inputs.program, index, value, used);
            look(value.opcode() == Opcode::PutModuleVariable ? value.variable() : VariableID());
            if (!isEntryBlock || value.opcode() != Opcode::PutModuleVariable)
                continue;
            ValueID made = index.valueBehindCasts(value.valueOperand());
            if (made && (index.isParameter(made) || index.value(made).opcode() != Opcode::Absent))
                stored.add(value.variable().index());
        }
        listFunctionsUsedBy(inputs.program, index, block.terminator(), true, true, VariableID(), used);
        look(VariableID());
        isEntryBlock = false;
    });
    return isShown;
}

static bool readsVariableOfAnotherFileStoredFirst(const WalkInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    if (!index.opcodes().contains(Opcode::GetModuleVariable))
        return false;
    ModuleID own = inputs.program.at(unit.function).module;
    bool reads = false;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            if (value.opcode() == Opcode::GetModuleVariable && inputs.program.at(value.variable()).module != own)
                reads |= inputs.facts.linked().variables[value.variable().index()].isFilled;
        }
    });
    return reads;
}

static bool readsScopesWhoseLivesMayHaveEnded(const WalkInputs& inputs, const BodyIndex& index)
{
    static constexpr OpcodeSet accessesToScopes { Opcode::GetEnvironmentField, Opcode::PutEnvironmentField };
    return (inputs.options.sourcesOfProofsChecked & bitOfSourceOfProofs(ProofSource::Lifetime)) && index.opcodes().intersects(accessesToScopes);
}

static bool isWrittenStraight(const WalkInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    static constexpr OpcodeSet operationsThatTypesMakeNoCheaper = inputOperationsThatHandOn | OpcodeSet {
        Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt, Opcode::Builtin, Opcode::NativeBinding,
        Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::TypeTest, Opcode::ThrowModuleFailure, Opcode::EvaluateModule, Opcode::RecordRequire, Opcode::RequireModule,
        Opcode::NewClosure, Opcode::NewRegExp, Opcode::NewClass, Opcode::NewClassToken, Opcode::ResolveHeritage, Opcode::DefineMethod, Opcode::FinishClass,
    };
    if ((unit.role != CodeRole::SlowBody && unit.role != CodeRole::Body) || (inputs.facts.isTakenToRunOnce(unit.function) && !isBodyWrittenInAnotherFile(inputs.program, unit.function)))
        return false;
    if (inputs.plan.promotedEnvironments().isMaker(unit.function) || inputs.plan.promotedEnvironments().numberOfRemovedCapturesOf(unit.function))
        return false;
    if (readsScopesWhoseLivesMayHaveEnded(inputs, index))
        return false;
    if (inputs.options.writesBodiesStraight)
        return true;
    if (unit.role != CodeRole::Body)
        return !index.opcodes().contains(Opcode::Builtin);
    if (!inputs.program.at(unit.function).isModuleBody())
        return false;
    return !index.hasLoops() && (index.opcodes() | operationsThatTypesMakeNoCheaper) == operationsThatTypesMakeNoCheaper;
}

std::optional<CompiledCode> CompileWorker::compileStraight(CodeUnit unit)
{
    const BodyIndex* index = m_inputs.walk.facts.bodies().find(unit.function);
    if (!index || !isWrittenStraight(m_inputs.walk, unit, *index) || !hasStraightForm(m_inputs.generation, unit, *index))
        return std::nullopt;
    m_types.reset();
    if (!entryBlockStoresVariablesInFrontOfTheirFirstReaders(m_inputs.walk, unit, *index) || readsVariableOfAnotherFileStoredFirst(m_inputs.walk, unit, *index))
        return std::nullopt;
    if (!storesOnlyWhatFitsByItsMaking(m_inputs.walk, m_types, *index) || !m_inputs.walk.facts.summaries().at(unit.function).scopesStoredFirst.isEmpty())
        return std::nullopt;
    Code code;
    code.reset(unit);
    std::optional<CompiledCode> compiled = generateStraightCode(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, code, *index, m_totals);
    if (!compiled || !compiled->unit) {
        m_arena->reset();
        return std::nullopt;
    }
    if (const LinkTables& linked = m_inputs.walk.facts.linked(); !linked.calls.isEmpty()) {
        static const OpcodeSet opcodesThatMayRunOtherCode = opcodesWhere([](Opcode opcode) {
            return declaredBoundInTable(opcode).mayRunOtherCode() || operandsMayRunCode(opcode);
        });
        if (index->opcodes().intersects(opcodesThatMayRunOtherCode)) {
            index->body().forEachBlock([&](BodyBlock block) {
                for (BodyValue value : block.values()) {
                    if (!opcodesThatMayRunOtherCode.contains(value.opcode()))
                        continue;
                    if (FunctionID callee = linked.calleeAt(unit.function, value.id()))
                        m_inputs.walk.arrivals.leaveAnything(callee);
                }
            });
        }
    }
    m_build.statistics().count(StatisticID::UnitsCompiled);
    m_build.statistics().count(StatisticID::UnitsWrittenStraight);
    return compiled;
}

std::optional<CompiledCode> CompileWorker::compileEntryByHand(CodeUnit unit)
{
    const WalkInputs& inputs = m_inputs.walk;
    if (unit.role != CodeRole::GenericCallEntry || inputs.options.checksProofs)
        return std::nullopt;

    m_types.reset();
    Code code;
    code.reset(unit);
    bool believesCallers = withoutMechanismsMissingDependencies(inputs.facts.mechanisms()).contains(Mechanism::Arrivals);
    std::optional<CompiledCode> compiled = generateEntryWrittenByHand(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, code, scopedLambda<TypeID(unsigned)>([&](unsigned entryParameter) -> TypeID {
        TypeID known = believesCallers ? inputs.arrivals.provenAt(m_types, unit.function, entryParameter) : TypeID();
        TypeID believed = inputs.facts.typeBelievedOfEntryParameter(m_types, unit.function, entryParameter, known);
        if (!m_types.at(believed).isJavaScriptValue())
            return { };
        TypeID asked = m_types.testablePartOf(believed);
        return m_types.isSubset(TypeTable::anything, asked) ? TypeID() : asked;
    }), m_totals);
    if (!compiled || !compiled->unit) {
        m_arena->reset();
        return std::nullopt;
    }
    m_build.statistics().count(StatisticID::UnitsCompiled);
    m_build.statistics().count(StatisticID::EntriesWrittenByHand);
    return compiled;
}

std::optional<CompiledCode> CompileWorker::compile(CodeUnit unit)
{
    CurrentWorkScope work(m_inputs.walk.program, unit.function, "walk"_s);

    m_arena->reset();
    if (std::optional<CompiledCode> straight = compileStraight(unit))
        return straight;
    if (std::optional<CompiledCode> entry = compileEntryByHand(unit))
        return entry;
    std::optional<uint64_t> instructionsAtStart = instructionsOfCompileThreadSoFar(m_build);
    if (!walkAndSweep(unit))
        return std::nullopt;
    std::optional<uint64_t> instructionsAfterSweep = instructionsOfCompileThreadSoFar(m_build);

    RELEASE_ASSERT(m_walker->arrivalsHeld().empty());
    work.setStep("emit"_s);
    const Code& swept = m_walker->code();
    CompiledCode code = generateCode(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, swept, m_totals);
    std::optional<uint64_t> instructionsAfterEncoding = instructionsOfCompileThreadSoFar(m_build);
    if (instructionsAtStart && instructionsAfterSweep && instructionsAfterEncoding) {
        m_instructionsOfSteps.walkAndSweep += *instructionsAfterSweep - *instructionsAtStart;
        m_instructionsOfSteps.emitToEncode += *instructionsAfterEncoding - *instructionsAfterSweep;
    }
    if (!code.unit) {
        RELEASE_ASSERT(m_build.diagnostics().hasErrors());
        return std::nullopt;
    }
    RELEASE_ASSERT(code.unit == unit);
    if (unitMentionsThreadArenaID(code.body)) {
        reportFailureOfCompileUnit(m_build, unit, InternalErrorKind::Unknown, "the code mentions an id from the thread's arena, which the code outlives"_s);
        return std::nullopt;
    }

    Statistics& statistics = m_build.statistics();
    statistics.count(StatisticID::UnitsCompiled);
    if (uint32_t slices = m_walker->numberOfSlicesMadeOfElementsInSight())
        statistics.count(StatisticID::SlicesMadeOfElementsInSight, slices);
    for (const Walker::CountOfUnit& count : m_walker->countsOfUnit())
        statistics.count(count.statistic, count.amount);
    if (uint32_t blocks = swept.numberOfBlocksPlacedOtherwise()) [[unlikely]]
        statistics.count(StatisticID::BlocksPlacedOtherwise, blocks);
    return code;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

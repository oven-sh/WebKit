/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Of a text that lies in a builder, where code of the program runs in the middle of a growth.
// TypedOperations: `s += obj`: every part that may run code is converted where the join stands, in the order of the parts, and only then the first part is appended: whatever the conversions run or throw finds the text as it was.
// Inlining: `s = join(s, x)`: the callee hands its parameter back with more behind it, so its copy appends. Where the walk makes the call after all, the call takes a string, and the builder is filled anew with what the call gives.
// At a suspension: no text lies in a builder across one. At a throw in a callee: no text that grows in callees lies in a builder under a handler. Withheld: the call is a reader.

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTDeclaredType.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

static constexpr uint32_t frontOperandInTextBuilderCallees = 0;
static_assert(OperandIndex::Add::left == frontOperandInTextBuilderCallees && OperandIndex::Strcat::parts == frontOperandInTextBuilderCallees);

static constexpr OpcodeSet callsInTextBuilderCallees { Opcode::Call, Opcode::CallDirect };

// ---- A part that may run code

bool convertsPartsThatMayRunCode()
{
    constexpr bool convertsWhereJoinStands = true;
    return convertsWhereJoinStands;
}

// Add converts an object with another hint than Strcat does. A symbol throws in either.
Reference primitivePartOfStringBuilder(Walker& walker, Opcode join, Reference part, Origin origin)
{
    if (hasTextByItself(walker.types().at(walker.typeOf(part))))
        return part;

    origin.isTailCall = false;
    Draft conversion(join == Opcode::Add ? Opcode::Add : Opcode::ToString, origin);
    walker.chooseConservativeFields(conversion);
    if (join == Opcode::Add) {
        Draft emptyText(Opcode::String, origin);
        emptyText.setString(walker.program().strings.emptyString());
        conversion.setLeftOperand(walker.constant(WTF::move(emptyText)));
        conversion.setRightOperand(part);
    } else
        conversion.setValueOperand(part);
    conversion.setForm(formAllowedForTypedOperator(walker, conversion));
    return walker.build(WTF::move(conversion));
}

static void appendPartsInTextBuilderCallees(Walker& walker, const BodyValue& join, Reference name, std::span<const Reference> parts, Origin origin)
{
    TextInStringBuilder& text = *findTextNamedBy(walker, join.id());
    if (text.web.standsUnderHandlers)
        appendPartsRolledBackWhereTheyFail(walker, text, name, parts, origin);
    else
        appendPartsToStringBuilder(walker, text, name, parts, origin);
}

// `line += c` with c out of a list that is written number[], `text += `${row.id},${row.title}\n``: what is written of each part that is not proven to have text by itself is tested, every test in front of every append.
// Where all pass no part runs code, and each is appended in its narrowest form. Where one fails the parts are converted as the join converts them. A test runs no code, so nothing changes its place.
static bool appendPartsBehindTestsOfWhatIsWrittenInTextBuilderCallees(Walker& walker, const BodyValue& join, std::span<const Reference> parts, Origin origin)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    // The parts are the operands of the join one by one: takePartsInWalkTexts() has refused the join by itself.
    Vector<ValueID, 8> written;
    join.forEachOperand([&](ValueID operand, uint32_t) {
        written.append(operand);
    });
    if (written.size() != parts.size())
        return false;

    struct GuessAtPart {
        size_t place;
        TypeID expected;
    };
    Vector<GuessAtPart, 4> guesses;
    constexpr uint32_t weightOfPartAppendedInLine = 1;
    for (size_t place = 1; place < parts.size(); ++place) {
        if (hasTextByItself(walker.types().at(walker.typeOf(parts[place]))))
            continue;
        if (!parts[place].isValue())
            return false;
        ExpectationOfValue expectation = findExpectation(walker, written[place]);
        if (!expectation.type || isNeverType(expectation.type) || !hasTextByItself(walker.types().at(expectation.type)))
            return false;
        // In a copy the part may be known to be something else than is written of it.
        if (!walker.types().overlap(walker.typeOf(parts[place]), expectation.type))
            return false;
        if (!shouldOpenGuess(walker, parts[place], expectation.type, expectation.isDeclared, weightOfPartAppendedInLine))
            return false;
        guesses.append({ place, expectation.type });
    }
    if (guesses.isEmpty())
        return false;

    // Every test is built before a block is made: a test that is decided opens no guess, and nothing is left half built.
    Vector<Reference, 4> tests;
    for (const GuessAtPart& guess : guesses) {
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(parts[guess.place]);
        test.setTestedType(guess.expected);
        Reference passes = walker.build(WTF::move(test));
        if (!passes)
            return true;
        if (!passes.isValue())
            return false;
        tests.append(passes);
    }

    BlockID converted = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    Vector<Reference, 8> appended;
    appended.append(parts.subspan(1));
    for (size_t position = 0; position < guesses.size(); ++position) {
        const GuessAtPart& guess = guesses[position];
        Reference part = parts[guess.place];
        Reference passes = tests[position];
        BlockID passed = walker.addBlock();
        walker.branch(passes, passed, converted);
        walker.continueIn(passed);

        Draft proof(Opcode::Narrowed, origin);
        walker.chooseConservativeFields(proof);
        proof.setValueOperand(part);
        proof.setTestOperand(passes);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        Reference narrowed = walker.build(WTF::move(proof));
        if (!narrowed) {
            walker.fail("a part of a text in a builder passes the test of what is written of it, and nothing can be built for it behind the test"_s);
            return true;
        }
        appended[guess.place - 1] = narrowed;
    }
    walker.noteGuessOpened();
    appendPartsInTextBuilderCallees(walker, join, parts[0], appended.span(), origin);
    if (!walker.isUnreachable())
        walker.jumpTo(meeting);

    walker.continueIn(converted);
    appended.shrink(0);
    for (Reference part : parts.subspan(1)) {
        Reference primitive = primitivePartOfStringBuilder(walker, join.opcode(), part, origin);
        if (!primitive)
            break;
        appended.append(primitive);
    }
    if (appended.size() + 1 == parts.size())
        appendPartsInTextBuilderCallees(walker, join, parts[0], appended.span(), origin);
    if (!walker.isUnreachable())
        walker.jumpTo(meeting);

    walker.continueIn(meeting);
    return true;
}

bool convertPartsAndAppendToTextInStringBuilder(Walker& walker, const BodyValue& join, std::span<const Reference> parts)
{
    if (!walker.hasTextsInStringBuilders() || parts.size() < 2 || !isTextInStringBuilder(walker, parts[0]) || !isGrowthOfTextInStringBuilder(walker, join))
        return false;
    for (Reference part : parts.subspan(1)) {
        if (!part || !part.isRecord() || isTextInStringBuilder(walker, part))
            return false;
    }
    if (!findTextNamedBy(walker, join.id()))
        return false;

    Origin origin = walker.originOf(join);
    origin.isTailCall = false;
    if (appendPartsBehindTestsOfWhatIsWrittenInTextBuilderCallees(walker, join, parts, origin)) {
        if (!walker.isUnreachable())
            walker.bind(join.id(), parts[0]);
        return true;
    }
    Vector<Reference, 8> converted;
    for (Reference part : parts.subspan(1)) {
        Reference primitive = primitivePartOfStringBuilder(walker, join.opcode(), part, origin);
        if (!primitive)
            return true;
        converted.append(primitive);
    }

    appendPartsInTextBuilderCallees(walker, join, parts[0], converted.span(), origin);
    if (!walker.isUnreachable())
        walker.bind(join.id(), parts[0]);
    return true;
}

// ---- A callee that hands its parameter back with more behind it

// What the scan of a loop takes for a cast, and no more: bindCastOfTextInStringBuilder() binds what is named so. For a claim the walk builds a test, and a promise of an int32 is held against what enters the position: both look at a string.
// The text of a text is the text.
static bool handsOperandOnInTextBuilderCallees(const BodyValue& operation, uint32_t operandIndex)
{
    constexpr OpcodeSet casts = opcodesThatStateTypes | OpcodeSet { Opcode::CheckInitialized, Opcode::RequireObjectCoercible };
    Opcode opcode = operation.opcode();
    if (opcode == Opcode::ToString)
        return true;
    if (!casts.contains(opcode) || !operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns))
        return false;
    if (opcode != Opcode::Convert)
        return true;
    DeclaredType declared = operation.as<Opcode::Convert>().declared();
    return !declared.has(PositionFlags::IsClaim) && !declared.has(PositionFlags::IsInt32Promise);
}

namespace {

// One forward scan of the body of a callee without loops and without handlers, for the text that one entry parameter takes. It gives what findGrowingTexts() gives of a loop: every name of the text, and its laws (a) and (b).
// At each point one version is current and only its names are mentioned. Every join that has the text in front counts as a growth: where the older state is mentioned behind one, there is no answer.
// Every Return hands back a name of the version that is current there.
class TextInCalleeScan {
    WTF_MAKE_NONCOPYABLE(TextInCalleeScan);
public:
    TextInCalleeScan(const BodyIndex& body, uint32_t entryParameter)
        : m_body(body)
        , m_entryParameter(entryParameter)
    {
    }

    bool run()
    {
        ValueID parameter = m_body.block(entryBlock).parameter(m_entryParameter);
        m_versionsOfValues.fill(ValueID(), m_body.numberOfValues());
        m_versionsAtEntries.fill(ValueID(), m_body.numberOfBlocks());
        m_versionsOfValues[parameter.index()] = parameter;
        m_versionsAtEntries[entryBlock.index()] = parameter;
        for (uint32_t number = 0; number < m_body.numberOfBlocks(); ++number) {
            if (m_body.isReachable(BlockID(number)) && !scanBlock(m_body.block(BlockID(number))))
                return false;
        }
        return m_grows && m_returns;
    }

    // Ascending by value, the entry parameter first.
    Vector<NameOfGrowingText, 8> names() const
    {
        Vector<NameOfGrowingText, 8> names;
        for (uint32_t value = 0; value < m_versionsOfValues.size(); ++value) {
            if (m_versionsOfValues[value])
                names.append(NameOfGrowingText { ValueID(value), m_versionsOfValues[value] });
        }
        return names;
    }

private:
    ValueID versionOf(ValueID value) const { return value ? m_versionsOfValues[value.index()] : ValueID(); }
    bool mentionsOnly(ValueID value, ValueID current) const
    {
        ValueID version = versionOf(value);
        return !version || version == current;
    }

    bool giveName(ValueID value, ValueID version)
    {
        ValueID& held = m_versionsOfValues[value.index()];
        if (!held)
            held = version;
        return held == version;
    }

    bool arriveAt(BodyBlock entered, std::optional<uint32_t> taking, ValueID arriving)
    {
        if (taking) {
            if (*taking >= entered.numberOfParameters())
                return false;
            ValueID parameter = entered.parameter(*taking);
            if (m_body.numberOfIncomingEdges(entered.id()) > 1)
                arriving = parameter;
            if (!giveName(parameter, arriving))
                return false;
        }
        ValueID& atEntry = m_versionsAtEntries[entered.id().index()];
        if (!atEntry)
            atEntry = arriving;
        return atEntry == arriving;
    }

    bool followEdge(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target, ValueID current)
    {
        std::optional<uint32_t> taking;
        auto passOver = [&](ValueID value, uint32_t parameterIndex) {
            ValueID version = versionOf(value);
            if (!version)
                return true;
            if (version != current || taking)
                return false;
            taking = parameterIndex;
            return true;
        };
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
            if (!passOver(arguments[argumentIndex], argumentIndex))
                return false;
        }
        for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
            std::optional<BodyEdgeResult> result = m_body.edgeResult(terminator, targetIndex, target, proofIndex);
            if (!result)
                continue;
            if (m_body.isUnused(result->parameter) ? !mentionsOnly(result->subject, current) : !passOver(result->subject, static_cast<uint32_t>(arguments.size()) + proofIndex))
                return false;
        }
        return arriveAt(m_body.block(target.block()), taking, current);
    }

    bool scanBlock(BodyBlock block)
    {
        ValueID current = m_versionsAtEntries[block.id().index()];
        if (!current || block.hasHandler() || block.isHandlerBlock())
            return false;

        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            // It reads nothing: no value is live for the sake of a hint.
            if (opcode == Opcode::ReleaseHint)
                continue;
            bool joinsTexts = operationsThatJoinTexts.contains(opcode);
            bool hasTextInFront = false;
            bool handsTextOn = false;
            bool isRefused = false;
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                ValueID version = versionOf(operand);
                if (!version)
                    return;
                if (version != current)
                    isRefused = true;
                else if (operandIndex == frontOperandInTextBuilderCallees && joinsTexts)
                    hasTextInFront = true;
                else if (handsOperandOnInTextBuilderCallees(operation, operandIndex))
                    handsTextOn = true;
                // `t + t`: no text is appended to itself.
                else if (hasTextInFront)
                    isRefused = true;
            });
            if (isRefused)
                return false;
            if (handsTextOn && !giveName(operation.id(), current))
                return false;
            if (!hasTextInFront)
                continue;
            if (opcode == Opcode::Strcat && operation.variadicOperands().size() < 2)
                return false;
            if (!giveName(operation.id(), operation.id()))
                return false;
            current = operation.id();
            m_grows = true;
        }

        BodyTerminator terminator = block.terminator();
        bool mentionsOlderState = false;
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            mentionsOlderState |= !mentionsOnly(operand, current);
        });
        if (mentionsOlderState || terminator.kind() == TerminatorKind::ReturnCall)
            return false;
        if (terminator.kind() == TerminatorKind::Return) {
            m_returns = true;
            return versionOf(terminator.operand(0)) == current;
        }

        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            if (!followEdge(terminator, targetIndex++, target, current))
                return false;
        }
        return true;
    }

    const BodyIndex& m_body;
    uint32_t m_entryParameter;
    // By value. Null: it names no state of the text.
    Vector<ValueID, 64> m_versionsOfValues;
    // By block. Null: no edge has arrived yet.
    Vector<ValueID, 8> m_versionsAtEntries;
    bool m_grows { false };
    bool m_returns { false };
};

struct CalleeThatGrowsText {
    FunctionID callee;
    uint32_t entryParameter { 0 };
    // Among the variadic operands of the call.
    uint32_t place { 0 };
    Vector<NameOfGrowingText, 8> names;
};

} // anonymous namespace

static const NameOfGrowingText* findNameInTextBuilderCallees(const GrowingText& web, ValueID value)
{
    auto found = std::ranges::lower_bound(web.names, value, { }, &NameOfGrowingText::value);
    return found != web.names.end() && found->value == value ? &*found : nullptr;
}

// It answers the same in front of the loop and at the call: by the record of what stands in front of the block being walked, and of what is made further on by the row alone.
static TypeID typeOfCalleeInTextBuilderCallees(Walker& walker, const BodyIndex& index, ValueID callee)
{
    ValueID root = index.valueBehindCasts(callee);
    if (index.strictlyDominates(index.blockOf(root), walker.inputBlockBeingWalked())) {
        Reference built = walker.referenceOf(root);
        return built && built.isRecord() ? walker.code().at(built.id()).type : TypeID();
    }
    if (index.isParameter(root))
        return { };
    BodyValue made = index.value(root);
    bool hasOperands = false;
    made.forEachOperand([&](ValueID, uint32_t) {
        hasOperands = true;
    });
    return hasOperands ? TypeID() : widestTypeOfOperation(walker, made);
}

// The tests that stand in front of a copy look at what is handed over, and the slow version behind a guard takes it as it came. A builder is no value of the program.
static bool copyLooksAtNoOperandInTextBuilderCallees(const Walker& walker, FunctionID callee, uint32_t entryParameter)
{
    if (walker.checksProofs() || walker.plan().conventions().hasParameterGuard(callee))
        return false;
    const DeclaredType* declared = walker.program().at(callee).declaredEntryParameter(entryParameter);
    return !declared || !declared->isEstablished();
}

// Whether a copy pays is asked of the policy, of the call as it is written: no operand is built yet.
static bool policyCopiesCalleeInTextBuilderCallees(Walker& walker, const BodyValue& call, FunctionID callee)
{
    Draft site(call.opcode());
    site.flags = static_cast<uint8_t>(call.flags());
    copyFields(call, site, [](ValueID operand) {
        return Reference::value(operand);
    });
    return shouldCopyCallee(walker, site, callee, { });
}

static std::optional<CalleeThatGrowsText> findCalleeThatGrowsTextInTextBuilderCallees(Walker& walker, const BodyIndex& index, const BodyValue& call)
{
    if (!callsInTextBuilderCallees.contains(call.opcode()) || !walker.has(Mechanism::Inlining) || !walker.opensGuesses())
        return std::nullopt;
    // `return join(s, x)`: the ReturnCall hands on what the call is bound to, and nobody makes a string for it.
    BodyTerminator end = index.terminator(index.blockOf(call.id()));
    if (end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call.id())
        return std::nullopt;

    CalleeThatGrowsText grown;
    if (call.opcode() == Opcode::CallDirect)
        grown.callee = call.as<Opcode::CallDirect>().function();
    else if (TypeID type = typeOfCalleeInTextBuilderCallees(walker, index, call.as<Opcode::Call>().calleeOperand()))
        grown.callee = findOnlyCallee(walker, call, type);
    const BodyIndex* body = grown.callee ? walker.facts().bodies().find(grown.callee) : nullptr;
    if (!body || body->hasLoops() || body->hasIrreducibleFlow() || !body->holdsAnyOf(operationsThatJoinTexts))
        return std::nullopt;
    if (!policyCopiesCalleeInTextBuilderCallees(walker, call, grown.callee))
        return std::nullopt;

    // A CallDirect lists what every entry parameter takes, a Call the arguments.
    const Function& function = walker.program().at(grown.callee);
    std::span<const ValueID> operands = call.variadicOperands();
    unsigned numberOfEntryParameters = function.numberOfEntryParameters();
    if (body->block(entryBlock).numberOfParameters() != numberOfEntryParameters)
        return std::nullopt;
    for (unsigned entryParameter = 0; entryParameter < numberOfEntryParameters; ++entryParameter) {
        if (function.entryParameterKind(entryParameter) != EntryParameter::Parameter)
            continue;
        uint32_t place = entryParameter;
        if (call.opcode() == Opcode::Call) {
            place -= function.firstParameterIndex();
            if (place >= function.numberOfPositionalParameters())
                continue;
        }
        if (place >= operands.size() || !operands[place] || !copyLooksAtNoOperandInTextBuilderCallees(walker, grown.callee, entryParameter))
            continue;
        TextInCalleeScan scan(*body, entryParameter);
        if (!scan.run())
            continue;
        grown.entryParameter = entryParameter;
        grown.place = place;
        grown.names = scan.names();
        return grown;
    }
    return std::nullopt;
}

std::optional<uint32_t> placeOfArgumentHandedBackWithMoreBehind(Walker& walker, const BodyIndex& index, const BodyValue& call)
{
    // True where Walker::walkThroughChain() asks growTextThroughCallee(). Without that link a call that is built after all takes the cell of the builder.
    constexpr bool chainAsksAboutTextsThatGrowInCallees = false;
    if (!chainAsksAboutTextsThatGrowInCallees)
        return std::nullopt;
    std::optional<CalleeThatGrowsText> grown = findCalleeThatGrowsTextInTextBuilderCallees(walker, index, call);
    return grown ? std::optional<uint32_t>(grown->place) : std::nullopt;
}

// The walk stands at the call. While the copy is walked a record of its own says which values of the callee name the text: the frame of the copy is the one on top then.
// The callee has no loop, so no block of it lies in the loop of that record, and no way out of the callee takes the text out of its builder. False: nothing was built.
static bool copyCalleeThatGrowsTextInTextBuilderCallees(Walker& walker, const BodyIndex& index, const BodyValue& call, CalleeThatGrowsText&& grown, Reference builderBegun)
{
    Draft site = walker.draftOf(call);
    Vector<Reference, 8> handedOver = operandsHandedToCopy(site, walker.program().at(grown.callee));
    bool callsParameterOfCopy = call.opcode() == Opcode::Call && walker.depthOfCopies() && index.block(entryBlock).indexOfParameter(index.valueBehindCasts(call.as<Opcode::Call>().calleeOperand()));
    if (!shouldCopyCallee(walker, site, grown.callee, handedOver.span(), callsParameterOfCopy))
        return false;

    TextInStringBuilder inCallee;
    inCallee.web.loopHead = entryBlock;
    inCallee.web.parameterIndex = grown.entryParameter;
    inCallee.web.names = WTF::move(grown.names);
    inCallee.frameIndex = static_cast<uint32_t>(walker.indexOfFrameOnTop() + 1);
    inCallee.builder = builderBegun;
    size_t placeOfRecord = walker.textsInStringBuilders().size();
    walker.textsInStringBuilders().append(WTF::move(inCallee));
    bool isCopied = walker.walkInline(call, grown.callee, handedOver.span());
    walker.textsInStringBuilders().removeAt(placeOfRecord);
    return isCopied;
}

// A link of the chain, in front of whoever takes calls. `call` is a growth of a text in a builder: findGrowingTexts() has taken it on the word of placeOfArgumentHandedBackWithMoreBehind().
extern const OpcodeSet opcodesThatGrowTextThroughCalleeTakes;
constinit const OpcodeSet opcodesThatGrowTextThroughCalleeTakes = callsInTextBuilderCallees;

bool growTextThroughCalleeOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (!callsInTextBuilderCallees.contains(call.opcode()))
        return false;
    TextInStringBuilder* named = findTextNamedBy(walker, call.id());
    const NameOfGrowingText* growth = named && named->web.growsInCallees ? findNameInTextBuilderCallees(named->web, call.id()) : nullptr;
    if (!growth || growth->version != call.id())
        return false;

    ValueID operand;
    std::optional<uint32_t> placeOfOperand;
    std::span<const ValueID> operands = call.variadicOperands();
    for (uint32_t place = 0; place < operands.size(); ++place) {
        if (operands[place] && findNameInTextBuilderCallees(named->web, operands[place])) {
            operand = operands[place];
            placeOfOperand = place;
        }
    }
    Reference name = operand ? walker.referenceBoundTo(operand) : Reference();
    if (!isTextInStringBuilder(walker, name))
        return false;
    // Neither a record for the copy nor a roll back speaks of a cursor.
    if (named->liesInFrame) {
        walker.fail("a text that grows in callees lies in a builder in the frame"_s);
        return true;
    }

    // The list of records may move.
    size_t placeOfText = static_cast<size_t>(named - walker.textsInStringBuilders().span().data());
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<CalleeThatGrowsText> grown = findCalleeThatGrowsTextInTextBuilderCallees(walker, index, call);
    if (grown && grown->place == *placeOfOperand && copyCalleeThatGrowsTextInTextBuilderCallees(walker, index, call, WTF::move(*grown), named->builder)) {
        // The copy has appended through its own record.
        TextInStringBuilder& text = walker.textsInStringBuilders()[placeOfText];
        text.stringOfState = { };
        text.lengthOfState = { };
        text.blockOfState = { };
        // Every Return of the callee hands back a name of the version that is current there. So behind the copy the builder holds what the call gives, whatever the returns have made of it.
        if (!walker.isUnreachable() && !walker.hasFailed())
            walker.bind(call.id(), name);
        return true;
    }

    // The closed arm of a guess calls, and so does whoever finds no room for a copy. The call is a reader, and what it gives is the whole text.
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Reference string = stringOfTextInStringBuilder(walker, walker.textsInStringBuilders()[placeOfText], name, origin);
    if (!string)
        return true;
    walker.bind(operand, string);
    if (!walker.has(Mechanism::DirectCalls) || !callDirectly(walker, call))
        walker.copy(call);
    walker.bind(operand, name);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;

    Reference whole = primitivePartOfStringBuilder(walker, Opcode::Strcat, walker.referenceOf(call.id()), origin);
    if (!whole)
        return true;
    Draft rollBack(Opcode::StringBuilderRollBack, origin);
    walker.chooseConservativeFields(rollBack);
    rollBack.setBuilderOperand(name);
    rollBack.setCheckpointOperand(walker.constant(Draft(Opcode::Integer, origin)));
    walker.build(WTF::move(rollBack));
    std::array<Reference, 1> parts { whole };
    appendPartsToStringBuilder(walker, walker.textsInStringBuilders()[placeOfText], name, parts, origin);
    if (!walker.isUnreachable())
        walker.bind(call.id(), name);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTPlainURL.h"
#include "AOTTypeArena.h"
#include "AOTWalkTextBuilders.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t operationsSavedByScanInStringURLs = 2;

namespace {

enum class QuestionInStringURLs : uint8_t { Pathname, Search, Size, Text, Value, Presence, Entries, Keys, Values, Walk };

struct RowInStringURLs {
    QuestionInStringURLs question { QuestionInStringURLs::Pathname };
    bool goesThroughSearchParams { false };
    ValueID last;
    ValueID name;
    Vector<ValueID, 2> reads;
};

} // anonymous namespace

static BuiltinID memberFoundOnObjectAsMadeInStringURLs(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    Type type = walker.types().at(typeOfObject);
    const ClassPart* part = type.isObject() ? singleBuiltinClassOf(walker.types(), type) : nullptr;
    PropertyKeyID key = read.key();
    BuiltinName name = part && key && !liesInThread(key) && !read.isFreeName() ? program.builtinNameOf(key) : BuiltinName();
    if (!name || !typeFoundOnObjectAsMade(walker, read, typeOfObject))
        return { };
    BuiltinMemberLookup found = table.findForInstance(part->builtinClass, name);
    return found.outcome == BuiltinMemberLookup::Outcome::Found ? table.canonicalID(found.member) : BuiltinID();
}

static bool isTakenByWalkAloneInStringURLs(const BodyIndex& index, ValueID value)
{
    BlockID block = index.blockOf(value);
    while (true) {
        ValueID user = index.onlyUsingValue(value);
        if (!user || index.blockOf(user) != block)
            return false;
        BodyValue operation = index.value(user);
        if (operation.opcode() == Opcode::IteratorOpen)
            return operation.iteratorKind() == IteratorKind::Sync;
        if (!opcodesThatStateTypes.contains(operation.opcode()) || operation.operand(0) != value)
            return false;
        value = user;
    }
}

static std::optional<QuestionInStringURLs> questionAskedByGetterInStringURLs(const BuiltinTable& table, BuiltinID member, bool isQuery)
{
    auto is = [&](BuiltinID named) {
        return table.contains(named) && member == table.canonicalID(named);
    };
    if (isQuery)
        return is(Builtins::URLSearchParamsPrototypeSize) ? std::optional(QuestionInStringURLs::Size) : std::nullopt;
    if (is(Builtins::URLPrototypePathname))
        return QuestionInStringURLs::Pathname;
    if (is(Builtins::URLPrototypeSearch))
        return QuestionInStringURLs::Search;
    return std::nullopt;
}

static std::optional<QuestionInStringURLs> questionAskedByMethodInStringURLs(const BuiltinTable& table, BuiltinID member, size_t numberOfArguments)
{
    auto is = [&](BuiltinID named) {
        return table.contains(named) && member == table.canonicalID(named);
    };
    if (numberOfArguments == 1) {
        if (is(Builtins::URLSearchParamsPrototypeGet))
            return QuestionInStringURLs::Value;
        if (is(Builtins::URLSearchParamsPrototypeHas))
            return QuestionInStringURLs::Presence;
        return std::nullopt;
    }
    if (numberOfArguments)
        return std::nullopt;
    if (is(Builtins::URLSearchParamsPrototypeToString))
        return QuestionInStringURLs::Text;
    if (is(Builtins::URLSearchParamsPrototypeEntries))
        return QuestionInStringURLs::Entries;
    if (is(Builtins::URLSearchParamsPrototypeKeys))
        return QuestionInStringURLs::Keys;
    if (is(Builtins::URLSearchParamsPrototypeValues))
        return QuestionInStringURLs::Values;
    return std::nullopt;
}

static std::optional<RowInStringURLs> findRowInStringURLs(Walker& walker, const CallByOneRow& call, bool isURL, TypeID typeOfObject)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID made = call.call.id();
    BodyBlock block = index.block(index.blockOf(made));
    uint32_t endOfStretch = block.firstValue().index() + walker.endOfStretchBeingWalked();
    auto standsInStretch = [&](ValueID value) {
        return value && index.blockOf(value) == block.id() && value.index() < endOfStretch;
    };

    RowInStringURLs row;
    ValueID subject = made;
    TypeID typeOfSubject = typeOfObject;
    bool isQuery = !isURL;
    while (true) {
        std::optional<BodyUser> mention = index.firstUser(subject);
        if (!mention || mention->kind != BodyUseKind::Operand)
            return std::nullopt;
        BodyValue user = index.value(mention->value);

        if (user.opcode() == Opcode::IteratorOpen) {
            if (!isQuery || row.goesThroughSearchParams || !isTakenByWalkAloneInStringURLs(index, subject))
                return std::nullopt;
            BuiltinID walked = findConstantMemberReadOff(std::as_const(walker), typeOfSubject, program.keyOf(table.nameOf(WellKnownSymbol::Iterator)));
            if (!walked || !table.contains(Builtins::URLSearchParamsPrototypeEntries) || table.canonicalID(walked) != table.canonicalID(Builtins::URLSearchParamsPrototypeEntries))
                return std::nullopt;
            row.question = QuestionInStringURLs::Walk;
            row.last = made;
            return row;
        }

        if (user.opcode() != Opcode::PropGet || user.objectOperand() != subject || !standsInStretch(user.id()))
            return std::nullopt;
        BuiltinID member = memberFoundOnObjectAsMadeInStringURLs(walker, user, typeOfSubject);
        if (!member)
            return std::nullopt;

        if (index.useCount(subject) == 1) {
            if (!readOffObjectAsMadeCallsGetter(walker, user, typeOfSubject))
                return std::nullopt;
            bool readsSearchParams = !isQuery && table.contains(Builtins::URLPrototypeSearchParams) && member == table.canonicalID(Builtins::URLPrototypeSearchParams);
            if (readsSearchParams) {
                row.goesThroughSearchParams = true;
                row.reads.append(user.id());
                typeOfSubject = typeFoundOnObjectAsMade(walker, user, typeOfSubject);
                subject = user.id();
                isQuery = true;
                continue;
            }
            std::optional<QuestionInStringURLs> question = row.goesThroughSearchParams ? std::nullopt : questionAskedByGetterInStringURLs(table, member, isQuery);
            if (!question)
                return std::nullopt;
            row.question = *question;
            row.last = user.id();
            return row;
        }

        ValueID callOfMethod = index.useCount(subject) == 2 && isQuery ? index.onlyUsingValue(user.id()) : ValueID();
        if (!standsInStretch(callOfMethod) || index.opcodeOf(callOfMethod) != Opcode::Call)
            return std::nullopt;
        BodyValue method = index.value(callOfMethod);
        std::span<const ValueID> arguments = method.argumentsOperands();
        if (method.calleeOperand() != user.id() || method.thisValueOperand() != subject || std::ranges::find(arguments, subject) != arguments.end() || method.isDoneInPlace())
            return std::nullopt;
        std::optional<QuestionInStringURLs> question = questionAskedByMethodInStringURLs(table, member, arguments.size());
        if (!question)
            return std::nullopt;
        bool asksForName = *question == QuestionInStringURLs::Value || *question == QuestionInStringURLs::Presence;
        bool listsPairs = *question == QuestionInStringURLs::Entries || *question == QuestionInStringURLs::Keys || *question == QuestionInStringURLs::Values;
        if ((row.goesThroughSearchParams && !asksForName) || (listsPairs && !isTakenByWalkAloneInStringURLs(index, callOfMethod)))
            return std::nullopt;
        if (asksForName && isBoundToStringInBuilder(walker, arguments[0]))
            return std::nullopt;
        row.question = *question;
        row.reads.append(user.id());
        row.last = callOfMethod;
        if (asksForName)
            row.name = arguments[0];
        return row;
    }
}

static bool nothingSeenStandsInRowInStringURLs(const BodyIndex& index, ValueID made, const RowInStringURLs& row)
{
    for (uint32_t between = made.index() + 1; between < row.last.index(); ++between) {
        if (row.reads.contains(ValueID(between)))
            continue;
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (!opcodesThatStateTypes.contains(opcode) && !declaredBoundInTable(opcode).isUnobservable())
            return false;
    }
    return true;
}

static BuiltinID helperThatListsInStringURLs(QuestionInStringURLs question)
{
    if (question == QuestionInStringURLs::Keys)
        return Builtins::CompilerHelpersPlainQueryKeys;
    return question == QuestionInStringURLs::Values ? Builtins::CompilerHelpersPlainQueryValues : Builtins::CompilerHelpersPlainQueryEntries;
}

static bool isPlainBaseInStringURLs(Walker& walker, Reference base)
{
    StringID known = textKnownForStrings(walker, base);
    return known && isPlainBaseURL(byteCast<Latin1Character>(walker.types().bytesOf(known)));
}

static BuiltinID helperThatScansInStringURLs(Walker& walker, bool isURL, Reference base)
{
    if (!isURL)
        return Builtins::CompilerHelpersScanPlainQuery;
    if (!base)
        return Builtins::CompilerHelpersScanPlainURL;
    return isPlainBaseInStringURLs(walker, base) ? Builtins::CompilerHelpersScanPlainURLWithPlainBase : Builtins::CompilerHelpersScanPlainURLWithBase;
}

static BuiltinID helperThatAnswersInStringURLs(const RowInStringURLs& row)
{
    switch (row.question) {
    case QuestionInStringURLs::Value:
    case QuestionInStringURLs::Presence:
        return row.goesThroughSearchParams ? Builtins::CompilerHelpersFindPlainQueryValue : Builtins::CompilerHelpersFindInPlainQuery;
    case QuestionInStringURLs::Entries:
    case QuestionInStringURLs::Keys:
    case QuestionInStringURLs::Values:
    case QuestionInStringURLs::Walk:
        return helperThatListsInStringURLs(row.question);
    case QuestionInStringURLs::Pathname:
    case QuestionInStringURLs::Search:
    case QuestionInStringURLs::Size:
    case QuestionInStringURLs::Text:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Reference shiftedInStringURLs(Walker& walker, Reference scan, unsigned shift, Origin origin)
{
    return shift ? buildOperatorForStrings(walker, Opcode::RShift, scan, integerForStrings(walker, shift), origin) : scan;
}

static Reference bitsInStringURLs(Walker& walker, Reference scan, unsigned shift, int64_t mask, Origin origin)
{
    Reference shifted = shiftedInStringURLs(walker, scan, shift, origin);
    return shifted ? buildOperatorForStrings(walker, Opcode::BitAnd, shifted, integerForStrings(walker, mask), origin) : Reference();
}

static Reference offsetInStringURLs(Walker& walker, Reference scan, unsigned shift, Origin origin)
{
    return bitsInStringURLs(walker, scan, shift, PlainURL::maxOffset, origin);
}

static Reference pieceInStringURLs(Walker& walker, Reference text, Reference start, Reference end, Origin origin)
{
    return start && end ? buildPieceOfString(walker, text, start, end, origin) : Reference();
}

static Reference searchInStringURLs(Walker& walker, Reference text, Reference scan, Origin origin)
{
    Reference endOfPath = offsetInStringURLs(walker, scan, PlainURL::bitsOfOffset, origin);
    Reference endOfQuery = offsetInStringURLs(walker, scan, 0, origin);
    Reference behindQuestionMark = endOfPath && endOfQuery ? buildOperatorForStrings(walker, Opcode::Add, endOfPath, integerForStrings(walker, 1), origin) : Reference();
    Reference hasQuery = behindQuestionMark ? buildOperatorForStrings(walker, Opcode::Less, behindQuestionMark, endOfQuery, origin) : Reference();
    if (!hasQuery)
        return { };
    auto cutQuery = [&](std::span<Reference> results) {
        results[0] = buildPieceOfString(walker, text, endOfPath, endOfQuery, origin);
    };
    auto giveEmptyText = [&](std::span<Reference> results) {
        results[0] = walker.constant(PrimitiveKnownToFolding::text(walker.program().strings.emptyString()));
    };
    std::array<Reference, 1> met;
    return buildArmsForStrings(walker, hasQuery, BlockFrequency::Normal, ArmForStrings(cutQuery), ArmForStrings(giveEmptyText), met) ? met[0] : Reference();
}

static Reference answerOfSearchInStringURLs(Walker& walker, Reference text, Reference found, bool answersValue, BlockID asWritten, Origin origin)
{
    Reference needsNoObject = buildOperatorForStrings(walker, Opcode::GreaterEq, found, integerForStrings(walker, PlainURL::absent), origin);
    if (!needsNoObject)
        return { };
    BlockID answered = walker.addBlock();
    walker.branch(needsNoObject, answered, asWritten);
    walker.continueIn(answered);

    Reference isFound = buildOperatorForStrings(walker, Opcode::GreaterEq, found, integerForStrings(walker, 0), origin);
    if (!isFound)
        return { };
    auto giveWhatWasFound = [&](std::span<Reference> results) {
        if (!answersValue) {
            results[0] = walker.constant(PrimitiveKnownToFolding::boolean(true));
            return;
        }
        results[0] = pieceInStringURLs(walker, text, shiftedInStringURLs(walker, found, PlainURL::bitsOfOffset, origin), offsetInStringURLs(walker, found, 0, origin), origin);
    };
    auto giveNothing = [&](std::span<Reference> results) {
        results[0] = walker.constant(answersValue ? PrimitiveKnownToFolding::null() : PrimitiveKnownToFolding::boolean(false));
    };
    std::array<Reference, 1> met;
    return buildArmsForStrings(walker, isFound, BlockFrequency::Normal, ArmForStrings(giveWhatWasFound), ArmForStrings(giveNothing), met) ? met[0] : Reference();
}

static Reference answerFromPlainTextInStringURLs(Walker& walker, const RowInStringURLs& row, Reference text, Reference scan, BlockID asWritten, Origin origin)
{
    switch (row.question) {
    case QuestionInStringURLs::Pathname:
        return pieceInStringURLs(walker, text, shiftedInStringURLs(walker, scan, 2 * PlainURL::bitsOfOffset, origin), offsetInStringURLs(walker, scan, PlainURL::bitsOfOffset, origin), origin);
    case QuestionInStringURLs::Search:
        return searchInStringURLs(walker, text, scan, origin);
    case QuestionInStringURLs::Size:
        return buildOperatorForStrings(walker, Opcode::URShift, scan, integerForStrings(walker, PlainURL::shiftOfPairCount), origin);
    case QuestionInStringURLs::Text:
        return pieceInStringURLs(walker, text, bitsInStringURLs(walker, scan, PlainURL::bitsOfOffset, 1, origin), offsetInStringURLs(walker, scan, 0, origin), origin);
    case QuestionInStringURLs::Value:
    case QuestionInStringURLs::Presence: {
        Reference name = walker.referenceOf(row.name);
        if (!name)
            return { };
        Reference found = row.goesThroughSearchParams
            ? buildCallOfHostHelperForStrings(walker, helperThatAnswersInStringURLs(row), std::array { text, scan, name }, origin)
            : buildCallOfHostHelperForStrings(walker, helperThatAnswersInStringURLs(row), std::array { text, name }, origin);
        return found ? answerOfSearchInStringURLs(walker, text, found, row.question == QuestionInStringURLs::Value, asWritten, origin) : Reference();
    }
    case QuestionInStringURLs::Entries:
    case QuestionInStringURLs::Keys:
    case QuestionInStringURLs::Values:
    case QuestionInStringURLs::Walk:
        return buildCallOfHostHelperForStrings(walker, helperThatAnswersInStringURLs(row), std::array { text, scan }, origin);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

using URLThatIsNeverMade = NotesOfStrings::URLThatIsNeverMade;
using MentionInStringURLs = URLThatIsNeverMade::Mention;

static bool readsSearchParamsInStringURLs(const BuiltinTable& table, BuiltinID member)
{
    return table.contains(Builtins::URLPrototypeSearchParams) && member == table.canonicalID(Builtins::URLPrototypeSearchParams);
}

static bool everyMentionIsAnsweredOutOfTextInStringURLs(Walker& walker, ValueID made, TypeID typeOfObject, URLThatIsNeverMade& url, uint32_t& numberOfReads)
{
    const BuiltinTable& table = walker.program().builtins();
    const BodyIndex& index = walker.currentBodyIndex();
    TypeID& typeOfQuery = url.typeOfQuery;
    uint32_t first = made.index();
    Vector<MentionInStringURLs, 128> kinds;
    Vector<uint8_t, 128> accounted;
    kinds.fill(MentionInStringURLs::None, index.numberOfValues() - first);
    accounted.fill(0, kinds.size());
    kinds[0] = MentionInStringURLs::URL;
    auto kindOf = [&](ValueID value) {
        return value && value.index() >= first ? kinds[value.index() - first] : MentionInStringURLs::None;
    };
    auto account = [&](ValueID value) {
        uint8_t& count = accounted[value.index() - first];
        count = std::min<unsigned>(count + 1, BodyIndex::maxCountedUses);
    };

    for (uint32_t blockIndex = index.blockOf(made).index(); blockIndex < index.numberOfBlocks(); ++blockIndex) {
        for (BodyValue user : index.block(BlockID(blockIndex)).values()) {
            if (user.id().index() <= first)
                continue;
            uint32_t numberOfMentions = 0;
            user.forEachOperand([&](ValueID operand, uint32_t) {
                numberOfMentions += kindOf(operand) != MentionInStringURLs::None;
            });
            if (!numberOfMentions)
                continue;
            MentionInStringURLs& kindOfUser = kinds[user.id().index() - first];

            if (opcodesThatStateTypes.contains(user.opcode())) {
                MentionInStringURLs handedOn = kindOf(user.operand(0));
                if (numberOfMentions != 1 || (handedOn != MentionInStringURLs::URL && handedOn != MentionInStringURLs::Query))
                    return false;
                kindOfUser = handedOn;
                account(user.operand(0));
                continue;
            }
            if (user.opcode() == Opcode::PropGet) {
                MentionInStringURLs object = kindOf(user.objectOperand());
                if (numberOfMentions != 1)
                    return false;
                if (object == MentionInStringURLs::URL) {
                    BuiltinID member = memberFoundOnObjectAsMadeInStringURLs(walker, user, typeOfObject);
                    if (!member || !readOffObjectAsMadeCallsGetter(walker, user, typeOfObject))
                        return false;
                    if (readsSearchParamsInStringURLs(table, member)) {
                        typeOfQuery = typeFoundOnObjectAsMade(walker, user, typeOfObject);
                        if (!typeOfQuery)
                            return false;
                        kindOfUser = MentionInStringURLs::Query;
                    } else if (!questionAskedByGetterInStringURLs(table, member, false))
                        return false;
                } else if (object == MentionInStringURLs::Query) {
                    BuiltinID member = memberFoundOnObjectAsMadeInStringURLs(walker, user, typeOfQuery);
                    std::optional<QuestionInStringURLs> question = member ? questionAskedByMethodInStringURLs(table, member, 1) : std::nullopt;
                    if (!question)
                        return false;
                    kindOfUser = MentionInStringURLs::Method;
                } else
                    return false;
                account(user.objectOperand());
                ++numberOfReads;
                continue;
            }
            if (user.opcode() == Opcode::Call) {
                ValueID method = user.calleeOperand();
                ValueID query = user.thisValueOperand();
                if (numberOfMentions != 2 || kindOf(method) != MentionInStringURLs::Method || kindOf(query) != MentionInStringURLs::Query)
                    return false;
                if (user.argumentsOperands().size() != 1 || user.isDoneInPlace() || index.blockOf(method) != index.blockOf(user.id()))
                    return false;
                if (index.stripCasts(index.value(method).objectOperand()) != index.stripCasts(query))
                    return false;
                account(method);
                account(query);
                continue;
            }
            return false;
        }
    }

    for (uint32_t place = 0; place < kinds.size(); ++place) {
        if (kinds[place] == MentionInStringURLs::None)
            continue;
        uint32_t uses = index.useCount(ValueID(first + place));
        if (uses >= BodyIndex::maxCountedUses || uses != accounted[place])
            return false;
        if (walker.isBoundAnewAtEveryBlock(ValueID(first + place)))
            return false;
        url.mentions.append({ ValueID(first + place), kinds[place] });
    }
    return true;
}

static MentionInStringURLs mentionInStringURLs(const URLThatIsNeverMade& url, ValueID value)
{
    for (auto [mentioned, kind] : url.mentions) {
        if (mentioned == value)
            return kind;
    }
    return MentionInStringURLs::None;
}

static const URLThatIsNeverMade* findURLThatIsNeverMadeByMention(Walker& walker, ValueID mentioned)
{
    for (const URLThatIsNeverMade& url : notesOfStrings(walker).urlsThatAreNeverMade) {
        if (url.inlineFrame != walker.originOfOperation().inlineFrame || mentionInStringURLs(url, mentioned) == MentionInStringURLs::None)
            continue;
        Reference bound = walker.referenceBoundTo(url.made);
        if (bound == url.object)
            return &url;
        if (!bound)
            bound = walker.referenceBoundTo(mentioned);
        if (bound && walker.types().overlap(walker.typeOf(bound), TypeTable::undefined))
            walker.fail("the name of a URL that is never made has met another value"_s);
    }
    return nullptr;
}

bool standsForMethodOfURLThatIsNeverMade(Walker& walker, const BodyValue& read)
{
    const URLThatIsNeverMade* url = findURLThatIsNeverMadeByMention(walker, read.objectOperand());
    return url && mentionInStringURLs(*url, read.id()) == MentionInStringURLs::Method;
}

static Reference buildAsWrittenInStringURLs(Walker& walker, const BodyValue& value)
{
    Draft written = walker.draftOf(value);
    walker.chooseConservativeFields(written);
    Reference built = walker.build(WTF::move(written));
    if (built)
        walker.bind(value.id(), built);
    return built;
}

static Reference isPlainInStringURLs(Walker& walker, const URLThatIsNeverMade& url, Origin origin)
{
    return buildOperatorForStrings(walker, Opcode::GreaterEq, url.scan, integerForStrings(walker, 0), origin);
}

static Reference meetInStringURLs(Walker& walker, BlockID meeting, Reference answer, BlockID endOfPlain, Reference written, BlockID endOfWritten)
{
    Vector<Walker::JoinArm, 2> arms;
    if (endOfPlain)
        arms.append(Walker::JoinArm { endOfPlain, { answer } });
    if (endOfWritten)
        arms.append(Walker::JoinArm { endOfWritten, { written } });
    Reference met;
    if (!arms.isEmpty())
        walker.joinArms(arms.span(), meeting, singleElementSpan(met));
    return met;
}

static bool answerReadOffURLInStringURLs(Walker& walker, const URLThatIsNeverMade& url, const BodyValue& read)
{
    const BuiltinTable& table = walker.program().builtins();
    Origin origin = walker.originOfOperation();
    BuiltinID member = memberFoundOnObjectAsMadeInStringURLs(walker, read, url.typeOfObject);
    std::optional<QuestionInStringURLs> question = member ? questionAskedByGetterInStringURLs(table, member, false) : std::nullopt;
    if (!member || (!question && !readsSearchParamsInStringURLs(table, member))) {
        walker.fail("a read off a URL that is never made is none of those that its text answers"_s);
        return true;
    }
    auto answerOutOfText = [&](std::span<Reference> results) {
        if (!question) {
            results[0] = walker.constant(PrimitiveKnownToFolding::undefined());
            return;
        }
        RowInStringURLs row;
        row.question = *question;
        results[0] = answerFromPlainTextInStringURLs(walker, row, url.text, url.scan, BlockID(), origin);
    };
    auto readOffObject = [&](std::span<Reference> results) {
        Walker::ClosedArm closedArm(walker);
        Reference written = buildAsWrittenInStringURLs(walker, read);
        results[0] = written && question ? walker.proveOrTrap(written, TypeTable::string, ProofSource::ResultOfCall, origin) : written;
    };
    std::array<Reference, 1> met;
    if (buildArmsForStrings(walker, isPlainInStringURLs(walker, url, origin), BlockFrequency::Rare, ArmForStrings(answerOutOfText), ArmForStrings(readOffObject), met))
        walker.bind(read.id(), met[0]);
    return true;
}

static bool answerCallOnQueryInStringURLs(Walker& walker, const URLThatIsNeverMade& url, const BodyValue& call, const BodyValue& readOfQuery)
{
    const BuiltinTable& table = walker.program().builtins();
    const BodyIndex& index = walker.currentBodyIndex();
    Origin origin = walker.originOfOperation();
    BodyValue method = index.value(call.calleeOperand());
    BuiltinID member = memberFoundOnObjectAsMadeInStringURLs(walker, method, url.typeOfQuery);
    std::optional<QuestionInStringURLs> question = member ? questionAskedByMethodInStringURLs(table, member, call.argumentsOperands().size()) : std::nullopt;
    if (!question || (*question != QuestionInStringURLs::Value && *question != QuestionInStringURLs::Presence)) {
        walker.fail("a call on the searchParams of a URL that is never made is none of those that its text answers"_s);
        return true;
    }
    Reference isPlain = isPlainInStringURLs(walker, url, origin);
    if (!isPlain)
        return true;

    BlockID plain = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.branch(isPlain, plain, asWritten);

    walker.continueIn(plain);
    RowInStringURLs row;
    row.question = *question;
    row.goesThroughSearchParams = true;
    row.name = call.argumentsOperands()[0];
    Reference answer = answerFromPlainTextInStringURLs(walker, row, url.text, url.scan, asWritten, origin);
    if (walker.hasFailed())
        return true;
    BlockID endOfPlain = answer && !walker.isUnreachable() ? walker.currentBlock() : BlockID();

    walker.continueIn(asWritten);
    Reference written;
    BlockID endOfWritten;
    {
        Walker::ClosedArm closedArm(walker);
        Reference kept = walker.referenceOf(call.thisValueOperand());
        auto makeObject = [&](std::span<Reference> results) {
            Draft again = *url.constructionAsWritten;
            walker.chooseConservativeFields(again);
            Reference object = walker.build(WTF::move(again));
            if (!object)
                return;
            walker.bind(url.made, object);
            walker.bind(readOfQuery.objectOperand(), object);
            results[0] = buildAsWrittenInStringURLs(walker, readOfQuery);
        };
        auto takeObjectKept = [&](std::span<Reference> results) {
            results[0] = kept;
        };
        std::array<Reference, 1> query;
        if (buildArmsForStrings(walker, isPlainInStringURLs(walker, url, origin), BlockFrequency::Normal, ArmForStrings(makeObject), ArmForStrings(takeObjectKept), query)) {
            walker.bind(method.objectOperand(), query[0]);
            walker.bind(call.thisValueOperand(), query[0]);
            if (buildAsWrittenInStringURLs(walker, method))
                written = buildAsWrittenInStringURLs(walker, call);
        }
        if (written && !walker.isUnreachable() && !walker.hasFailed())
            endOfWritten = walker.currentBlock();
    }
    if (walker.hasFailed())
        return true;
    if (Reference met = meetInStringURLs(walker, meeting, answer, endOfPlain, written, endOfWritten))
        walker.bind(call.id(), met);
    return true;
}

bool answerFromURLThatIsNeverMadeOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID subject = value.opcode() == Opcode::PropGet ? value.objectOperand() : value.opcode() == Opcode::Call ? value.thisValueOperand() : value.operand(0);
    const URLThatIsNeverMade* url = subject ? findURLThatIsNeverMadeByMention(walker, subject) : nullptr;
    if (!url)
        return walker.hasFailed();

    MentionInStringURLs mention = mentionInStringURLs(*url, subject);
    if (opcodesThatStateTypes.contains(value.opcode()) && mention != MentionInStringURLs::Method) {
        walker.bind(value.id(), walker.referenceBoundTo(subject));
        return true;
    }
    if (value.opcode() == Opcode::PropGet && mention == MentionInStringURLs::URL)
        return answerReadOffURLInStringURLs(walker, *url, value);
    if (value.opcode() == Opcode::PropGet && mention == MentionInStringURLs::Query && mentionInStringURLs(*url, value.id()) == MentionInStringURLs::Method) {
        walker.bind(value.id(), walker.constant(PrimitiveKnownToFolding::undefined()));
        return true;
    }
    if (value.opcode() == Opcode::Call && mention == MentionInStringURLs::Query && mentionInStringURLs(*url, value.calleeOperand()) == MentionInStringURLs::Method)
        return answerCallOnQueryInStringURLs(walker, *url, value, index.value(index.stripCasts(subject)));
    walker.fail("an operation takes a URL that is never made, and the look at the body has not seen it"_s);
    return true;
}

static bool buildURLThatIsNeverMade(Walker& walker, const CallByOneRow& call, Reference text, Reference base, TypeID typeOfObject)
{
    if (call.writtenArguments().empty())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID made = call.call.id();
    if (walker.hasStringsInBuilders()) {
        for (ValueID argument : call.writtenArguments()) {
            if (findStringInBuilder(walker, argument))
                return false;
        }
    }
    URLThatIsNeverMade url;
    uint32_t numberOfReads = 0;
    url.constructionAsWritten = walker.draftOf(call.call);
    if (!everyMentionIsAnsweredOutOfTextInStringURLs(walker, made, typeOfObject, url, numberOfReads) || !numberOfReads)
        return false;
    TypeID typeOfQuery = url.typeOfQuery;
    BuiltinID scanner = helperThatScansInStringURLs(walker, true, base);
    if (!canCallHostHelperForStrings(walker, scanner) || (typeOfQuery && !canCallHostHelperForStrings(walker, Builtins::CompilerHelpersFindPlainQueryValue)))
        return false;

    bool isProven = isProvenString(walker, walker.typeOf(text));
    if (!isProven) {
        TypeID written = findExpectation(walker, call.writtenArguments()[0]).type;
        if (!text.isValue() || !walker.types().overlap(walker.typeOf(text), TypeTable::string) || (written && !walker.types().overlap(written, TypeTable::string)))
            return false;
    }
    if (!shouldOpenGuess(walker, text, TypeTable::string, true, 1 + numberOfReads))
        return false;

    Origin origin = call.origin;
    BlockID block = index.blockOf(made);
    uint32_t place = placeInBlockForStrings(index, made);
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.letGuessFailUnasked(asWritten, origin);
    Reference string = text;
    if (!isProven) {
        Reference isString = buildTypeTestForStrings(walker, text, TypeTable::string, origin);
        if (!isString)
            return true;
        BlockID tested = walker.addBlock();
        walker.branch(isString, tested, asWritten);
        walker.continueIn(tested);
        string = buildProofBehindTestForStrings(walker, text, isString, OperandIndex::TypeTest::value, origin);
        if (!string)
            return true;
    }
    bool passesBase = base && scanner == BuiltinID(Builtins::CompilerHelpersScanPlainURLWithBase);
    Reference scan = passesBase
        ? buildCallOfHostHelperForStrings(walker, scanner, std::array { string, base }, origin)
        : buildCallOfHostHelperForStrings(walker, scanner, std::array { string }, origin);
    Reference isPlain = scan ? buildOperatorForStrings(walker, Opcode::GreaterEq, scan, integerForStrings(walker, 0), origin) : Reference();
    if (!isPlain)
        return true;
    BlockID plain = walker.addBlock();
    walker.branch(isPlain, plain, asWritten);
    walker.recordSpeculation();

    walker.continueIn(asWritten);
    Reference object;
    BlockID endOfWritten;
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block, place, place + 1);
        if (!walker.isUnreachable() && !walker.hasFailed()) {
            object = walker.referenceOf(made);
            endOfWritten = walker.currentBlock();
        }
    }
    if (walker.hasFailed())
        return true;

    Reference nothing = walker.constant(PrimitiveKnownToFolding::undefined());
    url.inlineFrame = walker.originOfOperation().inlineFrame;
    url.made = made;
    url.typeOfObject = typeOfObject;
    Vector<Walker::JoinArm, 2> arms;
    if (endOfWritten) {
        Reference noText = walker.constant(PrimitiveKnownToFolding::text(walker.program().strings.emptyString()));
        arms.append(Walker::JoinArm { endOfWritten, { object, noText, integerForStrings(walker, PlainURL::notPlain) } });
    }
    arms.append(Walker::JoinArm { plain, { nothing, string, scan } });
    std::array<Reference, 3> met;
    if (!walker.joinArms(arms.span(), meeting, met))
        return true;
    url.object = met[0];
    url.text = met[1];
    url.scan = met[2];
    walker.bind(made, url.object);

    Vector<URLThatIsNeverMade, 1>& urls = notesOfStrings(walker).urlsThatAreNeverMade;
    urls.removeAllMatching([&, inlineFrame = url.inlineFrame](const URLThatIsNeverMade& other) {
        return other.made == made && other.inlineFrame == inlineFrame;
    });
    urls.append(WTF::move(url));
    walker.noteURLThatIsNeverMade();
    walker.count(StatisticID::URLAllocationsElided);
    return true;
}

bool buildURLCallByScan(Walker& walker, const CallByOneRow& call)
{
    if (call.mode != BuiltinCallMode::Construct || call.call.opcode() != Opcode::Construct || call.receiver)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    bool isURL = table.contains(Builtins::URL) && call.builtin == table.canonicalID(Builtins::URL);
    bool isQuery = table.contains(Builtins::URLSearchParams) && call.builtin == table.canonicalID(Builtins::URLSearchParams);
    if ((!isURL && !isQuery) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    if (call.arguments.empty() || call.arguments.size() > (isURL ? 2u : 1u))
        return false;

    Reference text = call.arguments[0];
    Reference base = call.arguments.size() > 1 ? call.arguments[1] : Reference();
    if (base && primitiveKnownForStrings(walker, base).kind() == PrimitiveKnownToFolding::Kind::Undefined)
        base = { };
    if (!text || (base && !isProvenString(walker, walker.typeOf(base))))
        return false;

    TypeID typeOfObject = typeGivenByRowForStrings(walker, call);
    std::optional<RowInStringURLs> row = typeOfObject && isProvenString(walker, walker.typeOf(text)) ? findRowInStringURLs(walker, call, isURL, typeOfObject) : std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID made = call.call.id();
    if (!row || !nothingSeenStandsInRowInStringURLs(index, made, *row))
        return isURL && typeOfObject && !walker.walksStretchInsideAnother() && buildURLThatIsNeverMade(walker, call, text, base, typeOfObject);
    BuiltinID scanner = helperThatScansInStringURLs(walker, isURL, base);
    BuiltinID answerer = helperThatAnswersInStringURLs(*row);
    if (!canCallHostHelperForStrings(walker, scanner) || (answerer && !canCallHostHelperForStrings(walker, answerer)))
        return false;
    bool givesList = row->question == QuestionInStringURLs::Entries || row->question == QuestionInStringURLs::Keys || row->question == QuestionInStringURLs::Values || row->question == QuestionInStringURLs::Walk;
    if (givesList)
        return false;
    if (!shouldOpenGuess(walker, text, TypeTable::string, true, operationsSavedByScanInStringURLs))
        return false;

    BlockID block = index.blockOf(made);
    uint32_t placeOfConstruction = placeInBlockForStrings(index, made);
    uint32_t placeOfLast = placeInBlockForStrings(index, row->last);
    for (uint32_t place = placeOfConstruction + 1; place < placeOfLast; ++place) {
        if (row->reads.contains(ValueID(made.index() + (place - placeOfConstruction))))
            continue;
        walker.walkStretch(block, place, place + 1);
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }

    Origin origin = call.origin;
    bool passesBase = base && scanner == BuiltinID(Builtins::CompilerHelpersScanPlainURLWithBase);
    Reference scan = passesBase
        ? buildCallOfHostHelperForStrings(walker, scanner, std::array { text, base }, origin)
        : buildCallOfHostHelperForStrings(walker, scanner, std::array { text }, origin);
    Reference isPlain = scan ? buildOperatorForStrings(walker, Opcode::GreaterEq, scan, integerForStrings(walker, 0), origin) : Reference();
    if (!isPlain)
        return true;

    BlockID plain = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.letGuessFailUnasked(asWritten, origin);
    walker.branch(isPlain, plain, asWritten);
    walker.recordSpeculation();
    walker.count(StatisticID::URLAllocationsElided);

    walker.continueIn(plain);
    Reference answer = answerFromPlainTextInStringURLs(walker, *row, text, scan, asWritten, origin);
    if (walker.hasFailed())
        return true;
    BlockID endOfPlain = answer && !walker.isUnreachable() ? walker.currentBlock() : BlockID();

    walker.continueIn(asWritten);
    Reference written;
    BlockID endOfWritten;
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block, placeOfConstruction, placeOfLast + 1);
        if (!walker.isUnreachable() && !walker.hasFailed()) {
            written = walker.referenceOf(row->last);
            endOfWritten = walker.currentBlock();
        }
    }
    if (walker.hasFailed())
        return true;
    Reference met = meetInStringURLs(walker, meeting, answer, endOfPlain, written, endOfWritten);
    if (!met)
        return true;
    walker.bind(row->last, met);
    walker.walkStretch(block, placeOfLast + 1, placeOfLast + 1);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

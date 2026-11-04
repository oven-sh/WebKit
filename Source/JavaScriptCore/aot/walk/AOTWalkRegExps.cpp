/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// BuiltinCalls, with EscapeAnalysis. `new RegExp("a+")` is the literal, which has a record and code in the image. A RegExp that the body writes for one call alone, where the row of the call hands it to nobody and leaves it as it was made,
// is the one object that all evaluations share, and a match with it enters the code of its record.
// At a suspension: nothing, no match suspends. At a throw: what is built throws where the call threw. A text of which no RegExp can be made stays a call, which throws.
// Withheld: the call goes by its row with the object that was made for it, or is copied in its closed form.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSweep.h"
#include "AOTWalkStrings.h"
#include "AOTWalkStringFolds.h"
#include "YarrFlags.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfEnumerators<RegExpFlags> == Yarr::numberOfFlags);

// Null: nobody has handed the walk the summaries of the build, and nothing is claimed of any text.
static const RegExpSummary* summaryInWalkRegExps(Walker& walker, StringID pattern, OptionSet<RegExpFlags> flags)
{
    RegExpSummaries* summaries = walker.regExpSummaries();
    if (!summaries)
        return nullptr;
    Vector<char16_t> buffer;
    return &summaries->summaryOf(pattern, toYarrFlags(flags), walker.types().viewOf(pattern, buffer));
}

// ---- new RegExp(text, flags), RegExp(text, flags)

static StringID textKnownInWalkRegExps(Walker& walker, Reference reference)
{
    if (reference.isRecord() && walker.code().at(reference.id()).opcode == Opcode::String)
        return walker.code().at(reference.id()).string();
    TypeID type = walker.typeOf(reference);
    return type ? singleStringLiteralOf(walker.types().at(type)) : StringID();
}

static std::optional<OptionSet<RegExpFlags>> flagsKnownInWalkRegExps(Walker& walker, Reference reference)
{
    if (!reference)
        return OptionSet<RegExpFlags> { };
    TypeID type = walker.typeOf(reference);
    if (type && walker.types().at(type).hasOnlyPrimitives(Primitive::Undefined))
        return OptionSet<RegExpFlags> { };
    StringID letters = textKnownInWalkRegExps(walker, reference);
    if (!letters)
        return std::nullopt;
    Vector<char16_t> buffer;
    std::optional<OptionSet<Yarr::Flags>> parsed = Yarr::parseFlags(walker.types().viewOf(letters, buffer));
    if (!parsed)
        return std::nullopt;
    OptionSet<RegExpFlags> flags;
    for (RegExpFlags flag : allMembersOf<RegExpFlags>) {
        if (parsed->contains(toYarrFlag(flag)))
            flags.add(flag);
    }
    return flags;
}

// With the metadata of its record the run time builds no pattern, so making the object cannot throw.
Reference buildRegExpOfKnownTexts(Walker& walker, Reference pattern, Reference flagsOrNull, Origin origin)
{
    StringID text = textOfKnownPatternOfRegExp(walker, pattern);
    std::optional<OptionSet<RegExpFlags>> flags = text ? flagsKnownInWalkRegExps(walker, flagsOrNull) : std::nullopt;
    if (!flags)
        return { };
    const RegExpSummary* summary = summaryInWalkRegExps(walker, text, *flags);
    if (!summary || !summary->canBeCreated())
        return { };

    Draft creation(Opcode::NewRegExp, origin);
    walker.chooseConservativeFields(creation);
    creation.setPattern(text);
    creation.setRegExpFlags(*flags);
    return walker.build(WTF::move(creation));
}

// ---- A RegExp that is written for one call

// Every mention of what `operand` stands for is that one operand of the call or the object of the read that finds the callee, behind operations that state types alone.
static bool isMentionedByCallAloneInWalkRegExps(const BodyIndex& index, const BodyValue& call, ValueID operand)
{
    ValueID read = index.valueBehindCasts(call.calleeOperand());
    ValueID objectOfRead = read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet ? index.value(read).objectOperand() : ValueID();

    ValueID name = index.valueBehindCasts(operand);
    while (!index.isParameter(name)) {
        uint32_t mentionsByCall = 0;
        call.forEachOperand([&](ValueID mentioned, uint32_t) {
            mentionsByCall += mentioned == name;
        });
        uint32_t mentionsByRead = objectOfRead == name;
        if (mentionsByCall)
            return mentionsByCall == 1 && index.useCount(name) == 1 + mentionsByRead;
        ValueID next = mentionsByRead ? ValueID() : index.onlyUsingValue(name);
        if (!next || !opcodesThatStateTypes.contains(index.opcodeOf(next)))
            return false;
        name = next;
    }
    return false;
}

std::optional<RegExpWrittenForCall> findRegExpWrittenForCall(Walker& walker, const BodyValue& call, ValueID operand)
{
    Reference built = operand ? walker.referenceOf(operand) : Reference();
    if (!built || !built.isValue())
        return std::nullopt;
    const Code& code = walker.code();
    ValueID cell = rootOfCellInSweptCode(code, built.id());
    if (code.isConstant(cell) || !walker.state().isFresh(Reference::value(cell)))
        return std::nullopt;
    const Operation& made = code.at(cell);
    if (made.opcode != Opcode::NewRegExp || made.isNeverToldApart() || !isMentionedByCallAloneInWalkRegExps(walker.currentBodyIndex(), call, operand))
        return std::nullopt;
    return RegExpWrittenForCall { built, made.pattern(), made.regExpFlags(), made.origin() };
}

namespace {

// What a row does with a RegExp that it is handed.
enum class UseOfRegExpByRow : uint8_t {
    // It reads pattern and flags, and puts back what it stores.
    OnlyInspects,
    // Under g it stores 0 as lastIndex first, and stops at the failed match that stores 0 again.
    MatchesFromStart,
    MatchesFromLastIndex,
    Other,
};

} // anonymous namespace

// AppliesToContents: the protocols of the row run on what lastIndex holds, which in a RegExp that is as it was made is a number.
static UseOfRegExpByRow useOfRegExpByRowInWalkRegExps(const BuiltinTable& table, const BuiltinOverload& row, BuiltinOperand operand)
{
    constexpr OperandUses usesOfContents { OperandUse::ReadsContents, OperandUse::WritesContents };
    constexpr OperandUses usesOfMatching { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::AppliesToContents };

    const BuiltinParameter* parameter = table.parameterFor(row, operand);
    bool mayRunCodeThatNoOperandNames = row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
    if (!parameter || row.isUndeclared() || row.libraryFunction() || mayRunCodeThatNoOperandNames)
        return UseOfRegExpByRow::Other;
    if (!parameter->uses.containsAny(usesOfContents))
        return parameter->protocols.isEmpty() && parameter->uses.containsOnly({ OperandUse::Inspects }) ? UseOfRegExpByRow::OnlyInspects : UseOfRegExpByRow::Other;
    if (!parameter->uses.containsOnly(usesOfMatching) || !parameter->protocols.containsOnly({ Protocol::ToPrimitive }) || !row.code.implementation.inlineBuiltin)
        return UseOfRegExpByRow::Other;

    switch (*row.code.implementation.inlineBuiltin) {
    case InlineBuiltin::RegExpExec:
    case InlineBuiltin::RegExpTest:
        return UseOfRegExpByRow::MatchesFromLastIndex;
    case InlineBuiltin::StringMatch:
    case InlineBuiltin::StringReplace:
    case InlineBuiltin::StringReplaceAll:
        return UseOfRegExpByRow::MatchesFromStart;
    default:
        return UseOfRegExpByRow::Other;
    }
}

static bool leavesRegExpAsMadeInWalkRegExps(UseOfRegExpByRow use, OptionSet<RegExpFlags> flags)
{
    switch (use) {
    case UseOfRegExpByRow::OnlyInspects:
        return true;
    case UseOfRegExpByRow::MatchesFromStart:
        return !flags.contains(RegExpFlags::Sticky);
    case UseOfRegExpByRow::MatchesFromLastIndex:
        return !flags.containsAny({ RegExpFlags::Global, RegExpFlags::Sticky });
    case UseOfRegExpByRow::Other:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The cell is found by the place where the RegExp is written, its pattern and its flags. For a function without a text several literals would meet at the place of the call around it.
Reference buildRegExpSharedForCall(Walker& walker, BuiltinOverloadID overload, BuiltinOperand operand, const RegExpWrittenForCall& regExp)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!leavesRegExpAsMadeInWalkRegExps(useOfRegExpByRowInWalkRegExps(table, table.at(overload), operand), regExp.flags))
        return { };
    FunctionID writtenIn = walker.code().inlineFrames()[regExp.whereWritten.inlineFrame].callee;
    if (regExp.whereWritten.position == DraftWords::noPosition || !writtenIn || !hasModule(walker.program().at(writtenIn).locationAt(regExp.whereWritten.position)))
        return { };

    Draft shared(Opcode::SharedRegExpLiteral, regExp.whereWritten);
    walker.chooseConservativeFields(shared);
    shared.setPattern(regExp.pattern);
    shared.setRegExpFlags(regExp.flags);
    return walker.build(WTF::move(shared));
}

static std::optional<RegExpMatchForm> matchFormInWalkRegExps(const BuiltinOverload& row)
{
    if (!row.code.implementation.inlineBuiltin)
        return std::nullopt;
    switch (*row.code.implementation.inlineBuiltin) {
    case InlineBuiltin::RegExpTest:
        return RegExpMatchForm::Test;
    case InlineBuiltin::RegExpExec:
        return RegExpMatchForm::Exec;
    case InlineBuiltin::StringSearch:
        return RegExpMatchForm::Search;
    case InlineBuiltin::StringMatch:
        return RegExpMatchForm::Match;
    default:
        return std::nullopt;
    }
}

// A pattern that is one text has no code: whoever matches with its object searches for the text.
// A match without the object begins at the start of the string and leaves no record of itself, so nothing may ever read `RegExp.$1` and its like.
Reference buildMatchByCodeOfRegExp(Walker& walker, const BodyValue& call, BuiltinOverloadID overload, const RegExpWrittenForCall& regExp, Reference objectOfCall, Reference string, Origin origin, ExpectedType expected)
{
    std::optional<RegExpMatchForm> form = matchFormInWalkRegExps(walker.program().builtins().at(overload));
    TypeID typeOfString = string ? walker.typeOf(string) : TypeID();
    if (!form || !typeOfString || !walker.types().at(typeOfString).isAnyString())
        return { };
    const RegExpSummary* summary = summaryInWalkRegExps(walker, regExp.pattern, regExp.flags);
    if (summary && !summary->atom.isNull())
        return buildSearchForAtomOfRegExp(walker, call, overload, regExp, string, origin);
    if (!summary || !summary->canBeCreated() || !summary->atom.isNull())
        return { };

    Draft match(Opcode::RegExpMatchLiteral, origin);
    walker.chooseConservativeFields(match);
    match.setOverload(overload);
    match.setStringOperand(string);
    if (regExp.flags.containsAny(flagsOfMatchesUsingObject) || walker.facts().mayReadLegacyRegExpStatics())
        match.setRegExpOperand(objectOfCall);
    match.setPattern(regExp.pattern);
    match.setRegExpFlags(regExp.flags);
    match.setMatchForm(*form);
    match.expected = expected;
    match.inputValueGiven = call.id();
    return walker.build(WTF::move(match));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

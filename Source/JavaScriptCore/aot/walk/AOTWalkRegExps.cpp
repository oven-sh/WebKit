/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSweep.h"
#include "AOTWalkResultsInParts.h"
#include "AOTWalkStrings.h"
#include "AOTWalkStringFolds.h"
#include "YarrFlags.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfEnumerators<RegExpFlags> == Yarr::numberOfFlags);

static const RegExpSummary* summaryInWalkRegExps(Walker& walker, StringID pattern, OptionSet<RegExpFlags> flags)
{
    RegExpSummaries* summaries = walker.regExpSummaries();
    if (!summaries)
        return nullptr;
    Vector<char16_t> buffer;
    return &summaries->summaryOf(pattern, toYarrFlags(flags), walker.types().viewOf(pattern, buffer));
}

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

static bool isMentionedByCallAloneInWalkRegExps(const BodyIndex& index, const BodyValue& call, ValueID operand)
{
    ValueID read = index.stripCasts(call.calleeOperand());
    ValueID objectOfRead = read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet ? index.value(read).objectOperand() : ValueID();

    ValueID name = index.stripCasts(operand);
    while (true) {
        uint32_t mentionsByCall = 0;
        call.forEachOperand([&](ValueID mentioned, uint32_t) {
            mentionsByCall += mentioned == name;
        });
        uint32_t mentionsByRead = objectOfRead == name;
        if (mentionsByCall)
            return mentionsByCall == 1 && index.useCount(name) == 1 + mentionsByRead;
        ValueID next = mentionsByRead ? ValueID() : index.onlyUsingValue(name);
        if (!next || index.isParameter(next) || !opcodesThatStateTypes.contains(index.opcodeOf(next)))
            return false;
        name = next;
    }
}

std::optional<RegExpWrittenForCall> findRegExpWrittenForCall(Walker& walker, const BodyValue& call, ValueID operand)
{
    Reference built = operand ? walker.referenceOf(operand) : Reference();
    if (!built || !built.isValue())
        return std::nullopt;
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, built.id());
    if (code.isConstant(cell) || !walker.state().isFresh(Reference::value(cell)))
        return std::nullopt;
    const Operation& made = code.at(cell);
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = index.stripCasts(operand);
    if (made.opcode != Opcode::NewRegExp || !isMentionedByCallAloneInWalkRegExps(index, call, operand))
        return std::nullopt;
    if (index.isParameter(written)) {
        if (index.blockOf(written) != entryBlock || !walker.callCopiedAloneTakesRegExp(built))
            return std::nullopt;
        return RegExpWrittenForCall { built, made.pattern(), made.regExpFlags(), made.origin() };
    }
    Origin whereOperandIsWritten = walker.originOf(index.value(written));
    if (made.origin().position != whereOperandIsWritten.position || made.origin().inlineFrame != whereOperandIsWritten.inlineFrame)
        return std::nullopt;
    return RegExpWrittenForCall { built, made.pattern(), made.regExpFlags(), made.origin() };
}

static std::optional<HeldRegExp> findRegExpInBody(const Walker& walker, FunctionID function, const BodyIndex& index, ValueID operand, size_t framesOutward)
{
    if (!operand)
        return std::nullopt;
    while (!index.isParameter(operand) && inputPassThroughOpcodes.contains(index.opcodeOf(operand)))
        operand = index.value(operand).operand(0);
    if (index.isParameter(operand)) {
        const Function& holder = walker.program().at(function);
        std::optional<uint32_t> position = index.blockOf(operand) == entryBlock ? index.block(entryBlock).indexOfParameter(operand) : std::nullopt;
        if (!position)
            return std::nullopt;
        if (*position < holder.captures.size()) {
            const HeldRegExp* held = walker.plan().heldRegExps().find(function, *position);
            return held ? std::optional<HeldRegExp>(*held) : std::nullopt;
        }
        std::optional<Walker::CallOfCopy> copied = walker.callOfCopy(framesOutward);
        return copied ? findRegExpInBody(walker, copied->caller, *copied->index, findWrittenArgument(copied->index->value(copied->call), holder, *position), framesOutward + 1) : std::nullopt;
    }
    if (framesOutward && index.opcodeOf(operand) == Opcode::NewRegExp) {
        auto made = index.value(operand).as<Opcode::NewRegExp>();
        return HeldRegExp { made.pattern(), made.regExpFlags() };
    }
    if (index.opcodeOf(operand) == Opcode::GetEnvironmentField) {
        auto read = index.value(operand).as<Opcode::GetEnvironmentField>();
        const HeldRegExp* held = walker.plan().heldRegExps().find(FieldReference { read.layout(), read.field() });
        return held ? std::optional<HeldRegExp>(*held) : std::nullopt;
    }
    if (index.opcodeOf(operand) == Opcode::PrivateGet) {
        auto read = index.value(operand).as<Opcode::PrivateGet>();
        const HeldRegExp* held = walker.plan().heldRegExps().find(FieldReference { read.layout(), read.field() });
        return held ? std::optional<HeldRegExp>(*held) : std::nullopt;
    }
    if (index.opcodeOf(operand) == Opcode::PropGet) {
        auto read = index.value(operand).as<Opcode::PropGet>();
        const HeldRegExp* expected = read.isFreeName() ? nullptr : walker.plan().heldRegExps().findExpected(read.key());
        return expected ? std::optional<HeldRegExp>(*expected) : std::nullopt;
    }
    if (index.opcodeOf(operand) != Opcode::GetModuleVariable)
        return std::nullopt;
    const VariableID variable = index.value(operand).as<Opcode::GetModuleVariable>().variable();
    if (const HeldRegExp* held = walker.plan().heldRegExps().find(variable))
        return *held;
    const DeferredVariable* deferred = walker.plan().deferredDefinitions().findDeferred(variable);
    if (!deferred || deferred->opcode != Opcode::NewRegExp)
        return std::nullopt;
    const BodyIndex* definition = walker.facts().bodies().find(deferred->body);
    RELEASE_ASSERT(definition);
    auto made = definition->value(deferred->made).as<Opcode::NewRegExp>();
    return HeldRegExp { made.pattern(), made.regExpFlags() };
}

std::optional<HeldRegExp> findHeldRegExp(const Walker& walker, ValueID operand)
{
    return findRegExpInBody(walker, walker.currentFunction(), walker.currentBodyIndex(), operand, 0);
}

std::optional<HeldRegExp> findRegExpPassed(const Walker& walker, ValueID operand)
{
    if (!operand)
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    const ValueID written = index.stripCasts(operand);
    if (index.isParameter(written) || index.opcodeOf(written) != Opcode::NewRegExp) {
        std::optional<HeldRegExp> held = findHeldRegExp(walker, operand);
        return held && !held->isExpected ? held : std::nullopt;
    }
    auto made = index.value(written).as<Opcode::NewRegExp>();
    return HeldRegExp { made.pattern(), made.regExpFlags() };
}

namespace {

enum class UseOfRegExpByRow : uint8_t {
    OnlyInspects,
    MatchesFromStart,
    MatchesFromLastIndex,
    Other,
};

} // anonymous namespace

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
    if (Reference pieces = buildPlacesReadOfMatch(walker, call, overload, regExp, objectOfCall, string, origin); pieces || walker.hasFailed() || walker.isUnreachable())
        return pieces;

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

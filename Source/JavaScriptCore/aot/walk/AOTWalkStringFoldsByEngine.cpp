/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Folding with BuiltinCalls: a built-in whose answer depends on its operands alone is asked in a realm of the build's own where its operands are known primitives and RegExps that are written for the call.
// At a suspension and at a throw: nothing. Where the built-in throws the realm gives no answer, and the call goes by its row. Withheld, or without a realm: the call goes by its row.
// THE BUILD STAYS DETERMINISTIC: no row that reads or writes ExternalState is asked, a RegExp is made anew for each question, and nothing that a question leaves in the realm changes a later answer.

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
#include "AOTRegExpFlags.h"
#include "AOTRuntimeHooks.h"
#include "AOTScratchRealm.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include "Collector.h"
#include "JSCInlines.h"
#include "JSGlobalObjectFunctions.h"
#include "JSLock.h"
#include "Options.h"
#include "PureNaN.h"
#include "RegExpObject.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Scope.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

// ---- The realm of the build

ScratchRealm::ScratchRealm() = default;

ScratchRealm::~ScratchRealm()
{
    if (!m_vm)
        return;
    m_realm.clear();
    // The lock holder is left with the last reference, so the VM dies under its lock, and its collector with it.
    m_vm = nullptr;
    m_lockOfWholeLife.reset();
    m_noHelperThreads.reset();
}

// IT STARTS NO THREAD, as the rehearsal of RegExp's texts starts none (facts/AOTFactsRegExpTexts.cpp): a build starts the threads that it counts, and says so where it is refused one.
// Bytecode stays in the interpreter, so a built-in answers out of one tier. A worker makes, asks and destroys its realm on one thread, which holds the lock as long as the VM lives:
// a lock that is dropped in the middle of a collection would hand that on to a thread of the collector's.
void ScratchRealm::makeAtFirstQuestion()
{
    if (m_vm)
        return;
    m_noHelperThreads.emplace();
    m_vm = VM::create(HeapType::Small);
    m_vm->heap.collector().willCollectWithoutHelpers();
    m_lockOfWholeLife.emplace(*m_vm);
    m_realm.set(*m_vm, JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())));
}

double ScratchRealm::bytesOfAnswerSetByNumber(std::optional<InlineBuiltin> codeInLine, uint64_t bytesOfOperands, double number)
{
    if (!(number > 1))
        return static_cast<double>(bytesOfOperands);
    if (codeInLine == InlineBuiltin::StringRepeat)
        return static_cast<double>(bytesOfOperands) * number;
    if (codeInLine == InlineBuiltin::StringPadStart || codeInLine == InlineBuiltin::StringPadEnd)
        return std::max(static_cast<double>(bytesOfOperands), number * sizeof(char16_t));
    return static_cast<double>(bytesOfOperands);
}

JSValue ScratchRealm::toJSValue(const TypeArena& types, const Operand& operand)
{
    VM& vm = *m_vm;
    auto charactersOf = [&](StringID text) {
        Vector<char16_t> buffer;
        String characters = types.viewOf(text, buffer).toString();
        return characters.is8Bit() || !characters.containsOnlyLatin1() ? characters : String::make8Bit(characters.span16());
    };
    if (operand.regExp) {
        RegExp* regExp = RegExp::create(vm, charactersOf(operand.regExp->pattern), toYarrFlags(operand.regExp->flags));
        if (!regExp->isValid())
            return { };
        return RegExpObject::create(vm, m_realm->regExpStructure(), regExp);
    }

    PrimitiveKnownToFolding primitive = operand.primitive;
    switch (primitive.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        return jsUndefined();
    case PrimitiveKnownToFolding::Kind::Null:
        return jsNull();
    case PrimitiveKnownToFolding::Kind::Boolean:
        return jsBoolean(primitive.asBoolean());
    case PrimitiveKnownToFolding::Kind::Number:
        return jsNumber(primitive.asNumber());
    case PrimitiveKnownToFolding::Kind::Text:
        return jsString(vm, charactersOf(primitive.asText()));
    case PrimitiveKnownToFolding::Kind::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<ScratchRealm::Answer> ScratchRealm::call(const TypeArena& types, BuiltinID function, const Operand& receiver, std::span<const Operand> arguments)
{
    makeAtFirstQuestion();
    VM& vm = *m_vm;
    JSLockHolder locker(vm);
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto clearException = makeScopeExit([&] {
        scope.clearException();
    });

    auto callee = findInRealm(realm, BuiltinCellKey::object(function));
    if (!callee || !*callee)
        return std::nullopt;
    CallData callData = getCallData(*callee);
    if (callData.type == CallData::Type::None)
        return std::nullopt;

    MarkedArgumentBuffer list;
    for (const Operand& operand : arguments) {
        JSValue argument = toJSValue(types, operand);
        if (!argument)
            return std::nullopt;
        list.append(argument);
    }
    if (list.hasOverflowed())
        return std::nullopt;
    JSValue thisValue = toJSValue(types, receiver);
    if (!thisValue)
        return std::nullopt;

    NakedPtr<Exception> exception;
    JSValue result = JSC::call(realm, *callee, callData, thisValue, list, exception);
    if (exception)
        return std::nullopt;

    if (result.isString()) {
        // Asked of the string as the function hands it over: opening a short piece of a 16-bit string copies it into 8 bits where it fits.
        bool isHeldIn8Bits = asString(result)->is8Bit();
        bool mayHaveNarrowParts = !isHeldIn8Bits && asString(result)->isRope() && !asString(result)->isSubstring();
        auto text = asString(result)->value(realm);
        if (scope.exception())
            return std::nullopt;
        return Answer { { }, text->isolatedCopy(), !mayHaveNarrowParts && (isHeldIn8Bits || !text->containsOnlyLatin1()) };
    }
    if (result.isUndefined())
        return Answer { PrimitiveKnownToFolding::undefined(), { } };
    if (result.isNull())
        return Answer { PrimitiveKnownToFolding::null(), { } };
    if (result.isBoolean())
        return Answer { PrimitiveKnownToFolding::boolean(result.asBoolean()), { } };
    if (result.isNumber())
        return Answer { PrimitiveKnownToFolding::number(result.asNumber()), { } };
    return std::nullopt;
}

// Many built-in functions of a realm are bytecode. A collector that all VMs share waits for every thread that holds a lock, and a worker holds that of its realm while it compiles.
ScratchRealm* scratchRealmOfWalk(Walker& walker)
{
    return Options::crashIfInterpreterIsUsed() || Options::useGlobalGC() ? nullptr : walker.scratchRealm();
}

// ---- Whatever depends on its operands alone: the engine answers

// What the row does with a RegExp that it is handed is to read pattern and flags, or to match with it. As useOfRegExpByRowInWalkRegExps() of AOTWalkRegExps.cpp.
// AppliesToContents: the protocols of the row run on what lastIndex holds, which in a RegExp that is as it was made is a number.
static bool onlyMatchesWithRegExpInFoldsByEngine(const BuiltinTable& table, const BuiltinOverload& row, BuiltinOperand operand)
{
    constexpr OperandUses usesOfContents { OperandUse::ReadsContents, OperandUse::WritesContents };
    constexpr OperandUses usesOfMatching { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::AppliesToContents };

    const BuiltinParameter* parameter = table.parameterFor(row, operand);
    if (!parameter)
        return false;
    if (!parameter->uses.containsAny(usesOfContents))
        return parameter->protocols.isEmpty() && parameter->uses.containsOnly({ OperandUse::Inspects });
    if (!parameter->uses.containsOnly(usesOfMatching) || !parameter->protocols.containsOnly({ Protocol::ToPrimitive }) || !row.code.implementation.inlineBuiltin)
        return false;

    switch (*row.code.implementation.inlineBuiltin) {
    case InlineBuiltin::RegExpExec:
    case InlineBuiltin::RegExpTest:
    case InlineBuiltin::StringMatch:
    case InlineBuiltin::StringReplace:
    case InlineBuiltin::StringReplaceAll:
        return true;
    default:
        return false;
    }
}

// The RegExp is made for this call alone, and the call meets each RegExp that is made there once: what a match stores in it nobody ever reads.
static std::optional<ScratchRealm::RegExpLiteral> findRegExpOperandInFoldsByEngine(Walker& walker, const CallByOneRow& call, BuiltinOperand operand, ValueID written)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!written || !table.mayHavePristineInstances(BuiltinClasses::RegExp) || !onlyMatchesWithRegExpInFoldsByEngine(table, table.at(call.overload), operand))
        return std::nullopt;
    std::optional<RegExpWrittenForCall> regExp = findRegExpWrittenForCall(walker, call.call, written);
    if (!regExp)
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID made = index.valueBehindCasts(written);
    LoopID loopOfCall = index.loopOf(index.blockOf(call.call.id()));
    if (!made || (loopOfCall && !index.isInLoop(index.blockOf(made), loopOfCall)))
        return std::nullopt;
    return ScratchRealm::RegExpLiteral { regExp->pattern, regExp->flags };
}

// A pattern may be as long as a pattern that Image still compiles.
static bool isTooLongToBeAnsweredInFoldsByEngine(Walker& walker, const CallByOneRow& call, double bytes)
{
    if (bytes <= mostBytesOfTextMadeByStringFolds)
        return false;
    return !textFeedsOnlyPatternOfRegExp(walker, call.call.id()) || bytes > static_cast<double>(mostBytesOfPatternMadeByStringFolds);
}

// Unknown: the text is too long to be a constant.
static PrimitiveKnownToFolding textAnsweredInFoldsByEngine(Walker& walker, const CallByOneRow& call, PrimitiveKnownToFolding receiver, const String& text)
{
    if (text.isEmpty())
        return PrimitiveKnownToFolding::text(walker.program().strings.emptyString());
    Vector<uint8_t> bytes = encodeProgramString(StringView(text));
    if (isTooLongToBeAnsweredInFoldsByEngine(walker, call, static_cast<double>(bytes.size())))
        return { };
    TypeArena& types = walker.types();
    // Under the id of the receiver it has the type of a literal.
    if (receiver.isText() && std::ranges::equal(types.bytesOf(receiver.asText()), bytes))
        return receiver;
    return PrimitiveKnownToFolding::text(types.internText(bytes.span()));
}

// Beyond ASCII these ask ICU. On Darwin the ICU that runs the program is the system's, which is not the one that builds it.
static bool asksICUOfAnotherVersionInFoldsByEngine(const TypeArena& types, std::optional<InlineBuiltin> codeInLine, StringID receiver)
{
    if (targetOfThisBuild().platform != BuiltinPlatform::Darwin)
        return false;
    if (codeInLine != InlineBuiltin::StringToLowerCase && codeInLine != InlineBuiltin::StringToUpperCase && codeInLine != InlineBuiltin::StringNormalize)
        return false;
    return std::ranges::any_of(types.bytesOf(receiver), [](uint8_t byte) {
        return byte >= 0x80;
    });
}

// How long what replace() and replaceAll() answer can be: asked before the answer is made. Every match is replaced, there is at most one at each place of the subject,
// and "$&", "$`", "$'" and "$1" in the replacement each stand for a piece of the subject.
static double bytesOfAnswerOfReplacementInFoldsByEngine(const TypeArena& types, StringID subject, std::span<const ScratchRealm::Operand> arguments)
{
    // No other primitive is spelled with more.
    double bytesOfReplacement = 32;
    bool namesPieces = false;
    if (arguments.size() > 1 && arguments[1].primitive.isText()) {
        std::span<const uint8_t> replacement = types.bytesOf(arguments[1].primitive.asText());
        bytesOfReplacement = static_cast<double>(replacement.size());
        namesPieces = std::ranges::find(replacement, static_cast<uint8_t>('$')) != replacement.end();
    }
    double bytesOfSubject = static_cast<double>(types.bytesOf(subject).size());
    return bytesOfSubject + (bytesOfSubject + 1) * (namesPieces ? bytesOfReplacement * (bytesOfSubject + 1) : bytesOfReplacement);
}

bool foldCallAnsweredByEngine(Walker& walker, const CallByOneRow& call)
{
    if (call.call.opcode() != Opcode::Call || call.mode != BuiltinCallMode::Call || !call.receiver)
        return false;
    TypeArena& types = walker.types();
    const BuiltinOverload& row = walker.program().builtins().at(call.overload);
    if (row.effects.reads.contains(AbstractHeap::ExternalState) || row.effects.writes.contains(AbstractHeap::ExternalState))
        return false;

    uint64_t bytes = 0;
    Vector<BuiltinOperand, 2> regExps;
    auto describe = [&](Reference built, BuiltinOperand position, ValueID written) -> std::optional<ScratchRealm::Operand> {
        if (PrimitiveKnownToFolding primitive = built ? primitiveKnownForStrings(walker, built) : PrimitiveKnownToFolding()) {
            if (primitive.isText())
                bytes += types.bytesOf(primitive.asText()).size();
            return ScratchRealm::Operand { primitive };
        }
        std::optional<ScratchRealm::RegExpLiteral> regExp = findRegExpOperandInFoldsByEngine(walker, call, position, written);
        if (!regExp)
            return std::nullopt;
        bytes += types.bytesOf(regExp->pattern).size();
        regExps.append(position);
        return ScratchRealm::Operand { { }, *regExp };
    };

    std::optional<ScratchRealm::Operand> receiver = describe(call.receiver, BuiltinOperand::receiver(), call.call.thisValueOperand());
    if (!receiver || (!receiver->regExp && !receiver->primitive.isText()))
        return false;
    std::span<const ValueID> written = call.call.argumentsOperands();
    double firstArgument = 1;
    Vector<ScratchRealm::Operand, 2> arguments;
    for (unsigned index = 0; index < call.arguments.size(); ++index) {
        if (index > BuiltinOperand::maxArgumentIndex)
            return false;
        std::optional<ScratchRealm::Operand> argument = describe(call.arguments[index], BuiltinOperand::argument(index), index < written.size() ? written[index] : ValueID());
        if (!argument)
            return false;
        if (!index && argument->primitive.kind() == PrimitiveKnownToFolding::Kind::Number)
            firstArgument = argument->primitive.asNumber();
        arguments.append(*argument);
    }

    // A throw is not counted: the engine gives no answer where the built-in throws.
    std::optional<Effects> effects = effectsOfRowOnOperandsWithoutCode(walker, call, regExps.span());
    if (!effects || !effects->alwaysCompletes() || !effects->writes.containsOnly(heapsWhoseWritesNobodySees | AbstractHeapSet { AbstractHeap::RegExpState }))
        return false;
    // The RegExp of the call is seen by nobody, so what the last match leaves in the constructor is the one trace of a match.
    if (!effects->writes.containsOnly(heapsWhoseWritesNobodySees) && walker.facts().mayReadLegacyRegExpStatics())
        return false;
    // The rows of a built-in differ in how they take their operands, not in what it answers: a row that calls the host is bounded as the row beside it that has code in line.
    std::optional<InlineBuiltin> method = call.codeInLine;
    for (const BuiltinOverload& other : walker.program().builtins().overloadsOf(call.builtin)) {
        if (!method)
            method = other.code.implementation.inlineBuiltin;
    }
    // Never less than the bytes of the operands.
    if (isTooLongToBeAnsweredInFoldsByEngine(walker, call, ScratchRealm::bytesOfAnswerSetByNumber(method, bytes, firstArgument)))
        return false;
    if (receiver->primitive.isText()) {
        StringID subject = receiver->primitive.asText();
        if (asksICUOfAnotherVersionInFoldsByEngine(types, method, subject))
            return false;
        bool replaces = method == InlineBuiltin::StringReplace || method == InlineBuiltin::StringReplaceAll;
        if (replaces && isTooLongToBeAnsweredInFoldsByEngine(walker, call, bytesOfAnswerOfReplacementInFoldsByEngine(types, subject, arguments.span())))
            return false;
    }

    ScratchRealm* realm = scratchRealmOfWalk(walker);
    std::optional<ScratchRealm::Answer> answer = realm ? realm->call(types, call.builtin, *receiver, arguments.span()) : std::nullopt;
    if (!answer)
        return false;
    PrimitiveKnownToFolding known = answer->primitive;
    if (!answer->text.isNull()) {
        // How the answer is held follows how the operands are, and the realm was handed literals. A constant is held as a literal is.
        if (!answer->isStoredLikeLiteral)
            return false;
        known = textAnsweredInFoldsByEngine(walker, call, receiver->primitive, answer->text);
    }
    return bindAnswerOfFoldedCall(walker, call, known);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

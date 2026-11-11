/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
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
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Scope.h>
#include <wtf/StackPointer.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

ScratchRealm::ScratchRealm() = default;

ScratchRealm::~ScratchRealm()
{
    if (!m_vm)
        return;
    m_realm.clear();
    m_vm = nullptr;
    m_lockOfWholeLife.reset();
    m_noHelperThreads.reset();
}

void ScratchRealm::makeAtFirstQuestion()
{
    if (m_vm) {
        m_vm->setStackPointerAtVMEntry(currentStackPointer());
        return;
    }
    m_noHelperThreads.emplace();
    m_vm = VM::create(HeapType::Small);
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
    for (const Operand& operand : arguments)
        list.append(toJSValue(types, operand));
    if (list.hasOverflowed())
        return std::nullopt;
    JSValue thisValue = toJSValue(types, receiver);

    NakedPtr<Exception> exception;
    JSValue result = JSC::call(realm, *callee, callData, thisValue, list, exception);
    if (exception)
        return std::nullopt;

    if (result.isString()) {
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

ScratchRealm* scratchRealmOfWalk(Walker& walker)
{
    return Options::crashIfInterpreterIsUsed() || Options::useGlobalGC() ? nullptr : walker.scratchRealm();
}

static bool isTooLongToBeAnsweredInFoldsByEngine(Walker& walker, const CallByOneRow& call, double bytes)
{
    if (bytes <= mostBytesOfTextMadeByStringFolds)
        return false;
    return !textFeedsOnlyPatternOfRegExp(walker, call.call.id()) || bytes > static_cast<double>(mostBytesOfPatternMadeByStringFolds);
}

static PrimitiveKnownToFolding textAnsweredInFoldsByEngine(Walker& walker, const CallByOneRow& call, PrimitiveKnownToFolding receiver, const String& text)
{
    if (text.isEmpty())
        return PrimitiveKnownToFolding::text(walker.program().strings.emptyString());
    Vector<uint8_t> bytes = encodeProgramString(StringView(text));
    if (isTooLongToBeAnsweredInFoldsByEngine(walker, call, static_cast<double>(bytes.size())))
        return { };
    TypeArena& types = walker.types();
    if (std::ranges::equal(types.bytesOf(receiver.asText()), bytes))
        return receiver;
    return PrimitiveKnownToFolding::text(types.internText(bytes.span()));
}

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

static double bytesOfAnswerOfReplacementInFoldsByEngine(const TypeArena& types, StringID subject, std::span<const ScratchRealm::Operand> arguments)
{
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
    auto describe = [&](Reference built) -> std::optional<ScratchRealm::Operand> {
        if (PrimitiveKnownToFolding primitive = built ? primitiveKnownForStrings(walker, built) : PrimitiveKnownToFolding()) {
            if (primitive.isText())
                bytes += types.bytesOf(primitive.asText()).size();
            return ScratchRealm::Operand { primitive };
        }
        return std::nullopt;
    };

    std::optional<ScratchRealm::Operand> receiver = describe(call.receiver);
    if (!receiver || !receiver->primitive.isText())
        return false;
    double firstArgument = 1;
    Vector<ScratchRealm::Operand, 2> arguments;
    for (unsigned index = 0; index < call.arguments.size(); ++index) {
        if (index > BuiltinOperand::maxArgumentIndex)
            return false;
        std::optional<ScratchRealm::Operand> argument = describe(call.arguments[index]);
        if (!argument)
            return false;
        if (!index && argument->primitive.kind() == PrimitiveKnownToFolding::Kind::Number)
            firstArgument = argument->primitive.asNumber();
        arguments.append(*argument);
    }

    std::optional<Effects> effects = effectsOfRowOnOperandsWithoutCode(walker, call);
    if (!effects || !effects->alwaysCompletes() || !effects->writes.containsOnly(unobservableWriteHeaps | AbstractHeapSet { AbstractHeap::RegExpState }))
        return false;
    if (!effects->writes.containsOnly(unobservableWriteHeaps) && walker.facts().mayReadLegacyRegExpStatics())
        return false;
    std::optional<InlineBuiltin> method = call.codeInLine;
    for (const BuiltinOverload& other : walker.program().builtins().overloadsOf(call.builtin)) {
        if (!method)
            method = other.code.implementation.inlineBuiltin;
    }
    if (isTooLongToBeAnsweredInFoldsByEngine(walker, call, ScratchRealm::bytesOfAnswerSetByNumber(method, bytes, firstArgument)))
        return false;
    StringID subject = receiver->primitive.asText();
    if (asksICUOfAnotherVersionInFoldsByEngine(types, method, subject))
        return false;
    bool replaces = method == InlineBuiltin::StringReplace || method == InlineBuiltin::StringReplaceAll;
    if (replaces && isTooLongToBeAnsweredInFoldsByEngine(walker, call, bytesOfAnswerOfReplacementInFoldsByEngine(types, subject, arguments.span())))
        return false;

    ScratchRealm* realm = scratchRealmOfWalk(walker);
    std::optional<ScratchRealm::Answer> answer = realm ? realm->call(types, call.builtin, *receiver, arguments.span()) : std::nullopt;
    if (!answer)
        return false;
    PrimitiveKnownToFolding known = answer->primitive;
    if (!answer->text.isNull()) {
        if (!answer->isStoredLikeLiteral)
            return false;
        known = textAnsweredInFoldsByEngine(walker, call, receiver->primitive, answer->text);
    }
    return bindAnswerOfFoldedCall(walker, call, known);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

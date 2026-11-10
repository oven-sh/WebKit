/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTFormatPatternPlanner.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTScratchRealm.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include <optional>
#include <span>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<FormatterRequest> findFormatterMadeByCallInStringFormatters(const BuiltinTable& table, const CallByOneRow& call)
{
    using Required = IntlDateTimeFormat::RequiredComponent;
    using Defaults = IntlDateTimeFormat::Defaults;
    auto isCallOf = [&](BuiltinID named) {
        return table.contains(named) && call.builtin == table.canonicalID(named);
    };
    if (call.mode == BuiltinCallMode::Construct) {
        if (call.receiver)
            return std::nullopt;
        if (isCallOf(Builtins::IntlDateTimeFormat))
            return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { } };
        if (isCallOf(Builtins::IntlNumberFormat))
            return FormatterRequest { .kind = FormatPatternKind::Number, .locale = { } };
        return std::nullopt;
    }
    if (call.mode != BuiltinCallMode::Call || !call.receiver)
        return std::nullopt;
    if (isCallOf(Builtins::NumberPrototypeToLocaleString))
        return FormatterRequest { .kind = FormatPatternKind::Number, .locale = { } };
    if (isCallOf(Builtins::DatePrototypeToLocaleString))
        return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { }, .required = Required::Any, .defaults = Defaults::All };
    if (isCallOf(Builtins::DatePrototypeToLocaleDateString))
        return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { }, .required = Required::Date, .defaults = Defaults::Date };
    if (isCallOf(Builtins::DatePrototypeToLocaleTimeString))
        return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { }, .required = Required::Time, .defaults = Defaults::Time };
    return std::nullopt;
}

static String textInRequestOfStringFormatters(Walker& walker, StringID text)
{
    Vector<char16_t> buffer;
    return walker.types().viewOf(text, buffer).toString();
}

static bool addOptionInStringFormatters(Walker& walker, PropertyKeyID key, PrimitiveKnownToFolding value, FormatterRequest& request)
{
    const PropertyKey& name = walker.types().at(key);
    if (name.kind != PropertyKeyKind::Text || walker.program().nameClassOf(key) == NameClass::Proto)
        return false;
    FormatterOption option { textInRequestOfStringFormatters(walker, name.text), FormatterOptionKind::Number, { }, 0 };
    switch (value.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        return true;
    case PrimitiveKnownToFolding::Kind::Boolean:
        option.kind = FormatterOptionKind::Boolean;
        option.number = value.asBoolean();
        break;
    case PrimitiveKnownToFolding::Kind::Number:
        option.number = value.asNumber();
        break;
    case PrimitiveKnownToFolding::Kind::Text:
        option.kind = FormatterOptionKind::String;
        option.string = textInRequestOfStringFormatters(walker, value.asText());
        break;
    case PrimitiveKnownToFolding::Kind::Unknown:
    case PrimitiveKnownToFolding::Kind::Null:
        return false;
    }
    request.options.append(WTF::move(option));
    return true;
}

static bool addOptionsOfFreshLiteralInStringFormatters(Walker& walker, Reference literal, FormatterRequest& request)
{
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfFreshObject(walker, literal);
    if (!properties)
        return false;
    for (const PropertyOfFreshObject& property : *properties) {
        const Field& declared = fieldAt(walker.program(), property.field);
        if (!declared.key || !declared.form.isEmpty())
            return false;
        if (!addOptionInStringFormatters(walker, declared.key, primitiveKnownForStrings(walker, property.value), request))
            return false;
    }
    return true;
}

static PrimitiveKnownToFolding primitiveWrittenInDataForStringFormatters(const DataNode& node)
{
    if (node.is<DataNode::Undefined>())
        return PrimitiveKnownToFolding::undefined();
    if (const auto* boolean = node.tryAs<DataNode::Boolean>())
        return PrimitiveKnownToFolding::boolean(boolean->value);
    if (const auto* number = node.tryAs<DataNode::Number>())
        return PrimitiveKnownToFolding::number(number->value);
    if (const auto* string = node.tryAs<DataNode::String>())
        return PrimitiveKnownToFolding::text(string->value);
    return { };
}

static bool addOptionsOfDataThatCallAloneSeesInStringFormatters(Walker& walker, const CallByOneRow& call, FormatterRequest& request)
{
    if (call.call.opcode() != Opcode::Call && call.call.opcode() != Opcode::Construct)
        return false;
    std::span<const ValueID> written = call.writtenArguments();
    if (written.size() != 2)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID argument = written[1];
    ValueID definition = index.stripCasts(argument);
    if (!definition || index.isParameter(definition) || index.opcodeOf(definition) != Opcode::Data)
        return false;
    for (ValueID link = definition; link != argument;) {
        link = index.onlyUsingValue(link);
        if (!link || link.index() > argument.index())
            return false;
    }
    if (index.onlyUsingValue(argument) != call.call.id())
        return false;

    const auto* object = walker.program().at(index.value(definition).as<Opcode::Data>().data()).root.tryAs<DataNode::Object>();
    if (!object)
        return false;
    for (const DataMember& member : object->members) {
        if (!addOptionInStringFormatters(walker, member.key, primitiveWrittenInDataForStringFormatters(member.value), request))
            return false;
    }
    return true;
}

static bool findOptionsInStringFormatters(Walker& walker, const CallByOneRow& call, Reference options, FormatterRequest& request)
{
    request.hasOptions = true;
    if (!addOptionsOfFreshLiteralInStringFormatters(walker, options, request)) {
        request.options.shrink(0);
        if (!addOptionsOfDataThatCallAloneSeesInStringFormatters(walker, call, request))
            return false;
    }
    AOT::sort(request.options, [](const FormatterOption& first, const FormatterOption& second) {
        return codePointCompareLessThan(first.name, second.name);
    });
    for (size_t index = 1; index < request.options.size(); ++index) {
        if (request.options[index - 1].name == request.options[index].name)
            return false;
    }
    return true;
}

static void buildFormatterWithPatternInStringFormatters(Walker& walker, const CallByOneRow& call, TypeID typeOfFormatter, std::span<const uint32_t> pattern, Reference options)
{
    Draft allocation(Opcode::NewFormatterWithPattern, call.origin);
    walker.chooseConservativeFields(allocation);
    allocation.setValueType(typeOfFormatter);
    allocation.setFormatPattern(pattern);
    allocation.expected = call.expected;
    allocation.inputValueGiven = call.call.id();
    Reference formatter = walker.build(WTF::move(allocation));
    if (!formatter)
        return;
    if (options) {
        Draft setUp(Opcode::SetUpFormatterWithOptions, call.origin);
        walker.chooseConservativeFields(setUp);
        setUp.setFormatterOperand(formatter);
        setUp.setOptionsOperand(options);
        walker.build(WTF::move(setUp));
        if (walker.isUnreachable() || walker.hasFailed())
            return;
    }
    walker.bind(call.call.id(), formatter);
}

static void buildTextWrittenByPatternInStringFormatters(Walker& walker, const CallByOneRow& call, Opcode opcode, std::span<const uint32_t> pattern)
{
    static_assert(!OperandIndex::FormatDateWithPattern::date && !OperandIndex::FormatNumberWithPattern::number);
    Draft format(opcode, call.origin);
    walker.chooseConservativeFields(format);
    format.setOperand(0, call.receiver);
    format.setFormatPattern(pattern);
    Reference text = walker.build(WTF::move(format));
    if (!text)
        return;

    Draft test(Opcode::TypeTest, call.origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(text);
    test.setTestedType(TypeTable::string);
    Reference isWritten = walker.build(WTF::move(test));
    if (!isWritten)
        return;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = call.call.id();
    BlockID block = index.blockOf(written);
    uint32_t place = placeInBlockForStrings(index, written);
    auto takeText = [&](std::span<Reference> results) {
        results[0] = buildProofOfTypeTestForStrings(walker, text, isWritten, call.origin);
    };
    auto callAsWritten = [&](std::span<Reference> results) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block, place, place + 1);
        if (!walker.isUnreachable())
            results[0] = walker.referenceOf(written);
    };
    std::array<Reference, 1> met;
    if (buildArmsForStrings(walker, isWritten, BlockFrequency::Rare, ArmForStrings(takeText), ArmForStrings(callAsWritten), met))
        walker.bind(written, met[0]);
}

bool buildFormatterCallByPattern(Walker& walker, const CallByOneRow& call)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    std::optional<FormatterRequest> request = findFormatterMadeByCallInStringFormatters(table, call);
    if (!request)
        return false;
    if (!walker.opensGuesses() || table.at(call.overload).libraryFunction() || call.codeInLine)
        return false;

    Opcode opcode = !call.receiver ? Opcode::NewFormatterWithPattern : request->kind == FormatPatternKind::Number ? Opcode::FormatNumberWithPattern : Opcode::FormatDateWithPattern;
    if (call.receiver) {
        Type type = walker.types().at(walker.typeOf(call.receiver));
        if (type.isNever() || !(request->kind == FormatPatternKind::Number ? type.isNumber() : type.isObject()))
            return false;
        FunctionID function = walker.unit().function;
        if (call.call.opcode() != Opcode::Call || (!walker.isInLoop() && program.at(function).isModuleBody()))
            return false;
    }

    if (call.arguments.empty() || call.arguments.size() > 2)
        return false;
    StringID locale = textKnownForStrings(walker, call.arguments[0]);
    if (!locale)
        return false;
    request->locale = textInRequestOfStringFormatters(walker, locale);

    Reference options;
    if (call.arguments.size() == 2 && primitiveKnownForStrings(walker, call.arguments[1]).kind() != PrimitiveKnownToFolding::Kind::Undefined) {
        options = call.arguments[1];
        if (!walker.types().at(walker.typeOf(options)).isObject() || !findOptionsInStringFormatters(walker, call, options, *request))
            return false;
    }

    TypeID typeOfFormatter = call.receiver ? TypeID() : typeGivenByRowForStrings(walker, call);
    FormatPatternPlans* plans = walker.formatPatternPlans();
    ScratchRealm* realm = plans ? scratchRealmOfWalk(walker) : nullptr;
    if (!realm || (!call.receiver && (!typeOfFormatter || !walker.types().at(typeOfFormatter).isObject())))
        return false;
    const WTF::Vector<uint32_t>* pattern = plans->planFor(*realm, *request);
    if (!pattern)
        return false;

    if (!call.receiver)
        buildFormatterWithPatternInStringFormatters(walker, call, typeOfFormatter, pattern->span(), options);
    else
        buildTextWrittenByPatternInStringFormatters(walker, call, opcode, pattern->span());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

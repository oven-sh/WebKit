/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTTypedBuiltins.h"
#include "JSCInlines.h"
#include "RegExpInlines.h"
#include "YarrFlags.h"
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxConstantTextSize = MB;

static bool appendConstantText(const Program& program, const Function& function, ValueID id, Vector<uint8_t>& text)
{
    auto& value = function.at(id);
    switch (value.opcode) {
    case Opcode::String:
        text.append(program.string(value.string()));
        return text.size() <= maxConstantTextSize;
    case Opcode::Cast:
    case Opcode::ToString:
        return appendConstantText(program, function, value.child(0), text);
    case Opcode::Add:
        return appendConstantText(program, function, value.child(0), text) && appendConstantText(program, function, value.child(1), text);
    // Where the text does not say hot a Strcat is a call of a function that is nothing but that: see giveConcatenationsOneBody().
    case Opcode::CallDirect: {
        auto& callee = program.at(value.function());
        if (callee.blocks.size() != 1 || callee.blocks[0].values.size() != 1 || callee.blocks[0].terminator.kind != TerminatorKind::Return || callee.blocks[0].terminator.value != callee.blocks[0].values[0])
            return false;
        auto& body = callee.at(callee.blocks[0].values[0]);
        if (body.opcode != Opcode::Strcat || body.arguments.size() != value.arguments.size() || !std::ranges::equal(body.arguments, callee.blocks[0].parameters))
            return false;
        [[fallthrough]];
    }
    case Opcode::Strcat:
        for (ValueID argument : value.arguments) {
            if (!appendConstantText(program, function, argument, text))
                return false;
        }
        return true;
    default:
        return false;
    }
}

using PossibleFlags = Vector<OptionSet<Yarr::Flags>, 4>;

// `onlyFirst ? undefined : "g"`: whatever constant an edge passes. Where nothing is known: none, which is a guess.
static void appendPossibleFlags(const Program& program, const Function& function, ValueID id, PossibleFlags& all, unsigned joinsLeft)
{
    auto& value = function.at(id);
    Vector<uint8_t> text;
    if (value.opcode == Opcode::Undefined || appendConstantText(program, function, id, text)) {
        if (auto flags = Yarr::parseFlags(StringView { byteCast<Latin1Character>(text.span()) }); flags && !all.contains(*flags))
            all.append(*flags);
        return;
    }
    if (value.opcode == Opcode::Cast)
        return appendPossibleFlags(program, function, value.child(0), all, joinsLeft);
    if (value.opcode == Opcode::Parameter && joinsLeft) {
        for (unsigned joined = 1; joined < function.blocks.size(); ++joined) {
            size_t index = function.blocks[joined].parameters.find(id);
            if (index == notFound)
                continue;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    if (target.block.index() == joined && index < target.arguments.size())
                        appendPossibleFlags(program, function, target.arguments[index], all, joinsLeft - 1);
                }
            }
            return;
        }
    }
    if (!all.contains(OptionSet<Yarr::Flags> { }))
        all.append(OptionSet<Yarr::Flags> { });
}

// What the code of the modules makes of the texts that are written down in it, as far as the program alone says. marked builds its 42 expressions when it is loaded:
// `edit(/^(?:bull)/).replace(/bull/g, bullet).getRegex()`. This goes through that code with what it knows: primitives, expressions, and the closures, objects and arrays the code
// itself has made; everything else is unknown, and so is what is made of it. Whatever RegExp comes about with a known source and known flags is handed to the image.
// It proves nothing and changes nothing: see addRegExpLiterals(). So where it does not know which way a branch goes it guesses, it takes a call that throws for one that
// yields something unknown, and it stops when it has done enough. It goes by the program alone: the same program, the same image.
class EvaluatorOfTexts {
public:
    EvaluatorOfTexts(VM& vm, const Program& program, ImageBuilder& builder)
        : m_vm(vm)
        , m_program(program)
        , m_builtins(Builtins::singleton())
        , m_builder(builder)
    {
    }

    void run()
    {
        m_variables.grow(m_program.variables.size());
        for (unsigned i = 0; i < m_variables.size(); ++i) {
            if (m_program.variables[i].startsUndefined)
                m_variables[i] = Known::undefined();
        }
        for (FunctionID entry : m_program.entries)
            invoke(entry, { });
        // What that has not come to, each by itself: what other modules have made is unknown then.
        unsigned stepsLeftForAll = maxNumberOfSteps * 8;
        for (unsigned i = 0; i < m_program.functions.size() && stepsLeftForAll; ++i) {
            if (m_program.functions[i].kind != FunctionKind::Module || m_wasEntered.get(i))
                continue;
            m_stepsLeft = std::min(stepsLeftForAll, maxNumberOfSteps / 8);
            stepsLeftForAll -= m_stepsLeft;
            invoke(FunctionID(i), { });
        }
        // What the modules have made and not called is called later, with what is not known here: `(indent) => new RegExp(`^ {0,${Math.min(3, indent - 1)}}#`)`.
        for (unsigned i = 0; i < m_records.size() && stepsLeftForAll; ++i) {
            if (m_records[i]->kind != Record::Kind::Closure || m_records[i]->wasCalled)
                continue;
            m_stepsLeft = std::min(stepsLeftForAll, maxNumberOfSteps / 64);
            stepsLeftForAll -= m_stepsLeft;
            Vector<Known, 8> unknown(m_program.at(m_records[i]->function).parameterTypes.size());
            call(Known::of(m_records[i].get()), { }, unknown.span());
        }
    }

private:
    static constexpr unsigned maxNumberOfSteps = 3000000;
    static constexpr unsigned maxDepth = 48;
    static constexpr unsigned maxNumberOfRecords = 200000;
    static constexpr unsigned maxNumberOfGuesses = 256;
    static constexpr unsigned maxNumberOfChoices = 8;
    // `for (..) list.push(new RegExp("x" + i))`: the image is not to grow without bound for what may never run.
    static constexpr unsigned maxNumberOfExpressions = 1024;
    static constexpr unsigned maxSizeToGoBothWays = 256;
    static constexpr unsigned maxNumberOfBranchesGoneBothWays = 3;

    struct Record;
    struct Known {
        enum class Kind : uint8_t { Nothing, Undefined, Null, Boolean, Number, Text, RegExp, Record };
        Kind kind { Kind::Nothing };
        bool boolean { false };
        double number { 0 };
        // Of a RegExp: what it was made from.
        String text;
        String flags;
        Record* record { nullptr };

        explicit operator bool() const { return kind != Kind::Nothing; }
        bool isText() const { return kind == Kind::Text; }
        bool isUndefined() const { return kind == Kind::Undefined; }
        static Known undefined() { return of(Kind::Undefined); }
        static Known of(Kind kind)
        {
            Known result;
            result.kind = kind;
            return result;
        }
        static Known of(bool boolean)
        {
            Known result = of(Kind::Boolean);
            result.boolean = boolean;
            return result;
        }
        static Known of(double number)
        {
            Known result = of(Kind::Number);
            result.number = number;
            return result;
        }
        static Known of(String&& text)
        {
            if (text.isNull() || text.length() > maxConstantTextSize)
                return { };
            Known result = of(Kind::Text);
            result.text = WTF::move(text);
            return result;
        }
        static Known of(Record* record)
        {
            if (!record)
                return { };
            Known result = of(Kind::Record);
            result.record = record;
            return result;
        }
        static Known of(std::optional<bool> boolean) { return boolean ? of(*boolean) : Known(); }
        static Known of(std::optional<String>&& text) { return text ? of(WTF::move(*text)) : Known(); }
    };
    using Values = Vector<Known>;

    struct Record {
        // Choice: one of a few primitives, it is not known which: `Math.min(3, indent - 1)`. It is no value of the program's.
        enum class Kind : uint8_t { Object, Array, Set, Closure, Choice };
        Kind kind { Kind::Object };
        FunctionID function;
        bool wasCalled { false };
        // Of an array or a Set: the elements. Of a closure: what it captures.
        Values elements;
        // Also the variables of an environment.
        UncheckedKeyHashMap<String, Known> properties;
    };

    Record* make(Record::Kind kind)
    {
        if (m_records.size() >= maxNumberOfRecords)
            return nullptr;
        m_records.append(makeUniqueWithoutFastMallocCheck<Record>());
        m_records.last()->kind = kind;
        return m_records.last().get();
    }

    String textOf(StringID id) const { return String::fromUTF8(m_program.string(id)); }

    static std::optional<String> toText(const Known& value)
    {
        switch (value.kind) {
        case Known::Kind::Undefined:
            return String("undefined"_s);
        case Known::Kind::Null:
            return String("null"_s);
        case Known::Kind::Boolean:
            return String(value.boolean ? "true"_s : "false"_s);
        case Known::Kind::Number:
            return String::number(value.number);
        case Known::Kind::Text:
            return value.text;
        default:
            return std::nullopt;
        }
    }

    static bool isChoice(const Known& value) { return value.kind == Known::Kind::Record && value.record->kind == Record::Kind::Choice; }

    // The parts in a row, for every choice there is among them.
    Known concatenate(std::span<const Known> parts)
    {
        Vector<String, maxNumberOfChoices> all { emptyString() };
        for (auto& part : parts) {
            Vector<String, maxNumberOfChoices> texts;
            for (auto& one : isChoice(part) ? part.record->elements.span() : std::span { &part, 1 }) {
                auto text = toText(one);
                if (!text)
                    return { };
                texts.append(WTF::move(*text));
            }
            if (all.size() * texts.size() > maxNumberOfChoices)
                return { };
            Vector<String, maxNumberOfChoices> next;
            for (auto& head : all) {
                for (auto& tail : texts) {
                    if (head.length() + tail.length() > maxConstantTextSize)
                        return { };
                    next.append(makeString(head, tail));
                }
            }
            all = WTF::move(next);
        }
        if (all.size() == 1)
            return Known::of(WTF::move(all[0]));
        Record* choice = make(Record::Kind::Choice);
        if (!choice)
            return { };
        for (auto& text : all)
            choice->elements.append(Known::of(WTF::move(text)));
        return Known::of(choice);
    }

    static std::optional<bool> toBoolean(const Known& value)
    {
        switch (value.kind) {
        case Known::Kind::Nothing:
            return std::nullopt;
        case Known::Kind::Undefined:
        case Known::Kind::Null:
            return false;
        case Known::Kind::Boolean:
            return value.boolean;
        case Known::Kind::Number:
            return value.number && !std::isnan(value.number);
        case Known::Kind::Text:
            return !value.text.isEmpty();
        case Known::Kind::Record:
            if (isChoice(value))
                return std::nullopt;
            [[fallthrough]];
        case Known::Kind::RegExp:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static std::optional<TypeofType> typeOf(const Known& value)
    {
        switch (value.kind) {
        case Known::Kind::Nothing:
            return std::nullopt;
        case Known::Kind::Undefined:
            return TypeofType::Undefined;
        case Known::Kind::Boolean:
            return TypeofType::Boolean;
        case Known::Kind::Number:
            return TypeofType::Number;
        case Known::Kind::Text:
            return TypeofType::String;
        case Known::Kind::Null:
        case Known::Kind::RegExp:
            return TypeofType::Object;
        case Known::Kind::Record:
            if (isChoice(value))
                return std::nullopt;
            return value.record->kind == Record::Kind::Closure ? TypeofType::Function : TypeofType::Object;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static std::optional<bool> isStrictlyEqual(const Known& a, const Known& b)
    {
        if (!a || !b || isChoice(a) || isChoice(b))
            return std::nullopt;
        if (a.kind != b.kind)
            return false;
        switch (a.kind) {
        case Known::Kind::Undefined:
        case Known::Kind::Null:
            return true;
        case Known::Kind::Boolean:
            return a.boolean == b.boolean;
        case Known::Kind::Number:
            return a.number == b.number;
        case Known::Kind::Text:
            return a.text == b.text;
        case Known::Kind::Record:
            return a.record == b.record;
        // Which object it is is not kept.
        default:
            return std::nullopt;
        }
    }

    // As RegExp.prototype.source has it: see regExpProtoGetterSourceInternal().
    static String sourceOf(const Known& regExp)
    {
        if (regExp.text.isEmpty())
            return "(?:)"_s;
        StringBuilder result;
        bool isEscaped = false;
        bool isInClass = false;
        for (unsigned i = 0; i < regExp.text.length(); ++i) {
            char16_t character = regExp.text[i];
            if (!isEscaped) {
                if (isInClass)
                    isInClass = character != ']';
                else if (character == '/')
                    result.append('\\');
                else
                    isInClass = character == '[';
            }
            if (character == '\n' || character == '\r' || character == 0x2028 || character == 0x2029) {
                if (!isEscaped)
                    result.append('\\');
                result.append(character == '\n' ? "n"_s : character == '\r' ? "r"_s : character == 0x2028 ? "u2028"_s : "u2029"_s);
            } else
                result.append(character);
            isEscaped = !isEscaped && character == '\\';
        }
        return result.toString();
    }

    Known makeRegExp(std::span<const Known> arguments)
    {
        Known result = Known::of(Known::Kind::RegExp);
        const Known source = arguments.size() ? arguments[0] : Known::undefined();
        const Known flags = arguments.size() > 1 ? arguments[1] : Known::undefined();
        if (isChoice(source)) {
            for (auto& one : source.record->elements)
                makeRegExp(std::array { one, flags });
            return { };
        }
        if (source.kind == Known::Kind::RegExp) {
            result.text = source.text;
            result.flags = source.flags;
        } else if (source.isUndefined())
            result.text = emptyString();
        else if (auto text = toText(source))
            result.text = WTF::move(*text);
        else
            return { };
        if (result.flags.isNull())
            result.flags = emptyString();
        if (!flags.isUndefined()) {
            auto text = toText(flags);
            // As appendPossibleFlags() guesses.
            if (!text) {
                handOver(result.text, { });
                return { };
            }
            result.flags = WTF::move(*text);
        }
        auto parsed = Yarr::parseFlags(result.flags);
        if (!parsed)
            return { };
        handOver(result.text, *parsed);
        return result;
    }

    void handOver(const String& source, OptionSet<Yarr::Flags> flags)
    {
        if (m_numberOfExpressions == maxNumberOfExpressions || !m_handedOver.add(makeString(flags.toRaw(), '/', source)).isNewEntry)
            return;
        ++m_numberOfExpressions;
        m_builder.addRegExp(m_vm, source, flags);
    }

    // False: there is something in the replacement that this does not know.
    static bool appendReplacement(StringBuilder& result, StringView replacement, StringView subject, std::span<const int> offsets)
    {
        unsigned numberOfGroups = offsets.size() / 2;
        for (unsigned i = 0; i < replacement.length(); ++i) {
            char16_t character = replacement[i];
            if (character != '$' || i + 1 == replacement.length()) {
                result.append(character);
                continue;
            }
            char16_t next = replacement[i + 1];
            if (next == '<')
                return false;
            if (next == '$')
                result.append('$');
            else if (next == '&')
                result.append(subject.substring(offsets[0], offsets[1] - offsets[0]));
            else if (next == '`')
                result.append(subject.left(offsets[0]));
            else if (next == '\'')
                result.append(subject.substring(offsets[1]));
            else if (isASCIIDigit(next)) {
                unsigned group = next - '0';
                if (i + 2 < replacement.length() && isASCIIDigit(replacement[i + 2]) && group * 10 + (replacement[i + 2] - '0') < numberOfGroups && group * 10 + (replacement[i + 2] - '0')) {
                    group = group * 10 + (replacement[i + 2] - '0');
                    ++i;
                }
                if (!group || group >= numberOfGroups) {
                    result.append('$');
                    continue;
                }
                if (offsets[2 * group] >= 0)
                    result.append(subject.substring(offsets[2 * group], offsets[2 * group + 1] - offsets[2 * group]));
            } else {
                result.append('$');
                continue;
            }
            ++i;
        }
        return true;
    }

    std::optional<String> replace(const String& subject, const Known& pattern, const String& replacement, bool isReplaceAll)
    {
        StringBuilder result;
        unsigned copiedUpTo = 0;
        auto replaceAt = [&](std::span<const int> offsets) {
            result.append(StringView(subject).substring(copiedUpTo, offsets[0] - copiedUpTo));
            copiedUpTo = offsets[1];
            return appendReplacement(result, replacement, subject, offsets);
        };
        if (pattern.kind == Known::Kind::RegExp) {
            auto flags = Yarr::parseFlags(pattern.flags);
            if (!flags || flags->contains(Yarr::Flags::Sticky) || (isReplaceAll && !flags->contains(Yarr::Flags::Global)))
                return std::nullopt;
            RegExp* regExp = RegExp::create(m_vm, pattern.text, *flags);
            if (!regExp->isValid() || regExp->hasNamedCaptures())
                return std::nullopt;
            for (unsigned start = 0; start <= subject.length();) {
                if (!m_stepsLeft)
                    return std::nullopt;
                --m_stepsLeft;
                if (regExp->matchInline(nullptr, m_vm, subject, start, regExp->ovectorSpan()) < 0)
                    break;
                auto offsets = regExp->ovectorSpan().first((regExp->numSubpatterns() + 1) * 2);
                if (offsets[0] < 0 || offsets[1] < offsets[0] || !replaceAt(offsets))
                    return std::nullopt;
                if (!flags->contains(Yarr::Flags::Global))
                    break;
                start = offsets[1] + (offsets[1] == offsets[0]);
            }
        } else {
            auto searched = toText(pattern);
            if (!searched)
                return std::nullopt;
            for (size_t start = 0; start <= subject.length();) {
                size_t found = subject.find(*searched, start);
                if (found == notFound)
                    break;
                std::array<int, 2> offsets { static_cast<int>(found), static_cast<int>(found + searched->length()) };
                if (!replaceAt(offsets))
                    return std::nullopt;
                if (!isReplaceAll)
                    break;
                start = offsets[1] + searched->isEmpty();
            }
        }
        result.append(StringView(subject).substring(copiedUpTo));
        if (result.hasOverflowed() || result.length() > maxConstantTextSize)
            return std::nullopt;
        return result.toString();
    }

    Known callMethodOfText(StringView name, const String& receiver, std::span<const Known> arguments)
    {
        auto argument = [&](unsigned index) {
            return index < arguments.size() ? arguments[index] : Known::undefined();
        };
        bool isReplaceAll = name == "replaceAll"_s;
        if (name == "replace"_s || isReplaceAll) {
            if (!argument(1).isText())
                return { };
            return Known::of(replace(receiver, argument(0), argument(1).text, isReplaceAll));
        }
        if (name == "concat"_s) {
            StringBuilder result;
            result.append(receiver);
            for (auto& part : arguments) {
                auto text = toText(part);
                if (!text)
                    return { };
                result.append(*text);
            }
            return result.hasOverflowed() ? Known() : Known::of(String(result.toString()));
        }
        if (name == "repeat"_s) {
            double count = argument(0).number;
            if (argument(0).kind != Known::Kind::Number || !(count >= 0) || count != std::trunc(count) || count * receiver.length() > maxConstantTextSize)
                return { };
            StringBuilder result;
            for (unsigned i = 0; i < count; ++i)
                result.append(receiver);
            return Known::of(String(result.toString()));
        }
        if (name == "toString"_s || name == "valueOf"_s)
            return Known::of(String(receiver));
        return { };
    }

    Known join(const Record& array, const Known& separator)
    {
        auto between = separator.isUndefined() ? std::optional<String>(String(","_s)) : toText(separator);
        if (!between)
            return { };
        StringBuilder result;
        for (unsigned i = 0; i < array.elements.size(); ++i) {
            auto& element = array.elements[i];
            if (i)
                result.append(*between);
            if (element.isUndefined() || element.kind == Known::Kind::Null)
                continue;
            auto text = toText(element);
            if (!text)
                return { };
            result.append(*text);
        }
        return result.hasOverflowed() ? Known() : Known::of(String(result.toString()));
    }

    Known getProperty(const Known& object, const String& name)
    {
        if (name.isNull())
            return { };
        switch (object.kind) {
        case Known::Kind::Text:
            return name == "length"_s ? Known::of(static_cast<double>(object.text.length())) : Known();
        case Known::Kind::RegExp:
            if (name == "source"_s)
                return Known::of(sourceOf(object));
            // In the order of the getter.
            if (name == "flags"_s && object.flags.length() < 2)
                return Known::of(String(object.flags));
            return { };
        case Known::Kind::Record:
            if (object.record->kind == Record::Kind::Array && name == "length"_s)
                return Known::of(static_cast<double>(object.record->elements.size()));
            if (object.record->kind == Record::Kind::Object)
                return object.record->properties.get(name);
            return { };
        default:
            return { };
        }
    }

    void putProperty(const Known& object, const String& name, const Known& value)
    {
        if (object.kind == Known::Kind::Record && object.record->kind == Record::Kind::Object && !name.isNull())
            object.record->properties.set(name, value);
    }

    String nameOfField(const Value& value) const
    {
        auto& fields = m_program.at(value.layout()).fields;
        if (value.field() >= fields.size() || !fields[value.field()].name)
            return { };
        return textOf(fields[value.field()].name);
    }

    Known call(const Known& callee, const Known& thisValue, std::span<const Known> arguments)
    {
        if (callee.kind != Known::Kind::Record || callee.record->kind != Record::Kind::Closure)
            return { };
        auto& function = m_program.at(callee.record->function);
        if (callee.record->elements.size() != function.captureTypes.size())
            return { };
        callee.record->wasCalled = true;
        Values all;
        all.appendVector(callee.record->elements);
        if (function.has(Format::FunctionHasThis))
            all.append(thisValue);
        all.append(arguments);
        return invoke(callee.record->function, WTF::move(all));
    }

    Known callMethod(const Known& receiver, const String& name, std::span<const Known> arguments)
    {
        if (receiver.isText())
            return callMethodOfText(name, receiver.text, arguments);
        if (receiver.kind != Known::Kind::Record)
            return { };
        if (receiver.record->kind == Record::Kind::Array && name == "join"_s)
            return join(*receiver.record, arguments.size() ? arguments[0] : Known::undefined());
        return call(getProperty(receiver, name), receiver, arguments);
    }

    Known callBuiltin(const Value& value, const Known& receiver, std::span<const Known> arguments)
    {
        auto& builtin = m_builtins.at(value.builtin());
        auto argument = [&](unsigned index) {
            return index < arguments.size() ? arguments[index] : Known::undefined();
        };
        switch (builtin.kind) {
        case BuiltinKind::Function:
        case BuiltinKind::Constructor:
            if (builtin.name == "RegExp"_s)
                return makeRegExp(arguments);
            if (builtin.name == "String"_s && builtin.kind == BuiltinKind::Function)
                return arguments.size() ? Known::of(toText(arguments[0])) : Known::of(String(emptyString()));
            if (builtin.name == "Set"_s && builtin.kind == BuiltinKind::Constructor) {
                Record* set = make(Record::Kind::Set);
                return set && appendSpread(*set, argument(0)) ? Known::of(set) : Known();
            }
            return { };
        case BuiltinKind::Method:
        case BuiltinKind::Getter:
            if (builtin.kind == BuiltinKind::Getter)
                return getProperty(receiver, String(builtin.name));
            if ((builtin.holder == "String"_s && receiver.isText()) || (builtin.holder == "Array"_s && builtin.name == "join"_s))
                return callMethod(receiver, String(builtin.name), arguments);
            return { };
        case BuiltinKind::StaticFunction:
            if (builtin.holder == "Math"_s && (builtin.name == "min"_s || builtin.name == "max"_s) && arguments.size() == 2 && arguments[0].kind == Known::Kind::Number && arguments[1].kind == Known::Kind::Number && !std::isnan(arguments[0].number) && !std::isnan(arguments[1].number))
                return Known::of(builtin.name == "min"_s ? std::min(arguments[0].number, arguments[1].number) : std::max(arguments[0].number, arguments[1].number));
            if (builtin.holder == "Math"_s && (builtin.name == "min"_s || builtin.name == "max"_s) && arguments.size() == 2 && (isChoice(arguments[0]) ? arguments[1] : arguments[0]).kind == Known::Kind::Number && (isChoice(arguments[0]) || isChoice(arguments[1]))) {
                auto& choice = isChoice(arguments[0]) ? arguments[0] : arguments[1];
                double other = (isChoice(arguments[0]) ? arguments[1] : arguments[0]).number;
                Record* result = make(Record::Kind::Choice);
                if (!result)
                    return { };
                for (auto& one : choice.record->elements) {
                    if (one.kind != Known::Kind::Number || std::isnan(other))
                        return { };
                    Known bounded = Known::of(builtin.name == "min"_s ? std::min(one.number, other) : std::max(one.number, other));
                    if (!result->elements.containsIf([&](auto& element) { return element.number == bounded.number; }))
                        result->elements.append(bounded);
                }
                return Known::of(result);
            }
            // A small bound on something unknown: as a rule a count.
            if (builtin.holder == "Math"_s && builtin.name == "min"_s && arguments.size() == 2 && !arguments[0] != !arguments[1]) {
                auto& bound = arguments[0] ? arguments[0] : arguments[1];
                Record* choice = bound.kind == Known::Kind::Number && bound.number >= 0 && bound.number < maxNumberOfChoices && bound.number == std::trunc(bound.number) ? make(Record::Kind::Choice) : nullptr;
                if (!choice)
                    return { };
                for (unsigned i = 0; i <= bound.number; ++i)
                    choice->elements.append(Known::of(static_cast<double>(i)));
                return Known::of(choice);
            }
            return { };
        case BuiltinKind::Operation:
            if (builtin.name == "DefineField"_s && arguments.size() == 3) {
                if (arguments[1].isText())
                    putProperty(arguments[0], arguments[1].text, arguments[2]);
                return arguments[0];
            }
            if (builtin.name == "AppendSpread"_s && arguments.size() == 2) {
                if (arguments[0].kind != Known::Kind::Record || arguments[0].record->kind != Record::Kind::Array || !appendSpread(*arguments[0].record, arguments[1]))
                    return { };
                return arguments[0];
            }
            if (builtin.name == "HasOriginalStructure"_s && arguments.size() == 1 && arguments[0] && value.engineClass() < m_builtins.classes().size()) {
                auto& engineClass = m_builtins.classes()[value.engineClass()];
                if (&engineClass == &m_builtins.stringClass())
                    return Known::of(arguments[0].isText());
                if (engineClass.type == RegExpObjectType)
                    return Known::of(arguments[0].kind == Known::Kind::RegExp);
                if (engineClass.type == ArrayType)
                    return Known::of(arguments[0].kind == Known::Kind::Record && arguments[0].record->kind == Record::Kind::Array);
                if (engineClass.type == JSSetType)
                    return Known::of(arguments[0].kind == Known::Kind::Record && arguments[0].record->kind == Record::Kind::Set);
                return Known::of(false);
            }
            if (builtin.name == "IsCallable"_s && arguments.size() == 1) {
                auto type = typeOf(arguments[0]);
                return type ? Known::of(*type == TypeofType::Function) : Known();
            }
            return { };
        default:
            return { };
        }
    }

    bool appendSpread(Record& to, const Known& from)
    {
        if (from.isUndefined() && to.kind == Record::Kind::Set)
            return true;
        if (from.kind != Known::Kind::Record || (from.record->kind != Record::Kind::Array && from.record->kind != Record::Kind::Set))
            return false;
        for (auto& element : Values(from.record->elements)) {
            if (to.kind == Record::Kind::Set) {
                bool isThere = false;
                for (auto& other : to.elements) {
                    auto isEqual = isStrictlyEqual(element, other);
                    if (!isEqual)
                        return false;
                    isThere |= *isEqual;
                }
                if (isThere)
                    continue;
            }
            to.elements.append(element);
        }
        return true;
    }

    Known evaluate(const Values& values, const Value& value)
    {
        auto child = [&](unsigned index) -> const Known& {
            ValueID id = value.child(index);
            return id ? values[id.index()] : m_nothing;
        };
        auto arguments = [&] {
            Vector<Known, 4> result;
            for (ValueID argument : value.arguments)
                result.append(values[argument.index()]);
            return result;
        };
        auto compare = [&](auto&& holds) {
            if (child(0).kind != Known::Kind::Number || child(1).kind != Known::Kind::Number)
                return Known();
            return Known::of(static_cast<bool>(holds(child(0).number, child(1).number)));
        };
        auto compute = [&](auto&& result) {
            if (child(0).kind != Known::Kind::Number || child(1).kind != Known::Kind::Number)
                return Known();
            return Known::of(static_cast<double>(result(child(0).number, child(1).number)));
        };
        switch (value.opcode) {
        case Opcode::Undefined:
            return Known::undefined();
        case Opcode::Null:
            return Known::of(Known::Kind::Null);
        case Opcode::Boolean:
            return Known::of(value.boolean());
        case Opcode::Integer:
            return Known::of(static_cast<double>(value.integer()));
        case Opcode::Double:
            return Known::of(std::bit_cast<double>(value.bits()));
        case Opcode::String:
            return Known::of(textOf(value.string()));
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce: {
            Record* closure = make(Record::Kind::Closure);
            if (!closure)
                return { };
            closure->function = value.function();
            closure->elements.appendVector(arguments());
            return Known::of(closure);
        }
        case Opcode::GetModuleVariable:
            return m_variables[value.variable().index()];
        case Opcode::PutModuleVariable:
            m_variables[value.variable().index()] = child(0);
            return Known::undefined();
        case Opcode::NewObject:
        case Opcode::NewObjectWithShape:
            return Known::of(make(Record::Kind::Object));
        case Opcode::GetField:
            return getProperty(child(0), nameOfField(value));
        case Opcode::PutField:
            putProperty(child(0), nameOfField(value), child(1));
            return Known::undefined();
        case Opcode::GetById:
            return getProperty(child(0), textOf(value.string()));
        case Opcode::PutById:
            putProperty(child(0), textOf(value.string()), child(1));
            return Known::undefined();
        case Opcode::NewArray: {
            Record* array = make(Record::Kind::Array);
            if (!array)
                return { };
            array->elements.appendVector(arguments());
            return Known::of(array);
        }
        case Opcode::GetByVal:
            if (child(0).kind == Known::Kind::Record && child(0).record->kind == Record::Kind::Array && child(1).kind == Known::Kind::Number && child(1).number >= 0 && child(1).number < child(0).record->elements.size() && child(1).number == std::trunc(child(1).number))
                return child(0).record->elements[static_cast<unsigned>(child(1).number)];
            if (child(1).isText())
                return getProperty(child(0), child(1).text);
            return { };
        case Opcode::Add:
            if (child(0).kind == Known::Kind::Number && child(1).kind == Known::Kind::Number)
                return Known::of(child(0).number + child(1).number);
            if (!child(0).isText() && !child(1).isText())
                return { };
            return concatenate(std::array { child(0), child(1) });
        case Opcode::Strcat:
            return concatenate(arguments().span());
        case Opcode::ToString:
            return concatenate(std::array { child(0) });
        case Opcode::Sub:
            return compute([](double a, double b) { return a - b; });
        case Opcode::Mul:
            return compute([](double a, double b) { return a * b; });
        case Opcode::Less:
            return compare([](double a, double b) { return a < b; });
        case Opcode::LessEq:
            return compare([](double a, double b) { return a <= b; });
        case Opcode::Greater:
            return compare([](double a, double b) { return a > b; });
        case Opcode::GreaterEq:
            return compare([](double a, double b) { return a >= b; });
        case Opcode::StrictEq:
            return Known::of(isStrictlyEqual(child(0), child(1)));
        case Opcode::LogicalNot: {
            auto boolean = toBoolean(child(0));
            return boolean ? Known::of(!*boolean) : Known();
        }
        case Opcode::ToBoolean:
            return Known::of(toBoolean(child(0)));
        case Opcode::IsUndefinedOrNull:
            return child(0) ? Known::of(child(0).isUndefined() || child(0).kind == Known::Kind::Null) : Known();
        case Opcode::TypeOfIs: {
            auto type = typeOf(child(0));
            return type ? Known::of(*type == value.typeofType()) : Known();
        }
        case Opcode::IsClosureOf:
            if (!child(0))
                return { };
            return Known::of(child(0).kind == Known::Kind::Record && child(0).record->kind == Record::Kind::Closure && child(0).record->function == value.function());
        case Opcode::GetCapture:
            if (child(0).kind != Known::Kind::Record || child(0).record->kind != Record::Kind::Closure || child(0).record->function != value.function() || value.captureIndex() >= child(0).record->elements.size())
                return { };
            return child(0).record->elements[value.captureIndex()];
        case Opcode::Cast:
            return child(0);
        case Opcode::Call:
            return call(child(0), child(1), arguments().span());
        case Opcode::CallMethod:
            return callMethod(child(0), textOf(value.string()), arguments().span());
        case Opcode::CallDirect: {
            Values all;
            all.appendVector(arguments());
            return invoke(value.function(), WTF::move(all));
        }
        case Opcode::CallBuiltin:
            return callBuiltin(value, m_builtins.at(value.builtin()).takesReceiver() ? child(0) : Known(), arguments().span());
        default:
            return { };
        }
    }

    void pass(const Function& function, BlockID block, Values& values, std::span<const Known> arguments)
    {
        auto& parameters = function.at(block).parameters;
        for (unsigned i = 0; i < parameters.size(); ++i)
            values[parameters[i].index()] = i < arguments.size() ? arguments[i] : Known::undefined();
    }

    // What it is handed stands for the parameters of the first block, from the first on; the rest are `undefined`.
    Known invoke(FunctionID id, Values&& passed)
    {
        auto& function = m_program.at(id);
        if (m_depth >= maxDepth || function.blocks.isEmpty() || function.declaredAs || function.isGenericEntrypoint() || function.kind == FunctionKind::Constructor || function.has(Format::FunctionHasRestParameter) || function.has(Format::FunctionHasNewTarget))
            return { };
        SetForScope depth(m_depth, m_depth + 1);
        m_wasEntered.set(id.index());
        // For what the next line costs.
        m_stepsLeft -= std::min<unsigned>(m_stepsLeft, function.values.size() / 8);
        Values values(function.values.size());
        pass(function, BlockID(0u), values, passed.span());
        // In a small function both ways of the first branches that are not known are gone: `cache[i] ||= make(i)`.
        return runFrom(function, BlockID(0u), values, function.values.size() <= maxSizeToGoBothWays ? maxNumberOfBranchesGoneBothWays : 0);
    }

    Known runFrom(const Function& function, BlockID at, Values& values, unsigned branchesToGoBothWays)
    {
        unsigned numberOfGuesses = 0;
        for (;;) {
            auto& block = function.at(at);
            for (ValueID valueID : block.values) {
                if (!m_stepsLeft)
                    return { };
                --m_stepsLeft;
                values[valueID.index()] = evaluate(values, function.at(valueID));
            }
            if (!m_stepsLeft)
                return { };
            --m_stepsLeft;
            auto& terminator = block.terminator;
            auto enter = [&](unsigned index, Values& into) {
                auto& target = terminator.targets[index];
                Vector<Known, 4> arguments;
                for (ValueID argument : target.arguments)
                    arguments.append(values[argument.index()]);
                pass(function, target.block, into, arguments.span());
                return target.block;
            };
            unsigned taken = 0;
            switch (terminator.kind) {
            case TerminatorKind::Return:
                return terminator.value ? values[terminator.value.index()] : Known::undefined();
            case TerminatorKind::Throw:
            case TerminatorKind::Unreachable:
                return { };
            case TerminatorKind::Jump:
            case TerminatorKind::Invoke:
                break;
            case TerminatorKind::Branch:
                if (auto boolean = toBoolean(values[terminator.value.index()]))
                    taken = !*boolean;
                else {
                    if (++numberOfGuesses > maxNumberOfGuesses)
                        return { };
                    // The way that goes on.
                    auto kind = function.at(terminator.targets[0].block).terminator.kind;
                    taken = kind == TerminatorKind::Throw || kind == TerminatorKind::Unreachable;
                    if (branchesToGoBothWays) {
                        --branchesToGoBothWays;
                        Values copy = values;
                        BlockID other = enter(!taken, copy);
                        runFrom(function, other, copy, branchesToGoBothWays);
                    }
                }
                break;
            case TerminatorKind::Switch: {
                auto& subject = values[terminator.value.index()];
                if (subject.kind != Known::Kind::Number && ++numberOfGuesses > maxNumberOfGuesses)
                    return { };
                taken = terminator.targets.size() - 1;
                for (unsigned i = 0; i < terminator.cases.size() && subject.kind == Known::Kind::Number; ++i) {
                    if (static_cast<double>(terminator.cases[i]) == subject.number) {
                        taken = i;
                        break;
                    }
                }
                break;
            }
            }
            at = enter(taken, values);
        }
    }

    VM& m_vm;
    const Program& m_program;
    const Builtins& m_builtins;
    ImageBuilder& m_builder;
    Values m_variables;
    Vector<std::unique_ptr<Record>> m_records;
    BitVector m_wasEntered;
    UncheckedKeyHashSet<String> m_handedOver;
    unsigned m_numberOfExpressions { 0 };
    const Known m_nothing;
    unsigned m_stepsLeft { maxNumberOfSteps };
    unsigned m_depth { 0 };
};

// RegExp::compile() finds the code by source and flags, however the RegExp was made. So nothing depends on what is found here but whether there is code to find.
void addRegExpLiterals(VM& vm, const Program& program, ImageBuilder& builder)
{
    auto& builtins = Builtins::singleton();
    for (auto& function : program.functions) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty() || value.arguments.size() > 2)
                    continue;
                // Whichever entry: in code without types nothing says that the arguments are strings.
                auto& builtin = builtins.at(value.builtin());
                if ((builtin.kind != BuiltinKind::Function && builtin.kind != BuiltinKind::Constructor) || builtin.name != "RegExp"_s)
                    continue;
                Vector<uint8_t> source;
                if (!appendConstantText(program, function, value.arguments[0], source))
                    continue;
                String pattern = String::fromUTF8(source.span());
                if (pattern.isNull())
                    continue;
                PossibleFlags all;
                if (value.arguments.size() == 2)
                    appendPossibleFlags(program, function, value.arguments[1], all, 4);
                else
                    all.append(OptionSet<Yarr::Flags> { });
                for (auto flags : all)
                    builder.addRegExp(vm, pattern, flags);
            }
        }
    }
    EvaluatorOfTexts(vm, program, builder).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

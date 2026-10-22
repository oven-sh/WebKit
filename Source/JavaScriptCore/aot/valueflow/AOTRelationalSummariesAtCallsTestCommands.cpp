/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTIRPrinter.h"
#include "AOTRelationalSummariesInternal.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr std::pair<ValueFate, ASCIILiteral> namesOfFates[] {
    { ValueFate::IsReturned, "returned"_s }, { ValueFate::IsThrown, "thrown"_s }, { ValueFate::IsYielded, "yielded"_s }, { ValueFate::IsStoredInVariable, "stored-in-variable"_s },
    { ValueFate::IsStoredInEnvironment, "stored-in-environment"_s }, { ValueFate::IsStoredInUnknownObject, "stored-in-unknown-object"_s }, { ValueFate::IsHandedToUnknownCode, "handed-to-unknown-code"_s },
    { ValueFate::IsCalled, "called"_s }, { ValueFate::IsConstructed, "constructed"_s }, { ValueFate::HasMembersRead, "members-read"_s }, { ValueFate::HasMembersWritten, "members-written"_s },
    { ValueFate::HasMembersDeletedOrDefined, "members-deleted-or-defined"_s }, { ValueFate::HasKeysEnumerated, "keys-enumerated"_s }, { ValueFate::IsIterated, "iterated"_s }, { ValueFate::IsCompared, "compared"_s },
    { ValueFate::IsConvertedToPrimitive, "converted-to-primitive"_s },
};

// What a test says of the entry parameters of the functions of its program. Of a parameter that it does not name: anything.
class WrittenFates final : public FatesOfParameters {
public:
    struct OfParameter {
        FunctionID function;
        uint32_t parameter { 0 };
        LinkedFate whole { LinkedFate::nothing() };
        LinkedFate apart { LinkedFate::nothing() };
    };

    struct Row {
        FunctionID function;
        HandedToCalledParameter row;
    };

    LinkedFate whole(FunctionID function, uint32_t parameter) const final
    {
        const OfParameter* written = find(function, parameter);
        return written ? written->whole : LinkedFate::anything();
    }

    LinkedFate apartFromCalledParameters(FunctionID function, uint32_t parameter) const final
    {
        const OfParameter* written = find(function, parameter);
        return written ? written->apart : LinkedFate::anything();
    }

    bool listRows(FunctionID function, Vector<HandedToCalledParameter, 4>& rows) const final
    {
        for (const Row& row : m_rows) {
            if (row.function == function)
                rows.append(row.row);
        }
        return true;
    }

    OfParameter& ensure(FunctionID function, uint32_t parameter)
    {
        if (const OfParameter* written = find(function, parameter))
            return const_cast<OfParameter&>(*written);
        m_parameters.append(OfParameter { function, parameter });
        return m_parameters.last();
    }

    void add(Row&& row) { m_rows.append(WTF::move(row)); }

private:
    const OfParameter* find(FunctionID function, uint32_t parameter) const
    {
        for (const OfParameter& written : m_parameters) {
            if (written.function == function && written.parameter == parameter)
                return &written;
        }
        return nullptr;
    }

    Vector<OfParameter> m_parameters;
    Vector<Row> m_rows;
};

} // anonymous namespace

static void printFates(PrintStream& out, OptionSet<ValueFate> fates)
{
    if (fates.contains(ValueFate::IsIncomplete)) {
        out.print("anything"_s);
        return;
    }
    if (fates.isEmpty()) {
        out.print("nothing"_s);
        return;
    }
    CommaPrinter comma("+"_s);
    for (auto [fate, name] : namesOfFates) {
        if (fates.contains(fate))
            out.print(comma, name);
    }
}

static void printMask(PrintStream& out, ASCIILiteral label, uint32_t mask)
{
    if (!mask)
        return;
    out.print(" "_s, label, "=["_s);
    if (mask == UINT32_MAX)
        out.print("any"_s);
    else {
        CommaPrinter comma(", "_s);
        for (uint32_t rest = mask; rest; rest &= rest - 1)
            out.print(comma, std::countr_zero(rest));
    }
    out.print("]"_s);
}

static void printKeys(PrintStream& out, const Program& program, ASCIILiteral label, const KeysOfMembers& keys)
{
    if (keys.isEmpty())
        return;
    out.print(" "_s, label, "=["_s);
    if (keys.mayBeAnyKey)
        out.print("any"_s);
    CommaPrinter comma(", "_s);
    for (PropertyKeyID key : keys.keys) {
        out.print(comma);
        printPropertyKey(out, program, key);
    }
    out.print("]"_s);
}

static void printFate(PrintStream& out, const Program& program, const LinkedFate& fate)
{
    printFates(out, fate.itself);
    if (fate.isAnything())
        return;
    out.print(" contents="_s);
    printFates(out, fate.contents);
    printMask(out, "results"_s, fate.results);
    printMask(out, fate.mayBeStoredDeeper ? "stored-into-operands-or-deeper"_s : "stored-into-operands"_s, fate.storedIntoParameters);
    printMask(out, "stored-into-results"_s, fate.storedIntoResults);
    printKeys(out, program, "read"_s, fate.keysRead);
    printKeys(out, program, "written"_s, fate.keysWritten);
}

// nothing | anything | name+name..
static std::optional<OptionSet<ValueFate>> parseFates(StringView text)
{
    if (text == "nothing"_s)
        return OptionSet<ValueFate> { };
    if (text == "anything"_s)
        return everyValueFate;
    OptionSet<ValueFate> fates;
    for (StringView word : text.split('+')) {
        auto found = std::ranges::find_if(namesOfFates, [&](const auto& entry) {
            return word == entry.second;
        });
        if (found == std::end(namesOfFates))
            return std::nullopt;
        fates.add(found->first);
    }
    return fates;
}

static void setKeysFor(LinkedFate& fate)
{
    if (fate.itself.contains(ValueFate::HasMembersRead))
        fate.keysRead = KeysOfMembers::any();
    if (fate.itself.containsAny({ ValueFate::HasMembersWritten, ValueFate::HasMembersDeletedOrDefined }))
        fate.keysWritten = KeysOfMembers::any();
}

static bool setField(LinkedFate& fate, StringView field, StringView value)
{
    if (field.isEmpty() || field == "contents"_s) {
        std::optional<OptionSet<ValueFate>> fates = parseFates(value);
        if (!fates)
            return false;
        if (field.isEmpty()) {
            fate.itself = *fates;
            if (fate.isAnything())
                fate = LinkedFate::anything();
            else
                setKeysFor(fate);
        } else
            fate.contents = *fates;
        return true;
    }
    std::optional<uint32_t> mask = parseTestNumber<uint32_t>(value);
    if (!mask)
        return false;
    if (field == "results"_s) {
        fate.results = *mask;
        fate.itself.add(ValueFate::IsReturned);
    } else if (field == "into-parameters"_s)
        fate.storedIntoParameters = *mask;
    else if (field == "into-results"_s)
        fate.storedIntoResults = *mask;
    else
        return false;
    return true;
}

// callee | this | new-target | list | argumentN, each with `new-` in front at a construction.
static std::optional<PlaceAtCall> parsePlace(StringView text)
{
    PlaceAtCall place;
    if (text.startsWith("construct-"_s)) {
        place.isConstruction = true;
        text = text.substring(10);
    }
    if (text == "callee"_s)
        place.kind = PlaceAtCall::Kind::Callee;
    else if (text == "this"_s)
        place.kind = PlaceAtCall::Kind::This;
    else if (text == "new-target"_s)
        place.kind = PlaceAtCall::Kind::NewTarget;
    else if (text == "list"_s)
        place.kind = PlaceAtCall::Kind::ElementOfList;
    else if (text.startsWith("argument"_s)) {
        std::optional<uint32_t> argument = parseTestNumber<uint32_t>(text.substring(8));
        if (!argument)
            return std::nullopt;
        place.argument = *argument;
    } else
        return std::nullopt;
    return place;
}

// fN.calls=pC(pH|*pH@place)->fates: the function calls its parameter C and passes its parameter H, with a star something read through H, at the place. The fates are those of what the call answers.
static bool parseRow(StringView value, HandedToCalledParameter& row)
{
    size_t open = value.find('(');
    size_t at = value.find('@');
    size_t close = value.find(")->"_s);
    if (open == notFound || at == notFound || close == notFound || !value.startsWith('p') || open > at || at > close)
        return false;
    StringView handed = value.substring(open + 1, at - open - 1);
    if (handed.startsWith('*')) {
        row.isReadThroughHanded = true;
        handed = handed.substring(1);
    }
    std::optional<uint32_t> calledParameter = parseTestNumber<uint32_t>(value.substring(1, open - 1));
    std::optional<uint32_t> handedParameter = handed.startsWith('p') ? parseTestNumber<uint32_t>(handed.substring(1)) : std::nullopt;
    std::optional<PlaceAtCall> place = parsePlace(value.substring(at + 1, close - at - 1));
    std::optional<OptionSet<ValueFate>> resultOfCall = parseFates(value.substring(close + 3));
    if (!calledParameter || !handedParameter || !place || !resultOfCall)
        return false;
    row.called = *calledParameter;
    row.handed = *handedParameter;
    row.place = *place;
    row.resultOfCall = *resultOfCall;
    return true;
}

// fN.pM[.whole|.apart][.contents|.results|.into-parameters|.into-results]=value, or fN.calls=row.
static bool parseWord(StringView word, WrittenFates& written)
{
    size_t equals = word.find('=');
    if (equals == notFound || !word.startsWith('f'))
        return false;
    StringView value = word.substring(equals + 1);
    Vector<StringView, 4> path;
    for (StringView part : word.left(equals).split('.'))
        path.append(part);
    if (path.size() < 2)
        return false;
    std::optional<uint32_t> function = parseTestNumber<uint32_t>(path[0].substring(1));
    if (!function)
        return false;

    if (path[1] == "calls"_s) {
        WrittenFates::Row row { FunctionID(*function), { } };
        if (path.size() != 2 || !parseRow(value, row.row))
            return false;
        written.add(WTF::move(row));
        return true;
    }

    std::optional<uint32_t> parameter = path[1].startsWith('p') ? parseTestNumber<uint32_t>(path[1].substring(1)) : std::nullopt;
    if (!parameter)
        return false;
    WrittenFates::OfParameter& fates = written.ensure(FunctionID(*function), *parameter);
    size_t next = 2;
    bool setsWhole = true;
    bool setsApart = true;
    if (next < path.size() && (path[next] == "whole"_s || path[next] == "apart"_s)) {
        setsWhole = path[next] == "whole"_s;
        setsApart = !setsWhole;
        ++next;
    }
    StringView field = next < path.size() ? path[next++] : StringView();
    if (next != path.size())
        return false;
    return (!setsWhole || setField(fates.whole, field, value)) && (!setsApart || setField(fates.apart, field, value));
}

// fates-at-calls [operations] [word ..]: what becomes of each operand of each call and access, when flow's results have been applied. operations: of every other operation too.
void runFatesAtCallsTestCommand(TestCommandContext& context)
{
    WrittenFates written;
    bool hasWrittenFates = false;
    bool printsOperations = false;
    for (const String& word : context.arguments) {
        if (word == "operations"_s) {
            printsOperations = true;
            continue;
        }
        if (!parseWord(word, written))
            return printBadArgument(context.out, word);
        hasWrittenFates = true;
    }

    FlowResults results = analyzeValueFlow(*context.program, context.program->types, context.build);
    applyFlowResults(*context.program, results, context.build);
    context.program->setForm(IRForm::Prepared);

    const Program& program = *context.program;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& user = function.at(id);
                EffectRule rule = effectRuleOf(user.opcode);
                if (!printsOperations && (rule == EffectRule::Declared || rule == EffectRule::Operator))
                    continue;
                user.forEachOperand([&](ValueID, uint32_t operandIndex) {
                    LinkedFate fate = hasWrittenFates ? fateOfOperandAt(program, results, written, function, user, operandIndex) : results.fateOfOperandAt(function, user, operandIndex);
                    context.out.print("f"_s, index, " v"_s, id.index(), " "_s, nameOf(user.opcode), " operand "_s, operandIndex, ": "_s);
                    printFate(context.out, program, fate);
                    context.out.println();
                });
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

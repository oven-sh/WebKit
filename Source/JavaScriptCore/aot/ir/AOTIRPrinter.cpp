/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRPrinter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEdgeResults.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <wtf/CommaPrinter.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT { namespace Typed {

struct IDTable {
    ASCIILiteral kind;
    size_t size;
};

struct InvalidPart {
    ASCIILiteral kind;
    uint64_t number;
};

static std::optional<IDTable> tableOf(const Program& program, IDPrefix prefix)
{
    switch (prefix) {
    case IDPrefix::Value:
    case IDPrefix::Block:
        return std::nullopt;
    case IDPrefix::Function:
        return IDTable { "function"_s, program.functions.size() };
    case IDPrefix::Layout:
        return IDTable { "layout"_s, program.layouts.size() };
    case IDPrefix::Variable:
        return IDTable { "variable"_s, program.variables.size() };
    case IDPrefix::Module:
        return IDTable { "module"_s, program.modules.size() };
    case IDPrefix::ModuleFailure:
        return IDTable { "modulefailure"_s, program.moduleFailures.size() };
    case IDPrefix::TypeSite:
        return IDTable { "typesite"_s, program.typeSites.size() };
    case IDPrefix::EvalScope:
        return IDTable { "evalscope"_s, program.evalScopes.size() };
    case IDPrefix::ExpectedResult:
        return IDTable { "expectedresult"_s, program.expectedResults.size() };
    case IDPrefix::Data:
        return IDTable { "data"_s, program.data.size() };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isBuiltinOrNamespaceLayout(const Layout& layout)
{
    return layout.kind == LayoutKind::BuiltinResult || layout.kind == LayoutKind::ModuleNamespace;
}

// The reader of a program as read makes these again from the other layouts, so their lines are comments. The parser takes a number past its own lines for one of them.
static bool isDerivedAgainByReader(const Program& program, const Layout& layout)
{
    return program.form() == IRForm::AsRead && layout.isDerived();
}

static uint32_t countBuiltinAndNamespaceLayoutsBefore(const Program& program, LayoutID id)
{
    uint32_t firstAddedLayout = std::min<uint32_t>(program.numberOfLayoutsAsRead(), id.index());
    std::span<const Layout> addedLayouts = program.layouts.span().subspan(firstAddedLayout, id.index() - firstAddedLayout);
    return static_cast<uint32_t>(std::ranges::partition_point(addedLayouts, isBuiltinOrNamespaceLayout) - addedLayouts.begin());
}

static void printLayoutID(IRPrinter& printer, LayoutID id)
{
    const Program& program = printer.program();
    const Layout& layout = program.at(id);
    PrintStream& out = printer.out();
    out.print(letterOf(IDPrefix::Layout));
    if (!isBuiltinOrNamespaceLayout(layout)) {
        out.print(id.index() - countBuiltinAndNamespaceLayoutsBefore(program, id));
        return;
    }

    out.print("("_s);
    if (layout.kind == LayoutKind::BuiltinResult)
        printer.print(layout.builtinLayout);
    else
        printer.print(layout.namespaceOf);
    out.print(")"_s);
}

template<typename Entry>
static std::optional<InvalidPart> findInvalidStorage(const Instruction& instruction, Entry entry)
{
    if (instruction.numberOfNamedOperands() != numberOfNamedOperandsOf(entry) || instruction.numberOfOperands() < instruction.numberOfNamedOperands())
        return InvalidPart { "operands"_s, instruction.numberOfOperands() };
    size_t numberOfPayloadWords = instruction.inlinePayload().size() + instruction.outOfLinePayload().size();
    if (numberOfPayloadWords < defaultPayloadOf(entry).size())
        return InvalidPart { "payload"_s, numberOfPayloadWords };
    return std::nullopt;
}

static void printOrigin(IRPrinter& printer, const Instruction& instruction)
{
    uint32_t inlineFrame = printer.printsEngineFields() ? instruction.inlineFrame : 0;
    if (!instruction.hasPosition() && !inlineFrame)
        return;
    printer.out().print(" @"_s, instruction.hasPosition() ? instruction.position : 0u);
    if (inlineFrame)
        printer.out().print(":"_s, inlineFrame);
}

template<typename IDType>
static bool printToken(PrintStream& out, const BuiltinTable& builtins, IDType id)
{
    if (!builtins.contains(id))
        return false;
    out.print(builtins.tokenOf(id));
    return true;
}

template<typename IDType>
static bool printNumber(PrintStream& out, const BuiltinTable& builtins, IDType id)
{
    if (!builtins.contains(id))
        return false;
    out.print(id.index());
    return true;
}

static bool needsEscape(uint8_t byte)
{
    return byte < 0x20 || byte >= 0x7f || byte == '"' || byte == '\\';
}

static void beginLineOf(IRPrinter& printer, ProgramList list, uint32_t number)
{
    ASCIILiteral keyword = *keywordOf(list);
    std::optional<char> letter = prefixLetterOf(list);
    if (!letter) {
        printer.beginLine(keyword);
        return;
    }
    std::optional<IDPrefix> prefix = idPrefixOf(static_cast<uint8_t>(*letter));
    RELEASE_ASSERT(prefix);
    printer.beginLine(keyword, *prefix, number);
}

template<typename Record>
static void printLines(IRPrinter& printer, ProgramList list, const Vector<Record>& records)
{
    for (uint32_t index = 0; index < records.size(); ++index) {
        beginLineOf(printer, list, index);
        printFields(printer, records[index]);
        printer.endLine();
    }
}

static void printLayoutLines(IRPrinter& printer)
{
    const Program& program = printer.program();
    uint32_t numberOfLines = 0;
    for (const Layout& layout : program.layouts) {
        if (isBuiltinOrNamespaceLayout(layout))
            continue;
        uint32_t number = numberOfLines++;
        if (isDerivedAgainByReader(program, layout)) {
            if (!printer.printsEngineFields())
                continue;
            printer.out().print("# "_s);
        }
        beginLineOf(printer, ProgramList::Layouts, number);
        printFields(printer, static_cast<const LayoutRecord&>(layout));
        printer.endLine();
    }
}

static void printFunctionLines(IRPrinter& printer)
{
    const Program& program = printer.program();
    for (uint32_t index = 0; index < program.functions.size(); ++index)
        printer.printFunction(FunctionID(index), program.functions[index]);
}

static void printLinesOf(IRPrinter& printer, ProgramList list)
{
    const Program& program = printer.program();
    switch (list) {
    case ProgramList::TypeSites:
        printLines(printer, list, program.typeSites);
        return;
    case ProgramList::ExpectedResults:
        printLines(printer, list, program.expectedResults);
        return;
    case ProgramList::Layouts:
        printLayoutLines(printer);
        return;
    case ProgramList::Variables:
        printLines(printer, list, program.variables);
        return;
    case ProgramList::Modules:
        printLines(printer, list, program.modules);
        return;
    case ProgramList::ModuleFailures:
        printLines(printer, list, program.moduleFailures);
        return;
    case ProgramList::EvalScopes:
        printLines(printer, list, program.evalScopes);
        return;
    case ProgramList::Functions:
        printFunctionLines(printer);
        return;
    case ProgramList::Data:
        printLines(printer, list, program.data);
        return;
    case ProgramList::Entries:
        printLines(printer, list, program.entries);
        return;
    case ProgramList::RuntimeFunctions:
        printLines(printer, list, program.runtimeFunctions);
        return;
    case ProgramList::WebAssemblyModules:
        printLines(printer, list, program.webAssemblyModules);
        return;
    case ProgramList::Bytes:
    case ProgramList::Words:
    case ProgramList::Strings:
    case ProgramList::PropertyKeys:
    case ProgramList::Types:
    case ProgramList::ClassParts:
    case ProgramList::DeclaredTypes:
    case ProgramList::TemplateParts:
    case ProgramList::ValidationRules:
    case ProgramList::ValidationMembers:
    case ProgramList::ValidationNodes:
    case ProgramList::Fields:
    case ProgramList::StaticFields:
    case ProgramList::ModuleRequests:
    case ProgramList::ModuleExports:
    case ProgramList::SyntheticExports:
    case ProgramList::RequiredNames:
    case ProgramList::ColumnAdjustments:
    case ProgramList::EvalBindings:
    case ProgramList::RelationSubjects:
    case ProgramList::StoredIntos:
    case ProgramList::HandedTos:
    case ProgramList::OperandsOf:
    case ProgramList::Code:
    case ProgramList::DataNodes:
    case ProgramList::DataMembers:
    case ProgramList::HostReads:
        ASSERT(!keywordOf(list));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printProgram(PrintStream& out, const Program& program)
{
    IRPrinter(out, program).printProgram();
}

void printProgram(PrintStream& out, const Program& program, PrintedFields printedFields)
{
    IRPrinter(out, program, printedFields).printProgram();
}

void printFunction(PrintStream& out, const Program& program, FunctionID id, const Function& function)
{
    IRPrinter(out, program).printFunction(id, function);
}

void printValue(PrintStream& out, const Program& program, const Function& function, ValueID id)
{
    IRPrinter(out, program).printValue(function, id);
}

void printType(PrintStream& out, const Program& program, const Type& type)
{
    IRPrinter(out, program).printType(type);
}

void printType(PrintStream& out, const Program& program, TypeID id)
{
    IRPrinter(out, program).print(id);
}

void printString(PrintStream& out, const Program& program, StringID id)
{
    IRPrinter(out, program).print(id);
}

void printPropertyKey(PrintStream& out, const Program& program, PropertyKeyID id)
{
    IRPrinter(out, program).print(id);
}

IRPrinter::IRPrinter(PrintStream& out, const Program& program)
    : IRPrinter(out, program, PrintedFields::All)
{
}

IRPrinter::IRPrinter(PrintStream& out, const Program& program, PrintedFields printedFields)
    : m_out(out)
    , m_program(program)
    , m_printedFields(printedFields)
{
}

void IRPrinter::printProgram()
{
    for (ProgramList list : allMembersOf<ProgramList>)
        printLinesOf(*this, list);
}

void IRPrinter::printFunction(FunctionID id, const Function& function)
{
    beginLine(*keywordOf(ProgramList::Functions), IDPrefix::Function, id.index());
    printFields(*this, static_cast<const FunctionRecord&>(function));
    printBody(function);
    endLine();
}

void IRPrinter::printBody(const Function& function)
{
    if (!function.hasBody())
        return;
    m_out.print(" {\n"_s);
    for (size_t index = 0; index < function.blocks.size(); ++index)
        printBlock(function, BlockID(index));
    m_out.print("}"_s);
}

void IRPrinter::printBlock(const Function& function, BlockID id)
{
    const Block& block = function.at(id);
    printBlockHead(function, id, block);
    for (ValueID value : block.values) {
        m_out.print("    "_s);
        printValue(function, value);
        endLine();
    }
    m_out.print("    "_s);
    printTerminator(block.terminator);
    endLine();
}

void IRPrinter::printBlockHead(const Function& function, BlockID id, const Block& block)
{
    m_out.print("  "_s);
    print(id);
    m_out.print("("_s);
    CommaPrinter comma;
    for (ValueID parameter : block.parameters) {
        m_out.print(comma);
        if (parameter.index() >= function.values.size()) {
            printInvalid("value"_s, parameter.index());
            continue;
        }
        print(parameter);
        const Value& value = function.at(parameter);
        if (printsEngineFields())
            printEngineValueFields(value);
        else if (value.type) {
            m_out.print(": "_s);
            print(value.type);
        }
    }
    m_out.print(")"_s);

    m_needsBlankBeforeField = true;
    if (block.role != BlockRole::Ordinary)
        field("role"_s, block.role);
    if (block.subject)
        field("subject"_s, block.subject);
    if (block.frequency != BlockFrequency::Normal)
        field("frequency"_s, block.frequency);
    if (block.handler) {
        beginField("handler"_s);
        printTarget({ }, *block.handler);
    }
    m_out.print(":"_s);
    endLine();
}

void IRPrinter::printValue(const Function& function, ValueID id)
{
    if (id.index() >= function.values.size()) {
        printInvalid("value"_s, id.index());
        return;
    }

    const Value& value = function.at(id);
    auto opcodeNumber = static_cast<unsigned>(value.opcode);
    if (opcodeNumber >= numberOfEnumerators<Opcode>) {
        printInvalid("opcode"_s, opcodeNumber);
        return;
    }

    if (resultRuleOf(value.opcode) != ResultRule::None) {
        print(id);
        printEngineValueFields(value);
        m_out.print(" = "_s);
    }
    m_out.print(nameOf(value.opcode));
    if (auto invalidPart = findInvalidStorage(value, value.opcode)) {
        m_out.print(" "_s);
        printInvalid(invalidPart->kind, invalidPart->number);
        return;
    }
    m_needsBlankBeforeField = true;
    printFields(*this, value);

    unsigned flags = value.flags;
    if (!printsEngineFields())
        flags &= wireFlagsOf(value.opcode);
    if (flags) {
        std::span<const ASCIILiteral> names = flagNamesOf(value.opcode);
        CommaPrinter comma(", "_s, " ["_s);
        for (unsigned remainingFlags = flags; remainingFlags; remainingFlags &= remainingFlags - 1) {
            unsigned bit = WTF::ctz(remainingFlags);
            m_out.print(comma);
            if (bit < names.size())
                m_out.print(names[bit]);
            else
                printInvalid("flag"_s, bit);
        }
        m_out.print("]"_s);
    }
    printOrigin(*this, value);
}

void IRPrinter::printEngineValueFields(const Value& value)
{
    if (!printsEngineFields())
        return;
    if (value.type) {
        m_out.print(": "_s);
        print(value.type);
    }
    if (value.expected()) {
        m_out.print(" ~ "_s);
        print(value.expected());
    }
    if (value.representation != Representation::Unchosen) {
        m_out.print(" @"_s);
        print(value.representation);
    }
}

void IRPrinter::printTerminator(const Terminator& terminator)
{
    auto kindNumber = static_cast<unsigned>(terminator.kind);
    if (kindNumber >= numberOfEnumerators<TerminatorKind>) {
        printInvalid("terminator"_s, kindNumber);
        return;
    }

    m_out.print(nameOf(terminator.kind));
    if (auto invalidPart = findInvalidStorage(terminator, terminator.kind)) {
        m_out.print(" "_s);
        printInvalid(invalidPart->kind, invalidPart->number);
        return;
    }
    m_needsBlankBeforeField = true;
    printFields(*this, terminator);

    auto numberOfTargets = static_cast<uint32_t>(terminator.targets.size());
    CommaPrinter comma(", "_s, " -> "_s);
    for (uint32_t index = 0; index < numberOfTargets; ++index) {
        m_out.print(comma);
        const TargetInfo* info = infoOfTarget(terminator.kind, index, numberOfTargets);
        if (!info) {
            printInvalid("target"_s, index);
            m_out.print(" "_s);
        }
        printTarget(info ? info->name : ASCIILiteral(), terminator.targets[index]);
    }
    printOrigin(*this, terminator);
}

void IRPrinter::printTarget(ASCIILiteral name, const Target& target)
{
    if (!name.isEmpty())
        m_out.print(name, " "_s);
    print(target.block);
    m_out.print("("_s);
    printArguments(target.arguments.span());
    CommaPrinter comma(", "_s, target.arguments.isEmpty() ? "| "_s : " | "_s);
    for (uint32_t proof : target.proofs)
        m_out.print(comma, proof);
    m_out.print(")"_s);
}

void IRPrinter::printArguments(std::span<const ValueID> arguments)
{
    CommaPrinter comma;
    for (ValueID argument : arguments) {
        m_out.print(comma);
        print(argument);
    }
}

void IRPrinter::printID(IDPrefix prefix, uint32_t index)
{
    if (index == UINT32_MAX) {
        m_out.print("-"_s);
        return;
    }

    bool isProvisional = prefix == IDPrefix::Function && ProvisionalFunctions::contains(FunctionID(index));
    if (auto table = tableOf(m_program, prefix); table && index >= table->size && !isProvisional) {
        printInvalid(table->kind, index);
        return;
    }
    if (prefix == IDPrefix::Layout) {
        printLayoutID(*this, LayoutID(index));
        return;
    }
    m_out.print(letterOf(prefix), index);
}

void IRPrinter::printInvalid(ASCIILiteral kind, uint64_t number)
{
    m_out.print("<invalid "_s, kind, " "_s, number, ">"_s);
}

void IRPrinter::beginField(ASCIILiteral name)
{
    if (m_needsBlankBeforeField)
        m_out.print(" "_s);
    m_needsBlankBeforeField = true;
    m_out.print(name, "="_s);
}

void IRPrinter::beginRecord()
{
    m_out.print("{"_s);
    m_needsBlankBeforeField = false;
}

void IRPrinter::endRecord()
{
    m_needsBlankBeforeField = true;
    m_out.print("}"_s);
}

void IRPrinter::beginLine(ASCIILiteral keyword)
{
    m_out.print(keyword);
    m_needsBlankBeforeField = true;
}

void IRPrinter::beginLine(ASCIILiteral keyword, IDPrefix prefix, uint32_t index)
{
    m_out.print(keyword, " "_s, letterOf(prefix), index);
    m_needsBlankBeforeField = true;
}

void IRPrinter::endLine()
{
    m_out.print("\n"_s);
}

void IRPrinter::print(double value)
{
    if (std::isnan(value)) {
        m_out.print("NaN"_s);
        return;
    }
    if (std::isinf(value)) {
        m_out.print(value < 0 ? "-Infinity"_s : "Infinity"_s);
        return;
    }
    if (!value && std::signbit(value)) {
        m_out.print("-0"_s);
        return;
    }
    NumberToStringBuffer buffer;
    m_out.print(numberToStringAndSize(value, buffer));
}

void IRPrinter::print(std::span<const uint8_t> bytes)
{
    m_out.print("\""_s);
    while (!bytes.empty()) {
        if (needsEscape(bytes.front())) {
            m_out.printf("\\x%02x", bytes.front());
            bytes = bytes.subspan(1);
            continue;
        }
        // PrintStream takes the length of a span as an int.
        size_t plainLength = std::min<size_t>(std::ranges::find_if(bytes, needsEscape) - bytes.begin(), INT_MAX);
        m_out.print(byteCast<char>(bytes.first(plainLength)));
        bytes = bytes.subspan(plainLength);
    }
    m_out.print("\""_s);
}

void IRPrinter::print(BuiltinID id)
{
    if (!printToken(m_out, m_program.builtins(), id))
        printInvalid("builtin"_s, id.index());
}

void IRPrinter::print(BuiltinClassID id)
{
    if (!printToken(m_out, m_program.builtins(), id))
        printInvalid("class"_s, id.index());
}

void IRPrinter::print(BuiltinName name)
{
    if (!printNumber(m_out, m_program.builtins(), name))
        printInvalid("name"_s, name.index());
}

void IRPrinter::print(BuiltinLayoutID id)
{
    if (!printToken(m_out, m_program.builtins(), id))
        printInvalid("layout"_s, id.index());
}

void IRPrinter::print(BuiltinSlotID id)
{
    if (!printToken(m_out, m_program.builtins(), id))
        printInvalid("slot"_s, id.index());
}

void IRPrinter::print(BuiltinOverloadID id)
{
    if (!printToken(m_out, m_program.builtins(), id))
        printInvalid("overload"_s, id.index());
}

void IRPrinter::print(RuntimeModuleID id)
{
    if (!printToken(m_out, m_program.builtins(), id))
        printInvalid("runtimemodule"_s, id.index());
}

void IRPrinter::print(RuntimeFunctionID id)
{
    if (!printNumber(m_out, m_program.builtins(), id))
        printInvalid("runtimefunction"_s, id.index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRPrinter.h"

#include "AOTBodyVerifier.h"
#include "AOTBuiltinTable.h"
#include "AOTDiagnostics.h"
#include "AOTMemoryLocation.h"
#include "AOTTypeNarrowing.h"
#include "AOTVector.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <wtf/CommaPrinter.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT { namespace Typed {

struct PrintedIDTable {
    ASCIILiteral kind;
    size_t size;
};

static std::optional<PrintedIDTable> tableOfPrintedID(const Program& program, IDPrefix prefix)
{
    switch (prefix) {
    case IDPrefix::Value:
    case IDPrefix::Block:
        return std::nullopt;
    case IDPrefix::Function:
        return PrintedIDTable { "function"_s, program.functions.size() };
    case IDPrefix::Layout:
        return PrintedIDTable { "layout"_s, program.layouts.size() };
    case IDPrefix::Variable:
        return PrintedIDTable { "variable"_s, program.variables.size() };
    case IDPrefix::Module:
        return PrintedIDTable { "module"_s, program.modules.size() };
    case IDPrefix::ModuleFailure:
        return PrintedIDTable { "modulefailure"_s, program.moduleFailures.size() };
    case IDPrefix::TypeSite:
        return PrintedIDTable { "typesite"_s, program.typeSites.size() };
    case IDPrefix::EvalScope:
        return PrintedIDTable { "evalscope"_s, program.evalScopes.size() };
    case IDPrefix::ExpectedResult:
        return PrintedIDTable { "expectedresult"_s, program.expectedResults.size() };
    case IDPrefix::Data:
        return PrintedIDTable { "data"_s, program.data.size() };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isBuiltinOrNamespaceLayout(const Layout& layout)
{
    return layout.kind == LayoutKind::BuiltinResult || layout.kind == LayoutKind::ModuleNamespace;
}

static void printLayoutInText(IRPrinter& printer, LayoutID id)
{
    const Program& program = printer.program();
    const Layout& layout = program.at(id);
    PrintStream& out = printer.out();
    out.print(letterOf(IDPrefix::Layout));
    if (!isBuiltinOrNamespaceLayout(layout)) {
        out.print(program.numberInTextOf(id));
        return;
    }

    out.print("("_s);
    if (layout.kind == LayoutKind::BuiltinResult)
        printer.print(layout.builtinLayout);
    else
        printer.print(layout.namespaceOf);
    out.print(")"_s);
}

static void printBlockParameter(IRPrinter& printer, const BodyValue& parameter)
{
    printer.print(parameter.id());
    if (TypeID type = parameter.parameterType()) {
        printer.out().print(": "_s);
        printer.print(type);
    }
}

static bool cursorsMayReadPrintedBody(const Body& body)
{
    Vector<InternalError> errors = findErrorsInWords(FunctionID(), body);
    if (errors.isEmpty())
        return true;
    InternalErrorKind kind = errors.first().kind;
    return kind == InternalErrorKind::BlockOrderInvalid || kind == InternalErrorKind::UseNotDominated;
}

template<typename IDType>
static bool printBuiltinToken(PrintStream& out, const BuiltinTable& builtins, IDType id)
{
    if (!builtins.contains(id))
        return false;
    out.print(builtins.tokenOf(id));
    return true;
}

template<typename IDType>
static bool printBuiltinNumber(PrintStream& out, const BuiltinTable& builtins, IDType id)
{
    if (!builtins.contains(id))
        return false;
    out.print(id.index());
    return true;
}

static void beginLineOfList(IRPrinter& printer, ProgramList list, uint32_t number)
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
static void printLinesOfList(IRPrinter& printer, ProgramList list, const Vector<Record>& records)
{
    for (uint32_t index = 0; index < records.size(); ++index) {
        beginLineOfList(printer, list, index);
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
        if (layout.isDerived() || layout.kind == LayoutKind::Closure) {
            if (!printer.printsEngineFields())
                continue;
            printer.out().print("# "_s);
        }
        beginLineOfList(printer, ProgramList::Layouts, number);
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

static void printLinesOfProgramList(IRPrinter& printer, ProgramList list)
{
    const Program& program = printer.program();
    switch (list) {
    case ProgramList::TypeSites:
        printLinesOfList(printer, list, program.typeSites);
        return;
    case ProgramList::ExpectedResults:
        printLinesOfList(printer, list, program.expectedResults);
        return;
    case ProgramList::Layouts:
        printLayoutLines(printer);
        return;
    case ProgramList::Variables:
        printLinesOfList(printer, list, program.variables);
        return;
    case ProgramList::Modules:
        printLinesOfList(printer, list, program.modules);
        return;
    case ProgramList::ModuleFailures:
        printLinesOfList(printer, list, program.moduleFailures);
        return;
    case ProgramList::AmbiguousExports:
        printLinesOfList(printer, list, program.ambiguousExports);
        return;
    case ProgramList::SourceStamps:
        printLinesOfList(printer, list, program.sourceStamps);
        return;
    case ProgramList::DeclaredRecords:
        printLinesOfList(printer, list, program.declaredRecords);
        return;
    case ProgramList::EvalScopes:
        printLinesOfList(printer, list, program.evalScopes);
        return;
    case ProgramList::Functions:
        printFunctionLines(printer);
        return;
    case ProgramList::Data:
        printLinesOfList(printer, list, program.data);
        return;
    case ProgramList::Entries:
        printLinesOfList(printer, list, program.entries);
        return;
    case ProgramList::RuntimeFunctions:
        printLinesOfList(printer, list, program.runtimeFunctions);
        return;
    case ProgramList::WebAssemblyModules:
        printLinesOfList(printer, list, program.webAssemblyModules);
        return;
    case ProgramList::ReplaceableBuiltins:
        printLinesOfList(printer, list, program.replaceableBuiltins);
        return;
    case ProgramList::OpenedNames:
        printLinesOfList(printer, list, program.openedNames);
        return;
    case ProgramList::BuiltinsUsedAtStart:
        printLinesOfList(printer, list, program.builtinsUsedAtStart);
        return;
    case ProgramList::Bytes:
    case ProgramList::Words:
    case ProgramList::Strings:
    case ProgramList::PropertyKeys:
    case ProgramList::Types:
    case ProgramList::ClassParts:
    case ProgramList::DeclaredTypes:
    case ProgramList::DeclaredCaptures:
    case ProgramList::Fields:
    case ProgramList::StaticFields:
    case ProgramList::ModuleRequests:
    case ProgramList::ModuleExports:
    case ProgramList::SyntheticExports:
    case ProgramList::RequiredNames:
    case ProgramList::ColumnAdjustments:
    case ProgramList::QuotedSources:
    case ProgramList::EvalBindings:
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

void printFunction(PrintStream& out, const Program& program, FunctionID id)
{
    IRPrinter printer(out, program);
    if (id.index() >= program.functions.size()) {
        printer.print(id);
        printer.endLine();
        return;
    }
    printer.printFunction(id, program.at(id));
}

void printBody(PrintStream& out, const Program& program, const Body& body)
{
    IRPrinter(out, program).printBody(body);
}

void printValue(PrintStream& out, const Program& program, const BodyValue& value)
{
    IRPrinter(out, program).printValue(value);
}

void printTerminator(PrintStream& out, const Program& program, const BodyTerminator& terminator)
{
    IRPrinter(out, program).printTerminator(terminator);
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

IRPrinter::IRPrinter(PrintStream& out, const Program& program, TypeReader typesOfCode)
    : IRPrinter(out, program, PrintedFields::All)
{
    m_typesOfCode = typesOfCode;
}

void IRPrinter::printProgram()
{
    for (ProgramList list : allMembersOf<ProgramList>)
        printLinesOfProgramList(*this, list);
}

void IRPrinter::printFunction(FunctionID id, const Function& function)
{
    beginLine(*keywordOf(ProgramList::Functions), IDPrefix::Function, id.index());
    printFields(*this, static_cast<const FunctionRecord&>(function));
    printBody(function.body);
    endLine();
}

void IRPrinter::printBody(const Body& body)
{
    if (!body)
        return;
    m_out.print(" {\n"_s);
    if (cursorsMayReadPrintedBody(body)) {
        body.forEachBlock([&](const BodyBlock& block) {
            printBlock(block);
        });
    } else {
        m_out.print("  "_s);
        printInvalid("body"_s, body.words().size());
        endLine();
    }
    m_out.print("}"_s);
}

void IRPrinter::printBlock(const BodyBlock& block)
{
    printBlockHead(block);
    for (BodyValue value : block.values()) {
        m_out.print("    "_s);
        printValue(value);
        endLine();
    }
    m_out.print("    "_s);
    printTerminator(block.terminator());
    endLine();
}

void IRPrinter::printBlockHead(const BodyBlock& block)
{
    m_out.print("  "_s);
    print(block.id());
    m_out.print("("_s);
    CommaPrinter comma;
    for (uint32_t index = 0; index < block.numberOfParameters(); ++index) {
        m_out.print(comma);
        printBlockParameter(*this, block.parameterValue(index));
    }
    m_out.print(")"_s);

    m_needsBlankBeforeField = true;
    if (block.role() != BlockRole::Ordinary)
        field("role"_s, block.role());
    if (block.subject())
        field("subject"_s, block.subject());
    if (BodyTarget handler = block.handler()) {
        beginField("handler"_s);
        printTarget({ }, handler.block(), handler.arguments(), handler.proofs());
    }
    if (block.frequency() != BlockFrequency::Normal)
        field("frequency"_s, block.frequency());
    m_out.print(":"_s);
    endLine();
}

void IRPrinter::printValue(const BodyValue& value)
{
    if (!value) {
        printInvalid("value"_s, value.id().index());
        return;
    }
    if (value.isParameter()) {
        printBlockParameter(*this, value);
        return;
    }

    Opcode opcode = value.opcode();
    auto opcodeNumber = static_cast<unsigned>(opcode);
    if (opcodeNumber >= numberOfEnumerators<Opcode>) {
        printInvalid("opcode"_s, opcodeNumber);
        return;
    }

    if (resultRuleOf(opcode) != ResultRule::None) {
        print(value.id());
        m_out.print(" = "_s);
    }
    m_out.print(nameOf(opcode));
    m_needsBlankBeforeField = true;
    printFields(*this, value);
    printFlags(flagNamesOf(opcode), value.flags());
    printPositions(value);
}

void IRPrinter::printPositions(const BodyValue& value)
{
    constexpr size_t indexOfPositionInHead = offsetof(Wire::ValueHead, position) / sizeof(uint32_t);
    std::span<const uint32_t> words = unsafeMakeSpan(value.words(), value.sizeInWords());
    uint32_t firstPosition = words[indexOfPositionInHead];

    bool printsTailPosition = printsEngineFields() && value.standsInTailPosition();
    printOrigin({ .position = firstPosition, .inlineFrame = 0, .isTailCall = printsTailPosition, .isPartOfConstruction = false });

    WrittenPositionRole role = value.writtenPositionRole();
    if (role == WrittenPositionRole::None)
        return;
    m_out.print(" @"_s, words.back());
    if (role == WrittenPositionRole::StartForCallers && firstPosition != BodyValue::noPosition)
        m_out.print(" callers"_s);
}

void IRPrinter::printTerminator(const BodyTerminator& terminator)
{
    if (!terminator) {
        printInvalid("terminator"_s, terminator.block().index());
        return;
    }

    TerminatorKind kind = terminator.kind();
    auto kindNumber = static_cast<unsigned>(kind);
    if (kindNumber >= numberOfEnumerators<TerminatorKind>) {
        printInvalid("terminator"_s, kindNumber);
        return;
    }

    m_out.print(nameOf(kind));
    m_needsBlankBeforeField = true;
    printFields(*this, terminator);

    uint32_t numberOfTargets = terminator.numberOfTargets();
    uint32_t index = 0;
    CommaPrinter comma(", "_s, " -> "_s);
    for (BodyTarget target : terminator.targets()) {
        m_out.print(comma);
        const TargetInfo* info = infoOfTarget(kind, index, numberOfTargets);
        if (!info) {
            printInvalid("target"_s, index);
            m_out.print(" "_s);
        }
        printTarget(info ? info->name : ASCIILiteral(), target.block(), target.arguments(), target.proofs());
        ++index;
    }
    printOrigin(terminator.origin());
}

void IRPrinter::printFlags(std::span<const ASCIILiteral> names, unsigned flags)
{
    if (!flags)
        return;
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

void IRPrinter::printOrigin(Origin origin)
{
    bool hasPosition = origin.position != BodyValue::noPosition;
    if (!hasPosition && !origin.inlineFrame && !origin.isTailCall && !origin.isPartOfConstruction)
        return;
    m_out.print(" @"_s, hasPosition || m_typesOfCode ? origin.position : 0u);
    if (origin.inlineFrame)
        m_out.print(":"_s, origin.inlineFrame);
    if (origin.isTailCall)
        m_out.print(" tail"_s);
    if (origin.isPartOfConstruction)
        m_out.print(" constructs"_s);
}

void IRPrinter::printTarget(ASCIILiteral name, BlockID block, std::span<const ValueID> arguments, std::span<const uint32_t> proofs)
{
    if (!name.isEmpty())
        m_out.print(name, " "_s);
    print(block);
    m_out.print("("_s);
    printArguments(arguments);
    CommaPrinter comma(", "_s, arguments.empty() ? "| "_s : " | "_s);
    for (uint32_t proof : proofs)
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

    if (auto table = tableOfPrintedID(m_program, prefix); table && index >= table->size) {
        printInvalid(table->kind, index);
        return;
    }
    if (prefix == IDPrefix::Layout) {
        printLayoutInText(*this, LayoutID(index));
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
        size_t plainLength = std::min<size_t>(std::ranges::find_if(bytes, needsEscape) - bytes.begin(), INT_MAX);
        m_out.print(byteCast<char>(bytes.first(plainLength)));
        bytes = bytes.subspan(plainLength);
    }
    m_out.print("\""_s);
}

void IRPrinter::print(BuiltinID id)
{
    if (!printBuiltinToken(m_out, m_program.builtins(), id))
        printInvalid("builtin"_s, id.index());
}

void IRPrinter::print(BuiltinClassID id)
{
    if (!printBuiltinToken(m_out, m_program.builtins(), id))
        printInvalid("class"_s, id.index());
}

void IRPrinter::print(BuiltinName name)
{
    if (!printBuiltinNumber(m_out, m_program.builtins(), name))
        printInvalid("name"_s, name.index());
}

void IRPrinter::print(BuiltinLayoutID id)
{
    if (!printBuiltinToken(m_out, m_program.builtins(), id))
        printInvalid("layout"_s, id.index());
}

void IRPrinter::print(BuiltinSlotID id)
{
    if (!printBuiltinToken(m_out, m_program.builtins(), id))
        printInvalid("slot"_s, id.index());
}

void IRPrinter::print(BuiltinOverloadID id)
{
    if (!printBuiltinToken(m_out, m_program.builtins(), id))
        printInvalid("overload"_s, id.index());
}

void IRPrinter::print(RuntimeModuleID id)
{
    if (!printBuiltinToken(m_out, m_program.builtins(), id))
        printInvalid("runtimemodule"_s, id.index());
}

StringView tokenOf(const BuiltinTable& builtins, RuntimeFunctionID id)
{
    std::span<const RuntimeFunctionData> functions = builtins.runtimeFunctions();
    auto found = std::ranges::lower_bound(functions, id.index(), { }, [](const RuntimeFunctionData& function) { return function.id.index(); });
    if (found == functions.end() || found->id != id)
        return { };
    return byteCast<Latin1Character>(builtinTableRuntimeFunctionTokens[found - functions.begin()]);
}

RuntimeFunctionID runtimeFunctionFromToken(const BuiltinTable& builtins, StringView token)
{
    BuiltinTexts tokens = builtinTableRuntimeFunctionTokens;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (token == StringView(byteCast<Latin1Character>(tokens[i])))
            return builtins.runtimeFunctions()[i].id;
    }
    return { };
}

void IRPrinter::print(RuntimeFunctionID id)
{
    StringView token = tokenOf(m_program.builtins(), id);
    if (token.isNull())
        printInvalid("runtimefunction"_s, id.index());
    else
        m_out.print(token);
}

static void dumpLayoutsOfLocation(IRPrinter& printer, LayoutSetID layouts)
{
    if (liesInThread(layouts)) {
        printer.out().print("?"_s);
        return;
    }
    CommaPrinter bar("|"_s);
    printer.program().at(layouts).forEach([&](LayoutID member) {
        printer.out().print(bar);
        printer.print(member);
    });
}

void MemoryLocation::dump(PrintStream& out, const Program& program) const
{
    out.print(nameOf(m_heap));
    if (m_isEntireHeap)
        return;

    IRPrinter printer(out, program);
    out.print("["_s);
    switch (m_heap) {
    case AbstractHeap::ModuleVariables:
        printer.print(variable());
        break;
    case AbstractHeap::EnvironmentFields:
        printer.print(environmentLayout());
        out.print(", "_s);
        printer.print(environmentFieldIndex());
        break;
    case AbstractHeap::ObjectFields:
        printer.print(key());
        if (receivers()) {
            out.print(", "_s);
            dumpLayoutsOfLocation(printer, receivers());
        }
        break;
    case AbstractHeap::NamedProperties:
        printer.print(key());
        break;
    case AbstractHeap::ShapeWords:
        dumpLayoutsOfLocation(printer, layoutSet());
        break;
    case AbstractHeap::MemberGuards:
        printer.print(guardedLayout());
        break;
    case AbstractHeap::Elements:
        printer.print(elementBase());
        break;
    case AbstractHeap::OrderWords:
    case AbstractHeap::PrototypeLinks:
    case AbstractHeap::ArrayStorage:
    case AbstractHeap::TypedArrayStorage:
    case AbstractHeap::TypedArrayElements:
    case AbstractHeap::CollectionStorage:
    case AbstractHeap::CollectionEntries:
    case AbstractHeap::InternalFields:
    case AbstractHeap::RegExpState:
    case AbstractHeap::PromiseState:
    case AbstractHeap::StringStorage:
    case AbstractHeap::StringBuilderContents:
    case AbstractHeap::ParserState:
    case AbstractHeap::ExternalState:
    case AbstractHeap::AllocatorState:
    case AbstractHeap::RuntimeState:
    case AbstractHeap::CalleesInFrames:
    case AbstractHeap::ArrayLengths:
        RELEASE_ASSERT_NOT_REACHED();
    }
    out.print("]"_s);
}

void MemoryLocationDump::dump(PrintStream& out) const
{
    location.dump(out, program);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

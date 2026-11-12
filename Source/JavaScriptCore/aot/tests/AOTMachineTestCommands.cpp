/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTCode.h"
#include "AOTCodeDescription.h"
#include "AOTCompiledCode.h"
#include "AOTFormsSaid.h"
#include "AOTIRPrinter.h"
#include "AOTMachineAllocation.h"
#include "AOTMachineOrder.h"
#include "AOTMachinePrinter.h"
#include "AOTPlan.h"
#include "AOTStubDescriptor.h"
#include "AOTSweep.h"
#include "AOTTypeText.h"
#include <algorithm>
#include <wtf/ASCIICType.h>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>
#include <wtf/SHA1.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class WordOfMentionKey : uint8_t { Number, Key, Text, Module, Variable, Layout, Function, Data, Type, Builtin, BuiltinCell, ClassSide };

class NamesInBytesText {
    WTF_MAKE_NONCOPYABLE(NamesInBytesText);
public:
    NamesInBytesText(PrintStream&, const Program&);

    PrintStream& out() { return m_out; }

    void printFunctionName(FunctionID);
    void printUnitName(CodeUnit);
    void printMention(const CodeBody&, uint32_t index);
    void printSymbol(const CodeBody&, const Relocation&);
    void printSite(const SiteDescription&, OptionalIndex);

private:
    template<typename IDType> bool namesRecord(IDType, size_t numberOfRecords);

    void printModuleName(ModuleID);
    void printLayoutName(LayoutID);
    void printVariableName(VariableID);
    void printDataName(DataID);
    void printKeyName(PropertyKeyID);
    void printTypeName(TypeID);
    bool printNameInPlaceOf(IDPrefix, uint32_t number);
    void printNamesInPlaceOfNumbers(const WTF::CStringBase& textOfIRPrinter);
    void printWord(WordOfMentionKey, uint32_t word);
    void printKeyOfMention(NumberSpace, std::span<const uint32_t> key);

    PrintStream& m_out;
    const Program& m_program;
    IRPrinter m_printer;
    Vector<uint32_t> m_placeOfDataInModule;
    bool m_isListingFieldsOfLayout { false };
};

struct TotalOfCodeForm {
    uint64_t count { 0 };
    uint64_t bytes { 0 };
};

} // anonymous namespace

static constexpr ASCIILiteral lineOfFailedMachineCommand = "the build failed"_s;

static std::optional<Vector<CodeUnit>> unitsOfMachineCommand(TestCommandContext& context, StagedBuild& stage)
{
    if (!context.arguments.empty())
        return unitsNamedBy(stage.program(), context.arguments, context.out);

    Vector<CodeUnit> units = stage.unitsToCompile();
    if (!stage.prepared()) {
        context.out.println(lineOfFailedMachineCommand);
        return std::nullopt;
    }
    return units;
}

static void printHeadingOfMachineUnit(PrintStream& out, CodeUnit unit)
{
    out.print("unit "_s);
    printCodeUnit(out, unit);
}

static void printProceduresOfMachineUnits(TestCommandContext& context, MachineState state, void (*print)(PrintStream&, const Machine::Procedure&))
{
    StagedBuild stage = context.stage();
    std::optional<Vector<CodeUnit>> units = unitsOfMachineCommand(context, stage);
    if (!units)
        return;
    for (CodeUnit unit : *units) {
        printHeadingOfMachineUnit(context.out, unit);
        context.out.println();
        const Machine::Procedure* procedure = stage.machineOperations(unit, state);
        if (!procedure) {
            context.out.println(lineOfFailedMachineCommand);
            if (const Machine::Procedure* refused = stage.refusedMachineOperations())
                context.out.print(*refused);
            return;
        }
        print(context.out, *procedure);
    }
}

static void runMachineEmitCommand(TestCommandContext& context)
{
    printProceduresOfMachineUnits(context, MachineState::Appended, Machine::printProcedure);
}

static void runMachineOrderCommand(TestCommandContext& context)
{
    printProceduresOfMachineUnits(context, MachineState::Prepared, Machine::printOrder);
}

static void runMachineLivenessCommand(TestCommandContext& context)
{
    printProceduresOfMachineUnits(context, MachineState::Prepared, Machine::printLiveness);
}

static void runMachineAllocateCommand(TestCommandContext& context)
{
    printProceduresOfMachineUnits(context, MachineState::Prepared, Machine::printAllocation);
}

static void runMachineFrameCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    std::optional<Vector<CodeUnit>> units = unitsOfMachineCommand(context, stage);
    if (!units)
        return;
    for (CodeUnit unit : *units) {
        printHeadingOfMachineUnit(context.out, unit);
        context.out.println();
        const Machine::Procedure* procedure = stage.machineOperations(unit, MachineState::Prepared);
        if (!procedure) {
            context.out.println(lineOfFailedMachineCommand);
            if (const Machine::Procedure* refused = stage.refusedMachineOperations())
                context.out.print(*refused);
            return;
        }
        Machine::printFrame(context.out, *procedure);
        unsigned sizeOfFramePrinted = procedure->frame().bytesBelowFrameRecord();
        const CompiledCode* code = stage.plan() ? stage.compiledCode(unit) : nullptr;
        if (code && code->frame.hasFrame() && code->frame.frameSizeInBytes != sizeOfFramePrinted)
            context.out.println("the build makes this unit another way: size = "_s, code->frame.frameSizeInBytes);
    }
}

namespace {

struct StepsCounted {
    void count(const Machine::Procedure& procedure, const Machine::Block* block)
    {
        const Machine::Allocation& allocation = procedure.allocation();
        std::span<const Machine::Step> steps = allocation.steps();
        const bool isLikely = procedure.order().of(block).layoutPart == Machine::LayoutPart::Likely;
        auto countRange = [&](uint32_t begin, uint32_t end) {
            for (uint32_t index = begin; index < end && index < steps.size(); ++index) {
                const Machine::Step& step = steps[index];
                const bool executes = step.kind == Machine::StepKind::Execute;
                const bool isSaved = executes && step.operation->type() != Machine::Void && allocation.isStoredBehindDefinition(step.operation);
                if (!isLikely) {
                    elsewhere += 1 + isSaved;
                    continue;
                }
                ++(executes ? operations : moves);
                moves += isSaved;
                patches += executes && step.operation->opcode() == Machine::Patch;
            }
        };
        const Machine::BlockSteps& range = allocation.stepsOf(block);
        countRange(range.begin, range.end);
        countRange(range.beginOfEdgeMoves, range.endOfEdgeMoves);
        if (range.terminator < steps.size())
            countRange(range.terminator, range.terminator + 1);
    }

    uint32_t operations { 0 };
    uint32_t patches { 0 };
    uint32_t moves { 0 };
    uint32_t elsewhere { 0 };
};

struct BoundsOfSteps {
    bool areGiven { false };
    Vector<uint32_t, 4> most;
    size_t next { 0 };
};

} // anonymous namespace

static thread_local BoundsOfSteps boundsOfSteps;

static void printStepsCounted(PrintStream& out, ASCIILiteral what, const StepsCounted& counted, ASCIILiteral elsewhere)
{
    if (!boundsOfSteps.areGiven) {
        out.println(what, ": operations "_s, counted.operations, ", patches "_s, counted.patches, ", moves "_s, counted.moves, elsewhere, counted.elsewhere);
        return;
    }
    if (boundsOfSteps.next == boundsOfSteps.most.size()) {
        out.println(what, ": no bound is given for it"_s);
        return;
    }
    uint32_t most = boundsOfSteps.most[boundsOfSteps.next++];
    if (counted.operations + counted.moves <= most)
        out.println(what, ": at most "_s, most);
    else
        out.println(what, ": "_s, counted.operations + counted.moves, " operations and moves, over "_s, most);
}

static bool takeBoundsOfSteps(TestCommandContext& context)
{
    constexpr ASCIILiteral word = "most:"_s;
    boundsOfSteps = { };
    if (context.arguments.empty() || !context.arguments[0].startsWith(word))
        return true;
    boundsOfSteps.areGiven = true;
    for (StringView number : StringView(context.arguments[0]).substring(word.length()).split(',')) {
        std::optional<uint32_t> most = parseInteger<uint32_t>(number);
        if (!most) {
            context.out.println("bad argument "_s, context.arguments[0]);
            return false;
        }
        boundsOfSteps.most.append(*most);
    }
    context.arguments = context.arguments.subspan(1);
    return true;
}

static void printLoopsOfProcedure(PrintStream& out, const Machine::Procedure& procedure)
{
    const Machine::Order& order = procedure.order();
    std::span<const Machine::Loop> loops = order.loops();
    for (uint32_t loop = 0; loop < loops.size(); ++loop) {
        if (std::ranges::any_of(loops, [&](const Machine::Loop& other) { return other.parent == loop; }))
            continue;
        StepsCounted inLoop;
        for (const Machine::Block* block : order.blocksInLayout()) {
            if (order.of(block).loop == loop && !order.of(block).isRare())
                inLoop.count(procedure, block);
        }
        if (inLoop.operations || inLoop.moves || inLoop.elsewhere)
            printStepsCounted(out, "loop"_s, inLoop, ", unlikely "_s);
    }
}

static void printStepsOfProcedure(PrintStream& out, const Machine::Procedure& procedure)
{
    StepsCounted inUnit;
    for (const Machine::Block* block : procedure.order().blocksInLayout())
        inUnit.count(procedure, block);
    printStepsCounted(out, "steps"_s, inUnit, ", unlikely or rare "_s);
}

static void runMachineLoopsCommand(TestCommandContext& context)
{
    if (takeBoundsOfSteps(context))
        printProceduresOfMachineUnits(context, MachineState::Prepared, printLoopsOfProcedure);
}

static void runMachineStepsCommand(TestCommandContext& context)
{
    if (takeBoundsOfSteps(context))
        printProceduresOfMachineUnits(context, MachineState::Prepared, printStepsOfProcedure);
}

static void runMachineEmitCodeCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    const Plan* plan = stage.plan();
    if (!plan) {
        context.out.println(lineOfFailedMachineCommand);
        return;
    }
    const TestSection* section = context.findSection("code"_s);
    if (!section) {
        context.out.println("error: the file has no section `code`"_s);
        return;
    }
    Code code;
    String error = parseCode(CodeTextContext { stage.program(), stage.types() }, section->text, code);
    if (!error.isNull()) {
        context.out.println("error in the Code's text, which starts at line "_s, section->firstLine, " of the file: "_s, error);
        return;
    }
    RoomsOfSweep rooms;
    Sweep sweep(rooms);
    sweep.run(SweepInputs { stage.program(), *stage.facts(), *plan, stage.types() }, code);

    printHeadingOfMachineUnit(context.out, code.unit());
    context.out.println();
    const Machine::Procedure* procedure = stage.machineOperationsOf(code, MachineState::Appended);
    if (!procedure) {
        context.out.println(lineOfFailedMachineCommand);
        return;
    }
    Machine::printProcedure(context.out, *procedure);
}

static void runMachineEmitFormsOfAccessesToElementsCommand(TestCommandContext& context)
{
    const TestSection* section = context.findSection("code"_s);
    if (!section) {
        context.out.println("error: the file has no section `code`"_s);
        return;
    }
    StagedBuild stage = context.stage();
    const Plan* plan = stage.plan();
    if (!plan) {
        context.out.println(lineOfFailedMachineCommand);
        return;
    }
    static constexpr std::array opcodes { Opcode::HasArrayStorage, Opcode::IsElementPresent, Opcode::GetArrayElement };
    for (Opcode opcode : opcodes) {
        for (ArrayStorage kind : allMembersOf<ArrayStorage>) {
            for (unsigned flags = 0; flags < (opcode == Opcode::HasArrayStorage ? 4u : 2u); ++flags) {
                bool admitsInt32Storage = flags & 1;
                bool isWritable = flags & 2;
                String storage = kind == ArrayStorage::Other ? emptyString() : makeString(" arrayStorage="_s, nameOf(kind));
                String words = !flags ? emptyString() : makeString(" ["_s, isWritable ? "isWritable"_s : ""_s, flags == 3 ? ", "_s : ""_s, admitsInt32Storage ? "admitsInt32Storage"_s : ""_s, ']');
                context.out.print(nameOf(opcode), storage, words, ": "_s);
                if (!accessToElementsHasCode(opcode, kind, admitsInt32Storage, isWritable)) {
                    context.out.println("no code"_s);
                    continue;
                }
                String text = section->text;
                for (Opcode other : opcodes) {
                    text = makeStringByReplacingAll(text, makeString("$storageOf"_s, nameOf(other)), other == opcode ? storage : other == Opcode::HasArrayStorage ? String(" arrayStorage=Double"_s) : emptyString());
                    text = makeStringByReplacingAll(text, makeString("$flagsOf"_s, nameOf(other)), other == opcode ? words : emptyString());
                }
                Code code;
                String error = parseCode(CodeTextContext { stage.program(), stage.types() }, text, code);
                if (!error.isNull()) {
                    context.out.println("error in the Code's text: "_s, error);
                    continue;
                }
                RoomsOfSweep rooms;
                Sweep sweep(rooms);
                sweep.run(SweepInputs { stage.program(), *stage.facts(), *plan, stage.types() }, code);
                context.out.println(stage.machineOperationsOf(code, MachineState::Appended) ? "code"_s : "THE TABLE SAYS CODE AND THE EMITTER HAS NONE"_s);
            }
        }
    }
}

NamesInBytesText::NamesInBytesText(PrintStream& out, const Program& program)
    : m_out(out)
    , m_program(program)
    , m_printer(out, program)
{
    size_t numberOfModules = program.modules.size();
    Vector<uint32_t> numberOfDataInModule(FillWith { }, numberOfModules + 1, 0u);
    m_placeOfDataInModule.reserveInitialCapacity(program.data.size());
    for (const Data& data : program.data) {
        size_t module = data.module && data.module.index() < numberOfModules ? data.module.index() : numberOfModules;
        m_placeOfDataInModule.append(numberOfDataInModule[module]++);
    }
}

template<typename IDType>
bool NamesInBytesText::namesRecord(IDType id, size_t numberOfRecords)
{
    if (id && id.index() < numberOfRecords)
        return true;
    if (id)
        m_out.print("?"_s, id.index());
    else
        m_out.print("-"_s);
    return false;
}

void NamesInBytesText::printModuleName(ModuleID id)
{
    if (namesRecord(id, m_program.modules.size()))
        m_printer.print(m_program.at(id).path);
}

void NamesInBytesText::printFunctionName(FunctionID id)
{
    if (!namesRecord(id, m_program.functions.size()))
        return;
    const Function& function = m_program.at(id);
    printModuleName(function.module);
    m_out.print(":"_s, function.position, " "_s);
    m_printer.print(function.name);
}

void NamesInBytesText::printUnitName(CodeUnit unit)
{
    m_out.print(nameOf(unit.role), " "_s);
    printFunctionName(unit.function);
}

void NamesInBytesText::printLayoutName(LayoutID id)
{
    if (!namesRecord(id, m_program.layouts.size()))
        return;
    const Layout& layout = m_program.at(id);
    m_out.print(nameOf(layout.kind));
    if (layout.isDerived())
        m_out.print(" derived"_s);
    if (layout.name) {
        m_out.print(" "_s);
        m_printer.print(layout.name);
    }
    m_out.print(" "_s);
    if (layout.builtinLayout) {
        m_printer.print(layout.builtinLayout);
        return;
    }
    if (layout.namespaceOf) {
        printModuleName(layout.namespaceOf);
        return;
    }
    if (layout.module) {
        printModuleName(layout.module);
        m_out.print(":"_s, layout.position);
        return;
    }

    if (m_isListingFieldsOfLayout) {
        m_out.print("{..}"_s);
        return;
    }
    SetForScope isListingFields(m_isListingFieldsOfLayout, true);
    m_out.print("{"_s);
    CommaPrinter comma;
    for (const Field& field : layout.fields) {
        m_out.print(comma);
        printKeyName(field.key);
    }
    m_out.print("}"_s);
}

void NamesInBytesText::printVariableName(VariableID id)
{
    if (!namesRecord(id, m_program.variables.size()))
        return;
    const Variable& variable = m_program.at(id);
    m_printer.print(variable.name);
    m_out.print(" of "_s);
    printModuleName(variable.module);
    if (variable.owner) {
        m_out.print(" in "_s);
        printLayoutName(variable.owner);
    }
}

void NamesInBytesText::printDataName(DataID id)
{
    if (!namesRecord(id, m_program.data.size()))
        return;
    m_out.print(m_placeOfDataInModule[id.index()], " of "_s);
    printModuleName(m_program.at(id).module);
}

bool NamesInBytesText::printNameInPlaceOf(IDPrefix prefix, uint32_t number)
{
    switch (prefix) {
    case IDPrefix::Function:
        m_out.print("f("_s);
        printFunctionName(FunctionID::fromBits(number));
        break;
    case IDPrefix::Layout:
        m_out.print("L("_s);
        printLayoutName(m_program.layoutNumberedInText(number));
        break;
    case IDPrefix::Variable:
        m_out.print("g("_s);
        printVariableName(VariableID::fromBits(number));
        break;
    case IDPrefix::Module:
        m_out.print("m("_s);
        printModuleName(ModuleID::fromBits(number));
        break;
    case IDPrefix::Data:
        m_out.print("d("_s);
        printDataName(DataID::fromBits(number));
        break;
    case IDPrefix::Value:
    case IDPrefix::Block:
    case IDPrefix::ModuleFailure:
    case IDPrefix::TypeSite:
    case IDPrefix::EvalScope:
    case IDPrefix::ExpectedResult:
        return false;
    }
    m_out.print(")"_s);
    return true;
}

void NamesInBytesText::printNamesInPlaceOfNumbers(const WTF::CStringBase& textOfIRPrinter)
{
    std::span<const char> text = textOfIRPrinter.span();
    bool isInQuotes = false;
    for (size_t index = 0; index < text.size();) {
        char character = text[index];
        if (isInQuotes) {
            size_t length = character == '\\' && index + 1 < text.size() ? 2 : 1;
            m_out.print(text.subspan(index, length));
            isInQuotes = character != '"';
            index += length;
            continue;
        }

        size_t end = index + 1;
        uint64_t number = 0;
        while (end < text.size() && isASCIIDigit(text[end]) && number < UINT32_MAX)
            number = number * 10 + static_cast<uint64_t>(text[end++] - '0');
        bool standsAlone = (!index || !isASCIIAlphanumeric(text[index - 1])) && (end == text.size() || !isASCIIAlphanumeric(text[end]));
        std::optional<IDPrefix> prefix = idPrefixOf(static_cast<uint8_t>(character));
        if (prefix && end > index + 1 && standsAlone && number < UINT32_MAX && printNameInPlaceOf(*prefix, static_cast<uint32_t>(number))) {
            index = end;
            continue;
        }

        isInQuotes = character == '"';
        m_out.print(character);
        ++index;
    }
}

void NamesInBytesText::printKeyName(PropertyKeyID key)
{
    StringPrintStream text;
    printPropertyKey(text, m_program, key);
    printNamesInPlaceOfNumbers(text.toUTF8CString());
}

void NamesInBytesText::printTypeName(TypeID type)
{
    StringPrintStream text;
    printType(text, TypeReader(m_program.types), type);
    printNamesInPlaceOfNumbers(text.toUTF8CString());
}

static WordOfMentionKey wordOfPlaceInMentionKeyAt(size_t index)
{
    return index % 2 ? WordOfMentionKey::Number : WordOfMentionKey::Module;
}

static WordOfMentionKey wordOfMentionKeyAt(NumberSpace space, std::span<const uint32_t> key, size_t index)
{
    switch (space) {
    case NumberSpace::Name:
    case NumberSpace::KeyCell:
    case NumberSpace::PropertyRun:
        return WordOfMentionKey::Key;
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::BigInt:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
        return WordOfMentionKey::Text;
    case NumberSpace::BuiltinCell:
        return WordOfMentionKey::BuiltinCell;
    case NumberSpace::GlobalBinding:
        return index ? WordOfMentionKey::Number : WordOfMentionKey::Key;
    case NumberSpace::RequireEdge:
        return index < 2 ? WordOfMentionKey::Module : WordOfMentionKey::Text;
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
        return WordOfMentionKey::Variable;
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
        return WordOfMentionKey::Module;
    case NumberSpace::ModuleBuiltinCell:
        return index ? WordOfMentionKey::Module : WordOfMentionKey::Builtin;
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
        return WordOfMentionKey::Data;
    case NumberSpace::RegExpLiteralCell:
        if (index == 2)
            return WordOfMentionKey::Text;
        return index ? WordOfMentionKey::Number : WordOfMentionKey::Module;
    case NumberSpace::TemplateObjectCell:
        return index ? WordOfMentionKey::Number : WordOfMentionKey::Module;
    case NumberSpace::ClassGuard:
        if (index < 2)
            return index ? WordOfMentionKey::ClassSide : WordOfMentionKey::Layout;
        if (!(index % 2))
            return WordOfMentionKey::Number;
        return key[index - 1] == static_cast<uint32_t>(NumberSpace::Name) ? WordOfMentionKey::Key : WordOfMentionKey::Text;
    case NumberSpace::ClassRank:
    case NumberSpace::HeirLookedAt:
    case NumberSpace::EnvironmentHandedBack:
        return WordOfMentionKey::Layout;
    case NumberSpace::SiteCache:
        if (index + 2 < key.size())
            return wordOfPlaceInMentionKeyAt(index);
        return index + 2 == key.size() ? WordOfMentionKey::Key : WordOfMentionKey::Number;
    case NumberSpace::ArrayProfile:
        return index + 1 < key.size() ? wordOfPlaceInMentionKeyAt(index) : WordOfMentionKey::Number;
    case NumberSpace::ReadGroupCache:
        return index ? WordOfMentionKey::Number : WordOfMentionKey::Function;
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
        return WordOfMentionKey::Function;
    case NumberSpace::Allocation:
        if (index == layoutWordOfAllocationKey)
            return WordOfMentionKey::Layout;
        return index == constructorWordOfAllocationKey ? WordOfMentionKey::Function : WordOfMentionKey::Number;
    case NumberSpace::Type:
        return WordOfMentionKey::Type;
    case NumberSpace::RegExp:
        return index ? WordOfMentionKey::Number : WordOfMentionKey::Text;
    case NumberSpace::CardOfWord:
    case NumberSpace::Request:
    case NumberSpace::MethodSelector:
        return WordOfMentionKey::Number;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WordOfMentionKey wordOfRequestKeyAt(RequestKind kind, size_t index)
{
    switch (kind) {
    case RequestKind::NewClass:
        return WordOfMentionKey::Layout;
    case RequestKind::TemplateObject:
        return WordOfMentionKey::Text;
    case RequestKind::DirectEval:
        return index ? WordOfMentionKey::Module : WordOfMentionKey::Number;
    case RequestKind::Data:
    case RequestKind::CollectionData:
        return index ? WordOfMentionKey::Number : WordOfMentionKey::Data;
    case RequestKind::FormatPattern:
    case RequestKind::ImportChecks:
        return WordOfMentionKey::Number;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void NamesInBytesText::printWord(WordOfMentionKey kind, uint32_t word)
{
    switch (kind) {
    case WordOfMentionKey::Number:
        m_out.print(word);
        return;
    case WordOfMentionKey::Key:
        printKeyName(PropertyKeyID::fromBits(word));
        return;
    case WordOfMentionKey::Text:
        m_printer.print(StringID::fromBits(word));
        return;
    case WordOfMentionKey::Module:
        printModuleName(ModuleID::fromBits(word));
        return;
    case WordOfMentionKey::Variable:
        printVariableName(VariableID::fromBits(word));
        return;
    case WordOfMentionKey::Layout:
        printLayoutName(LayoutID::fromBits(word));
        return;
    case WordOfMentionKey::Function:
        printFunctionName(FunctionID::fromBits(word));
        return;
    case WordOfMentionKey::Data:
        printDataName(DataID::fromBits(word));
        return;
    case WordOfMentionKey::Type:
        printTypeName(TypeID::fromBits(word));
        return;
    case WordOfMentionKey::Builtin:
        m_printer.print(BuiltinID(word));
        return;
    case WordOfMentionKey::BuiltinCell:
        if (std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(word, m_program.builtins())) {
            m_out.print(WTF::enumName(key->kind()), " "_s);
            m_printer.print(key->builtin());
        } else
            m_out.print("?"_s, word);
        return;
    case WordOfMentionKey::ClassSide:
        if (word == sideWordOfClassGuardOfName(ClassSide::Prototype, AskerOfClassGuardOfName::StoresAlone))
            m_out.print("PrototypeForStores");
        else if (word == sideWordOfClassGuardOfName(ClassSide::Prototype, AskerOfClassGuardOfName::ReadsBehindLookAtObject))
            m_out.print("PrototypeBehindLookAtObject");
        else
            m_out.print(WTF::enumName(static_cast<ClassSide>(word)));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void NamesInBytesText::printKeyOfMention(NumberSpace space, std::span<const uint32_t> key)
{
    m_out.print(WTF::enumName(space));
    if (space == NumberSpace::CardOfWord && !key.empty() && key[0] < numberOfNumberSpaces) {
        m_out.print(" of "_s);
        printKeyOfMention(static_cast<NumberSpace>(key[0]), key.subspan(1));
        return;
    }
    if (space == NumberSpace::Request && !key.empty() && key[0] <= static_cast<uint32_t>(RequestKind::CollectionData)) {
        auto kind = static_cast<RequestKind>(key[0]);
        m_out.print(" "_s, WTF::enumName(kind));
        for (size_t index = 1; index < key.size(); ++index) {
            m_out.print(" "_s);
            printWord(wordOfRequestKeyAt(kind, index - 1), key[index]);
        }
        return;
    }
    if (space == NumberSpace::ClassGuard && isRowOfClassGuardCountsKey(key)) {
        m_out.print(" row of counts of"_s);
        forEachSlotInRowOfClassGuardCountsKey(key, [&](uint32_t, std::span<const uint32_t> slot) {
            m_out.print(" ["_s);
            forEachGuardInSlotOfRowOfClassGuardCounts(slot, [&](std::span<const uint32_t> guard) {
                m_out.print("("_s);
                printKeyOfMention(space, guard);
                m_out.print(")"_s);
            });
            m_out.print("]"_s);
        });
        return;
    }
    if (space == NumberSpace::ClassGuard && isClassGuardCountKey(key)) {
        m_out.print(" count of"_s);
        forEachGuardInClassGuardCountKey(key, [&](std::span<const uint32_t> guard) {
            m_out.print(" ("_s);
            printKeyOfMention(space, guard);
            m_out.print(")"_s);
        });
        return;
    }
    for (size_t index = 0; index < key.size(); ++index) {
        m_out.print(" "_s);
        printWord(wordOfMentionKeyAt(space, key, index), key[index]);
    }
}

void NamesInBytesText::printMention(const CodeBody& body, uint32_t index)
{
    if (index >= body.mentions.size() || static_cast<unsigned>(body.mentions[index].space) >= numberOfNumberSpaces) {
        m_out.print("?"_s, index);
        return;
    }
    const Mention& mention = body.mentions[index];
    printKeyOfMention(mention.space, keyOf(mention, body.mentionWords.span()));
    if (mention.addend)
        m_out.print(" plus "_s, mention.addend);
}

static void printStubInBytesText(PrintStream& out, StubKey key)
{
    if (!key.hasFamily()) {
        out.print("?"_s, key.bits());
        return;
    }
    const StubDescriptor& descriptor = descriptorOf(key.family());
    out.print(descriptor.name);
    if (key.form() < descriptor.formNames.size())
        out.print("."_s, descriptor.formNames[key.form()]);
    if (descriptor.axes.contains(StubAxis::Count))
        out.print(" count "_s, key.count());
    if (key.callerFrame() != CallerFrame::HasFrame)
        out.print(" "_s, WTF::enumName(key.callerFrame()));
    if (isOperationCallHead(key))
        out.print(" "_s, nameOf(entryAtOffset(*key.immediate())));
    else if (key.immediate())
        out.print(" immediate "_s, *key.immediate());
}

void NamesInBytesText::printSymbol(const CodeBody& body, const Relocation& relocation)
{
    uint32_t index = relocation.symbol;
    m_out.print(WTF::enumName(relocation.space), " "_s);
    switch (relocation.space) {
    case SymbolSpace::Function:
        if (index >= body.callees.size())
            break;
        printUnitName(body.callees[index]);
        m_out.print(" entry "_s, relocation.entry);
        return;
    case SymbolSpace::Stub: {
        if (index >= body.stubKeys.size())
            break;
        auto numbered = std::ranges::find(body.rareLists->numberedStubKeys, index, &NumberedStubKey::stubKey);
        if (numbered == body.rareLists->numberedStubKeys.end()) {
            printStubInBytesText(m_out, body.stubKeys[index]);
            return;
        }
        printStubInBytesText(m_out, body.stubKeys[index].withoutImmediate());
        m_out.print(body.stubKeys[index].immediate() ? " immediate ("_s : " under ("_s);
        printMention(body, numbered->mention);
        m_out.print(")"_s);
        return;
    }
    case SymbolSpace::Number:
    case SymbolSpace::InstanceWord:
    case SymbolSpace::RegExp:
        printMention(body, index);
        return;
    case SymbolSpace::ArgumentThunk:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        m_out.print(index);
        return;
    }
    m_out.print("?"_s, index);
}

void NamesInBytesText::printSite(const SiteDescription& sites, OptionalIndex site)
{
    if (!site) {
        m_out.print(" without site"_s);
        return;
    }
    m_out.print(" site "_s, *site);
    if (*site >= sites.positions.size())
        return;
    const SitePosition& position = sites.positions[*site];
    m_out.print(" "_s);
    printModuleName(position.source);
    m_out.print(":"_s, position.line, ":"_s, position.column);
    if (position.inlineFrame)
        m_out.print(" in inline frame "_s, position.inlineFrame);
}

static void printHexInBytesText(PrintStream& out, std::span<const uint8_t> bytes)
{
    for (uint8_t byte : bytes)
        out.printf("%02x", byte);
}

static void printHashInBytesText(PrintStream& out, const CodeBody& body)
{
    Vector<uint8_t> bytes(body.bytes.span());
    for (const Relocation& relocation : body.relocations)
        MacroAssembler::clearRelocation(relocation.kind, bytes.mutableSpan().subspan(relocation.offset, MacroAssembler::relocationSize(relocation.kind)));

    WTF::SHA1 hash;
    hash.addBytes(bytes.span());
    WTF::SHA1::Digest digest;
    hash.computeHash(digest);
    out.print("  hash "_s);
    printHexInBytesText(out, digest);
    out.println();
}

static void printSummaryInBytesText(PrintStream& out, const CompiledCode& code)
{
    const CodeBody& body = code.body;
    const FrameDescription& frame = code.frame;
    const SiteDescription& sites = code.sites;

    out.print("  size="_s, body.bytes.size(), " frame="_s);
    if (frame.hasFrame()) {
        out.print(frame.frameSizeInBytes);
        ASCIILiteral separator = "@"_s;
        for (const CodeRange& range : frame.frameRanges) {
            out.print(separator, range.begin, "-"_s, range.end);
            separator = ","_s;
        }
    } else
        out.print("none"_s);
    out.print(" entries="_s, body.entries.size(), " sites="_s, sites.positions.isEmpty() ? 0 : sites.positions.size() - 1, " handlers="_s, sites.rareLists->handlers.size());
    out.print(" faults="_s, sites.faultingAccesses.size(), " polls="_s, sites.rareLists->pollSites.size());
    out.print(" constants="_s, body.rareLists->constants.size(), " jumpTables="_s, body.rareLists->jumpTables.size(), " prologueChecks="_s, frame.prologueChecks.size());
    out.print(" relocations="_s, body.relocations.size(), " returns="_s, body.returnSites.size(), " thunks="_s, body.argumentMoves.size(), " inlineFrames="_s, sites.inlineFrames.size());
    out.print(" stackUsedAtCalls="_s, frame.stackUsedAtCalls, " aboveFrameRecord="_s, frame.bytesAboveFrameRecord, " deadAtSuspensions="_s, frame.bytesDeadAtSuspensions);
    if (frame.isEnteredWithoutStackCheck)
        out.print(" enteredWithoutStackCheck"_s);
    if (frame.isProvenToFitOnStack)
        out.print(" provenToFitOnStack"_s);
    if (!frame.hasFrameRecord)
        out.print(" noFrameRecord"_s);
    if (frame.spillsArgumentRegisters)
        out.print(" spillsArgumentRegisters"_s);
    out.println();

    out.print("  parameters="_s, static_cast<unsigned>(code.entry.numberOfParameters), " borrowedArguments="_s, static_cast<unsigned>(code.entry.borrowedArguments));
    if (code.entry.endsItsTailCalls)
        out.print(" endsItsTailCalls"_s);
    if (code.entry.isGenericEntrypoint)
        out.print(" genericEntrypoint"_s);
    out.println();
}

static void printArgumentThunkInBytesText(PrintStream& out, std::span<const uint64_t> words)
{
    if (words.size() < numberOfHeaderWords) {
        out.print("?"_s);
        return;
    }
    printStubInBytesText(out, StubKey::fromBits(words[0]));
    if (words[1] & hasImmediate)
        out.print(words[1] & immediateIsMention ? " immediate mention "_s : " immediate "_s, static_cast<uint32_t>(words[1]));
    for (size_t index = numberOfHeaderWords; index + 1 < words.size(); index += 2) {
        auto source = static_cast<ArgumentSource>(words[index] & 0xff);
        uint64_t payload = words[index + 1];
        out.print(", "_s, static_cast<GPRReg>(words[index] >> 8), " = "_s);
        if (source == ArgumentSource::Register)
            out.print(static_cast<GPRReg>(payload));
        else
            out.print(WTF::enumName(source), " "_s, source == ArgumentSource::Constant ? static_cast<int64_t>(payload) : static_cast<int64_t>(static_cast<int32_t>(payload)));
    }
}

static void printListsOfBodyInBytesText(NamesInBytesText& names, const CodeBody& body)
{
    PrintStream& out = names.out();
    for (const Relocation& relocation : body.relocations) {
        out.print("  relocation "_s, relocation.offset, " "_s, WTF::enumName(relocation.kind), " "_s);
        names.printSymbol(body, relocation);
        if (relocation.space == SymbolSpace::Stub && relocation.entry == entryInFrontOfStubThatDoesTwoMoves)
            out.println(", entered in front of two moves"_s);
        else
            out.println(relocation.space == SymbolSpace::Stub && relocation.entry == entryInFrontOfStub ? ", entered in front"_s : ""_s);
    }
    for (uint32_t index = 0; index < body.mentions.size(); ++index) {
        bool isNamedByCode = std::ranges::any_of(body.relocations, [&](const Relocation& relocation) {
            return relocation.space == SymbolSpace::Number && relocation.symbol == index;
        });
        if (body.mentions[index].space != NumberSpace::ClosureCell || isNamedByCode)
            continue;
        out.print("  mention without code "_s);
        names.printMention(body, index);
        out.println();
    }
    for (size_t index = 0; index < body.argumentMoves.size(); ++index) {
        out.print("  argument thunk "_s, index, " "_s);
        printArgumentThunkInBytesText(out, body.argumentMoves[index]);
        out.println();
    }
    const RareListsOfBody& rareLists = *body.rareLists;
    for (size_t index = 0; index < rareLists.constants.size(); ++index) {
        out.print("  constant "_s, index, " align "_s, rareLists.constants[index].alignment, " "_s, WTF::enumName(rareLists.constants[index].kind), " "_s);
        printHexInBytesText(out, rareLists.constants[index].bytes.span());
        out.println();
    }
    for (size_t index = 0; index < rareLists.jumpTables.size(); ++index) {
        out.print("  jump table "_s, index);
        for (uint32_t target : rareLists.jumpTables[index].targets)
            out.print(" "_s, target);
        out.println();
    }
}

static void printSitesInBytesText(NamesInBytesText& names, const CodeBody& body, const SiteDescription& sites)
{
    PrintStream& out = names.out();
    for (const ReturnSite& site : body.returnSites) {
        out.print("  return "_s, site.offset);
        names.printSite(sites, site.site);
        out.println();
    }
    for (const FirstReadSite& read : body.firstReadSites) {
        out.print("  first read returns at "_s, read.returnOffset);
        names.printSite(sites, read.site ? OptionalIndex { read.site } : OptionalIndex { });
        out.println(" columns "_s, read.columnDelta);
    }
    for (const FlaggedSite& flagged : sites.flaggedSites) {
        out.print(" "_s);
        names.printSite(sites, OptionalIndex { flagged.site });
        out.println(" is "_s, WTF::enumName(flagged.kind));
    }
    for (const ConstructedCallee& constructed : sites.rareLists->constructedCallees) {
        out.print("  constructs "_s);
        if (constructed.callee < body.callees.size())
            names.printUnitName(body.callees[constructed.callee]);
        else
            out.print("?"_s, constructed.callee);
        out.print(" at"_s);
        names.printSite(sites, OptionalIndex { constructed.site });
        out.println();
    }
    for (const SiteHandler& handler : sites.rareLists->handlers) {
        out.print("  handler "_s, WTF::enumName(handler.type), " at "_s, handler.codeOffset, " for"_s);
        names.printSite(sites, OptionalIndex { handler.site });
        out.println();
    }
    for (const FaultingAccess& access : sites.faultingAccesses) {
        out.print("  fault "_s, access.codeOffset);
        names.printSite(sites, access.callSite);
        out.print(" throws"_s);
        for (size_t index = 0; index < access.throwSite.size(); ++index) {
            if (!(access.wordsThatAreMentions >> index & 1)) {
                out.print(" "_s, access.throwSite[index]);
                continue;
            }
            out.print(" ("_s);
            names.printMention(body, access.throwSite[index]);
            out.print(")"_s);
        }
        out.println();
    }
    for (const PollSite& poll : sites.rareLists->pollSites) {
        out.print("  poll "_s, poll.offset);
        if (poll.callerIsFrameless)
            out.print(" frameless"_s);
        names.printSite(sites, poll.callSite);
        out.println();
    }
    if (!sites.rareLists->frameMapWords.isEmpty()) {
        out.print("  frame map"_s);
        for (uint32_t word : sites.rareLists->frameMapWords)
            out.printf(" %08x", word);
        out.println();
    }
}

static void printCompiledCodeAsBytesText(NamesInBytesText& names, const CompiledCode& code)
{
    PrintStream& out = names.out();
    if (code.body.bytes.isEmpty()) {
        out.println("  no code"_s);
        return;
    }

    printSummaryInBytesText(out, code);
    printHashInBytesText(out, code.body);
    for (const CodeEntry& entry : code.body.entries)
        out.println("  entry "_s, entry.id, " at "_s, entry.offset);
    for (const RegisterAtOffset& saved : code.frame.calleeSaveRegisters)
        out.println("  saves "_s, saved.reg(), " at "_s, saved.offset());
    for (const PrologueCheck& check : code.frame.prologueChecks)
        out.println("  prologue check calls at "_s, check.callOffset, " and resumes at "_s, check.resumeOffset);
    printListsOfBodyInBytesText(names, code.body);
    printSitesInBytesText(names, code.body, code.sites);
}

static void runMachineBytesCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    std::optional<Vector<CodeUnit>> units = unitsOfMachineCommand(context, stage);
    if (!units)
        return;
    if (!stage.plan()) {
        context.out.println(lineOfFailedMachineCommand);
        return;
    }

    NamesInBytesText names(context.out, stage.program());
    for (CodeUnit unit : *units) {
        printHeadingOfMachineUnit(context.out, unit);
        context.out.print(" "_s);
        names.printFunctionName(unit.function);
        context.out.println();
        const CompiledCode* code = stage.compiledCode(unit);
        if (!code) {
            context.out.println(lineOfFailedMachineCommand);
            return;
        }
        printCompiledCodeAsBytesText(names, *code);
    }
}

static_assert(namedFormOf(Opcode::Mod, StateOfKnowledge::NumbersAndOtherDivisor, Place::Anywhere) == Form::Runtime && namedFormOf(Opcode::Mod, StateOfKnowledge::Numbers, Place::Anywhere) == Form::Open);
static_assert(!namedFormOf(Opcode::Mod, StateOfKnowledge::Always, Place::Anywhere) && !namedFormOf(Opcode::Mod, StateOfKnowledge::Strings, Place::Anywhere));
static_assert(namedFormOf(Opcode::Call, StateOfKnowledge::Always, Place::Anywhere) == Form::Closed && !namedFormOf(Opcode::Call, StateOfKnowledge::Generic, Place::Anywhere));
static_assert(!namedFormOf(Opcode::Call, StateOfKnowledge::Always, Place::Loop) && namedFormOf(Opcode::PropGet, StateOfKnowledge::Always, Place::Loop) == Form::Open);
static_assert(namedFormOf(Opcode::Box, StateOfKnowledge::Always, Place::Anywhere) == Form::Open && namedFormOf(Opcode::Box, StateOfKnowledge::Always, Place::RareBlock) == Form::Closed);

static uint64_t keyOfRowInBytesByFormText(const BytesOfForm& row)
{
    return static_cast<uint64_t>(row.opcode) << 32 | static_cast<uint64_t>(row.state) << 16 | static_cast<uint64_t>(row.place) << 8 | static_cast<uint64_t>(row.form);
}

static String nameOfRowInBytesByFormText(uint64_t key)
{
    return makeString(nameOf(static_cast<Opcode>(key >> 32)), ' ', nameOf(static_cast<StateOfKnowledge>(key >> 16 & 0xFFFF)), ' ', nameOf(static_cast<Place>(key >> 8 & 0xFF)), ' ', nameOf(formReportedAs(static_cast<Wire::CodeForm>(key & 0xFF))));
}

static ASCIILiteral questionInBytesByFormTextAbout(Place place)
{
    switch (place) {
    case Place::Anywhere:
    case Place::Loop:
        break;
    case Place::UnlikelyBlock:
    case Place::RareBlock:
        return "Block::frequency"_s;
    case Place::CodeThatRunsOnce:
        return "Facts::runsAtMostOnce(), AccessEmitter::comesAgain()"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<FormSaidFalsely> parseFormSaidFalsely(StringView word)
{
    size_t colon = word.find(':');
    if (colon == notFound)
        return std::nullopt;
    std::optional<Opcode> opcode = parseEnum<Opcode>(word.left(colon));
    std::optional<Form> form = parseEnum<Form>(word.substring(colon + 1));
    if (!opcode || !form)
        return std::nullopt;
    return FormSaidFalsely { *opcode, *form };
}

static void runMachineBytesByFormCommand(TestCommandContext& context)
{
    constexpr ASCIILiteral control = "says="_s;
    Vector<CodeRole, 2> roles;
    std::optional<FormSaidFalsely> saidFalsely;
    for (const String& word : context.arguments) {
        if (word.startsWith(control)) {
            saidFalsely = parseFormSaidFalsely(StringView(word).substring(control.length()));
            if (saidFalsely)
                continue;
        }
        std::optional<CodeRole> role = parseEnum<CodeRole>(word);
        if (!role) {
            context.out.println("bad argument "_s, word);
            return;
        }
        roles.append(*role);
    }

    if (!AOT::hasFlag(context.build.options().reports, Wire::ReportSet::Flow)) {
        context.out.println("bytes by form are counted under report=Flow alone"_s);
        return;
    }

    struct TotalOfRow {
        TotalOfCodeForm all;
        uint64_t countNotNamed { 0 };
        uint64_t countThatLooksOtherwise { 0 };
        uint64_t countLongerThanCall { 0 };
        uint64_t countFoundOpen { 0 };
        uint64_t countFoundClosed { 0 };
        uint64_t countFoundRuntime { 0 };
        bool isChosenForColdCode { false };
    };

    setFormSaidFalsely(saidFalsely);
    auto endControl = makeScopeExit([] {
        setFormSaidFalsely(std::nullopt);
    });
    StagedBuild stage = context.stage();
    UncheckedKeyHashMap<uint64_t, TotalOfRow, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> totals;
    TotalOfCodeForm totalOfAllForms;
    uint64_t codeFormSchemaMismatches = 0;
    uint64_t coldCodeFormsChosen = 0;
    uint64_t bytesOfCode = 0;
    uint64_t bytesInInlineFrames = 0;
    uint64_t bytesInInlineFramesOfLibrary = 0;
    uint64_t bytesInFramesOfBuiltins = 0;
    uint64_t numberOfUnits = 0;
    uint64_t numberOfInlineFrames = 0;
    for (CodeUnit unit : stage.unitsToCompile()) {
        if (!roles.isEmpty() && !roles.contains(unit.role))
            continue;
        const CompiledCode* code = stage.compiledCode(unit);
        if (!code)
            break;
        ++numberOfUnits;
        numberOfInlineFrames += code->sites.inlineFrames.size();
        bytesOfCode += code->body.bytes.size();
        bytesInInlineFrames += code->measurements->bytesInInlineFrames;
        bytesInInlineFramesOfLibrary += code->measurements->bytesInInlineFramesOfLibrary;
        bytesInFramesOfBuiltins += code->measurements->bytesInFramesOfBuiltins;
        for (const BytesOfForm& row : code->measurements->bytesByForm) {
            TotalOfRow& total = totals.add(keyOfRowInBytesByFormText(row), TotalOfRow { }).iterator->value;
            total.all.count += row.count;
            total.all.bytes += row.bytes;
            total.countNotNamed += row.isNamed ? 0 : row.count;
            total.countThatLooksOtherwise += row.countThatLooksOtherwise;
            total.countLongerThanCall += row.countLongerThanCall;
            total.countFoundOpen += row.countFoundOpen;
            total.countFoundClosed += row.countFoundClosed;
            total.countFoundRuntime += row.countFoundRuntime;
            total.isChosenForColdCode = row.isChosenForColdCode();
            codeFormSchemaMismatches += row.schemaMismatchCount;
            coldCodeFormsChosen += row.isChosenForColdCode() ? row.count : 0;
            totalOfAllForms.count += row.count;
            totalOfAllForms.bytes += row.bytes;
        }
    }
    if (!stage.prepared()) {
        context.out.println(lineOfFailedMachineCommand);
        return;
    }

    Vector<uint64_t> keys = copyToVector(totals.keys());
    std::ranges::sort(keys);
    for (uint64_t key : keys) {
        const TotalOfCodeForm& total = totals.find(key)->value.all;
        context.out.println(nameOfRowInBytesByFormText(key), " "_s, total.count, " "_s, total.bytes, " "_s, total.bytes / total.count);
    }
    uint64_t rowsNotNamed = 0;
    for (uint64_t key : keys) {
        uint64_t count = totals.find(key)->value.countNotNamed;
        if (!count)
            continue;
        ++rowsNotNamed;
        context.out.println("not named: "_s, nameOfRowInBytesByFormText(key), " "_s, count);
    }
    for (uint64_t key : keys) {
        const TotalOfRow& total = totals.find(key)->value;
        if (total.countThatLooksOtherwise)
            context.out.println("looks otherwise: "_s, nameOfRowInBytesByFormText(key), " "_s, total.countThatLooksOtherwise, " found Open "_s, total.countFoundOpen, " Closed "_s, total.countFoundClosed, " Runtime "_s, total.countFoundRuntime);
    }
    for (uint64_t key : keys) {
        if (uint64_t count = totals.find(key)->value.countLongerThanCall)
            context.out.println("longer than a call: "_s, nameOfRowInBytesByFormText(key), " "_s, count);
    }
    for (uint64_t key : keys) {
        const TotalOfRow& total = totals.find(key)->value;
        if (total.isChosenForColdCode)
            context.out.println("picked by the place: "_s, nameOfRowInBytesByFormText(key), " "_s, total.all.count, " "_s, total.all.bytes, " asked by "_s, questionInBytesByFormTextAbout(static_cast<Place>(key >> 8 & 0xFF)));
    }
    context.out.println("forms that differ from the schema "_s, codeFormSchemaMismatches);
    context.out.println("forms picked by a place other than a loop "_s, coldCodeFormsChosen);
    if (rowsNotNamed)
        context.out.println("forms not named "_s, rowsNotNamed);
    else
        context.out.println("every form is named"_s);
    context.out.println("all forms "_s, totalOfAllForms.count, " "_s, totalOfAllForms.bytes);
    context.out.println("in inline frames "_s, bytesInInlineFrames);
    context.out.println("in inline frames of the library "_s, bytesInInlineFramesOfLibrary);
    context.out.println("in frames of built-ins "_s, bytesInFramesOfBuiltins);
    context.out.println("all code "_s, bytesOfCode);
    if (!roles.isEmpty()) {
        context.out.println("units "_s, numberOfUnits);
        context.out.println("inline frames "_s, numberOfInlineFrames);
    }
}

static void runMachineNamedFormsCommand(TestCommandContext& context)
{
    for (const String& word : context.arguments) {
        std::optional<Opcode> opcode = parseEnum<Opcode>(word);
        if (!opcode) {
            context.out.println("bad argument "_s, word);
            return;
        }
        for (StateOfKnowledge state : allMembersOf<StateOfKnowledge>) {
            std::optional<Form> named = namedFormOf(*opcode, state, Place::Anywhere);
            if (!named)
                continue;
            FormAtPlace outsideLoops = formByLoopAlone(*opcode, state, false);
            FormAtPlace inLoop = formByLoopAlone(*opcode, state, true);
            RELEASE_ASSERT(outsideLoops.form == *named && outsideLoops.place == Place::Anywhere);
            context.out.println(nameOf(*opcode), " "_s, nameOf(state), " "_s, nameOf(*named), ", in a loop "_s, nameOf(inLoop.form), inLoop.place == Place::Loop ? " by the loop"_s : ""_s);
        }
        for (Place place : allMembersOf<Place>) {
            if (place == Place::Anywhere)
                continue;
            if (std::optional<Form> named = namedFormOf(*opcode, StateOfKnowledge::Always, place))
                context.out.println(nameOf(*opcode), " in "_s, nameOf(place), " "_s, nameOf(*named), place == Place::Loop ? ""_s : ", which takesOpenForm() does not ask"_s);
        }
    }
}

static void runMachineBytesOutsideOperationsCommand(TestCommandContext& context)
{
    Vector<CodeRole, 2> roles;
    for (const String& word : context.arguments) {
        std::optional<CodeRole> role = parseEnum<CodeRole>(word);
        if (!role) {
            context.out.println("bad argument "_s, word);
            return;
        }
        roles.append(*role);
    }

    if (!AOT::hasFlag(context.build.options().reports, Wire::ReportSet::Flow)) {
        context.out.println("bytes outside operations are counted under report=Flow alone"_s);
        return;
    }

    StagedBuild stage = context.stage();
    UncheckedKeyHashMap<uint64_t, TotalOfCodeForm, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> totals;
    TotalOfCodeForm totalOfAllRows;
    uint64_t bytesOfCode = 0;
    for (CodeUnit unit : stage.unitsToCompile()) {
        if (!roles.isEmpty() && !roles.contains(unit.role))
            continue;
        const CompiledCode* code = stage.compiledCode(unit);
        if (!code)
            break;
        bytesOfCode += code->body.bytes.size();
        for (const BytesOutsideOperations& row : code->measurements->bytesOutsideOperations) {
            TotalOfCodeForm& total = totals.add(static_cast<uint64_t>(row.origin) << 1 | row.isRare, TotalOfCodeForm { }).iterator->value;
            total.count += row.count;
            total.bytes += row.bytes;
            totalOfAllRows.count += row.count;
            totalOfAllRows.bytes += row.bytes;
        }
    }
    if (!stage.prepared()) {
        context.out.println(lineOfFailedMachineCommand);
        return;
    }

    Vector<uint64_t> keys = copyToVector(totals.keys());
    std::ranges::sort(keys);
    for (uint64_t key : keys) {
        StringPrintStream what;
        printOriginOutsideOperations(what, static_cast<uint32_t>(key >> 1));
        const TotalOfCodeForm& total = totals.find(key)->value;
        context.out.println(what.toString(), key & 1 ? " rare "_s : " main "_s, total.count, " "_s, total.bytes);
    }
    context.out.println("all rows "_s, totalOfAllRows.count, " "_s, totalOfAllRows.bytes);
    context.out.println("all code "_s, bytesOfCode);
}

static void runMachineRangesCommand(TestCommandContext& context)
{
    if (!AOT::hasFlag(context.build.options().reports, Wire::ReportSet::Flow)) {
        context.out.println("ranges are noted under report=Flow alone"_s);
        return;
    }

    StagedBuild stage = context.stage();
    std::optional<Vector<CodeUnit>> units = unitsOfMachineCommand(context, stage);
    if (!units)
        return;
    for (CodeUnit unit : *units) {
        printHeadingOfMachineUnit(context.out, unit);
        context.out.println();
        const CompiledCode* code = stage.compiledCode(unit);
        if (!code) {
            context.out.println(lineOfFailedMachineCommand);
            return;
        }
        for (const RangeOfCode& range : code->measurements->ranges) {
            context.out.print(range.begin, " "_s, range.end, range.isRare ? " rare "_s : " main "_s);
            if (range.value)
                context.out.print(nameOf(range.opcode), " v"_s, range.value.index(), " frame "_s, range.inlineFrame, range.standsInRareBlock ? " rareBlock"_s : " plainBlock"_s);
            else
                printOriginOutsideOperations(context.out, range.origin);
            context.out.println();
        }
    }
}

std::span<const TestCommand> machineTestCommands()
{
    static constexpr TestCommand commands[] {
        { "emit"_s, runMachineEmitCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
        { "emit-code"_s, runMachineEmitCodeCommand, ProgramOfCommand::WithLibrary },
        { "emit-forms-of-accesses-to-elements"_s, runMachineEmitFormsOfAccessesToElementsCommand, ProgramOfCommand::WithLibrary },
        { "order"_s, runMachineOrderCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
        { "liveness-of-operations"_s, runMachineLivenessCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
        { "allocate"_s, runMachineAllocateCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
        { "frame"_s, runMachineFrameCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
        { "loops"_s, runMachineLoopsCommand, ProgramOfCommand::WithLibrary, "[most:n,n..] unit.."_s },
        { "steps"_s, runMachineStepsCommand, ProgramOfCommand::WithLibrary, "[most:n,n..] unit.."_s },
        { "bytes"_s, runMachineBytesCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
        { "bytes-by-form"_s, runMachineBytesByFormCommand, ProgramOfCommand::WithLibrary, "[says=operation:form] role.."_s },
        { "named-forms"_s, runMachineNamedFormsCommand, ProgramOfCommand::None, "operation.."_s },
        { "bytes-outside-operations"_s, runMachineBytesOutsideOperationsCommand, ProgramOfCommand::WithLibrary, "role.."_s },
        { "ranges"_s, runMachineRangesCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

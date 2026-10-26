/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementTestCommands.h"

#if ENABLE(AOT)

#include "AOTClassMemberTable.h"
#include "AOTCodeGenerationDriver.h"
#include "AOTFieldTable.h"
#include "AOTFunctionDescription.h"
#include "AOTIdentifierNumbering.h"
#include "AOTIdentifierTable.h"
#include "AOTObjectVisiting.h"
#include "AOTPlacementTestSupport.h"
#include "AOTProgramTableEncoder.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeType.h"
#include "AOTTableInImage.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include <algorithm>
#include <bit>
#include <cinttypes>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TablesContext {
    PrintStream& out;
    IRPrinter& printer;
    const Program& program;
    const Placement& placement;
    const ProgramTables& tables;
    std::span<const uint8_t> bytes;
    const BitVector& namedStrings;
    const ProgramTableInputs& inputs;
    BuildContext& build;
};

struct TablesView {
    ASCIILiteral name;
    void (*print)(const TablesContext&);
};

struct TableDamage {
    ProgramTableRule rule;
    bool (*apply)(std::span<uint8_t> bytes);
};

struct FlagName {
    uint32_t mask;
    ASCIILiteral name;
};

} // anonymous namespace

#define AOT_FIELD_FLAG_NAME(name) FlagName { field##name, #name ""_s }
static constexpr FlagName fieldFlagNames[] {
    AOT_FIELD_FLAG_NAME(IsOptional), AOT_FIELD_FLAG_NAME(IsOutOfLine), AOT_FIELD_FLAG_NAME(IsStoredOnce),
    AOT_FIELD_FLAG_NAME(MayExistWithoutStorage), AOT_FIELD_FLAG_NAME(DeferredClosureCapturesObject), AOT_FIELD_FLAG_NAME(DeferredClosureCapturesEnvironment),
    AOT_FIELD_FLAG_NAME(IsPrivate), AOT_FIELD_FLAG_NAME(IsBrand), AOT_FIELD_FLAG_NAME(IsNeverDeleted), AOT_FIELD_FLAG_NAME(IsNeverRedefined),
    AOT_FIELD_FLAG_NAME(IsNeverStoredWithOtherType), AOT_FIELD_FLAG_NAME(IsReadOnly), AOT_FIELD_FLAG_NAME(IsNotEnumerable),
    AOT_FIELD_FLAG_NAME(IsNotConfigurable), AOT_FIELD_FLAG_NAME(IsAccessor),
};
#undef AOT_FIELD_FLAG_NAME

#define AOT_METHOD_FLAG_NAME(name) FlagName { method##name, #name ""_s }
static constexpr FlagName methodFlagNames[] {
    AOT_METHOD_FLAG_NAME(HasComputedName), AOT_METHOD_FLAG_NAME(TakesEnvironment), AOT_METHOD_FLAG_NAME(IsCreatedAtDefinition),
};
#undef AOT_METHOD_FLAG_NAME

static constexpr uint32_t maskOf(std::span<const FlagName> flags)
{
    uint32_t mask = 0;
    for (const FlagName& flag : flags)
        mask |= flag.mask;
    return mask;
}

static constexpr uint16_t unknownFieldFlag = allFieldFlags + 1;
static_assert(maskOf(fieldFlagNames) == allFieldFlags);
static_assert(unknownFieldFlag && !(unknownFieldFlag & allFieldFlags));

static ASCIILiteral nameOf(CallableKind kind)
{
    switch (kind) {
    case CallableKind::Plain:
        return "Plain"_s;
    case CallableKind::Arrow:
        return "Arrow"_s;
    case CallableKind::Method:
        return "Method"_s;
    case CallableKind::Bound:
        return "Bound"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(TraceVisibility visibility)
{
    switch (visibility) {
    case TraceVisibility::Visible:
        return "Visible"_s;
    case TraceVisibility::VisibleOnlyOnTop:
        return "VisibleOnlyOnTop"_s;
    case TraceVisibility::Hidden:
        return "Hidden"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(StackTraceBuiltinKind kind)
{
    switch (kind) {
    case StackTraceBuiltinKind::None:
        return "None"_s;
    case StackTraceBuiltinKind::JSBuiltin:
        return "JSBuiltin"_s;
    case StackTraceBuiltinKind::JSBuiltinConstructor:
        return "JSBuiltinConstructor"_s;
    case StackTraceBuiltinKind::HostFunction:
        return "HostFunction"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printIdentifier(const TablesContext& context, uint32_t number)
{
    const IdentifierNumbering& identifiers = context.build.identifierNumbering();
    if (!number || number >= identifiers.count()) {
        context.out.print(number ? "<invalid identifier>"_s : "-"_s);
        return;
    }
    const NumberedIdentifier& identifier = identifiers.at(number);
    if (identifier.key)
        context.printer.print(identifier.key);
    else
        context.printer.print(identifier.text);
}

static void printStringConstant(const TablesContext& context, uint32_t number)
{
    const StringConstantNumbering& constants = context.inputs.stringConstants;
    context.printer.print(number < constants.count() ? constants.stringOf(number) : StringID());
}

template<typename T>
static void printValue(const TablesContext& context, const T& value)
{
    context.printer.print(value);
}

static void printValue(const TablesContext& context, ASCIILiteral text)
{
    context.out.print(text);
}

static void printValue(const TablesContext& context, SlotData slot)
{
    context.printer.print(slot.representation());
    context.out.print("@", static_cast<uint32_t>(slot.offset));
}

static void printValue(const TablesContext& context, const ShapeTestData& test)
{
    context.out.print(test.count ? test.first() : 0, "+", test.count);
    if (test.bitSetOffset != noBitSet)
        context.out.print("@", test.bitSetOffset);
}

template<typename T>
static void printValue(const TablesContext& context, const std::optional<T>& value)
{
    if (value)
        printValue(context, *value);
    else
        context.out.print("-");
}

namespace {

class RecordLine {
    WTF_MAKE_NONCOPYABLE(RecordLine);
public:
    RecordLine(const TablesContext& context, ASCIILiteral keyword, std::optional<uint32_t> number = std::nullopt)
        : m_context(context)
    {
        context.printer.beginLine(keyword);
        if (number)
            context.out.print(" ", *number);
    }

    RecordLine(const TablesContext& context, ASCIILiteral keyword, IDPrefix prefix, uint32_t index)
        : m_context(context)
    {
        context.printer.beginLine(keyword, prefix, index);
    }

    ~RecordLine() { m_context.printer.endLine(); }

    RecordLine& word(ASCIILiteral text)
    {
        m_context.out.print(" ", text);
        return *this;
    }

    RecordLine& fieldWith(ASCIILiteral name, NOESCAPE const auto& print)
    {
        m_context.printer.beginField(name);
        print();
        return *this;
    }

    template<typename T> RecordLine& field(ASCIILiteral name, const T& value) { return fieldWith(name, [&] { printValue(m_context, value); }); }
    template<typename Range> RecordLine& list(ASCIILiteral name, const Range& items, NOESCAPE const auto& printItem) { return fieldWith(name, [&] { printList(m_context.out, items, printItem); }); }
    template<typename Range> RecordLine& numbers(ASCIILiteral name, const Range& items) { return list(name, items, [&](uint32_t number) { m_context.out.print(number); }); }
    template<typename Range> RecordLine& strings(ASCIILiteral name, const Range& items) { return list(name, items, [&](uint32_t number) { m_context.printer.print(StringID(number)); }); }
    RecordLine& flag(ASCIILiteral name, bool isSet) { return isSet ? word(name) : *this; }
    RecordLine& string(ASCIILiteral name, uint32_t number) { return field(name, StringID(number)); }
    RecordLine& identifier(ASCIILiteral name, uint32_t number) { return fieldWith(name, [&] { printIdentifier(m_context, number); }); }
    RecordLine& hex(ASCIILiteral name, uint64_t bits) { return fieldWith(name, [&] { m_context.out.printf("0x%" PRIx64, bits); }); }

    RecordLine& flags(ASCIILiteral name, uint32_t bits, std::span<const FlagName> flagNames)
    {
        Vector<ASCIILiteral, 16> namesOfSetFlags;
        for (const FlagName& flagName : flagNames) {
            if (bits & flagName.mask)
                namesOfSetFlags.append(flagName.name);
        }
        return list(name, namesOfSetFlags, [&](ASCIILiteral flagName) {
            m_context.out.print(flagName);
        });
    }

private:
    const TablesContext& m_context;
};

} // anonymous namespace

static void printTableSizes(const TablesContext& context)
{
    PrintStream& out = context.out;
    out.println(context.tables.magic == ProgramTables::expectedMagic ? "magic ok"_s : "magic wrong"_s);
    out.println("instances ", (context.tables.flags & ProgramTables::mayRunInMultipleInstances) ? "many"_s : "one"_s);
#define AOT_PRINT_PROGRAM_TABLE_SIZE(accessor, ElementType) \
    out.println(#accessor " ", context.tables.accessor().size(), " ", context.tables.accessor().size_bytes());
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_PRINT_PROGRAM_TABLE_SIZE)
#undef AOT_PRINT_PROGRAM_TABLE_SIZE
    out.println("namedStrings ", context.namedStrings.bitCount());
    out.println("total ", context.bytes.size());
}

static void printBuiltinCell(const TablesContext& context, uint32_t number)
{
    auto cells = context.tables.builtinCells();
    std::optional<BuiltinCellKey> key = number < cells.size() ? BuiltinCellKey::decode(cells[number], context.program.builtins()) : std::nullopt;
    if (!key) {
        context.out.print("<invalid builtin cell>");
        return;
    }
    switch (key->kind()) {
    case BuiltinCellKey::Kind::Object:
        break;
    case BuiltinCellKey::Kind::Getter:
        context.out.print("get:");
        break;
    case BuiltinCellKey::Kind::Setter:
        context.out.print("set:");
        break;
    case BuiltinCellKey::Kind::Value:
        context.out.print("value:");
        break;
    }
    context.printer.print(key->builtin());
}

static void printCellTest(const TablesContext& context, const RuntimeCellTest& test)
{
    context.printer.print(test.kind);
    context.out.print("(");
    switch (test.kind) {
    case CellTestKind::IsObject:
    case CellTestKind::IsCallable:
        break;
    case CellTestKind::CellTypeIs:
        context.out.print(static_cast<unsigned>(test.firstCellType), ",", static_cast<unsigned>(test.lastCellType));
        break;
    case CellTestKind::ShapeIn:
        printValue(context, context.tables.shapeTestAt(test.shapeTestOffset()));
        break;
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
        context.printer.print(test.builtinClass());
        break;
    case CellTestKind::IsBuiltinCell:
        printBuiltinCell(context, test.builtinCellNumber());
        break;
    case CellTestKind::IsClosureOf:
        context.printer.print(test.function());
        break;
    case CellTestKind::IsSymbol:
        printIdentifier(context, test.symbolKeyNumber());
        break;
    case CellTestKind::SharingModeIs:
        context.out.print(test.sharingMode());
        break;
    case CellTestKind::IsNamespaceOf:
        context.printer.print(test.module());
        break;
    }
    context.out.print(")");
}

static void printRuntimeType(const TablesContext& context, uint32_t type, uint32_t offset)
{
    const RuntimeType& record = context.tables.typeAt(offset);
    RecordLine line(context, "type"_s);
    context.out.print(" ");
    context.printer.print(TypeID(type));
    line.field("offset"_s, offset).hex("primitives"_s, record.primitives);
    line.list("tests"_s, record.cellTests(), [&](const RuntimeCellTest& test) {
        printCellTest(context, test);
    });
    line.strings("literals"_s, record.literalStrings());
    if (auto range = record.integerRange())
        line.field("minimum"_s, range->first).field("maximum"_s, range->second);
    line.field("contents"_s, record.contents()).field("set"_s, record.layoutSet());
}

static void printTypes(const TablesContext& context)
{
    for (uint32_t type = 0; type < context.tables.numberOfTypes(); ++type) {
        if (uint32_t offset = context.tables.offsetOfType(type))
            printRuntimeType(context, type, offset);
    }
    auto sets = context.tables.layoutSets();
    for (uint32_t index = 0; index < sets.size(); ++index) {
        const LayoutSetData& set = sets[index];
        RecordLine(context, "set"_s, index).string("name"_s, set.name).field("test"_s, set.members);
    }
    for (const TypeSiteData& site : context.tables.typeSites()) {
        StringID text = site.text == TypeSiteData::noText ? StringID() : StringID(static_cast<uint32_t>(site.text));
        RecordLine(context, "site"_s, IDPrefix::TypeSite, site.site).field("module"_s, ModuleID(site.module)).field("position"_s, site.position).field("kind"_s, static_cast<TypeSiteKind>(site.kind)).field("text"_s, text);
    }
}

static void printFieldRecords(const TablesContext& context, ASCIILiteral keyword, const LayoutData& layout, std::span<const FieldData> fields)
{
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const FieldData& field = fields[index];
        RecordLine line(context, keyword, index);
        line.identifier("name"_s, field.name).flags("flags"_s, field.flags, fieldFlagNames).field("slot"_s, field.slot).field("type"_s, field.type);
        line.flag("isFilledAtBirth"_s, field.isFilledAtBirth).flag("holdsRawValueFromBirth"_s, field.holdsRawValueFromBirth).flag("keepsStateOfBirth"_s, field.keepsStateOfBirth);
        if (field.slot.representation() != SlotRepresentation::DeferredClosure)
            continue;
        line.field("function"_s, layout.deferredClosureOf(field));
        if (field.flags & fieldDeferredClosureCapturesEnvironment)
            line.field("environment"_s, layout.closureEnvironmentSlotOf(field));
    }
}

static void printReferences(const TablesContext& context, std::span<const uint32_t> references)
{
    if (references.empty())
        return;
    auto takeFirst = [&](size_t count) {
        std::span<const uint32_t> taken = references.first(count);
        references = references.subspan(count);
        return taken;
    };
    RecordLine line(context, "  references"_s);
    std::span<const uint32_t> counts = takeFirst(numberOfInlineReferenceCounts);
    line.numbers("values"_s, takeFirst(counts[0]));
    line.numbers("cells"_s, takeFirst(counts[1]));
    line.numbers("narrow"_s, takeFirst(counts[2]));
    if (references.empty())
        return;
    std::span<const uint32_t> header = takeFirst(numberOfExtensionHeaderWords);
    line.field("extension"_s, SlotData::decode(header[0]));
    ExtensionReferences sets = extensionReferencesAt(header.data());
    Vector<uint32_t> wideOutOfLine;
    Vector<uint32_t> narrowOutOfLine;
    for (uint32_t granule = 0; granule < maxExtensionGranules; ++granule) {
        uint32_t offset = offsetOfFirstExtensionSlot + granule * extensionGranuleSize;
        if (sets.wide >> granule & 1)
            wideOutOfLine.append(offset);
        if (sets.narrowInFirstHalf >> granule & 1)
            narrowOutOfLine.append(offset);
        if (sets.narrowInSecondHalf >> granule & 1)
            narrowOutOfLine.append(offset + static_cast<uint32_t>(sizeof(uint32_t)));
    }
    line.numbers("wideOutOfLine"_s, wideOutOfLine.span());
    line.numbers("narrowOutOfLine"_s, narrowOutOfLine.span());
}

static void printLayoutRecord(const TablesContext& context, const LayoutData& layout)
{
    RecordLine(context, "layout"_s).field("shape"_s, layout.shape).field("index"_s, LayoutID(layout.index)).field("kind"_s, layout.kind()).string("name"_s, layout.name).field("size"_s, layout.size)
        .field("impossible"_s, layout.impossibleMutations()).flag("keysAreNeverEnumerated"_s, layout.keysAreNeverEnumerated).flag("hasFieldsWithSymbolKeys"_s, layout.hasFieldsWithSymbolKeys)
        .flag("hasOnlyPlainSlots"_s, layout.hasOnlyPlainSlots)
        .flag("hasFieldThatKeepsStateOfBirth"_s, layout.hasFieldThatKeepsStateOfBirth);
    printFieldRecords(context, "  field"_s, layout, layout.fields());
    printReferences(context, referenceTableOf(layout));
    for (uint32_t order = 0; order < layout.numberOfOrders; ++order)
        RecordLine(context, "  order"_s, order).numbers("fields"_s, context.tables.keyOrder(layout, order).entries());
    if (layout.hasExtensionData || layout.hasExtraPropertiesSlot)
        RecordLine(context, "  rare"_s).field("extension"_s, layout.extensionSlot()).field("extraProperties"_s, layout.extraPropertiesSlot()).field("extensionSize"_s, layout.extensionSize());
}

static void printParsedRecordFields(const TablesContext& context)
{
    for (const ParsedRecordFieldData& row : context.tables.parsedRecordFields())
        RecordLine(context, "parsedRecordField"_s).identifier("name"_s, row.name).field("shape"_s, row.shape);
}

static void printEnvironmentRecord(const TablesContext& context, const EnvironmentData& environment)
{
    RecordLine(context, "environment"_s).field("shape"_s, environment.shape).field("index"_s, LayoutID(environment.index)).field("kind"_s, environment.kind).string("name"_s, environment.name).field("size"_s, environment.size);
    for (const VariableData& variable : environment.describedVariables())
        RecordLine(context, "  variable"_s).identifier("name"_s, variable.name).field("slot"_s, variable.slot);
    printReferences(context, referenceTableOf(environment));
}

static void printMethodRecords(const TablesContext& context, ASCIILiteral keyword, std::span<const MethodData> methods)
{
    for (const MethodData& method : methods)
        RecordLine(context, keyword).identifier("name"_s, method.name).field("kind"_s, method.kind()).flags("flags"_s, method.flags, methodFlagNames).field("function"_s, FunctionID(method.function));
}

static void printSetsOfGuard(const TablesContext& context, MemberGuardAggregates aggregates)
{
    if (!aggregates.count)
        return;
    std::span<const uint32_t> words = unsafeMakeSpan(context.tables.at<uint32_t>(aggregates.offset), 2 * static_cast<size_t>(aggregates.count));
    for (uint32_t index = 0; index < aggregates.count; ++index) {
        const uint32_t* list = context.tables.at<uint32_t>(words[aggregates.count + index]);
        Vector<uint32_t, 8> members;
        Vector<uint32_t, 8> awaited;
        Vector<uint32_t, 8> holdingWhileClear;
        for (uint32_t entry : unsafeMakeSpan(list, 1 + static_cast<size_t>(*list)).subspan(1)) {
            members.append(MemberGuardAggregates::numberInListEntry(entry));
            if (entry & MemberGuardAggregates::isAwaitedBeforeClassExists)
                awaited.append(members.last());
            if (entry & MemberGuardAggregates::holdsWhileClear)
                holdingWhileClear.append(members.last());
        }
        RecordLine(context, "    set"_s).field("bit"_s, MemberGuardAggregates::numberInListEntry(words[index])).flag("holdsWhileClear"_s, words[index] & MemberGuardAggregates::holdsWhileClear)
            .numbers("members"_s, members).numbers("awaitedBeforeClassExists"_s, awaited).numbers("holdingWhileClear"_s, holdingWhileClear);
    }
}

static void printClassRecord(const TablesContext& context, const LayoutData& layout)
{
    const ClassData& data = layout.classData();
    RecordLine(context, "class"_s).field("index"_s, LayoutID(layout.index)).field("shape"_s, layout.shape).field("base"_s, LayoutID(data.base)).field("expectedBase"_s, LayoutID(data.expectedBase)).field("constructor"_s, FunctionID(data.classConstructor))
        .field("environment"_s, LayoutID(data.environment)).field("evaluation"_s, layout.evaluation()).numbers("guardWords"_s, std::span(data.guardWordsOffset))
        .flag("isUntyped"_s, layout.isUntyped).flag("hasUntypedBase"_s, layout.hasUntypedBase).flag("createsMembersLazily"_s, layout.createsMembersLazily)
        .flag("createsPrototypeBeforeClassObject"_s, layout.createsPrototypeBeforeClassObject).flag("createsClassObjectOnMutation"_s, layout.createsClassObjectOnMutation)
        .flag("guardsHoldWhileClear"_s, data.guardsHoldWhileClear);
    printMethodRecords(context, "  method"_s, layout.methods());
    printMethodRecords(context, "  staticMethod"_s, layout.staticMethods());
    printFieldRecords(context, "  staticField"_s, layout, layout.staticFields());
    for (const MemberGuardData& guard : layout.memberGuards()) {
        RecordLine(context, "  guard"_s).identifier("name"_s, guard.name).field("kind"_s, nameOf(guard.kind())).field("side"_s, guard.side()).field("bit"_s, guard.bit);
        printSetsOfGuard(context, layout.aggregatesOf(guard));
    }
    if (data.subclassesOffset) {
        const uint32_t* subclasses = context.tables.at<uint32_t>(data.subclassesOffset);
        RecordLine(context, "  subclasses"_s).list("classes"_s, unsafeMakeSpan(subclasses, 1 + static_cast<size_t>(*subclasses)).subspan(1), [&](uint32_t index) {
            context.printer.print(LayoutID(index));
        });
    }
}

static void printLayouts(const TablesContext& context)
{
    const ProgramTables& tables = context.tables;
    RecordLine(context, "shapes"_s).field("count"_s, tables.numberOfShapes).field("firstEnvironment"_s, tables.firstEnvironmentShape).field("layoutIndices"_s, tables.numberOfLayoutIndices);
    auto offsets = tables.layouts();
    for (uint32_t shape = 0; shape < offsets.size(); ++shape) {
        if (!offsets[shape])
            continue;
        if (shape < tables.firstEnvironmentShape)
            printLayoutRecord(context, tables.layoutForShape(shape));
        else
            printEnvironmentRecord(context, tables.environmentForShape(shape));
    }
    for (uint32_t index = 0; index < tables.numberOfLayoutIndices; ++index) {
        if (uint32_t offset = tables.offsetOfClass(index))
            printClassRecord(context, *tables.at<LayoutData>(offset));
    }
    printParsedRecordFields(context);
    RecordLine(context, "shapeSetBits"_s).list("words"_s, tables.shapeSetBits(), [&](uint32_t word) {
        context.out.printf("0x%08x", word);
    });
}

static void printAllocations(const TablesContext& context)
{
    auto allocations = context.tables.allocations();
    for (uint32_t index = 0; index < allocations.size(); ++index) {
        const AllocationData& allocation = allocations[index];
        RecordLine(context, "allocation"_s, index).field<uint32_t>("shape"_s, allocation.shape).hex("order"_s, allocation.orderWord()).field("size"_s, allocation.size()).field("constructedBy"_s, FunctionID(context.tables.constructorOfAllocation(index)));
    }
}

static void printStatics(const TablesContext& context)
{
    const ProgramTables& tables = context.tables;
    InstanceSizes sizes = tables.instanceSizes;
    RecordLine(context, "instance"_s).field("statics"_s, sizes.sizeOfStatics).field("stringCells"_s, sizes.numberOfStringCells).field("siteCaches"_s, sizes.numberOfSiteCaches);
    for (const StaticsLayout& layout : tables.staticsLayout())
        RecordLine(context, "layout"_s).field("filledAtProgramEntry"_s, layout.sizeFilledAtProgramEntry).field("fromArrayProfileToLastArray"_s, layout.distanceFromArrayProfileToLastArray);
    for (const StaticsRun& run : tables.staticsRuns())
        RecordLine(context, "run"_s).field("firstWord"_s, run.firstWord).field("words"_s, run.numberOfWords).hex("bits"_s, run.bits());
    for (const StaticsRange& range : tables.staticsRoots())
        RecordLine(context, "roots"_s).field("offset"_s, range.offset).field("size"_s, range.size);
    for (const StaticsRange& range : tables.staticsNarrowRoots())
        RecordLine(context, "narrowRoots"_s).field("offset"_s, range.offset).field("size"_s, range.size);
    for (const StaticsRange& range : tables.staticsClearedEachCollection())
        RecordLine(context, "cleared"_s).field("offset"_s, range.offset).field("size"_s, range.size);
    for (const FunctionVariableData& variable : tables.functionVariables())
        RecordLine(context, "functionVariable"_s).field("offset"_s, variable.offset).field("function"_s, FunctionID(variable.function));
}

static void printFunctionRecord(const TablesContext& context, uint32_t index)
{
    const ProgramTables& tables = context.tables;
    FunctionKindData kind = tables.kindOfFunction(index);
    RecordLine line(context, "function"_s, IDPrefix::Function, index);
    line.string("name"_s, tables.nameOfFunction(index));
    line.field("callable"_s, nameOf(kind.callable())).field("accessor"_s, kind.accessor()).field("structure"_s, static_cast<uint32_t>(kind.closureStructure))
        .flag("slim"_s, kind.isSlim).flag("async"_s, kind.isAsync).flag("generator"_s, kind.isGenerator).flag("takesNewTarget"_s, kind.takesNewTarget)
        .flag("commonJSModule"_s, kind.isCommonJSModule).flag("inRuntimeModule"_s, kind.isInRuntimeModule).flag("constructor"_s, kind.isConstructor)
        .flag("derivedConstructor"_s, kind.isDerivedConstructor).flag("inferredName"_s, kind.hasInferredName);
    if (index < tables.closureFunctions.first || index >= tables.closureFunctions.end)
        return;
    CaptureCounts counts = tables.captureCountsOf(index);
    line.field("length"_s, tables.functionLengthOf(index)).field("references"_s, static_cast<uint32_t>(counts.references)).field("captures"_s, static_cast<uint32_t>(counts.captures));
}

static void printFunctions(const TablesContext& context)
{
    const ProgramTables& tables = context.tables;
    ClosureFunctionRangeData range = tables.closureFunctions;
    RecordLine(context, "closures"_s).field("first"_s, range.first).field("end"_s, range.end).field("omittedWords"_s, range.omittedWordCount);
    for (uint32_t index = 0; index < tables.numberOfFunctions(); ++index)
        printFunctionRecord(context, index);
    for (const ConstructorData& constructor : tables.constructors())
        RecordLine(context, "constructor"_s).field("function"_s, FunctionID(constructor.function)).field("layout"_s, LayoutID(constructor.layoutIndex));
    for (const BoundFunctionData& bound : tables.boundFunctions()) {
        RecordLine(context, "bound"_s).field("function"_s, FunctionID(bound.function)).field("target"_s, FunctionID(bound.target))
            .field("arguments"_s, static_cast<uint32_t>(bound.numberOfBoundArguments)).field("targetClosureSlot"_s, bound.targetClosureSlot())
            .field("constructs"_s, !!bound.constructs).field("firstArgumentSlot"_s, static_cast<uint32_t>(bound.firstArgumentSlot)).field("capturedArguments"_s, static_cast<uint32_t>(bound.numberOfCapturedArguments));
    }
    for (const ConstructEntrypointData& entrypoint : tables.constructEntrypoints())
        RecordLine(context, "constructEntrypoint"_s).field("function"_s, FunctionID(entrypoint.function)).field("entrypoint"_s, FunctionID(entrypoint.constructEntrypoint())).field("allocatesThis"_s, !!(entrypoint.constructEntrypointAndFlag & ConstructEntrypointData::allocatesThis));
}

static void printFunctionTraces(const TablesContext& context)
{
    for (uint32_t index = 0; index < context.tables.numberOfFunctions(); ++index) {
        const FunctionTraceData& trace = context.tables.traceOfFunction(index);
        std::optional<BuiltinID> implemented;
        if (std::optional<uint32_t> builtin = context.tables.builtinImplementedBy(index))
            implemented = BuiltinID(*builtin);
        RecordLine(context, "trace"_s, IDPrefix::Function, index).field("role"_s, context.program.at(FunctionID(index)).role).field("source"_s, FunctionID(context.tables.sourceFunctionOf(index)))
            .field("implements"_s, implemented).field("visibility"_s, nameOf(trace.visibility())).field("builtinKind"_s, nameOf(trace.builtinKind()))
            .word(trace.codeType() == ModuleCode ? "module"_s : "function"_s).word(trace.isStrict ? "strict"_s : "sloppy"_s).flag("constructor"_s, trace.isConstructor)
            .flag("async"_s, trace.isAsync).flag("rejectsOnThrow"_s, trace.rejectsOnThrow).flag("namedByOwner"_s, trace.isNamedByOwner)
            .flag("nakedConstructor"_s, trace.isNakedConstructor).flag("namedByComputedKey"_s, trace.isNamedByComputedKey);
    }
}

static void printFunctionTexts(const TablesContext& context)
{
    auto texts = context.tables.functionTexts();
    if (texts.empty())
        return;
    auto entries = unsafeMakeSpan(context.tables.at<FunctionTextEntry>(texts[0].entriesOffset), texts[0].numberOfEntries);
    auto text = byteCast<Latin1Character>(context.bytes.subspan(texts[0].textOffset));
    for (const FunctionTextEntry& entry : entries)
        context.out.println("f", entry.function, " ", entry.start, "..", entry.end, " \"", StringView(text.subspan(entry.start, entry.end - entry.start)), "\"");
}

static void printModuleRecords(const TablesContext& context)
{
    auto printListStarts = [](RecordLine& line, const ModuleData& module) {
        line.field<uint32_t>("requests"_s, module.firstRequest).field<uint32_t>("exports"_s, module.firstExport).field<uint32_t>("syntheticExports"_s, module.firstSyntheticExport).field<uint32_t>("requiredNames"_s, module.firstRequiredName)
            .field<uint32_t>("linkRequests"_s, module.firstLinkRequest).field<uint32_t>("evaluationRequests"_s, module.firstEvaluationRequest);
    };
    // A path is printed as the string of the program that its pieces make.
    auto pathOf = [&](const ModuleData& module, size_t bytesLeftOut) {
        std::span<const ModuleDirectoryData> directories = context.tables.moduleDirectories();
        Vector<uint32_t, 16> pieces { module.publicPathOrName };
        for (uint32_t directory = module.directory; directory != Number24::max; directory = directories[directory].parent)
            pieces.append(directories[directory].name);
        Vector<uint8_t, 256> path;
        for (uint32_t piece : pieces | std::views::reverse)
            path.append(context.program.strings[StringID(piece)]);
        return context.program.strings.find(path.span().subspan(bytesLeftOut));
    };
    auto displayPathOf = [&](uint32_t index, const ModuleData& module) {
        if (module.charactersInFrontOfDisplayPath == ModuleData::hasNoDisplayPath)
            return context.program.strings.emptyString();
        if (module.charactersInFrontOfDisplayPath != ModuleData::displayPathIsListed)
            return pathOf(module, module.charactersInFrontOfDisplayPath);
        return StringID(std::ranges::lower_bound(context.tables.moduleDisplayPaths(), index, { }, [](const ModuleDisplayPathData& entry) -> uint32_t { return entry.module; })->path);
    };
    auto modules = context.tables.modules();
    for (uint32_t index = 0; index + 1 < modules.size(); ++index) {
        const ModuleData& module = modules[index];
        RecordLine line(context, "module"_s, IDPrefix::Module, index);
        std::optional<RuntimeModuleID> runtimeModule = module.registryID() ? std::optional(RuntimeModuleID(*module.registryID())) : std::nullopt;
        line.field<ModuleKind>("kind"_s, module.kind).field("path"_s, pathOf(module, 0)).field("displayPath"_s, displayPathOf(index, module)).field<ExternalKind>("externalKind"_s, module.externalKind)
            .field<SyntheticExportRule>("exportRule"_s, module.exportRule).field<RequireResult>("requireResult"_s, module.requireResult).hex("flags"_s, module.flags).field("body"_s, module.bodyFunction().value_or(FunctionID()))
            .field("referrer"_s, module.referrerModule().value_or(ModuleID())).field("runtimeModule"_s, runtimeModule).field("value"_s, module.value);
        printListStarts(line, module);
    }
    RecordLine line(context, "endOfModules"_s);
    printListStarts(line, modules.back());
}

static void printModuleExportRecord(const TablesContext& context, uint32_t index, const ModuleExportData& moduleExport)
{
    RecordLine line(context, "export"_s, index);
    line.identifier("name"_s, moduleExport.name).field("kind"_s, moduleExport.kind());
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable:
        line.field("variable"_s, moduleExport.variable);
        break;
    case ModuleExportKind::Namespace:
        line.field("module"_s, moduleExport.targetModule());
        break;
    case ModuleExportKind::Indirect:
        line.field("indirectExport"_s, moduleExport.indirectExport);
        break;
    }
    line.flag("deferred"_s, moduleExport.isDeferred).flag("fromStarExport"_s, moduleExport.isFromStarExport);
}

static void printModules(const TablesContext& context)
{
    const ProgramTables& tables = context.tables;
    printModuleRecords(context);
    auto requests = tables.moduleRequests();
    for (uint32_t index = 0; index < requests.size(); ++index)
        RecordLine(context, "request"_s, index).field("target"_s, requests[index].targetModule()).flag("deferred"_s, requests[index].isDeferred).flag("dynamicStarExport"_s, requests[index].isDynamicStarExport).flag("hoisted"_s, requests[index].isHoisted);
    ModuleNumbers linkRequests(tables.linkRequests(), tables.modules().size());
    for (uint32_t index = 0; index < linkRequests.size(); ++index)
        RecordLine(context, "linkRequest"_s, index).field("target"_s, ModuleID(linkRequests[index]));
    ModuleNumbers evaluationRequests(tables.evaluationRequests(), tables.modules().size());
    for (uint32_t index = 0; index < evaluationRequests.size(); ++index)
        RecordLine(context, "evaluationRequest"_s, index).field("target"_s, ModuleID(evaluationRequests[index]));
    for (const LinkPlanStepData& step : tables.entryPointLinkPlan())
        RecordLine(context, "linkPlanStep"_s).field("module"_s, step.targetModule()).flag("resolvesNames"_s, step.resolvesNames);
    for (const LinkPlanRootData& root : tables.linkPlanRoots()) {
        if (&root == &tables.linkPlanRoots().back()) {
            RecordLine(context, "endOfLinkPlanRoots"_s).field("firstStep"_s, root.firstStep);
            break;
        }
        RecordLine(context, "linkPlanRoot"_s).field("module"_s, ModuleID(root.module)).field("firstStep"_s, root.firstStep).field("memberOfCycle"_s, ModuleID(root.memberOfCycle));
    }
    for (const LinkPlanStepData& step : tables.linkPlanStepsOfRoots())
        RecordLine(context, "linkPlanStepOfRoot"_s).field("module"_s, step.targetModule()).flag("resolvesNames"_s, step.resolvesNames);
    auto exports = tables.moduleExports();
    for (uint32_t index = 0; index < exports.size(); ++index)
        printModuleExportRecord(context, index, exports[index]);
    auto indirectExports = tables.indirectExports();
    for (uint32_t index = 0; index < indirectExports.size(); ++index)
        RecordLine(context, "indirectExport"_s, index).field("module"_s, indirectExports[index].targetModule()).identifier("importName"_s, indirectExports[index].importName);
    auto syntheticExports = tables.syntheticExports();
    for (uint32_t index = 0; index < syntheticExports.size(); ++index) {
        const SyntheticExportData& syntheticExport = syntheticExports[index];
        std::optional<SlotData> variable = syntheticExport.hasVariable ? std::optional(syntheticExport.variable) : std::nullopt;
        RecordLine(context, "syntheticExport"_s, index).identifier("name"_s, syntheticExport.name).field("variable"_s, variable).flag("absentIsUndefined"_s, syntheticExport.absentIsUndefined);
    }
    auto requiredNames = tables.requiredNames();
    for (uint32_t index = 0; index < requiredNames.size(); ++index)
        RecordLine(context, "requiredName"_s, index).field("exporter"_s, requiredNames[index].exporterModule()).identifier("name"_s, requiredNames[index].name).flag("mayBeAbsent"_s, requiredNames[index].mayBeAbsent);
    for (const ModuleFailureData& failure : tables.moduleFailures())
        RecordLine(context, "failure"_s).field("importer"_s, failure.importerModule()).field("specifier"_s, failure.specifierString());
    RecordLine(context, "modulesByPath"_s).list("modules"_s, ModuleNumbers(tables.modulesByPath(), tables.modules().size()), [&](uint32_t module) {
        context.printer.print(ModuleID(module));
    });
    for (const CompiledRuntimeModuleData& runtimeModule : tables.compiledRuntimeModules())
        RecordLine(context, "runtimeModule"_s).field("id"_s, RuntimeModuleID(runtimeModule.registryID)).field("module"_s, ModuleID(runtimeModule.module));
    for (const CompiledRuntimeFunctionData& runtimeFunction : tables.compiledRuntimeFunctions()) {
        RecordLine(context, "runtimeFunction"_s).field("id"_s, RuntimeFunctionID(runtimeFunction.id)).field("function"_s, FunctionID(runtimeFunction.function))
            .field("visibility"_s, static_cast<FunctionVisibility>(runtimeFunction.visibility));
    }
}

static void printImmediate(const TablesContext& context, uint64_t word)
{
    JSValue value = JSValue::decode(static_cast<EncodedJSValue>(word));
    if (!value)
        context.out.print("hole");
    else if (value.isUndefined())
        context.out.print("undefined");
    else if (value.isNull())
        context.out.print("null");
    else if (value.isBoolean())
        context.printer.print(value.asBoolean());
    else if (value.isInt32())
        context.out.print(value.asInt32());
    else
        context.printer.print(value.asDouble());
}

static void printDataWord(const TablesContext& context, uint64_t word)
{
    uint32_t operand = DataWord::operandOf(word);
    switch (DataWord::kindOf(word)) {
    case DataWord::Kind::Immediate:
        printImmediate(context, word);
        return;
    case DataWord::Kind::StringConstant:
        printStringConstant(context, operand);
        return;
    case DataWord::Kind::BigIntConstant:
        context.printer.print(StringID(context.tables.bigIntConstants()[operand]));
        context.out.print("n");
        return;
    case DataWord::Kind::Node:
        context.out.print("@", operand);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printDataNode(const TablesContext& context, uint32_t index, DataNodeHeader header, std::span<const uint64_t> body)
{
    std::span<const uint64_t> values = body.subspan(header.kind() == DataNodeHeader::Kind::Array ? DataNodeHeader::wordsBeforeElements - 1 : 0, header.count);
    auto printWord = [&](uint64_t word) {
        printDataWord(context, word);
    };
    auto printDoubleOrHole = [&](uint64_t bits) {
        double number = std::bit_cast<double>(bits);
        if (number != number)
            context.out.print("hole");
        else
            context.printer.print(number);
    };
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array: {
        auto indexingType = static_cast<IndexingType>(header.operand & DataNodeHeader::indexingTypeMask);
        RecordLine line(context, "array"_s, index);
        context.out.print(" ", IndexingTypeDump(indexingType));
        line.flag("isStorage"_s, header.operand & DataNodeHeader::isStorage);
        if (indexingType == ArrayWithDouble)
            line.list("elements"_s, values, printDoubleOrHole);
        else
            line.list("elements"_s, values, printWord);
        return;
    }
    case DataNodeHeader::Kind::Object:
    case DataNodeHeader::Kind::ObjectWithOtherMembers: {
        RecordLine line(context, "object"_s, index);
        line.field("allocation"_s, static_cast<uint32_t>(header.operand))
            .numbers("fields"_s, spanReinterpretCast<const uint16_t>(body.subspan(header.count)).first(header.count)).list("values"_s, values, printWord);
        if (header.kind() == DataNodeHeader::Kind::ObjectWithOtherMembers)
            line.fieldWith("otherMembers"_s, [&] { printWord(body.back()); });
        return;
    }
    case DataNodeHeader::Kind::ObjectBody:
        RecordLine(context, "objectBody"_s, index).field("allocation"_s, static_cast<uint32_t>(header.operand)).list("words"_s, values, [&](uint64_t word) { context.out.printf("0x%llx", static_cast<unsigned long long>(word)); });
        return;
    case DataNodeHeader::Kind::OtherMembers:
    case DataNodeHeader::Kind::OrdinaryObject:
        RecordLine(context, header.kind() == DataNodeHeader::Kind::OtherMembers ? "otherMembers"_s : "ordinaryObject"_s, index)
            .list("keys"_s, spanReinterpretCast<const uint32_t>(body.subspan(header.count)).first(header.count), [&](uint32_t number) { printIdentifier(context, number); }).list("values"_s, values, printWord);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printData(const TablesContext& context)
{
    std::span<const uint64_t> words = context.tables.data();
    for (uint32_t index = DataNodeHeader::firstNode; index < words.size();) {
        DataNodeHeader header = DataNodeHeader::decode(words[index]);
        uint32_t numberOfWords = DataNodeHeader::numberOfWords(header.kind(), header.count);
        printDataNode(context, index, header, words.subspan(index + 1, numberOfWords - 1));
        index += numberOfWords;
    }
}

template<typename T>
static const T& requestAt(const TablesContext& context, uint32_t offset)
{
    return *context.tables.at<T>(context.tables.offsetOf(context.tables.requests().data()) + offset);
}

static void printTemplateObjectRequest(const TablesContext& context, uint32_t offset)
{
    const auto& request = requestAt<TemplateObjectRequest>(context, offset);
    RecordLine(context, "request"_s, offset).word("TemplateObject"_s).list("parts"_s, unsafeMakeSpan(request.parts, request.count), [&](const TemplateObjectRequest::Part& part) {
        if (part.cookedPlusOne)
            printStringConstant(context, part.cookedPlusOne - 1);
        else
            context.out.print("-");
        context.out.print("/");
        printStringConstant(context, part.raw);
    });
}

static void printDirectEvalRequest(const TablesContext& context, uint32_t offset)
{
    const auto& request = requestAt<DirectEvalRequest>(context, offset);
    RecordLine(context, "request"_s, offset).word("DirectEval"_s).field("module"_s, ModuleID(request.module)).flag("strict"_s, request.isStrict).flag("inFunction"_s, request.isInFunction)
        .flag("inArrowFunction"_s, request.isInArrowFunction).flag("inFieldInitializer"_s, request.isInFieldInitializer).flag("inDerivedConstructor"_s, request.isInDerivedConstructor)
        .flag("allowsSuperProperty"_s, request.allowsSuperProperty).flag("allowsSuperCall"_s, request.allowsSuperCall).flag("allowsNewTarget"_s, request.allowsNewTarget).numbers("bindingsByName"_s, request.bindingsByName());
    for (const EvalBindingData& binding : unsafeMakeSpan(request.bindings, request.numberOfBindings)) {
        RecordLine(context, "  binding"_s).identifier("name"_s, binding.name).field("kind"_s, static_cast<EvalBindingKind>(binding.kind)).field("depth"_s, static_cast<uint32_t>(binding.depth))
            .field("operand"_s, optionalIDInImage<uint32_t>(binding.operand)).field("function"_s, optionalIDInImage<uint32_t>(binding.function))
            .field("setter"_s, optionalIDInImage<uint32_t>(binding.setter)).identifier("privateKey"_s, binding.privateKey).field("slot"_s, binding.slot).field("type"_s, binding.type);
    }
}

static void printCollectionDataRequest(const TablesContext& context, uint32_t offset)
{
    const auto& request = requestAt<CollectionDataRequest>(context, offset);
    RecordLine line(context, "request"_s, offset);
    line.word("CollectionData"_s);
    if (!request.tableInImage) {
        line.field("elements"_s, request.elements);
        return;
    }
    const TableInImage& table = *context.tables.at<TableInImage>(request.tableInImage);
    line.field("table"_s, request.tableInImage).field("capacity"_s, table.capacity).strings("keys"_s, unsafeMakeSpan(table.strings(), table.count));
    if (!table.hasValues)
        return;
    line.list("values"_s, unsafeMakeSpan(table.values(), table.count), [&](uint64_t word) {
        printDataWord(context, word);
    });
}

static void printRequest(const TablesContext& context, const RequestRecord& record, uint32_t offset)
{
    WTF::switchOn(record,
        [&](const NewClassRecord&) {
            RecordLine(context, "request"_s, offset).word("NewClass"_s).field("class"_s, LayoutID(requestAt<NewClassRequest>(context, offset).classIndex));
        },
        [&](const TemplateObjectRecord&) {
            printTemplateObjectRequest(context, offset);
        },
        [&](const DirectEvalRecord&) {
            printDirectEvalRequest(context, offset);
        },
        [&](const DataRecord&) {
            const auto& request = requestAt<DataRequest>(context, offset);
            RecordLine(context, "request"_s, offset).word("Data"_s).field("root"_s, request.root).field("sharedStorage"_s, optionalIDInImage<uint32_t>(request.sharedStorageOffset));
        },
        [&](const CollectionDataRecord&) {
            printCollectionDataRequest(context, offset);
        });
}

static void printRequests(const TablesContext& context)
{
    const RequestTable& requests = context.placement.requests();
    for (unsigned index = 0; index < requests.records().size(); ++index)
        printRequest(context, requests.records()[index], requests.offsetOf(index));
}

static void printPropertyRuns(const TablesContext& context)
{
    std::span<const uint32_t> words = context.tables.propertyRuns();
    uint32_t numberOfRuns = words.empty() ? 0 : words[0] - 1;
    for (uint32_t number = 0; number < numberOfRuns; ++number) {
        std::span<const uint32_t> row = context.tables.propertyRunRow(number);
        PropertyRunHeader header = PropertyRunHeader::decode(row[0]);
        RecordLine line(context, "run"_s, number);
        line.list("keys"_s, row.subspan(1, header.count), [&](uint32_t key) {
            printIdentifier(context, key);
        });
    }
}

static ASCIILiteral nameOf(FieldTableEntry::Form form)
{
    switch (form) {
    case FieldTableEntry::Form::InlineValue:
        return "InlineValue"_s;
    case FieldTableEntry::Form::InlineInt32:
        return "InlineInt32"_s;
    case FieldTableEntry::Form::InlineNarrowCell:
        return "InlineNarrowCell"_s;
    case FieldTableEntry::Form::OutOfLineValue:
        return "OutOfLineValue"_s;
    case FieldTableEntry::Form::Field:
        return "Field"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<uint32_t> offsetIn(FieldTableEntry entry)
{
    switch (entry.form()) {
    case FieldTableEntry::Form::InlineValue:
    case FieldTableEntry::Form::OutOfLineValue:
        return entry.operand() * sizeof(EncodedJSValue);
    case FieldTableEntry::Form::InlineInt32:
    case FieldTableEntry::Form::InlineNarrowCell:
        return entry.operand() * sizeof(uint32_t);
    case FieldTableEntry::Form::Field:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What a lookup finds for every object shape under every name that some layout has, shape by shape.
static void printFieldTable(const TablesContext& context)
{
    const ProgramTables& tables = context.tables;
    auto entries = tables.fieldTableEntries();
    BitVector namesWithEntries(FieldTableEntry::maxName + 1);
    uint32_t numberOfOccupiedEntries = 0;
    for (uint32_t bits : entries) {
        if (!bits)
            continue;
        namesWithEntries.quickSet(FieldTableEntry { bits }.name());
        ++numberOfOccupiedEntries;
    }
    RecordLine(context, "table"_s).field("rows"_s, tables.numberOfFieldTableRows).field("occupied"_s, numberOfOccupiedEntries).field("names"_s, static_cast<uint32_t>(namesWithEntries.bitCount()));
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        for (size_t name : namesWithEntries) {
            uint32_t index = tables.fieldTableRowOf(shape) + static_cast<uint32_t>(name);
            FieldTableEntry entry { entries[index] };
            RecordLine line(context, "lookup"_s);
            line.field("shape"_s, shape).identifier("name"_s, static_cast<uint32_t>(name));
            if (entry.name() != name) {
                line.word("none"_s);
                continue;
            }
            line.field("form"_s, nameOf(entry.form()));
            if (std::optional<uint32_t> offset = offsetIn(entry))
                line.field("offset"_s, *offset);
            FieldRecordIndex field { tables.fieldRecordIndices()[index] };
            line.field("field"_s, static_cast<uint32_t>(field.indexInLayoutRecord())).flag("storedWithoutTypeTest"_s, field.bits & FieldRecordIndex::isStoredWithoutTypeTest);
        }
    }

    // What the prototype object of every class has under the same names, class by class.
    const uint32_t* offsetsOfClasses = tables.classes().data() + tables.numberOfClassGroupWords();
    auto rowsOfClasses = tables.classMemberRows();
    for (uint32_t rank = 0; rank < rowsOfClasses.size(); ++rank) {
        LayoutID classIndex(tables.at<LayoutData>(offsetsOfClasses[rank])->index);
        if (rowsOfClasses[rank] == ClassRowEntry::noRow) {
            RecordLine(context, "members"_s).field("class"_s, classIndex).word("unknown"_s);
            continue;
        }
        for (size_t name : namesWithEntries) {
            uint32_t index = rowsOfClasses[rank] + static_cast<uint32_t>(name);
            FieldTableEntry entry { entries[index] };
            if (entry.name() != name)
                continue;
            RecordLine line(context, "member"_s);
            line.field("class"_s, classIndex).identifier("name"_s, static_cast<uint32_t>(name));
            uint32_t number = ClassRowEntry::numberIn(entry, tables.fieldRecordIndices()[index]);
            if (ClassRowEntry::formOf(entry) == ClassRowEntry::Form::Method) {
                line.field("method"_s, number);
                continue;
            }
            auto accessor = std::bit_cast<ClassAccessorData>(tables.classAccessors()[number]);
            line.field("getter"_s, accessor.getter).field("setter"_s, accessor.setter);
        }
    }
}

static void printConstants(const TablesContext& context)
{
    const ProgramTables& tables = context.tables;
    for (uint32_t number = 0; number < tables.builtinCells().size(); ++number) {
        RecordLine(context, "builtinCell"_s, number).fieldWith("cell"_s, [&] {
            printBuiltinCell(context, number);
        });
    }
    for (uint32_t number : tables.eagerBuiltinCells()) {
        RecordLine(context, "eagerBuiltinCell"_s, number).fieldWith("cell"_s, [&] {
            printBuiltinCell(context, number);
        });
    }
    for (uint32_t member : tables.constantBuiltinMembers())
        RecordLine(context, "constantBuiltinMember"_s).field("member"_s, BuiltinID(member));
    auto bigIntConstants = tables.bigIntConstants();
    for (uint32_t number = 0; number < bigIntConstants.size(); ++number)
        RecordLine(context, "bigInt"_s, number).string("digits"_s, bigIntConstants[number]);
    auto literalStrings = tables.literalStrings();
    for (uint32_t number = 0; number < literalStrings.size(); ++number)
        RecordLine(context, "literalString"_s, number).string("text"_s, literalStrings[number]);
    auto literalStringTable = tables.literalStringTable();
    for (uint32_t slot = 0; slot < literalStringTable.size(); ++slot) {
        if (literalStringTable[slot])
            RecordLine(context, "literalStringSlot"_s, slot).field("number"_s, AOT::LiteralStringTable::numberOf(literalStringTable[slot]));
    }
    for (const LiteralStringLimits& limits : tables.literalStringLimits())
        RecordLine(context, "literalStringLimits"_s).field("maxLength"_s, limits.maxLength);
    for (const LiteralStringDigest& digest : tables.literalStringDigests())
        RecordLine(context, "literalStringDigest"_s, digest.number).field("length"_s, digest.length);
    if (IdentifierTable identifiersByHash(tables.identifiersByHash()); !identifiersByHash.isEmpty()) {
        std::span<const uint32_t> slots = identifiersByHash.slots();
        for (uint32_t slot = 0; slot < slots.size(); ++slot) {
            if (slots[slot])
                RecordLine(context, "identifierSlot"_s, slot).identifier("name"_s, identifiersByHash.numberIn(slots[slot]));
        }
    }
    for (uint32_t module : tables.entryPoints())
        RecordLine(context, "entryPoint"_s).field("module"_s, ModuleID(module));
}

static void printNamedStrings(const TablesContext& context)
{
    for (size_t index : context.namedStrings) {
        context.printer.print(StringID(index));
        context.out.print("\n");
    }
}

template<typename T>
static std::span<T> mutableTable(std::span<uint8_t> bytes, ProgramTableIndex index)
{
    TableRange range = reinterpretCastSpanStartTo<ProgramTables>(bytes).ranges[static_cast<unsigned>(index)];
    return spanReinterpretCast<T>(bytes.subspan(range.offset, range.count * sizeof(T)));
}

static bool enlargeTypeTable(std::span<uint8_t> bytes)
{
    reinterpretCastSpanStartTo<ProgramTables>(bytes).ranges[static_cast<unsigned>(ProgramTableIndex::types)].count = 1u << 30;
    return true;
}

static bool swapFirstConstructors(std::span<uint8_t> bytes)
{
    std::span<ConstructorData> constructors = mutableTable<ConstructorData>(bytes, ProgramTableIndex::constructors);
    if (constructors.size() < 2)
        return false;
    std::swap(constructors[0], constructors[1]);
    return true;
}

static bool removeFirstLayout(std::span<uint8_t> bytes)
{
    std::span<uint32_t> layouts = mutableTable<uint32_t>(bytes, ProgramTableIndex::layouts);
    if (layouts.size() < 2 || !layouts[1])
        return false;
    layouts[1] = 0;
    return true;
}

static bool setUnknownFieldFlag(std::span<uint8_t> bytes)
{
    const ProgramTables& tables = reinterpretCastSpanStartTo<ProgramTables>(bytes);
    for (uint32_t shape = 0; shape < tables.layouts().size(); ++shape) {
        if (!tables.hasLayoutForShape(shape) || !tables.layoutForShape(shape).numberOfFields)
            continue;
        reinterpretCastSpanStartTo<FieldData>(bytes.subspan(tables.layouts()[shape] + sizeof(LayoutData))).flags |= unknownFieldFlag;
        return true;
    }
    return false;
}

static std::span<uint32_t> mutableFieldTableEntries(std::span<uint8_t> bytes)
{
    return mutableTable<uint32_t>(bytes, ProgramTableIndex::fieldTable).subspan(reinterpretCastSpanStartTo<ProgramTables>(bytes).numberOfFieldTableRows);
}

// The first entry of a row of a shape, or of a row of a class.
static uint32_t* findFieldTableEntry(std::span<uint8_t> bytes, bool inRowOfClass)
{
    std::span<uint32_t> entries = mutableFieldTableEntries(bytes);
    std::span<uint32_t> rowsOfClasses = mutableTable<uint32_t>(bytes, ProgramTableIndex::classMemberRows);
    for (uint32_t index = 0; index < entries.size(); ++index) {
        if (entries[index] && std::ranges::contains(rowsOfClasses, index - FieldTableEntry { entries[index] }.name()) == inRowOfClass)
            return &entries[index];
    }
    return nullptr;
}

static bool clearFirstFieldTableEntry(std::span<uint8_t> bytes)
{
    uint32_t* occupied = findFieldTableEntry(bytes, false);
    if (!occupied)
        return false;
    *occupied = 0;
    return true;
}

static bool clearFirstEntryOfClass(std::span<uint8_t> bytes)
{
    uint32_t* occupied = findFieldTableEntry(bytes, true);
    if (!occupied)
        return false;
    *occupied = 0;
    return true;
}

static bool copyFieldTableEntryToFreePlace(std::span<uint8_t> bytes)
{
    std::span<uint32_t> entries = mutableFieldTableEntries(bytes);
    auto occupied = std::ranges::find_if(entries, std::identity { });
    auto free = std::ranges::find(entries, 0u);
    if (occupied == entries.end() || free == entries.end())
        return false;
    *free = *occupied;
    return true;
}

static const TableDamage* findTableDamage(StringView argument)
{
    static constexpr TableDamage damages[] {
        { ProgramTableRule::TableOutOfBounds, enlargeTypeTable },
        { ProgramTableRule::TableNotSorted, swapFirstConstructors },
        { ProgramTableRule::ShapeWithoutLayout, removeFirstLayout },
        { ProgramTableRule::UnknownFieldFlag, setUnknownFieldFlag },
        { ProgramTableRule::FieldMissingFromFieldTable, clearFirstFieldTableEntry },
        { ProgramTableRule::MemberMissingFromClassMemberTable, clearFirstEntryOfClass },
        { ProgramTableRule::FieldTableEntryWithoutField, copyFieldTableEntryToFreePlace },
    };
    static constexpr ASCIILiteral prefix = "damage="_s;
    if (!argument.startsWith(prefix))
        return nullptr;
    StringView ruleName = argument.substring(prefix.length());
    auto found = std::ranges::find_if(damages, [&](const TableDamage& damage) {
        return ruleName == nameOf(damage.rule);
    });
    return found == std::end(damages) ? nullptr : &*found;
}

static void checkDamagedTables(const TablesContext& context, const TableDamage& damage)
{
    static constexpr void (*tableGroups[])(TableEncoder&) {
        encodeTypeTables, encodeDataTable, encodeLayoutTables, encodePropertyRunTable, encodeFieldTable, encodeStaticsTables,
        encodeFunctionTables, encodeModuleTables, encodeRequestTable, encodeConstantTables,
    };

    Vector<uint8_t> damagedBytes(context.bytes);
    if (!damage.apply(damagedBytes.mutableSpan())) {
        context.out.println("error: nothing to damage");
        return;
    }
    TableEncoder encoder(context.inputs, context.build);
    for (auto* encodeGroup : tableGroups)
        encodeGroup(encoder);
    checkProgramTables(encoder, damagedBytes.span());
}

static std::span<const TablesView> tablesViews()
{
    static constexpr TablesView views[] {
        { "types"_s, printTypes },
        { "layouts"_s, printLayouts },
        { "allocations"_s, printAllocations },
        { "statics"_s, printStatics },
        { "functions"_s, printFunctions },
        { "function-traces"_s, printFunctionTraces },
        { "function-texts"_s, printFunctionTexts },
        { "modules"_s, printModules },
        { "data"_s, printData },
        { "requests"_s, printRequests },
        { "property-runs"_s, printPropertyRuns },
        { "field-table"_s, printFieldTable },
        { "constants"_s, printConstants },
        { "named-strings"_s, printNamedStrings },
    };
    return views;
}

static void printTables(TestCommandContext& context, const PlacedProgram& placed)
{
    CodeGenerationPlan plan = prepareCodeGeneration(*placed.output, context.build);
    if (context.build.diagnostics().hasErrors())
        return;
    ProgramTableInputs inputs { *placed.output, plan.instanceSizes, plan.stringConstants, plan.bodyFactsForTables };
    EncodedProgramTables encoded = encodeProgramTables(inputs, context.build);
    if (encoded.bytes.isEmpty())
        return;

    IRPrinter printer(context.out, placed.program());
    const ProgramTables& tables = reinterpretCastSpanStartTo<ProgramTables>(encoded.bytes.span());
    TablesContext tablesContext { context.out, printer, placed.program(), placed.placement(), tables, encoded.bytes.span(), encoded.namedStrings, inputs, context.build };
    if (context.arguments.empty())
        printTableSizes(tablesContext);
    for (const String& argument : context.arguments) {
        if (const TablesView* view = findView(tablesViews(), argument))
            view->print(tablesContext);
        else
            checkDamagedTables(tablesContext, *findTableDamage(argument));
    }
}

static void printFailedRules(TestCommandContext& context, const Program& program)
{
    Diagnostics reported = context.build.takeDiagnostics();
    reported.finish(&program);
    for (const InternalError& error : reported.internalErrors()) {
        if (error.kind == InternalErrorKind::ProgramTableCheckFailed)
            context.out.println("rule ", nameOf(static_cast<ProgramTableRule>(error.count)));
    }
    context.build.diagnostics().append(Vector<UserDiagnostic>(reported.userDiagnostics()));
    context.build.diagnostics().append(Vector<InternalError>(reported.internalErrors()));
}

void runTables(TestCommandContext& context)
{
    for (const String& argument : context.arguments) {
        if (!findView(tablesViews(), argument) && !findTableDamage(argument)) {
            printBadArgument(context.out, argument);
            return;
        }
    }

    std::unique_ptr<PlacedProgram> placed = runPipelineForPlacementTest(context, PassID::CheckPlacement);
    if (!placed)
        return;
    printTables(context, *placed);
    printFailedRules(context, placed->program());
    context.program = WTF::move(placed->output->program);
    context.form = context.program->form();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

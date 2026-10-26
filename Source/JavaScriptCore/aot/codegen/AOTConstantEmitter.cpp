/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConstantEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTInstance.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTOperationData.h"
#include "AOTPlacement.h"
#include "AOTPureOperations.h"
#include "AOTStaticsEmitter.h"
#include "AOTStringTable.h"
#include "AOTStringUses.h"
#include "AOTStubsArrays.h"
#include "AOTStubsConstants.h"
#include "AOTVector.h"
#include "JSCellButterfly.h"
#include "SmallStrings.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<uint16_t> markerPrimitiveOf(JSValue value)
{
    if (!value)
        return Primitive::Absent;
    if (value.isUndefined())
        return Primitive::Undefined;
    if (value.isNull())
        return Primitive::Null;
    return std::nullopt;
}

static int64_t trackedIntegerOf(JSValue value)
{
    if (value.isInt32())
        return value.asInt32();
    RELEASE_ASSERT(value.isDouble() && isTrackedInteger(value.asDouble()));
    return static_cast<int64_t>(value.asDouble());
}

static std::optional<uint8_t> singleLatin1CharacterOf(std::span<const uint8_t> text)
{
    if (text.size() > 2)
        return std::nullopt;
    Vector<char16_t> units = decodeProgramString(text);
    if (units.size() != 1 || units[0] > maxSingleCharacterString)
        return std::nullopt;
    return static_cast<uint8_t>(units[0]);
}

static bool hasKeyCell(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ptrdiff_t offsetOfKeyCells(bool isNarrow)
{
    return isNarrow ? AOT_INSTANCE_OFFSET(runtime.constants.keyCells) : AOT_INSTANCE_OFFSET(runtime.constants.wideKeyCells);
}

// Of the tables of cells that are read here through a pointer in the instance, the narrow key cells alone hold narrow references.
static constexpr ptrdiff_t sizeOfCellWordIn(ptrdiff_t offsetOfTable)
{
    return offsetOfTable == offsetOfKeyCells(true) ? sizeof(uint32_t) : sizeof(JSCell*);
}

void ConstantEmitter::generateInteger(const ValueContext& value)
{
    int64_t constant = value->integer();
    if (value.representation() == Representation::Int64) {
        define(value, integer(constant));
        return;
    }
    define(value, constantOf(jsNumber(static_cast<double>(constant)), value.representation()));
}

void ConstantEmitter::generateString(const ValueContext& value)
{
    StringID string = value->string();
    if (value.representation() != Representation::LiteralString) {
        defineCell(value, stringConstant(value, string));
        return;
    }
    // Only a literal that something compares or stores as a word has a number. The word of any other is its string cell.
    if (inputs().literalStrings.indexOf(string))
        define(value, emitter<ConversionEmitter>().literalStringConstant(string));
    else
        define(value, LiteralStringWord(stringConstant(value, string).value()));
}

void ConstantEmitter::generateBuiltin(const ValueContext& value)
{
    defineCell(value, builtinCell(value, m_builtins.canonicalID(value->builtin())));
}

void ConstantEmitter::generateModuleBuiltin(const ValueContext& value)
{
    BuiltinID builtin = value->builtin();
    ModuleID module = value->module();
    auto callModuleBuiltinCell = [&](bool isOnRarePath) {
        return callOperation<CellPointer>(value, RuntimeOperation::ModuleBuiltinCell, { word32(static_cast<int32_t>(builtin.index())), word32(static_cast<int32_t>(module.index())) }, { .isOnRarePath = isOnRarePath });
    };
    std::optional<uint32_t> offset = m_placement.moduleBuiltinCellOffset(builtin, module);
    if (!offset) {
        defineCell(value, callModuleBuiltinCell(false));
        return;
    }

    TypedPointer cellWord = emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *offset);
    defineCell(value, permanentCell(cellWord, false, [&] {
        CellPointer cell = callModuleBuiltinCell(true);
        m_out.store64(cell.value(), cellWord);
        emitter<BarrierEmitter>().markCardOfStatics(*offset);
        return cell;
    }));
}

void ConstantEmitter::generateData(const ValueContext& value)
{
    switch (value->encoding()) {
    case DataEncoding::Unselected:
        failAndDefineZero(value, InternalErrorKind::PlacedPayloadMissing);
        return;
    case DataEncoding::Expanded:
    case DataEncoding::ReadOnlyTree:
    case DataEncoding::CopyOnWriteArray:
        break;
    }
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    if (value->encoding() == DataEncoding::CopyOnWriteArray) {
        defineCell(value, newCopyOnWriteArray(value, *request));
        return;
    }
    defineCell(value, callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { word32(static_cast<int32_t>(request->value())) }));
}

// Every array of the site is a cell around one storage. Storage without cells lies in the image. Any other is created by the first array of the site.
CellPointer ConstantEmitter::newCopyOnWriteArray(const ValueContext& value, Request request)
{
    auto createOutOfLine = [&](bool isOnRarePath) {
        return callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { word32(static_cast<int32_t>(request.value())) }, { .isOnRarePath = isOnRarePath });
    };
    const RequestRecord* record = m_placement.requests().recordAt(request);
    const DataRecord* data = record ? std::get_if<DataRecord>(record) : nullptr;
    const auto* elements = m_program.at(value->data()).root.tryAs<DataNode::Array>();
    if (!data || !(data->sharedStorageOffset || data->node) || !elements)
        return createOutOfLine(false);
    IndexingType indexingType = indexingTypeOf(m_program, *elements);
    // In place the array is 80 bytes of code. A short array stands where a literal handed its elements to a stub, which is half of that.
    bool isShort = elements->elements.size() < build().limit(LimitID::MaxExpandedDataNodes);
    if (value.shape() == CodeShape::Compact || (isShort && value.shape() != CodeShape::Fast)) {
        CallOptions options { .immediate = request.value() };
        if (!data->node)
            return CellPointer(callStub(value, arrayOfData(ArrayOfData::SharedValues), { m_out.load64(emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *data->sharedStorageOffset)) }, options).first);
        ArrayOfData form = indexingType == ArrayWithInt32 ? ArrayOfData::Int32 : indexingType == ArrayWithDouble ? ArrayOfData::Double : ArrayOfData::Values;
        return CellPointer(callStub(value, arrayOfData(form), { }, options).first);
    }

    build().statistics().count(StatisticID::CopyOnWriteArraysCreatedInline);
    LBasicBlock outOfLine = newBlock();
    Join<CellPointer> arrays(m_out);
    Pointer firstElement = [&] {
        if (data->node)
            return offsetBy(loadImmutablePointer(AOT_INSTANCE_OFFSET(image.data)), (static_cast<ptrdiff_t>(*data->node) + DataNodeHeader::wordsBeforeElements) * sizeof(uint64_t));
        Pointer storage(m_out.load64(emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *data->sharedStorageOffset)));
        branchUnless(Condition(m_out.notNull(storage.value())), outOfLine);
        return offsetBy(storage, JSCellButterfly::offsetOfData());
    }();
    Integer header = loadMutable64(Instance::offsetOfNewCopyOnWriteArrayHeader(indexingType));
    branchUnless(isNotZero(header), outOfLine);
    arrays.add(ArrayAllocationEmitter(generator()).allocateArrayCell(header, firstElement, outOfLine));

    m_out.appendTo(outOfLine);
    arrays.add(createOutOfLine(true));
    return arrays.finish();
}

void ConstantEmitter::generateKeyConstant(const ValueContext& value)
{
    PropertyKeyID key = value->key();
    if (value.representation() != Representation::LiteralString) {
        defineCell(value, keyConstant(value, key));
        return;
    }
    const PropertyKey& record = m_program.at(key);
    RELEASE_ASSERT(record.kind == PropertyKeyKind::Text);
    if (inputs().literalStrings.indexOf(record.text))
        define(value, emitter<ConversionEmitter>().literalStringConstant(record.text));
    else
        define(value, LiteralStringWord(keyConstant(value, key).value()));
}

TypedPointer ConstantEmitter::cellWordInTable(const B3::AbstractHeap& heap, ptrdiff_t offsetOfTable, uint32_t number)
{
    Pointer table = loadImmutablePointer(offsetOfTable);
    return m_out.address(heap, table.value(), static_cast<ptrdiff_t>(number) * sizeOfCellWordIn(offsetOfTable));
}

TypedPointer ConstantEmitter::builtinCellWord(uint32_t number)
{
    const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::AllocatorState);
    if (InstanceLayout::holdsBuiltinCells(m_placement.builtinCells().size()))
        return instanceField(heap, InstanceLayout::offsetOfBuiltinCells + number * sizeof(JSCell*));
    return cellWordInTable(heap, AOT_INSTANCE_PREFIX_OFFSET(builtinCells), number);
}

// Compact code calls the stub each time. Fast code calls it only while the word of the cell is empty.
CellPointer ConstantEmitter::cellMadeByStub(const OperationContext& context, StubKey stub, CallOptions options, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord)
{
    auto callStubThatMakesCell = [&] {
        return CellPointer(callStub(context, stub, { }, options).first);
    };
    if (context.shape() != CodeShape::Fast)
        return callStubThatMakesCell();
    options.isOnRarePath = true;
    return permanentCell(cellWord(), isNarrow, callStubThatMakesCell);
}

CellPointer ConstantEmitter::stringConstant(const OperationContext& context, StringID id)
{
    std::span<const uint8_t> text = m_program.string(id);
    if (text.empty())
        return emptyString();
    if (std::optional<uint8_t> character = singleLatin1CharacterOf(text))
        return singleCharacterString(word32(*character));

    // A string without a number has no root. As a pure function, B3 could reuse one result after a suspension, whose frame map has only what the IR says is live.
    std::optional<uint32_t> number = inputs().stringConstants.numberOf(id);
    if (!number)
        return CellPointer(callStub(context, createConstant(CreatedConstant::NewString), { }, { .immediate = id.index() }).first);

    bool isNarrow = !m_options.recorded.useWideReferences();
    return cellMadeByStub(context, createConstant(CreatedConstant::String), { .immediate = *number, .pureFunction = pureFunction(PureOperation::StringConstant, *number) }, isNarrow, [&] {
        return isNarrow
            ? instanceField(m_heaps.AOTInstance_stringCells, InstanceLayout::offsetOfStringCell(*number))
            : cellWordInTable(m_heaps.AOTInstance_stringCells, AOT_INSTANCE_OFFSET(runtime.constants.wideStringCells), *number);
    });
}

CellPointer ConstantEmitter::keyConstant(const OperationContext& context, PropertyKeyID key)
{
    if (!hasKeyCell(m_program.at(key).kind))
        return failWithoutCell(context, InternalErrorKind::OpcodeWithoutCode);
    std::optional<uint32_t> number = m_placement.keyCell(key);
    if (!number)
        return failWithoutCell(context, InternalErrorKind::PlacedPayloadMissing);

    // A key cell lives as long as the program, like a string cell. The check of narrow references knows that of a word it sees loaded from this heap.
    bool isNarrow = !m_options.recorded.useWideReferences();
    return cellMadeByStub(context, createConstant(CreatedConstant::Key), { .immediate = *number, .pureFunction = pureFunction(PureOperation::KeyConstant, *number) }, isNarrow, [&] {
        return cellWordInTable(m_heaps.AOTInstance_stringCells, offsetOfKeyCells(isNarrow), *number);
    });
}

CellPointer ConstantEmitter::bigIntConstant(const OperationContext& context, StringID digits)
{
    std::optional<uint32_t> number = m_placement.bigIntConstant(digits);
    if (!number)
        return failWithoutCell(context, InternalErrorKind::PlacedPayloadMissing);

    TypedPointer cellWord = cellWordInTable(m_heaps.entire(AbstractHeap::AllocatorState), AOT_INSTANCE_OFFSET(runtime.constants.bigIntCells), *number);
    return permanentCell(cellWord, false, [&] {
        CallOptions options { .pureFunction = pureFunction(PureOperation::BigIntConstant, *number), .isOnRarePath = true };
        return unboxCell(callOperation<Boxed>(context, RuntimeOperation::BigIntConstant, { word32(static_cast<int32_t>(*number)) }, options));
    });
}

CellPointer ConstantEmitter::builtinCell(const OperationContext& context, BuiltinCellKey key)
{
    std::optional<uint32_t> number = m_placement.builtinCell(key);
    if (!number)
        return failWithoutCell(context, InternalErrorKind::PlacedPayloadMissing);

    // The result is the same each time, and only the first call does anything: the host makes the object, and a member that its holder makes at the first read gets a slot in the holder.
    return cellMadeByStub(context, AOT::builtinCell(), { .immediate = *number, .pureFunction = pureFunction(PureOperation::BuiltinCell, *number), .runsCodeOfHost = true }, false, [&] {
        return builtinCellWord(*number);
    });
}

CellPointer ConstantEmitter::keyConstant(PropertyKeyID key)
{
    std::optional<uint32_t> number = m_placement.keyCell(key);
    RELEASE_ASSERT(number);
    bool isNarrow = !m_options.recorded.useWideReferences();
    TypedPointer cellWord = cellWordInTable(m_heaps.AOTInstance_stringCells, offsetOfKeyCells(isNarrow), *number);
    return permanentCell(cellWord, isNarrow, [&] {
        return callWithoutSite<CellPointer>(RuntimeOperation::KeyConstant, { word32(static_cast<int32_t>(*number)) }, { .pureFunction = pureFunction(PureOperation::KeyConstant, *number), .isOnRarePath = true });
    });
}

CellPointer ConstantEmitter::builtinCell(BuiltinID id)
{
    std::optional<uint32_t> number = m_placement.builtinCell(id);
    RELEASE_ASSERT(number);
    TypedPointer cellWord = builtinCellWord(*number);
    return permanentCell(cellWord, false, [&] {
        return callWithoutSite<CellPointer>(RuntimeOperation::BuiltinCell, { word32(static_cast<int32_t>(*number)) }, { .pureFunction = pureFunction(PureOperation::BuiltinCell, *number), .isOnRarePath = true });
    });
}

// A boxed cell is its address. No value has the bits of a word that is still empty, so whether the cell has been made matters only where the two differ.
void ConstantEmitter::branchOnIsBuiltinCell(LValue boxedValueOrCell, BuiltinID id, BranchTargets targets)
{
    std::optional<uint32_t> number = m_placement.builtinCell(id);
    RELEASE_ASSERT(number);
    LBasicBlock differs = newBlock();
    LBasicBlock isNotMade = newBlock();
    m_out.branch(m_out.equal(boxedValueOrCell, m_out.load64(builtinCellWord(*number))), targets.taken, B3::FrequentedBlock(differs, targets.notTaken.frequency()));

    // Read through the address of the table: a word that two blocks looked at would go through a register, and the usual way compares it where it lies.
    m_out.appendTo(differs);
    LValue cell = m_out.load64(cellWordInTable(m_heaps.entire(AbstractHeap::AllocatorState), AOT_INSTANCE_PREFIX_OFFSET(builtinCells), *number));
    m_out.branch(m_out.notZero64(cell), targets.notTaken, rareEdge(isNotMade));

    m_out.appendTo(isNotMade);
    CellPointer made = callWithoutSite<CellPointer>(RuntimeOperation::BuiltinCell, { word32(static_cast<int32_t>(*number)) }, { .pureFunction = pureFunction(PureOperation::BuiltinCell, *number), .isOnRarePath = true });
    m_out.branch(m_out.equal(boxedValueOrCell, made.value()), targets.taken, targets.notTaken);
}

Operand ConstantEmitter::constantOf(JSValue value, Representation representation)
{
    std::optional<uint16_t> marker = markerPrimitiveOf(value);
    auto markerBits = [&] {
        RELEASE_ASSERT(marker);
        return ConversionEmitter::markerBits(*marker, representation);
    };

    switch (representation) {
    case Representation::Void:
        return { };
    case Representation::JSValue:
        return boxedConstant(value);
    case Representation::Boolean:
        RELEASE_ASSERT(value.isBoolean());
        return boolean(value.asBoolean());
    case Representation::Int32:
        RELEASE_ASSERT(value.isInt32());
        return word32(value.asInt32());
    case Representation::Int64:
        return integer(trackedIntegerOf(value));
    case Representation::Double:
        RELEASE_ASSERT(value.isNumber());
        return number(value.asNumber());
    case Representation::OptionalInt64:
        return Operand(representation, m_out.constInt64(marker ? static_cast<int64_t>(markerBits()) : trackedIntegerOf(value)));
    case Representation::OptionalDouble:
        RELEASE_ASSERT(marker || value.isNumber());
        return Operand(representation, m_out.constDouble(marker ? std::bit_cast<double>(markerBits()) : value.asNumber()));
    case Representation::Cell32:
        return Operand(representation, m_out.constInt32(static_cast<int32_t>(markerBits())));
    case Representation::LiteralString:
        return Operand(representation, m_out.constInt64(static_cast<int64_t>(markerBits())));
    case Representation::Unchosen:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConstantEmitter::zeroOf(Representation representation)
{
    switch (representation) {
    case Representation::Void:
        return { };
    case Representation::Double:
    case Representation::OptionalDouble:
        return Operand(representation, m_out.constDouble(0));
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Cell32:
        return Operand(representation, m_out.constInt32(0));
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return Operand(representation, m_out.constInt64(0));
    case Representation::Unchosen:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ConstantEmitter::defineCell(const ValueContext& value, CellPointer cell)
{
    switch (value.representation()) {
    case Representation::Cell:
        define(value, cell);
        return;
    case Representation::JSValue:
        define(value, boxCell(cell));
        return;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        failAndDefineZero(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ConstantEmitter::failAndDefineZero(const ValueContext& value, InternalErrorKind kind)
{
    fail(value, kind);
    define(value, zeroOf(value.representation()));
}

CellPointer ConstantEmitter::failWithoutCell(const OperationContext& context, InternalErrorKind kind)
{
    fail(context, kind);
    return zeroOf(Representation::Cell).as<CellPointer>();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

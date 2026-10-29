/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHashTableBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTConversionEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRepresentation.h"
#include "AOTStringEmitter.h"
#include "AOTVector.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include "JSMap.h"

namespace JSC { namespace AOT { namespace Typed {

static KeyComparison keyComparisonOf(const Type& key)
{
    if (key.primitives & Primitive::AnyString)
        return key.hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull) ? KeyComparison::String : KeyComparison::AnyValue;
    if (key.primitives & Primitive::BigInt)
        return KeyComparison::AnyValue;
    if (!(key.primitives & Primitive::Number))
        return KeyComparison::Bits;
    switch (defaultRepresentationOf(key)) {
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return KeyComparison::Bits;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return KeyComparison::AnyValue;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StubKey lookupStubFor(HashTableOwner owner, bool returnsValue, KeyComparison comparison)
{
    switch (owner) {
    case HashTableOwner::Map:
        return returnsValue ? mapGet(comparison) : mapHas(comparison);
    case HashTableOwner::Set:
        return setHas(comparison);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

HashTableBuiltinEmitter::HashTableBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Boxed HashTableBuiltinEmitter::normalizedNumber(const OperationContext& context, Number key)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    auto anyNumber = [&] {
        // Every NaN is one key.
        Boxed boxedKey = conversions.boxNumber(ImpureNumber(key.value()));
        Condition isZero = equal(key, 0);
        return select(isZero, boxInt32(word32(0)), boxedKey);
    };
    if (context.shape() != CodeShape::Fast)
        return anyNumber();

    LBasicBlock fitsInt32 = newBlock();
    LBasicBlock otherwise = newBlock();
    Join<Boxed> join(m_out);
    Word32 truncated = low32(conversions.toInteger(key));
    m_out.branch(equal(toNumber(truncated), key).value(), normalEdge(fitsInt32), unlikelyEdge(otherwise));

    m_out.appendTo(fitsInt32);
    join.add(boxInt32(truncated));

    m_out.appendTo(otherwise);
    join.add(anyNumber());
    return join.finish();
}

Boxed HashTableBuiltinEmitter::normalizedKey(const OperationContext& context, ValueID id)
{
    const Type& type = typeOf(id);
    Boxed key = boxedOperand(context, id);
    bool calleeNormalizes = keyComparisonOf(type) != KeyComparison::Bits;
    if (calleeNormalizes || !(type.primitives & Primitive::Number))
        return key;

    Condition isBoxedDouble = logicalNot(isInt32(key));
    if (!type.isNumber())
        isBoxedDouble = logicalAnd(isNumber(key), isBoxedDouble);
    bool numbersFitInt32 = !(type.primitives & Primitive::Double) && type.integers.isSubsetOf(Range::int32());
    LBasicBlock needsNormalization = newBlock();
    LBasicBlock isNormalized = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(isBoxedDouble.value(), numbersFitInt32 ? unlikelyEdge(needsNormalization) : normalEdge(needsNormalization), normalEdge(isNormalized));

    m_out.appendTo(isNormalized);
    join.add(key);

    m_out.appendTo(needsNormalization);
    join.add(normalizedNumber(context, unboxDouble(key)));
    return join.finish();
}

Word32 HashTableBuiltinEmitter::hashOfBits(Boxed key)
{
    LValue a = m_out.bitXor(key.value(), loadImmutable64(AOT_INSTANCE_OFFSET(numbers.hashSecrets)).value());
    LValue b = m_out.bitXor(key.value(), loadImmutable64(AOT_INSTANCE_OFFSET(numbers.hashSecrets) + sizeof(uint64_t)).value());
    LValue lowerHalf = m_out.mul(a, b);
    return Word32(m_out.castToInt32(m_out.bitXor(lowerHalf, m_out.uMulHigh(a, b))));
}

Integer HashTableBuiltinEmitter::sizeOf(CellPointer table, HashTableOwner owner)
{
    CellPointer storage = loadField<CellPointer>(table, owner == HashTableOwner::Map ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
    return either<Integer>(Condition(m_out.notNull(storage.value())), [&] {
        return zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), storage.value(), CompactTable::offsetOfAliveCount()))));
    }, [&] {
        return integer(0);
    });
}

// A table of at most eight entries keeps a byte of each key's hash in one word. The stub takes every other table, a key without a hash, and a key whose characters must be compared.
// What depends on the table alone is read from a header that is always there, so those loads can leave a loop.
Boxed HashTableBuiltinEmitter::lookUpAmongTags(const InlineCall& call, CellPointer owner, CellPointer key, StubKey stub, bool returnsValue)
{
    constexpr ptrdiff_t offsetOfTags = CompactTable::offsetOfIndex();
    constexpr ptrdiff_t offsetOfKeys = offsetOfTags + CompactTable::maxCapacityWithTags;
    // The word at the capacity holds the capacity, the index bits, the flags of the keys and the width of the values.
    constexpr uint64_t otherKinds = (0xffull | (CompactTable::keysAreInImage | CompactTable::keysAreWide) << 8) << 32;
    constexpr uint64_t ones = 0x0101010101010101;
    static_assert(!(CompactTable::offsetOfCapacity() % 8) && CompactTable::offsetOfIndexBits() == CompactTable::offsetOfCapacity() + 4 && CompactTable::offsetOfKeysShift() == CompactTable::offsetOfCapacity() + 5 && CompactTable::offsetOfValueWidth() == CompactTable::offsetOfCapacity() + 6);
    static_assert(offsetOfKeys == sizeof(EmptyTableWords) && CompactTable::maxCapacityWithTags == sizeof(uint64_t));
    auto inHeader = [&](LValue table, ptrdiff_t offset) { return m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), table, offset); };
    auto inColumns = [&](LValue address, ptrdiff_t offset) { return m_out.address(m_heaps.entire(AbstractHeap::CollectionEntries), address, offset); };

    LBasicBlock hasTags = newBlock();
    LBasicBlock isHashed = newBlock();
    LBasicBlock hasMatch = newBlock();
    LBasicBlock isUsed = newBlock();
    LBasicBlock isOtherWord = newBlock();
    LBasicBlock isOtherCell = newBlock();
    LBasicBlock isOtherString = newBlock();
    LBasicBlock found = newBlock();
    LBasicBlock absent = newBlock();
    LBasicBlock byStub = newBlock();
    Join<Boxed> join(m_out);

    m_proc.setKnownDereferenceableBytes(owner.value(), JSMap::offsetOfStorage() + sizeof(void*));
    LValue storage = m_out.loadPtr(inHeader(owner.value(), JSMap::offsetOfStorage()));
    LValue emptyTable = m_out.add(instance().value(), m_out.constIntPtr(AOT_INSTANCE_OFFSET(emptyTable)));
    LValue table = m_out.select(m_out.notNull(storage), storage, emptyTable);
    m_proc.setKnownDereferenceableBytes(table, offsetOfKeys);
    LValue kinds = m_out.load64(inHeader(table, CompactTable::offsetOfCapacity()));
    m_out.branch(m_out.testIsZero64(kinds, m_out.constInt64(otherKinds)), normalEdge(hasTags), unlikelyEdge(byStub));

    // B3 moves out of a loop what stands on every likely path through it, so the rest of the header is read here.
    m_out.appendTo(hasTags);
    LValue tags = m_out.load64(inColumns(table, offsetOfTags));
    LValue bitsOfUsedTags = m_out.shl(m_out.zeroExtPtr(m_out.load32(inHeader(table, CompactTable::offsetOfUsed()))), m_out.constInt32(3));
    LValue values = returnsValue ? m_out.add(table, m_out.shl(m_out.zeroExtPtr(m_out.castToInt32(kinds)), m_out.constInt32(2))) : nullptr;
    LValue valuesAreNarrow = returnsValue ? m_out.equal(m_out.bitAnd(m_out.lShr(kinds, m_out.constInt32(48)), m_out.constInt64(0xff)), m_out.constInt64(static_cast<int64_t>(CompactTable::Width::Narrow))) : nullptr;
    Pointer impl = emitter<StringEmitter>().implOfFlatString(key);
    LValue hashAndFlags = loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags).value();
    m_out.branch(m_out.aboveOrEqual(hashAndFlags, m_out.constInt32(1 << StringImpl::s_flagCount)), normalEdge(isHashed), unlikelyEdge(byStub));

    m_out.appendTo(isHashed);
    LValue tag = m_out.zeroExtPtr(m_out.lShr(hashAndFlags, m_out.constInt32(StringImpl::s_flagCount + CompactTable::tagShiftOfTags)));
    LValue difference = m_out.bitXor(m_out.mul(tag, m_out.constInt64(ones)), tags);
    LValue matches = m_out.bitAnd(m_out.bitAnd(m_out.sub(difference, m_out.constInt64(ones)), m_out.bitNot(difference)), m_out.constInt64(ones << 7));
    m_out.branch(m_out.isZero64(matches), normalEdge(absent), normalEdge(hasMatch));

    m_out.appendTo(hasMatch);
    B3::PatchpointValue* firstMatch = m_out.patchpoint(B3::Int64);
    firstMatch->appendSomeRegister(matches);
    firstMatch->effects = B3::Effects::none();
    firstMatch->setRunsWithoutFrame(true);
    firstMatch->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.countTrailingZeros64WithoutNullCheck(params[1].gpr(), params[0].gpr());
    });
    // The bit is the highest of the tag of the entry: 8 * entry + 7. Half of that, less 3, is the offset in a narrow column, and the whole, less 7, the one in a wide column.
    m_out.branch(m_out.below(firstMatch, bitsOfUsedTags), normalEdge(isUsed), unlikelyEdge(absent));

    m_out.appendTo(isUsed);
    LValue offsetInNarrowColumn = m_out.lShr(firstMatch, m_out.constInt32(1));
    NarrowReference word(m_out.load32(inColumns(m_out.add(table, offsetInNarrowColumn), offsetOfKeys - 3)));
    CellPointer candidate = decode(word);
    m_out.branch(m_out.equal(candidate.value(), key.value()), normalEdge(found), unlikelyEdge(isOtherWord));

    m_out.appendTo(isOtherWord);
    m_out.branch(isCellReferenceAmongAllWords(word).value(), normalEdge(isOtherCell), unlikelyEdge(byStub));

    m_out.appendTo(isOtherCell);
    m_out.branch(m_out.equal(m_out.load8ZeroExt32(candidate.value(), m_heaps.JSCell_typeInfoType), m_out.constInt32(StringType)), normalEdge(isOtherString), unlikelyEdge(byStub));

    m_out.appendTo(isOtherString);
    m_out.branch(m_out.equal(loadField<Pointer>(candidate, m_heaps.JSString_value).value(), impl.value()), normalEdge(found), unlikelyEdge(byStub));

    m_out.appendTo(found);
    if (!returnsValue)
        join.add(boxedConstant(jsBoolean(true)));
    else {
        LBasicBlock isNarrow = newBlock();
        LBasicBlock isWide = newBlock();
        // A narrow column holds no number.
        bool expectsNumbers = typeOf(call.context().valueID()).primitives & Primitive::Number;
        m_out.branch(valuesAreNarrow, expectsNumbers ? unlikelyEdge(isNarrow) : normalEdge(isNarrow), expectsNumbers ? normalEdge(isWide) : unlikelyEdge(isWide));

        m_out.appendTo(isNarrow);
        NarrowReference narrow(m_out.load32(inColumns(m_out.add(values, offsetInNarrowColumn), offsetOfKeys - 3)));
        join.add(select(isCellReferenceAmongAllWords(narrow), boxCell(decode(narrow)), Boxed(m_out.zeroExtPtr(narrow.value()))));

        m_out.appendTo(isWide);
        join.add(Boxed(m_out.load64(inColumns(m_out.add(values, firstMatch), offsetOfKeys - 7))));
    }

    m_out.appendTo(absent);
    join.add(boxedConstant(returnsValue ? jsUndefined() : jsBoolean(false)));

    m_out.appendTo(byStub);
    join.add(Boxed(callStub(call.context(), stub, { owner.value(), key.value() }).first));
    return join.finish();
}

static bool hasOnlyMapsAndSets(const Program& program, const Type& type)
{
    auto isMapOrSet = [](BuiltinClassID builtinClass) {
        return builtinClass == BuiltinClasses::Map || builtinClass == BuiltinClasses::Set;
    };
    if (!type.isObject() || type.anyObject || type.anyFunction || !type.functions.isEmpty() || !type.builtins.isEmpty())
        return false;
    for (const ClassPart& part : type.classes) {
        if (!isMapOrSet(part.builtinClass))
            return false;
    }
    return std::ranges::all_of(membersOf(program, type), [&](LayoutID layout) {
        return isMapOrSet(builtinBaseClassOf(program, layout));
    });
}

bool HashTableBuiltinEmitter::looksAmongTags(const InlineCall& call)
{
    // A larger table pays for the header that is read here and again in the stub. Keys that are written in the program are few, and mostly so are the entries of the table they are looked up in.
    if (call.context().shape() != CodeShape::Fast || !typeOf(call.argument(0)).hasOnlyPrimitives(Primitive::LiteralString) || m_options.recorded.useWideReferences())
        return false;
    // B3 may read the table wherever it can compute the receiver, so the receiver has to be a Map or a Set there, not only behind a test.
    // A cast is lowered to the value that it casts.
    ValueID source = call.receiver();
    while (resultRuleOf(m_function.at(source).opcode) == ResultRule::Cast)
        source = m_function.at(source).valueOperand();
    if (!hasOnlyMapsAndSets(m_program, typeOf(source)) || !generator().holdsTypeWhereverComputed(source))
        return false;
    // Only out of a loop can the header be moved.
    const NaturalLoops& loops = analyses().naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(call.context().block());
    BlockID definition = analyses().useLists().blockOf(call.receiver());
    return loop && !(definition && loops.belongsTo(definition, *loop));
}

void HashTableBuiltinEmitter::lookUp(const InlineCall& call, HashTableOwner owner, bool returnsValue)
{
    RELEASE_ASSERT(owner == HashTableOwner::Map || !returnsValue);
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    StubKey stub = lookupStubFor(owner, returnsValue, comparison);
    Boxed answer = looksAmongTags(call) ? lookUpAmongTags(call, table, unboxCell(key), stub, returnsValue) : Boxed(callStub(call.context(), stub, { table.value(), key.value() }).first);
    if (returnsValue)
        finish(call, answer);
    else
        finish(call, unboxBoolean(answer));
}

void HashTableBuiltinEmitter::insert(const InlineCall& call, HashTableOwner owner)
{
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    switch (owner) {
    case HashTableOwner::Map:
        callStub(call.context(), mapSet(comparison), { table.value(), key.value(), boxedOperand(call.context(), call.argument(1)).value() });
        break;
    case HashTableOwner::Set:
        callStub(call.context(), setAdd(comparison), { table.value(), key.value() });
        break;
    }
    finish(call, table);
}

void HashTableBuiltinEmitter::remove(const InlineCall& call, HashTableOwner owner)
{
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    StubKey stub = owner == HashTableOwner::Map ? mapDelete(comparison) : setDelete(comparison);
    finish(call, unboxBoolean(Boxed(callStub(call.context(), stub, { table.value(), key.value() }).first)));
}

void HashTableBuiltinEmitter::generateSize(const InlineCall& call, HashTableOwner owner)
{
    finish(call, sizeOf(cellOperand(call.receiver()), owner), Range::length());
}

void HashTableBuiltinEmitter::generateNewMap(const InlineCall& call)
{
    finish(call, CellPointer(callStub(call.context(), newMap(), { }).first));
}

void HashTableBuiltinEmitter::generateNewSet(const InlineCall& call)
{
    finish(call, CellPointer(callStub(call.context(), newSet(), { }).first));
}

void HashTableBuiltinEmitter::generateMapGet(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Map, true);
}

void HashTableBuiltinEmitter::generateMapHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Map, false);
}

void HashTableBuiltinEmitter::generateSetHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Set, false);
}

void HashTableBuiltinEmitter::generateMapSet(const InlineCall& call)
{
    insert(call, HashTableOwner::Map);
}

void HashTableBuiltinEmitter::generateSetAdd(const InlineCall& call)
{
    insert(call, HashTableOwner::Set);
}

void HashTableBuiltinEmitter::generateMapSize(const InlineCall& call)
{
    generateSize(call, HashTableOwner::Map);
}

void HashTableBuiltinEmitter::generateSetSize(const InlineCall& call)
{
    generateSize(call, HashTableOwner::Set);
}

KeyPartsLookupEmitter::KeyPartsLookupEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Pointer KeyPartsLookupEmitter::partsBuffer(const ValueContext& value)
{
    Vector<Boxed, 8> parts;
    for (ValueID part : value->partsOperands())
        parts.append(boxedOperand(value, part));
    return argumentBuffer(parts.span());
}

void KeyPartsLookupEmitter::generateMapGetByKeyParts(const ValueContext& value)
{
    Word32 count = word32(static_cast<int32_t>(value->partsOperands().size()));
    define(value, callOperation<Boxed>(value, RuntimeOperation::MapGetByKeyParts, { cellOperand(value->mapOperand()), count, partsBuffer(value) }));
}

void KeyPartsLookupEmitter::generateHasByKeyParts(const ValueContext& value)
{
    Word32 count = word32(static_cast<int32_t>(value->partsOperands().size()));
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::HasByKeyParts, { cellOperand(value->collectionOperand()), count, partsBuffer(value) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

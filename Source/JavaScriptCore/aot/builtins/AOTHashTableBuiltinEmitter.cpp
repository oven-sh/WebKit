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
#include "AOTRuntimeCallEmitter.h"
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

bool HashTableBuiltinEmitter::hasReceiverThatLeavesLoop(const InlineCall& call)
{
    if (call.context().shape() != CodeShape::Fast || m_options.recorded.useWideReferences())
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
    if (!loop || (definition && loops.belongsTo(definition, *loop)))
        return false;
    FunctionAnalyses& functionAnalyses = analyses();
    auto writesTables = [](const Effects& effects) { return effects.writes.isTop() || effects.writes.contains(AbstractHeap::CollectionStorage); };
    for (unsigned i = 0; i < loop->size(); ++i) {
        BlockID block = loop->at(i);
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            // A lookup only moves a table in the image, which code that has read it takes the stub for.
            if (value.opcode == Opcode::CallInline && (value.inlineBuiltin() == InlineBuiltin::MapGet || value.inlineBuiltin() == InlineBuiltin::MapHas || value.inlineBuiltin() == InlineBuiltin::SetHas))
                continue;
            if (writesTables(functionAnalyses.effectsOf(id).onAnyEdge()))
                return false;
        }
        if (writesTables(functionAnalyses.effectsOfTerminator(block).onAnyEdge()))
            return false;
    }
    return true;
}

bool HashTableBuiltinEmitter::looksAmongTags(const InlineCall& call)
{
    // A larger table pays for the header that is read here and again in the stub. Keys that are written in the program are few, and mostly so are the entries of the table they are looked up in.
    return typeOf(call.argument(0)).hasOnlyPrimitives(Primitive::LiteralString) && hasReceiverThatLeavesLoop(call);
}

bool HashTableBuiltinEmitter::looksIntoHomeSlot(const InlineCall& call, KeyComparison comparison)
{
    // A string that was just made has no hash yet, so the stub would run as well.
    static constexpr OpcodeSet makersOfStrings { Opcode::Add, Opcode::CallBuiltin, Opcode::Strcat, Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::StringBuilderFinish, Opcode::StringBuilderToString, Opcode::ToString };
    ValueID key = call.argument(0);
    const Type& type = typeOf(key);
    bool isKey = comparison == KeyComparison::String ? !makersOfStrings.contains(m_function.at(key).opcode) : comparison == KeyComparison::Bits && type.isNumber();
    return isKey && hasReceiverThatLeavesLoop(call);
}

// Reads the table once, out of the loop where B3 can move it. A table in the image, one that is too large and a string without a hash or another cell with the same text go to the stub.
Boxed HashTableBuiltinEmitter::lookUpInHomeSlot(const InlineCall& call, CellPointer owner, Boxed key, KeyComparison comparison, HashTableOwner kindOfOwner, bool returnsValue)
{
    constexpr ptrdiff_t index = CompactTable::offsetOfIndex();
    constexpr uint64_t imageFlag = static_cast<uint64_t>(CompactTable::keysAreInImage) << 40;
    constexpr uint64_t wideFlag = static_cast<uint64_t>(CompactTable::keysAreWide) << 40;
    constexpr uint64_t indexBitsMask = 0xffull << 32;
    constexpr uint64_t ones = 0x0101010101010101;
    const bool keysAreWide = comparison == KeyComparison::Bits;
    auto inHeader = [&](LValue table, ptrdiff_t offset) { return m_out.address(m_heaps.AOTCompactTable_lookupHeader, table, offset); };
    auto inColumns = [&](LValue address) { return m_out.address(m_heaps.entire(AbstractHeap::CollectionEntries), address, 0); };

    LBasicBlock isOfKindAndSize = newBlock();
    LBasicBlock hasHash = newBlock();
    LBasicBlock hasTags = newBlock();
    LBasicBlock matchAmongTags = newBlock();
    LBasicBlock hasSlots = newBlock();
    LBasicBlock atHome = newBlock();
    LBasicBlock notAtHome = newBlock();
    LBasicBlock searchesEight = newBlock();
    LBasicBlock matchAmongEight = newBlock();
    LBasicBlock noMatchAmongEight = newBlock();
    LBasicBlock found = newBlock();
    LBasicBlock absent = newBlock();
    LBasicBlock byStub = newBlock();
    Join<Boxed> join(m_out);
    emitter<RuntimeCallEmitter>().setCallsOnlyMoveTables(true);

    m_proc.setKnownDereferenceableBytes(owner.value(), JSMap::offsetOfStorage() + sizeof(void*));
    LValue storage = m_out.loadPtr(inHeader(owner.value(), JSMap::offsetOfStorage()));
    LValue emptyTable = m_out.add(instance().value(), m_out.constIntPtr(AOT_INSTANCE_OFFSET(emptyTable)));
    LValue table = m_out.select(m_out.notNull(storage), storage, emptyTable);
    m_proc.setKnownDereferenceableBytes(table, index + CompactTable::maxCapacityWithTags);
    LValue kinds = m_out.load64(inHeader(table, CompactTable::offsetOfCapacity()));

    // All that the table alone says is worked out here, where B3 can move it out of the loop. One test is left in the loop.
    LValue capacity = m_out.zeroExtPtr(m_out.castToInt32(kinds));
    LValue indexBits = m_out.bitAnd(m_out.lShr(kinds, m_out.constInt32(32)), m_out.constInt64(0xff));
    LValue slots = m_out.shl(m_out.constInt64(1), m_out.castToInt32(indexBits));
    LValue slotMask = m_out.sub(slots, m_out.constInt64(1));
    LValue hasLongSlots = m_out.above(capacity, m_out.constInt64(CompactTable::maxCapacityWithShortSlots));
    LValue bytesOfNumber = m_out.select(hasLongSlots, m_out.constInt64(3), m_out.constInt64(1));
    LValue numbers = m_out.add(table, m_out.constIntPtr(index));
    LValue tags = m_out.add(numbers, m_out.mul(slots, bytesOfNumber));
    LValue hasNoSlots = m_out.testIsZero64(kinds, m_out.constInt64(indexBitsMask));
    LValue keys = m_out.select(hasNoSlots, m_out.add(numbers, m_out.constIntPtr(CompactTable::maxCapacityWithTags)), m_out.add(tags, slots));
    auto loadWordOfTags = [&] { return m_out.load64(inColumns(numbers)); };
    auto loadBitsOfUsedTags = [&] { return m_out.shl(m_out.zeroExtPtr(m_out.load32(inHeader(table, CompactTable::offsetOfUsed()))), m_out.constInt32(3)); };
    // A map of number keys is rarely small enough for tags: their reads stay out of the way of the slots' registers.
    LValue wordOfTags = keysAreWide ? nullptr : loadWordOfTags();
    LValue bitsOfUsedTags = keysAreWide ? nullptr : loadBitsOfUsedTags();
    LValue values = returnsValue ? m_out.add(keys, m_out.shl(capacity, m_out.constInt32(keysAreWide ? 3 : 2))) : nullptr;
    LValue valuesAreNarrow = returnsValue ? m_out.equal(m_out.bitAnd(m_out.lShr(kinds, m_out.constInt32(48)), m_out.constInt64(0xff)), m_out.constInt64(static_cast<int64_t>(CompactTable::Width::Narrow))) : nullptr;
    LValue isOfKind = m_out.equal(m_out.bitAnd(kinds, m_out.constInt64(imageFlag | wideFlag)), m_out.constInt64(keysAreWide ? wideFlag : 0));
    LValue isNotHuge = m_out.belowOrEqual(capacity, m_out.constInt64(CompactTable::maxCapacityWithLongSlots));
    // One word says how to search. It takes one register in the loop, where the tests it stands for would take three.
    enum Form : int32_t { Tags, ShortSlots, LongSlots, OtherTable };
    LValue searchableForm = m_out.select(hasNoSlots, m_out.constInt32(Tags), m_out.select(hasLongSlots, m_out.constInt32(LongSlots), m_out.constInt32(ShortSlots)));
    LValue form = m_out.select(m_out.bitAnd(isOfKind, isNotHuge), searchableForm, m_out.constInt32(OtherTable));
    m_out.branch(m_out.below(form, m_out.constInt32(OtherTable)), normalEdge(isOfKindAndSize), unlikelyEdge(byStub));

    m_out.appendTo(isOfKindAndSize);
    LValue hash;
    if (keysAreWide) {
        hash = m_out.bitAnd(hashOfBits(key).value(), m_out.constInt32(CompactTable::hashMask));
        m_out.jump(hasHash);
    } else {
        LBasicBlock isCell = newBlock();
        LBasicBlock isFlat = newBlock();
        LBasicBlock isHashed = newBlock();
        m_out.branch(PrimitiveEmitter::isCell(key).value(), normalEdge(isCell), unlikelyEdge(byStub));
        m_out.appendTo(isCell);
        Pointer impl = loadField<Pointer>(unboxCell(key), m_heaps.JSString_value);
        m_out.branch(hasNoBits(impl, JSString::isRopeInPointer).value(), normalEdge(isFlat), unlikelyEdge(byStub));
        m_out.appendTo(isFlat);
        LValue hashAndFlags = loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags).value();
        m_out.branch(m_out.aboveOrEqual(hashAndFlags, m_out.constInt32(1 << StringImpl::s_flagCount)), normalEdge(isHashed), unlikelyEdge(byStub));
        m_out.appendTo(isHashed);
        hash = m_out.lShr(hashAndFlags, m_out.constInt32(StringImpl::s_flagCount));
        m_out.jump(hasHash);
    }

    auto loadNumber = [&](LValue slot) {
        LBasicBlock isLong = newBlock();
        LBasicBlock isShort = newBlock();
        LBasicBlock loaded = newBlock();
        m_out.branch(m_out.equal(form, m_out.constInt32(LongSlots)), unlikelyEdge(isLong), normalEdge(isShort));
        m_out.appendTo(isLong);
        LValue threeBytes = m_out.bitAnd(m_out.load32(inColumns(m_out.add(numbers, m_out.mul(slot, m_out.constInt64(3))))), m_out.constInt32(0xffffff));
        ValueFromBlock fromLong = m_out.anchor(threeBytes);
        m_out.jump(loaded);
        m_out.appendTo(isShort);
        ValueFromBlock fromShort = m_out.anchor(m_out.load8ZeroExt32(inColumns(m_out.add(numbers, slot))));
        m_out.jump(loaded);
        m_out.appendTo(loaded);
        return m_out.zeroExtPtr(m_out.phi(B3::Int32, fromLong, fromShort));
    };
    auto keyIsAt = [&](LValue number) {
        if (keysAreWide)
            return m_out.equal(m_out.load64(inColumns(m_out.add(keys, m_out.shl(number, m_out.constInt32(3))))), key.value());
        NarrowReference word(m_out.load32(inColumns(m_out.add(keys, m_out.shl(number, m_out.constInt32(2))))));
        return m_out.equal(decode(word).value(), key.value());
    };

    auto firstBitOf = [&](LValue bits) {
        B3::PatchpointValue* firstBit = m_out.patchpoint(B3::Int64);
        firstBit->appendSomeRegister(bits);
        firstBit->effects = B3::Effects::none();
        firstBit->setRunsWithoutFrame(true);
        firstBit->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            jit.countTrailingZeros64WithoutNullCheck(params[1].gpr(), params[0].gpr());
        });
        return firstBit;
    };

    m_out.appendTo(hasHash);
    m_out.branch(m_out.equal(form, m_out.constInt32(Tags)), keysAreWide ? unlikelyEdge(hasTags) : normalEdge(hasTags), normalEdge(hasSlots));

    m_out.appendTo(hasTags);
    if (keysAreWide)
        wordOfTags = loadWordOfTags();
    LValue differenceOfTags = m_out.bitXor(wordOfTags, m_out.mul(m_out.zeroExtPtr(m_out.lShr(hash, m_out.constInt32(CompactTable::tagShiftOfTags))), m_out.constInt64(ones)));
    LValue matchesAmongTags = m_out.bitAnd(m_out.bitAnd(m_out.sub(differenceOfTags, m_out.constInt64(ones)), m_out.bitNot(differenceOfTags)), m_out.constInt64(ones << 7));
    m_out.branch(m_out.isZero64(matchesAmongTags), normalEdge(absent), normalEdge(matchAmongTags));

    m_out.appendTo(matchAmongTags);
    LValue bitOfMatch = firstBitOf(matchesAmongTags);
    LBasicBlock isUsed = newBlock();
    if (keysAreWide)
        bitsOfUsedTags = loadBitsOfUsedTags();
    m_out.branch(m_out.below(bitOfMatch, bitsOfUsedTags), normalEdge(isUsed), unlikelyEdge(absent));

    m_out.appendTo(isUsed);
    LValue numberOfMatch = m_out.lShr(bitOfMatch, m_out.constInt32(3));
    ValueFromBlock numberAmongTags = m_out.anchor(numberOfMatch);
    m_out.branch(keyIsAt(numberOfMatch), normalEdge(found), unlikelyEdge(byStub));

    m_out.appendTo(hasSlots);
    LValue slot = m_out.bitAnd(m_out.zeroExtPtr(hash), slotMask);
    LValue tag = m_out.bitOr(m_out.lShr(hash, m_out.constInt32(CompactTable::tagShiftOfSlots)), m_out.constInt32(CompactTable::takenSlot));
    LValue tagAtHome = m_out.load8ZeroExt32(inColumns(m_out.add(tags, slot)));
    m_out.branch(m_out.equal(tagAtHome, tag), normalEdge(atHome), unlikelyEdge(notAtHome));

    m_out.appendTo(atHome);
    LValue numberAtHome = loadNumber(slot);
    ValueFromBlock numberOfHome = m_out.anchor(numberAtHome);
    // One key in 128 has the tag of another, and two string cells may hold one text: the stub sorts those out.
    m_out.branch(keyIsAt(numberAtHome), normalEdge(found), unlikelyEdge(byStub));

    m_out.appendTo(notAtHome);
    m_out.branch(m_out.isZero32(tagAtHome), normalEdge(absent), normalEdge(searchesEight));

    // The eight tags that the home slot lies among, for one key in four. A second match or a key behind them is left to the stub.
    m_out.appendTo(searchesEight);
    LValue first = m_out.bitAnd(slot, m_out.constInt64(~static_cast<int64_t>(CompactTable::slotsPerGroup - 1)));
    LValue eightTags = m_out.load64(inColumns(m_out.add(tags, first)));
    LValue difference = m_out.bitXor(eightTags, m_out.mul(m_out.zeroExtPtr(tag), m_out.constInt64(ones)));
    LValue matches = m_out.bitAnd(m_out.bitAnd(m_out.sub(difference, m_out.constInt64(ones)), m_out.bitNot(difference)), m_out.constInt64(ones << 7));
    m_out.branch(m_out.isZero64(matches), normalEdge(noMatchAmongEight), normalEdge(matchAmongEight));

    m_out.appendTo(matchAmongEight);
    LValue firstMatch = firstBitOf(matches);
    LValue numberAmongEight = loadNumber(m_out.add(first, m_out.lShr(firstMatch, m_out.constInt32(3))));
    ValueFromBlock numberOfEight = m_out.anchor(numberAmongEight);
    m_out.branch(keyIsAt(numberAmongEight), normalEdge(found), unlikelyEdge(byStub));

    m_out.appendTo(noMatchAmongEight);
    // A taken slot has the top bit of its tag set, as the tag has.
    m_out.branch(m_out.testNonZero64(difference, m_out.constInt64(ones << 7)), normalEdge(absent), unlikelyEdge(byStub));

    m_out.appendTo(found);
    LValue number = m_out.phi(B3::Int64, numberAmongTags, numberOfHome, numberOfEight);
    if (!returnsValue)
        join.add(boxedConstant(jsBoolean(true)));
    else {
        LBasicBlock isNarrow = newBlock();
        LBasicBlock isWide = newBlock();
        // A narrow column holds no number.
        bool expectsNumbers = typeOf(call.context().valueID()).primitives & Primitive::Number;
        m_out.branch(valuesAreNarrow, expectsNumbers ? unlikelyEdge(isNarrow) : normalEdge(isNarrow), expectsNumbers ? normalEdge(isWide) : unlikelyEdge(isWide));

        m_out.appendTo(isNarrow);
        NarrowReference narrow(m_out.load32(inColumns(m_out.add(values, m_out.shl(number, m_out.constInt32(2))))));
        join.add(select(isCellReferenceAmongAllWords(narrow), boxCell(decode(narrow)), Boxed(m_out.zeroExtPtr(narrow.value()))));

        m_out.appendTo(isWide);
        join.add(Boxed(m_out.load64(inColumns(m_out.add(values, m_out.shl(number, m_out.constInt32(3)))))));
    }

    m_out.appendTo(absent);
    join.add(boxedConstant(returnsValue ? jsUndefined() : jsBoolean(false)));

    m_out.appendTo(byStub);
    join.add(Boxed(callStub(call.context(), lookupStubFor(kindOfOwner, returnsValue, comparison), { owner.value(), key.value() }).first));
    return join.finish();
}

void HashTableBuiltinEmitter::lookUp(const InlineCall& call, HashTableOwner owner, bool returnsValue)
{
    RELEASE_ASSERT(owner == HashTableOwner::Map || !returnsValue);
    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    StubKey stub = lookupStubFor(owner, returnsValue, comparison);
    Boxed answer = looksAmongTags(call) ? lookUpAmongTags(call, table, unboxCell(key), stub, returnsValue) : looksIntoHomeSlot(call, comparison) ? lookUpInHomeSlot(call, table, key, comparison, owner, returnsValue) : Boxed(callStub(call.context(), stub, { table.value(), key.value() }).first);
    emitter<RuntimeCallEmitter>().setCallsOnlyMoveTables(false);
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

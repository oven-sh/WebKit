/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHashTableBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
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
#include "RegisterSet.h"

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

static StubKey lookupStubFor(HashTableOwner owner, LookupAnswer answer, KeyComparison comparison)
{
    switch (owner) {
    case HashTableOwner::Map:
        return answer == LookupAnswer::IsPresent ? mapHas(comparison) : answer == LookupAnswer::Value ? mapGet(comparison) : mapFind(comparison);
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

// For code on the hot path of a lookup: B3 would do two multiplications, and one mul gives both halves, as in the stubs.
// The secrets are read where they are used: moved out of a loop they would hold two registers that the search needs.
Word32 HashTableBuiltinEmitter::hashOfBitsByOneMultiplication(Boxed key)
{
#if CPU(X86_64)
    B3::PatchpointValue* halves = m_out.patchpoint(B3::Int64);
    halves->append(key.value(), B3::ValueRep::SomeLateRegister);
    halves->resultConstraints = { B3::ValueRep::reg(X86Registers::eax) };
    halves->clobber(RegisterSet(X86Registers::edx));
    halves->effects = B3::Effects::none();
    halves->setRunsWithoutFrame(true);
    halves->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        GPRReg key = params[1].gpr();
        jit.load64(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets)), X86Registers::eax);
        jit.xor64(key, X86Registers::eax);
        jit.load64(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(numbers.hashSecrets) + sizeof(uint64_t)), X86Registers::edx);
        jit.xor64(key, X86Registers::edx);
        jit.m_assembler.mulq_r(X86Registers::edx);
        jit.m_assembler.xorq_rr(X86Registers::edx, X86Registers::eax);
    });
    return Word32(m_out.castToInt32(halves));
#else
    return hashOfBits(key);
#endif
}

// A set bit at the top of each of the eight tags that differs from the tag by a number below the distance. The tag above such a one may be marked too.
// B3 would load each of the three constants wherever it is used, and test the result by working it out again.
LValue HashTableBuiltinEmitter::tagsNearTag(LValue eightTags, LValue tag, uint8_t distance)
{
    constexpr uint64_t ones = CompactTable::onesOfTags;
#if CPU(X86_64)
    B3::PatchpointValue* marks = m_out.patchpoint(B3::Int64);
    marks->append(eightTags, B3::ValueRep::SomeRegister);
    marks->append(tag, B3::ValueRep::SomeRegister);
    marks->numGPScratchRegisters = 1;
    marks->effects = B3::Effects::none();
    marks->setRunsWithoutFrame(true);
    marks->setGenerator([distance](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        GPRReg marks = params[0].gpr();
        GPRReg difference = params.gpScratch(0);
        jit.move(CCallHelpers::TrustedImm64(ones), difference);
        jit.mul64(params[2].gpr(), difference);
        jit.xor64(params[1].gpr(), difference);
        // (difference - distance) & ~difference, as the complement of (distance - 1 - difference) | difference.
        jit.move(CCallHelpers::TrustedImm64(distance * ones - 1), marks);
        jit.sub64(difference, marks);
        jit.or64(difference, marks);
        jit.not64(marks);
        jit.move(CCallHelpers::TrustedImm64(ones << 7), difference);
        jit.and64(difference, marks);
    });
    return marks;
#else
    LValue difference = m_out.bitXor(eightTags, m_out.mul(tag, m_out.constInt64(ones)));
    return m_out.bitAnd(m_out.bitAnd(m_out.sub(difference, m_out.constInt64(distance * ones)), m_out.bitNot(difference)), m_out.constInt64(ones << 7));
#endif
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
static JSValue answerWhenAbsent(LookupAnswer answer)
{
    switch (answer) {
    case LookupAnswer::IsPresent:
        return jsBoolean(false);
    case LookupAnswer::Value:
        return jsUndefined();
    case LookupAnswer::ValueOrNothing:
        return JSValue();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed HashTableBuiltinEmitter::lookUpAmongTags(const InlineCall& call, CellPointer owner, CellPointer key, StubKey stub, LookupAnswer answer, ValueID valueRead)
{
    const bool returnsValue = answer != LookupAnswer::IsPresent;
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
    // The type says which characters the key has, not which object it is: a text that was joined or cut out of another one may still be a rope. What is written here is flat.
    ValueID written = call.argument(0);
    while (resultRuleOf(m_function.at(written).opcode) == ResultRule::Cast)
        written = m_function.at(written).valueOperand();
    bool isWrittenHere = m_function.at(written).opcode == Opcode::String;
    build().statistics().count(isWrittenHere ? StatisticID::TagLookupsWithConstantKeys : StatisticID::TagLookupsWithOtherKeys);
    if (!isWrittenHere) {
        Subject subject = analyses().subject();
        subject.value = call.context().valueID();
        subject.location = m_function.locationOf(call.context().origin());
        build().addDecline({ .kind = Wire::DeclineKind::TagLookupKeyNotConstant, .witness = witnessOf(subject) });
    }
    Pointer impl = isWrittenHere ? emitter<StringEmitter>().implOfFlatString(key) : loadField<Pointer>(key, m_heaps.JSString_value);
    if (!isWrittenHere) {
        LBasicBlock isFlat = newBlock();
        m_out.branch(hasNoBits(impl, JSString::isRopeInPointer).value(), normalEdge(isFlat), unlikelyEdge(byStub));
        m_out.appendTo(isFlat);
    }
    LValue hashAndFlags = loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags).value();
    m_out.branch(m_out.aboveOrEqual(hashAndFlags, m_out.constInt32(1 << StringImpl::s_flagCount)), normalEdge(isHashed), unlikelyEdge(byStub));

    m_out.appendTo(isHashed);
    LValue tag = m_out.zeroExtPtr(m_out.lShr(hashAndFlags, m_out.constInt32(StringImpl::s_flagCount + CompactTable::tagShiftOfTags)));
    LValue difference = m_out.bitXor(m_out.mul(tag, m_out.constInt64(ones)), tags);
    LValue matches = m_out.bitAnd(m_out.bitAnd(m_out.sub(difference, m_out.constInt64(ones)), m_out.bitNot(difference)), m_out.constInt64(ones << 7));
    m_out.branch(m_out.isZero64(matches), normalEdge(absent), normalEdge(hasMatch));

    m_out.appendTo(hasMatch);
    LValue firstMatch = countTrailingZerosOfNonZero(matches);
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
        bool expectsNumbers = typeOf(valueRead).primitives & Primitive::Number;
        m_out.branch(valuesAreNarrow, expectsNumbers ? unlikelyEdge(isNarrow) : normalEdge(isNarrow), expectsNumbers ? normalEdge(isWide) : unlikelyEdge(isWide));

        m_out.appendTo(isNarrow);
        NarrowReference narrow(m_out.load32(inColumns(m_out.add(values, offsetInNarrowColumn), offsetOfKeys - 3)));
        join.add(select(isCellReferenceAmongAllWords(narrow), boxCell(decode(narrow)), Boxed(m_out.zeroExtPtr(narrow.value()))));

        m_out.appendTo(isWide);
        join.add(Boxed(m_out.load64(inColumns(m_out.add(values, firstMatch), offsetOfKeys - 7))));
    }

    m_out.appendTo(absent);
    join.add(boxedConstant(answerWhenAbsent(answer)));

    m_out.appendTo(byStub);
    join.add(Boxed(callStub(call.context(), stub, { owner.value(), key.value() }).first));
    return join.finish();
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
            if (value.opcode == Opcode::MoveTableToHeap || (value.opcode == Opcode::CallInline && (value.inlineBuiltin() == InlineBuiltin::MapGet || value.inlineBuiltin() == InlineBuiltin::MapHas || value.inlineBuiltin() == InlineBuiltin::SetHas)))
                continue;
            if (writesTables(functionAnalyses.effectsOf(id).onAnyEdge()))
                return false;
        }
        if (writesTables(functionAnalyses.effectsOfTerminator(block).onAnyEdge()))
            return false;
    }
    return true;
}

static bool mayHoldMoreNamesThanTags(const Program& program, const Type& collection)
{
    return std::ranges::any_of(collection.classes, [&](const ClassPart& part) {
        ContentsSlot keys = part.builtinClass == BuiltinClasses::Map ? ContentsSlot::Key : ContentsSlot::Element;
        return program.at(part.expectedContentsAt(program.builtins(), keys).predicted()).stringLiterals.size() > CompactTable::maxCapacityWithTags;
    });
}

bool HashTableBuiltinEmitter::looksAmongTags(const InlineCall& call)
{
    // A larger table pays for the header that is read here and again in the stub. Keys that are written in the program are few, and mostly so are the entries of the table they are looked up in.
    return typeOf(call.argument(0)).hasOnlyPrimitives(Primitive::LiteralString) && !mayHoldMoreNamesThanTags(m_program, typeOf(call.receiver())) && hasReceiverThatLeavesLoop(call);
}

bool HashTableBuiltinEmitter::looksIntoHomeSlot(const InlineCall& call, KeyComparison comparison)
{
    ValueID key = call.argument(0);
    const Type& type = typeOf(key);
    bool isKey = comparison == KeyComparison::String ? !makersOfStrings.contains(m_function.at(key).opcode) : comparison == KeyComparison::Bits && (type.isNumber() || type.withoutNullish().isObject());
    return isKey && hasReceiverThatLeavesLoop(call);
}

// Reads the table once, out of the loop where B3 can move it. A table in the image, one that is too large, a string without a hash and a text that must be compared by its characters go to the stub.
Boxed HashTableBuiltinEmitter::lookUpInHomeSlot(const InlineCall& call, CellPointer owner, Boxed key, KeyComparison comparison, HashTableOwner kindOfOwner, LookupAnswer answer, ValueID valueRead)
{
    const bool returnsValue = answer != LookupAnswer::IsPresent;
    constexpr ptrdiff_t index = CompactTable::offsetOfIndex();
    constexpr uint64_t imageFlag = static_cast<uint64_t>(CompactTable::keysAreInImage) << 40;
    constexpr uint64_t wideFlag = static_cast<uint64_t>(CompactTable::keysAreWide) << 40;
    constexpr uint64_t indexBitsMask = 0xffull << 32;
    constexpr uint64_t ones = 0x0101010101010101;
    // A number never fits a narrow key, and a table of objects has narrow keys: what is looked for says which of the two widths is searched here.
    const bool comparesTexts = comparison == KeyComparison::String;
    const bool keysAreWide = !comparesTexts && typeOf(call.argument(0)).isNumber();
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
    LBasicBlock allEightAreTaken = newBlock();
    LBasicBlock goesOn = newBlock();
    LBasicBlock searchesNextEight = newBlock();
    LBasicBlock matchAmongNextEight = newBlock();
    LBasicBlock noMatchAmongNextEight = newBlock();
    LBasicBlock goesOnToNextEight = newBlock();
    LBasicBlock isOtherWord = comparesTexts ? newBlock() : nullptr;
    LBasicBlock isOtherCell = comparesTexts ? newBlock() : nullptr;
    LBasicBlock isOtherString = comparesTexts ? newBlock() : nullptr;
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
    // Slots are counted from the end: the last is -1. The tags end where the numbers begin and the numbers where the keys begin, so two addresses reach all three, and one word makes a slot of a hash and wraps around.
    LValue bitsAboveSlots = m_out.neg(slots);
    LValue hasLongSlots = m_out.above(capacity, m_out.constInt64(CompactTable::maxCapacityWithShortSlots));
    LValue start = m_out.add(table, m_out.constIntPtr(index));
    LValue numbers = m_out.add(start, slots);
    LValue hasNoSlots = m_out.testIsZero64(kinds, m_out.constInt64(indexBitsMask));
    LValue keys = nullptr;
    // A map of number keys is rarely small enough for tags: their reads stay out of the way of the slots' registers, and go by the address of the keys, which lie behind the eight tags.
    constexpr ptrdiff_t tableFromKeysBehindTags = -(index + static_cast<ptrdiff_t>(CompactTable::maxCapacityWithTags));
    auto loadWordOfTags = [&] { return m_out.load64(inColumns(keysAreWide ? m_out.add(keys, m_out.constIntPtr(tableFromKeysBehindTags + index)) : start)); };
    auto loadBitsOfUsedTags = [&] { return m_out.shl(m_out.zeroExtPtr(m_out.load32(keysAreWide ? inHeader(keys, tableFromKeysBehindTags + CompactTable::offsetOfUsed()) : inHeader(table, CompactTable::offsetOfUsed()))), m_out.constInt32(3)); };
    LValue wordOfTags = keysAreWide ? nullptr : loadWordOfTags();
    LValue bitsOfUsedTags = keysAreWide ? nullptr : loadBitsOfUsedTags();
    LValue valuesAreNarrow = returnsValue ? m_out.equal(m_out.bitAnd(m_out.lShr(kinds, m_out.constInt32(48)), m_out.constInt64(0xff)), m_out.constInt64(static_cast<int64_t>(CompactTable::Width::Narrow))) : nullptr;
    LValue isOfKind = m_out.equal(m_out.bitAnd(kinds, m_out.constInt64(imageFlag | wideFlag)), m_out.constInt64(keysAreWide ? wideFlag : 0));
    LValue isNotHuge = m_out.belowOrEqual(capacity, m_out.constInt64(CompactTable::maxCapacityWithLongSlots));
    // One word says how to search. It takes one register in the loop, where the tests it stands for would take three.
    // Number keys rarely meet a table small enough for tags, so one test sends them to the slots.
    const int32_t Tags = keysAreWide ? 2 : 0;
    const int32_t ShortSlots = keysAreWide ? 0 : 1;
    const int32_t LongSlots = keysAreWide ? 1 : 2;
    const int32_t OtherTable = 3;
    LValue searchableForm = m_out.select(hasNoSlots, m_out.constInt32(Tags), m_out.select(hasLongSlots, m_out.constInt32(LongSlots), m_out.constInt32(ShortSlots)));
    LValue form = m_out.select(m_out.bitAnd(isOfKind, isNotHuge), searchableForm, m_out.constInt32(OtherTable));
    // For string keys one word stands for the form in the loop. It holds the bits that no cell has. For a table that this code cannot search it holds all bits, and no key is zero:
    // so one test sends to the stub a key that is no cell and every key for such a table. Three more of the bits that no cell has say what the table is like.
    // A table has eight tags or slots, so one word is the tags or where the numbers of the slots begin, and another the number of tags in use or the bits above a slot.
    constexpr int64_t slotsAreLong = 1;
    constexpr int64_t narrowValues = 4;
    constexpr int64_t noSlots = 8;
    static_assert(!(JSValue::NotCellMask & (slotsAreLong | narrowValues | noSlots)) && MarkedBlock::atomSize > (slotsAreLong | narrowValues | noSlots));
    LValue bitsOfNoCell = nullptr;
    if (!keysAreWide) {
        auto bitIf = [&](LValue condition, int64_t bit) { return m_out.select(condition, m_out.constInt64(bit), m_out.constInt64(0)); };
        LValue bits = m_out.bitOr(m_out.bitOr(m_out.constInt64(JSValue::NotCellMask), bitIf(hasLongSlots, slotsAreLong)), bitIf(hasNoSlots, noSlots));
        if (returnsValue)
            bits = m_out.bitOr(bits, bitIf(valuesAreNarrow, narrowValues));
        bitsOfNoCell = m_out.select(m_out.equal(form, m_out.constInt32(OtherTable)), m_out.constInt64(-1), bits);
        wordOfTags = numbers = m_out.select(hasNoSlots, wordOfTags, numbers);
        bitsOfUsedTags = bitsAboveSlots = m_out.select(hasNoSlots, bitsOfUsedTags, bitsAboveSlots);
    }
    // The addresses that the loop reads through come last. Ahead of the words above, they would be alive while those are worked out, where more values are alive than there are registers, and would stay on the stack in the loop too.
    keys = m_out.select(hasNoSlots, m_out.add(start, m_out.constIntPtr(CompactTable::maxCapacityWithTags)), m_out.add(start, m_out.mul(slots, m_out.select(hasLongSlots, m_out.constInt64(4), m_out.constInt64(2)))));
    LValue values = returnsValue ? m_out.add(keys, m_out.shl(capacity, m_out.constInt32(keysAreWide ? 3 : 2))) : nullptr;
    m_out.jump(isOfKindAndSize);

    m_out.appendTo(isOfKindAndSize);
    LValue hash;
    if (keysAreWide) {
        // What lies above the bits of the hash stays: the mask of the slots and the tests of the tags leave it out.
        hash = hashOfBitsByOneMultiplication(key).value();
        m_out.jump(hasHash);
    } else if (!comparesTexts) {
        build().statistics().count(StatisticID::ObjectKeysSearchedInLine);
        LBasicBlock isCell = newBlock();
        m_out.branch(m_out.testIsZero64(key.value(), bitsOfNoCell), normalEdge(isCell), unlikelyEdge(byStub));
        m_out.appendTo(isCell);
        hash = hashOfBitsByOneMultiplication(key).value();
        m_out.jump(hasHash);
    } else {
        LBasicBlock isCell = newBlock();
        LBasicBlock isFlat = newBlock();
        LBasicBlock isHashed = newBlock();
        m_out.branch(m_out.testIsZero64(key.value(), bitsOfNoCell), normalEdge(isCell), unlikelyEdge(byStub));
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

    auto lowByte = [&](LValue word) { return comparesTexts ? word : m_out.bitAnd(word, m_out.constInt32(0xff)); };
    auto loadNumber = [&](LValue slot) {
        LBasicBlock isLong = newBlock();
        LBasicBlock isShort = newBlock();
        LBasicBlock loaded = newBlock();
        m_out.branch(keysAreWide ? m_out.equal(form, m_out.constInt32(LongSlots)) : m_out.testNonZero64(bitsOfNoCell, m_out.constInt64(slotsAreLong)), unlikelyEdge(isLong), normalEdge(isShort));
        m_out.appendTo(isLong);
        // The byte ahead of the first number is the last tag.
        LValue threeBytes = m_out.lShr(m_out.load32(inColumns(m_out.add(m_out.add(keys, m_out.mul(slot, m_out.constInt64(3))), m_out.constIntPtr(-1)))), m_out.constInt32(8));
        ValueFromBlock fromLong = m_out.anchor(threeBytes);
        m_out.jump(loaded);
        m_out.appendTo(isShort);
        ValueFromBlock fromShort = m_out.anchor(m_out.load8ZeroExt32(inColumns(m_out.add(keys, slot))));
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
    // Two string cells may hold one text: a name out of Object.keys() and the same name as the program writes it. One place compares what the two cells hold, for every match.
    Vector<ValueFromBlock, 4> numbersOfOtherWords;
    // One object in 64 has the tag of another: the stub goes on from there.
    auto branchOnStringKeyAt = [&](LValue number) {
        if (!comparesTexts) {
            m_out.branch(keyIsAt(number), normalEdge(found), unlikelyEdge(build().isSabotaged(Sabotage::ObjectsWithTheTagOfAnotherAreAbsent) ? absent : byStub));
            return;
        }
        numbersOfOtherWords.append(m_out.anchor(number));
        m_out.branch(keyIsAt(number), normalEdge(found), unlikelyEdge(isOtherWord));
    };

    m_out.appendTo(hasHash);
    if (keysAreWide) {
        LBasicBlock noSlots = newBlock();
        m_out.branch(m_out.below(form, m_out.constInt32(Tags)), normalEdge(hasSlots), unlikelyEdge(noSlots));
        m_out.appendTo(noSlots);
        m_out.branch(m_out.equal(form, m_out.constInt32(Tags)), normalEdge(hasTags), unlikelyEdge(byStub));
    } else
        m_out.branch(m_out.testNonZero64(bitsOfNoCell, m_out.constInt64(noSlots)), normalEdge(hasTags), normalEdge(hasSlots));

    m_out.appendTo(hasTags);
    if (keysAreWide)
        wordOfTags = loadWordOfTags();
    LValue matchesAmongTags = tagsNearTag(wordOfTags, m_out.zeroExtPtr(lowByte(m_out.lShr(hash, m_out.constInt32(CompactTable::tagShiftOfTags)))), 1);
    m_out.branch(m_out.isZero64(matchesAmongTags), normalEdge(absent), normalEdge(matchAmongTags));

    m_out.appendTo(matchAmongTags);
    LValue bitOfMatch = countTrailingZerosOfNonZero(matchesAmongTags);
    LBasicBlock isUsed = newBlock();
    if (keysAreWide)
        bitsOfUsedTags = loadBitsOfUsedTags();
    m_out.branch(m_out.below(bitOfMatch, bitsOfUsedTags), normalEdge(isUsed), unlikelyEdge(absent));

    m_out.appendTo(isUsed);
    LValue numberOfMatch = m_out.lShr(bitOfMatch, m_out.constInt32(3));
    ValueFromBlock numberAmongTags = m_out.anchor(numberOfMatch);
    if (keysAreWide)
        m_out.branch(keyIsAt(numberOfMatch), normalEdge(found), unlikelyEdge(byStub));
    else
        branchOnStringKeyAt(numberOfMatch);

    m_out.appendTo(hasSlots);
    LValue slot = m_out.bitOr(m_out.zeroExtPtr(hash), bitsAboveSlots);
    // The lowest bit of the tag in a slot says that a key has left the slot. The tag that is looked for has it set, so the difference has it where no key has left.
    LValue tag = m_out.bitOr(m_out.lShr(hash, m_out.constInt32(CompactTable::tagShiftOfSlots - 1)), m_out.constInt32(CompactTable::takenSlot | CompactTable::keyLeftHomeSlot));
    LValue differenceAtHome = m_out.bitXor(tag, m_out.load8ZeroExt32(inColumns(m_out.add(numbers, slot))));
    m_out.branch(m_out.testIsZero32(differenceAtHome, m_out.constInt32(0xff & ~CompactTable::keyLeftHomeSlot)), normalEdge(atHome), unlikelyEdge(notAtHome));

    auto firstOfEight = [&](LValue slot) { return m_out.bitAnd(slot, m_out.constInt64(~static_cast<int64_t>(CompactTable::slotsPerGroup - 1))); };
    auto loadEightTags = [&](LValue first) { return m_out.load64(inColumns(m_out.add(numbers, first))); };
    auto matchesIn = [&](LValue eightTags, LValue tag) { return tagsNearTag(eightTags, tag, CompactTable::keyLeftHomeSlot + 1); };
    // Where the key in the home slot is another one, the tag in that slot is read again, and it is the tag to go on with. Kept from the first read it would hold a register, or cost a store, on the way of every key.
    auto loadTagAtHomeAgain = [&] { return m_out.load8ZeroExt32(inColumns(m_out.add(numbers, m_out.opaque(slot)))); };
    auto slotOfFirstMatch = [&](LValue first, LValue matches) { return m_out.add(first, m_out.lShr(countTrailingZerosOfNonZero(matches), m_out.constInt32(3))); };
    // A taken slot has the top bit of its tag set.
    auto hasFreeSlot = [&](LValue eightTags) { return m_out.notEqual(m_out.bitAnd(eightTags, m_out.constInt64(ones << 7)), m_out.constInt64(ones << 7)); };
    auto nextEight = [&](LValue first) { return m_out.bitOr(m_out.add(first, m_out.constInt64(CompactTable::slotsPerGroup)), bitsAboveSlots); };
    Vector<ValueFromBlock, 3> whereToGoOn;
    Vector<ValueFromBlock, 3> tagsToGoOnWith;
    // One key in 64 has the tag of another. A number is then another key, and the search goes on. Two texts with one tag are left to the stub.
    auto branchOnKeyAt = [&](LValue number, LValue tagBehindHome, NOESCAPE const Invocable<LValue()> auto& firstToGoOnFrom) {
        if (!keysAreWide) {
            branchOnStringKeyAt(number);
            return;
        }
        LBasicBlock isOtherKey = newBlock();
        m_out.branch(keyIsAt(number), normalEdge(found), unlikelyEdge(isOtherKey));
        m_out.appendTo(isOtherKey);
        if (!tagBehindHome) {
            LBasicBlock someKeyHasLeft = newBlock();
            LValue tagAtHome = loadTagAtHomeAgain();
            m_out.branch(m_out.testIsZero32(tagAtHome, m_out.constInt32(CompactTable::keyLeftHomeSlot)), normalEdge(absent), normalEdge(someKeyHasLeft));
            m_out.appendTo(someKeyHasLeft);
            tagBehindHome = m_out.zeroExtPtr(tagAtHome);
        }
        whereToGoOn.append(m_out.anchor(firstToGoOnFrom()));
        tagsToGoOnWith.append(m_out.anchor(tagBehindHome));
        m_out.jump(goesOn);
    };

    m_out.appendTo(atHome);
    LValue numberAtHome = loadNumber(slot);
    ValueFromBlock numberOfHome = m_out.anchor(numberAtHome);
    branchOnKeyAt(numberAtHome, nullptr, [&] { return firstOfEight(slot); });

    m_out.appendTo(notAtHome);
    m_out.branch(m_out.testNonZero32(differenceAtHome, m_out.constInt32(CompactTable::keyLeftHomeSlot)), normalEdge(absent), normalEdge(searchesEight));

    // The eight tags that the home slot lies among, for one key in four that the table has, and for one in ten to one in five that it lacks.
    m_out.appendTo(searchesEight);
    LValue first = firstOfEight(slot);
    LValue eightTags = loadEightTags(first);
    LValue tagBehindHome = m_out.zeroExtPtr(lowByte(tag));
    LValue matches = matchesIn(eightTags, tagBehindHome);
    m_out.branch(m_out.isZero64(matches), normalEdge(noMatchAmongEight), normalEdge(matchAmongEight));

    m_out.appendTo(matchAmongEight);
    LValue numberAmongEight = loadNumber(slotOfFirstMatch(first, matches));
    ValueFromBlock numberOfEight = m_out.anchor(numberAmongEight);
    branchOnKeyAt(numberAmongEight, tagBehindHome, [&] { return first; });

    m_out.appendTo(noMatchAmongEight);
    m_out.branch(hasFreeSlot(eightTags), normalEdge(absent), unlikelyEdge(allEightAreTaken));

    m_out.appendTo(allEightAreTaken);
    whereToGoOn.append(m_out.anchor(nextEight(first)));
    tagsToGoOnWith.append(m_out.anchor(tagBehindHome));
    m_out.jump(goesOn);

    // Every match, and every eight up to the first free slot, as CompactTable::nextSlotWithTag() goes: a table that is nearly full has many eights without a free slot.
    // Among eight slots that are all taken none is ever freed, so a key lies ahead of the first eight with a free slot, and an index always has one.
    m_out.appendTo(goesOn);
    ValueFromBlock firstToGoOnWith = m_out.anchor(m_out.phi(B3::Int64, whereToGoOn));
    LValue tagToGoOnWith = m_out.phi(B3::Int64, tagsToGoOnWith);
    // B3 makes a version of the loop around the lookup for number keys in short slots. It cannot take a loop into that version, and would leave it for good here.
    if (keysAreWide)
        m_out.branch(m_out.equal(form, m_out.constInt32(LongSlots)), normalEdge(searchesNextEight), normalEdge(byStub));
    else
        m_out.jump(searchesNextEight);

    m_out.appendTo(searchesNextEight);
    LValue firstOfNext = m_out.phi(B3::Int64, firstToGoOnWith);
    LValue nextEightTags = loadEightTags(firstOfNext);
    LValue matchesOfNext = matchesIn(nextEightTags, tagToGoOnWith);
    ValueFromBlock allMatchesOfNext = m_out.anchor(matchesOfNext);
    m_out.branch(m_out.isZero64(matchesOfNext), normalEdge(noMatchAmongNextEight), normalEdge(matchAmongNextEight));

    m_out.appendTo(matchAmongNextEight);
    LValue matchesLeft = m_out.phi(B3::Int64, allMatchesOfNext);
    LValue numberAmongNextEight = loadNumber(slotOfFirstMatch(firstOfNext, matchesLeft));
    ValueFromBlock numberOfNextEight = m_out.anchor(numberAmongNextEight);
    if (!keysAreWide)
        branchOnStringKeyAt(numberAmongNextEight);
    else {
        LBasicBlock isOtherKey = newBlock();
        m_out.branch(keyIsAt(numberAmongNextEight), normalEdge(found), unlikelyEdge(isOtherKey));

        m_out.appendTo(isOtherKey);
        LValue matchesBehind = m_out.bitAnd(matchesLeft, m_out.sub(matchesLeft, m_out.constInt64(1)));
        m_out.addIncomingToPhi(matchesLeft, m_out.anchor(matchesBehind));
        m_out.branch(m_out.isZero64(matchesBehind), normalEdge(noMatchAmongNextEight), unlikelyEdge(matchAmongNextEight));
    }

    m_out.appendTo(noMatchAmongNextEight);
    m_out.branch(hasFreeSlot(nextEightTags), normalEdge(absent), unlikelyEdge(goesOnToNextEight));

    m_out.appendTo(goesOnToNextEight);
    m_out.addIncomingToPhi(firstOfNext, m_out.anchor(nextEight(firstOfNext)));
    m_out.jump(searchesNextEight);

    Vector<ValueFromBlock, 5> numbersFound { numberAmongTags, numberOfHome, numberOfEight, numberOfNextEight };
    if (comparesTexts) {
        m_out.appendTo(isOtherWord);
        LValue numberOfOtherWord = m_out.phi(B3::Int64, numbersOfOtherWords);
        NarrowReference word(m_out.load32(inColumns(m_out.add(keys, m_out.shl(numberOfOtherWord, m_out.constInt32(2))))));
        m_out.branch(isCellReferenceAmongAllWords(word).value(), normalEdge(isOtherCell), unlikelyEdge(byStub));

        m_out.appendTo(isOtherCell);
        CellPointer candidate = decode(word);
        m_out.branch(m_out.equal(m_out.load8ZeroExt32(candidate.value(), m_heaps.JSCell_typeInfoType), m_out.constInt32(StringType)), normalEdge(isOtherString), unlikelyEdge(byStub));

        // What the key holds is read again. Kept from the first read it would hold a register through the whole search.
        m_out.appendTo(isOtherString);
        numbersFound.append(m_out.anchor(numberOfOtherWord));
        Pointer implOfKey = loadField<Pointer>(unboxCell(Boxed(m_out.opaque(key.value()))), m_heaps.JSString_value);
        m_out.branch(m_out.equal(loadField<Pointer>(candidate, m_heaps.JSString_value).value(), implOfKey.value()), normalEdge(found), unlikelyEdge(byStub));
    }

    m_out.appendTo(found);
    LValue number = m_out.phi(B3::Int64, numbersFound);
    if (!returnsValue)
        join.add(boxedConstant(jsBoolean(true)));
    else {
        LBasicBlock isNarrow = newBlock();
        LBasicBlock isWide = newBlock();
        // A narrow column holds no number.
        bool expectsNumbers = typeOf(valueRead).primitives & Primitive::Number;
        m_out.branch(keysAreWide ? valuesAreNarrow : m_out.testNonZero64(bitsOfNoCell, m_out.constInt64(narrowValues)), expectsNumbers ? unlikelyEdge(isNarrow) : normalEdge(isNarrow), expectsNumbers ? normalEdge(isWide) : unlikelyEdge(isWide));

        m_out.appendTo(isNarrow);
        NarrowReference narrow(m_out.load32(inColumns(m_out.add(values, m_out.shl(number, m_out.constInt32(2))))));
        join.add(select(isCellReferenceAmongAllWords(narrow), boxCell(decode(narrow)), Boxed(m_out.zeroExtPtr(narrow.value()))));

        m_out.appendTo(isWide);
        join.add(Boxed(m_out.load64(inColumns(m_out.add(values, m_out.shl(number, m_out.constInt32(3)))))));
    }

    m_out.appendTo(absent);
    join.add(boxedConstant(answerWhenAbsent(answer)));

    m_out.appendTo(byStub);
    join.add(Boxed(callStub(call.context(), lookupStubFor(kindOfOwner, answer, comparison), { owner.value(), key.value() }).first));
    return join.finish();
}

// The lookups read the table through the reference that is returned, so B3 reads nothing of it ahead of the move.
CellPointer HashTableBuiltinEmitter::moveTableToHeap(const OperationContext& context, CellPointer owner)
{
    auto inHeader = [&](LValue base, ptrdiff_t offset) { return m_out.address(m_heaps.entire(AbstractHeap::CollectionStorage), base, offset); };
    LBasicBlock hasTable = newBlock();
    LBasicBlock isOnHeap = newBlock();
    LBasicBlock isInImage = newBlock();
    Join<CellPointer> join(m_out);

    LValue table = m_out.loadPtr(inHeader(owner.value(), JSMap::offsetOfStorage()));
    m_out.branch(m_out.notNull(table), normalEdge(hasTable), normalEdge(isOnHeap));

    m_out.appendTo(hasTable);
    LValue flags = m_out.load8ZeroExt32(inHeader(table, CompactTable::offsetOfKeysShift()));
    m_out.branch(m_out.testNonZero32(flags, m_out.constInt32(CompactTable::keysAreInImage)), rareEdge(isInImage), normalEdge(isOnHeap));

    m_out.appendTo(isOnHeap);
    join.add(owner);

    m_out.appendTo(isInImage);
    emitter<RuntimeCallEmitter>().setCallsOnlyMoveTables(true);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::MoveTableToHeap, { owner }, { .isOnRarePath = true }));
    emitter<RuntimeCallEmitter>().setCallsOnlyMoveTables(false);
    return join.finish();
}

// `map.has(key) ? map.get(key) : ..`: the get that control comes to from the edge on which the key is present, where nothing in between writes to a table.
ValueID HashTableBuiltinEmitter::findGetBehindHas(const InlineCall& has, InlineBuiltin get)
{
    constexpr uint32_t isPresentTargetIndex = 0;
    if (!has.value())
        return { };
    auto behindCasts = [&](ValueID id) {
        while (resultRuleOf(m_function.at(id).opcode) == ResultRule::Cast)
            id = m_function.at(id).valueOperand();
        return id;
    };
    auto isLookup = [&](const Value& value) {
        return value.opcode == Opcode::MoveTableToHeap || (value.opcode == Opcode::CallInline && (value.inlineBuiltin() == InlineBuiltin::MapGet || value.inlineBuiltin() == InlineBuiltin::MapHas || value.inlineBuiltin() == InlineBuiltin::SetHas));
    };
    auto writesTables = [&](const Effects& effects) {
        if (build().isSabotaged(Sabotage::GetsBehindHasOutlastWrites)) [[unlikely]]
            return false;
        return effects.writes.isTop() || effects.writes.contains(AbstractHeap::CollectionStorage) || effects.writes.contains(AbstractHeap::CollectionEntries);
    };
    ValueID receiver = behindCasts(has.receiver());
    ValueID key = behindCasts(has.argument(0));
    BlockID block = has.context().block();
    const Terminator& branch = m_function.at(block).terminator;
    if (!branch.isConditionalBranch() || branch.conditionOperand() != has.value()->id())
        return { };

    const BlockValues& valuesBesideHas = m_function.at(block).values;
    size_t next = valuesBesideHas.find(has.value()->id());
    RELEASE_ASSERT(next != notFound);
    ++next;
    while (true) {
        const BlockValues& values = m_function.at(block).values;
        for (; next < values.size(); ++next) {
            const Value& value = m_function.at(values[next]);
            if (value.opcode == Opcode::CallInline && value.inlineBuiltin() == get && value.receiverOperand() && behindCasts(value.receiverOperand()) == receiver && value.argumentsOperands().size() == 1 && behindCasts(value.argumentsOperands()[0]) == key)
                return block == has.context().block() ? ValueID() : values[next];
            if (!isLookup(value) && writesTables(analyses().effectsOf(values[next]).onAnyEdge()))
                return { };
        }
        const Terminator& terminator = m_function.at(block).terminator;
        bool isBranchOnHas = block == has.context().block();
        if ((!isBranchOnHas && terminator.kind != TerminatorKind::Jump) || writesTables(analyses().effectsOfTerminator(block).onAnyEdge()))
            return { };
        block = terminator.targets[isBranchOnHas ? isPresentTargetIndex : 0].block;
        // Both edges of a branch may lead to one block.
        if (analyses().cfg().incomingEdges(block).size() != 1)
            return { };
        next = 0;
    }
}

void HashTableBuiltinEmitter::didFindValueFor(ValueID get, Boxed valueOrNothing)
{
    m_valuesFoundByHas.append({ get, valueOrNothing.value() });
    build().statistics().count(StatisticID::GetsAnsweredByHas);
}

std::optional<Boxed> HashTableBuiltinEmitter::valueFoundFor(ValueID get)
{
    size_t found = m_valuesFoundByHas.findIf([&](const auto& entry) { return entry.first == get; });
    if (found == notFound)
        return std::nullopt;
    return Boxed(m_valuesFoundByHas[found].second);
}

void HashTableBuiltinEmitter::lookUp(const InlineCall& call, HashTableOwner owner, LookupAnswer answer)
{
    RELEASE_ASSERT(owner == HashTableOwner::Map || answer == LookupAnswer::IsPresent);
    ValueID valueRead = call.context().valueID();
    if (std::optional<Boxed> found = answer == LookupAnswer::Value ? valueFoundFor(valueRead) : std::nullopt) {
        finish(call, *found);
        return;
    }
    ValueID getBehindHas = owner == HashTableOwner::Map && answer == LookupAnswer::IsPresent ? findGetBehindHas(call, InlineBuiltin::MapGet) : ValueID();
    if (getBehindHas) {
        answer = LookupAnswer::ValueOrNothing;
        valueRead = getBehindHas;
    }

    CellPointer table = cellOperand(call.receiver());
    KeyComparison comparison = keyComparisonOf(typeOf(call.argument(0)));
    Boxed key = normalizedKey(call.context(), call.argument(0));
    StubKey stub = lookupStubFor(owner, answer, comparison);
    build().statistics().count(StatisticID::InlineHashLookups);
    Boxed found = looksAmongTags(call) ? lookUpAmongTags(call, table, unboxCell(key), stub, answer, valueRead) : looksIntoHomeSlot(call, comparison) ? lookUpInHomeSlot(call, table, key, comparison, owner, answer, valueRead) : Boxed(callStub(call.context(), stub, { table.value(), key.value() }).first);
    emitter<RuntimeCallEmitter>().setCallsOnlyMoveTables(false);
    switch (answer) {
    case LookupAnswer::IsPresent:
        finish(call, unboxBoolean(found));
        return;
    case LookupAnswer::Value:
        finish(call, found);
        return;
    case LookupAnswer::ValueOrNothing:
        didFindValueFor(getBehindHas, found);
        finish(call, Condition(m_out.notZero64(found.value())));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    lookUp(call, HashTableOwner::Map, LookupAnswer::Value);
}

void HashTableBuiltinEmitter::generateMapHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Map, LookupAnswer::IsPresent);
}

void HashTableBuiltinEmitter::generateSetHas(const InlineCall& call)
{
    lookUp(call, HashTableOwner::Set, LookupAnswer::IsPresent);
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

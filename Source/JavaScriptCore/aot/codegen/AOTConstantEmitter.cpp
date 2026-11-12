/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConstantEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuiltinTable.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTNumbering.h"
#include "AOTOperationData.h"
#include "AOTOperatorEmitter.h"
#include "AOTPureOperations.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStringTable.h"
#include "AOTStubCalls.h"
#include "AOTStubCatalog.h"
#include "AOTStubsConstants.h"
#include "AOTTypeArena.h"
#include "AllowMacroScratchRegisterUsage.h"
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
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return true;
    case PropertyKeyKind::PrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ptrdiff_t offsetOfKeyCells(bool isNarrow)
{
    return isNarrow ? AOT_INSTANCE_OFFSET(runtime.constants.keyCells) : AOT_INSTANCE_OFFSET(runtime.constants.wideKeyCells);
}

static constexpr Scale scaleOfKeyCells(bool isNarrow)
{
    return isNarrow ? ScaleFour : ScaleEight;
}

static TypedPointer cellWordInConstantTable(Output& out, const Machine::AbstractHeap& heap, Pointer table, Word32 number, Scale sizeOfWord)
{
    return out.baseIndex(heap, table.value(), out.zeroExtPtr(number.value()), sizeOfWord);
}

static constexpr OpcodeSet constantsWithCells { Opcode::String, Opcode::KeyConstant, Opcode::Builtin };

static uint32_t mentionOfDataRequest(MentionList& mentions, DataID data, DataEncoding encoding)
{
    uint32_t words[] { data.index(), static_cast<uint32_t>(encoding) };
    return mentions.request(RequestKind::Data, words);
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
    if (value->isAtom() && !hasCellOfVM(types().bytesOf(string)))
        m_mentions.add(NumberSpace::AtomOfString, string);
    defineCell(value, stringConstant(value, string));
}

void ConstantEmitter::generateBuiltin(const ValueContext& value)
{
    BuiltinCellKey key = BuiltinCellKey::object(m_builtins.canonicalID(value->builtin()));
    bool endsChains = m_builtins.canonicalID(value->builtin()) == m_builtins.canonicalID(Builtins::ObjectPrototype);
    if (!endsChains && key.hasCell(m_builtins) && key.kind() != BuiltinCellKey::Kind::Value && formOfConstantsWithCellsAt(value).form == Form::Closed && emitter<OperatorEmitter>().leaveOperandToInstanceOf(value, InstanceOfOperand::PrototypeThatIsBuiltin, NumberedImmediate { m_mentions.builtinCell(key) })) {
        defineCell(value, CellPointer(m_out.constInt64(0)));
        return;
    }
    defineCell(value, builtinCell(value, key));
}

void ConstantEmitter::generateModuleBuiltin(const ValueContext& value)
{
    BuiltinID builtin = value->builtin();
    ModuleID module = value->module();
    auto callModuleBuiltinCell = [&](bool isOnRarePath) {
        return callOperation<CellPointer>(value, RuntimeOperation::ModuleBuiltinCell, { word32(static_cast<int32_t>(builtin.index())), word32(static_cast<int32_t>(module.index())) }, { .isOnRarePath = isOnRarePath });
    };
    if (m_program.at(module).flags.contains(ModuleFlags::MayBeReevaluated)) {
        defineCell(value, callModuleBuiltinCell(false));
        return;
    }
    uint32_t key[] { builtin.index(), module.index() };
    uint32_t mention = m_mentions.add(NumberSpace::ModuleBuiltinCell, std::span<const uint32_t>(key));
    defineCell(value, cellNumberedBehindJoin(pureFunction(PureOperation::ModuleBuiltinCell, mention), [&] {
        TypedPointer cellWord = instanceWord(m_heaps.AOTInstance_constantCells, mention);
        return permanentCell(cellWord, false, [&] {
            CellPointer cell = callModuleBuiltinCell(true);
            m_out.store64(cell.value(), cellWord);
            emitter<BarrierEmitter>().markCard(m_mentions.cardOfWord(NumberSpace::ModuleBuiltinCell, key));
            return cell;
        });
    }));
}

void ConstantEmitter::generateData(const ValueContext& value)
{
    switch (value->encoding()) {
    case DataEncoding::Unselected:
        failAndDefineZero(value, InternalErrorKind::PlacedPayloadMissing);
        return;
    case DataEncoding::ReadOnlyTree:
    case DataEncoding::CopyOnWriteArray:
    case DataEncoding::TreeWithOwnStorage:
        break;
    case DataEncoding::SharedObject:
        defineCell(value, sharedObjectOfData(value));
        return;
    }
    if (value->encoding() == DataEncoding::CopyOnWriteArray) {
        defineCell(value, generator().takesOpenForm(value, StateOfKnowledge::CopyOnWriteArray) ? newCopyOnWriteArray(value) : newCopyOnWriteArrayByStub(value));
        return;
    }
    DataID data = value->data();
    uint32_t request = mentionOfDataRequest(m_mentions, data, value->encoding());
    m_out.noteForm(Form::Runtime, StateOfKnowledge::ReadOnlyTree);
    defineCell(value, callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { numberOf(request) }));
}

CellPointer ConstantEmitter::sharedObjectOfData(const ValueContext& value)
{
    DataID data = value->data();
    uint32_t request = mentionOfDataRequest(m_mentions, data, DataEncoding::SharedObject);
    LBasicBlock isUnmade = newBlock();
    Join<CellPointer> objects(m_out);
    LValue object = m_out.load64(instanceWord(m_heaps.AOTInstance_constantCells, m_mentions.add(NumberSpace::DataStorage, data)));
    branchUnless(Condition(m_out.notNull(object)), isUnmade);
    objects.add(CellPointer(object));

    m_out.appendTo(isUnmade);
    objects.add(callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { numberOf(request) }, { .isOnRarePath = true }));
    return objects.finish();
}

CellPointer ConstantEmitter::newCopyOnWriteArray(const ValueContext& value)
{
    DataID data = value->data();
    const auto* elements = m_program.at(data).root.tryAs<DataNode::Array>();
    if (!elements)
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    uint32_t request = mentionOfDataRequest(m_mentions, data, DataEncoding::CopyOnWriteArray);
    IndexingType indexingType = arrayStorageFormOf(arrayStorageOfData(m_program, *elements)).indexingType;
    LBasicBlock outOfLine = newBlock();
    Join<CellPointer> arrays(m_out);
    Pointer firstElement = [&] {
        if (isStorageOfArray(*elements))
            return Pointer(m_out.add(loadImmutablePointer(AOT_INSTANCE_OFFSET(image.data)).value(), m_out.zeroExtPtr(numberOf(m_mentions.add(NumberSpace::DataElements, data)).value())));
        Pointer storage(m_out.load64(instanceWord(m_heaps.AOTInstance_constantCells, m_mentions.add(NumberSpace::DataStorage, data))));
        branchUnless(Condition(m_out.notNull(storage.value())), outOfLine);
        return offsetBy(storage, JSCellButterfly::offsetOfData());
    }();
    Integer header(m_out.load64(instanceField(m_heaps.entire(AbstractHeap::AllocatorState), Instance::offsetOfNewCopyOnWriteArrayHeader(indexingType))));
    branchUnless(isNotZero(header), outOfLine);
    arrays.add(ArrayAllocationEmitter(generator()).allocateArrayCell(header, firstElement, outOfLine));

    m_out.appendTo(outOfLine);
    arrays.add(callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { numberOf(request) }, { .isOnRarePath = true }));
    return arrays.finish();
}

CellPointer ConstantEmitter::newCopyOnWriteArrayByStub(const ValueContext& value)
{
    DataID data = value->data();
    const auto* elements = m_program.at(data).root.tryAs<DataNode::Array>();
    if (!elements)
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    Word32 request = numberOf(mentionOfDataRequest(m_mentions, data, DataEncoding::CopyOnWriteArray));
    ArrayElementKind kind = arrayElementKindOf(arrayStorageFormOf(arrayStorageOfData(m_program, *elements)).indexingType);
    if (isStorageOfArray(*elements))
        return CellPointer(callStub(value, AOT::newCopyOnWriteArray(kind, SharedElements::InImage), { request.value(), numberOf(m_mentions.add(NumberSpace::DataElements, data)).value() }).first);
    LValue storage = m_out.load64(instanceWord(m_heaps.AOTInstance_constantCells, m_mentions.add(NumberSpace::DataStorage, data)));
    return CellPointer(callStub(value, AOT::newCopyOnWriteArray(kind, SharedElements::InStorageOfSite), { request.value(), storage }).first);
}

void ConstantEmitter::generateKeyConstant(const ValueContext& value)
{
    PropertyKeyID key = value->key();
    defineCell(value, keyConstant(value, key));
}

TypedPointer ConstantEmitter::builtinCellWord(BuiltinCellKey key)
{
    return instanceWord(m_heaps.AOTInstance_fieldsFilledOnce, m_mentions.builtinCell(key));
}

const RegisterSet& ConstantEmitter::registersLostAtSiteOf(ConstantAtSite constant)
{
    std::optional<RegisterSet>& lost = m_registersLostAtSites[static_cast<size_t>(constant)];
    if (lost)
        return *lost;
    const StubCatalog& catalog = inputs().stubCatalog;
    lost = RegisterSet::macroClobberedGPRs();
    for (GPRReg result : allocatableGPRs) {
        RegisterSet clobbered = catalog.summaryOf(createConstantAtSite(constant).withOperandRegister(result)).clobbered;
        clobbered.remove(result);
        lost->merge(clobbered);
    }
    return *lost;
}

static void addHeapWrittenByMakerOfCell(Machine::Procedure& proc, Machine::PatchOperation* patch, const Machine::AbstractHeap& heap)
{
    if (patch->effects.writes.isTop() || heap.traits().contains(Machine::HeapTrait::Immutable))
        return;
    std::span<const Machine::HeapIndex> written = patch->effects.writes.heaps();
    std::span<Machine::HeapIndex> heaps = proc.arena().createSpan<Machine::HeapIndex>(written.size() + 1);
    std::ranges::copy(written, heaps.begin());
    heaps.back() = heap.index();
    patch->effects.writes = Machine::HeapSet::of(heaps);
}

CellPointer ConstantEmitter::makeConstantAtSite(const OperationContext* context, const ConstantMadeAtSite& request, const Machine::AbstractHeap& heapOfWord)
{
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    StubKey stub = createConstantAtSite(request.constant);
    Effects effects = inputs().stubCatalog.effectsOf(stub);
    if (context)
        effects = calls.effectsOfCall(*context, effects);
    effects.reads = { };
    RELEASE_ASSERT(!effects.mayExitSideways() && !effects.mayCollect);

    Machine::PatchOperation* patch = m_out.patchpoint(Machine::Int64);
    patch->clobberEarly(RegisterSet::macroClobberedGPRs());
    patch->clobberLate(registersLostAtSiteOf(request.constant));
    calls.applyEffects(patch, effects, ArgumentBufferAccess::None, false, request.runsCodeOfHost ? RuntimeCallEmitter::CellConversions::WhateverEffectsSay : RuntimeCallEmitter::CellConversions::None);
    addHeapWrittenByMakerOfCell(m_proc, patch, heapOfWord);
    patch->setGenerator([records = &m_records, stub, number = request.number](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg result = params[0].gpr();
        records->stubCalls.call(jit, stub.withOperandRegister(result), OptionalIndex { });
#if CPU(X86_64)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(ThrowSiteWord::opcodeInFrontOfWordBehindCall));
#endif
        records->relocations.emitNumber32(CodeSymbol { SymbolSpace::Number, number });
    });
    return CellPointer(patch);
}

ALWAYS_INLINE_INTO_OTHER_FILES FormAtPlace ConstantEmitter::formOfConstantsWithCellsAt(const OperationContext& context)
{
    constexpr StateOfKnowledge always = StateOfKnowledge::Always;
    static_assert(formByLoopAlone(Opcode::KeyConstant, always, true).form == formByLoopAlone(Opcode::String, always, true).form && formByLoopAlone(Opcode::KeyConstant, always, false).form == formByLoopAlone(Opcode::String, always, false).form);
    static_assert(formByLoopAlone(Opcode::Builtin, always, true).form == formByLoopAlone(Opcode::String, always, true).form && formByLoopAlone(Opcode::Builtin, always, false).form == formByLoopAlone(Opcode::String, always, false).form);
    constexpr FormAtPlace namedInLoops = formByLoopAlone(Opcode::String, always, true);
    constexpr FormAtPlace namedOutsideLoops = formByLoopAlone(Opcode::String, always, false);
    bool standsInLoop = generator().isInLoop(context.block());
    FormAtPlace named = standsInLoop ? namedInLoops : namedOutsideLoops;
    bool runsOnce = !standsInLoop && m_program.at(m_id).isModuleBody();
    bool standsInRareBlock = m_code.at(context.block()).frequency == BlockFrequency::Rare;
    bool standsInUnlikelyBlock = m_code.at(context.block()).frequency == BlockFrequency::Unlikely;
    if (runsOnce || standsInRareBlock || standsInUnlikelyBlock)
        return { Form::Closed, runsOnce ? Place::CodeThatRunsOnce : standsInRareBlock ? Place::RareBlock : Place::UnlikelyBlock };
    return named;
}

CellPointer ConstantEmitter::cellMadeByStub(const OperationContext& context, StubKey stub, const CallOptions& options, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord)
{
    FormAtPlace named = formOfConstantsWithCellsAt(context);
    m_out.noteFormOf(constantsWithCells, named.form, StateOfKnowledge::Always, named.place);
    if (named.form == Form::Closed)
        return CellPointer(callStub(context, stub, { }, options).first);
    ConstantAtSite constant = stub.family() == StubFamily::BuiltinCell ? ConstantAtSite::BuiltinCell : stub.form() == static_cast<uint8_t>(CreatedConstant::Key) ? ConstantAtSite::Key : ConstantAtSite::String;
    return numberedPermanentCell(*options.pureFunction, isNarrow, cellWord, [&](TypedPointer word) {
        return makeConstantAtSite(&context, { constant, options.numberedImmediate->mention, options.runsCodeOfHost }, *word.heap());
    });
}

bool ConstantEmitter::loadsStringConstantByStub(const OperationContext& context, StringID id)
{
    std::span<const uint8_t> text = types().bytesOf(id);
    return !text.empty() && !singleLatin1CharacterOf(text) && formOfConstantsWithCellsAt(context).form == Form::Closed;
}

bool ConstantEmitter::tryStoreStringConstantByStub(const OperationContext& context, ValueID string, CellPointer object, uint64_t offset, const TypedPointer& word)
{
    const Operation& constant = operationAt(string);
    if (constant.opcode != Opcode::String || m_code.useCountOf(string) != 1 || !isWordOfStoreStringConstant(offset) || !loadsStringConstantByStub(context, constant.string()))
        return false;
    StringID id = constant.string();
    if (constant.isAtom() && !hasCellOfVM(types().bytesOf(id)))
        m_mentions.add(NumberSpace::AtomOfString, id);
    const NumberedImmediate number { m_mentions.add(NumberSpace::StringCell, id) };
    FormAtPlace named = formOfConstantsWithCellsAt(context);
    m_out.noteFormOf(constantsWithCells, named.form, StateOfKnowledge::Always, named.place);

    const StubKey stub = storeStringConstant(offset);
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    const StubCatalog& catalog = inputs().stubCatalog;
    Effects effects = catalog.effectsOf(stub);
    RELEASE_ASSERT(!effects.mayExitSideways() && !effects.mayCollect);
    effects.reads = { };
    effects.writes = { };
    const GPRReg immediate = *catalog.summaryOf(stub).signature.immediate;
    RegisterSet clobbered = catalog.clobberedByRegisterVariantsOf(stub);
    clobbered.add(immediate, IgnoreVectors);
    clobbered.merge(RegisterSet::macroClobberedGPRs());
    RegisterSet clobberedEarly = RegisterSet::macroClobberedGPRs();
    clobberedEarly.add(immediate, IgnoreVectors);

    Machine::PatchOperation* patch = m_out.patchpoint(Machine::Void);
    patch->append(object.value(), Machine::ValueRep(Machine::ValueRep::SomeRegister));
    patch->clobberEarly(clobberedEarly);
    patch->clobberLate(clobbered);
    calls.applyEffects(patch, effects, ArgumentBufferAccess::None, false, RuntimeCallEmitter::CellConversions::None);
    addHeapWrittenByMakerOfCell(m_proc, patch, *word.heap());
    patch->setGenerator([records = &m_records, stub, number](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        records->stubCalls.call(jit, stub.withOperandRegister(params[0].gpr()), number, OptionalIndex { });
    });
    return true;
}

CellPointer ConstantEmitter::stringConstant(const OperationContext& context, StringID id)
{
    std::span<const uint8_t> text = types().bytesOf(id);
    if (text.empty())
        return emptyString();
    if (std::optional<uint8_t> character = singleLatin1CharacterOf(text)) {
        emitter<StringEmitter>().recordInlined(id);
        return singleCharacterString(word32(*character));
    }

    uint32_t mention = m_mentions.add(NumberSpace::StringCell, id);
    bool isNarrow = !m_options.recorded.useWideReferences();
    return cellMadeByStub(context, createConstant(CreatedConstant::String), { .numberedImmediate = NumberedImmediate { mention }, .pureFunction = pureFunction(PureOperation::StringConstant, id.index()) }, isNarrow, [&] {
        return isNarrow
            ? instanceWord(m_heaps.AOTInstance_stringCells, mention)
            : cellWordInConstantTable(m_out, m_heaps.AOTInstance_stringCells, loadImmutablePointer(AOT_INSTANCE_OFFSET(runtime.constants.wideStringCells)), numberOf(mention), ScaleEight);
    });
}

std::optional<CellPointer> ConstantEmitter::stringConstantIfMade(StringID id)
{
    std::span<const uint8_t> text = types().bytesOf(id);
    if (text.empty() || singleLatin1CharacterOf(text) || m_options.recorded.useWideReferences())
        return std::nullopt;
    LValue word = m_out.load32(instanceWord(m_heaps.AOTInstance_stringCells, m_mentions.add(NumberSpace::StringCell, id)));
    return decode(NarrowReference(word));
}

CellPointer ConstantEmitter::keyConstant(const OperationContext& context, PropertyKeyID key)
{
    if (liesInThread(key)) {
        const PropertyKey& threadLocalKey = types().at(key);
        RELEASE_ASSERT(threadLocalKey.kind == PropertyKeyKind::Text);
        return stringConstant(context, threadLocalKey.text);
    }
    if (!hasKeyCell(m_program.at(key).kind))
        return failWithoutCell(context, InternalErrorKind::OpcodeWithoutCode);

    uint32_t mention = m_mentions.add(NumberSpace::KeyCell, key);
    bool isNarrow = !m_options.recorded.useWideReferences();
    return cellMadeByStub(context, createConstant(CreatedConstant::Key), { .numberedImmediate = NumberedImmediate { mention }, .pureFunction = pureFunction(PureOperation::KeyConstant, key.index()) }, isNarrow, [&] {
        return cellWordInConstantTable(m_out, m_heaps.AOTInstance_stringCells, loadImmutablePointer(offsetOfKeyCells(isNarrow)), numberOf(mention), scaleOfKeyCells(isNarrow));
    });
}

CellPointer ConstantEmitter::bigIntConstant(const OperationContext& context, StringID digits)
{
    uint32_t mention = m_mentions.add(NumberSpace::BigInt, digits);
    Machine::PureFunction function = pureFunction(PureOperation::BigIntConstant, digits.index());
    return cellNumberedBehindJoin(function, [&] {
        TypedPointer cellWord = cellWordInConstantTable(m_out, m_heaps.AOTInstance_constantCells, loadImmutablePointer(AOT_INSTANCE_OFFSET(runtime.constants.bigIntCells)), numberOf(mention), ScaleEight);
        return permanentCell(cellWord, false, [&] {
            CallOptions options { .pureFunction = function, .isOnRarePath = true };
            return unboxCell(callOperation<Boxed>(context, RuntimeOperation::BigIntConstant, { numberOf(mention) }, options));
        });
    });
}

CellPointer ConstantEmitter::builtinCell(const OperationContext& context, BuiltinCellKey key)
{
    if (!key.hasCell(m_builtins))
        return failWithoutCell(context, InternalErrorKind::PlacedPayloadMissing);

    CallOptions options { .numberedImmediate = NumberedImmediate { m_mentions.builtinCell(key) }, .pureFunction = pureFunction(PureOperation::BuiltinCell, key.encoded()), .runsCodeOfHost = true };
    if (key.kind() == BuiltinCellKey::Kind::Value) {
        return permanentCell(builtinCellWord(key), false, [&] {
            return CellPointer(callStub(context, AOT::builtinCell(), { }, options).first);
        });
    }
    return cellMadeByStub(context, AOT::builtinCell(), options, false, [&] {
        return builtinCellWord(key);
    });
}

CellPointer ConstantEmitter::keyConstant(PropertyKeyID key)
{
    RELEASE_ASSERT(hasKeyCell(m_program.at(key).kind));
    uint32_t mention = m_mentions.add(NumberSpace::KeyCell, key);
    bool isNarrow = !m_options.recorded.useWideReferences();
    Machine::PureFunction function = pureFunction(PureOperation::KeyConstant, key.index());
    return numberedPermanentCell(function, isNarrow, [&] {
        return cellWordInConstantTable(m_out, m_heaps.AOTInstance_stringCells, loadImmutablePointer(offsetOfKeyCells(isNarrow)), numberOf(mention), scaleOfKeyCells(isNarrow));
    }, [&](TypedPointer cellWord) {
        return makeConstantAtSite(nullptr, { ConstantAtSite::Key, mention }, *cellWord.heap());
    });
}

CellPointer ConstantEmitter::builtinCell(BuiltinID id)
{
    BuiltinCellKey key = BuiltinCellKey::object(id);
    RELEASE_ASSERT(key.hasCell(m_builtins));
    Machine::PureFunction function = pureFunction(PureOperation::BuiltinCell, key.encoded());
    return numberedPermanentCell(function, false, [&] {
        return builtinCellWord(key);
    }, [&](TypedPointer cellWord) {
        return makeConstantAtSite(nullptr, { ConstantAtSite::BuiltinCell, m_mentions.builtinCell(key), true }, *cellWord.heap());
    });
}

void ConstantEmitter::branchOnIsBuiltinCell(LValue boxedValueOrCell, BuiltinID id, BranchTargets targets)
{
    build().statistics().count(StatisticID::NativeFunctionTestsEmitted);
    BuiltinCellKey key = BuiltinCellKey::object(id);
    RELEASE_ASSERT(key.hasCell(m_builtins));
    if (LValue known = m_out.findPurePatch(pureFunction(PureOperation::BuiltinCell, key.encoded()), Machine::Int64, { })) {
        m_out.branch(m_out.equal(boxedValueOrCell, known), targets.taken, targets.notTaken);
        return;
    }
    LBasicBlock differs = newBlock();
    LBasicBlock isNotMade = newBlock();
    LValue word = m_out.load64(builtinCellWord(key));
    m_out.branch(m_out.equal(boxedValueOrCell, word), targets.taken, BlockEdge(differs, targets.notTaken.edgeClass()));

    m_out.appendTo(differs);
    m_out.branch(m_out.notZero64(word), targets.notTaken, rareEdge(isNotMade));

    m_out.appendTo(isNotMade);
    CellPointer made = callWithoutSite<CellPointer>(RuntimeOperation::BuiltinCell, { numberOf(m_mentions.builtinCell(key)) }, { .pureFunction = pureFunction(PureOperation::BuiltinCell, key.encoded()), .isOnRarePath = true, .runsCodeOfHost = true });
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

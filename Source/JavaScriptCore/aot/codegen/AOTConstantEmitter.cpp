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
#include "AOTInstance.h"
#include "AOTNumbering.h"
#include "AOTOperationData.h"
#include "AOTPureOperations.h"
#include "AOTRuntimeCallEmitter.h"
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

// Of the tables of cells that are read here through a pointer in the instance, the narrow key cells alone hold narrow references.
static constexpr Scale scaleOfKeyCells(bool isNarrow)
{
    return isNarrow ? ScaleFour : ScaleEight;
}

// The table has no place in the instance that Image could name, so the number of the cell is scaled where the code runs.
static TypedPointer cellWordInConstantTable(Output& out, const Machine::AbstractHeap& heap, Pointer table, Word32 number, Scale sizeOfWord)
{
    return out.baseIndex(heap, table.value(), out.zeroExtPtr(number.value()), sizeOfWord);
}

// A cell that another operation makes for itself is a part of that one's form.
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
    // Whoever takes this copy of the constant looks the text up or compares it. The cells of the VM are atoms already.
    if (value->isAtom() && !hasCellOfVM(types().bytesOf(string)))
        m_mentions.add(NumberSpace::AtomOfString, string);
    if (value.representation() != Representation::LiteralString) {
        defineCell(value, stringConstant(value, string));
        return;
    }
    // The program may spell a text that the walk has made and have a word for it, so the walk keeps no such text as a word.
    if (liesInThread(string)) {
        failAndDefineZero(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    // Only a literal that something compares or stores as a word has a number. The word of any other is its string cell.
    if (literalStrings().indexOf(string))
        define(value, spelledLiteralString(string));
    else
        define(value, LiteralStringWord(stringConstant(value, string).value()));
}

// The word holds no number that Image gives. The mention says that the program makes a value of the string, so the image keeps its text: whoever turns the word into a cell reads it.
LiteralStringWord ConstantEmitter::spelledLiteralString(StringID string)
{
    m_mentions.add(NumberSpace::String, string);
    return emitter<ConversionEmitter>().literalStringConstant(string);
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
    // Each run of a module that runs again has a module object, a `require` and an `import.meta` of its own, so no word holds one for good.
    if (m_program.at(module).flags.contains(ModuleFlags::RunsAgain)) {
        defineCell(value, callModuleBuiltinCell(false));
        return;
    }
    uint32_t key[] { builtin.index(), module.index() };
    // The two ids fit no payload. A unit mentions a key once, so within the unit the mention names the cell.
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
    case DataEncoding::Expanded:
        failAndDefineZero(value, InternalErrorKind::PlacedPayloadMissing);
        return;
    case DataEncoding::ReadOnlyTree:
    case DataEncoding::CopyOnWriteArray:
    case DataEncoding::MadeOnce:
        break;
    }
    if (value->encoding() == DataEncoding::CopyOnWriteArray) {
        defineCell(value, generator().takesOpenForm(value, StateOfKnowledge::CopyOnWriteArray) ? newCopyOnWriteArray(value) : newCopyOnWriteArrayByStub(value));
        return;
    }
    DataID data = value->data();
    uint32_t request = mentionOfDataRequest(m_mentions, data, value->encoding());
    if (value->encoding() == DataEncoding::MadeOnce) {
        m_out.noteForm(Form::Open, StateOfKnowledge::MadeOnce);
        uint32_t key[] { data.index() };
        defineCell(value, cellNumberedBehindJoin(pureFunction(PureOperation::DataCell, data.index()), [&] {
            TypedPointer cellWord = instanceWord(m_heaps.AOTInstance_constantCells, m_mentions.add(NumberSpace::DataCell, data));
            return permanentCell(cellWord, false, [&] {
                CellPointer cell = callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { numberOf(request) }, { .isOnRarePath = true });
                mutatorFence();
                m_out.store64(cell.value(), cellWord);
                emitter<BarrierEmitter>().markCard(m_mentions.cardOfWord(NumberSpace::DataCell, key));
                return cell;
            });
        }));
        return;
    }
    m_out.noteForm(Form::Runtime, StateOfKnowledge::ReadOnlyTree);
    defineCell(value, callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { numberOf(request) }));
}

// Every array of the site is a cell around one storage. Storage without cells lies in the image. Any other is created by the first array of the site.
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
        // The data has a node because the call out of line mentions the request.
        if (isStorageOfArray(*elements))
            return Pointer(m_out.add(loadImmutablePointer(AOT_INSTANCE_OFFSET(image.data)).value(), m_out.zeroExtPtr(numberOf(m_mentions.add(NumberSpace::DataElements, data)).value())));
        Pointer storage(m_out.load64(instanceWord(m_heaps.AOTInstance_constantCells, m_mentions.add(NumberSpace::DataStorage, data))));
        branchUnless(Condition(m_out.notNull(storage.value())), outOfLine);
        return offsetBy(storage, JSCellButterfly::offsetOfData());
    }();
    // The header of a new cell is state of the allocator, which is all that Data declares. It is zero in a realm that is having a bad time.
    Integer header(m_out.load64(instanceField(m_heaps.entire(AbstractHeap::AllocatorState), Instance::offsetOfNewCopyOnWriteArrayHeader(indexingType))));
    branchUnless(isNotZero(header), outOfLine);
    arrays.add(ArrayAllocationEmitter(generator()).allocateArrayCell(header, firstElement, outOfLine));

    m_out.appendTo(outOfLine);
    arrays.add(callOperation<CellPointer>(value, RuntimeOperation::NewFromData, { numberOf(request) }, { .isOnRarePath = true }));
    return arrays.finish();
}

// On x86-64 17 or 18 bytes, against 116 in line, and the call, the return and four instructions more.
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
    if (value.representation() != Representation::LiteralString) {
        defineCell(value, keyConstant(value, key));
        return;
    }
    const PropertyKey& record = types().at(key);
    RELEASE_ASSERT(record.kind == PropertyKeyKind::Text);
    if (literalStrings().indexOf(record.text))
        define(value, spelledLiteralString(record.text));
    else
        define(value, LiteralStringWord(keyConstant(value, key).value()));
}

// The word of a cell is zero until the cell is made, then holds that cell for good, and no compiled code stores there. A test reads it too, and a test declares no effect.
// The word of a value is emptied when the program writes the member, so what it names is no root for good and it stays out of AOTInstance_constantCells.
TypedPointer ConstantEmitter::builtinCellWord(BuiltinCellKey key)
{
    bool isFilledOnce = key.kind() != BuiltinCellKey::Kind::Value;
    return instanceWord(isFilledOnce ? m_heaps.AOTInstance_fieldsFilledOnce : m_heaps.entire(AbstractHeap::AllocatorState), m_mentions.builtinCell(key));
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

// No write ends a load from a heap whose words are filled once.
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

CellPointer ConstantEmitter::makeConstantAtSite(const OperationContext* context, const ConstantMadeAtSite& request, const Machine::AbstractHeap& heapOfWord, LValue literalStringWord)
{
    RELEASE_ASSERT(!literalStringWord == readsNumberBehindCall(request.constant));
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    StubKey stub = createConstantAtSite(request.constant);
    Effects effects = inputs().stubCatalog.effectsOf(stub);
    if (context)
        effects = calls.effectsOfCall(*context, effects);
    effects.reads = { };
    RELEASE_ASSERT(!effects.mayExitSideways() && !effects.mayCollect);

    Machine::PatchOperation* patch = m_out.patchpoint(Machine::Int64);
    if (literalStringWord)
        patch->appendSomeRegister(literalStringWord);
    patch->clobberEarly(RegisterSet::macroClobberedGPRs());
    patch->clobberLate(registersLostAtSiteOf(request.constant));
    // Whoever makes a string or a symbol makes a cell, and leaves every cell that exists as it is.
    calls.applyEffects(patch, effects, ArgumentBufferAccess::None, false, request.runsCodeOfHost ? RuntimeCallEmitter::CellConversions::WhateverEffectsSay : RuntimeCallEmitter::CellConversions::None);
    addHeapWrittenByMakerOfCell(m_proc, patch, heapOfWord);
    patch->setGenerator([records = &m_records, stub, number = request.number, numberIsMention = request.numberIsMention](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg result = params[0].gpr();
        bool takesWordInRegister = params.size() > 1;
        if (takesWordInRegister)
            jit.move(params[1].gpr(), result);
        records->stubCalls.call(jit, stub.withOperandRegister(result), OptionalIndex { });
        if (takesWordInRegister)
            return;
#if CPU(X86_64)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(ThrowSiteWord::opcodeInFrontOfWordBehindCall));
#endif
        if (numberIsMention)
            records->relocations.emitNumber32(CodeSymbol { SymbolSpace::Number, number });
        else
            jit.m_assembler.buffer().putInt(number);
    });
    return CellPointer(patch);
}

// The stub reads the word before it makes anything. On x86-64 a string in line is 24 bytes and 6 instructions, the cell of a built-in 16 bytes and 3 instructions. The rare arm of either is 15 bytes: makeConstantAtSite() and the jump back.
// By the stub they are 10 and 5 bytes, 4 and 3 instructions more beside the call and the return, and a frame in a function that has no other use for one.
// So the rows name the stub wherever no loop of the unit stands around, and in a block of a loop that is rare or unlikely.
// (One binary: for strings the servers are 3.0% smaller and ten of their routes 0.1 to 0.6% slower, the compiler benchmark 3.3% smaller and 0.2% slower;
// for the cells of built-ins the servers are 1.7% smaller and thirteen routes 0.1 to 0.5% slower; for keys nothing changes.)
// The cell is a root for good, so the call may be found again behind a suspension.
CellPointer ConstantEmitter::cellMadeByStub(const OperationContext& context, StubKey stub, CallOptions options, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord)
{
    auto callStubThatMakesCell = [&] {
        return CellPointer(callStub(context, stub, { }, options).first);
    };
    constexpr StateOfKnowledge always = StateOfKnowledge::Always;
    static_assert(formByLoopAlone(Opcode::KeyConstant, always, true).form == formByLoopAlone(Opcode::String, always, true).form && formByLoopAlone(Opcode::KeyConstant, always, false).form == formByLoopAlone(Opcode::String, always, false).form);
    static_assert(formByLoopAlone(Opcode::Builtin, always, true).form == formByLoopAlone(Opcode::String, always, true).form && formByLoopAlone(Opcode::Builtin, always, false).form == formByLoopAlone(Opcode::String, always, false).form);
    bool standsInLoop = generator().isInLoop(context.block());
    FormAtPlace named = formByLoopAlone(Opcode::String, always, standsInLoop);
    bool runsOnce = !standsInLoop && (m_facts.runsAtMostOnce(m_id) || m_program.at(m_id).isModuleBody());
    bool standsInRareBlock = m_code.at(context.block()).frequency == BlockFrequency::Rare;
    bool standsInUnlikelyBlock = m_code.at(context.block()).frequency == BlockFrequency::Unlikely;
    if (named.form == Form::Closed || runsOnce || standsInRareBlock || standsInUnlikelyBlock) {
        m_out.noteFormOf(constantsWithCells, Form::Closed, always, runsOnce ? Place::CodeThatRunsOnce : standsInRareBlock ? Place::RareBlock : standsInUnlikelyBlock ? Place::UnlikelyBlock : named.place);
        return callStubThatMakesCell();
    }
    m_out.noteFormOf(constantsWithCells, named.form, always, named.place);
    ConstantAtSite constant = stub.family() == StubFamily::BuiltinCell ? ConstantAtSite::BuiltinCell : stub.form() == static_cast<uint8_t>(CreatedConstant::Key) ? ConstantAtSite::Key : ConstantAtSite::String;
    return numberedPermanentCell(*options.pureFunction, isNarrow, cellWord, [&](TypedPointer word) {
        return makeConstantAtSite(&context, { constant, options.numberedImmediate->mention, true, options.runsCodeOfHost }, *word.heap());
    });
}

// A pure function is named by the id that the program has for the constant: the same in every function, whatever the mentions of the function are.
// A text that the walk has made is named by the id that the walk gave it, which follows from the function alone.
CellPointer ConstantEmitter::stringConstant(const OperationContext& context, StringID id)
{
    std::span<const uint8_t> text = types().bytesOf(id);
    if (text.empty())
        return emptyString();
    if (std::optional<uint8_t> character = singleLatin1CharacterOf(text))
        return singleCharacterString(word32(*character));

    // ONE CELL A TEXT, in code that runs once too: the bodies of modules write the same few texts thousands of times (`type: "string"`), and what they make of them stays.

    // The cell in the instance holds a narrow reference. A program with wide references has a table of pointers in its place.
    uint32_t mention = m_mentions.add(NumberSpace::StringCell, id);
    bool isNarrow = !m_options.recorded.useWideReferences();
    return cellMadeByStub(context, createConstant(CreatedConstant::String), { .numberedImmediate = NumberedImmediate { mention }, .pureFunction = pureFunction(PureOperation::StringConstant, id.index()) }, isNarrow, [&] {
        return isNarrow
            ? instanceWord(m_heaps.AOTInstance_stringCells, mention)
            : cellWordInConstantTable(m_out, m_heaps.AOTInstance_stringCells, loadImmutablePointer(AOT_INSTANCE_OFFSET(runtime.constants.wideStringCells)), numberOf(mention), ScaleEight);
    });
}

CellPointer ConstantEmitter::keyConstant(const OperationContext& context, PropertyKeyID key)
{
    // A text that no key of the program has. Image numbers keys of the program alone, and whoever takes the cell of a key converts it with toPropertyKey().
    if (liesInThread(key)) {
        const PropertyKey& threadLocalKey = types().at(key);
        RELEASE_ASSERT(threadLocalKey.kind == PropertyKeyKind::Text);
        return stringConstant(context, threadLocalKey.text);
    }
    if (!hasKeyCell(m_program.at(key).kind))
        return failWithoutCell(context, InternalErrorKind::OpcodeWithoutCode);

    // A key cell lives as long as the program, like a string cell. The check of narrow references knows that of a word it sees loaded from this heap.
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

    // The result is the same each time, and only the first call does anything: the host makes the object, and a member that its holder makes at the first read gets a slot in the holder.
    CallOptions options { .numberedImmediate = NumberedImmediate { m_mentions.builtinCell(key) }, .pureFunction = pureFunction(PureOperation::BuiltinCell, key.encoded()), .runsCodeOfHost = true };
    // What the word of a value names is no root for good, so the call stays in an arm of its own, where nothing behind the join finds it again.
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
        return makeConstantAtSite(nullptr, { ConstantAtSite::Key, mention, true }, *cellWord.heap());
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
        return makeConstantAtSite(nullptr, { ConstantAtSite::BuiltinCell, m_mentions.builtinCell(key), true, true }, *cellWord.heap());
    });
}

// A boxed cell is its address. No value has the bits of a word that is still empty, so whether the cell has been made matters only where the two differ.
void ConstantEmitter::branchOnIsBuiltinCell(LValue boxedValueOrCell, BuiltinID id, BranchTargets targets)
{
    build().statistics().count(StatisticID::NativeFunctionTestsEmitted);
    BuiltinCellKey key = BuiltinCellKey::object(id);
    RELEASE_ASSERT(key.hasCell(m_builtins));
    // Where the scope holds the cell it has been made, so the one comparison answers.
    if (LValue known = m_out.findPurePatch(pureFunction(PureOperation::BuiltinCell, key.encoded()), Machine::Int64, { })) {
        m_out.branch(m_out.equal(boxedValueOrCell, known), targets.taken, targets.notTaken);
        return;
    }
    LBasicBlock differs = newBlock();
    LBasicBlock isNotMade = newBlock();
    // One read serves both tests. A read that is found again by its number may be older than the cell. A second read in the next block would be new, find the word filled, and refuse the cell itself.
    LValue word = m_out.load64(builtinCellWord(key));
    m_out.branch(m_out.equal(boxedValueOrCell, word), targets.taken, BlockEdge(differs, targets.notTaken.edgeClass()));

    m_out.appendTo(differs);
    m_out.branch(m_out.notZero64(word), targets.notTaken, rareEdge(isNotMade));

    m_out.appendTo(isNotMade);
    CellPointer made = callWithoutSite<CellPointer>(RuntimeOperation::BuiltinCell, { numberOf(m_mentions.builtinCell(key)) }, { .pureFunction = pureFunction(PureOperation::BuiltinCell, key.encoded()), .isOnRarePath = true });
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

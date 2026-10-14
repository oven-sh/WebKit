/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"
#include "AOTTypedThreads.h"

#if ENABLE(AOT)

#include "B3MemoryValue.h"
#include "B3StackmapGenerationParams.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include <wtf/ScopedLambda.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

std::optional<FieldAccess> fieldOf(const Program& program, const Type& receiver, StringID name)
{
    LayoutID view = receiver.onlyView();
    if (!view)
        return std::nullopt;
    auto field = program.at(view).fieldNamed(name);
    if (!field)
        return std::nullopt;
    return FieldAccess { view, *field };
}

FunctionID findMethod(const Program& program, LayoutID layout, StringID name, MethodKind kind)
{
    for (; layout; layout = program.at(layout).base) {
        // A property of the prototype hides what the prototypes behind it have by the name: a getter alone leaves no setter to be found.
        bool isHidden = false;
        FunctionID found;
        for (auto& method : program.at(layout).methods) {
            if (method.name != name)
                continue;
            if (method.mayBeReplaced)
                return { };
            if (method.kind == kind)
                found = method.function;
            isHidden = true;
        }
        if (isHidden)
            return found;
    }
    return { };
}

// Whether anything of the layout says that its objects have to be asked for some name.
static bool mayAsk(const Layout& layout)
{
    return layout.mayHaveOtherPrototype;
}

// Nothing is known of its prototype chain: see Format::ShapeMayHaveOtherPrototype. What is never seen absent is its own property whatever it inherits from. `field`: what fieldNamed() finds.
static bool mayAskFor(const Layout& layout, std::optional<uint32_t> field)
{
    return layout.mayHaveOtherPrototype && !(field && layout.fields[*field].flags & fieldIsNeverSeenAbsentAsWritten);
}

bool mayInherit(const Program& program, LayoutID view, StringID name)
{
    if (inheritsByItsLayout(program, view, name))
        return true;
    return isObjectLayout(program.at(view).kind) && !isPrivateName(program.string(name)) && std::ranges::any_of(program.at(view).members, [&](LayoutID member) {
        auto& layout = program.at(member);
        return mayAsk(layout) && mayAskFor(layout, layout.fieldNamed(name));
    });
}

bool storeMayAskPrototypeChain(const Program& program, const Function& function, const Block& block, size_t index, LayoutID view, StringID name)
{
    return view && !isInitializingStore(program, function, block, index) && mayInherit(program, view, name);
}

bool mayGiveUp(const Program& program, LayoutID view)
{
    return program.at(view).mayGiveUp || std::ranges::any_of(program.at(view).members, [&](LayoutID member) {
        return program.at(member).mayGiveUp;
    });
}

bool storeMayMakeObjectGiveUp(const Program& program, const Function& function, const Block& block, size_t index, LayoutID view)
{
    return view && !isInitializingStore(program, function, block, index) && mayGiveUp(program, view);
}

bool mayInheritAtBirth(const Program& program, const Function& function, ValueID object, StringID name)
{
    LayoutID made = function.at(object).layout();
    if (isBornUnderAnyPrototype(program, function, object))
        return mayInherit(program, made, name);
    return inheritsByItsLayout(program, made, name);
}

bool isBornUnderAnyPrototype(const Program& program, const Function& function, ValueID object)
{
    auto& made = program.at(function.at(object).layout());
    if (!made.mayHaveOtherPrototype || !constructedBy(program, function, object))
        return false;
    // Where a field is read without a test the run time keeps the `prototype` object of the function, and lets nothing be defined on it under the name of a field but what a
    // store goes over: Typed::PrototypeObject::mayDefine(). So [[Set]] on what is born under it creates the field, marked or not.
    return std::ranges::none_of(made.fields, [](const Field& field) {
        return field.flags & fieldIsNeverSeenAbsentAsWritten;
    });
}

bool isOnObjectPrototype(const Program& program, StringID name)
{
    static constexpr ASCIILiteral namesOnObjectPrototype[] = {
        "constructor"_s, "hasOwnProperty"_s, "isPrototypeOf"_s, "propertyIsEnumerable"_s, "toLocaleString"_s, "toString"_s, "valueOf"_s,
        "__proto__"_s, "__defineGetter__"_s, "__defineSetter__"_s, "__lookupGetter__"_s, "__lookupSetter__"_s,
    };
    for (ASCIILiteral onObjectPrototype : namesOnObjectPrototype) {
        if (equalSpans(program.string(name), onObjectPrototype.span8()))
            return true;
    }
    return false;
}

bool inheritsByItsLayout(const Program& program, LayoutID view, StringID name)
{
    // `#name` is no property.
    if (!isObjectLayout(program.at(view).kind) || isPrivateName(program.string(name)))
        return false;
    if (isOnObjectPrototype(program, name))
        return true;
    // Any record may have been made by a function of the program: `new` does not always know its callee. The `prototype` of such a function
    // takes no other name of a field: see Typed::PrototypeObject. Nothing can be added to the prototype object of a class.
    bool hasRecord = false;
    for (LayoutID member : program.at(view).members) {
        auto& layout = program.at(member);
        auto field = layout.fieldNamed(name);
        hasRecord |= layout.kind != LayoutKind::Instance && (layout.kind != LayoutKind::Record || layout.mayBeMadeByNew || !layout.mayHaveOtherPrototype) && !(field && layout.fields[*field].flags & fieldIsNeverSeenAbsentAsWritten);
    }
    return hasRecord && program.prototypePropertyNames.get(name.index());
}

static bool walkMembersForWhetherOnlyFieldsSupply(const Program& program, LayoutID view, StringID name, bool ofUnmarkedObject)
{
    // The receiver may be any object, which is asked.
    if (program.at(view).admitsForeignObjects || inheritsByItsLayout(program, view, name))
        return false;
    // As mayInherit() asks, in the one walk.
    bool asks = !ofUnmarkedObject && isObjectLayout(program.at(view).kind) && !isPrivateName(program.string(name));
    for (LayoutID member : program.at(view).members) {
        auto& layout = program.at(member);
        auto field = layout.fieldNamed(name);
        if (asks && mayAskFor(layout, field))
            return false;
        if (field)
            continue;
        if (layout.mayHaveOtherPrototype && !ofUnmarkedObject)
            return false;
        // Of a class: what its prototype object has. Of the type of an object literal: the accessors it declares, which the object has as
        // extra properties.
        for (LayoutID ancestor = member; ancestor; ancestor = program.at(ancestor).base) {
            if (program.at(ancestor).hasMethodOfUnknownName)
                return false;
            for (auto& method : program.at(ancestor).methods) {
                if (method.name == name)
                    return false;
            }
        }
    }
    return true;
}

bool onlyFieldsSupply(const Program& program, LayoutID view, StringID name, bool ofUnmarkedObject)
{
    if (program.at(view).members.size() <= 4)
        return walkMembersForWhetherOnlyFieldsSupply(program, view, name, ofUnmarkedObject);
    uint64_t key = static_cast<uint64_t>(ofUnmarkedObject) << 63 | static_cast<uint64_t>(view.index()) << 32 | name.index();
    auto& table = program.whetherOnlyFieldsSupply[(view.index() * 31 + name.index()) % program.whetherOnlyFieldsSupply.size()];
    {
        Locker locker { table.lock };
        if (auto found = table.answers.find(key); found != table.answers.end())
            return found->value;
    }
    bool answer = walkMembersForWhetherOnlyFieldsSupply(program, view, name, ofUnmarkedObject);
    Locker locker { table.lock };
    table.answers.add(key, answer);
    return answer;
}

// No object of the view has the property but as an extra property.
bool onlyExtraPropertiesSupply(const Program& program, LayoutID view, StringID name)
{
    if (!onlyFieldsSupply(program, view, name))
        return false;
    for (LayoutID member : program.at(view).members) {
        if (program.at(member).fieldNamed(name))
            return false;
        // A cast elsewhere may have made it one of these, with what was an extra property in a field.
        for (LayoutID specialization : program.at(member).specializations) {
            if (program.at(specialization).fieldNamed(name))
                return false;
        }
    }
    return true;
}

bool storeMayTransition(const Program& program, LayoutID view, StringID name)
{
    for (LayoutID member : program.at(view).members) {
        auto& layout = program.at(member);
        if (layout.fieldNamed(name))
            continue;
        for (LayoutID specialization : layout.specializations) {
            if (program.at(specialization).fieldNamed(name))
                return true;
        }
    }
    return false;
}

LValue Lowering::marked(LValue load, unsigned flags)
{
    auto* memory = load->as<MemoryValue>();
    if (flags & IsSafe)
        memory->setControlDependent(false);
    if (flags & IsFixed)
        memory->setReadsMutability(Mutability::Immutable);
    return load;
}

uint32_t Lowering::leastSizeOf(LayoutID view) const
{
    return m_leastSizes.ensure(view.index(), [&] {
        auto& layout = m_program.at(view);
        if (layout.members.isEmpty())
            return layout.size;
        uint32_t least = UINT32_MAX;
        for (LayoutID member : layout.members)
            least = std::min(least, m_program.at(member).size);
        return least;
    }).iterator->value;
}

// Whether every cell the value can be, going by the type it was defined with, is at least this large.
unsigned Lowering::safeIfReaches(CellPointer cell, unsigned end) const
{
    auto found = m_typeOfCell.find(cell.value());
    if (found == m_typeOfCell.end())
        return 0;
    if (end <= MarkedBlock::atomSize)
        return IsSafe;
    auto& type = m_program.at(found->value);
    if (type.primitives || type.anyObject || type.anyFunction)
        return 0;
    // Of the classes of the engine, only of typed arrays more is known than that they are objects.
    for (uint16_t engineClass : type.engineClasses) {
        if (m_builtins.classes()[engineClass].typedArrayType == NotTypedArray || end > sizeof(JSArrayBufferView))
            return 0;
    }
    for (LayoutID view : type.views) {
        if (m_program.at(view).admitsForeignObjects || leastSizeOf(view) < end)
            return 0;
    }
    return IsSafe;
}

// A cast gives an object of a member that lacks the field the field, with what the object had as an extra property, and is not summed up
// as a store to fields: nobody can have loaded it. So no load of it gets ahead of the test of the shape that leads to it.
unsigned Lowering::safeIfAllMembersHave(CellPointer cell, const Field& field) const
{
    return field.isLackedBySomeMember || field.isOutOfLine ? 0 : safeIfReaches(cell, field.offset + sizeOf(field.store));
}

Operand Lowering::load(Store store, TypedPointer pointer, unsigned flags, LValue holder)
{
    switch (store) {
    case Store::Uint8:
    case Store::Boolean:
        return Word32(marked(m_out.load8ZeroExt32(pointer), flags));
    case Store::Int8:
        return Word32(marked(m_out.load8SignExt32(pointer), flags));
    case Store::Uint16:
        return Word32(marked(m_out.load16ZeroExt32(pointer), flags));
    case Store::Int16:
        return Word32(marked(m_out.load16SignExt32(pointer), flags));
    case Store::Int32:
    case Store::Int32OrElsewhere:
        return Word32(marked(m_out.load32(pointer), flags));
    case Store::Uint32:
        return zeroExtended(Word32(marked(m_out.load32(pointer), flags)));
    case Store::Int64:
    case Store::OptionalInt64:
    case Store::Cell:
    case Store::JSValue:
        return Operand(repOf(store), marked(m_out.load64(pointer), flags));
    case Store::Cell32:
    case Store::OptionalCell32: {
        LValue index = m_out.zeroExtPtr(marked(m_out.load32(pointer), flags));
        // The back end may take the index from an earlier store or load, across calls that collect. Until here the slot is what keeps the cell alive.
        RELEASE_ASSERT(holder);
        ensureStillAliveHere(CellPointer(holder));
        // Opaque, so that no address absorbs the sum: the collector's scan of the stack has to find the pointer.
        LValue cell = decodeCell32(index);
        if (!isDroppedForTesting()) [[likely]]
            cell = m_out.opaque(cell);
        m_indexOfCell.add(cell, index);
        if (store == Store::Cell32)
            return CellPointer(cell);
        return boxDecoded(index, cell);
    }
    case Store::OptionalBoolean:
        return Boxed(m_out.zeroExtPtr(marked(m_out.load8ZeroExt32(pointer), flags)));
    case Store::Float:
        return Number(m_out.floatToDouble(marked(m_out.loadFloat(pointer), flags)));
    case Store::Double:
    case Store::OptionalDouble:
        return Operand(repOf(store), marked(m_out.loadDouble(pointer), flags));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed Lowering::boxDecoded(LValue index, LValue cell, LValue loaded)
{
    LValue word = loaded ? loaded : index;
    LValue boxed = m_out.select(m_out.below(word, m_out.constInt64(firstCellWord)), index, cell);
    m_decoded.add(boxed, Decoded { index, cell, word, !!loaded });
    return Boxed(boxed);
}

Lowering::Parts Lowering::partsOf(Boxed value)
{
    if (auto found = m_decoded.find(value.value()); found != m_decoded.end())
        return { found->value.loaded, found->value.cell, found->value.absentReadsUndefined };
    return { value.value(), value.value(), false };
}

Boxed Lowering::fromParts(LValue word, LValue cell, bool zeroReadsUndefined)
{
    if (!zeroReadsUndefined)
        return boxDecoded(word, cell);
    return boxDecoded(m_out.select(m_out.isZero64(word), m_out.constInt64(undefinedCell32), word), cell, word);
}

Lowering::Joined Lowering::joinedAs(const Type& type) const
{
    Rep rep = repOf(type);
    // Where nothing goes by a type, what is boxed may be anything.
    return { rep, rep == Rep::JSValue && !(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull)) && !Options::auditAOTTypedAssertions() };
}

Operand Lowering::OperandJoin::finish()
{
    Output& out = m_lowering.m_out;
    bool isInParts = false;
    if (m_joined.isReference) {
        for (auto& arm : m_arms)
            isInParts |= m_lowering.m_decoded.contains(arm.operand.value);
    }
    bool keepsInt32 = false;
    bool keepsExactInteger = false;
    if (m_joined.rep == Rep::Double) {
        for (auto& arm : m_arms) {
            keepsInt32 |= m_lowering.m_int32sOfNumbers.contains(arm.operand.value);
            keepsExactInteger |= m_lowering.m_exactIntegersOfNumbers.contains(arm.operand.value);
        }
    }
    bool keepsWord = false;
    if (m_joined.rep == Rep::Double || m_joined.rep == Rep::OptionalDouble) {
        for (auto& arm : m_arms)
            keepsWord |= m_lowering.m_wordsOfNumbers.contains(arm.operand.value);
    }
    Vector<ValueFromBlock, 4> values;
    Vector<ValueFromBlock, 4> cells;
    Vector<ValueFromBlock, 4> int32s;
    Vector<ValueFromBlock, 4> exactIntegers;
    Vector<ValueFromBlock, 4> words;
    bool zeroReadsUndefined = false;
    for (auto& arm : m_arms) {
        out.appendTo(arm.block);
        if (keepsWord) {
            auto found = m_lowering.m_wordsOfNumbers.find(arm.operand.value);
            words.append(out.anchor(found != m_lowering.m_wordsOfNumbers.end() ? found->value : out.constInt64(INT32_MIN)));
        }
        if (keepsInt32) {
            auto found = m_lowering.m_int32sOfNumbers.find(arm.operand.value);
            LValue int32 = found != m_lowering.m_int32sOfNumbers.end() ? found->value : m_lowering.toInt32InArm(arm.operand.as<Number>(), false).value();
            int32s.append(out.anchor(int32));
        }
        if (keepsExactInteger) {
            auto found = m_lowering.m_exactIntegersOfNumbers.find(arm.operand.value);
            LValue exactInteger = found != m_lowering.m_exactIntegersOfNumbers.end() ? found->value : m_lowering.exactIntegerInArm(arm.operand.as<Number>()).value();
            exactIntegers.append(out.anchor(exactInteger));
        }
        if (isInParts) {
            Parts parts = m_lowering.partsOf(arm.operand.as<Boxed>());
            values.append(out.anchor(parts.word));
            cells.append(out.anchor(parts.cell));
            zeroReadsUndefined |= parts.zeroReadsUndefined;
        } else if (m_joined.rep != Rep::Void)
            values.append(out.anchor(arm.operand.value));
        out.jump(m_continuation);
    }
    out.appendTo(m_continuation);
    if (m_joined.rep == Rep::Void)
        return { };
    LValue value = out.phi(b3TypeOf(m_joined.rep), values);
    if (keepsInt32)
        m_lowering.m_int32sOfNumbers.add(value, out.phi(Int32, int32s));
    if (keepsExactInteger)
        m_lowering.m_exactIntegersOfNumbers.add(value, out.phi(Int64, exactIntegers));
    if (keepsWord)
        m_lowering.m_wordsOfNumbers.add(value, out.phi(Int64, words));
    if (!isInParts)
        return Operand(m_joined.rep, value);
    return m_lowering.fromParts(value, out.phi(Int64, cells), zeroReadsUndefined);
}

Boxed Lowering::remade(Boxed value)
{
    auto found = m_decoded.find(value.value());
    if (found == m_decoded.end())
        return value;
    Decoded decoded = found->value;
    // Opaque: or the back end takes what was made at the read, which is equal, and keeps that alive.
    LValue loaded = m_out.opaque(decoded.loaded);
    LValue index = decoded.absentReadsUndefined ? m_out.select(m_out.isZero64(loaded), m_out.constInt64(undefinedCell32), loaded) : loaded;
    return Boxed(m_out.select(m_out.belowOrEqual(loaded, m_out.constInt64(undefinedCell32)), index, m_out.opaque(decoded.cell)));
}

void Lowering::validateCell32(LValue index, LValue stored, bool mayBeNil)
{
    // A slot that cannot be nil is zero until it is initialized.
    LValue isNil = mayBeNil ? m_out.belowOrEqual(stored, m_out.constInt64(undefinedCell32)) : m_out.isZero64(stored);
    LValue isExact = m_out.bitOr(isNil, m_out.equal(decodeCell32(m_out.zeroExtPtr(index)), stored));
    LBasicBlock isLost = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isExact, usually(continuation), rarely(isLost));
    m_out.appendTo(isLost);
    AOT_TYPED_OPERATION(FactDoesNotHold, Boxed(stored), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::store(Store store, Operand operand, TypedPointer pointer)
{
    RELEASE_ASSERT(operand.rep == repOf(store));
    switch (store) {
    case Store::Uint8:
    case Store::Int8:
    case Store::Boolean:
        m_out.store32As8(operand.value, pointer);
        return;
    case Store::Uint16:
    case Store::Int16:
        m_out.store32As16(operand.value, pointer);
        return;
    case Store::Int32:
    case Store::Int32OrElsewhere:
        m_out.store32(operand.value, pointer);
        return;
    case Store::Uint32:
        m_out.store32(m_out.castToInt32(operand.value), pointer);
        return;
    case Store::Int64:
    case Store::OptionalInt64:
    case Store::Cell:
    case Store::JSValue:
        m_out.store64(operand.value, pointer);
        return;
    case Store::Cell32:
    case Store::OptionalCell32: {
        bool mayBeNil = store == Store::OptionalCell32 && !m_typeOfCell.contains(operand.value);
        LValue index = nullptr;
        LValue kept = operand.value;
        if (auto decoded = m_decoded.find(operand.value); decoded != m_decoded.end() && m_indexOfCell.contains(decoded->value.cell)) {
            index = decoded->value.index;
            kept = decoded->value.cell;
        } else if (auto found = m_indexOfCell.find(operand.value); found != m_indexOfCell.end())
            index = found->value;
        if (!index) {
            index = m_out.lShr(operand.value, m_out.constInt32(cell32Shift));
            if (mayBeNil)
                index = m_out.select(m_out.belowOrEqual(operand.value, m_out.constInt64(undefinedCell32)), operand.value, index);
        }
        index = m_out.castToInt32(index);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateCell32(index, operand.value, mayBeNil);
        m_out.store32(index, pointer);
        // The back end may compute the index earlier, before calls that collect: with that of an earlier store, or ahead of a loop. Until here the pointer is what keeps the cell alive.
        if (!kept->hasInt64())
            ensureStillAliveHere(CellPointer(kept));
        return;
    }
    case Store::OptionalBoolean:
        m_out.store32As8(m_out.castToInt32(operand.value), pointer);
        return;
    case Store::Float:
        m_out.storeFloat(m_out.doubleToFloat(operand.value), pointer);
        return;
    case Store::Double:
    case Store::OptionalDouble:
        m_out.storeDouble(operand.value, pointer);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The threshold changes, and with the usual one the state of a cell, only while this thread is in a call. See applyEffects().
static bool mayStandBetweenStoreAndBarrier(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Nop:
    case B3::Identity:
    case B3::Opaque:
    case B3::Const32:
    case B3::Const64:
    case B3::ConstDouble:
    case B3::ConstFloat:
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
    case B3::Neg:
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
    case B3::BitwiseCast:
    case B3::SExt8:
    case B3::SExt16:
    case B3::SExt32:
    case B3::ZExt32:
    case B3::Trunc:
    case B3::IToD:
    case B3::FloatToDouble:
    case B3::DoubleToFloat:
    case B3::PurifyNaN:
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::Select:
    case B3::Load8Z:
    case B3::Load8S:
    case B3::Load16Z:
    case B3::Load16S:
    case B3::Load:
    case B3::Store8:
    case B3::Store16:
    case B3::Store:
    case B3::Upsilon:
    case B3::Phi:
    case B3::Jump:
    case B3::Branch:
        return true;
    default:
        return false;
    }
}

bool Lowering::everyWayLeadsHere(LBasicBlock block, size_t index, Vector<LBasicBlock, 8>& underWay, Vector<LBasicBlock, 8>& walked) const
{
    if (underWay.contains(block) || underWay.size() + walked.size() >= 8)
        return false;
    if (walked.contains(block))
        return true;
    for (; index < block->size(); ++index) {
        LValue value = block->at(index);
        bool emitsNothing = value->opcode() == B3::Patchpoint && value->as<PatchpointValue>()->generator() == m_generatorOfNothing;
        if (!emitsNothing && !mayStandBetweenStoreAndBarrier(value->opcode()))
            return false;
    }
    if (block == m_out.m_block)
        return true;
    if (!block->numSuccessors())
        return false;
    underWay.append(block);
    for (LBasicBlock successor : block->successorBlocks()) {
        if (!everyWayLeadsHere(successor, 0, underWay, walked))
            return false;
    }
    underWay.removeLast();
    walked.append(block);
    return true;
}

void Lowering::leaveLastBarrierToBarrierOn(CellPointer owner)
{
    LaidBarrier last = std::exchange(m_lastBarrier, LaidBarrier { });
    if (last.owner != owner.value())
        return;
    Vector<LBasicBlock, 8> underWay;
    Vector<LBasicBlock, 8> walked;
    if (!everyWayLeadsHere(last.next, last.indexInNext, underWay, walked))
        return;
    if (last.blockOfBranch)
        last.test->replaceWithJump(last.blockOfBranch, FrequentedBlock(last.next));
    else
        last.test->replaceWithNop();
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: f", m_id.index(), ": a write barrier is left to the one behind it");
}

void Lowering::barrier(CellPointer owner)
{
    leaveLastBarrierToBarrierOn(owner);
    if (!m_isInLoop) {
#if !CPU(X86_64)
        LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
        LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, owner.value(), JSCell::cellStateOffset()));
#endif
        PatchpointValue* patchpoint = m_out.patchpoint(Void);
        m_lastBarrier = LaidBarrier { owner.value(), patchpoint, nullptr, m_out.m_block, m_out.m_block->size() };
        applyEffects(patchpoint, effectsOfOperation<operationAOTTypedBarrierSlow>());
        patchpoint->append(ConstrainedValue(owner.value(), ValueRep::SomeRegister));
#if !CPU(X86_64)
        patchpoint->append(ConstrainedValue(state, ValueRep::SomeRegister));
        patchpoint->append(ConstrainedValue(threshold, ValueRep::SomeRegister));
#endif
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        if (LoweringContext::coldSitesForceFrame()) {
            m_context.emitsCalls = true;
            patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        } else if (!hasStubsForFunctionsWithoutFrame)
            m_context.holdsColdSites = true;
        uint32_t immediate = static_cast<uint32_t>(static_cast<unsigned>(isARM64() ? Entry::operationAOTTypedBarrierSlowKeepingRegisters : Entry::operationAOTTypedBarrierSlow) * sizeof(void*));
        patchpoint->setGenerator([context = &m_context, immediate](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg owner = params[0].gpr();
#if CPU(X86_64)
            jit.load8(CCallHelpers::Address(owner, JSCell::cellStateOffset()), jit.scratchRegister());
            CCallHelpers::Jump isSlow = jit.branch32(CCallHelpers::BelowOrEqual, jit.scratchRegister(), CCallHelpers::Address(instanceGPR, Instance::offsetOfBarrierThreshold()));
#else
            CCallHelpers::Jump isSlow = jit.branch32(CCallHelpers::BelowOrEqual, params[1].gpr(), params[2].gpr());
#endif
            CCallHelpers::Label done = jit.label();
            Vector<GPRReg, 4> saved;
            for (GPRReg reg : { GPRInfo::returnValueGPR, operationArgumentGPR(1), stubTemporaryGPRs[0], stubTemporaryGPRs[1] }) {
                if (isARM64())
                    break;
                bool isTemporary = reg == stubTemporaryGPRs[0] || reg == stubTemporaryGPRs[1];
                if (isTemporary && !registersChangedBy(Stub::ColdPlainOperation)->contains(reg, IgnoreVectors))
                    continue;
                if (params.usedRegisters().contains(reg, IgnoreVectors) && !(reg == operationArgumentGPR(1) && reg == owner))
                    saved.append(reg);
            }
            if (saved.size() & 1)
                saved.append(jit.scratchRegister());
            bool isLeaf = hasStubsForFunctionsWithoutFrame && context->hasNoFrame(params.proc().code());
#if CPU(X86_64)
            bool makesFrame = context->hasNoFrame(params.proc().code());
#endif
            bool holdsFPR = false;
            RegisterSet::registersToSaveForCCall(params.usedRegisters()).forEach([&](Reg reg) {
                holdsFPR |= reg.isFPR();
            });
            params.addLatePath([=](CCallHelpers& jit) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                isSlow.link(&jit);
#if CPU(X86_64)
                if (makesFrame)
                    jit.emitFunctionPrologue();
#endif
                for (unsigned i = 0; i < saved.size(); i += 2)
                    jit.pushPair(saved[i], saved[i + 1]);
#if CPU(ARM64)
                if (isLeaf)
                    jit.move(CCallHelpers::linkRegister, ARM64Registers::x17);
#endif
                unsigned stub = static_cast<unsigned>(Stub::ColdPlainOperation);
                if (!holdsFPR)
                    stub += 2;
                if (isLeaf)
                    ++stub;
                ArgumentMoves ownerToArgument;
#if CPU(ARM64)
                appendArgumentMove(ownerToArgument, ARM64Registers::x16, ArgumentSource::Register, operationArgumentGPR(1));
#endif
                if (owner != operationArgumentGPR(1))
                    appendArgumentMove(ownerToArgument, operationArgumentGPR(1), ArgumentSource::Register, owner);
                if (ownerToArgument.isEmpty())
                    context->stubCalls.call(jit, static_cast<Stub>(stub), immediate, CallSite { });
                else
                    context->stubCalls.call(jit, static_cast<Stub>(stub), immediate, CallSite { }, ownerToArgument);
                for (unsigned i = saved.size(); i; i -= 2)
                    jit.popPair(saved[i - 2], saved[i - 1]);
#if CPU(X86_64)
                if (makesFrame)
                    jit.emitFunctionEpilogueWithEmptyFrame();
#endif
                jit.jump().linkTo(done, &jit);
            });
        });
        return;
    }
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, owner.value(), JSCell::cellStateOffset()));
    LBasicBlock blockOfBranch = m_out.m_block;
    m_out.branch(m_out.above(state, threshold), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath);
    AOT_TYPED_OPERATION(BarrierSlow, owner);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    m_lastBarrier = LaidBarrier { owner.value(), blockOfBranch->last(), blockOfBranch, continuation, 0 };
}

// What barrier() asks.
Word32 Lowering::isBeyondBarrierThreshold(CellPointer owner)
{
    LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, owner.value(), JSCell::cellStateOffset()));
    return Word32(m_out.above(state, threshold));
}

void Lowering::barrier(CellPointer array, std::span<const Boxed> stored)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfBarrierThreshold()));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, array.value(), JSCell::cellStateOffset()));
    m_out.branch(m_out.above(state, threshold), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath);
    for (Boxed value : stored)
        callAllocationStub(Stub::RememberElement, { { array.value(), stubTemporaryGPRs[0] }, { value.value(), stubTemporaryGPRs[2] } }, effectsOfOperation<operationAOTTypedRememberElement>(), false);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// The collector may look at the cell, from another thread, as soon as it is stored anywhere.
void Lowering::fenceAfterInitialization()
{
#if CPU(X86_64)
    m_out.fence(&m_heaps.root, nullptr);
#else
    LBasicBlock fence = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, m_instance, Instance::offsetOfMutatorShouldBeFenced())), rarely(fence), usually(continuation));
    m_out.appendTo(fence);
    m_out.fence(&m_heaps.root, nullptr);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
#endif
}

void Lowering::didLowerInitializingStore()
{
    if (!m_fieldsAwaitingFirstStore.isEmpty()) [[unlikely]] {
        if (auto field = fieldInitializedBy(m_layoutOfFieldsAwaitingFirstStore, m_function.at(m_currentBlock->values[m_currentIndexInBlock])))
            m_fieldsAwaitingFirstStore.clear(*field);
    }
    size_t next = m_currentIndexInBlock + 1;
    auto& store = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    if (store.opcode == Opcode::PutField && m_function.environmentsInFrame.get(originOf(store.child(0)).index()))
        return;
    if (next < m_currentBlock->values.size() && isInitializingStore(m_program, m_function, *m_currentBlock, next))
        return;
    if (store.opcode == Opcode::PutField && m_program.at(store.layout()).isCoroutineFrame && next < m_currentBlock->values.size()) {
        auto& following = m_function.at(m_currentBlock->values[next]);
        if (following.opcode == Opcode::NewClosure && m_program.isResumeFunction(m_program.at(following.function())) && following.arguments[0] == store.child(0)) {
            PatchpointValue* line = m_out.patchpoint(Void);
            line->effects = Effects::none();
            line->effects.fence = true;
            line->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
            return;
        }
    }
    if (store.opcode == Opcode::PutField && m_program.at(store.layout()).reuseVariable) {
        m_isFillingReusedEnvironment = false;
        barrier(in<CellPointer>(store.child(0)));
    }
    fenceAfterInitialization();
}

const Field* Lowering::fieldFor(const Type& receiver, StringID name) const
{
    auto access = fieldOf(m_program, receiver, name);
    return access ? &m_program.at(access->layout).fields[access->field] : nullptr;
}

// For the order of the code only. By the layouts that objects are made as: nothing says how many objects.
bool Lowering::mostMembersLack(LayoutID view, const Field& field) const
{
    uint64_t have = 0;
    uint64_t lack = 0;
    for (LayoutID member : m_program.at(view).members) {
        auto& layout = m_program.at(member);
        if (layout.shape)
            ++(m_program.passes(field.membersWithIt, layout.shape) ? have : lack);
    }
    return lack >= 4 * have;
}

bool Lowering::mayGiveUp(LayoutID view) const
{
    return m_viewsThatMayGiveUp.ensure(view.index(), [&] {
        return Typed::mayGiveUp(m_program, view);
    }).iterator->value;
}

bool Lowering::mayBeFrozen(LayoutID view) const
{
    return m_viewsThatMayBeFrozen.ensure(view.index(), [&] {
        for (LayoutID member : m_program.at(view).members) {
            if (m_program.at(member).mayBeFrozen)
                return true;
        }
        return false;
    }).iterator->value;
}

Word32 Lowering::extensionCovers(CellPointer cell, const Field& field)
{
    Word32 word = extensionWordOf(cell);
    if (field.offset + sizeOf(field.store) <= leastExtensionSize)
        return logicalNot(isZero(word));
    LBasicBlock hasExtension = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> answers;
    answers.append(m_out.anchor(m_out.int32Zero));
    m_out.branch(isZero(word).value(), unsure(continuation), unsure(hasExtension));
    m_out.appendTo(hasExtension);
    Word32 size = loadField<Word32>(extensionAt(word), m_heaps.AOTTypedExtension_size, IsFixed);
    answers.append(m_out.anchor(aboveOrEqual(size, word32(field.offset + sizeOf(field.store))).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Word32(m_out.phi(Int32, answers));
}

Operand Lowering::loadSlot(CellPointer cell, const Field& field, unsigned flags)
{
    if (!field.isOutOfLine)
        return load(field.store, fieldAt(cell, field), flags, cell.value());
    RELEASE_ASSERT(isAbsentAsZero(field.store) && repOf(field.store) == Rep::JSValue);
    Word32 word = extensionWordOf(cell);
    LBasicBlock hasExtension = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> words;
    words.append(m_out.anchor(m_out.int64Zero));
    m_out.branch(isZero(word).value(), unsure(continuation), unsure(hasExtension));
    m_out.appendTo(hasExtension);
    Pointer extension = extensionAt(word);
    if (field.offset + sizeOf(field.store) > leastExtensionSize) {
        LBasicBlock within = m_out.newBlock();
        Word32 size = loadField<Word32>(extension, m_heaps.AOTTypedExtension_size, IsFixed);
        words.append(m_out.anchor(m_out.int64Zero));
        m_out.branch(aboveOrEqual(size, word32(field.offset + sizeOf(field.store))).value(), unsure(within), unsure(continuation));
        m_out.appendTo(within);
    }
    TypedPointer slot = m_out.address(heapOf(field.slot), extension.value(), field.offset);
    words.append(m_out.anchor(sizeOf(field.store) == 8 ? m_out.load64(slot) : m_out.zeroExtPtr(sizeOf(field.store) == 4 ? m_out.load32(slot) : m_out.load8ZeroExt32(slot))));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    LValue raw = m_out.phi(Int64, words);
    ensureStillAliveHere(cell);
    if (field.store != Store::OptionalCell32)
        return Boxed(raw);
    LValue decoded = decodeCell32(raw);
    if (!isDroppedForTesting()) [[likely]]
        decoded = m_out.opaque(decoded);
    m_indexOfCell.add(decoded, raw);
    return boxDecoded(raw, decoded);
}

// One load. The slot holds what the widest of the types says that share it: where that is more than the type of the value, a test follows.
Operand Lowering::readField(CellPointer cell, const Field& field, const Type& type)
{
    if (field.store == Store::Int32OrElsewhere)
        return readInt32OrElsewhere(cell, field, type);
    Operand raw = loadSlot(cell, field, safeIfAllMembersHave(cell, field));
    // The load has cleared the upper bits of what has no sign, so this costs nothing.
    if (field.store == Store::Uint8 || field.store == Store::Uint16)
        raw = zeroExtended(raw.as<Word32>());
    auto& stored = m_program.at(field.stored);
    if (!(stored.primitives & Primitive::Absent))
        return ensure(raw, stored, type);
    if (std::exchange(m_fieldIsPresent, false) && raw.rep == Rep::JSValue)
        return ensure(raw, stored.withoutPrimitives(Primitive::Absent), type);

    // A field that is absent reads `undefined`.
    Type held = stored;
    held.primitives = (held.primitives & ~Primitive::Absent) | Primitive::Undefined;
    if (auto found = m_decoded.find(raw.value); found != m_decoded.end()) {
        Decoded decoded = found->value;
        return ensure(boxDecoded(m_out.select(m_out.isZero64(decoded.index), m_out.constInt64(undefinedCell32), decoded.index), decoded.cell, decoded.index), held, type);
    }
    // Where only a number will do, what stands for the one is turned away like what stands for the other.
    if (raw.rep == Rep::JSValue || type.primitives & Primitive::Undefined || (m_fusedCasts && &type == &m_fusedCasts->type)) {
        Integer bits = bitsOf(raw);
        raw = fromBits(select(isAbsent(raw), integer(bitsOfMarker(Primitive::Undefined, raw.rep)), bits), raw.rep);
        static_assert(JSValue::ValueUndefined == undefinedCell32 && JSValue::ValueNull == nullCell32);
        if (raw.rep == Rep::JSValue && !(held.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull)))
            m_decoded.add(raw.value, Decoded { raw.value, bits.value(), bits.value(), true });
    }
    return ensure(raw, held, type);
}

Operand Lowering::askMarkedObject(CellPointer cell, StringID name, const Type& type)
{
    if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions() || isPrivateName(m_program.string(name))) [[unlikely]]
        return ensure(emitGetById(box(cell), name), type);
    TypeID typeOfRead = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    if (!m_fusedCasts && &m_program.at(typeOfRead) == &type && holdsCells(repOf(type)) && !type.isNever() && !m_useCastTest && !m_foreignObjectsDoNotFit && !handlerEnteredByBranch())
        return convert(AOT_TYPED_OPERATION(GetByNameHeld, cell, word32(name.index()), word32(typeOfRead.index())), type, repOf(type));
    return ensure(AOT_TYPED_OPERATION(GetByName, box(cell), word32(name.index())), type);
}

// What an object that has the field finds under the name: its own property, or what it inherits where the field is absent.
Operand Lowering::readProperty(CellPointer cell, LayoutID view, const Field& field, const Type& type)
{
    if (!(m_program.at(field.stored).primitives & Primitive::Absent) || !mayInherit(m_program, view, field.name))
        return readField(cell, field, type);
    auto read = [&] {
        SetForScope fieldIsPresent(m_fieldIsPresent, true);
        return readField(cell, field, type);
    };
    auto ask = [&] {
        return ensure(emitGetById(box(cell), field.name), type);
    };
    if (inheritsByItsLayout(m_program, view, field.name))
        return eitherOperand(hasField(cell, field), joinedAs(type), read, ask);
    // That an optional field is not set is nothing rare. That the object has to be asked is.
    LBasicBlock isPresent = m_out.newBlock();
    LBasicBlock isAbsent = m_out.newBlock();
    LBasicBlock isUnmarked = m_out.newBlock();
    LBasicBlock asks = newColdBlock();
    OperandJoin join(*this, joinedAs(type));
    m_out.branch(hasField(cell, field).value(), unsure(isPresent), unsure(isAbsent));
    m_out.appendTo(isPresent);
    join.add(read());
    m_out.appendTo(isAbsent);
    m_out.branch(lacksBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(isUnmarked), rarely(asks));
    m_out.appendTo(isUnmarked);
    join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
    m_out.appendTo(asks);
    join.add(askMarkedObject(cell, field.name, type));
    return join.finish();
}

Word32 Lowering::hasField(CellPointer cell, const Field& field)
{
    if (!(m_program.at(field.stored).primitives & Primitive::Absent))
        return boolean(true);
    return logicalNot(isFieldAbsent(cell, field, safeIfAllMembersHave(cell, field)));
}

Word32 Lowering::hasOwnField(CellPointer cell, const Field& field)
{
    if (!(field.flags & fieldMayExistWithoutStorage))
        return hasField(cell, field);
    return either<Word32>(hasField(cell, field), [&] {
        return boolean(true);
    }, [&] {
        return AOT_TYPED_OPERATION(HasExtra, cell, word32(field.name.index() | nameMayBeOfOwnField));
    });
}

bool Lowering::isStoreThatNeedsNoStorage(const Field& field, ValueID stored) const
{
    if (!field.isOutOfLine)
        return false;
    ValueID constant = m_program.constantOf(m_function, stored);
    RELEASE_ASSERT(field.flags & fieldMayExistWithoutStorage && constant && m_function.at(constant).opcode == Opcode::Undefined);
    return true;
}

// The slot may say so.
Word32 Lowering::isFieldAbsent(CellPointer cell, const Field& field, unsigned flags)
{
    Operand raw = loadSlot(cell, field, flags);
    if (field.store == Store::Int32OrElsewhere)
        return equal(raw.as<Word32>(), word32(absentInt32));
    return isAbsent(raw);
}

// Store::Int32OrElsewhere, see AOTTypedData.h. The field is of the type it is of: this is how it is kept.

static Type numbersKeptInPlace()
{
    return Type::integer({ leastInt32InPlace, INT32_MAX });
}

// Whether the word is the number. Ends the block.
void Lowering::branchOnInt32InPlace(Integer word, FTL::WeightedTarget inPlace, FTL::WeightedTarget otherwise)
{
#if CPU(ARM64)
    // leastInt32InPlace takes an instruction to make. The words below it are those with the highest bit set and the next one clear.
    static_assert(leastInt32InPlace == -(1 << 30));
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    patchpoint->effects = Effects::none();
    patchpoint->effects.terminal = true;
    patchpoint->append(word.value(), ValueRep::SomeRegister);
    // Of two successors that are as frequent as one another the second comes next in the code, like the side of a branch that is not taken.
    m_out.appendSuccessor(otherwise);
    m_out.appendSuccessor(inPlace);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.m_assembler.bic<32, ARM64Assembler::S>(ARM64Registers::zr, params[0].gpr(), params[0].gpr(), ARM64Assembler::LSL, 1);
        CCallHelpers::Jump isOtherWord = jit.branchOnFlags(CCallHelpers::LessThan);
        CCallHelpers::Jump isInPlace;
        if (!params.fallsThroughToSuccessor(1))
            isInPlace = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            isOtherWord.linkTo(*labels[0], &jit);
            if (isInPlace.isSet())
                isInPlace.linkTo(*labels[1], &jit);
        });
    });
#else
    m_out.branch(lessThan(word, integer(leastInt32InPlace)).value(), otherwise, inPlace);
#endif
}

// A load, a comparison, a branch that is not taken: then the word is read as a field of int32 is.
Operand Lowering::readInt32OrElsewhere(CellPointer cell, const Field& field, const Type& type)
{
    auto& stored = m_program.at(field.stored);
    // Nothing else uses what is loaded, so that the back end makes one instruction of the two.
    Integer word = signExtended(load(field.store, fieldAt(cell, field), safeIfAllMembersHave(cell, field)).as<Word32>());
    bool readsUndefined = stored.primitives & (std::exchange(m_fieldIsPresent, false) ? Primitive::Undefined : Primitive::Absent | Primitive::Undefined);
    bool readsNull = stored.primitives & Primitive::Null;
    Rep rep = repOf(type);
    Join join(m_out, rep, b3TypeOf(rep));
    ValueID lowered = valueBeingLowered();
    bool keepsIntegers = rep == Rep::Double && lowered;
    bool keepsInt32 = keepsIntegers && isTakenAsInt32(lowered);
    bool keepsExactInteger = keepsIntegers;
    bool keepsWord = rep == Rep::Double || rep == Rep::OptionalDouble;
    Vector<ValueFromBlock, 4> int32s;
    Vector<ValueFromBlock, 4> exactIntegers;
    Vector<ValueFromBlock, 4> words;
    bool isInPlaceArm = false;
    auto add = [&](Operand number, Word32 int32, Integer exactInteger) {
        if (keepsWord)
            words.append(m_out.anchor(isInPlaceArm ? word.value() : m_out.constInt64(INT32_MIN)));
        if (keepsInt32)
            int32s.append(m_out.anchor(int32.value()));
        if (keepsExactInteger)
            exactIntegers.append(m_out.anchor(exactInteger.value()));
        join.add(number);
    };
    LBasicBlock inPlace = m_out.newBlock();
    LBasicBlock elsewhere = newUnlikelyBlock();
    if (!readsUndefined && !readsNull)
        branchOnInt32InPlace(word, usually(inPlace), rarely(elsewhere));
    else {
        // Nothing says that these are rare.
        bool isMostlyThere = !(field.flags & fieldIsOptional) || field.flags & fieldIsSetAtCreation;
        LBasicBlock isNoNumber = isMostlyThere ? newUnlikelyBlock() : m_out.newBlock();
        LBasicBlock isNil = m_out.newBlock();
        branchOnInt32InPlace(word, unsure(inPlace), unsure(isNoNumber));
        m_out.appendTo(isNoNumber);
        // The word is one of the three that are no number, or refers to a box.
        static_assert(absentInt32 < nullInt32 && undefinedInt32 < nullInt32);
        m_out.branch(greaterThan(word, integer(nullInt32)).value(), rarely(elsewhere), usually(isNil));
        m_out.appendTo(isNil);
        if (readsUndefined && readsNull) {
            LBasicBlock isNull = m_out.newBlock();
            LBasicBlock isUndefined = m_out.newBlock();
            m_out.branch(equal(word, integer(nullInt32)).value(), unsure(isNull), unsure(isUndefined));
            m_out.appendTo(isNull);
            add(ensure(boxedConstant(jsNull()), Type::ofPrimitives(Primitive::Null), type), word32(0), integer(INT64_MIN));
            m_out.appendTo(isUndefined);
        }
        // A field that is absent reads `undefined`.
        if (readsUndefined)
            add(ensure(undefined(), Type::ofPrimitives(Primitive::Undefined), type), word32(0), integer(INT64_MIN));
        else
            add(ensure(boxedConstant(jsNull()), Type::ofPrimitives(Primitive::Null), type), word32(0), integer(INT64_MIN));
    }
    m_out.appendTo(inPlace);
    isInPlaceArm = true;
    add(ensure(word, numbersKeptInPlace(), type), low32(word), word);
    isInPlaceArm = false;
    m_out.appendTo(elsewhere);
    Type numbers = stored.withoutPrimitives(Primitive::Absent | Primitive::UndefinedOrNull);
    Number boxed(m_out.loadDouble(numberBoxAt(field, low32(word))));
    ensureStillAliveHere(cell);
    Operand kept = ensure(boxed, numbers, type);
    Word32 int32 = keepsInt32 ? toInt32InArm(kept.as<Number>(), false) : word32(0);
    add(kept, int32, keepsExactInteger ? exactIntegerInArm(kept.as<Number>()) : integer(INT64_MIN));
    Operand joined = join.finish();
    if (keepsInt32)
        m_int32sOfNumbers.add(joined.value, m_out.phi(Int32, int32s));
    if (keepsExactInteger)
        m_exactIntegersOfNumbers.add(joined.value, m_out.phi(Int64, exactIntegers));
    if (keepsWord)
        m_wordsOfNumbers.add(joined.value, m_out.phi(Int64, words));
    return joined;
}

// Whether the store comes down to a store of 32 bits whatever the value is. Otherwise it may call the run time, which may collect garbage.
bool Lowering::isStoredInPlace(const Field& field, ValueID stored) const
{
    return field.store != Store::Int32OrElsewhere || typeOf(stored).withoutPrimitives(Primitive::UndefinedOrNull).isWithin(numbersKeptInPlace(), m_program);
}

// What is stored is of the type of the field. `otherwise`, if there is such a block: where to go with a number that the word cannot
// hold, nothing having been done.
void Lowering::storeInt32OrElsewhere(CellPointer cell, const Field& field, ValueID stored, bool isFirstStore, LBasicBlock otherwise)
{
    auto& type = typeOf(stored);
    auto put = [&](Word32 word) {
        store(field.store, word, fieldAt(cell, field));
    };
    Type inPlace = numbersKeptInPlace();
    if (type.isWithin(inPlace, m_program) && isFirstStore) {
        put(low32(in<Integer>(stored)));
        return;
    }
    LBasicBlock continuation = m_out.newBlock();
    if (type.primitives & Primitive::Number && !isFirstStore) {
        LBasicBlock hasBox = newUnlikelyBlock();
        LBasicBlock hasNoBox = m_out.newBlock();
        Word32 current = load(field.store, fieldAt(cell, field), 0).as<Word32>();
        m_out.branch(refersToNumberBox(current).value(), rarely(hasBox), usually(hasNoBox));
        m_out.appendTo(hasBox);
        m_out.storeDouble(fitOrElse(get(stored), type, Type::ofPrimitives(Primitive::Number), hasNoBox).as<Number>().value(), numberBoxAt(field, current));
        ensureStillAliveHere(cell);
        m_out.jump(continuation);
        m_out.appendTo(hasNoBox);
    }
    if (type.isWithin(inPlace, m_program)) {
        put(low32(in<Integer>(stored)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    if (type.primitives & Primitive::Number) {
        LBasicBlock isNotInPlace = type.mayBeUndefinedOrNull() ? m_out.newBlock() : newColdBlock();
        if (type.isInteger() && type.integers.isWithin(Range::int32())) {
            Integer word = in<Integer>(stored);
            LBasicBlock isInPlace = m_out.newBlock();
            branchOnInt32InPlace(word, usually(isInPlace), rarely(isNotInPlace));
            m_out.appendTo(isInPlace);
            put(low32(word));
        } else if (auto found = m_wordsOfNumbers.find(get(stored).value); found != m_wordsOfNumbers.end()) {
            Integer word(found->value);
            LBasicBlock isWord = m_out.newBlock();
            LBasicBlock isNoWord = newUnlikelyBlock();
            branchOnInt32InPlace(word, usually(isWord), rarely(isNoWord));
            m_out.appendTo(isWord);
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                LBasicBlock doesNotHold = newColdBlock();
                LBasicBlock holds = m_out.newBlock();
                m_out.branch(equal(bitsOf(convertToNumber(word)), bitsOf(get(stored))).value(), usually(holds), rarely(doesNotHold));
                m_out.appendTo(doesNotHold);
                {
                    SetForScope isReporting(m_isReportingThatFactDoesNotHold, true);
                    AOT_TYPED_OPERATION(FactDoesNotHold, boxInt32(low32(word)), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
                }
                m_out.jump(holds);
                m_out.appendTo(holds);
            }
            put(low32(word));
            m_out.jump(continuation);
            m_out.appendTo(isNoWord);
            put(low32(fitOrElse(get(stored), type, inPlace, isNotInPlace).as<Integer>()));
        } else
            put(low32(fitOrElse(get(stored), type, inPlace, isNotInPlace).as<Integer>()));
        m_out.jump(continuation);
        m_out.appendTo(isNotInPlace);
    }
    for (auto [primitive, pattern] : { std::pair { Primitive::Undefined, undefinedInt32 }, std::pair { Primitive::Null, nullInt32 } }) {
        if (!(type.primitives & primitive))
            continue;
        LBasicBlock isIt = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(fits(get(stored), type, Type::ofPrimitives(primitive)).value(), unsure(isIt), unsure(isNot));
        m_out.appendTo(isIt);
        put(word32(pattern));
        m_out.jump(continuation);
        m_out.appendTo(isNot);
    }
    if (isStoredInPlace(field, stored))
        m_out.unreachable();
    else if (otherwise)
        m_out.jump(otherwise);
    else {
        if (isFirstStore)
            put(word32(0));
        if (get(stored).rep == Rep::Double)
            AOT_TYPED_OPERATION(PutNumberElsewhere, cell, word32(field.name.index()), in<Number>(stored));
        else
            AOT_TYPED_OPERATION(PutElsewhere, cell, word32(field.name.index()), in<Boxed>(stored));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

// Whether the order word an object is allocated with counts the property in.
bool Lowering::isBornWith(const Allocation& allocation, StringID name) const
{
    auto& layout = m_program.at(allocation.layout);
    if (!isObjectLayout(layout.kind))
        return true;
    auto field = layout.fieldNamed(name);
    auto born = allocation.order.span().first(allocation.born);
    return field && std::ranges::find(born, *field) != born.end();
}

// The field of the layout that one of the first stores to a cell goes to. The cell may have been allocated with another layout than the store
// names: see Allocation::numberOfLiterals.
std::optional<uint32_t> Lowering::fieldInitializedBy(LayoutID id, const Value& store) const
{
    if (store.opcode == Opcode::PutField && store.layout() == id)
        return store.field();
    return m_program.at(id).fieldNamed(store.opcode == Opcode::PutById ? store.string() : m_program.at(store.layout()).fields[store.field()].name);
}

// Of a store that is one of the first to the object: whether one before it may have called the run time.
bool Lowering::collectorMayHaveRunSinceBirth(ValueID object) const
{
    auto& layout = m_program.at(m_function.at(object).layout());
    auto& values = m_currentBlock->values;
    auto& last = m_walkedBackFrom.add(object.index(), WalkedBackFrom { }).iterator->value;
    if (last.block != m_currentBlock || last.index > m_currentIndexInBlock)
        last = WalkedBackFrom { m_currentBlock, 0, false };
    if (last.collectorMayHaveRun)
        return true;
    size_t walkedAlready = std::exchange(last.index, m_currentIndexInBlock);
    for (size_t i = m_currentIndexInBlock; i-- > walkedAlready && values[i] != object;) {
        auto& earlier = m_function.at(values[i]);
        auto field = fieldInitializedBy(m_function.at(object).layout(), earlier);
        // A store that creates the field calls the run time too: see writeField().
        if (field && (!isStoredInPlace(layout.fields[*field], earlier.child(1)) || !isBornWith(m_program.allocations[m_function.at(object).allocation()], layout.fields[*field].name))) {
            last.collectorMayHaveRun = true;
            return true;
        }
    }
    return false;
}

// One store: what is stored is of the type of the field, which the verifier has seen to.
// The field, which was absent and is about to be stored to, is the last of the keys from here on. What becomes of the order word goes by the shape, by the order word and by the field, and stays the same for good. The site remembers the two words as
// they were the last time, and the order word that came of them: see generateDidCreateTypedField().
// `unlessAsked`: emitted where the words of the object are not those of the last time, which have none of the bits 28 to 31.
void Lowering::didCreateFieldInCell(CellPointer cell, const Field& field, OperationEffects effects, const ScopedLambda<void()>& unlessAsked)
{
    uint32_t slotKey = slotKeyFor(field.offset, field.isOutOfLine);
    auto ensureStorage = [&] {
        if (!field.isOutOfLine)
            return;
        LBasicBlock lacksStorage = m_out.newBlock();
        LBasicBlock hasNoExtension = m_out.newBlock();
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock hasStorage = m_out.newBlock();
        m_out.branch(extensionCovers(cell, field).value(), unsure(hasStorage), unsure(lacksStorage));
        m_out.appendTo(lacksStorage);
        m_out.branch(isZero(extensionWordOf(cell)).value(), usually(hasNoExtension), rarely(slowPath));
        m_out.appendTo(hasNoExtension);
        size_t size = WTF::roundUpToMultipleOf<leastExtensionSize>(field.offset + sizeOf(field.store));
        LValue storage = allocateHeapCell(allocatorForSize(fixedPointer(Instance::offsetOfAuxiliarySpace()), size), slowPath);
        applyEffects(storage->as<PatchpointValue>(), { Writes::RuntimeState, false });
        static_assert(!offsetOfExtensionSize);
        for (size_t offset = 0; offset < size; offset += sizeof(uint64_t))
            m_out.store64(m_out.constInt64(offset ? 0 : size), TypedPointer(m_heaps.root, m_out.add(storage, m_out.constIntPtr(offset))));
        fenceAfterInitialization();
        storeField(cell, m_heaps.AOTTypedObject_extension, Word32(m_out.castToInt32(m_out.lShr(storage, m_out.constInt32(cell32Shift)))));
        barrier(cell);
        m_out.jump(hasStorage);
        m_out.appendTo(slowPath);
        AOT_TYPED_OPERATION_WITH_EFFECTS(EnsureStorage, effects, cell, word32(field.name.index()));
        m_out.jump(hasStorage);
        m_out.appendTo(hasStorage);
    };
    auto throughStub = [&](LValue cache) {
        unlessAsked();
        if (!slotKey)
            AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(field.name.index()));
        else if (isARM64()) {
            // The stub keeps whatever the site holds in registers but its own temporaries: see generateDidCreateTypedField().
            PatchpointValue* patchpoint = m_out.patchpoint(Void);
            applyEffects(patchpoint, effects);
            patchpoint->append(ConstrainedValue(cell.value(), ValueRep::reg(GPRInfo::argumentGPR1)));
            patchpoint->append(ConstrainedValue(cache, ValueRep::reg(GPRInfo::argumentGPR3)));
            RegisterSet changed = RegisterSet::macroClobberedGPRs();
            changed.add(GPRInfo::argumentGPR2, IgnoreVectors);
            for (GPRReg reg : stubTemporaryGPRs)
                changed.add(reg, IgnoreVectors);
            patchpoint->clobber(changed);
            if (!hasStubsForFunctionsWithoutFrame) {
                m_context.emitsCalls = true;
                patchpoint->clobberLate(RegisterSet { callMarkerGPR });
            }
            patchpoint->setGenerator([context = &m_context, offset = slotKey](CCallHelpers& jit, const StackmapGenerationParams& params) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                bool isLeaf = context->hasNoFrame(params.proc().code());
#if CPU(ARM64)
                if (isLeaf)
                    jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
                jit.move(CCallHelpers::TrustedImm32(offset), GPRInfo::argumentGPR2);
                context->stubCalls.call(jit, isLeaf ? Stub::LeafDidCreateTypedField : Stub::DidCreateTypedField, CallSite { });
            });
        } else
            callICStub(Stub::DidCreateTypedField, Void, { { cell.value(), GPRInfo::argumentGPR1 }, { cache, GPRInfo::argumentGPR3 } }, std::pair { GPRInfo::argumentGPR2, slotKey }, effects);
    };
    if (m_context.sites.size() >= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
        throughStub(m_out.constIntPtr(0));
        ensureStorage();
        return;
    }
    m_context.sites.append(Site { });
    Pointer cache = slotAddress(m_context.sites.size() - 1);
    auto inCache = [&](size_t offset) {
        // The stub writes it, like any call.
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    Word32 isHit(m_out.equal(words, m_out.load64(inCache(OBJECT_OFFSETOF(AOT::Slot, pointer)))));
    m_out.branch(isHit.value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    m_out.store64(m_out.bitXor(words, m_out.zeroExtPtr(m_out.load32(inCache(OBJECT_OFFSETOF(AOT::Slot, offset))))), cell.value(), m_heaps.AOTTypedObject_orderWord);
    m_out.jump(continuation);
    m_out.appendTo(miss);
    throughStub(cache.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    ensureStorage();
}

bool Lowering::isOnlyComparedWithTextsBeforeAnyStore(ValueID read, Vector<ValueID, 4>& textsOfIndex)
{
    auto mayStoreToField = [&](const Block& block, size_t index) {
        auto& value = m_function.at(block.values[index]);
        switch (value.opcode) {
        case Opcode::CallDirect:
            return m_program.at(value.function()).writes >= Writes::Fields;
        // Also to an object that this run of the function has made, which is nothing to its callers.
        case Opcode::PutById:
        case Opcode::PutField:
        case Opcode::PutByVal:
        case Opcode::DeleteById:
        case Opcode::DeleteByVal:
            return true;
        default:
            return writesOf(m_program, m_function, block, index) >= Writes::Fields;
        }
    };
    // By block: it is reached from the read without coming by the read again; on some such way a field may have been stored to before it is entered.
    size_t blockOfRead = m_currentBlock - m_function.blocks.span().data();
    BitVector isReached(m_function.blocks.size());
    BitVector isEnteredAfterStore(m_function.blocks.size());
    Vector<uint32_t, 16> worklist;
    auto leave = [&](const Block& block, bool afterStore) {
        for (auto& target : block.terminator.targets) {
            uint32_t next = target.block.index();
            if (next == blockOfRead || (isReached.get(next) && (!afterStore || isEnteredAfterStore.get(next))))
                continue;
            isReached.set(next);
            if (afterStore)
                isEnteredAfterStore.set(next);
            worklist.append(next);
        }
    };
    auto storesIn = [&](const Block& block, size_t from, size_t to) {
        for (size_t index = from; index < to; ++index) {
            if (mayStoreToField(block, index))
                return true;
        }
        return false;
    };
    leave(*m_currentBlock, storesIn(*m_currentBlock, m_currentIndexInBlock + 1, m_currentBlock->values.size()));
    while (!worklist.isEmpty()) {
        uint32_t next = worklist.takeLast();
        auto& block = m_function.blocks[next];
        leave(block, isEnteredAfterStore.get(next) || storesIn(block, 0, block.values.size()));
    }
    Vector<ValueID, 4> same { read };
    auto countComparisons = [&]() -> std::optional<unsigned> {
        unsigned numberOfComparisons = 0;
        textsOfIndex.clear();
        for (size_t number = 0; number < m_function.blocks.size(); ++number) {
            auto& block = m_function.blocks[number];
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = m_function.at(block.values[index]);
                if (value.opcode != Opcode::StrictEq || (!same.contains(value.child(0)) && !same.contains(value.child(1))))
                    continue;
                ValueID text = value.child(same.contains(value.child(0)));
                if (m_function.at(text).opcode != Opcode::String)
                    return std::nullopt;
                if (number == blockOfRead ? storesIn(block, m_currentIndexInBlock + 1, index) : isEnteredAfterStore.get(number) || storesIn(block, 0, index))
                    return std::nullopt;
                ++numberOfComparisons;
                bool isKnown = std::ranges::any_of(textsOfIndex, [&](ValueID known) {
                    return m_function.at(known).string() == m_function.at(text).string();
                });
                if (!isKnown)
                    textsOfIndex.append(text);
            }
        }
        return numberOfComparisons;
    };
    auto ofRead = countComparisons();
    if (!ofRead)
        return false;
    unsigned numberOfUses = numberOfUsesOf(read);
    if (*ofRead == numberOfUses) {
        if (*ofRead <= textsOfIndex.size() || *ofRead < textsOfIndex.size() * textsOfIndex.size())
            textsOfIndex.clear();
        return numberOfUses;
    }
    for (size_t known = 0; known != same.size();) {
        known = same.size();
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && same.contains(value.child(0)) && !same.contains(id))
                    same.append(id);
            }
        }
    }
    for (size_t i = 1; i < same.size(); ++i)
        numberOfUses += numberOfUsesOf(same[i]);
    if (same.size() == 1)
        return false;
    auto ofAll = countComparisons();
    return ofAll && *ofAll && *ofAll + same.size() - 1 == numberOfUses;
}

TypedPointer Lowering::guardOfPinnedFields(SlotID slot)
{
    return m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfTypedAlteredLiterals() + slot.index() % numberOfGuardsOfPinnedFields);
}

Word32 Lowering::hasShapePinnedTo(const PinnedRead& read, StringID text)
{
    Vector<LayoutID, 1> withText;
    for (LayoutID member : m_program.at(read.view).members) {
        auto& layout = m_program.at(member);
        auto& field = layout.fields[*layout.fieldNamed(read.name)];
        RELEASE_ASSERT(field.isPinned);
        if (field.literals[0].string == text)
            withText.append(member);
    }
    return withText.isEmpty() ? boolean(false) : passesOneOf(read.shapeWord, withText);
}

Word32 Lowering::isPinnedTo(const PinnedRead& read, ValueID constant)
{
    StringID text = m_function.at(constant).string();
    if (read.index) {
        size_t which = read.texts.find(text);
        RELEASE_ASSERT(which != notFound);
        return equal(read.index, word32(which));
    }
    return either<Word32>(isZero(read.isAltered), [&] {
        return hasShapePinnedTo(read, text);
    }, [&] {
        // Nothing has been stored to a field since the read: what the field holds now is what it held then.
        auto& view = m_program.at(read.view);
        auto& field = view.fields[*view.fieldNamed(read.name)];
        auto ascii = constantASCIIText(constant);
        auto& stored = m_program.at(field.stored);
        if (stored.primitives & Primitive::String && stored.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull) && !field.isLackedBySomeMember && !field.isElsewhereInSomeMember && ascii && !ascii->empty()) {
            Operand held = readField(read.cell, field, stored);
            if (held.rep == Rep::Cell)
                return isTextByStub(held.as<CellPointer>(), constant);
            Boxed value = held.as<Boxed>();
            return either<Word32>(isCell(value, stored), [&] {
                return isTextByStub(asCell(value), constant);
            }, [&] {
                return boolean(false);
            });
        }
        return Word32(m_out.castToInt32(AOT_TYPED_OPERATION(FieldHasText, read.cell, word32(read.name.index()), word32(text.index())).value()));
    });
}

void Lowering::guardPinnedFields(LayoutID holder, const Field& field, ValueID stored)
{
    if (!field.slot || !m_program.at(field.slot).hasPinnedFields)
        return;
    if (ValueID constant = m_program.constantOf(m_function, stored))
        stored = constant;
    // The string that every pinned field it can get to is pinned to.
    bool keepsThem = holder && m_function.at(stored).opcode == Opcode::String;
    if (keepsThem) {
        for (LayoutID member : m_program.at(holder).members) {
            auto& layout = m_program.at(member);
            auto own = layout.fieldNamed(field.name);
            if (own && layout.fields[*own].slot == field.slot && layout.fields[*own].isPinned)
                keepsThem &= layout.fields[*own].literals[0].string == m_function.at(stored).string();
        }
    }
    if (!keepsThem)
        m_out.store32As8(m_out.int32One, guardOfPinnedFields(field.slot));
}

void Lowering::writeField(CellPointer cell, ValueID object, const Field& field, ValueID stored)
{
    guardPinnedFields(isLoweringInitializingStore() ? m_function.at(object).layout() : typeOf(object).onlyView(), field, stored);
    auto put = [&] {
        if (field.store == Store::Int32OrElsewhere)
            storeInt32OrElsewhere(cell, field, stored, isLoweringInitializingStore());
        else
            store(field.store, in(stored, repOf(field.store)), fieldAt(cell, field));
    };
    SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
    // Nothing has happened to the object yet. The collector has not run, or every field is fit to be looked at: see lowerNewObject().
    bool isInitializing = isLoweringInitializingStore();
    if (isInitializing && isBornWith(m_program.allocations[m_function.at(object).allocation()], field.name)) {
        if (isStoreThatNeedsNoStorage(field, stored)) {
            didLowerInitializingStore();
            return;
        }
        didEmitWrite(Writes::RuntimeState);
        put();
        if (holdsCells(field.store) && typeOf(stored).hasCells() && collectorMayHaveRunSinceBirth(object))
            barrier(cell);
        didLowerInitializingStore();
        return;
    }
    didEmitWrite(Writes::Fields);
    // The layout was chosen from the discriminant, so once it is set it keeps its value: a constructor function may be run on an object
    // that is not new. The same value again is no store. Any other is for the run time to refuse.
    bool keepsDiscriminant = !isInitializing && m_program.at(field.slot).isDiscriminant;
    bool mayBeAbsent = m_program.at(field.stored).primitives & Primitive::Absent;
    LayoutID view = typeOf(object).onlyView();
    // [[Set]] asks the prototype chain before it creates a property: there may be a setter, or a property that is not writable.
    bool mayAsk = mayBeAbsent && storeMayAskPrototypeChain(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, view, field.name);
    bool asksPrototypeChain = mayAsk && inheritsByItsLayout(m_program, view, field.name);
    // The object says so: it has another prototype than its layout says, see mayInherit(). Or it takes no more properties.
    bool mayAskPrototypeChain = mayAsk && !asksPrototypeChain;
    bool mayRefuse = mayBeAbsent && view && !asksPrototypeChain && mayBeFrozen(view) && !isPrivateName(m_program.string(field.name));
    bool mayExistAlready = field.flags & fieldMayExistWithoutStorage;
    LBasicBlock done = keepsDiscriminant || asksPrototypeChain || mayAskPrototypeChain || (mayRefuse && (isInSloppyCode() || mayExistAlready)) ? m_out.newBlock() : nullptr;
    auto keepDiscriminant = [&] {
        LBasicBlock differs = newColdBlock();
        Boxed current = readField(cell, field, m_program.at(m_program.types.anything)).as<Boxed>();
        m_out.branch(equal(current, in<Boxed>(stored)).value(), usually(done), rarely(differs));
        m_out.appendTo(differs);
        OperationEffects effects { Writes::Fields, true, storeMayMakeObjectGiveUp(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, view) };
        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(field.name.index()), in<Boxed>(stored));
        m_out.jump(done);
    };
    if (keepsDiscriminant && !mayBeAbsent) {
        keepDiscriminant();
        m_out.appendTo(done);
        return;
    }
    // A store to a field that is absent creates the property, after every other key of the object: the run time writes that down.
    if (mayBeAbsent) {
        LBasicBlock creates = field.flags & fieldIsSetAtCreation ? newUnlikelyBlock() : m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LBasicBlock present = keepsDiscriminant ? m_out.newBlock() : continuation;
        m_out.branch(isFieldAbsent(cell, field, 0).value(), unsure(creates), unsure(present));
        if (keepsDiscriminant) {
            m_out.appendTo(present);
            keepDiscriminant();
        }
        m_out.appendTo(creates);
        if (asksPrototypeChain) {
            setByName(box(cell), field.name, in<Boxed>(stored));
            m_out.jump(done);
        } else {
            auto unlessAsked = [&] {
                if (!mayAskPrototypeChain && !mayRefuse)
                    return;
                LBasicBlock asks = newColdBlock();
                LBasicBlock goesOn = newColdBlock();
                uint32_t bits = (mayAskPrototypeChain ? orderWordHasExtraProperties : 0) | (mayRefuse ? orderWordIsNotExtensible : 0);
                m_out.branch(lacksBits(orderWordOf(cell), bits).value(), usually(goesOn), rarely(asks));
                m_out.appendTo(asks);
                if (mayAskPrototypeChain) {
                    setByName(box(cell), field.name, in<Boxed>(stored));
                    m_out.jump(done);
                } else if (mayExistAlready) {
                    OperationEffects effects { Writes::Fields, true, storeMayMakeObjectGiveUp(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, view) };
                    if (isInSloppyCode())
                        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByNameSloppy, effects, box(cell), word32(field.name.index()), in<Boxed>(stored));
                    else
                        AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(field.name.index()), in<Boxed>(stored));
                    m_out.jump(done);
                } else if (isInSloppyCode())
                    m_out.jump(done);
                else {
                    // The object turns the store away.
                    OperationEffects refusal { Writes::Nothing, true };
                    AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, refusal, box(cell), word32(field.name.index()), in<Boxed>(stored));
                    m_out.unreachable();
                }
                m_out.appendTo(goesOn);
            };
            OperationEffects effects { Writes::Fields, false, false, true };
            // Neither the stub nor the site counts anything.
            if (Options::countAOTTypedAllocations()) {
                unlessAsked();
                AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(field.name.index()));
            } else
                didCreateFieldInCell(cell, field, effects, unlessAsked);
            m_out.jump(continuation);
        }
        m_out.appendTo(continuation);
    }
    put();
    if (field.isOutOfLine)
        ensureStillAliveHere(cell);
    if (holdsCells(field.store) && typeOf(stored).hasCells())
        barrier(cell);
    if (isInitializing)
        didLowerInitializingStore();
    if (done) {
        m_out.jump(done);
        m_out.appendTo(done);
    }
}

// Accesses by name to a receiver that is expected to be an object of a type of the program. In place: the object has the field, and
// something is in it. That is its own property, whoever asks. For all else the object itself is asked, the general way.

// The field such an access comes down to. Null: there is none, or no object has it.
const Field* Lowering::fieldExpectedFor(const Type& expected, StringID name) const
{
    LayoutID view = expected.onlyView();
    if (!view || !isObjectLayout(m_program.at(view).kind) || m_program.at(view).test.passesNothing())
        return nullptr;
    const Field* field = fieldFor(expected, name);
    if (!field || field->isOutOfLine || (field->isLackedBySomeMember && field->membersWithIt.passesNothing()))
        return nullptr;
    return field;
}

// The receiver has been found to be of the type of the view.
CellPointer Lowering::withFieldOrElse(ValueID object, LayoutID view, const Field& field, LBasicBlock otherwise, bool mayBeAbsent)
{
    CellPointer cell = get(object).as<CellPointer>();
    if (field.isLackedBySomeMember)
        orElse(passes(shapeWordOf(cell), field.membersWithIt, otherwise), otherwise);
    else if (m_program.at(view).admitsForeignObjects)
        orElse(passes(shapeWordOf(cell), m_program.at(view).test, otherwise), otherwise);
    if (!mayBeAbsent && m_program.at(field.stored).primitives & Primitive::Absent)
        orElse(hasField(cell, field), otherwise);
    return cell;
}

bool Lowering::hasMemberWithField(LayoutID view, StringID name) const
{
    for (LayoutID member : m_program.at(view).members) {
        if (m_program.at(member).fieldNamed(name))
            return true;
    }
    return false;
}

// Whether an object turns away every store to the field that was not compiled with its type, whatever is stored.
bool Lowering::hasMemberThatKeepsField(LayoutID view, StringID name) const
{
    for (LayoutID member : m_program.at(view).members) {
        auto& layout = m_program.at(member);
        if (auto field = layout.fieldNamed(name); field && layout.fields[*field].flags & fieldIsImmutable)
            return true;
    }
    return false;
}

static bool isUnescapedAllocation(const Program& program, const Function& function, const Block& block, size_t index, ValueID allocation)
{
    if (function.at(allocation).opcode != Opcode::NewObjectWithShape)
        return false;
    auto& layout = program.at(function.at(allocation).layout());
    if (!layout.mayGiveUp)
        return true;
    auto isAllocation = [&](ValueID id) {
        while (function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        return id == allocation;
    };
    auto doesNotEscape = [&](const Value& value) {
        switch (value.opcode) {
        case Opcode::Cast:
            return value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce;
        case Opcode::GetField:
            return true;
        case Opcode::GetById:
        case Opcode::InById:
            return layout.fieldNamed(value.string()) && !mayInheritAtBirth(program, function, allocation, value.string());
        case Opcode::PutField:
            return !isAllocation(value.child(1));
        case Opcode::PutById: {
            auto field = layout.fieldNamed(value.string());
            if (!field || *field == layout.discriminant || isAllocation(value.child(1)) || mayInheritAtBirth(program, function, allocation, value.string()))
                return false;
            TypeID type = layout.fields[*field].type;
            return !type || program.typeOf(function, value.child(1)).isWithin(program.at(type), program);
        }
        default:
            return false;
        }
    };
    for (size_t i = index; i--;) {
        if (block.values[i] == allocation)
            return true;
        auto& value = function.at(block.values[i]);
        bool usesAllocation = false;
        value.forEachChild([&](ValueID child) {
            usesAllocation |= isAllocation(child);
        });
        if (usesAllocation && !doesNotEscape(value))
            return false;
    }
    return false;
}

// Whether this function has allocated the object, with a member of the view: then it is not for a test to tell it from a foreign one.
// A cast leaves such an object where it is, and what it may turn it into is a member of every view that its layout is a member of.
bool Lowering::isAllocatedAsMemberOf(ValueID object, LayoutID view) const
{
    while (m_function.at(object).opcode == Opcode::Cast)
        object = m_function.at(object).child(0);
    auto& made = m_function.at(object);
    return made.opcode == Opcode::NewObjectWithShape && std::ranges::binary_search(m_program.at(view).members, made.layout()) && isUnescapedAllocation(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, object);
}

// GetField and PutField name a field of the layout the object was allocated with.

static ValueID withoutCasts(const Function& function, ValueID id)
{
    while (function.at(id).opcode == Opcode::Cast)
        id = function.at(id).child(0);
    return id;
}

Operand Lowering::lowerGetField(ValueID result, const Value& value)
{
    RELEASE_ASSERT_WITH_MESSAGE(!mayGiveUp(value.layout()) || isUnescapedAllocation(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, withoutCasts(m_function, value.child(0))), "AOT: GetField on an allocation that may have given up its layout");
    return readField(in<CellPointer>(value.child(0)), m_program.at(value.layout()).fields[value.field()], typeOf(result));
}

void Lowering::lowerPutField(ValueID, const Value& value)
{
    RELEASE_ASSERT_WITH_MESSAGE(!mayGiveUp(value.layout()) || isUnescapedAllocation(m_program, m_function, *m_currentBlock, m_currentIndexInBlock, withoutCasts(m_function, value.child(0))), "AOT: PutField on an allocation that may have given up its layout");
    writeField(in<CellPointer>(value.child(0)), value.child(0), m_program.at(value.layout()).fields[value.field()], value.child(1));
}

// Whether of each of the classes there is one class object, and so one closure of each of its methods.
static bool areEvaluatedOnce(const Program& program, const Vector<LayoutID>& classes)
{
    for (LayoutID id : classes) {
        if (!program.at(id).isEvaluatedOnce)
            return false;
    }
    return true;
}

Operand Lowering::lowerGetById(ValueID result, const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    // The casts behind it are carried out in its arms, where the last of them is lowered: see lowerFusedCast().
    if (!m_fusedCasts && (numberOfCastsFusedWithRead(m_currentIndexInBlock) || indexOfUseFusedWithRead(m_currentIndexInBlock, 0)))
        return placeholder(value.rep);
    auto& type = m_fusedCasts ? m_fusedCasts->type : typeOf(result);
    if (value.isAtKnownPlace())
        return ensure(getAtKnownPlace(value, in<Boxed>(object)), type);
    LayoutID view = receiver.onlyView();
    const Field* field = fieldFor(receiver, name);
    if (!field) {
        // A method of a class that is evaluated once is one closure, and stays the method. The object may have a property of its own by
        // the name: the method, bound to it.
        if (auto targets = view ? methodTargets(view, name) : std::nullopt; targets && targets->size() == 1 && targets->first().function && m_program.genericEntrypoints.contains(targets->first().function.index()) && areEvaluatedOnce(m_program, m_program.at(view).members)) {
            CellPointer cell = nonNil(object, name);
            auto askObject = [&] {
                return ensure(emitGetById(box(cell), name), type);
            };
            return ifOfMember(cell, object, view, repOf(type), [&] {
                return eitherOperand(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), repOf(type), [&] {
                    return ensure(closureConstant(targets->first().function), m_program.at(m_program.types.function), type);
                }, [&] {
                    return askMarkedObject(cell, name, type);
                });
            }, askObject);
        }
        if (view && onlyExtraPropertiesSupply(m_program, view, name)) {
            CellPointer cell = nonNil(object, name);
            return eitherOperand(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), joinedAs(type), [&] {
                return ensure(undefined(), m_program.at(m_program.types.undefined), type);
            }, [&] {
                return askMarkedObject(cell, name, type);
            });
        }
        auto generally = [&] {
            if (auto property = getPropertyOfGlobalThis(object, name))
                return ensure(*property, type);
            return ensure(emitGetById(in<Boxed>(object), name, false, functionDefining(object)), type);
        };
        if (static const BuiltinName length = Builtins::singleton().nameOf("length"_s.span8()); length && m_program.builtinName(name) == length && receiver == Type::function()) {
            CellPointer closure = get(object).rep == Rep::JSValue ? asCell(in<Boxed>(object)) : in<CellPointer>(object);
            return eitherOperand(isUnreifiedSlimClosure(closure), repOf(type), [&] {
                LValue data = fixedPointer(Instance::offsetOfTypedData());
                LValue lengths = m_out.add(data, m_out.zeroExtPtr(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, data, OBJECT_OFFSETOF(Data, functionLengthsOffset))), IsFixed)));
                LValue function = m_out.zeroExtPtr(functionOfClosure(closure, true).value());
                if (m_program.functionLengthsAreBytes)
                    return ensure(Word32(marked(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, m_out.add(lengths, function), 0)), IsFixed)), Type::integer({ 0, UINT8_MAX }), type);
                LValue address = m_out.add(lengths, m_out.shl(function, m_out.constInt32(1)));
                return ensure(Word32(marked(m_out.load16ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, address, 0)), IsFixed)), Type::integer({ 0, UINT16_MAX }), type);
            }, generally);
        }
        if (receiver == Type::function() && equalSpans(m_program.string(name), "name"_s.span8())) {
            CellPointer closure = get(object).rep == Rep::JSValue ? asCell(in<Boxed>(object)) : in<CellPointer>(object);
            return eitherOperand(isUnreifiedSlimClosure(closure), repOf(type), [&] {
                LValue data = fixedPointer(Instance::offsetOfTypedData());
                LValue names = m_out.add(data, m_out.zeroExtPtr(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, data, OBJECT_OFFSETOF(Data, functionsOffset))), IsFixed)));
                LValue address = m_out.add(names, m_out.shl(m_out.zeroExtPtr(functionOfClosure(closure, true).value()), m_out.constInt32(2)));
                LValue key = m_out.zeroExtPtr(m_out.add(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, address, 0)), IsFixed), m_out.constInt32(1)));
                return ensure(CellPointer(callAllocationStub(Stub::TypedString, { { key, stubTemporaryGPRs[0] } }, { Writes::RuntimeState, false }, false)), m_program.at(m_program.types.string), type);
            }, generally);
        }
        if (!m_isInLoop && !m_program.isHotByText(m_id))
            return generally();
        // A primitive that the receiver is expected to be besides takes the general way.
        if (auto expectedObject = predictionOf(object, Primitive::All)) {
            if (const Field* expectedField = fieldExpectedFor(*expectedObject, name)) {
                return ifAsExpected({ { object, &*expectedObject } }, repOf(type), [&](LBasicBlock otherwise) {
                    return readField(withFieldOrElse(object, expectedObject->onlyView(), *expectedField, otherwise), *expectedField, type);
                }, generally);
            }
        }
        auto expected = predictionOf(object);
        if (!expected)
            return generally();
        if (auto builtin = expectedBuiltin(BuiltinKind::Getter, object, name, { }))
            return lowerExpectedBuiltin(result, *builtin, std::nullopt, generally);
        // A method: what a string or a number finds under a name cannot change.
        if (BuiltinName known = m_program.builtinName(name); known && (expected->isString() || expected->isNumber())) {
            BuiltinID method = m_builtins.findForInstance(BuiltinKind::Method, expected->isString() ? m_builtins.stringClass() : m_builtins.numberClass(), known);
            if (method && m_builtins.hasCell(method)) {
                return ifAsExpected({ { object, &m_program.at(expected->isString() ? m_program.types.string : m_program.types.number) } }, repOf(type), [&](LBasicBlock) {
                    return ensure(builtinCell(m_builtins.cellOf(method)), m_program.at(m_program.types.function), type);
                }, generally);
            }
        }
        return generally();
    }
    CellPointer cell = nonNil(object, name);
    // The shape says what it holds, unless the byte is set: see lowerStrictEq(). There is no value.
    Vector<ValueID, 4> textsOfIndex;
    if (field->isPinnedInEveryMember && !m_fusedCasts && isOnlyComparedWithTextsBeforeAnyStore(result, textsOfIndex)) {
        PinnedRead read { cell, shapeWordOf(cell), Word32(m_out.load8ZeroExt32(guardOfPinnedFields(field->slot))), view, name, { }, { } };
        if (!textsOfIndex.isEmpty()) {
            auto indexBy = [&](auto holds) {
                LValue index = m_out.constInt32(textsOfIndex.size());
                for (size_t i = textsOfIndex.size(); i--;)
                    index = m_out.select(holds(textsOfIndex[i]).value(), m_out.constInt32(i), index);
                return Word32(index);
            };
            Word32 index = either<Word32>(isZero(read.isAltered), [&] {
                return indexBy([&](ValueID text) {
                    return hasShapePinnedTo(read, m_function.at(text).string());
                });
            }, [&] {
                return indexBy([&](ValueID text) {
                    return isPinnedTo(read, text);
                });
            });
            read.index = index;
            for (ValueID text : textsOfIndex)
                read.texts.append(m_function.at(text).string());
        }
        m_pinnedReads.set(result.index(), WTF::move(read));
        return placeholder(repOf(type));
    }
    auto has = [&] {
        return readProperty(cell, view, *field, type);
    };
    auto lacks = [&] {
        bool suppliesAlone = onlyFieldsSupply(m_program, view, name);
        if (!suppliesAlone && !onlyFieldsSupply(m_program, view, name, true))
            return ensureProperty(cell, name, emitGetById(box(cell), name), type);
        if (field->isElsewhereInSomeMember) {
            auto ask = [&] {
                return ensure(AOT_TYPED_OPERATION(GetExtra, cell, word32(name.index() | nameMayBeOfOwnField)), type);
            };
            return byOwnFieldOfMember(cell, view, *field, joinedAs(type), [&](LayoutID, const Field& own) {
                if (!(m_program.at(own.stored).primitives & Primitive::Absent))
                    return readField(cell, own, type);
                return eitherOperand(hasField(cell, own), joinedAs(type), [&] {
                    return readField(cell, own, type);
                }, ask);
            }, ask);
        }
        return eitherOperand(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), joinedAs(type), [&] {
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }, [&] {
            if (!suppliesAlone)
                return askMarkedObject(cell, name, type);
            return ensure(AOT_TYPED_OPERATION(GetExtra, cell, word32(name.index())), type);
        });
    };
    auto sunk = sunkCoercionAt(m_currentIndexInBlock);
    if (!sunk)
        return throughView(cell, object, view, *field, joinedAs(type), has, lacks);
    // The written cast the receiver comes from has left its work to this read: an object that has the field is none that the cast makes anything of. One that it does make something of may
    // have the field afterwards, with what was an extra property in it.
    RELEASE_ASSERT(field->isLackedBySomeMember && !field->membersWithIt.passesNothing());
    bool mostlyLacksIt = mostMembersLack(view, *field);
    LBasicBlock hasIt = mostlyLacksIt ? newUnlikelyBlock() : m_out.newBlock();
    LBasicBlock mayLackIt = mostlyLacksIt ? m_out.newBlock() : newUnlikelyBlock();
    LBasicBlock lacksIt = m_out.newBlock();
    LBasicBlock isSource = newColdBlock();
    OperandJoin join(*this, joinedAs(type));
    m_out.branch(passes(shapeWordOf(cell), field->membersWithIt, mayLackIt).value(), unsure(hasIt), unsure(mayLackIt));
    m_out.appendTo(mayLackIt);
    branchOnTransitionSource(cell, *sunk, rarely(isSource), usually(lacksIt));
    // Only the test stands twice.
    m_out.appendTo(isSource);
    carryOut(cell, *sunk);
    m_out.branch(passes(shapeWordOf(cell), field->membersWithIt, lacksIt).value(), unsure(hasIt), unsure(lacksIt));
    m_out.appendTo(hasIt);
    join.add(has());
    m_out.appendTo(lacksIt);
    join.add(lacks());
    return join.finish();
}

std::optional<Lowering::InitialStores> Lowering::initialStoresAt(size_t index) const
{
    auto& values = m_currentBlock->values;
    auto& store = m_function.at(values[index]);
    if (!store.isInitialStore() || m_isLoweringInitialStoresOneByOne)
        return std::nullopt;
    auto& constructor = store.inlineFrame ? m_program.at(m_function.inlineFrames[store.inlineFrame].callee) : m_function;
    // The rest of the list goes the general way.
    if (store.initialStoreIndex() >= constructor.numberOfInitialStores)
        return std::nullopt;
    // The verifier has seen to it that they are all there.
    RELEASE_ASSERT(store.initialStoreIndex() <= index);
    InitialStores result { index - store.initialStoreIndex(), constructor.numberOfInitialStores, constructor.orderWordAfterInitialStores, &m_program.at(constructor.thisType) };
    RELEASE_ASSERT(result.first + result.count <= values.size());
    for (unsigned i = 0; i < result.count; ++i) {
        auto& other = m_function.at(values[result.first + i]);
        RELEASE_ASSERT(other.opcode == Opcode::PutById && other.initialStoreIndex() == i && other.inlineFrame == store.inlineFrame && other.child(0) == store.child(0));
    }
    // Then the allocation may be born with the fields: it says so itself. What the stores look like now and what they looked like when it was given its Allocation are two things.
    if (isInitializingStore(m_program, m_function, *m_currentBlock, result.first)) {
        auto& allocation = m_program.allocations[m_function.at(store.child(0)).allocation()];
        bool isBornWithAll = true;
        for (unsigned i = 0; i < result.count; ++i)
            isBornWithAll &= isBornWith(allocation, m_function.at(values[result.first + i]).string());
        if (isBornWithAll)
            return std::nullopt;
    }
    return result;
}

// The first stores of a constructor function, or of a copy of one. As a rule the object is fresh: nothing has happened to it since it was allocated with no property. Then these
// are plain stores, and one more says that the properties exist and in which order. A slot that has no way to say "absent" relies on that: nothing else counts its
// field among the keys. Any other object (`F.call(old)`) is stored to as by any store.
void Lowering::lowerInitialStores(const InitialStores& stores)
{
    auto& values = m_currentBlock->values;
    ValueID object = m_function.at(values[stores.first]).child(0);
    CellPointer cell = nonNil(object, m_function.at(values[stores.first]).string());
    LBasicBlock isFresh = m_out.newBlock();
    LBasicBlock isNotFresh = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    if (LayoutID view = stores.typeOfThis->onlyView(); m_program.at(view).admitsForeignObjects && !isAllocatedAsMemberOf(object, view))
        orElse(passes(shapeWordOf(cell), m_program.at(view).test, isNotFresh), isNotFresh);
    // No property, no flag, and not keysAreInLayoutOrder: an allocation that is born with nothing has the first order of its layout.
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    m_out.branch(isZero(Word32(m_out.castToInt32(words))).value(), usually(isFresh), rarely(isNotFresh));

    m_out.appendTo(isFresh);
    {
        SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
        didEmitWrite(Writes::Fields);
        bool storesCells = false;
        for (unsigned i = 0; i < stores.count; ++i) {
            SetForScope currentIndex(m_currentIndexInBlock, stores.first + i);
            auto& value = m_function.at(values[stores.first + i]);
            const Field* field = fieldFor(*stores.typeOfThis, value.string());
            RELEASE_ASSERT(field && !field->isLackedBySomeMember);
            bool needsNoStorage = isStoreThatNeedsNoStorage(*field, value.child(1));
            guardPinnedFields(stores.typeOfThis->onlyView(), *field, value.child(1));
            // So that the counters see the field. It comes to the same order word.
            if (Options::countAOTTypedAllocations()) [[unlikely]] {
                OperationEffects effects { Writes::Fields, false, false, true };
                AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(value.string().index() | (needsNoStorage ? nameOfFieldCreatedWithoutStorage : 0)));
            }
            if (needsNoStorage)
                continue;
            if (field->store == Store::Int32OrElsewhere)
                storeInt32OrElsewhere(cell, *field, value.child(1), true);
            else
                store(field->store, in(value.child(1), repOf(field->store)), fieldAt(cell, *field));
            storesCells |= holdsCells(field->store) && typeOf(value.child(1)).hasCells();
        }
        if (!Options::countAOTTypedAllocations()) [[likely]]
            m_out.store64(m_out.bitOr(words, m_out.constInt64(stores.orderWord)), cell.value(), m_heaps.AOTTypedObject_orderWord);
        if (storesCells)
            barrier(cell);
    }
    m_out.jump(continuation);

    m_out.appendTo(isNotFresh);
    {
        SetForScope oneByOne(m_isLoweringInitialStoresOneByOne, true);
        m_out.setFrequency(0);
        for (unsigned i = 0; i < stores.count; ++i) {
            SetForScope currentIndex(m_currentIndexInBlock, stores.first + i);
            lowerPutById(values[stores.first + i], m_function.at(values[stores.first + i]));
        }
        m_out.setFrequency(1);
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

bool Lowering::isAccessorDefinition(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Operation && (implementation.operation == LanguageOperation::DefineGetter || implementation.operation == LanguageOperation::DefineSetter);
}

Lowering::AccessorGroup* Lowering::accessorGroupAt(size_t index)
{
    // Four words of the frame each.
    static constexpr unsigned maxMembers = 64;
    if (m_blockOfAccessorGroups != m_currentBlock) {
        m_blockOfAccessorGroups = m_currentBlock;
        m_accessorGroups.clear();
        auto& values = m_currentBlock->values;
        AccessorGroup open;
        auto close = [&] {
            if (open.members.size() >= 2)
                m_accessorGroups.append(open);
            open.members.clear();
        };
        auto mayBeMember = [&](const Value& value) {
            if (value.arguments.size() != 5 || typeOf(value.arguments[0]).primitives || !typeOf(value.arguments[0]).hasObjects())
                return false;
            auto& key = m_function.at(value.arguments[1]);
            auto& position = m_function.at(value.arguments[4]);
            return key.opcode == Opcode::String && !isPrivateName(m_program.string(key.string())) && m_function.at(value.arguments[3]).opcode == Opcode::Boolean
                && position.opcode == Opcode::Integer && position.integer() >= 0 && position.integer() <= std::numeric_limits<int32_t>::max();
        };
        for (size_t i = 0; i < values.size(); ++i) {
            auto& value = m_function.at(values[i]);
            if (isAccessorDefinition(value)) {
                if (!mayBeMember(value)) {
                    close();
                    continue;
                }
                if (!open.members.isEmpty() && (open.members.size() == maxMembers || m_function.at(values[open.members[0]]).arguments[0] != value.arguments[0]))
                    close();
                open.members.append(i);
                continue;
            }
            bool maySeparate = value.opcode == Opcode::String || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::NewClosure || value.opcode == Opcode::FunctionReference
                || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast);
            if (!maySeparate)
                close();
        }
        close();
    }
    for (auto& group : m_accessorGroups) {
        if (group.members.contains(index))
            return &group;
    }
    return nullptr;
}

// See operationAOTTypedDefineAccessors for the four words.
void Lowering::lowerMemberOfAccessorGroup(AccessorGroup& group, const Value& value)
{
    unsigned count = group.members.size();
    if (!group.buffer)
        group.buffer = m_out.lockedStackSlot(4 * count * sizeof(EncodedJSValue));
    size_t place = group.members.find(m_currentIndexInBlock);
    auto store = [&](unsigned word, Boxed boxed) {
        m_out.store64(boxed.value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, group.buffer, (4 * place + word) * sizeof(EncodedJSValue)));
    };
    bool isGetter = m_builtins.at(value.builtin()).implementation.operation == LanguageOperation::DefineGetter;
    store(0, boxedConstant(jsNumber(static_cast<int32_t>(m_function.at(value.arguments[1]).string().index()))));
    store(1, in<Boxed>(value.arguments[2]));
    store(2, boxedConstant(jsNumber(static_cast<int32_t>(isGetter | m_function.at(value.arguments[3]).boolean() << 1))));
    store(3, boxedConstant(jsNumber(static_cast<int32_t>(m_function.at(value.arguments[4]).integer()))));
    if (place + 1 == count)
        AOT_TYPED_OPERATION_WITH_EFFECTS(DefineAccessors, effectsOfBuiltinCall(value), in<CellPointer>(value.arguments[0]), word32(count), Pointer(group.buffer));
}

bool Lowering::isDefineField(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::DefineField;
}

bool Lowering::isObjectCreateNull(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 1 || m_function.at(value.arguments[0]).opcode != Opcode::Null)
        return false;
    auto& builtin = m_builtins.at(value.builtin());
    return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "create"_s;
}

bool Lowering::isDefineProperty(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 3)
        return false;
    auto& builtin = m_builtins.at(value.builtin());
    return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "defineProperty"_s;
}

// The index of the Object.defineProperty() that the literal is the descriptor of, if that and its own members are all that use it, and if it says at compile time what ToPropertyDescriptor() makes of it.
std::optional<size_t> Lowering::definitionDescribedBy(const ObjectLiteral& literal)
{
    auto& values = m_currentBlock->values;
    if (literal.hasNullPrototype || literal.newTarget || literal.fields.isEmpty())
        return std::nullopt;
    ValueID id = values[literal.allocation];
    bool isData = false;
    bool isAccessor = false;
    for (size_t index : literal.fields) {
        auto& field = m_function.at(values[index]);
        if (field.opcode == Opcode::PutById)
            return std::nullopt;
        auto name = m_program.string(m_function.at(field.arguments[1]).string());
        bool isFlag = equalSpans(name, "writable"_s.span8()) || equalSpans(name, "enumerable"_s.span8()) || equalSpans(name, "configurable"_s.span8());
        if (isFlag && m_function.at(field.arguments[2]).opcode != Opcode::Boolean)
            return std::nullopt;
        isData |= equalSpans(name, "value"_s.span8()) || equalSpans(name, "writable"_s.span8());
        isAccessor |= equalSpans(name, "get"_s.span8()) || equalSpans(name, "set"_s.span8());
        if (!isFlag && !equalSpans(name, "value"_s.span8()) && !equalSpans(name, "get"_s.span8()) && !equalSpans(name, "set"_s.span8()))
            return std::nullopt;
    }
    // That throws.
    if (isData && isAccessor)
        return std::nullopt;
    for (size_t k = literal.fields.last() + 1; k < values.size(); ++k) {
        auto& value = m_function.at(values[k]);
        if (value.opcode == Opcode::Undefined || value.opcode == Opcode::Null || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::Double || value.opcode == Opcode::String)
            continue;
        // What a handler watches is carried out where it stands.
        bool isWatched = m_currentBlock->terminator.kind == TerminatorKind::Invoke && k + 1 == values.size();
        if (!isDefineProperty(value) || isWatched || hasGenericFallback(m_program, m_function, value) || value.arguments[2] != id || value.arguments[0] == id || value.arguments[1] == id)
            return std::nullopt;
        return numberOfUsesOf(id) == literal.fields.size() + 1 ? std::optional(k) : std::nullopt;
    }
    return std::nullopt;
}

void Lowering::lowerPropertyDefinition(const ObjectLiteral& definition)
{
    auto& block = m_currentBlock->values;
    ValueID id = block[definition.allocation];
    auto& call = m_function.at(id);
    auto* descriptor = objectLiteralAt(*definition.descriptor);
    RELEASE_ASSERT(descriptor && descriptor->describes == definition.allocation);
    uint32_t literal = 0;
    Boxed valueOrGetter = boxedConstant(jsUndefined());
    Boxed setter = boxedConstant(jsUndefined());
    for (size_t index : descriptor->fields) {
        auto& field = m_function.at(block[index]);
        auto name = m_program.string(m_function.at(field.arguments[1]).string());
        auto flag = [&](uint32_t has, uint32_t is) {
            literal |= has | (m_function.at(field.arguments[2]).boolean() ? is : 0);
        };
        if (equalSpans(name, "writable"_s.span8()))
            flag(DescriptorLiteral::hasWritable, DescriptorLiteral::isWritable);
        else if (equalSpans(name, "enumerable"_s.span8()))
            flag(DescriptorLiteral::hasEnumerable, DescriptorLiteral::isEnumerable);
        else if (equalSpans(name, "configurable"_s.span8()))
            flag(DescriptorLiteral::hasConfigurable, DescriptorLiteral::isConfigurable);
        else if (equalSpans(name, "set"_s.span8())) {
            literal |= DescriptorLiteral::hasSet;
            setter = in<Boxed>(field.arguments[2]);
        } else {
            literal |= equalSpans(name, "get"_s.span8()) ? DescriptorLiteral::hasGet : DescriptorLiteral::hasValue;
            valueOrGetter = in<Boxed>(field.arguments[2]);
        }
    }
    Boxed object = in<Boxed>(call.arguments[0]);
    OperationEffects effects = effectsOfBuiltinCall(call);
    auto slot = [&]() -> std::optional<unsigned> {
        auto& key = m_function.at(call.arguments[1]);
        if (key.opcode != Opcode::String || !(literal & DescriptorLiteral::hasValue))
            return std::nullopt;
        auto text = m_program.string(key.string());
        bool mayBeIndex = !text.empty() && std::ranges::all_of(text, [](uint8_t character) {
            return isASCIIDigit(character);
        });
        return mayBeIndex || isPrivateName(text) ? std::nullopt : allocateSite(key.string(), literal);
    }();
    if (slot)
        callICStub(Stub::DefineOwnProperty, Void, { { object.value(), firstStubOperandGPR }, { valueOrGetter.value(), GPRInfo::argumentGPR1 }, { slotAddress(*slot).value(), GPRInfo::argumentGPR2 } }, std::nullopt, effects);
    else
        AOT_TYPED_OPERATION_WITH_EFFECTS(DefineOwnProperty, effects, object, in<Boxed>(call.arguments[1]), valueOrGetter, setter, word32(literal));
    // It yields the object.
    define(id, ensure(object, typeOf(id)));
}

uint32_t maxSlotsOfFunction()
{
    return std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots);
}

// The literal that the allocation or the DefineField at the index is part of.
Lowering::ObjectLiteral* Lowering::objectLiteralAt(size_t index)
{
    // There every value is looked at by itself, where it is defined.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return nullptr;
    if (m_blockOfObjectLiterals != m_currentBlock) {
        m_blockOfObjectLiterals = m_currentBlock;
        m_objectLiterals.clear();
        auto& values = m_currentBlock->values;
        for (size_t i = 0; i < values.size(); ++i) {
            bool hasNullPrototype = isObjectCreateNull(m_function.at(values[i]));
            // The constructor that runs is a constant of the site, or new.target: the site's slots keep the latter only.
            bool isCreateThis = m_function.at(values[i]).opcode == Opcode::CreateThis && (!m_function.at(values[i]).child(1) || m_function.at(m_function.at(values[i]).child(1)).opcode == Opcode::FunctionReference);
            if ((m_function.at(values[i]).opcode != Opcode::NewObject && !hasNullPrototype && !isCreateThis) || isLeftToItsFirstUse(values[i]))
                continue;
            ObjectLiteral literal { i, hasNullPrototype, { } };
            if (isCreateThis)
                literal.newTarget = m_function.at(values[i]).child(0);
                literal.constructor = m_function.at(values[i]).child(1) ? m_function.at(values[i]).child(1) : literal.newTarget;
            literal.fields = fieldsMadeWithAllocation(m_program, m_function, *m_currentBlock, i, isCreateThis, hasNullPrototype);
            if (auto call = definitionDescribedBy(literal)) {
                literal.describes = call;
                ObjectLiteral definition { *call, false, { } };
                definition.descriptor = i;
                m_objectLiterals.append(WTF::move(literal));
                m_objectLiterals.append(WTF::move(definition));
                continue;
            }
            if (!isCreateThis)
                findRowOf(literal);
            if (literal.row != Format::none) {
                m_objectLiterals.append(WTF::move(literal));
                continue;
            }
            if (auto reserved = m_slotsOfWholeBirths.find(values[i].index()); reserved != m_slotsOfWholeBirths.end()) {
                literal.slot = reserved->value;
                m_objectLiterals.append(WTF::move(literal));
                continue;
            }
            unsigned numberOfSlots = isCreateThis ? 3 : 2;
            if (m_context.sites.size() + numberOfSlots > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
                continue;
            literal.slot = m_context.sites.size();
            for (unsigned k = 0; k < numberOfSlots; ++k)
                m_context.sites.append(Site { });
            m_objectLiterals.append(WTF::move(literal));
        }
    }
    for (auto& literal : m_objectLiterals) {
        if (literal.allocation == index || literal.fields.contains(index))
            return &literal;
    }
    return nullptr;
}

uint32_t Lowering::numberOfRowFor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype) const
{
    if (names.size() > maxPropertiesNamedBySite)
        return Format::none;
    for (size_t i = 0; i < names.size(); ++i) {
        auto text = m_program.string(names[i]);
        if ((!text.empty() && isASCIIDigit(text[0])) || std::ranges::any_of(names.first(i), [&](StringID earlier) { return earlier == names[i]; }))
            return Format::none;
    }
    return m_program.numberOfRow(names, inlineCapacity, hasNullPrototype);
}

void Lowering::findRowOf(ObjectLiteral& literal) const
{
    auto& block = m_currentBlock->values;
    ValueID id = block[literal.allocation];
    Vector<StringID, 8> names;
    for (size_t index : literal.fields) {
        auto& field = m_function.at(block[index]);
        names.append(field.opcode == Opcode::PutById ? field.string() : m_function.at(field.arguments[1]).string());
    }
    if (m_function.at(id).hasRowOfBirth()) {
        auto& row = m_program.rowsOfBirths[m_function.at(id).rowOfBirth()];
        if (row.isWhole && names.size() < row.firstPlace)
            return;
        for (size_t i = names.size() - row.firstPlace; row.isWhole && i < row.names.size(); ++i)
            names.append(row.names[i]);
    }
    uint32_t numberByPass = m_function.at(id).numberOfRowOfBirth();
    literal.roomInLine = numberByPass != Format::none ? m_program.numberedRows[numberByPass].inlineCapacity : roomInLineOfLiteral(m_program, m_function, id, !literal.fields.isEmpty());
    literal.row = numberOfRowFor(names.span(), literal.roomInLine, literal.hasNullPrototype);
}

// An ordinary object with the properties, allocated from the cache of a site, of which `slot` is the first slot. The site remembers the structure such an object ends up with: then the object is allocated with it, and the values are
// stored one after the other. `newTarget`: of a CreateThis, else nothing.
CellPointer Lowering::newObjectWithProperties(unsigned slot, bool hasNullPrototype, ValueID newTargetID, ValueID constructorID, unsigned inlineCapacity, std::span<const StringID> names, std::span<const Boxed> values, uint64_t stores, const ObjectLiteral* group, uint32_t row)
{
    RELEASE_ASSERT(!stores || (newTargetID && group));
    unsigned count = names.size();
    RELEASE_ASSERT(count == values.size());
    if (row != Format::none) {
        m_roadOfLiteral = 'r';
        RELEASE_ASSERT(!newTargetID && numberOfRowFor(names, inlineCapacity, hasNullPrototype) == row);
        unsigned inRegisters = std::min(count, maxLiteralPropertiesInRegisters);
        Vector<std::pair<LValue, GPRReg>, 4> operands { { m_out.constInt64(row), GPRInfo::argumentGPR1 } };
        for (unsigned i = 0; i < inRegisters; ++i)
            operands.append({ values[i].value(), GPRInfo::toArgumentRegister(2 + i) });
        LValue made = callICStub(static_cast<Stub>(static_cast<unsigned>(Stub::NewObjectOfRow0) + inRegisters), Int64, operands, std::nullopt, { Writes::RuntimeState, false });
        for (unsigned i = inRegisters; i < count; ++i)
            m_out.store64(values[i].value(), m_out.address(m_heaps.properties.atAnyNumber(), made, JSObject::offsetOfInlineStorage() + i * sizeof(EncodedJSValue)));
        if (count > inRegisters)
            fenceAfterInitialization();
        return CellPointer(made);
    }
    Pointer cache = slotAddress(slot);
    auto cellOf = [&](ValueID id) {
        Operand operand = get(id);
        return operand.rep == Rep::JSValue ? asCell(operand.as<Boxed>()) : operand.as<CellPointer>();
    };
    auto newTarget = [&] { return cellOf(newTargetID); };
    auto constructor = [&] { return cellOf(constructorID); };
    // A store may find a setter, or a property that is read-only. So each member is carried out by itself, as what stands there: a stack trace shows it. Then the run time looks at what came out.
    auto oneByOne = [&] {
        auto indexInBlock = m_currentIndexInBlock;
        m_currentIndexInBlock = group->allocation;
        CellPointer object = AOT_TYPED_OPERATION(CreateThisOfCapacity, constructor(), newTarget(), word32(inlineCapacity));
        LValue buffer = m_out.lockedStackSlot(count * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < count; ++i) {
            m_currentIndexInBlock = group->fields[i];
            if (stores >> i & 1)
                emitPutById(box(object), names[i], values[i]);
            else if (!emitDefineByName(box(object), names[i], values[i]))
                AOT_TYPED_OPERATION(DefineByName, box(object), word32(names[i].index()), values[i]);
            m_out.store64(integer(names[i].index() | (stores >> i & 1) << 32).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        }
        m_currentIndexInBlock = indexInBlock;
        AOT_TYPED_OPERATION(DidStoreToThis, cache, constructor(), newTarget(), object, word32(count), word32(inlineCapacity), Pointer(buffer));
        return object;
    };
    auto generally = [&] {
        if (stores && std::ranges::none_of(names, [&](StringID name) { return isNamedLastIndex(name); })) {
            CellPointer constructorCell = constructor();
            CellPointer newTargetCell = newTarget();
            LValue buffer = transientBuffer(count * sizeof(EncodedJSValue));
            Vector<uint32_t, 16> row;
            Vector<uint32_t, 16> items;
            SetForScope currentIndex(m_currentIndexInBlock, m_currentIndexInBlock);
            for (unsigned i = 0; i < count; ++i) {
                RELEASE_ASSERT(!(names[i].index() >> 31));
                m_out.store64(values[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
                row.append(names[i].index() | static_cast<uint32_t>(stores >> i & 1) << 31);
                m_currentIndexInBlock = group->fields[i];
                items.append(nextItemSite());
            }
            m_currentIndexInBlock = group->allocation;
            uint32_t callSite = m_nextSite;
            CellPointer made = AOT_TYPED_OPERATION(CreateThisWithStores, cache, constructorCell, newTargetCell, word32(count), word32(inlineCapacity), Pointer(buffer), word32(rowOf(row.span())));
            RELEASE_ASSERT(m_nextSite == callSite + 1);
            for (unsigned i = 0; i < count; ++i)
                m_context.itemSites.append({ callSite, i, items[i] });
            return made;
        }
        if (stores) {
            // All at once where nothing on the prototype chain has a say: the object is born with the structure that has all the names, and none is made for the steps between.
            CellPointer constructorCell = constructor();
            CellPointer newTargetCell = newTarget();
            LValue buffer = transientBuffer(2 * count * sizeof(EncodedJSValue));
            for (unsigned i = 0; i < count; ++i) {
                m_out.store64(values[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
                m_out.store64(integer(names[i].index() | (stores >> i & 1) << 32).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, (count + i) * sizeof(EncodedJSValue)));
            }
            CellPointer made = AOT_TYPED_OPERATION(TryCreateThisWithStores, cache, constructorCell, newTargetCell, word32(count), word32(inlineCapacity), Pointer(buffer));
            LBasicBlock isMade = newColdBlock();
            LBasicBlock isNotMade = newColdBlock();
            LBasicBlock done = newColdBlock();
            m_out.branch(m_out.notNull(made.value()), usually(isMade), rarely(isNotMade));
            m_out.appendTo(isMade);
            ValueFromBlock atOnce = m_out.anchor(made.value());
            m_out.jump(done);
            m_out.appendTo(isNotMade);
            ValueFromBlock bySteps = m_out.anchor(oneByOne().value());
            m_out.jump(done);
            m_out.appendTo(done);
            return CellPointer(m_out.phi(Int64, atOnce, bySteps));
        }
        std::optional<CellPointer> constructorCell;
        std::optional<CellPointer> newTargetCell;
        if (newTargetID) {
            constructorCell = constructor();
            newTargetCell = newTarget();
        }
        LValue buffer = transientBuffer(std::max(count, 1u) * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < count; ++i)
            m_out.store64(values[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        Word32 row = word32(rowOfNames(names));
        if (!newTargetID)
            return AOT_TYPED_OPERATION(NewObjectLiteral, cache, word32(count), word32(hasNullPrototype | inlineCapacity << 1), Pointer(buffer), row);
        return AOT_TYPED_OPERATION(CreateThisWithFields, cache, *constructorCell, *newTargetCell, word32(count), word32(inlineCapacity), Pointer(buffer), row);
    };
    auto bySlot = [&](unsigned given) {
        unsigned inRegisters = std::min(given, maxLiteralPropertiesInRegisters);
        Vector<std::pair<LValue, GPRReg>, 4> operands { { cache.value(), GPRInfo::argumentGPR1 } };
        for (unsigned i = 0; i < inRegisters; ++i)
            operands.append({ values[i].value(), GPRInfo::toArgumentRegister(2 + i) });
        LValue made = callICStub(static_cast<Stub>(static_cast<unsigned>(Stub::NewObjectLiteral0) + inRegisters), Int64, operands, std::nullopt, { Writes::RuntimeState, false });
        // The stub has cleared the other slots, all of which are in the object. Nothing has looked at it yet.
        bool hasStoredBehindStub = false;
        for (unsigned i = inRegisters; i < count; ++i) {
            if (values[i].value()->hasInt64() && !values[i].value()->asInt64())
                continue;
            m_out.store64(values[i].value(), m_out.address(m_heaps.properties.atAnyNumber(), made, JSObject::offsetOfInlineStorage() + i * sizeof(EncodedJSValue)));
            hasStoredBehindStub = true;
        }
        if (hasStoredBehindStub)
            fenceAfterInitialization();
        return made;
    };
    // The stub asks the slot itself. What it calls where that is empty finds the names by the site, and leaves the object as the stub does: see newObjectLiteralAtSite(). Nothing stands here for that case.
    bool mayBeIndex = std::ranges::any_of(names, [&](StringID name) {
        auto text = m_program.string(name);
        return !text.empty() && isASCIIDigit(text[0]);
    });
    bool areDistinct = true;
    for (unsigned i = 0; i < count; ++i)
        areDistinct = areDistinct && std::ranges::none_of(names.first(i), [&](StringID earlier) { return earlier == names[i]; });
    m_roadOfLiteral = 'g';
    if (!newTargetID && count <= maxPropertiesNamedBySite && inlineCapacity < 1u << 15 && !mayBeIndex && areDistinct) {
        m_roadOfLiteral = 's';
        Vector<uint32_t, 16> row { count | inlineCapacity << 16 | static_cast<uint32_t>(hasNullPrototype) << 31 };
        for (StringID name : names)
            row.append(name.index());
        row.append(m_program.numberOfRow(names, inlineCapacity, hasNullPrototype) + 1);
        m_context.rowsOfSlots.append({ slot + 1, rowOf(row.span()) });
        unsigned given = count;
        while (given && values[given - 1].value()->hasInt64() && !values[given - 1].value()->asInt64())
            --given;
        return CellPointer(bySlot(given));
    }
    LBasicBlock isFilled = m_out.newBlock();
    LBasicBlock isEmpty = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    static_assert(!OBJECT_OFFSETOF(AOT::Slot, structureID));
    auto inCache = [&](size_t offset) {
        // A collection may clear the slot.
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LValue isValid = m_out.notZero32(m_out.load32(inCache(0)));
    if (newTargetID) {
        // See operationAOTTypedCreateThisWithFields.
        isValid = m_out.bitAnd(isValid, m_out.equal(m_out.load32(inCache(2 * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer))), loadRuntimeState32(Instance::offsetOfTypedPrototypeEpoch())));
        if (m_function.at(newTargetID).opcode != Opcode::FunctionReference)
            isValid = m_out.bitAnd(isValid, m_out.equal(m_out.load64(inCache(OBJECT_OFFSETOF(AOT::Slot, pointer))), newTarget().value()));
    }
    m_out.branch(isValid, usually(isFilled), rarely(isEmpty));
    m_out.appendTo(isFilled);
    ValueFromBlock fast = m_out.anchor(bySlot(count));
    m_out.jump(continuation);
    m_out.appendTo(isEmpty);
    ValueFromBlock slow = m_out.anchor(generally().value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

// Where the numbers of the names start in the function's constant data. The copies of a literal share a row.
uint32_t Lowering::rowOfNames(std::span<const StringID> names)
{
    Vector<uint32_t, 16> row;
    for (StringID name : names)
        row.append(name.index());
    return rowOf(row.span());
}

uint32_t Lowering::rowOf(std::span<const uint32_t> row)
{
    auto& plans = m_context.plans;
    for (size_t start = 0; start + row.size() <= plans.size(); ++start) {
        if (std::ranges::equal(plans.span().subspan(start, row.size()), row))
            return start;
    }
    uint32_t start = plans.size();
    plans.append(row);
    return start;
}

// `{ name: value, .. }`, for whoever has names and values at hand. No name is an index.
CellPointer Lowering::newObjectLiteral(std::span<const StringID> names, std::span<const Boxed> values)
{
    if (m_context.sites.size() + 2 > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
        CellPointer object = newEmptyObject(names.size());
        for (unsigned i = 0; i < names.size(); ++i)
            AOT_TYPED_OPERATION(DefineByName, box(object), word32(names[i].index()), values[i]);
        return object;
    }
    unsigned slot = m_context.sites.size();
    m_context.sites.append(Site { });
    m_context.sites.append(Site { });
    return newObjectWithProperties(slot, false, { }, { }, capacityThatFillsTheCell(names.size()), names, values);
}

bool Lowering::isExpectedToBeSymbol(const Function& function, ValueID id) const
{
    TypeID prediction = function.at(id).prediction;
    auto& type = prediction ? m_program.at(prediction) : m_program.typeOf(function, id);
    return type.primitives & Primitive::Symbol && type.hasOnlyPrimitives(Primitive::Symbol | Primitive::UndefinedOrNull);
}

// A cell is of a size class: what room it has beyond what was asked for is asked for too. So fewer capacities meet the rows of names, and there are fewer structures.
static size_t cellSizeInCompiledProgram(size_t size)
{
    static LazyNeverDestroyed<Vector<size_t>> sizeClasses;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        sizeClasses.construct(MarkedSpace::sizeClassesFor(Options::sizeClassProgressionWithAOT()));
    });
    auto sizeClass = std::ranges::lower_bound(sizeClasses.get(), size);
    return sizeClass == sizeClasses->end() ? WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(size) : *sizeClass;
}

unsigned Lowering::capacityThatFillsTheCell(unsigned inlineCapacity)
{
    if (!inlineCapacity)
        return 0;
    size_t size = cellSizeInCompiledProgram(JSFinalObject::allocationSize(inlineCapacity));
    return std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
}

static unsigned roomToBecomeRecord(const Program& program, std::span<const StringID> names)
{
    const Vector<LayoutID, 1>* records = nullptr;
    for (StringID name : names) {
        auto found = program.recordsThatPlainDataBecomesByName.find(name.index() + 1);
        if (found == program.recordsThatPlainDataBecomesByName.end())
            return 0;
        if (!records || found->value.size() < records->size())
            records = &found->value;
    }
    if (!records)
        return 0;
    size_t size = JSFinalObject::allocationSize(0);
    for (LayoutID record : *records) {
        auto& layout = program.at(record);
        if (layout.sizeWithRoomToSpecialize <= size)
            continue;
        bool hasFieldForEachName = std::ranges::all_of(names, [&](StringID name) {
            return layout.fields.containsIf([&](auto& field) {
                return field.name == name;
            });
        });
        bool hasNameForEachRequiredField = std::ranges::all_of(layout.fields, [&](auto& field) {
            return field.flags & (fieldIsOptional | fieldIsNeverStored) || std::ranges::find(names, field.name) != names.end();
        });
        if (hasFieldForEachName && hasNameForEachRequiredField)
            size = layout.sizeWithRoomToSpecialize;
    }
    return WTF::roundUpToMultipleOf<sizeof(EncodedJSValue)>(size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue);
}

unsigned Lowering::roomInLineOfLiteral(const Program& program, const Function& function, ValueID id, bool hasFields, const UsersInBlocks* users)
{
    auto& builtins = Builtins::singleton();
    auto withoutUpcasts = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Cast && function.at(value).castReason() == CastReason::Upcast)
            value = function.at(value).child(0);
        return value;
    };
    auto makesItsFirstArgument = [&](const Value& user) {
        if (user.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = builtins.at(user.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ObjectAssign)
            return true;
        return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::CopyDataProperties;
    };
    Vector<StringID, 16> names;
    unsigned numberOfKeys = 0;
    collectPropertiesGivenTo(program, function, id, 3, names, numberOfKeys, users);
    unsigned inlineCapacity = std::min<unsigned>(names.size() + numberOfKeys, JSFinalObject::maxInlineCapacity);
    if (inlineCapacity <= JSFinalObject::defaultInlineCapacity) {
        bool isStoredToByKey = false;
        bool isCopiedInto = false;
        auto look = [&](ValueID user) {
            auto& value = function.at(user);
            isStoredToByKey |= value.opcode == Opcode::PutByVal && withoutUpcasts(value.child(0)) == id;
            isCopiedInto |= makesItsFirstArgument(value) && !value.arguments.isEmpty() && withoutUpcasts(value.arguments[0]) == id;
        };
        for (size_t i = 0; users && i < users->size(); ++i)
            look(users->at(i).value);
        for (size_t i = 0; !users && i < function.blocks.size(); ++i) {
            for (ValueID user : function.blocks[i].values)
                look(user);
        }
        if (isCopiedInto || (isStoredToByKey && !hasFields))
            inlineCapacity = 0;
    }
    if (auto& birth = function.at(id); birth.hasRowOfBirth() && !program.rowsOfBirths[birth.rowOfBirth()].isWhole) {
        auto& row = program.rowsOfBirths[birth.rowOfBirth()];
        inlineCapacity = std::max<unsigned>(inlineCapacity, row.firstPlace + row.names.size());
    }
    if (inlineCapacity)
        inlineCapacity = std::max(inlineCapacity, roomToBecomeRecord(program, names.span()));
    return capacityThatFillsTheCell(inlineCapacity);
}

void Lowering::numberRowsOfBirths(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        for (auto& value : program.functions[i].values) {
            if (value.opcode == Opcode::NewObject)
                value.immediates[1] = Format::none;
        }
    });
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (layout.kind != LayoutKind::Record || !layout.shape || layout.admitsForeignObjects || layout.sizeWithRoomToSpecialize <= JSFinalObject::allocationSize(2) || layout.sizeWithRoomToSpecialize > JSFinalObject::allocationSize(JSFinalObject::defaultInlineCapacity))
            continue;
        for (auto& field : layout.fields)
            program.recordsThatPlainDataBecomesByName.add(field.name.index() + 1, Vector<LayoutID, 1>()).iterator->value.append(LayoutID(i));
    }
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return;
    auto& builtins = Builtins::singleton();
    struct Birth {
        ValueID id;
        unsigned room;
        bool hasNullPrototype;
        unsigned numberOfNamesAtBirth;
        Vector<StringID, 8> names;
        Vector<unsigned> blocksOfStores;
    };
    Vector<Vector<Birth>> birthsOfFunctions(program.functions.size());
    const UsersInBlocks noUsers;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        auto makesObjectWithoutPrototype = [&](const Value& value) {
            if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 1 || function.at(value.arguments[0]).opcode != Opcode::Null)
                return false;
            auto& builtin = builtins.at(value.builtin());
            return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == "create"_s;
        };
        BitVector isBirth;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::NewObject || makesObjectWithoutPrototype(function.at(id)))
                    isBirth.set(id.index());
            }
        }
        if (isBirth.isEmpty())
            return;
        UncheckedKeyHashMap<uint32_t, UsersInBlocks, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> usersOfBirths;
        for (unsigned block = 0; block < function.blocks.size(); ++block) {
            for (ValueID user : function.blocks[block].values) {
                function.at(user).forEachChild([&](ValueID child) {
                    while (function.at(child).opcode == Opcode::Cast && function.at(child).castReason() == CastReason::Upcast)
                        child = function.at(child).child(0);
                    if (!isBirth.get(child.index()))
                        return;
                    auto& users = usersOfBirths.add(child.index(), UsersInBlocks { }).iterator->value;
                    if (users.isEmpty() || users.last().value != user)
                        users.append({ block, user });
                });
            }
        }
        for (auto& block : function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                auto& value = function.at(id);
                bool hasNullPrototype = makesObjectWithoutPrototype(value);
                if (value.opcode != Opcode::NewObject && !hasNullPrototype)
                    continue;
                auto found = usersOfBirths.find(id.index());
                const UsersInBlocks& users = found == usersOfBirths.end() ? noUsers : found->value;
                auto fields = fieldsMadeWithAllocation(program, function, block, index, false, hasNullPrototype);
                Vector<StringID, 8> names;
                for (size_t field : fields) {
                    auto& member = function.at(block.values[field]);
                    names.append(member.opcode == Opcode::PutById ? member.string() : function.at(member.arguments[1]).string());
                }
                if (value.hasRowOfBirth()) {
                    auto& row = program.rowsOfBirths[value.rowOfBirth()];
                    for (size_t i = names.size() - std::min<size_t>(names.size(), row.firstPlace); row.isWhole && names.size() >= row.firstPlace && i < row.names.size(); ++i)
                        names.append(row.names[i]);
                }
                bool isNamedBySite = names.size() <= maxPropertiesNamedBySite;
                for (size_t i = 0; isNamedBySite && i < names.size(); ++i) {
                    auto text = program.string(names[i]);
                    isNamedBySite = (text.empty() || !isASCIIDigit(text[0])) && names.find(names[i]) == i;
                }
                if (!isNamedBySite)
                    continue;
                unsigned room = roomInLineOfLiteral(program, function, id, !fields.isEmpty(), &users);
                unsigned numberOfNamesAtBirth = names.size();
                size_t roomForNames = std::max<size_t>(names.size(), room);
                Vector<unsigned> blocksOfStores;
                for (size_t k = 0; k < users.size() && !hasNullPrototype; ++k) {
                    auto& store = function.at(users[k].value);
                    if (store.opcode != Opcode::PutById || store.isInitialStore() || store.hasWordOfKnownPlace() || names.size() >= std::min<size_t>(roomForNames, maxPropertiesNamedBySite) || names.contains(store.string()))
                        continue;
                    ValueID receiver = store.child(0);
                    while (function.at(receiver).opcode == Opcode::Cast && function.at(receiver).castReason() == CastReason::Upcast)
                        receiver = function.at(receiver).child(0);
                    auto text = program.string(store.string());
                    if (receiver != id || (!text.empty() && isASCIIDigit(text[0])) || equalSpans(text, "__proto__"_s.span8()))
                        continue;
                    names.append(store.string());
                    blocksOfStores.append(users[k].block);
                }
                birthsOfFunctions[i].append(Birth { id, room, hasNullPrototype, numberOfNamesAtBirth, WTF::move(names), WTF::move(blocksOfStores) });
            }
        }
    });
    unsigned births = 0;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = program.functions[i];
        for (auto& birth : birthsOfFunctions[i]) {
            ++births;
            uint32_t number = program.ensureNumberOfRow(birth.names.span().first(birth.numberOfNamesAtBirth), birth.room, birth.hasNullPrototype);
            if (function.at(birth.id).opcode == Opcode::NewObject) {
                function.values[birth.id.index()].immediates[1] = number;
                program.rowsThatStayAsBorn.set(number);
            }
            unsigned block = UINT_MAX;
            for (unsigned k = 0; k < birth.blocksOfStores.size(); ++k) {
                if (birth.blocksOfStores[k] != block) {
                    if (program.numberedRows.size() > 60000)
                        break;
                    block = birth.blocksOfStores[k];
                }
                uint32_t grown = program.ensureNumberOfRow(birth.names.span().first(birth.numberOfNamesAtBirth + k + 1), birth.room, false);
                program.rowsGrownInto.append({ number, birth.names[birth.numberOfNamesAtBirth + k].index(), grown });
                number = grown;
            }
        }
    }
    program.firstNumberOfRowsOfThis = program.numberedRows.size();
    for (auto& row : program.rowsOfBirths) {
        bool isNumbered = row.isOfThis && row.isWhole && row.names.size() <= maxPropertiesNamedBySite;
        for (size_t i = 0; isNumbered && i < row.names.size(); ++i) {
            auto text = program.string(row.names[i]);
            isNumbered = text.empty() || !isASCIIDigit(text[0]);
        }
        if (isNumbered)
            row.number = program.ensureNumberOfRow(row.names.span(), 0xff, false);
    }
    for (auto& grown : program.rowsGrownInto)
        program.rowsThatStayAsBorn.clear(grown[0]);
    for (uint32_t number = 0; number < program.numberedRows.size(); ++number) {
        for (StringID name : program.numberedRows[number].names)
            program.numbersOfRowsByName.add(name.index() + 1, Vector<uint32_t, 1>()).iterator->value.append(number);
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT passes: rows by number: ", program.numberedRows.size(), " rows of ", births, " births; of them rows of this: ", program.numberedRows.size() - program.firstNumberOfRowsOfThis);
}

void numberRowsOfBirths(Program& program)
{
    Lowering::numberRowsOfBirths(program);
}

CellPointer Lowering::newEmptyObject(unsigned numberOfNames)
{
    if (numberOfNames <= JSFinalObject::defaultInlineCapacity)
        return AOT_TYPED_OPERATION(NewObject);
    return AOT_TYPED_OPERATION(NewObjectWithRoom, word32(capacityThatFillsTheCell(std::min<unsigned>(numberOfNames, JSFinalObject::maxInlineCapacity))));
}

std::optional<InlineBuiltin> Lowering::builtinThatListed(ValueID list) const
{
    const Value* listing = &m_function.at(withoutCasts(m_function, list));
    if (listing->opcode == Opcode::CallDirect) {
        auto& callee = m_program.at(listing->function());
        if (callee.blocks.size() != 1 || callee.blocks[0].terminator.kind != TerminatorKind::Return || !callee.blocks[0].terminator.value)
            return std::nullopt;
        listing = &callee.at(callee.blocks[0].terminator.value);
    }
    if (listing->opcode != Opcode::CallBuiltin)
        return std::nullopt;
    auto& implementation = m_builtins.at(listing->builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline)
        return std::nullopt;
    return implementation.inlined;
}

ValueID Lowering::listReadBy(ValueID element, bool mustBeFirstElement) const
{
    auto& value = m_function.at(withoutCasts(m_function, element));
    ValueID list;
    ValueID index = value.child(1);
    if (value.opcode == Opcode::GetByVal)
        list = value.child(0);
    else if (value.opcode == Opcode::IteratorValue && m_function.at(withoutCasts(m_function, value.child(0))).opcode == Opcode::IteratorOpen)
        list = m_function.at(withoutCasts(m_function, value.child(0))).child(0);
    else if (value.opcode == Opcode::CallBuiltin && value.arguments.size() == 2) {
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || (implementation.inlined != InlineBuiltin::GetDoubleIndex && implementation.inlined != InlineBuiltin::GetContiguousIndex && implementation.inlined != InlineBuiltin::ArrayGetIndex))
            return { };
        list = value.arguments[0];
        index = value.arguments[1];
    } else
        return { };
    if (mustBeFirstElement) {
        auto& place = m_function.at(withoutUpcasts(index));
        if (place.opcode != Opcode::Integer || place.integer())
            return { };
    }
    return list;
}

bool Lowering::isKeyEnumeratedFromObject(ValueID key, unsigned depth) const
{
    key = withoutUpcasts(key);
    if (m_function.at(key).opcode == Opcode::Parameter) {
        if (depth == 2)
            return false;
        bool isAnyEnumerated = false;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                size_t place = m_function.at(target.block).parameters.find(key);
                if (place == notFound)
                    continue;
                if (place >= target.arguments.size())
                    return false;
                ValueID arriving = target.arguments[place];
                if (m_function.at(withoutUpcasts(arriving)).opcode == Opcode::Undefined)
                    continue;
                if (!isKeyEnumeratedFromObject(arriving, depth + 1))
                    return false;
                isAnyEnumerated = true;
            }
        }
        return isAnyEnumerated;
    }
    if (ValueID list = listReadBy(key, false)) {
        if (auto builtin = builtinThatListed(list); builtin && (*builtin == InlineBuiltin::ForInKeys || *builtin == InlineBuiltin::ObjectKeys || *builtin == InlineBuiltin::ObjectGetOwnPropertyNames || *builtin == InlineBuiltin::ReflectOwnKeys))
            return true;
    }
    if (ValueID pair = listReadBy(key, true)) {
        if (ValueID list = listReadBy(pair, false))
            return builtinThatListed(list) == InlineBuiltin::ObjectEntries;
    }
    return false;
}

bool Lowering::isFilledInLoopByKeyThatIsNotEnumerated(ValueID object) const
{
    if (m_isInLoop)
        return false;
    for (uint32_t i = 0; i < m_function.blocks.size(); ++i) {
        if (!m_loopBlocks.get(i))
            continue;
        for (ValueID user : m_function.blocks[i].values) {
            auto& value = m_function.at(user);
            if (value.opcode == Opcode::PutByVal && withoutUpcasts(value.child(0)) == object && !typeOf(value.child(1)).isNumber() && !isKeyEnumeratedFromObject(value.child(1)))
                return true;
        }
    }
    return false;
}

// Where the last of the DefineFields stands, or the allocation if there is none. Until here nothing has the object, so nobody can tell that it did not exist.
void Lowering::lowerObjectLiteral(const ObjectLiteral& literal)
{
    if (literal.describes)
        return;
    if (literal.descriptor)
        return lowerPropertyDefinition(literal);
    auto& block = m_currentBlock->values;
    ValueID id = block[literal.allocation];
    if (literal.fields.isEmpty() && !literal.newTarget && !m_function.at(id).hasRowOfBirth() && isFilledInLoopByKeyThatIsNotEnumerated(id)) {
        defineObjectMadeLater(id, countPlainAllocation(PlainAllocationKind::Object, id, AOT_TYPED_OPERATION(NewObjectForKeys, word32(literal.hasNullPrototype))));
        return;
    }
    Vector<StringID, 6> names;
    Vector<Boxed, 6> values;
    uint64_t stores = 0;
    for (size_t index : literal.fields) {
        auto& field = m_function.at(block[index]);
        bool isStore = field.opcode == Opcode::PutById;
        if (isStore && literal.newTarget)
            stores |= 1ull << names.size();
        names.append(isStore ? field.string() : m_function.at(field.arguments[1]).string());
        values.append(in<Boxed>(isStore ? field.child(1) : field.arguments[2]));
    }
    if (Options::aotMapFilePath()) [[unlikely]] {
        unsigned operands = 0;
        for (size_t index : literal.fields) {
            auto& field = m_function.at(block[index]);
            switch (m_function.at(field.opcode == Opcode::PutById ? field.child(1) : field.arguments[2]).opcode) {
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::Boolean:
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::String:
            case Opcode::BigInt:
            case Opcode::FunctionReference:
                break;
            default:
                ++operands;
            }
        }
        m_context.literalsForMap.append('\t', block[m_currentIndexInBlock].index() + 1, '\t', literal.newTarget ? 't' : literal.hasNullPrototype ? 'n' : 'o', m_isInLoop ? "l"_s : ""_s, '\t', operands, '\t', names.isEmpty() ? "-"_s : ""_s);
        for (size_t i = 0; i < names.size(); ++i)
            m_context.literalsForMap.append(i ? ","_s : ""_s, names[i].index());
    }
    if (m_function.at(id).hasRowOfBirth()) {
        auto& row = m_program.rowsOfBirths[m_function.at(id).rowOfBirth()];
        bool isGapless = row.isWhole && names.size() >= row.firstPlace;
        for (size_t i = 0; isGapless && i < row.names.size(); ++i)
            isGapless = row.firstPlace + i < names.size() ? names[row.firstPlace + i] == row.names[i] : !names.contains(row.names[i]);
        if (!isGapless && row.isWhole)
            fail("a birth that was settled to be whole is not"_s);
        for (size_t i = names.size() - row.firstPlace; isGapless && i < row.names.size(); ++i) {
            names.append(row.names[i]);
            values.append(Boxed(m_out.constInt64(0)));
        }
    }
    // Room for what is seen to be given to the object. Of a literal: zero: nothing is seen, and the engine has its default.
    unsigned inlineCapacity = 0;
    if (literal.row != Format::none)
        inlineCapacity = literal.roomInLine;
    else if (!literal.newTarget)
        inlineCapacity = roomInLineOfLiteral(m_program, m_function, id, !literal.fields.isEmpty());
    else {
        Vector<StringID, 16> names;
        unsigned numberOfKeys = 0;
        collectPropertiesGivenTo(m_program, m_function, id, 3, names, numberOfKeys);
        inlineCapacity = std::min<unsigned>(names.size() + numberOfKeys, JSFinalObject::maxInlineCapacity);
        if (!inlineCapacity)
            inlineCapacity = JSFinalObject::defaultInlineCapacity;
        inlineCapacity = capacityThatFillsTheCell(inlineCapacity);
    }
    defineObjectMadeLater(id, countPlainAllocation(literal.newTarget ? PlainAllocationKind::This : PlainAllocationKind::Object, id, newObjectWithProperties(literal.slot, literal.hasNullPrototype, literal.newTarget, literal.constructor, inlineCapacity, names.span(), values.span(), stores, &literal, literal.row)));
    if (Options::aotMapFilePath()) [[unlikely]]
        m_context.literalsForMap.append('\t', m_roadOfLiteral);
}

CellPointer Lowering::countPlainAllocation(PlainAllocationKind kind, ValueID id, CellPointer cell)
{
    if (Options::countAOTTypedAllocations()) [[unlikely]]
        AOT_TYPED_OPERATION(CountPlainAllocation, cell, word32(m_id.index()), word32(static_cast<uint32_t>(kind) << plainAllocationKindShift | id.index()));
    return cell;
}

// It is what the allocation would have made, so it is of its type.
void Lowering::defineObjectMadeLater(ValueID allocation, CellPointer object)
{
    Rep rep = m_function.at(allocation).rep;
    RELEASE_ASSERT(rep == Rep::Cell || rep == Rep::JSValue);
    define(allocation, rep == Rep::Cell ? Operand(object) : Operand(box(object)));
}

// The layouts that a copy of an object of the type is made of in place. Of many members: those the type names.
Vector<LayoutID, 2> Lowering::layoutsClonedInPlace(const Type& type) const
{
    auto isClonedInPlace = [&](LayoutID id) {
        auto& layout = m_program.at(id);
        if (layout.allocationOfClone == Format::none || layout.hasExtension || !layout.shape)
            return false;
        auto& allocation = m_program.allocations[layout.allocationOfClone];
        return allocation.size == layout.sizeWithRoomToSpecialize && !allocation.numberOfLiterals;
    };
    Vector<LayoutID, 2> result;
    for (LayoutID view : type.views) {
        for (LayoutID member : m_program.at(view).members) {
            if (isClonedInPlace(member) && !result.contains(member))
                result.append(member);
        }
    }
    if (result.size() <= 2)
        return result;
    result.removeAllMatching([&](LayoutID id) {
        return !type.views.contains(id);
    });
    if (result.size() > 2)
        result.clear();
    return result;
}

void Lowering::unlessHasNumberBox(CellPointer cell, const Layout& layout, LBasicBlock otherwise)
{
    for (auto& field : layout.fields) {
        if (field.store == Store::Int32OrElsewhere && !(field.flags & fieldIsNeverStored))
            orElse(logicalNot(refersToNumberBox(load(field.store, fieldAt(cell, field), 0).as<Word32>())), otherwise);
    }
}

// A fresh object of the layout with what the fields of the other hold, which is of the same layout and has no extra properties and no number in a box. `overwritten`: the caller stores to these before anybody looks. It also sees to the fence.
CellPointer Lowering::copyRecord(CellPointer cell, Word32 orderWord, const Layout& layout, const Vector<StringID, 4>& overwritten)
{
    CellPointer copy = allocate(layout.allocationOfClone);
    storeField(copy, m_heaps.AOTTypedObject_orderWord, bitAnd(orderWord, word32(orderWordHasExtraProperties - 1)));
    for (auto& field : layout.fields) {
        if (field.flags & fieldIsNeverStored || overwritten.contains(field.name))
            continue;
        TypedPointer from = fieldAt(cell, field);
        TypedPointer to = fieldAt(copy, field);
        switch (sizeOf(field.store)) {
        case 1:
            m_out.store32As8(m_out.load8ZeroExt32(from), to);
            break;
        case 2:
            m_out.store32As16(m_out.load16ZeroExt32(from), to);
            break;
        case 4:
            m_out.store32(m_out.load32(from), to);
            break;
        case 8:
            m_out.store64(m_out.load64(from), to);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    // What a 32-bit slot refers to is kept alive by the source until the copy has it.
    ensureStillAliveHere(cell);
    return copy;
}

std::optional<Lowering::MergeGroup> Lowering::findMergeGroup(size_t indexOfFirstMember)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    if (indexOfFirstMember + 2 >= values.size())
        return std::nullopt;
    auto& first = m_function.at(values[indexOfFirstMember]);
    auto& second = m_function.at(values[indexOfFirstMember + 1]);
    auto& cast = m_function.at(values[indexOfFirstMember + 2]);
    bool beginsWithClone = m_builtins.at(first.builtin()).implementation.operation == LanguageOperation::CloneObject;
    ValueID object = beginsWithClone ? values[indexOfFirstMember] : first.arguments[0];
    ValueID firstSource = first.arguments[beginsWithClone ? 0 : 1];
    bool isSpread = second.opcode == Opcode::CallBuiltin && m_builtins.at(second.builtin()).implementation.kind == BuiltinImplementation::Operation && m_builtins.at(second.builtin()).implementation.operation == LanguageOperation::CopyDataProperties;
    if (!isSpread || second.arguments.size() != 2 || second.arguments[0] != object || second.arguments[1] == object || firstSource == object)
        return std::nullopt;
    if (cast.opcode != Opcode::Cast || cast.castReason() != CastReason::As || cast.child(0) != object || numberOfUsesOf(object) != (beginsWithClone ? 2 : 3))
        return std::nullopt;
    LayoutID view = typeOf(values[indexOfFirstMember + 2]).onlyView();
    if (!view || m_program.at(view).kind != LayoutKind::Record || !m_program.at(view).shape || !m_program.at(view).members.contains(view))
        return std::nullopt;
    if (!typeOf(firstSource).onlyView() || !typeOf(second.arguments[1]).onlyView())
        return std::nullopt;
    if (beginsWithClone && typeOf(firstSource).onlyView() != typeOf(second.arguments[1]).onlyView())
        return std::nullopt;
    return MergeGroup { indexOfFirstMember, view, beginsWithClone };
}

Operand Lowering::lowerMergeGroup(MergeGroup group)
{
    auto& values = m_currentBlock->values;
    auto& first = m_function.at(values[group.first]);
    auto& second = m_function.at(values[group.first + 1]);
    Rep rep = m_function.at(values[group.first + 2]).rep;
    auto& layout = m_program.at(group.layout);
    LBasicBlock isMerged = m_out.newBlock();
    LBasicBlock isNotMerged = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer merged = AOT_TYPED_OPERATION(TryMergeRecordsAs, word32(layout.shape), in<Boxed>(first.arguments[group.beginsWithClone ? 0 : 1]), in<Boxed>(second.arguments[1]));
    m_out.branch(m_out.notZero64(merged.value()), usually(isMerged), rarely(isNotMerged));
    m_out.appendTo(isMerged);
    join.add(convert(Operand(merged), m_program.at(layout.type), rep));
    m_out.appendTo(isNotMerged);
    Operand result;
    {
        SetForScope isCarryingOut(m_isCarryingOutMergeGroup, true);
        SetForScope isInArm(m_isInArm, true);
        SetForScope indexInBlock(m_currentIndexInBlock, m_currentIndexInBlock);
        for (size_t k = group.first; k <= group.first + 2; ++k) {
            m_currentIndexInBlock = k;
            result = lower(values[k], m_function.at(values[k]));
            define(values[k], result);
        }
    }
    join.add(result);
    return join.finish();
}

CellPointer Lowering::copyRecordAs(const Value& value, LayoutID to, NamesOfCopy namesAre)
{
    auto& target = m_program.at(to);
    ValueID sourceID = value.arguments[0];
    auto& typeOfSource = typeOf(sourceID);
    bool excludesNames = namesAre == NamesOfCopy::Excluded;
    bool storesNamesFirst = namesAre == NamesOfCopy::StoredFirst;
    auto isRecordWithoutExtraProperties = [&](LValue words) {
        return m_out.bitAnd(m_out.aboveOrEqual(words, m_out.constInt64(0xffULL << 56)), m_out.testIsZero64(words, m_out.constInt64(orderWordHasExtraProperties)));
    };
    auto outOfLine = [&] {
        LBasicBlock mayBeCopied = m_out.newBlock();
        LBasicBlock isNoRecord = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 2> copies;
        Boxed boxed = in<Boxed>(sourceID);
        orElse(isCell(boxed, typeOfSource), isNoRecord);
        m_out.branch(isRecordWithoutExtraProperties(m_out.load64(TypedPointer(m_heaps.root, m_out.add(asCell(boxed).value(), m_out.constIntPtr(offsetOfOrderWord))))), unsure(mayBeCopied), unsure(isNoRecord));
        m_out.appendTo(mayBeCopied);
        auto [count, list] = argumentBuffer(value.arguments);
        copies.append(m_out.anchor(AOT_TYPED_OPERATION(TryCopyRecordAs, word32(target.shape), word32(static_cast<uint32_t>(namesAre)), count, list).value()));
        m_out.jump(continuation);
        m_out.appendTo(isNoRecord);
        copies.append(m_out.anchor(m_out.int64Zero));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return CellPointer(m_out.phi(Int64, copies));
    };
    Vector<StringID, 4> names;
    Vector<ValueID, 4> given;
    for (size_t i = 1; i < value.arguments.size(); i += storesNamesFirst ? 2 : 1) {
        auto& name = m_function.at(value.arguments[i]);
        if (name.opcode != Opcode::String || (storesNamesFirst && (names.contains(name.string()) || i + 1 == value.arguments.size())))
            return outOfLine();
        names.append(name.string());
        if (storesNamesFirst)
            given.append(value.arguments[i + 1]);
    }
    bool isAllocatedInPlace = target.allocationOfClone != Format::none && !target.hasExtension && target.shape && m_program.allocations[target.allocationOfClone].size == target.sizeWithRoomToSpecialize
        && !m_program.allocations[target.allocationOfClone].numberOfLiterals;
    if (!isAllocatedInPlace || typeOfSource.primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt) || typeOfSource.anyObject || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations())
        return outOfLine();
    struct Source {
        LayoutID layout;
        Vector<std::pair<uint32_t, uint32_t>, 8> moves;
    };
    auto sameLiterals = [](const Field& a, const Field& b) {
        return a.literals.size() == b.literals.size() && std::ranges::equal(a.literals, b.literals, [](const Literal& x, const Literal& y) {
            return x.string == y.string && std::bit_cast<uint64_t>(x.number) == std::bit_cast<uint64_t>(y.number);
        });
    };
    auto restIsBornOrAbsent = [&](const BitVector& isSupplied) {
        for (uint32_t i = 0; i < target.fields.size(); ++i) {
            auto& held = target.fields[i];
            if (held.flags & fieldIsNeverStored || isSupplied.get(i))
                continue;
            uint16_t primitives = m_program.at(held.stored).primitives;
            if (held.isOutOfLine)
                return false;
            if (!excludesNames && names.contains(held.name)) {
                if (held.flags & fieldIsImmutable || held.isPinned || m_program.at(held.slot).isDiscriminant || (!storesNamesFirst && primitives & Primitive::Absent && !(primitives & Primitive::Undefined)))
                    return false;
                if (ValueID stored = storesNamesFirst ? given[names.find(held.name)] : ValueID(); stored && ((held.type && !typeOf(stored).isWithin(m_program.at(held.type), m_program)) || !isStoredInPlace(held, stored)))
                    return false;
            } else if (!(primitives & Primitive::Absent))
                return false;
        }
        return true;
    };
    auto movesFrom = [&](LayoutID id) -> std::optional<Source> {
        auto& layout = m_program.at(id);
        if (layout.kind != LayoutKind::Record || !layout.shape || Typed::shapeWordOf(layout.shape) >> 24 != 0xff || !layout.isAllocated || layout.hasExtension)
            return std::nullopt;
        Source source { id, { } };
        BitVector isSupplied;
        for (uint32_t i = 0; i < layout.fields.size(); ++i) {
            auto& field = layout.fields[i];
            if (field.flags & fieldIsNeverStored || (!storesNamesFirst && names.contains(field.name)))
                continue;
            if (names.contains(field.name) && m_program.at(field.stored).primitives & Primitive::Absent)
                return std::nullopt;
            auto index = target.fieldNamed(field.name);
            if (!index)
                return std::nullopt;
            auto& held = target.fields[*index];
            if (held.flags & (fieldIsNeverStored | fieldIsImmutable) || field.isOutOfLine || held.isOutOfLine || (held.store != field.store && ((held.store == Store::Int32OrElsewhere && !m_program.at(field.stored).isNumber()) || field.store == Store::Int32OrElsewhere || m_program.at(field.stored).primitives & Primitive::Absent)) || held.isPinned || field.isPinned || !sameLiterals(held, field)
                || m_program.at(held.slot).isDiscriminant || !m_program.at(field.stored).isWithin(m_program.at(held.stored), m_program))
                return std::nullopt;
            source.moves.append({ i, *index });
            isSupplied.set(*index);
        }
        if (!restIsBornOrAbsent(isSupplied))
            return std::nullopt;
        return source;
    };
    Vector<Source, 2> sources;
    for (LayoutID view : typeOfSource.views) {
        for (LayoutID member : m_program.at(view).members) {
            bool isKnown = sources.containsIf([&](auto& source) {
                return source.layout == member;
            });
            if (isKnown || sources.size() == 2)
                continue;
            if (auto source = movesFrom(member))
                sources.append(WTF::move(*source));
        }
    }
    bool makesRecordOfNothing = namesAre == NamesOfCopy::StoredNext && typeOfSource.primitives & Primitive::UndefinedOrNull && restIsBornOrAbsent({ });
    for (StringID name : names) {
        if (!excludesNames && !target.fieldNamed(name)) {
            sources.clear();
            makesRecordOfNothing = false;
        }
    }
    if (m_context.sites.size() + sources.size() + 1 + makesRecordOfNothing > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots) || (m_program.runsOnce(m_id) && !m_isInLoop))
        return outOfLine();
    unsigned firstSlot = m_context.sites.size();
    for (unsigned i = 0; i <= sources.size() + makesRecordOfNothing; ++i)
        m_context.sites.append(Site { });
    Pointer cache = slotAddress(firstSlot);
    auto inCache = [&](size_t offset) {
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock isNoRecord = newColdBlock();
    LBasicBlock isNoCell = makesRecordOfNothing ? m_out.newBlock() : isNoRecord;
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> copies;
    Boxed boxed = in<Boxed>(sourceID);
    orElse(isCell(boxed, typeOfSource), isNoCell);
    CellPointer cell = asCell(boxed);
    static_assert(offsetOfOrderWord + sizeof(uint32_t) == offsetOfShapeWord && !(offsetOfOrderWord % sizeof(uint64_t)));
    LValue words = m_out.load64(TypedPointer(m_heaps.root, m_out.add(cell.value(), m_out.constIntPtr(offsetOfOrderWord))));
    orElse(Word32(m_out.notZero64(words)), isNoRecord);
    for (unsigned i = 0; i < sources.size(); ++i) {
        auto& layout = m_program.at(sources[i].layout);
        LBasicBlock hit = m_out.newBlock();
        LBasicBlock otherwise = i + 1 == sources.size() ? slowPath : m_out.newBlock();
        m_out.branch(m_out.equal(words, m_out.load64(inCache(i * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer)))), usually(hit), rarely(otherwise));
        m_out.appendTo(hit);
        Vector<LValue, 8> numbersInPlace;
        for (auto [from, into] : sources[i].moves) {
            if (layout.fields[from].store == Store::Int32OrElsewhere)
                orElse(logicalNot(refersToNumberBox(load(Store::Int32OrElsewhere, fieldAt(cell, layout.fields[from]), 0).as<Word32>())), slowPath);
            bool isFitted = layout.fields[from].store != Store::Int32OrElsewhere && target.fields[into].store == Store::Int32OrElsewhere;
            numbersInPlace.append(isFitted ? low32(fitOrElse(load(layout.fields[from].store, fieldAt(cell, layout.fields[from]), 0, cell.value()), m_program.at(layout.fields[from].stored), numbersKeptInPlace(), slowPath).as<Integer>()).value() : nullptr);
        }
        CellPointer copy = allocate(target.allocationOfClone);
        m_out.store32(m_out.load32(inCache(i * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, offset))), copy.value(), m_heaps.AOTTypedObject_orderWord);
        BitVector isMoved;
        for (unsigned k = 0; k < sources[i].moves.size(); ++k) {
            auto [from, into] = sources[i].moves[k];
            TypedPointer source = fieldAt(cell, layout.fields[from]);
            TypedPointer destination = fieldAt(copy, target.fields[into]);
            isMoved.set(into);
            if (numbersInPlace[k]) {
                store(Store::Int32OrElsewhere, Word32(numbersInPlace[k]), destination);
                continue;
            }
            if (layout.fields[from].store != target.fields[into].store) {
                store(target.fields[into].store, convert(load(layout.fields[from].store, source, 0, cell.value()), m_program.at(layout.fields[from].stored), repOf(target.fields[into].store)), destination);
                continue;
            }
            switch (sizeOf(target.fields[into].store)) {
            case 1:
                m_out.store32As8(m_out.load8ZeroExt32(source), destination);
                break;
            case 2:
                m_out.store32As16(m_out.load16ZeroExt32(source), destination);
                break;
            case 4:
                m_out.store32(m_out.load32(source), destination);
                break;
            case 8:
                m_out.store64(m_out.load64(source), destination);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        for (uint32_t k = 0; k < target.fields.size(); ++k) {
            auto& held = target.fields[k];
            if (held.flags & fieldIsNeverStored || isMoved.get(k))
                continue;
            if (!storesNamesFirst || !names.contains(held.name))
                store(held.store, initialStateOf(held, !excludesNames && names.contains(held.name)), fieldAt(copy, held));
            else if (held.store == Store::Int32OrElsewhere)
                storeInt32OrElsewhere(copy, held, given[names.find(held.name)], true);
            else
                store(held.store, in(given[names.find(held.name)], repOf(held.store)), fieldAt(copy, held));
        }
        ensureStillAliveHere(cell);
        fenceAfterInitialization();
        copies.append(m_out.anchor(copy.value()));
        m_out.jump(continuation);
        if (otherwise != slowPath)
            m_out.appendTo(otherwise);
    }
    if (sources.isEmpty())
        m_out.jump(slowPath);
    m_out.appendTo(slowPath);
    static_assert(Typed::shapeWordOf(1) >> 24 == 0xff);
    LBasicBlock mayBeCopied = newColdBlock();
    m_out.branch(isRecordWithoutExtraProperties(words), unsure(mayBeCopied), unsure(isNoRecord));
    m_out.appendTo(mayBeCopied);
    orElse(Word32(m_out.notEqual(words, m_out.load64(inCache(sources.size() * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer))))), isNoRecord);
    auto tryOutOfLine = [&] {
        auto [count, list] = argumentBuffer(value.arguments);
        copies.append(m_out.anchor(AOT_TYPED_OPERATION(TryCopyRecordAsAt, cache, word32(sources.isEmpty() ? 0 : m_program.at(sources[0].layout).shape), word32(sources.size() > 1 ? m_program.at(sources[1].layout).shape : 0), word32(target.shape), word32(static_cast<uint32_t>(namesAre)), count, list).value()));
        m_out.jump(continuation);
    };
    tryOutOfLine();
    if (makesRecordOfNothing) {
        size_t slot = (sources.size() + 1) * sizeof(AOT::Slot);
        LBasicBlock isKnown = m_out.newBlock();
        LBasicBlock isNotKnown = newColdBlock();
        m_out.appendTo(isNoCell);
        m_out.branch(m_out.notZero64(m_out.load64(inCache(slot + OBJECT_OFFSETOF(AOT::Slot, pointer)))), usually(isKnown), rarely(isNotKnown));
        m_out.appendTo(isKnown);
        CellPointer made = allocate(target.allocationOfClone);
        m_out.store32(m_out.load32(inCache(slot + OBJECT_OFFSETOF(AOT::Slot, offset))), made.value(), m_heaps.AOTTypedObject_orderWord);
        for (auto& held : target.fields) {
            if (!(held.flags & fieldIsNeverStored))
                store(held.store, initialStateOf(held, names.contains(held.name)), fieldAt(made, held));
        }
        fenceAfterInitialization();
        copies.append(m_out.anchor(made.value()));
        m_out.jump(continuation);
        m_out.appendTo(isNotKnown);
        tryOutOfLine();
    }
    m_out.appendTo(isNoRecord);
    copies.append(m_out.anchor(m_out.int64Zero));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, copies));
}

std::optional<Lowering::CloneGroup> Lowering::findCloneGroup(size_t indexOfClone)
{
    // There every value is looked at by itself, where it is defined. What a handler watches is carried out where it stands.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    ValueID copy = values[indexOfClone];
    CloneGroup group { indexOfClone };
    bool somethingReadsOrThrows = false;
    for (size_t k = indexOfClone + 1; k < values.size(); ++k) {
        auto& value = m_function.at(values[k]);
        if (value.opcode == Opcode::PutById && !value.isInitialStore() && value.child(0) == copy && value.child(1) != copy) {
            group.stores.append(k);
            group.isDecidedAtClone = somethingReadsOrThrows;
            continue;
        }
        if (value.opcode == Opcode::Cast && value.castReason() == CastReason::As && value.child(0) == copy) {
            group.cast = k;
            group.isDecidedAtClone = somethingReadsOrThrows;
            break;
        }
        bool usesCopy = false;
        value.forEachChild([&](ValueID child) {
            usesCopy |= child == copy;
        });
        if (usesCopy)
            break;
        bool dependsOnNothing = value.opcode == Opcode::Undefined || value.opcode == Opcode::Null || value.opcode == Opcode::Boolean || value.opcode == Opcode::Integer || value.opcode == Opcode::Double || value.opcode == Opcode::String
            || value.opcode == Opcode::NewArray || value.opcode == Opcode::FunctionReference || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast);
        if (dependsOnNothing)
            continue;
        // The source is what it was.
        if (value.opcode == Opcode::Cast || value.opcode == Opcode::CallDirect || writesOf(m_program, m_function, *m_currentBlock, k) > Writes::Nothing)
            break;
        somethingReadsOrThrows = true;
    }
    if (group.stores.isEmpty() && group.cast == notFound)
        return std::nullopt;
    group.layouts = layoutsClonedInPlace(typeOf(m_function.at(copy).arguments[0]));
    group.layouts.removeAllMatching([&](LayoutID id) {
        auto& layout = m_program.at(id);
        for (size_t k : group.stores) {
            auto& store = m_function.at(values[k]);
            auto index = layout.fieldNamed(store.string());
            if (!index)
                return true;
            auto& field = layout.fields[*index];
            if (field.flags & fieldIsNeverStored || field.isOutOfLine || m_program.at(field.slot).isDiscriminant)
                return true;
            // See fieldIsPinned: nothing but the string it is pinned to is stored past the guard.
            if (field.isPinned) {
                ValueID stored = store.child(1);
                if (ValueID constant = m_program.constantOf(m_function, stored))
                    stored = constant;
                if (m_function.at(stored).opcode != Opcode::String || m_function.at(stored).string() != field.literals[0].string)
                    return true;
            }
            // What is not of the type of the field may be made something else on its way in.
            if ((field.type && !typeOf(store.child(1)).isWithin(m_program.at(field.type), m_program)) || !isStoredInPlace(field, store.child(1)))
                return true;
        }
        return group.cast != notFound && !m_program.isHeldAlready(m_program.at(layout.type), typeOf(values[group.cast]), CastReason::As);
    });
    if (group.layouts.isEmpty())
        return std::nullopt;
    return group;
}

// `onLayout(index among the layouts)` ends its block.
template<typename Functor>
void Lowering::testSourceOfCloneGroup(const CloneGroup& group, LBasicBlock slowPath, const Functor& onLayout)
{
    auto& values = m_currentBlock->values;
    ValueID sourceID = m_function.at(values[group.clone]).arguments[0];
    Boxed source = in<Boxed>(sourceID);
    orElse(isCell(source, typeOf(sourceID)), slowPath);
    CellPointer cell = asCell(source);
    Word32 shapeWord = shapeWordOf(cell);
    orElse(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), slowPath);
    for (unsigned i = 0; i < group.layouts.size(); ++i) {
        auto& layout = m_program.at(group.layouts[i]);
        LBasicBlock isOfLayout = m_out.newBlock();
        LBasicBlock otherwise = i + 1 == group.layouts.size() ? slowPath : m_out.newBlock();
        m_out.branch(isZero(add(shapeWord, word32(layout.shape))).value(), usually(isOfLayout), rarely(otherwise));
        m_out.appendTo(isOfLayout);
        // A store to a field that is absent creates the property: see lowerCloneGroup(). [[Set]] asks the prototype chain first, where that may have the name.
        for (size_t k : group.stores) {
            auto& field = layout.fields[*layout.fieldNamed(m_function.at(values[k]).string())];
            if (m_program.at(field.stored).primitives & Primitive::Absent && inheritsByItsLayout(m_program, group.layouts[i], field.name))
                orElse(logicalNot(isFieldAbsent(cell, field, 0)), slowPath);
        }
        unlessHasNumberBox(cell, layout, slowPath);
        onLayout(i);
        if (otherwise != slowPath)
            m_out.appendTo(otherwise);
    }
}

void Lowering::decideCloneGroup(CloneGroup& group)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> decisions;
    Vector<ValueFromBlock, 3> copies;
    testSourceOfCloneGroup(group, slowPath, [&](unsigned i) {
        decisions.append(m_out.anchor(m_out.constInt32(i + 1)));
        copies.append(m_out.anchor(m_out.int64Zero));
        m_out.jump(continuation);
    });
    m_out.appendTo(slowPath);
    decisions.append(m_out.anchor(m_out.int32Zero));
    copies.append(m_out.anchor(AOT_TYPED_OPERATION(CloneObject, in<Boxed>(m_function.at(m_currentBlock->values[group.clone]).arguments[0]), word32(true)).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    group.decision = m_out.phi(Int32, decisions);
    group.copied = m_out.phi(Int64, copies);
}

// Where the last member stands. What it yields is that member's.
Operand Lowering::lowerCloneGroup(CloneGroup group)
{
    auto& values = m_currentBlock->values;
    ValueID copyID = values[group.clone];
    ValueID sourceID = m_function.at(copyID).arguments[0];
    bool hasCast = group.cast != notFound;
    Rep repOfCast = hasCast ? m_function.at(values[group.cast]).rep : Rep::Void;
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> copies;
    Vector<ValueFromBlock, 3> results;
    Vector<StringID, 4> overwritten;
    for (size_t k : group.stores)
        overwritten.append(m_function.at(values[k]).string());
    auto inPlace = [&](unsigned i) {
        auto& layout = m_program.at(group.layouts[i]);
        CellPointer cell = asCell(in<Boxed>(sourceID));
        CellPointer copy = copyRecord(cell, orderWordOf(cell), layout, overwritten);
        // Nobody has seen it, and nothing has been allocated since.
        for (size_t k : group.stores) {
            auto& member = m_function.at(values[k]);
            auto& field = layout.fields[*layout.fieldNamed(member.string())];
            if (field.store == Store::Int32OrElsewhere)
                storeInt32OrElsewhere(copy, field, member.child(1), true);
            else
                store(field.store, in(member.child(1), repOf(field.store)), fieldAt(copy, field));
        }
        // What the source lacked is a new key of the copy, behind the others, in the order of the stores. Last: a miss calls the run time, and the copy is whole by now.
        Vector<StringID, 4> created;
        for (size_t k : group.stores) {
            auto& field = layout.fields[*layout.fieldNamed(m_function.at(values[k]).string())];
            if (!(m_program.at(field.stored).primitives & Primitive::Absent) || inheritsByItsLayout(m_program, group.layouts[i], field.name) || created.contains(field.name))
                continue;
            created.append(field.name);
            SetForScope indexInBlock(m_currentIndexInBlock, k);
            SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, copyID));
            LBasicBlock creates = m_out.newBlock();
            LBasicBlock isThere = m_out.newBlock();
            m_out.branch(isFieldAbsent(cell, field, 0).value(), unsure(creates), unsure(isThere));
            m_out.appendTo(creates);
            didEmitWrite(Writes::Fields);
            didCreateFieldInCell(copy, field, OperationEffects { Writes::Fields, false, false, true }, [] { });
            m_out.jump(isThere);
            m_out.appendTo(isThere);
        }
        fenceAfterInitialization();
        copies.append(m_out.anchor(copy.value()));
        if (hasCast)
            results.append(m_out.anchor(convert(Operand(copy), m_program.at(layout.type), repOfCast).value));
        m_out.jump(continuation);
    };
    if (!group.isDecidedAtClone)
        testSourceOfCloneGroup(group, slowPath, inPlace);
    else {
        for (unsigned i = 0; i < group.layouts.size(); ++i) {
            LBasicBlock isOfLayout = m_out.newBlock();
            LBasicBlock otherwise = i + 1 == group.layouts.size() ? slowPath : m_out.newBlock();
            m_out.branch(m_out.equal(group.decision, m_out.constInt32(i + 1)), usually(isOfLayout), rarely(otherwise));
            m_out.appendTo(isOfLayout);
            inPlace(i);
            if (otherwise != slowPath)
                m_out.appendTo(otherwise);
        }
    }

    // Each member as what stands there.
    m_out.appendTo(slowPath);
    CellPointer other = group.isDecidedAtClone ? CellPointer(group.copied) : AOT_TYPED_OPERATION(CloneObject, in<Boxed>(sourceID), word32(true));
    defineObjectMadeLater(copyID, other);
    Operand result;
    {
        SetForScope indexInBlock(m_currentIndexInBlock, m_currentIndexInBlock);
        SetForScope isInArm(m_isInArm, true);
        auto carryOut = [&](size_t k) {
            m_currentIndexInBlock = k;
            result = lower(values[k], m_function.at(values[k]));
            define(values[k], result);
        };
        for (size_t k : group.stores)
            carryOut(k);
        if (hasCast)
            carryOut(group.cast);
    }
    copies.append(m_out.anchor(other.value()));
    if (hasCast)
        results.append(m_out.anchor(result.value));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    defineObjectMadeLater(copyID, CellPointer(m_out.phi(Int64, copies)));
    if (!hasCast)
        return undefined();
    return Operand(repOfCast, m_out.phi(b3TypeOf(repOfCast), results));
}

// `{ ...source }`. See operationAOTTypedCloneObject, and Typed::Object::clone() for what is done in place.
Lowering::DefinedOnCopy Lowering::whatIsDefinedOnCopy(ValueID object, bool mayCarryOut)
{
    DefinedOnCopy result { 0, { } };
    auto& values = m_currentBlock->values;
    m_blockOfDefinitionsMadeWithCopy = m_currentBlock;
    m_definitionsMadeWithCopy.clear();
    for (size_t index = m_currentIndexInBlock + 1; mayCarryOut && index < values.size() && result.values.size() < 2; ++index) {
        auto& user = m_function.at(values[index]);
        if (user.opcode == Opcode::String)
            continue;
        if (!isDefineField(user) || user.arguments.size() != 3 || user.arguments[0] != object || objectLiteralAt(index) || propertyRunAt(index))
            break;
        auto& key = m_function.at(user.arguments[1]);
        size_t whereMade = values.find(user.arguments[2]);
        if (key.opcode != Opcode::String || key.string().index() + 1 >= (1u << 24) || user.arguments[2] == object || (whereMade != notFound && whereMade > m_currentIndexInBlock))
            break;
        result.more |= static_cast<uint64_t>(key.string().index() + 1) << (24 * result.values.size());
        result.values.append(user.arguments[2]);
        m_definitionsMadeWithCopy.append(index);
    }
    uint64_t others = 0;
    for (auto& block : m_function.blocks) {
        for (size_t index = 0; index < block.values.size(); ++index) {
            auto& user = m_function.at(block.values[index]);
            if (&block == m_currentBlock && m_definitionsMadeWithCopy.contains(index))
                continue;
            others += (user.opcode == Opcode::PutByVal && withoutUpcasts(user.child(0)) == object) || (isDefineField(user) && !user.arguments.isEmpty() && withoutUpcasts(user.arguments[0]) == object);
        }
    }
    result.more |= std::min<uint64_t>(others, 255) << 48;
    return result;
}

CellPointer Lowering::cloneObject(ValueID sourceID, bool keepsLayout, const DefinedOnCopy& defined)
{
    if (!keepsLayout)
        return cloneAsPlainObject(sourceID, defined);
    Boxed source = in<Boxed>(sourceID);
    auto outOfLine = [&] {
        return AOT_TYPED_OPERATION(CloneObject, source, word32(keepsLayout));
    };
    auto& type = typeOf(sourceID);
    Vector<LayoutID, 2> layouts;
    if (keepsLayout && !Options::auditAOTTypedAssertions() && !Options::countAOTTypedAllocations())
        layouts = layoutsClonedInPlace(type);
    if (layouts.isEmpty())
        return outOfLine();

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    orElse(isCell(source, type), slowPath);
    CellPointer cell = asCell(source);
    Word32 shapeWord = shapeWordOf(cell);
    Word32 orderWord = orderWordOf(cell);
    orElse(lacksBits(orderWord, orderWordHasExtraProperties), slowPath);
    for (LayoutID id : layouts) {
        auto& layout = m_program.at(id);
        LBasicBlock isOfLayout = m_out.newBlock();
        LBasicBlock otherwise = id == layouts.last() ? slowPath : m_out.newBlock();
        m_out.branch(isZero(add(shapeWord, word32(layout.shape))).value(), usually(isOfLayout), rarely(otherwise));
        m_out.appendTo(isOfLayout);
        unlessHasNumberBox(cell, layout, slowPath);
        CellPointer copy = copyRecord(cell, orderWord, layout, { });
        fenceAfterInitialization();
        results.append(m_out.anchor(copy.value()));
        m_out.jump(continuation);
        if (otherwise != slowPath)
            m_out.appendTo(otherwise);
    }
    m_out.appendTo(slowPath);
    results.append(m_out.anchor(outOfLine().value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, results));
}

// What is handed a fresh, empty object for its first argument and can make the object itself.
bool Lowering::makesItsFirstArgument(const Value& user) const
{
    if (user.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(user.builtin()).implementation;
    if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ObjectAssign)
        return true;
    return implementation.kind == BuiltinImplementation::Operation && implementation.operation == LanguageOperation::CopyDataProperties;
}

// A NewObject that is not allocated: the first value that uses it makes it, and defines it (defineObjectMadeLater()). Nobody has the object before that.
std::optional<size_t> Lowering::indexOfUseThatMakes(ValueID id) const
{
    // There every value is looked at by itself, where it is defined. What a handler watches is carried out where it stands.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    if (m_function.at(id).opcode != Opcode::NewObject)
        return std::nullopt;
    if (m_function.at(id).hasRowOfBirth() && m_program.rowsOfBirths[m_function.at(id).rowOfBirth()].isWhole)
        return std::nullopt;
    auto& block = m_currentBlock->values;
    size_t index = block.find(id);
    if (index == notFound)
        return std::nullopt;
    for (size_t k = index + 1; k < block.size(); ++k) {
        auto& user = m_function.at(block[k]);
        unsigned uses = 0;
        user.forEachChild([&](ValueID child) {
            uses += child == id;
        });
        if (uses)
            return uses == 1 && makesItsFirstArgument(user) && user.arguments.size() >= 2 && user.arguments[0] == id ? std::optional(k) : std::nullopt;
    }
    return std::nullopt;
}

const Lowering::NamedStores* Lowering::namedStoresAt(size_t index)
{
    // Fewer are not worth the code: the values and names go through memory.
    static constexpr unsigned minStores = 3;
    static constexpr unsigned maxStores = 8;
    if (m_blockOfNamedStores != m_currentBlock) {
        m_blockOfNamedStores = m_currentBlock;
        m_namedStores.clear();
        auto& values = m_currentBlock->values;
        NamedStores open;
        auto close = [&] {
            if (open.size() >= minStores)
                m_namedStores.append(open);
            open.clear();
        };
        // What a handler watches ends its block.
        for (size_t i = 0; i < values.size() && m_currentBlock->terminator.kind != TerminatorKind::Invoke; ++i) {
            auto& value = m_function.at(values[i]);
            switch (value.opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::String:
            case Opcode::ToBoolean:
            case Opcode::LogicalNot:
                continue;
            case Opcode::PutById:
                if (!value.isInitialStore() && value.string() != m_length && typeOf(value.child(0)).onlyEngineClass() == &m_builtins.arrayClass()) {
                    bool goesOn = open.size() < maxStores && std::ranges::all_of(open, [&](size_t earlier) {
                        auto& store = m_function.at(values[earlier]);
                        return store.child(0) == value.child(0) && store.string() != value.string();
                    });
                    if (!goesOn)
                        close();
                    open.append(i);
                    continue;
                }
                break;
            default:
                break;
            }
            close();
        }
        close();
    }
    for (auto& stores : m_namedStores) {
        if (stores.contains(index))
            return &stores;
    }
    return nullptr;
}

// Where the last of them stands. As a rule the array has none of the properties yet: then they are added together, with one change of its structure and one of its storage. Otherwise nothing
// has been done, and each store goes the general way, as if it stood alone.
void Lowering::lowerNamedStores(const NamedStores& stores)
{
    auto& values = m_currentBlock->values;
    auto storeAt = [&](unsigned i) -> const Value& {
        return m_function.at(values[stores[i]]);
    };
    unsigned count = stores.size();
    CellPointer cell;
    {
        SetForScope currentIndex(m_currentIndexInBlock, stores[0]);
        cell = nonNil(storeAt(0).child(0), storeAt(0).string());
    }
    // The values, then the names.
    LValue buffer = m_out.lockedStackSlot(2 * count * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < count; ++i) {
        m_out.store64(in<Boxed>(storeAt(i).child(1)).value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        m_out.store64(m_out.constInt64(storeAt(i).string().index()), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, (count + i) * sizeof(EncodedJSValue)));
    }
    LBasicBlock oneByOne = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Word32 isDone = AOT_TYPED_OPERATION(TryAddNamedProperties, cell, word32(count), Pointer(buffer));
    m_out.branch(isDone.value(), usually(continuation), rarely(oneByOne));
    m_out.appendTo(oneByOne);
    // The values are still where the operation was to take them from: nothing but the array is kept over the call for this.
    for (unsigned i = 0; i < count; ++i) {
        SetForScope currentIndex(m_currentIndexInBlock, stores[i]);
        emitPutById(box(cell), storeAt(i).string(), Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)))));
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

ValueID Lowering::withoutUpcasts(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
        id = m_function.at(id).child(0);
    return id;
}

bool Lowering::isLazyClosureStore(const Value& closure, const Value& store)
{
    if (store.opcode != Opcode::PutById || store.isInitialStore() || store.isAtKnownPlace() || Options::auditAOTTypedAssertions() || closure.opcode != Opcode::NewClosure || closure.arguments.size() != 1)
        return false;
    if (withoutUpcasts(closure.arguments[0]) != withoutUpcasts(store.child(0)) || !typeOf(store.child(0)).anyObject || fieldFor(typeOf(store.child(0)), store.string()))
        return false;
    auto& callee = m_program.at(closure.function());
    if (callee.kind == FunctionKind::Constructor || m_program.isResumeFunction(callee) || m_program.referencesAndCapturesOfClosures(callee) != std::pair(1u, 1u))
        return false;
    return !isPrivateName(m_program.string(store.string()));
}

void Lowering::putLazyClosure(Boxed holder, StringID name, FunctionID function)
{
    constexpr unsigned isDirect = 1;
    constexpr unsigned isStrict = 2;
    auto slot = allocateSite(name, isDirect | isStrict | AOT::Site::storesLazyClosure);
    if (!slot) {
        AOT_TYPED_OPERATION(PutLazyClosure, holder, word32(genericEntrypointFor(function).index()), word32(name.index()));
        return;
    }
    Boxed cell = [&] {
        SetForScope fetchesInLine(m_isInLoop, true);
        return box(closureConstant(function));
    }();
    Pointer address = slotAddress(*slot);
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(holder).value(), usually(isCellCase), rarely(miss));
    m_out.appendTo(isCellCase);
    CellPointer object = asCell(holder);
    TypedPointer structureID(m_heaps.root, m_out.add(object.value(), m_out.constIntPtr(JSCell::structureIDOffset())));
    Integer word(m_out.load64(TypedPointer(m_heaps.root, address.value())));
    static_assert(OBJECT_OFFSETOF(AOT::Slot, newStructureID) == sizeof(uint64_t));
    LValue newStructureID = m_out.load32(TypedPointer(m_heaps.root, m_out.add(address.value(), m_out.constIntPtr(sizeof(uint64_t)))));
    m_out.branch(equal(low32(word), Word32(m_out.load32(structureID))).value(), usually(hit), rarely(miss));
    m_out.appendTo(hit);
    LValue location = m_out.aShr(m_out.shl(word.value(), m_out.constInt32(32 - AOT::Slot::offsetBits)), m_out.constInt32(64 - AOT::Slot::offsetBits));
    LValue storage = m_out.select(m_out.lessThan(location, m_out.constInt64(0)), m_out.load64(TypedPointer(m_heaps.root, m_out.add(object.value(), m_out.constIntPtr(JSObject::butterflyOffset())))), object.value());
    m_out.store64(cell.value(), TypedPointer(m_heaps.root, m_out.add(storage, m_out.shl(location, m_out.constInt32(3)))));
    m_out.store32(newStructureID, structureID);
    m_out.jump(continuation);
    inColdBlock(miss, [&] {
        callICStub(Stub::PutById, Void, { { holder.value(), firstStubOperandGPR }, { cell.value(), GPRInfo::argumentGPR1 }, { slotAddress(*slot, SlotAddressIsFor::ColdBlock).value(), GPRInfo::argumentGPR2 } });
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

std::optional<FunctionID> Lowering::lazyClosureStoredBy(const Value& store)
{
    if (store.opcode != Opcode::PutById)
        return std::nullopt;
    ValueID stored = store.child(1);
    auto& closure = m_function.at(stored);
    if (!isLazyClosureStore(closure, store) || numberOfUsesOf(stored) != 1 || !m_currentBlock->values.contains(stored))
        return std::nullopt;
    return closure.function();
}

const Value* Lowering::lazyClosureStoreBehindJump(const Block& from)
{
    if (from.terminator.kind != TerminatorKind::Jump || from.values.isEmpty())
        return nullptr;
    ValueID closure = from.values.last();
    auto& target = from.terminator.targets[0];
    size_t index = target.arguments.find(closure);
    if (m_function.at(closure).opcode != Opcode::NewClosure || numberOfUsesOf(closure) != 1 || index == notFound)
        return nullptr;
    auto& to = m_function.at(target.block);
    if (to.values.isEmpty() || (to.terminator.kind == TerminatorKind::Invoke && to.values.size() == 1) || numberOfUsesOf(to.parameters[index]) != 1)
        return nullptr;
    auto& store = m_function.at(to.values.first());
    return store.opcode == Opcode::PutById && store.child(1) == to.parameters[index] && !to.parameters.contains(withoutUpcasts(store.child(0))) && isLazyClosureStore(m_function.at(closure), store) ? &store : nullptr;
}

bool Lowering::mayBeStoredAheadOfJump(const Value& store)
{
    if (store.opcode != Opcode::PutById || m_currentBlock->values.isEmpty() || &m_function.at(m_currentBlock->values.first()) != &store || !m_currentBlock->parameters.contains(store.child(1)))
        return false;
    for (auto& from : m_function.blocks) {
        if (lazyClosureStoreBehindJump(from) == &store)
            return true;
    }
    return false;
}

bool Lowering::isStoredAsLazyClosure(ValueID closure)
{
    if (numberOfUsesOf(closure) != 1)
        return false;
    auto& values = m_currentBlock->values;
    for (size_t i = m_currentIndexInBlock + 1; i < values.size(); ++i) {
        auto& user = m_function.at(values[i]);
        if (user.opcode == Opcode::PutById && user.child(1) == closure)
            return !!lazyClosureStoredBy(user);
    }
    return false;
}

std::optional<Lowering::StoreInPropertyRun> Lowering::storeInPropertyRunAt(size_t index)
{
    auto& value = m_function.at(m_currentBlock->values[index]);
    StoreInPropertyRun store;
    switch (value.opcode) {
    case Opcode::PutById:
        if (value.isInitialStore() || value.isAtKnownPlace() || value.isFirstStoreToThis() || lazyClosureStoredBy(value) || mayBeStoredAheadOfJump(value))
            return std::nullopt;
        store = { index, value.child(0), value.string(), { }, value.child(1), false };
        break;
    case Opcode::PutByVal:
        if (!isExpectedToBeSymbol(m_function, value.child(1)))
            return std::nullopt;
        store = { index, value.child(0), { }, value.child(1), value.child(2), false };
        break;
    case Opcode::CallBuiltin:
        if (!isDefineField(value) || m_function.at(value.arguments[1]).opcode != Opcode::String)
            return std::nullopt;
        store = { index, value.arguments[0], m_function.at(value.arguments[1]).string(), { }, value.arguments[2], true };
        break;
    default:
        return std::nullopt;
    }
    if (!typeOf(store.object).anyObject || m_function.at(store.object).prediction || objectLiteralAt(index))
        return std::nullopt;
    if (store.name) {
        auto text = m_program.string(store.name);
        bool mayBeIndex = !text.empty() && std::ranges::all_of(text, [](uint8_t character) {
            return isASCIIDigit(character);
        });
        // The one accessor of Object.prototype that a store by name finds.
        if (mayBeIndex || isPrivateName(text) || equalSpans(text, "__proto__"_s.span8()) || fieldFor(typeOf(store.object), store.name))
            return std::nullopt;
    }
    return store;
}

// It changes nothing, cannot throw, and reads nothing but variables, which are read again where that could tell: see lowerPropertyRun().
bool Lowering::maySeparateStoresOfPropertyRun(size_t index)
{
    auto& value = m_function.at(m_currentBlock->values[index]);
    // What is made of the value of a variable is not made again.
    auto isNoVariable = [&](ValueID id) {
        return m_function.at(withoutUpcasts(id)).opcode != Opcode::GetModuleVariable;
    };
    switch (value.opcode) {
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
    case Opcode::FunctionReference:
    case Opcode::BuiltinFunction:
        return true;
    case Opcode::Cast:
        return value.castReason() == CastReason::Upcast;
    case Opcode::GetModuleVariable:
        return !m_program.variables[value.variable().index()].mayBeSeenUninitialized();
    case Opcode::NewArray:
    case Opcode::NewClosure:
        return std::ranges::all_of(value.arguments, isNoVariable);
    case Opcode::LogicalNot:
    case Opcode::IsUndefinedOrNull:
    case Opcode::TypeOfIs:
    case Opcode::ToBoolean:
        return isNoVariable(value.child(0));
    case Opcode::StrictEq:
        return isNoVariable(value.child(0)) && isNoVariable(value.child(1));
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
    case Opcode::Negate:
    case Opcode::BitNot:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        // On numbers.
        return writesOfArithmetic(typeOf(value.child(0)), value.child(1) ? &typeOf(value.child(1)) : nullptr) == Writes::Nothing && isNoVariable(value.child(0)) && (!value.child(1) || isNoVariable(value.child(1)));
    // A literal of an ordinary object.
    case Opcode::NewObject:
    case Opcode::CallBuiltin: {
        auto* literal = objectLiteralAt(index);
        return literal && !literal->newTarget && !literal->descriptor && (literal->allocation == index || isNoVariable(value.arguments[2]));
    }
    default:
        return false;
    }
}

std::optional<FunctionID> Lowering::functionWhosePrototypeIsRead(ValueID id) const
{
    auto& value = m_function.at(withoutUpcasts(id));
    if (!m_program.runsOnce(m_id) || m_isInLoop || value.opcode != Opcode::GetById || !equalSpans(m_program.string(value.string()), "prototype"_s.span8()))
        return std::nullopt;
    auto& function = m_function.at(withoutUpcasts(value.child(0)));
    if (function.opcode != Opcode::FunctionReference)
        return std::nullopt;
    return function.function();
}

const Lowering::PropertyRun* Lowering::propertyRunAt(size_t index)
{
    // Fewer are not worth the code.
    static constexpr unsigned minStores = 3;
    // operationAOTTypedTryPutProperties has a bit for each.
    static constexpr unsigned maxStores = 64;
    static constexpr unsigned maxStoresThatReplace = 32;
    if (m_blockOfPropertyRuns != m_currentBlock) {
        m_blockOfPropertyRuns = m_currentBlock;
        m_propertyRuns.clear();
        auto& values = m_currentBlock->values;
        PropertyRun open;
        auto& stores = open.stores;
        auto close = [&] {
            while (!open.elementStores.isEmpty() && (stores.isEmpty() || open.elementStores.last() > stores.last().index))
                open.elementStores.removeLast();
            unsigned numberOfSlots = 1;
            for (auto& store : stores)
                numberOfSlots += !!store.key;
            open.mayReplace = numberOfSlots == 1 && open.elementStores.isEmpty() && !open.mayBePrototype && !stores.isEmpty() && stores.size() <= maxStoresThatReplace && (m_isInLoop || m_program.isHotOutsideItsLoops(m_id));
            if (numberOfSlots == 1)
                numberOfSlots = 2;
            if (open.mayReplace)
                numberOfSlots += (stores.size() - 1) / sizeof(void*);
            if (stores.size() >= minStores && m_context.sites.size() + numberOfSlots <= std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
                open.slot = m_context.sites.size();
                for (unsigned k = 0; k < numberOfSlots; ++k)
                    m_context.sites.append(Site { });
                m_propertyRuns.append(open);
            }
            stores.clear();
            open.elementStores.clear();
            open.mayBePrototype = false;
        };
        auto isSameValueOrVariable = [&](ValueID a, ValueID b) {
            auto& first = m_function.at(withoutUpcasts(a));
            auto& second = m_function.at(withoutUpcasts(b));
            if (&first == &second || (first.opcode == Opcode::GetModuleVariable && second.opcode == Opcode::GetModuleVariable && first.variable() == second.variable()))
                return true;
            auto function = functionWhosePrototypeIsRead(a);
            return function && function == functionWhosePrototypeIsRead(b);
        };
        bool defersElementStores = m_program.runsOnce(m_id) && !m_isInLoop;
        auto isElementStoreTo = [&](size_t i, ValueID object) {
            auto& value = m_function.at(values[i]);
            if (!defersElementStores || value.opcode != Opcode::PutByVal || !isSameValueOrVariable(value.child(0), object))
                return false;
            auto& key = m_function.at(withoutUpcasts(value.child(1)));
            return key.opcode == Opcode::Integer && key.integer() >= 0 && key.integer() <= static_cast<int64_t>(MAX_ARRAY_INDEX);
        };
        // What a handler watches ends its block.
        for (size_t i = 0; i < values.size() && m_currentBlock->terminator.kind != TerminatorKind::Invoke; ++i) {
            if (auto store = storeInPropertyRunAt(i)) {
                bool goesOn = stores.size() < maxStores && std::ranges::all_of(stores, [&](auto& earlier) {
                    if (!isSameValueOrVariable(earlier.object, store->object))
                        return false;
                    return store->key ? !earlier.key || !isSameValueOrVariable(earlier.key, store->key) : earlier.name != store->name;
                });
                if (!goesOn)
                    close();
                stores.append(*store);
                open.mayBePrototype |= !!functionWhosePrototypeIsRead(store->object);
                continue;
            }
            if (!stores.isEmpty() && isElementStoreTo(i, stores.first().object)) {
                open.elementStores.append(i);
                continue;
            }
            if (!maySeparateStoresOfPropertyRun(i) && !functionWhosePrototypeIsRead(values[i]))
                close();
        }
        close();
    }
    for (auto& run : m_propertyRuns) {
        bool hasIt = run.elementStores.contains(index) || run.stores.containsIf([&](auto& store) {
            return store.index == index;
        });
        if (hasIt)
            return &run;
    }
    return nullptr;
}

// Where the last of them stands. As a rule the object has none of the properties yet and is of the structure it was of the last time, which the site remembers: then the values are stored one behind the other, and the object gets the structure that has
// them all. Otherwise the run time is asked to do the same for the structure at hand. Where it cannot (a setter on the prototype chain, a property that is there already, no ordinary object) nothing has been done, and everything from the first store on is
// carried out in its order: each store the general way, at its own position, and each variable read again, because a store may have run code.
void Lowering::lowerPropertyRun(const PropertyRun& run)
{
    auto& block = m_currentBlock->values;
    auto& stores = run.stores;
    unsigned count = stores.size();
    Boxed base = in<Boxed>(stores[0].object);
    Vector<Boxed, 8> values;
    Vector<Boxed, 8> keys;
    uint64_t keyed = 0;
    bool storesCells = false;
    for (unsigned i = 0; i < count; ++i) {
        values.append(in<Boxed>(stores[i].stored));
        keys.append(stores[i].key ? in<Boxed>(stores[i].key) : Boxed());
        keyed |= static_cast<uint64_t>(!!stores[i].key) << i;
        storesCells |= typeOf(stores[i].stored).hasCells();
    }
    Pointer cache = slotAddress(run.slot);
    auto inCache = [&](size_t offset) {
        // A collection may clear a slot.
        return TypedPointer(m_heaps.AOTTyped_collectorState, m_out.add(cache.value(), m_out.constIntPtr(offset)));
    };
    LBasicBlock miss = newColdBlock();
    LBasicBlock ask = newColdBlock();
    LBasicBlock oneByOne = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();

    static_assert(!OBJECT_OFFSETOF(AOT::Slot, structureID) && OBJECT_OFFSETOF(AOT::Slot, offset) == sizeof(uint32_t));
    Integer word(m_out.load64(inCache(0)));
    bool hasElementStores = !run.elementStores.isEmpty();
    if (hasElementStores || run.mayBePrototype)
        orElse(boolean(false), miss);
    if (get(stores[0].object).rep != Rep::Cell)
        orElse(isCell(base), miss);
    CellPointer cell = asCell(base);
    auto* slotOfBuffer = m_out.m_proc.addStackSlot(2 * count * sizeof(EncodedJSValue));
    auto addressOfBuffer = [&]() -> LValue {
        return m_out.m_block->appendNew<B3::SlotBaseValue>(m_out.m_proc, m_out.origin(), slotOfBuffer);
    };
    LValue buffer = nullptr;
    auto inBuffer = [&](unsigned i) {
        return m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue));
    };
    auto writeValuesToBuffer = [&] {
        buffer = addressOfBuffer();
        for (unsigned i = 0; i < count; ++i) {
            m_out.store64(values[i].value(), inBuffer(i));
            if (keys[i])
                m_out.store64(keys[i].value(), inBuffer(count + i));
        }
    };
    LBasicBlock isNotKnown = newColdBlock();
    auto putWithStorage = [&] {
        LBasicBlock areStored = newColdBlock();
        LValue isDone = callICStub(Stub::PutPropertiesWithStorage, Int64, { { cell.value(), GPRInfo::argumentGPR1 }, { m_out.add(cache.value(), m_out.constIntPtr(sizeof(AOT::Slot))), GPRInfo::argumentGPR2 }, { buffer, GPRInfo::argumentGPR3 } }, std::pair { stubTemporaryGPRs[0], count }, { Writes::Heap, false });
        m_out.branch(m_out.notZero64(isDone), usually(areStored), rarely(isNotKnown));
        m_out.appendTo(areStored);
        if (storesCells)
            barrier(cell);
        m_out.jump(continuation);
    };
    if (run.mayReplace) {
        LBasicBlock adds = m_out.newBlock();
        LBasicBlock isNotFirst = newUnlikelyBlock();
        LBasicBlock isKnown = newUnlikelyBlock();
        LBasicBlock replaces = newUnlikelyBlock();
        LBasicBlock hasStorage = newColdBlock();
        Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID);
        m_out.branch(equal(low32(word), structureID).value(), unsure(adds), unsure(isNotFirst));
        m_out.appendTo(isNotFirst);
        Integer second(m_out.load64(inCache(sizeof(AOT::Slot))));
        m_out.branch(equal(low32(second), structureID).value(), unsure(isKnown), rarely(miss));
        m_out.appendTo(isKnown);
        m_out.branch(m_out.testNonZero64(second.value(), m_out.constInt64(static_cast<uint64_t>(AOT::Slot::pointerIsNotCell) << 32)), usually(replaces), rarely(hasStorage));
        m_out.appendTo(replaces);
        didEmitWrite(Writes::Heap);
        LValue places = nullptr;
        for (unsigned i = 0; i < count; ++i) {
            if (!(i % sizeof(void*)))
                places = m_out.load64(inCache((1 + i / sizeof(void*)) * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer)));
            LValue place = m_out.bitAnd(m_out.lShr(places, m_out.constInt32(i % sizeof(void*) * 8)), m_out.constInt64(0xff));
            m_out.store64(values[i].value(), TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(cell.value(), m_out.shl(place, m_out.constInt32(3)))));
        }
        if (storesCells)
            barrier(cell);
        m_out.jump(continuation);
        m_out.appendTo(hasStorage);
        writeValuesToBuffer();
        putWithStorage();
        m_out.appendTo(adds);
    } else
        orElse(equal(low32(word), loadField<Word32>(cell, m_heaps.JSCell_structureID)), miss);
    unsigned numberOfKeys = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (keys[i])
            orElse(equal(Boxed(m_out.load64(inCache(++numberOfKeys * sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, pointer)))), keys[i]), miss);
    }
    didEmitWrite(Writes::Heap);
    LValue location = m_out.bitAnd(m_out.lShr(word.value(), m_out.constInt32(32)), m_out.constInt64(AOT::Slot::directLocationMask));
    LValue first = m_out.add(cell.value(), m_out.shl(location, m_out.constInt32(3)));
    for (unsigned i = 0; i < count; ++i)
        m_out.store64(values[i].value(), m_out.address(m_heaps.properties.atAnyNumber(), first, i * sizeof(EncodedJSValue)));
    // The collector goes by the structure, which comes last.
    m_out.store32(m_out.load32(inCache(OBJECT_OFFSETOF(AOT::Slot, newStructureID))), cell.value(), m_heaps.JSCell_structureID);
    if (storesCells)
        barrier(cell);
    m_out.jump(continuation);

    m_out.appendTo(miss);
    // The values, then the keys and the numbers of the names.
    writeValuesToBuffer();
    // They do not all fit in the object, and the site knows where they go in one of this structure.
    if (!keyed && !hasElementStores && !run.mayBePrototype && !run.mayReplace) {
        LBasicBlock isObject = newColdBlock();
        LBasicBlock isKnown = newColdBlock();
        m_out.branch(isCell(base).value(), unsure(isObject), unsure(isNotKnown));
        m_out.appendTo(isObject);
        m_out.branch(equal(Word32(m_out.load32(inCache(sizeof(AOT::Slot) + OBJECT_OFFSETOF(AOT::Slot, structureID)))), loadField<Word32>(cell, m_heaps.JSCell_structureID)).value(), unsure(isKnown), unsure(isNotKnown));
        m_out.appendTo(isKnown);
        putWithStorage();
    } else
        m_out.jump(isNotKnown);
    m_out.appendTo(isNotKnown);
    if (run.mayReplace)
        buffer = addressOfBuffer();
    // The run time has refused too often: an empty slot that says so. See operationAOTTypedTryPutProperties.
    constexpr uint64_t refusesForNow = static_cast<uint64_t>(AOT::Slot::attemptsMask | AOT::Slot::isGetter) << 32;
    m_out.branch(equal(Integer(m_out.bitAnd(word.value(), integer(refusesForNow | UINT32_MAX).value())), integer(refusesForNow)).value(), rarely(oneByOne), usually(ask));
    m_out.appendTo(ask);
    for (unsigned i = 0; i < count; ++i) {
        if (!keys[i])
            m_out.store64(m_out.constInt64(stores[i].name.index()), inBuffer(count + i));
    }
    Word32 isDone = AOT_TYPED_OPERATION(TryPutProperties, cache, base, word32(count | (hasElementStores ? 1u << 31 : 0) | (run.mayBePrototype ? 1u << 30 : 0) | (run.mayReplace ? 1u << 29 : 0)), integer(keyed), Pointer(buffer));
    LBasicBlock namesAreStored = hasElementStores ? m_out.newBlock() : continuation;
    m_out.branch(isDone.value(), usually(namesAreStored), rarely(oneByOne));
    if (hasElementStores) {
        m_out.appendTo(namesAreStored);
        for (size_t index : run.elementStores) {
            SetForScope currentIndex(m_currentIndexInBlock, index);
            auto& store = m_function.at(block[index]);
            emitPutByVal(base, in<Boxed>(store.child(1)), in<Boxed>(store.child(2)), false);
        }
        m_out.jump(continuation);
    }

    m_out.appendTo(oneByOne);
    Vector<std::pair<ValueID, Boxed>, 4> readAgain;
    auto readAgainAs = [&](ValueID id) {
        ValueID source = withoutUpcasts(id);
        for (auto& [variableRead, value] : readAgain) {
            if (variableRead == source)
                return value;
        }
        return Boxed();
    };
    // Nothing but the object is kept over the calls for this.
    auto asOfNow = [&](ValueID id, Boxed early, unsigned inBufferAt) {
        if (Boxed value = readAgainAs(id))
            return value;
        return early.value()->hasInt64() ? early : Boxed(m_out.load64(inBuffer(inBufferAt)));
    };
    unsigned next = 0;
    for (size_t index = stores.first().index; index <= stores.last().index; ++index) {
        SetForScope currentIndex(m_currentIndexInBlock, index);
        auto& value = m_function.at(block[index]);
        if (value.opcode == Opcode::GetModuleVariable) {
            readAgain.append({ block[index], convertTo<Boxed>(lowerGetModuleVariable(block[index], value), typeOf(block[index])) });
            continue;
        }
        if (run.mayBePrototype && functionWhosePrototypeIsRead(block[index])) {
            readAgain.append({ block[index], emitGetById(in<Boxed>(value.child(0)), value.string()) });
            continue;
        }
        if (run.elementStores.contains(index)) {
            Boxed receiver = readAgainAs(value.child(0));
            Boxed stored = readAgainAs(value.child(2));
            emitPutByVal(receiver ? receiver : base, in<Boxed>(value.child(1)), stored ? stored : in<Boxed>(value.child(2)), false);
            continue;
        }
        if (index != stores[next].index)
            continue;
        auto& store = stores[next];
        Boxed stored = asOfNow(store.stored, values[next], next);
        Boxed receiver = readAgainAs(store.object);
        if (!receiver)
            receiver = base;
        if (store.key)
            emitPutByVal(receiver, asOfNow(store.key, keys[next], count + next), stored, true);
        else if (!store.isDefinition)
            emitPutById(receiver, store.name, stored);
        else if (!emitDefineByName(receiver, store.name, stored))
            AOT_TYPED_OPERATION(DefineByName, receiver, word32(store.name.index()), stored);
        ++next;
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::lowerPutById(ValueID, const Value& value)
{
    if (auto function = lazyClosureStoredBy(value)) {
        putLazyClosure(in<Boxed>(value.child(0)), value.string(), *function);
        return;
    }
    if (mayBeStoredAheadOfJump(value)) {
        LBasicBlock isNotStoredYet = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Boxed stored = in<Boxed>(value.child(1));
        m_out.branch(m_out.isZero64(stored.value()), unsure(continuation), unsure(isNotStoredYet));
        m_out.appendTo(isNotStoredYet);
        emitPutById(in<Boxed>(value.child(0)), value.string(), stored);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }
    if (m_cloneGroup && m_cloneGroup->stores.contains(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == m_cloneGroup->last())
            lowerCloneGroup(*std::exchange(m_cloneGroup, std::nullopt));
        return;
    }
    if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == literal->fields.last())
            lowerObjectLiteral(*literal);
        return;
    }
    if (auto stores = initialStoresAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == stores->first)
            lowerInitialStores(*stores);
        return;
    }
    if (auto* stores = namedStoresAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == stores->last())
            lowerNamedStores(*stores);
        return;
    }
    if (auto* run = propertyRunAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == run->stores.last().index)
            lowerPropertyRun(*run);
        return;
    }
    ValueID object = value.child(0);
    ValueID stored = value.child(1);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    const Field* field = fieldFor(receiver, name);
    if (!field) {
        auto generally = [&]() -> Operand {
            emitPutById(in<Boxed>(object), name, in<Boxed>(stored));
            return { };
        };
        if (value.isFirstStoreToThis() && value.birthOfFirstStore() && !Options::validateAOTTypedFacts() && !Options::auditAOTTypedAssertions()) {
            if (!value.knownPlace())
                giveThisItsRow(value, in<Boxed>(object));
            if (!value.isDefinitionInRowOfThis())
                emitCachedPutByIdOnItsOwn(in<Boxed>(object), name, in<Boxed>(stored), false, AOT::Site::isOfRowOfThis);
            else if (!emitDefineByName(in<Boxed>(object), name, in<Boxed>(stored)))
                AOT_TYPED_OPERATION(DefineByName, in<Boxed>(object), word32(name.index()), in<Boxed>(stored));
            return;
        }
        if (value.isDefinitionInRowOfThis()) {
            if (!emitDefineByName(in<Boxed>(object), name, in<Boxed>(stored)))
                AOT_TYPED_OPERATION(DefineByName, in<Boxed>(object), word32(name.index()), in<Boxed>(stored));
            return;
        }
        if (value.isAtKnownPlace() && !(value.isStoreToObjectBornWhole() && value.birthOfFirstStore() && !m_program.rowsOfBirths[value.birthOfFirstStore() - 1].isWhole)) {
            putAtKnownPlace(value, in<Boxed>(object), in<Boxed>(stored));
            return;
        }
        auto expected = predictionOf(object);
        const Field* expectedField = expected ? fieldExpectedFor(*expected, name) : nullptr;
        if (!expectedField || hasMemberThatKeepsField(expected->onlyView(), name) || m_program.at(expectedField->slot).isDiscriminant || (!m_isInLoop && !m_program.isHotByText(m_id))) {
            generally();
            return;
        }
        // What a store that was not compiled with the type of the receiver may store: whatever the slot holds.
        Type held = m_program.at(expectedField->stored).withoutPrimitives(Primitive::Absent);
        if (expectedField->type && m_program.at(expectedField->type).withoutPrimitives(Primitive::Absent).isWithin(held, m_program))
            held = m_program.at(expectedField->type).withoutPrimitives(Primitive::Absent);
        ifAsExpected({ { object, &*expected }, { stored, &held, false, true } }, Rep::Void, [&](LBasicBlock otherwise) -> Operand {
            LayoutID view = expected->onlyView();
            // [[Set]] asks the prototype chain before it creates a property.
            // A number that is not kept in place leaves for the general way, which must not find the key there already.
            bool createsInPlace = m_program.at(expectedField->stored).primitives & Primitive::Absent && !inheritsByItsLayout(m_program, view, name);
            LBasicBlock isStored = createsInPlace && expectedField->store == Store::Int32OrElsewhere ? m_out.newBlock() : nullptr;
            CellPointer cell = withFieldOrElse(object, view, *expectedField, otherwise, createsInPlace);
            if (mayBeFrozen(view))
                orElse(lacksBits(orderWordOf(cell), orderWordIsFrozen), otherwise);
            didEmitWrite(Writes::Fields);
            guardPinnedFields(view, *expectedField, stored);
            // As in writeField().
            if (createsInPlace) {
                LBasicBlock creates = m_out.newBlock();
                LBasicBlock continuation = m_out.newBlock();
                m_out.branch(isFieldAbsent(cell, *expectedField, 0).value(), unsure(creates), unsure(continuation));
                m_out.appendTo(creates);
                Operand numberInPlace = isStored ? fitOrElse(get(stored), typeOf(stored), numbersKeptInPlace(), otherwise) : Operand { };
                auto unlessAsked = [&] {
                    if (uint32_t bits = (mayInherit(m_program, view, name) ? orderWordHasExtraProperties : 0) | (mayBeFrozen(view) ? orderWordIsNotExtensible : 0))
                        orElse(lacksBits(orderWordOf(cell), bits), otherwise);
                };
                OperationEffects effects { Writes::Fields, false, false, true };
                if (Options::countAOTTypedAllocations()) {
                    unlessAsked();
                    AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(name.index()));
                } else
                    didCreateFieldInCell(cell, *expectedField, effects, unlessAsked);
                if (isStored)
                    store(expectedField->store, low32(numberInPlace.as<Integer>()), fieldAt(cell, *expectedField));
                m_out.jump(isStored ? isStored : continuation);
                m_out.appendTo(continuation);
            }
            if (expectedField->store == Store::Int32OrElsewhere)
                storeInt32OrElsewhere(cell, *expectedField, stored, false, otherwise);
            else
                store(expectedField->store, in(stored, repOf(expectedField->store)), fieldAt(cell, *expectedField));
            if (isStored) {
                m_out.jump(isStored);
                m_out.appendTo(isStored);
            }
            if (holdsCells(expectedField->store) && held.hasCells())
                barrier(cell);
            return { };
        }, generally);
        return;
    }
    // The object is of the layout itself.
    if (isLoweringInitializingStore()) {
        writeField(in<CellPointer>(object), object, *field, stored);
        return;
    }
    LayoutID view = receiver.onlyView();
    CellPointer cell = nonNil(object, name);
    throughView(cell, object, view, *field, Rep::Void, [&]() -> Operand {
        // `#name` is no property: it stays writable.
        if (mayBeFrozen(view) && !isPrivateName(m_program.string(name))) {
            LBasicBlock asks = newColdBlock();
            LBasicBlock stores = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(lacksBits(orderWordOf(cell), orderWordIsFrozen).value(), usually(stores), rarely(asks));
            m_out.appendTo(asks);
            {
                SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
                OperationEffects effects { Writes::Fields, true };
                AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(name.index()), in<Boxed>(stored));
            }
            m_out.jump(continuation);
            m_out.appendTo(stores);
            writeField(cell, object, *field, stored);
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return { };
        }
        writeField(cell, object, *field, stored);
        return { };
    }, [&]() -> Operand {
        bool suppliesAlone = onlyFieldsSupply(m_program, view, name);
        if (!suppliesAlone && !onlyFieldsSupply(m_program, view, name, true)) {
            emitPutById(box(cell), name, in<Boxed>(stored));
            return { };
        }
        if (!suppliesAlone) {
            LBasicBlock asks = newColdBlock();
            LBasicBlock isUnmarked = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(lacksBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(isUnmarked), rarely(asks));
            m_out.appendTo(asks);
            emitPutById(box(cell), name, in<Boxed>(stored));
            m_out.jump(continuation);
            m_out.appendTo(isUnmarked);
            bool mayTransition = storeMayTransition(m_program, view, name);
            OperationEffects effects { Writes::Fields, true, mayTransition, !mayTransition };
            AOT_TYPED_OPERATION_WITH_EFFECTS(PutExtra, effects, cell, word32(name.index()), in<Boxed>(stored));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return { };
        }
        SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
        // A transition writes the shape word and fields. Without one only the order word changes.
        bool mayTransition = storeMayTransition(m_program, view, name);
        OperationEffects effects { Writes::Fields, true, mayTransition, !mayTransition };
        AOT_TYPED_OPERATION_WITH_EFFECTS(PutExtra, effects, cell, word32(name.index()), in<Boxed>(stored));
        return { };
    });
}

// `name in object`
Word32 Lowering::emitInByVal(Boxed object, Boxed key)
{
    return Word32(callICStub(Stub::InByValCached, Int32, { { object.value(), firstStubOperandGPR }, { key.value(), GPRInfo::argumentGPR1 } }));
}

Word32 Lowering::lowerInById(const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    const Field* field = fieldFor(receiver, name);
    if (!field) {
        if (LayoutID view = receiver.onlyView(); view && get(object).rep == Rep::Cell && !isPrivateName(m_program.string(name)) && onlyExtraPropertiesSupply(m_program, view, name)) {
            CellPointer cell = get(object).as<CellPointer>();
            return either<Word32>(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), [&] {
                return boolean(false);
            }, [&] {
                return AOT_TYPED_OPERATION(HasExtra, cell, word32(name.index()));
            });
        }
        auto generally = [&] {
            if (isPrivateName(m_program.string(name)))
                return AOT_TYPED_OPERATION(HasName, in<Boxed>(object), word32(name.index()));
            return emitInByVal(in<Boxed>(object), box(stringConstant(name)));
        };
        auto expected = predictionOf(object);
        const Field* expectedField = expected ? fieldExpectedFor(*expected, name) : nullptr;
        if (!expectedField)
            return generally();
        return ifAsExpected({ { object, &*expected } }, Rep::Int32, [&](LBasicBlock otherwise) {
            withFieldOrElse(object, expected->onlyView(), *expectedField, otherwise);
            return boolean(true);
        }, generally).as<Word32>();
    }
    LayoutID view = receiver.onlyView();
    Operand operand = get(object);
    if (operand.rep != Rep::Cell) {
        // The language has a TypeError of its own for this.
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isNil = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isCell(boxed, receiver).value(), usually(continuation), rarely(isNil));
        m_out.appendTo(isNil);
        OperationEffects onlyThrows { Writes::Nothing, true };
        AOT_TYPED_OPERATION_WITH_EFFECTS(HasName, onlyThrows, remade(boxed), word32(name.index()));
        m_out.unreachable();
        m_out.appendTo(continuation);
        operand = asCell(boxed);
    }
    CellPointer cell = operand.as<CellPointer>();
    return throughView(cell, object, view, *field, Rep::Int32, [&]() -> Operand {
        if (!(m_program.at(field->stored).primitives & Primitive::Absent) || !mayInherit(m_program, view, name))
            return hasOwnField(cell, *field);
        auto ask = [&] {
            return AOT_TYPED_OPERATION(HasName, box(cell), word32(name.index()));
        };
        if (inheritsByItsLayout(m_program, view, name)) {
            return either<Word32>(hasField(cell, *field), [&] {
                return boolean(true);
            }, ask);
        }
        // As in readProperty().
        LBasicBlock isPresent = m_out.newBlock();
        LBasicBlock isAbsent = m_out.newBlock();
        LBasicBlock isUnmarked = m_out.newBlock();
        LBasicBlock asks = newColdBlock();
        OperandJoin join(*this, Rep::Int32);
        m_out.branch(hasOwnField(cell, *field).value(), unsure(isPresent), unsure(isAbsent));
        m_out.appendTo(isPresent);
        join.add(boolean(true));
        m_out.appendTo(isAbsent);
        m_out.branch(lacksBits(orderWordOf(cell), orderWordHasExtraProperties).value(), usually(isUnmarked), rarely(asks));
        m_out.appendTo(isUnmarked);
        join.add(boolean(false));
        m_out.appendTo(asks);
        join.add(ask());
        return join.finish();
    }, [&]() -> Operand {
        bool suppliesAlone = onlyFieldsSupply(m_program, view, name);
        if (!suppliesAlone && !onlyFieldsSupply(m_program, view, name, true))
            return AOT_TYPED_OPERATION(HasName, box(cell), word32(name.index()));
        if (field->isElsewhereInSomeMember) {
            return byOwnFieldOfMember(cell, view, *field, Rep::Int32, [&](LayoutID, const Field& own) -> Operand {
                return hasOwnField(cell, own);
            }, [&]() -> Operand {
                return AOT_TYPED_OPERATION(HasExtra, cell, word32(name.index() | nameMayBeOfOwnField));
            });
        }
        return either<Word32>(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), [&] {
            return boolean(false);
        }, [&] {
            if (!suppliesAlone)
                return AOT_TYPED_OPERATION(HasName, box(cell), word32(name.index()));
            return AOT_TYPED_OPERATION(HasExtra, cell, word32(name.index()));
        });
    }).as<Word32>();
}

// Which of Program::allocations the object is made with: see Allocation::numberOfLiterals. It goes by what the child is, not by how much is
// known of it: what an object has room for decides which casts are carried out, and that is not for a pass to change.
Integer Lowering::allocationFor(const Value& value)
{
    auto& allocation = m_program.allocations[value.allocation()];
    Integer itself = integer(value.allocation());
    ValueID child = value.child(0);
    if (!child || !allocation.numberOfLiterals)
        return itself;
    auto& type = typeOf(child);
    Type anyInteger = Type::integer(Range::full());
    auto byLiteral = [&](Integer discriminant) {
        Integer index = sub(discriminant, integer(allocation.firstLiteral));
        Integer count = integer(allocation.numberOfLiterals);
        if (!type.isInteger() || !type.integers.isWithin({ allocation.firstLiteral, static_cast<int64_t>(allocation.firstLiteral) + allocation.numberOfLiterals - 1 }))
            index = select(below(index, count), index, count);
        return add(index, integer(allocation.allocationOfFirstLiteral));
    };
    if (type.isWithin(anyInteger, m_program))
        return byLiteral(in<Integer>(child));
    LBasicBlock isNoInteger = m_out.newBlock();
    Join join(m_out, Rep::Int64, b3TypeOf(Rep::Int64));
    join.add(byLiteral(fitOrElse(get(child), type, anyInteger, isNoInteger).as<Integer>()));
    m_out.appendTo(isNoInteger);
    join.add(itself);
    return join.finish().as<Integer>();
}

// The entry a constant discriminant picks, or the site itself if it has no table. Nothing: the discriminant is not constant.
std::optional<uint32_t> Lowering::constantAllocation(const Value& value) const
{
    auto& site = m_program.allocations[value.allocation()];
    ValueID child = value.child(0);
    if (!child || !site.numberOfLiterals)
        return value.allocation();
    ValueID origin = child;
    if (ValueID constant = m_program.constantOf(m_function, child))
        origin = constant;
    auto& type = typeOf(child);
    std::optional<int64_t> constant;
    if (m_function.at(origin).opcode == Opcode::Integer)
        constant = m_function.at(origin).integer();
    else if (type.isInteger() && type.integers.least == type.integers.most)
        constant = type.integers.least;
    if (!constant)
        return std::nullopt;
    uint64_t index = static_cast<uint64_t>(*constant) - static_cast<uint64_t>(static_cast<int64_t>(site.firstLiteral));
    return site.allocationOfFirstLiteral + static_cast<uint32_t>(std::min<uint64_t>(index, site.numberOfLiterals));
}

bool Lowering::layoutVariesByDiscriminant(const Allocation& site) const
{
    if (!site.numberOfLiterals)
        return false;
    for (uint32_t i = 0; i <= site.numberOfLiterals; ++i) {
        if (m_program.allocations[site.allocationOfFirstLiteral + i].layout != site.layout)
            return true;
    }
    return false;
}

CellPointer Lowering::lowerMaterializeEnvironment(const Value& value)
{
    CellPointer current = in<CellPointer>(value.child(0));
    auto& birth = m_function.at(value.child(1));
    auto& layout = m_program.at(birth.layout());
    RELEASE_ASSERT(layout.mayBeInFrame && !layout.isInFrame && !layout.hasExtension);
    for (auto& field : layout.fields)
        RELEASE_ASSERT(!(field.flags & fieldIsNeverStored) && !field.isOutOfLine && field.store != Store::Int32OrElsewhere && field.store != Store::Cell32 && field.store != Store::OptionalCell32);
    RELEASE_ASSERT(layout.size > sizeof(JSCell) && !(layout.size % sizeof(uint64_t)));
    if (Options::countAOTTypedAllocations()) [[unlikely]] {
        LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, current.value(), JSCell::cellStateOffset()));
        return either<CellPointer>(Word32(m_out.notEqual(state, m_out.constInt32(UINT8_MAX))), [&] {
            return current;
        }, [&] {
            return AOT_TYPED_OPERATION(MaterializeEnvironment, word32(birth.allocation()), Pointer(current.value()));
        });
    }
    m_context.emitsCalls = true;
    return CellPointer(callAllocationStub(Stub::MaterializeEnvironment, { { current.value(), GPRInfo::returnValueGPR }, { integer(static_cast<int64_t>(birth.allocation() | static_cast<uint64_t>(layout.size) << 32)).value(), stubTemporaryGPRs[0] } }, effectsOfOperation<operationAOTTypedMaterializeEnvironment>(), false));
}

// Room in the frame for what would be a cell. It is a new value at each birth, with the effects of an allocation: the address is the same every time round a loop, and what reads a word that
// never changes has to stay behind the birth it belongs to. The block needs a frame: see FunctionSplitting.
CellPointer Lowering::cellInFrame(size_t size)
{
    m_context.emitsCalls = true;
    LValue slot = m_out.lockedStackSlot(size);
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->append(slot, ValueRep::SomeRegister);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.move(params[1].gpr(), params[0].gpr());
    });
    applyEffects(patchpoint, { Writes::RuntimeState, false });
    return CellPointer(patchpoint);
}

Operand Lowering::initialStateOf(const Field& field, bool isBorn)
{
    uint16_t primitives = m_program.at(field.stored).primitives;
    bool startsAbsent = primitives & Primitive::Absent && !isBorn;
    bool startsUndefined = !startsAbsent && primitives & Primitive::Undefined;
    switch (repOf(field.store)) {
    case Rep::Double:
        return number(0);
    case Rep::Int32:
        return word32(field.store != Store::Int32OrElsewhere ? 0 : startsAbsent ? absentInt32 : startsUndefined ? undefinedInt32 : 0);
    case Rep::Int64:
        return integer(0);
    case Rep::OptionalInt64:
        return OptionalInteger(integer(startsAbsent ? absentInteger : startsUndefined ? undefinedInteger : 0).value());
    case Rep::OptionalDouble:
        return OptionalNumber(m_out.constDouble(std::bit_cast<double>(startsAbsent ? absentDouble : startsUndefined ? undefinedDouble : 0)));
    case Rep::Cell:
        return CellPointer(m_out.int64Zero);
    case Rep::JSValue:
        return startsUndefined ? undefined() : Boxed(m_out.int64Zero);
    case Rep::Void:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand Lowering::lowerNewObject(ValueID id, const Value& value)
{
    // The discriminant picks the entry by its value. Constant or not, the same value gives the same object.
    auto constant = constantAllocation(value);
    bool fromTemplate = !constant && layoutVariesByDiscriminant(m_program.allocations[value.allocation()]);
    auto& allocation = m_program.allocations[constant.value_or(value.allocation())];
    auto& layout = m_program.at(allocation.layout);
    // Of an object: the properties it has from the start. A variable holds nothing until it is initialized.
    BitVector born;
    if (isObjectLayout(layout.kind)) {
        for (uint16_t field : allocation.order.span().first(allocation.born))
            born.set(field);
    }
    // The fields that the values right after this one store to need no initial state, but for one that such a store creates: see writeField().
    BitVector storedNext;
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    // Nor is one spared that is stored to after a store that may call the run time: the collector may run there, and looks at the object.
    bool mayHaveCollected = false;
    for (; next < values.size() && isInitializingStore(m_program, m_function, *m_currentBlock, next); ++next) {
        auto& following = m_function.at(values[next]);
        uint32_t field = *fieldInitializedBy(allocation.layout, following);
        if (!mayHaveCollected && (born.get(field) || !isObjectLayout(layout.kind)))
            storedNext.set(field);
        mayHaveCollected |= !isStoredInPlace(layout.fields[field], following.child(1)) || (isObjectLayout(layout.kind) && !born.get(field));
    }
    bool isInFrame = m_function.environmentsInFrame.get(id.index());
    RELEASE_ASSERT(!isInFrame || layout.mayBeInFrame);
    auto allocateCell = [&] {
        return constant ? allocate(*constant) : allocate(value.allocation(), allocationFor(value), fromTemplate ? FieldInit::FromTemplate : FieldInit::ByCaller);
    };
    auto reuseOrAllocateCell = [&] {
        auto& variable = m_program.variables[layout.reuseVariable.index()];
        TypedPointer place = addressOf(layout.reuseVariable);
        TypedPointer address(m_heaps.AOTTyped_runtimeState, place.base(), place.offset());
        Boxed held = convertTo<Boxed>(load(variable.store, address, IsSafe), m_program.at(variable.type));
        LBasicBlock reused = m_out.newBlock();
        LBasicBlock fresh = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isCell(held).value(), unsure(reused), unsure(fresh));
        m_out.appendTo(reused);
        didEmitWrite(Writes::RuntimeState);
        store(variable.store, convert(undefined(), Type::ofPrimitives(Primitive::Undefined), repOf(variable.store)), address);
        CellPointer taken = asCell(held);
        ValueFromBlock first = m_out.anchor(taken.value());
        m_out.jump(continuation);
        m_out.appendTo(fresh);
        ValueFromBlock second = m_out.anchor(allocateCell().value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return CellPointer(m_out.phi(Int64, first, second));
    };
    bool mayBeUsedAgain = !isInFrame && layout.reuseVariable && !Options::validateAOTTypedFacts();
    CellPointer cell = isInFrame ? cellInFrame(layout.size) : mayBeUsedAgain ? reuseOrAllocateCell() : allocateCell();
    m_isFillingReusedEnvironment = mayBeUsedAgain;
    if (isInFrame && !layout.isInFrame)
        m_out.store32As8(m_out.constInt32(UINT8_MAX), m_out.address(m_heaps.AOTTyped_collectorState, cell.value(), JSCell::cellStateOffset()));
    if (!fromTemplate) {
        m_fieldsAwaitingFirstStore = storedNext;
        m_layoutOfFieldsAwaitingFirstStore = allocation.layout;
    }
    if (layout.hasExtension)
        storeField(cell, m_heaps.AOTTypedObject_extension, word32(0));
    for (uint32_t i = 0; i < layout.fields.size() && !fromTemplate; ++i) {
        auto& field = layout.fields[i];
        if (storedNext.get(i) || field.isOutOfLine || field.flags & fieldIsNeverStored)
            continue;
        if (layout.isCoroutineFrame && !holdsCells(field.store))
            continue;
        store(field.store, initialStateOf(field, born.get(i)), fieldAt(cell, field));
    }
    if (next == m_currentIndexInBlock + 1 && layout.reuseVariable) {
        m_isFillingReusedEnvironment = false;
        barrier(cell);
    }
    if (next == m_currentIndexInBlock + 1 && !isInFrame)
        fenceAfterInitialization();
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

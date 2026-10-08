/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3MemoryValue.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
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

// Whether, in a member of the view that has no field of the name, only an extra property can be of the name, and that is data as far as the
// program says: nothing on its prototype chain is of the name, and no object literal declares an accessor of the name.
bool onlyFieldsSupply(const Program& program, LayoutID view, StringID name)
{
    // The receiver may be any object, which is asked.
    if (program.at(view).admitsForeignObjects)
        return false;
    BitVector recordsThatLackIt;
    bool isLacked = false;
    for (LayoutID member : program.at(view).members) {
        auto& layout = program.at(member);
        if (layout.fieldNamed(name))
            continue;
        isLacked = true;
        if (layout.kind != LayoutKind::Instance)
            recordsThatLackIt.set(member.index());
        if (layout.mayHaveOtherPrototype)
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
    if (!isLacked)
        return true;
    // Nothing can be added to it.
    static constexpr ASCIILiteral namesOnObjectPrototype[] = {
        "constructor"_s, "hasOwnProperty"_s, "isPrototypeOf"_s, "propertyIsEnumerable"_s, "toLocaleString"_s, "toString"_s, "valueOf"_s,
        "__proto__"_s, "__defineGetter__"_s, "__defineSetter__"_s, "__lookupGetter__"_s, "__lookupSetter__"_s,
    };
    for (ASCIILiteral onObjectPrototype : namesOnObjectPrototype) {
        if (equalSpans(program.string(name), onObjectPrototype.span8()))
            return false;
    }
    if (recordsThatLackIt.isEmpty())
        return true;
    // The `prototype` of a function of the program may have anything.
    for (auto& allocation : program.allocations) {
        if (allocation.constructedBy && recordsThatLackIt.get(allocation.layout.index()))
            return false;
    }
    return true;
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
    return field.isLackedBySomeMember ? 0 : safeIfReaches(cell, field.offset + sizeOf(field.store));
}

Operand Lowering::load(Store store, TypedPointer pointer, unsigned flags)
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
        return Word32(marked(m_out.load32(pointer), flags));
    case Store::Uint32:
        return zeroExtended(Word32(marked(m_out.load32(pointer), flags)));
    case Store::Int64:
    case Store::OptionalInt64:
    case Store::Cell:
    case Store::JSValue:
        return Operand(repOf(store), marked(m_out.load64(pointer), flags));
    case Store::Float:
        return Number(m_out.floatToDouble(marked(m_out.loadFloat(pointer), flags)));
    case Store::Double:
    case Store::OptionalDouble:
        return Operand(repOf(store), marked(m_out.loadDouble(pointer), flags));
    }
    RELEASE_ASSERT_NOT_REACHED();
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
void Lowering::barrier(CellPointer owner)
{
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue threshold = m_out.load32(m_out.address(m_heaps.AOTTyped_collectorState, m_vm, VM::offsetOfHeapBarrierThreshold()));
    LValue state = m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, owner.value(), JSCell::cellStateOffset()));
    m_out.branch(m_out.above(state, threshold), usually(continuation), rarely(slowPath));
    m_out.appendTo(slowPath);
    AOT_TYPED_OPERATION(BarrierSlow, owner);
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
    m_out.branch(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_collectorState, m_vm, VM::offsetOfHeapMutatorShouldBeFenced())), rarely(fence), usually(continuation));
    m_out.appendTo(fence);
    m_out.fence(&m_heaps.root, nullptr);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
#endif
}

void Lowering::didLowerInitializingStore()
{
    size_t next = m_currentIndexInBlock + 1;
    if (next >= m_currentBlock->values.size() || !isInitializingStore(m_program, m_function, *m_currentBlock, next))
        fenceAfterInitialization();
}

const Field* Lowering::fieldFor(const Type& receiver, StringID name) const
{
    auto access = fieldOf(m_program, receiver, name);
    return access ? &m_program.at(access->layout).fields[access->field] : nullptr;
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

// One load. The slot holds what the widest of the types says that share it: where that is more than the type of the value, a test follows.
Operand Lowering::readField(CellPointer cell, const Field& field, const Type& type)
{
    Operand raw = load(field.store, fieldAt(cell, field), safeIfAllMembersHave(cell, field));
    // The load has cleared the upper bits of what has no sign, so this costs nothing.
    if (field.store == Store::Uint8 || field.store == Store::Uint16)
        raw = zeroExtended(raw.as<Word32>());
    auto& stored = m_program.at(field.stored);
    if (!(stored.primitives & Primitive::Absent))
        return ensure(raw, stored, type);

    // A field that is absent reads `undefined`.
    Type held = stored;
    held.primitives = (held.primitives & ~Primitive::Absent) | Primitive::Undefined;
    // Where only a number will do, what stands for the one is turned away like what stands for the other.
    if (raw.rep == Rep::JSValue || type.primitives & Primitive::Undefined) {
        Integer bits = bitsOf(raw);
        raw = fromBits(select(isAbsent(raw), integer(bitsOfMarker(Primitive::Undefined, raw.rep)), bits), raw.rep);
    }
    return ensure(raw, held, type);
}

Word32 Lowering::hasField(CellPointer cell, const Field& field)
{
    if (!(m_program.at(field.stored).primitives & Primitive::Absent))
        return boolean(true);
    return logicalNot(isAbsent(load(field.store, fieldAt(cell, field), safeIfAllMembersHave(cell, field))));
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

// One store: what is stored is of the type of the field, which the verifier has seen to.
void Lowering::writeField(CellPointer cell, ValueID object, const Field& field, ValueID stored)
{
    Operand operand = in(stored, repOf(field.store));
    // Nothing has happened to the object yet, nor has the collector run.
    bool isInitializing = isLoweringInitializingStore();
    if (isInitializing && isBornWith(m_program.allocations[m_function.at(object).allocation()], field.name)) {
        didEmitWrite(Writes::RuntimeState);
        store(field.store, operand, fieldAt(cell, field));
        didLowerInitializingStore();
        return;
    }
    SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
    didEmitWrite(Writes::Fields);
    // A store to a field that is absent creates the property, after every other key of the object: the run time writes that down.
    if (m_program.at(field.stored).primitives & Primitive::Absent) {
        LBasicBlock creates = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isAbsent(load(field.store, fieldAt(cell, field), 0)).value(), unsure(creates), unsure(continuation));
        m_out.appendTo(creates);
        if (LayoutID view = typeOf(object).onlyView(); view && mayBeFrozen(view) && !isPrivateName(m_program.string(field.name))) {
            LBasicBlock isNotExtensible = newColdBlock();
            LBasicBlock isExtensible = m_out.newBlock();
            m_out.branch(lacksBits(orderWordOf(cell), orderWordIsNotExtensible).value(), usually(isExtensible), rarely(isNotExtensible));
            m_out.appendTo(isNotExtensible);
            // The object turns the store away.
            OperationEffects refusal { Writes::Nothing, true };
            AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, refusal, box(cell), word32(field.name.index()), in<Boxed>(stored));
            m_out.unreachable();
            m_out.appendTo(isExtensible);
        }
        OperationEffects effects { Writes::Fields, false, false, true };
        AOT_TYPED_OPERATION_WITH_EFFECTS(DidCreateField, effects, cell, word32(field.name.index()));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
    store(field.store, operand, fieldAt(cell, field));
    if (holdsCells(field.store) && typeOf(stored).hasCells())
        barrier(cell);
    if (isInitializing)
        didLowerInitializingStore();
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
    if (!field || (field->isLackedBySomeMember && field->membersWithIt.passesNothing()))
        return nullptr;
    return field;
}

// The receiver has been found to be of the type of the view.
CellPointer Lowering::withFieldOrElse(ValueID object, LayoutID view, const Field& field, LBasicBlock otherwise)
{
    CellPointer cell = get(object).as<CellPointer>();
    if (field.isLackedBySomeMember)
        orElse(passes(shapeWordOf(cell), field.membersWithIt, otherwise), otherwise);
    else if (m_program.at(view).admitsForeignObjects)
        orElse(passes(shapeWordOf(cell), m_program.at(view).test, otherwise), otherwise);
    if (m_program.at(field.stored).primitives & Primitive::Absent)
        orElse(hasField(cell, field), otherwise);
    return cell;
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

// Whether this function has allocated the object, with a member of the view: then it is not for a test to tell it from a foreign one.
// A cast leaves such an object where it is, and what it may turn it into is a member of every view that its layout is a member of.
bool Lowering::isAllocatedAsMemberOf(ValueID object, LayoutID view) const
{
    while (m_function.at(object).opcode == Opcode::Cast)
        object = m_function.at(object).child(0);
    auto& made = m_function.at(object);
    return made.opcode == Opcode::NewObjectWithShape && std::ranges::binary_search(m_program.at(view).members, made.layout());
}

// GetField and PutField name a field of the layout the object was allocated with.

Operand Lowering::lowerGetField(ValueID result, const Value& value)
{
    return readField(in<CellPointer>(value.child(0)), m_program.at(value.layout()).fields[value.field()], typeOf(result));
}

void Lowering::lowerPutField(ValueID, const Value& value)
{
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
    auto& type = typeOf(result);
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
                }, askObject);
            }, askObject);
        }
        auto generally = [&] {
            return ensure(emitGetById(in<Boxed>(object), name), type);
        };
        auto expected = predictionOf(object);
        if (!expected)
            return generally();
        if (const Field* expectedField = fieldExpectedFor(*expected, name)) {
            return ifAsExpected({ { object, &*expected } }, repOf(type), [&](LBasicBlock otherwise) {
                return readField(withFieldOrElse(object, expected->onlyView(), *expectedField, otherwise), *expectedField, type);
            }, generally);
        }
        if (auto builtin = expectedBuiltin(BuiltinKind::Getter, object, m_program.builtinName(name), { }))
            return lowerExpectedBuiltin(result, *builtin, std::nullopt, generally);
        return generally();
    }
    CellPointer cell = nonNil(object, name);
    return throughView(cell, object, view, *field, repOf(type), [&] {
        return readField(cell, *field, type);
    }, [&] {
        if (!onlyFieldsSupply(m_program, view, name))
            return ensure(emitGetById(box(cell), name), type);
        return eitherOperand(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), repOf(type), [&] {
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }, [&] {
            return ensure(AOT_TYPED_OPERATION(GetExtra, cell, word32(name.index())), type);
        });
    });
}

void Lowering::lowerPutById(ValueID, const Value& value)
{
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
        auto expected = predictionOf(object);
        const Field* expectedField = expected ? fieldExpectedFor(*expected, name) : nullptr;
        if (!expectedField || hasMemberThatKeepsField(expected->onlyView(), name)) {
            generally();
            return;
        }
        // What a store that was not compiled with the type of the receiver may store: whatever the slot holds.
        Type held = m_program.at(expectedField->stored).withoutPrimitives(Primitive::Absent);
        ifAsExpected({ { object, &*expected }, { stored, &held } }, Rep::Void, [&](LBasicBlock otherwise) -> Operand {
            CellPointer cell = withFieldOrElse(object, expected->onlyView(), *expectedField, otherwise);
            if (mayBeFrozen(expected->onlyView()))
                orElse(lacksBits(orderWordOf(cell), orderWordIsFrozen), otherwise);
            didEmitWrite(Writes::Fields);
            store(expectedField->store, in(stored, repOf(expectedField->store)), fieldAt(cell, *expectedField));
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
            LBasicBlock isFrozen = newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(lacksBits(orderWordOf(cell), orderWordIsFrozen).value(), usually(continuation), rarely(isFrozen));
            m_out.appendTo(isFrozen);
            // The object turns the store away.
            OperationEffects effects { Writes::Nothing, true };
            AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effects, box(cell), word32(name.index()), in<Boxed>(stored));
            m_out.unreachable();
            m_out.appendTo(continuation);
        }
        writeField(cell, object, *field, stored);
        return { };
    }, [&]() -> Operand {
        if (!onlyFieldsSupply(m_program, view, name)) {
            emitPutById(box(cell), name, in<Boxed>(stored));
            return { };
        }
        SetForScope isLocal(m_isStoringToLocalAllocation, isLocalAllocation(m_function, object));
        OperationEffects effects { Writes::Fields, true, false, true };
        AOT_TYPED_OPERATION_WITH_EFFECTS(PutExtra, effects, cell, word32(name.index()), in<Boxed>(stored));
        return { };
    });
}

// `name in object`
Word32 Lowering::lowerInById(const Value& value)
{
    ValueID object = value.child(0);
    StringID name = value.string();
    auto& receiver = typeOf(object);
    const Field* field = fieldFor(receiver, name);
    if (!field) {
        auto generally = [&] {
            return AOT_TYPED_OPERATION(HasName, in<Boxed>(object), word32(name.index()));
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
        m_out.branch(isCell(boxed).value(), usually(continuation), rarely(isNil));
        m_out.appendTo(isNil);
        OperationEffects onlyThrows { Writes::Nothing, true };
        AOT_TYPED_OPERATION_WITH_EFFECTS(HasName, onlyThrows, boxed, word32(name.index()));
        m_out.unreachable();
        m_out.appendTo(continuation);
        operand = asCell(boxed);
    }
    CellPointer cell = operand.as<CellPointer>();
    return throughView(cell, object, view, *field, Rep::Int32, [&]() -> Operand {
        return hasField(cell, *field);
    }, [&]() -> Operand {
        if (!onlyFieldsSupply(m_program, view, name))
            return AOT_TYPED_OPERATION(HasName, box(cell), word32(name.index()));
        return either<Word32>(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), [&] {
            return boolean(false);
        }, [&] {
            return AOT_TYPED_OPERATION(HasExtra, cell, word32(name.index()));
        });
    }).as<Word32>();
}

Operand Lowering::lowerNewObject(ValueID, const Value& value)
{
    auto& allocation = m_program.allocations[value.allocation()];
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
    for (; next < values.size() && isInitializingStore(m_program, m_function, *m_currentBlock, next); ++next) {
        auto& following = m_function.at(values[next]);
        uint32_t field = *layout.fieldNamed(following.opcode == Opcode::PutById ? following.string() : m_program.at(following.layout()).fields[following.field()].name);
        if (born.get(field) || !isObjectLayout(layout.kind))
            storedNext.set(field);
    }
    CellPointer cell = allocate(value.allocation());
    for (uint32_t i = 0; i < layout.fields.size(); ++i) {
        if (storedNext.get(i))
            continue;
        auto& field = layout.fields[i];
        uint16_t primitives = m_program.at(field.stored).primitives;
        bool startsAbsent = primitives & Primitive::Absent && !born.get(i);
        bool startsUndefined = !startsAbsent && primitives & Primitive::Undefined;
        Operand initial;
        switch (repOf(field.store)) {
        case Rep::Double:
            initial = number(0);
            break;
        case Rep::Int32:
            initial = word32(0);
            break;
        case Rep::Int64:
            initial = integer(0);
            break;
        case Rep::OptionalInt64:
            initial = OptionalInteger(integer(startsAbsent ? absentInteger : startsUndefined ? undefinedInteger : 0).value());
            break;
        case Rep::OptionalDouble:
            initial = OptionalNumber(m_out.constDouble(std::bit_cast<double>(startsAbsent ? absentDouble : startsUndefined ? undefinedDouble : 0)));
            break;
        case Rep::Cell:
            initial = CellPointer(m_out.int64Zero);
            break;
        case Rep::JSValue:
            initial = startsUndefined ? undefined() : Boxed(m_out.int64Zero);
            break;
        case Rep::Void:
            RELEASE_ASSERT_NOT_REACHED();
        }
        store(field.store, initial, fieldAt(cell, field));
    }
    if (next == m_currentIndexInBlock + 1)
        fenceAfterInitialization();
    return cell;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

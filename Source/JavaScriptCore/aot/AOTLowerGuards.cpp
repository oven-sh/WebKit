/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "AOTProgram.h"
#include "B3ValueInlines.h"
#include "BytecodeStructs.h"
#include "FTLSwitchCase.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "NativeExecutable.h"

namespace JSC { namespace AOT {

using namespace B3;

// The fast copy of a loop (see BasicBlock::isGeneric). A guard is lowered to the short way of doing the instruction that comes
// after it, all of it, with a way out to the generic copy from wherever it turns out not to apply. Up to the last way out it does
// nothing that can be told from the outside: the generic copy starts the instruction over.

static LValue lowHalf(FTL::Output& out, LValue word) { return out.castToInt32(word); }
static LValue hasFlag(FTL::Output& out, LValue word, uint32_t flag) { return out.testNonZero64(word, out.constInt64(static_cast<int64_t>(static_cast<uint64_t>(flag) << 32))); }

void Lowering::lowerGuard(BasicBlock* block, Node* guard)
{
    RELEASE_ASSERT(block->successors.size() == 2);
    if (!m_exit)
        m_exit = newColdBlock();
    emitGuard(guard);
    emitUpsilons(block, block->successors[0]);
    m_out.jump(block->successors[0]->lowered);

    // What the generic copy wants boxed is boxed on the way there.
    m_out.appendTo(m_exit);
    emitUpsilons(block, block->successors[1]);
    m_out.jump(block->successors[1]->lowered);
    m_exit = nullptr;
}

LBasicBlock Lowering::newColdBlock()
{
    m_out.setFrequency(coldFrequency);
    LBasicBlock result = m_out.newBlock();
    m_out.setFrequency(m_block->isGeneric ? coldFrequency : 1);
    return result;
}

// Another thread sets them, which is nothing that B3 has a way to be told: it takes two loads with no store between them for one,
// and a loop that stores nothing would look once. So it is not shown a load.
LValue Lowering::trapBits()
{
    PatchpointValue* patchpoint = m_out.patchpoint(Int32);
    patchpoint->append(m_vm, ValueRep::SomeRegister);
    patchpoint->effects = Effects::none();
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->effects.writesLocalState = true;
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.load32(CCallHelpers::Address(params[1].gpr(), VM::offsetOfTrapsBits()), params[0].gpr());
    });
    return patchpoint;
}

// from: what the value is known to be. Every type can be told from every other by looking.
void Lowering::exitUnlessOfType(LValue value, Type from, Type wanted)
{
    LBasicBlock pass = m_out.newBlock();
    Type remaining = from;
    LValue type = nullptr;
    auto cellTypeOfValue = [&] {
        if (!type)
            type = cellType(value);
        return type;
    };
    auto isType = [&](JSType jsType) { return m_out.equal(cellTypeOfValue(), m_out.constInt32(jsType)); };
    // In an order in which each test can take it that the ones before it failed: what is not a cell comes first.
    auto consider = [&](Type atoms, auto&& test) {
        if (!mayBe(remaining, atoms))
            return;
        // With one verdict for all that is left there is nothing to find out.
        if (isSubtype(remaining, wanted) || !mayBe(remaining, wanted))
            return;
        RELEASE_ASSERT(isSubtype(atoms & remaining, wanted) || !mayBe(atoms & remaining, wanted));
        LBasicBlock next = m_out.newBlock();
        m_out.branch(test(), unsure(mayBe(atoms & remaining, wanted) ? pass : m_exit), unsure(next));
        m_out.appendTo(next);
        remaining &= ~atoms;
    };
    consider(TInt32, [&] { return isInt32(value); });
    consider(TDouble, [&] { return isNumber(value); });
    consider(TBoolean, [&] { return isBoolean(value); });
    consider(TUndefined, [&] { return m_out.equal(value, m_out.constInt64(JSValue::ValueUndefined)); });
    consider(TNull, [&] { return m_out.equal(value, m_out.constInt64(JSValue::ValueNull)); });
    consider(TEmpty, [&] { return m_out.isZero64(value); });
    consider(TString, [&] { return isType(StringType); });
    consider(TSymbol, [&] { return isType(SymbolType); });
    consider(TBigInt, [&] { return isType(HeapBigIntType); });
    consider(TCellOther, [&] { return m_out.below(cellTypeOfValue(), m_out.constInt32(ObjectType)); });
    consider(TFunction, [&] { return m_out.bitOr(isType(JSFunctionType), isType(InternalFunctionType)); });
    consider(TArray, [&] { return m_out.bitOr(isType(ArrayType), isType(DerivedArrayType)); });
    for (unsigned i = 0; i < NumberOfTypedArrayTypesExcludingDataView; ++i) {
        JSType typedArrayType = static_cast<JSType>(FirstTypedArrayType + i);
        consider(typeOfTypedArray(typedArrayType), [&] { return isType(typedArrayType); });
    }
    m_out.jump(mayBe(remaining, wanted) ? pass : m_exit);
    m_out.appendTo(pass);
}

void Lowering::guardReentry(BasicBlock* block)
{
    for (Node* node : block->nodes) {
        if (node->kind != NodeKind::Narrow || !node->useCount)
            continue;
        Node* value = node->uses[0].node;
        if (!isSubtype(value->type, node->type))
            exitUnlessOfType(lowJSValue(value), value->type, node->type);

        if (!node->isInteger() || (value->isInteger() && value->range.min >= node->range.min && value->range.max <= node->range.max)) {
            node->lowered = convert(lowRaw(value), value->rep(), node->type, node->rep());
            continue;
        }
        // It has to be an integer, and no bigger than the loop's own get.
        LValue integer;
        if (value->isInteger())
            integer = lowInt64(value);
        else if (value->rep() == Rep::Double) {
            LValue number = lowRaw(value);
            integer = m_out.doubleToInt64(number);
            exitUnless(m_out.doubleEqual(m_out.intToDouble(integer), number));
            exitUnless(m_out.bitOr(m_out.notZero64(integer), m_out.isZero64(m_out.bitCast(number, Int64))));
        } else {
            LValue jsValue = lowRaw(value);
            exitUnless(isInt32(jsValue));
            integer = m_out.signExt32To64(unboxInt32(jsValue));
        }
        // Numbers, whatever they look like (see compile()).
        m_graph.wideIntegerConstants.add(node->range.min);
        m_graph.wideIntegerConstants.add(node->range.max);
        exitUnless(m_out.greaterThanOrEqual(integer, m_out.constInt64(node->range.min)));
        exitUnless(m_out.lessThanOrEqual(integer, m_out.constInt64(node->range.max)));
        node->lowered = convert(integer, Rep::Int64, node->type, node->rep());
    }
}

unsigned Lowering::slotOfPropertyGuard(Node* guard)
{
    if (guard->opcode == op_get_by_id)
        return sharedSite(guard, guard->as<OpGetById>().m_property);
    auto bytecode = guard->as<OpPutById>();
    return sharedSite(guard, bytecode.m_property, (bytecode.m_flags.isDirect() ? 1 : 0) | (bytecode.m_flags.ecmaMode().isStrict() ? 2 : 0));
}

// There is always something there to load.
LValue Lowering::loadSlotWord(unsigned slot, unsigned word)
{
    LValue result = m_out.load64(slotWord(slot, word));
    static_cast<MemoryValue*>(result)->setControlDependent(false);
    return result;
}

void Lowering::checkStructure(Node* baseNode, LValue base, LValue word)
{
    if (!isSubtype(baseNode->type, TCell))
        exitUnless(isCell(base));
    exitUnless(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), m_out.castToInt32(word)));
}

void Lowering::emitGuard(Node* guard)
{
    switch (guard->guardKind) {
    case GuardKind::Whole:
        break;
    case GuardKind::Nothing:
        return;
    case GuardKind::Reentry:
        guardReentry(guard->block);
        return;
    case GuardKind::Structure:
        checkStructure(guard->uses[0].node, lowJSValue(guard->uses[0].node), loadSlotWord(slotOfPropertyGuard(guard->site), 0));
        return;
    case GuardKind::SlotsAgree:
        exitUnless(m_out.equal(m_out.castToInt32(loadSlotWord(slotOfPropertyGuard(guard->site), 0)), m_out.castToInt32(loadSlotWord(slotOfPropertyGuard(guard->otherSite), 0))));
        return;
    case GuardKind::SlotIsPlain:
        exitUnless(m_out.logicalNot(hasFlag(m_out, loadSlotWord(slotOfPropertyGuard(guard->site), 0), Slot::isIntricate)));
        return;
    case GuardKind::BeginSlotChecks: {
        m_slotOfSlotChecks = allocateSlot();
        m_afterSlotChecks = m_out.newBlock();
        LBasicBlock check = m_out.newBlock();
        m_slotEpoch = m_out.load64(m_data, m_heaps.AOTData_slotEpoch);
        m_out.branch(m_out.equal(m_slotEpoch, m_out.load64(slotWord(m_slotOfSlotChecks, 1))), usually(m_afterSlotChecks), rarely(check));
        m_out.appendTo(check);
        return;
    }
    case GuardKind::EndSlotChecks:
        m_out.store64(m_slotEpoch, slotWord(m_slotOfSlotChecks, 1));
        m_out.jump(m_afterSlotChecks);
        m_out.appendTo(m_afterSlotChecks);
        return;
    case GuardKind::Callee:
        checkCallee(guard);
        return;
    case GuardKind::KnownCallee: {
        const KnownFunction* known = m_graph.knownCallee(guard);
        unsigned slot = siteOfKnownCall(guard, m_graph.indexOfKnownCallee(known->keyFor(false)), false);
        exitUnless(m_out.equal(loadSlotWord(slot, 1), lowJSValue(guard->uses[0].node)));
        return;
    }
    case GuardKind::TypedArrayStorage: {
        LValue base = lowJSValue(guard->uses[0].node);
        exitUnless(m_out.testIsZero32(m_out.load8ZeroExt32(base, m_heaps.JSArrayBufferView_mode), m_out.constInt32(isResizableOrGrowableSharedMode)));
        guard->loweredLength = m_out.loadPtr(base, m_heaps.JSArrayBufferView_length);
        guard->lowered = m_out.loadPtr(base, m_heaps.JSArrayBufferView_vector);
        return;
    }
    }

    guard->isHandled = true;
    switch (guard->opcode) {
    case op_resolve_scope:
        guard->isHandled = guardResolveScope(guard);
        break;
    case op_get_from_scope:
        guard->isHandled = guardGetFromScope(guard);
        break;
    case op_call:
    case op_call_ignore_result:
        guard->isHandled = guardCall(guard);
        break;
    case op_get_by_id:
        guardGetById(guard);
        break;
    case op_put_by_id:
        guardPutById(guard);
        break;
    case op_get_by_val:
        guardGetByVal(guard);
        break;
    case op_put_by_val:
        guardPutByVal(guard);
        break;
    case op_get_length:
        guardGetLength(guard);
        break;
    case op_check_type:
        guardCheckType(guard);
        break;
    case op_check_traps:
        exitUnless(m_out.testIsZero32(trapBits(), m_out.constInt32(VMTraps::AsyncEvents)));
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void Lowering::lowerGuarded(Node* node)
{
    Node* guard = node->guard;
    switch (node->opcode) {
    case op_get_by_val:
        if (auto type = Graph::typedArrayAccessed(node)) {
            if (*type == Float32ArrayType || *type == Float64ArrayType)
                setDouble(node, guard->lowered);
            else if (*type == Uint32ArrayType)
                setInt64(node, guard->lowered);
            else
                setInt32(node, guard->lowered);
            return;
        }
        if (isSubtype(node->type, TNumber))
            setResult(node, guard->lowered, node->rep() == Rep::Int32 ? Rep::Int32 : Rep::Double);
        else
            setJSValue(node, guard->lowered);
        return;
    case op_get_by_id:
    case op_get_length:
    case op_resolve_scope:
    case op_get_from_scope:
        setJSValue(node, guard->lowered);
        return;
    case op_call:
        switch (m_graph.intrinsicOfCall(node)) {
        case CallIntrinsic::MathIMul:
        case CallIntrinsic::StringCharCodeAt:
        case CallIntrinsic::ArrayPush:
            setInt32(node, guard->lowered);
            break;
        default:
            setDouble(node, guard->lowered);
            break;
        }
        return;
    case op_call_ignore_result:
        return;
    case op_check_type: {
        Node* value = node->use(node->as<OpCheckType>().m_value);
        if (guard->lowered)
            setDouble(node, guard->lowered);
        else if (value->rep() != Rep::JSValue)
            setResult(node, lowRaw(value), value->rep());
        else
            setJSValue(node, lowRaw(value));
        return;
    }
    case op_put_by_id:
    case op_put_by_val:
    case op_check_traps:
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void Lowering::exitUnless(LValue condition)
{
    LBasicBlock next = m_out.newBlock();
    m_out.branch(condition, usually(next), rarely(m_exit));
    m_out.appendTo(next);
}


// Where a property that is in the base itself is.
static LValue plainLocation(FTL::Output& out, LValue base, LValue word)
{
    LValue location = out.bitAnd(out.lShr(word, out.constInt32(32)), out.constInt64(Slot::offsetMask));
    return out.add(base, out.shl(location, out.constInt32(3)));
}

void Lowering::guardGetById(Node* guard)
{
    auto bytecode = guard->as<OpGetById>();
    Node* baseNode = guard->use(bytecode.m_base);
    if (!Site::fits(bytecode.m_property, 0)) {
        exitUnless(m_out.booleanFalse);
        guard->lowered = m_out.int64Zero;
        return;
    }
    // No two names are ever the same place.
    const AbstractHeap& heap = m_heaps.properties[bytecode.m_property];
    LValue base = lowJSValue(baseNode);
    unsigned slot = slotOfPropertyGuard(guard);
    LValue word = loadSlotWord(slot, 0);
    if (!guard->structureIsChecked)
        checkStructure(baseNode, base, word);

    if (guard->slotIsPlain) {
        guard->lowered = m_out.load64(TypedPointer(heap, plainLocation(m_out, base, word)));
        return;
    }

    LBasicBlock plain = m_out.newBlock();
    LBasicBlock intricate = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasFlag(m_out, word, Slot::isIntricate), rarely(intricate), usually(plain));

    m_out.appendTo(plain, intricate);
    ValueFromBlock plainResult = m_out.anchor(m_out.load64(TypedPointer(heap, plainLocation(m_out, base, word))));
    m_out.jump(continuation);

    m_out.appendTo(intricate, continuation);
    exitUnless(m_out.logicalNot(hasFlag(m_out, word, Slot::isGetter)));
    LValue holder = loadSlotWord(slot, 1);
    ValueFromBlock intricateResult = m_out.anchor(m_out.load64(cachedPropertyAddress(m_out.select(m_out.notNull(holder), holder, base), word, &heap)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    guard->lowered = m_out.phi(Int64, plainResult, intricateResult);
}

void Lowering::guardPutById(Node* guard)
{
    auto bytecode = guard->as<OpPutById>();
    Node* baseNode = guard->use(bytecode.m_base);
    Node* valueNode = guard->use(bytecode.m_value);
    uint32_t flags = (bytecode.m_flags.isDirect() ? 1 : 0) | (bytecode.m_flags.ecmaMode().isStrict() ? 2 : 0);
    if (!Site::fits(bytecode.m_property, flags)) {
        exitUnless(m_out.booleanFalse);
        return;
    }
    const AbstractHeap& heap = m_heaps.properties[bytecode.m_property];
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);
    unsigned slot = slotOfPropertyGuard(guard);
    LValue word = loadSlotWord(slot, 0);
    if (!guard->structureIsChecked)
        checkStructure(baseNode, base, word);

    if (guard->slotIsPlain) {
        m_out.store64(value, TypedPointer(heap, plainLocation(m_out, base, word)));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        return;
    }

    LBasicBlock plain = m_out.newBlock();
    LBasicBlock intricate = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasFlag(m_out, word, Slot::isIntricate), rarely(intricate), usually(plain));

    m_out.appendTo(plain, intricate);
    ValueFromBlock plainAddress = m_out.anchor(plainLocation(m_out, base, word));
    m_out.jump(continuation);

    // Out of line. A new property is not for here: what it does to the structure, everything else here would have to look out for.
    m_out.appendTo(intricate, continuation);
    exitUnless(m_out.isZero32(lowHalf(m_out, loadSlotWord(slot, 1))));
    ValueFromBlock intricateAddress = m_out.anchor(cachedPropertyAddress(base, word).value());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    m_out.store64(value, TypedPointer(heap, m_out.phi(pointerType(), plainAddress, intricateAddress)));
    if (mayBe(valueNode->type, TCell))
        storeBarrier(base);
}

namespace {

struct TypedArrayKind {
    JSType type;
    unsigned logSize;
};

constexpr TypedArrayKind typedArrayKinds[] = {
    { Int8ArrayType, 0 }, { Uint8ArrayType, 0 }, { Int16ArrayType, 1 }, { Uint16ArrayType, 1 }, { Int32ArrayType, 2 }, { Uint32ArrayType, 2 },
    { Float32ArrayType, 2 }, { Float64ArrayType, 3 },
};

} // anonymous namespace

TypedPointer Lowering::elementOfTypedArray(Node* guard, Node* baseNode, Node* propertyNode, JSType type)
{
    LValue index;
    if (propertyNode->isInteger())
        index = lowInt64(propertyNode); // One that is negative is, unsigned, beyond any length.
    else if (!mayBe(propertyNode->type, TNumber)) {
        exitUnless(m_out.booleanFalse);
        index = m_out.int64Zero;
    } else {
        LBasicBlock haveIndex = m_out.newBlock();
        LValue narrow = lowIndex(propertyNode, haveIndex, m_exit);
        m_out.appendTo(haveIndex);
        index = m_out.signExt32To64(narrow);
    }

    LValue vector;
    LValue length;
    if (guard->storage) {
        vector = guard->storage->lowered;
        length = guard->storage->loweredLength;
    } else {
        LValue base = lowJSValue(baseNode);
        exitUnless(m_out.testIsZero32(m_out.load8ZeroExt32(base, m_heaps.JSArrayBufferView_mode), m_out.constInt32(isResizableOrGrowableSharedMode)));
        length = m_out.loadPtr(base, m_heaps.JSArrayBufferView_length);
        vector = m_out.loadPtr(base, m_heaps.JSArrayBufferView_vector);
    }
    // One that has been detached has no length.
    exitUnless(m_out.below(index, length));
    unsigned logSize = 0;
    for (auto& kind : typedArrayKinds) {
        if (kind.type == type)
            logSize = kind.logSize;
    }
    return TypedPointer(m_heaps.TypedArrayProperties, m_out.add(vector, m_out.shl(index, m_out.constInt32(logSize))));
}

void Lowering::guardGetByVal(Node* guard)
{
    auto bytecode = guard->as<OpGetByVal>();
    Node* baseNode = guard->use(bytecode.m_base);
    Node* propertyNode = guard->use(bytecode.m_property);
    if (auto type = Graph::typedArrayAccessed(guard)) {
        TypedPointer pointer = elementOfTypedArray(guard, baseNode, propertyNode, *type);
        switch (*type) {
        case Int8ArrayType:
            guard->lowered = m_out.load8SignExt32(pointer);
            break;
        case Uint8ArrayType:
            guard->lowered = m_out.load8ZeroExt32(pointer);
            break;
        case Int16ArrayType:
            guard->lowered = m_out.load16SignExt32(pointer);
            break;
        case Uint16ArrayType:
            guard->lowered = m_out.load16ZeroExt32(pointer);
            break;
        case Int32ArrayType:
            guard->lowered = m_out.load32(pointer);
            break;
        case Uint32ArrayType:
            guard->lowered = m_out.zeroExt(m_out.load32(pointer), Int64);
            break;
        case Float32ArrayType:
            guard->lowered = m_out.purifyNaN(m_out.floatToDouble(m_out.loadFloat(pointer)));
            break;
        case Float64ArrayType:
            guard->lowered = m_out.purifyNaN(m_out.loadDouble(pointer));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        return;
    }
    if (!mayBe(baseNode->type, TAnyObject) || !mayBe(propertyNode->type, TNumber)) {
        exitUnless(m_out.booleanFalse);
        guard->lowered = guard->guarded && isSubtype(guard->guarded->type, TNumber) ? (guard->guarded->rep() == Rep::Int32 ? m_out.int32Zero : m_out.doubleZero) : m_out.int64Zero;
        return;
    }

    LValue base = lowJSValue(baseNode);
    LBasicBlock haveIndex = m_out.newBlock();
    LValue index = lowIndex(propertyNode, haveIndex, m_exit);
    m_out.appendTo(haveIndex);
    if (!isSubtype(baseNode->type, TCell))
        exitUnless(isCell(base));

    if (guard->guarded && isSubtype(guard->guarded->type, TNumber)) {
        // An array that is taken to hold numbers (see the type of op_get_by_val).
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        if (guard->guarded->rep() == Rep::Int32) {
            exitUnless(m_out.equal(shape, m_out.constInt32(Int32Shape)));
            LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
            exitUnless(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)));
            LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedInt32Properties, butterfly, m_out.zeroExtPtr(index)));
            exitUnless(m_out.notZero64(element));
            guard->lowered = unboxInt32(element);
            return;
        }
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        exitUnless(m_out.belowOrEqual(m_out.sub(shape, m_out.constInt32(Int32Shape)), m_out.constInt32(ContiguousShape - Int32Shape)));
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        exitUnless(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)));
        LBasicBlock holdsDoubles = m_out.newBlock();
        LBasicBlock holdsValues = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.equal(shape, m_out.constInt32(DoubleShape)), unsure(holdsDoubles), unsure(holdsValues));

        m_out.appendTo(holdsDoubles, holdsValues);
        LValue number = m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly, m_out.zeroExtPtr(index)));
        exitUnless(m_out.doubleEqual(number, number));
        ValueFromBlock doubleResult = m_out.anchor(number);
        m_out.jump(continuation);

        m_out.appendTo(holdsValues, continuation);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        exitUnless(isNumber(element));
        ValueFromBlock valueResult = m_out.anchor(numberToDouble(element));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        guard->lowered = m_out.phi(Double, doubleResult, valueResult);
        return;
    }

    LBasicBlock hasButterfly = m_out.newBlock();
    LBasicBlock holdsValues = m_out.newBlock();
    LBasicBlock holdsDoubles = m_out.newBlock();
    LBasicBlock noButterfly = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 12> results;

    LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
    m_out.branch(m_out.notZero32(shape), unsure(hasButterfly), unsure(noButterfly));

    m_out.appendTo(hasButterfly, holdsValues);
    LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    exitUnless(m_out.belowOrEqual(m_out.sub(shape, m_out.constInt32(Int32Shape)), m_out.constInt32(ContiguousShape - Int32Shape)));
    exitUnless(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)));
    m_out.branch(m_out.equal(shape, m_out.constInt32(DoubleShape)), unsure(holdsDoubles), unsure(holdsValues));

    m_out.appendTo(holdsValues, holdsDoubles);
    LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
    exitUnless(m_out.notZero64(element));
    results.append(m_out.anchor(element));
    m_out.jump(continuation);

    // A hole is not a number.
    m_out.appendTo(holdsDoubles, noButterfly);
    LValue number = m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly, m_out.zeroExtPtr(index)));
    exitUnless(m_out.doubleEqual(number, number));
    results.append(m_out.anchor(boxDouble(number)));
    m_out.jump(continuation);

    m_out.appendTo(noButterfly, continuation);
    if (mayBe(baseNode->type, TTypedArray)) {
        LValue type = cellType(base);
        exitUnless(m_out.below(m_out.sub(type, m_out.constInt32(FirstTypedArrayType)), m_out.constInt32(NumberOfTypedArrayTypesExcludingDataView)));
        exitUnless(m_out.testIsZero32(m_out.load8ZeroExt32(base, m_heaps.JSArrayBufferView_mode), m_out.constInt32(isResizableOrGrowableSharedMode)));
        // One that has been detached has no length.
        LValue wideIndex = m_out.zeroExtPtr(index);
        exitUnless(m_out.below(wideIndex, m_out.loadPtr(base, m_heaps.JSArrayBufferView_length)));
        LValue vector = m_out.loadPtr(base, m_heaps.JSArrayBufferView_vector);
        Vector<FTL::SwitchCase> cases;
        Vector<LBasicBlock, 8> blocks;
        for (auto& kind : typedArrayKinds) {
            blocks.append(m_out.newBlock());
            cases.append(FTL::SwitchCase(m_out.constInt32(kind.type), blocks.last(), FTL::Weight()));
        }
        m_out.switchInstruction(type, cases, m_exit, FTL::Weight());
        for (unsigned i = 0; i < std::size(typedArrayKinds); ++i) {
            auto& kind = typedArrayKinds[i];
            m_out.appendTo(blocks[i]);
            TypedPointer pointer(m_heaps.TypedArrayProperties, m_out.add(vector, m_out.shl(wideIndex, m_out.constInt32(kind.logSize))));
            LValue result;
            switch (kind.type) {
            case Int8ArrayType:
                result = boxInt32(m_out.load8SignExt32(pointer));
                break;
            case Uint8ArrayType:
                result = boxInt32(m_out.load8ZeroExt32(pointer));
                break;
            case Int16ArrayType:
                result = boxInt32(m_out.load16SignExt32(pointer));
                break;
            case Uint16ArrayType:
                result = boxInt32(m_out.load16ZeroExt32(pointer));
                break;
            case Int32ArrayType:
                result = boxInt32(m_out.load32(pointer));
                break;
            case Uint32ArrayType: {
                LValue bits = m_out.load32(pointer);
                result = m_out.select(m_out.greaterThanOrEqual(bits, m_out.int32Zero), boxInt32(bits), boxDouble(m_out.unsignedToDouble(bits)));
                break;
            }
            case Float32ArrayType:
                result = boxDouble(m_out.purifyNaN(m_out.floatToDouble(m_out.loadFloat(pointer))));
                break;
            case Float64ArrayType:
                result = boxDouble(m_out.purifyNaN(m_out.loadDouble(pointer)));
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            results.append(m_out.anchor(result));
            m_out.jump(continuation);
        }
    } else
        m_out.jump(m_exit);

    m_out.appendTo(continuation);
    guard->lowered = m_out.phi(Int64, results);
}

void Lowering::guardPutByVal(Node* guard)
{
    auto bytecode = guard->as<OpPutByVal>();
    Node* baseNode = guard->use(bytecode.m_base);
    Node* propertyNode = guard->use(bytecode.m_property);
    Node* valueNode = guard->use(bytecode.m_value);
    if (!mayBe(baseNode->type, TAnyObject) || !mayBe(propertyNode->type, TNumber)) {
        exitUnless(m_out.booleanFalse);
        return;
    }

    bool valueIsNumber = isSubtype(valueNode->type, TNumber);
    bool valueMayBeNumber = mayBe(valueNode->type, TNumber);
    // Only asked for where it is one, or where it has been made sure of.
    auto valueAsDouble = [&]() -> LValue {
        if (valueIsNumber)
            return lowDouble(valueNode);
        LValue value = lowJSValue(valueNode);
        exitUnless(isNumber(value));
        return numberToDouble(value);
    };
    auto valueAsInt32 = [&]() -> LValue {
        if (valueIsNumber)
            return toInt32ForBitOp(valueNode);
        LValue value = lowJSValue(valueNode);
        exitUnless(isInt32(value));
        return unboxInt32(value);
    };
    auto storeToTypedArray = [&](JSType type, TypedPointer pointer) {
        switch (type) {
        case Int8ArrayType:
        case Uint8ArrayType:
            m_out.store32As8(valueAsInt32(), pointer);
            break;
        case Int16ArrayType:
        case Uint16ArrayType:
            m_out.store32As16(valueAsInt32(), pointer);
            break;
        case Int32ArrayType:
        case Uint32ArrayType:
            m_out.store32(valueAsInt32(), pointer);
            break;
        case Float32ArrayType:
            m_out.storeFloat(m_out.doubleToFloat(valueAsDouble()), pointer);
            break;
        case Float64ArrayType:
            m_out.storeDouble(valueAsDouble(), pointer);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    if (auto type = Graph::typedArrayAccessed(guard)) {
        if (!valueMayBeNumber) {
            exitUnless(m_out.booleanFalse);
            return;
        }
        storeToTypedArray(*type, elementOfTypedArray(guard, baseNode, propertyNode, *type));
        return;
    }

    LValue base = lowJSValue(baseNode);
    LBasicBlock haveIndex = m_out.newBlock();
    LValue index = lowIndex(propertyNode, haveIndex, m_exit);
    m_out.appendTo(haveIndex);
    if (!isSubtype(baseNode->type, TCell))
        exitUnless(isCell(base));
    LValue wideIndex = m_out.zeroExtPtr(index);

    LBasicBlock hasButterfly = m_out.newBlock();
    LBasicBlock holdsValues = m_out.newBlock();
    LBasicBlock holdsDoubles = m_out.newBlock();
    LBasicBlock noButterfly = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    // Copy on write storage has the shape bits of what it holds and one more bit.
    LValue indexingMode = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask | CopyOnWrite));
    m_out.branch(m_out.notZero32(indexingMode), unsure(hasButterfly), unsure(noButterfly));

    // An element for which there is room. What is between the old length and it are holes already.
    m_out.appendTo(hasButterfly, holdsValues);
    LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
    exitUnless(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_vectorLength)));
    auto lengthen = [&] {
        LBasicBlock beyondLength = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), rarely(beyondLength));
        m_out.appendTo(beyondLength, inBounds);
        m_out.store32(m_out.add(index, m_out.int32One), butterfly, m_heaps.Butterfly_publicLength);
        m_out.jump(inBounds);
        m_out.appendTo(inBounds);
    };
    m_out.branch(m_out.equal(indexingMode, m_out.constInt32(ContiguousShape)), unsure(holdsValues), unsure(holdsDoubles));

    m_out.appendTo(holdsValues, holdsDoubles);
    {
        LValue value = lowJSValue(valueNode);
        lengthen();
        m_out.store64(value, m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, wideIndex));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        m_out.jump(continuation);
    }

    m_out.appendTo(holdsDoubles, noButterfly);
    if (valueMayBeNumber) {
        LBasicBlock holdsInt32s = m_out.newBlock();
        LBasicBlock notInt32s = m_out.newBlock();
        m_out.branch(m_out.equal(indexingMode, m_out.constInt32(Int32Shape)), unsure(holdsInt32s), unsure(notInt32s));
        m_out.appendTo(holdsInt32s, notInt32s);
        {
            LValue boxed;
            if (valueNode->rep() == Rep::Int32)
                boxed = lowJSValue(valueNode);
            else if (valueNode->rep() == Rep::Int64) {
                LValue wide = lowRaw(valueNode);
                LValue narrow = m_out.castToInt32(wide);
                exitUnless(m_out.equal(m_out.signExt32To64(narrow), wide));
                boxed = boxInt32(narrow);
            } else if (valueNode->rep() == Rep::Double) {
                LValue number = lowRaw(valueNode);
                LValue integer = m_out.doubleToInt32(number);
                exitUnless(m_out.doubleEqual(m_out.intToDouble(integer), number));
                exitUnless(m_out.bitOr(m_out.notZero32(integer), m_out.isZero64(m_out.bitCast(number, Int64))));
                boxed = boxInt32(integer);
            } else {
                boxed = lowJSValue(valueNode);
                exitUnless(isInt32(boxed));
            }
            lengthen();
            m_out.store64(boxed, m_out.baseIndex(m_heaps.indexedInt32Properties, butterfly, wideIndex));
            m_out.jump(continuation);
        }
        m_out.appendTo(notInt32s);
        exitUnless(m_out.equal(indexingMode, m_out.constInt32(DoubleShape)));
        LValue number = valueAsDouble();
        exitUnless(m_out.doubleEqual(number, number));
        lengthen();
        m_out.storeDouble(number, m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly, wideIndex));
        m_out.jump(continuation);
    } else
        m_out.jump(m_exit);

    m_out.appendTo(noButterfly, continuation);
    if (mayBe(baseNode->type, TTypedArray) && valueMayBeNumber) {
        LValue type = cellType(base);
        exitUnless(m_out.below(m_out.sub(type, m_out.constInt32(FirstTypedArrayType)), m_out.constInt32(NumberOfTypedArrayTypesExcludingDataView)));
        exitUnless(m_out.testIsZero32(m_out.load8ZeroExt32(base, m_heaps.JSArrayBufferView_mode), m_out.constInt32(isResizableOrGrowableSharedMode)));
        exitUnless(m_out.below(wideIndex, m_out.loadPtr(base, m_heaps.JSArrayBufferView_length)));
        LValue vector = m_out.loadPtr(base, m_heaps.JSArrayBufferView_vector);
        Vector<FTL::SwitchCase> cases;
        Vector<LBasicBlock, 8> blocks;
        for (auto& kind : typedArrayKinds) {
            blocks.append(m_out.newBlock());
            cases.append(FTL::SwitchCase(m_out.constInt32(kind.type), blocks.last(), FTL::Weight()));
        }
        m_out.switchInstruction(type, cases, m_exit, FTL::Weight());
        for (unsigned i = 0; i < std::size(typedArrayKinds); ++i) {
            auto& kind = typedArrayKinds[i];
            m_out.appendTo(blocks[i]);
            storeToTypedArray(kind.type, TypedPointer(m_heaps.TypedArrayProperties, m_out.add(vector, m_out.shl(wideIndex, m_out.constInt32(kind.logSize)))));
            m_out.jump(continuation);
        }
    } else
        m_out.jump(m_exit);

    m_out.appendTo(continuation);
}

void Lowering::guardGetLength(Node* guard)
{
    Node* baseNode = guard->use(guard->as<OpGetLength>().m_base);
    LValue base = lowJSValue(baseNode);
    if (isSubtype(baseNode->type, TTypedArray)) {
        exitUnless(m_out.testIsZero32(m_out.load8ZeroExt32(base, m_heaps.JSArrayBufferView_mode), m_out.constInt32(isResizableOrGrowableSharedMode)));
        LValue length = m_out.loadPtr(base, m_heaps.JSArrayBufferView_length);
        exitUnless(m_out.belowOrEqual(length, m_out.constInt64(INT32_MAX)));
        guard->lowered = boxInt32(m_out.castToInt32(length));
        return;
    }
    if (!mayBe(baseNode->type, TArray | TString)) {
        exitUnless(m_out.booleanFalse);
        guard->lowered = m_out.int64Zero;
        return;
    }
    if (!isSubtype(baseNode->type, TCell))
        exitUnless(isCell(base));

    LBasicBlock arrayCase = m_out.newBlock();
    LBasicBlock notArrayCase = m_out.newBlock();
    LBasicBlock ropeCase = m_out.newBlock();
    LBasicBlock notRopeCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    // An array that has storage of some kind has its length there. One above what an int32 holds is for the runtime.
    if (mayBe(baseNode->type, TArray)) {
        LValue indexingType = m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc);
        LValue isArrayWithStorage = m_out.bitAnd(m_out.testNonZero32(indexingType, m_out.constInt32(IsArray)), m_out.testNonZero32(indexingType, m_out.constInt32(IndexingShapeMask)));
        m_out.branch(isArrayWithStorage, unsure(arrayCase), unsure(notArrayCase));
    } else
        m_out.jump(notArrayCase);

    m_out.appendTo(arrayCase, notArrayCase);
    LValue arrayLength = m_out.load32(m_out.loadPtr(base, m_heaps.JSObject_butterfly), m_heaps.Butterfly_publicLength);
    exitUnless(m_out.greaterThanOrEqual(arrayLength, m_out.int32Zero));
    results.append(m_out.anchor(boxInt32(arrayLength)));
    m_out.jump(continuation);

    m_out.appendTo(notArrayCase, ropeCase);
    if (!mayBe(baseNode->type, TString))
        m_out.jump(m_exit);
    else {
        if (!isSubtype(baseNode->type, TString))
            exitUnless(isCellOfType(base, StringType));
        LValue fiber = m_out.loadPtr(base, m_heaps.JSRopeString_fiber0);
        m_out.branch(m_out.testNonZeroPtr(fiber, m_out.constIntPtr(JSString::isRopeInPointer)), rarely(ropeCase), usually(notRopeCase));

        m_out.appendTo(ropeCase, notRopeCase);
        results.append(m_out.anchor(boxInt32(m_out.load32(base, m_heaps.JSRopeString_length))));
        m_out.jump(continuation);

        m_out.appendTo(notRopeCase, continuation);
        results.append(m_out.anchor(boxInt32(m_out.load32(fiber, m_heaps.StringImpl_length))));
        m_out.jump(continuation);
    }

    m_out.appendTo(continuation);
    guard->lowered = m_out.phi(Int64, results);
}

void Lowering::guardCheckType(Node* guard)
{
    auto bytecode = guard->as<OpCheckType>();
    Node* value = guard->use(bytecode.m_value);
    unsigned mask = bytecode.m_mask;
    guard->lowered = nullptr;
    if (value->isKnownToPass(mask))
        return;

    LValue jsValue = lowJSValue(value);
    if (isSubtype(value->type & typeAdmittedByMask(mask), TNumber) && mayBe(value->type, TDouble)) {
        // What comes of it is going to be held as a double. Taking the offset of a double's encoding away leaves the double's own
        // bits, all of which are below what it leaves of anything else.
        LBasicBlock isDouble = m_out.newBlock();
        LBasicBlock isNotDouble = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue bits = m_out.add(jsValue, m_numberTag);
        static_assert(JSValue::NumberTag + JSValue::DoubleEncodeOffset == 0);
        m_out.branch(m_out.below(bits, m_out.constInt64(JSValue::NumberTag - JSValue::DoubleEncodeOffset)), usually(isDouble), rarely(isNotDouble));

        m_out.appendTo(isDouble, isNotDouble);
        ValueFromBlock doubleResult = m_out.anchor(m_out.bitCast(bits, Double));
        m_out.jump(continuation);

        m_out.appendTo(isNotDouble, continuation);
        exitUnless(isInt32(jsValue));
        ValueFromBlock int32Result = m_out.anchor(m_out.intToDouble(unboxInt32(jsValue)));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        guard->lowered = m_out.phi(Double, doubleResult, int32Result);
        return;
    }

    LBasicBlock passed = m_out.newBlock();
    emitTypeTests(value, jsValue, mask, passed, m_exit);
    m_out.appendTo(passed);
}

bool Lowering::guardResolveScope(Node* guard)
{
    auto bytecode = guard->as<OpResolveScope>();
    if (isStaticClosureVarResolveType(bytecode.m_resolveType))
        return false;
    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
    if (variable.kind == StaticVariable::Import) {
        LValue importer = lowCell(guard->use(bytecode.m_scope));
        for (unsigned i = 0; i < variable.depth; ++i)
            importer = m_out.loadPtr(importer, m_heaps.JSScope_next);
        guard->lowered = m_out.load64(importer, m_heaps.JSLexicalEnvironment_variables[variable.import.scopeOffsetOfSlot]);
        exitUnless(m_out.notZero64(guard->lowered));
        return true;
    }
    unsigned extra = m_graph.extraOfResolveScope(bytecode);
    if (variable.kind != StaticVariable::Unresolved || !Site::fits(bytecode.m_var, extra))
        return false;

    // See operationAOTResolveScope().
    LValue scope = lowCell(guard->use(bytecode.m_scope));
    unsigned slot = sharedSite(guard, bytecode.m_var, extra);
    LValue tag = m_out.castToInt32(m_out.lShr(m_out.load64(slotWord(slot, 0)), m_out.constInt32(32)));
    LValue epoch = m_out.load32(m_globalObject, m_heaps.JSGlobalObject_globalLexicalBindingEpoch);

    LBasicBlock byDepth = m_out.newBlock();
    LBasicBlock walk = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;
    results.append(m_out.anchor(m_out.loadPtr(slotWord(slot, 1))));
    m_out.branch(m_out.equal(tag, m_out.add(epoch, m_out.int32One)), usually(continuation), rarely(byDepth));

    m_out.appendTo(byDepth, walk);
    exitUnless(m_out.testNonZero32(tag, m_out.constInt32(Slot::resolvesByDepth)));
    ValueFromBlock startScope = m_out.anchor(scope);
    ValueFromBlock startDepth = m_out.anchor(m_out.bitAnd(tag, m_out.constInt32(~Slot::resolvesByDepth)));
    m_out.jump(walk);

    m_out.appendTo(walk, continuation);
    LValue current = m_out.phi(pointerType(), startScope);
    LValue depth = m_out.phi(Int32, startDepth);
    results.append(m_out.anchor(current));
    LBasicBlock further = m_out.newBlock();
    m_out.branch(m_out.isZero32(depth), unsure(continuation), unsure(further));
    m_out.appendTo(further);
    m_out.addIncomingToPhi(current, m_out.anchor(m_out.loadPtr(current, m_heaps.JSScope_next)));
    m_out.addIncomingToPhi(depth, m_out.anchor(m_out.sub(depth, m_out.int32One)));
    m_out.jump(walk);

    m_out.appendTo(continuation);
    guard->lowered = m_out.phi(pointerType(), results);
    return true;
}

bool Lowering::guardGetFromScope(Node* guard)
{
    auto bytecode = guard->as<OpGetFromScope>();
    ResolveType type = bytecode.m_getPutInfo.resolveType();
    LValue scope = lowCell(guard->use(bytecode.m_scope));

    // A function declaration that nobody has asked for yet is not there yet.
    auto loadLazily = [&](unsigned offset) {
        guard->lowered = m_out.load64(scope, m_heaps.JSLexicalEnvironment_variables[offset]);
        exitUnless(m_out.notZero64(guard->lowered));
        return true;
    };
    if (type == ResolvedClosureVar)
        return false;
    if (type == ResolvedLazyClosureVar)
        return loadLazily(bytecode.m_offset);
    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
    if (variable.kind == StaticVariable::Closure) {
        if (!variable.inModule)
            return false;
        return loadLazily(variable.offset.offset());
    }
    if (variable.kind == StaticVariable::Import)
        return loadLazily(variable.offset.offset());
    unsigned throwIfNotFound = m_graph.extraOfGetFromScope(bytecode);
    if (!variable.isCachedInSlot() || !Site::fits(bytecode.m_var, throwIfNotFound))
        return false;

    // See operationAOTGetFromScope().
    unsigned slot = sharedSite(guard, bytecode.m_var, throwIfNotFound);
    LValue word = m_out.load64(slotWord(slot, 0));
    exitUnless(m_out.equal(m_out.load32(scope, m_heaps.JSCell_structureID), lowHalf(m_out, word)));
    LValue pointer = m_out.loadPtr(slotWord(slot, 1));

    LBasicBlock hasPointer = m_out.newBlock();
    LBasicBlock isAddress = m_out.newBlock();
    LBasicBlock isSymbolTable = m_out.newBlock();
    LBasicBlock isProperty = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    m_out.branch(m_out.notNull(pointer), unsure(hasPointer), unsure(isProperty));

    m_out.appendTo(hasPointer, isAddress);
    m_out.branch(hasFlag(m_out, word, Slot::pointerIsCell), unsure(isSymbolTable), unsure(isAddress));

    // Whatever kind of scope the variable is in, this is the heap that stores to variables are in.
    const AbstractHeap& variables = m_heaps.JSLexicalEnvironment_variables.atAnyIndex();
    m_out.appendTo(isAddress, isSymbolTable);
    LValue atAddress = m_out.load64(TypedPointer(variables, pointer));
    exitUnless(m_out.notZero64(atAddress));
    results.append(m_out.anchor(atAddress));
    m_out.jump(continuation);

    m_out.appendTo(isSymbolTable, isProperty);
    exitUnless(m_out.equal(m_out.loadPtr(scope, m_heaps.JSSymbolTableObject_symbolTable), pointer));
    LValue offset = m_out.bitAnd(m_out.lShr(word, m_out.constInt32(32)), m_out.constInt64(Slot::offsetMask));
    LValue inEnvironment = m_out.load64(TypedPointer(variables, m_out.add(scope, m_out.add(m_out.shl(offset, m_out.constInt32(3)), m_out.constIntPtr(JSLexicalEnvironment::offsetOfVariables())))));
    exitUnless(m_out.notZero64(inEnvironment));
    results.append(m_out.anchor(inEnvironment));
    m_out.jump(continuation);

    m_out.appendTo(isProperty, continuation);
    results.append(m_out.anchor(loadProperty(scope, m_out.castToInt32(m_out.lShr(word, m_out.constInt32(32))))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    guard->lowered = m_out.phi(Int64, results);
    return true;
}

void Lowering::checkCallee(Node* guard)
{
    CallIntrinsic intrinsic = m_graph.intrinsicOfCall(guard);
    Node* calleeNode = guard->use(Graph::operandsOfCall(guard->instruction).callee);
    LValue callee = lowJSValue(calleeNode);
    if (!isSubtype(calleeNode->type, TCell))
        exitUnless(isCell(callee));
    exitUnless(isCellOfType(callee, JSFunctionType));

    LBasicBlock hasRareData = m_out.newBlock();
    LBasicBlock haveExecutable = m_out.newBlock();
    LValue executableOrRareData = m_out.loadPtr(callee, m_heaps.JSFunction_executableOrRareData);
    ValueFromBlock direct = m_out.anchor(executableOrRareData);
    m_out.branch(m_out.testNonZeroPtr(executableOrRareData, m_out.constIntPtr(JSFunction::rareDataTag)), rarely(hasRareData), usually(haveExecutable));
    m_out.appendTo(hasRareData, haveExecutable);
    ValueFromBlock indirect = m_out.anchor(m_out.loadPtr(m_out.address(m_heaps.FunctionRareData_executable, executableOrRareData, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag)));
    m_out.jump(haveExecutable);
    m_out.appendTo(haveExecutable);
    LValue executable = m_out.phi(pointerType(), direct, indirect);

    Entry function;
    switch (intrinsic) {
    case CallIntrinsic::MathSqrt: function = Entry::HostMathSqrt; break;
    case CallIntrinsic::MathAbs: function = Entry::HostMathAbs; break;
    case CallIntrinsic::MathFloor: function = Entry::HostMathFloor; break;
    case CallIntrinsic::MathCeil: function = Entry::HostMathCeil; break;
    case CallIntrinsic::MathTrunc: function = Entry::HostMathTrunc; break;
    case CallIntrinsic::MathFround: function = Entry::HostMathFround; break;
    case CallIntrinsic::MathMin: function = Entry::HostMathMin; break;
    case CallIntrinsic::MathMax: function = Entry::HostMathMax; break;
    case CallIntrinsic::MathIMul: function = Entry::HostMathIMul; break;
    case CallIntrinsic::StringCharCodeAt: function = Entry::HostStringCharCodeAt; break;
    case CallIntrinsic::ArrayPush: function = Entry::HostArrayPush; break;
    case CallIntrinsic::None:
        RELEASE_ASSERT_NOT_REACHED();
    }
    // Every kind of executable is big enough to have something there, and only in a native one is it ever the address of a function.
    exitUnless(m_out.equal(m_out.loadPtr(executable, m_heaps.NativeExecutable_function), entry(function)));
}

bool Lowering::guardCall(Node* guard)
{
    CallIntrinsic intrinsic = m_graph.intrinsicOfCall(guard);
    if (intrinsic == CallIntrinsic::None)
        return false;
    auto operands = Graph::operandsOfCall(guard->instruction);
    if (!guard->calleeIsChecked)
        checkCallee(guard);

    auto argument = [&](unsigned index) -> LValue {
        Node* node = guard->use(operands.argument(index));
        if (isSubtype(node->type, TNumber))
            return lowDouble(node);
        LValue value = lowJSValue(node);
        exitUnless(isNumber(value));
        return numberToDouble(value);
    };
    switch (intrinsic) {
    case CallIntrinsic::MathSqrt:
        guard->lowered = m_out.doubleSqrt(argument(1));
        break;
    case CallIntrinsic::MathAbs:
        guard->lowered = m_out.doubleAbs(argument(1));
        break;
    case CallIntrinsic::MathFloor:
        guard->lowered = m_out.doubleFloor(argument(1));
        break;
    case CallIntrinsic::MathCeil:
        guard->lowered = m_out.doubleCeil(argument(1));
        break;
    case CallIntrinsic::MathTrunc:
        guard->lowered = m_out.doubleTrunc(argument(1));
        break;
    case CallIntrinsic::MathFround:
        guard->lowered = m_out.floatToDouble(m_out.doubleToFloat(argument(1)));
        break;
    case CallIntrinsic::MathMin: {
        LValue left = argument(1);
        guard->lowered = m_out.doubleMin(left, argument(2));
        break;
    }
    case CallIntrinsic::MathMax: {
        LValue left = argument(1);
        guard->lowered = m_out.doubleMax(left, argument(2));
        break;
    }
    case CallIntrinsic::MathIMul: {
        LValue left = doubleToInt32(argument(1));
        guard->lowered = m_out.mul(left, doubleToInt32(argument(2)));
        break;
    }
    case CallIntrinsic::StringCharCodeAt: {
        // Of a string that has its characters together, at a place where there is one.
        Node* thisNode = guard->use(operands.argument(0));
        Node* indexNode = guard->use(operands.argument(1));
        LValue string = lowJSValue(thisNode);
        if (!isSubtype(thisNode->type, TString)) {
            if (!isSubtype(thisNode->type, TCell))
                exitUnless(isCell(string));
            exitUnless(isCellOfType(string, StringType));
        }
        LValue impl = m_out.loadPtr(string, m_heaps.JSString_value);
        exitUnless(m_out.testIsZeroPtr(impl, m_out.constIntPtr(JSString::isRopeInPointer)));
        LValue index;
        if (indexNode->isInteger())
            index = lowInt64(indexNode);
        else if (!mayBe(indexNode->type, TNumber)) {
            exitUnless(m_out.booleanFalse);
            index = m_out.int64Zero;
        } else {
            LBasicBlock haveIndex = m_out.newBlock();
            LValue narrow = lowIndex(indexNode, haveIndex, m_exit);
            m_out.appendTo(haveIndex);
            index = m_out.signExt32To64(narrow);
        }
        exitUnless(m_out.below(index, m_out.zeroExt(m_out.load32(impl, m_heaps.StringImpl_length), Int64)));
        LValue data = m_out.loadPtr(impl, m_heaps.StringImpl_data);
        LBasicBlock is8Bit = m_out.newBlock();
        LBasicBlock is16Bit = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.testNonZero32(m_out.load32(impl, m_heaps.StringImpl_hashAndFlags), m_out.constInt32(StringImpl::flagIs8Bit())), unsure(is8Bit), unsure(is16Bit));
        m_out.appendTo(is8Bit, is16Bit);
        ValueFromBlock narrowCharacter = m_out.anchor(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data, index))));
        m_out.jump(continuation);
        m_out.appendTo(is16Bit, continuation);
        ValueFromBlock wideCharacter = m_out.anchor(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), m_out.add(data, m_out.shl(index, m_out.constInt32(1))))));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        guard->lowered = m_out.phi(Int32, narrowCharacter, wideCharacter);
        break;
    }
    case CallIntrinsic::ArrayPush: {
        // On to an array that has room, in storage of a kind that the value can go in as it is.
        Node* thisNode = guard->use(operands.argument(0));
        Node* valueNode = guard->use(operands.argument(1));
        LValue array = lowJSValue(thisNode);
        if (!isSubtype(thisNode->type, TCell))
            exitUnless(isCell(array));
        exitUnless(isCellOfType(array, ArrayType));
        LValue indexingMode = m_out.bitAnd(m_out.load8ZeroExt32(array, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask | CopyOnWrite));
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        exitUnless(m_out.belowOrEqual(m_out.sub(indexingMode, m_out.constInt32(Int32Shape)), m_out.constInt32(ContiguousShape - Int32Shape)));
        LValue butterfly = m_out.loadPtr(array, m_heaps.JSObject_butterfly);
        LValue length = m_out.load32(butterfly, m_heaps.Butterfly_publicLength);
        exitUnless(m_out.below(length, m_out.load32(butterfly, m_heaps.Butterfly_vectorLength)));
        LValue wideLength = m_out.zeroExtPtr(length);

        LBasicBlock holdsValues = m_out.newBlock();
        LBasicBlock holdsNumbers = m_out.newBlock();
        LBasicBlock holdsInt32s = m_out.newBlock();
        LBasicBlock holdsDoubles = m_out.newBlock();
        LBasicBlock stored = m_out.newBlock();
        m_out.branch(m_out.equal(indexingMode, m_out.constInt32(ContiguousShape)), unsure(holdsValues), unsure(holdsNumbers));

        m_out.appendTo(holdsValues, holdsNumbers);
        m_out.store64(lowJSValue(valueNode), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, wideLength));
        m_out.jump(stored);

        m_out.appendTo(holdsNumbers, holdsInt32s);
        if (!mayBe(valueNode->type, TNumber))
            m_out.jump(m_exit);
        else
            m_out.branch(m_out.equal(indexingMode, m_out.constInt32(Int32Shape)), unsure(holdsInt32s), unsure(holdsDoubles));

        m_out.appendTo(holdsInt32s, holdsDoubles);
        {
            LValue value = lowJSValue(valueNode);
            if (valueNode->rep() != Rep::Int32)
                exitUnless(isInt32(value));
            m_out.store64(value, m_out.baseIndex(m_heaps.indexedInt32Properties, butterfly, wideLength));
            m_out.jump(stored);
        }

        m_out.appendTo(holdsDoubles, stored);
        {
            exitUnless(m_out.equal(indexingMode, m_out.constInt32(DoubleShape)));
            LValue number = argument(1);
            exitUnless(m_out.doubleEqual(number, number));
            m_out.storeDouble(number, m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly, wideLength));
            m_out.jump(stored);
        }

        m_out.appendTo(stored);
        guard->lowered = m_out.add(length, m_out.int32One);
        m_out.store32(guard->lowered, butterfly, m_heaps.Butterfly_publicLength);
        if (mayBe(valueNode->type, TCell))
            storeBarrier(array);
        break;
    }
    case CallIntrinsic::None:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return true;
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

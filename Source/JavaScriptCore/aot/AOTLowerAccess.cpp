/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "B3PatchpointValue.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

// The slot's first word is { structureID, offset }, low half first.
static LValue lowHalf(FTL::Output& out, LValue word) { return out.castToInt32(word); }
static LValue highHalf(FTL::Output& out, LValue word) { return out.castToInt32(out.lShr(word, out.constInt32(32))); }

LValue Lowering::loadProperty(LValue object, LValue offset)
{
    LBasicBlock inlineCase = m_out.newBlock();
    LBasicBlock outOfLineCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.lessThan(offset, m_out.constInt32(firstOutOfLineOffset)), unsure(inlineCase), unsure(outOfLineCase));

    m_out.appendTo(inlineCase, outOfLineCase);
    LValue inlineAddress = m_out.add(object, m_out.add(m_out.shl(m_out.signExt32ToPtr(offset), m_out.constInt32(3)), m_out.constIntPtr(JSObject::offsetOfInlineStorage())));
    ValueFromBlock inlineResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), inlineAddress)));
    m_out.jump(continuation);

    m_out.appendTo(outOfLineCase, continuation);
    LValue butterfly = m_out.loadPtr(object, m_heaps.JSObject_butterfly);
    // Out of line property n is at butterfly[-(n - firstOutOfLineOffset) - 2].
    LValue index = m_out.sub(m_out.constIntPtr(firstOutOfLineOffset - 2), m_out.signExt32ToPtr(offset));
    LValue outOfLineAddress = m_out.add(butterfly, m_out.shl(index, m_out.constInt32(3)));
    ValueFromBlock outOfLineResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), outOfLineAddress)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, inlineResult, outOfLineResult);
}

// word: the first word of a slot whose offset field is the location of a property (locationOfProperty()).
TypedPointer Lowering::cachedPropertyAddress(LValue object, LValue word, const AbstractHeap* heap)
{
    // (A direct slot may have a property name id above the location.)
    LValue location = m_out.select(m_out.testNonZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isIndirect) << 32)),
        m_out.aShr(m_out.shl(word, m_out.constInt32(32 - Slot::offsetBits)), m_out.constInt32(64 - Slot::offsetBits)),
        m_out.bitAnd(m_out.lShr(word, m_out.constInt32(32)), m_out.constInt64(Slot::directLocationMask)));
    LValue storage = m_out.select(m_out.lessThan(location, m_out.int64Zero), m_out.loadPtr(object, m_heaps.JSObject_butterfly), object);
    return TypedPointer(heap ? *heap : m_heaps.properties.atAnyNumber(), m_out.add(storage, m_out.shl(location, m_out.constInt32(3))));
}

std::optional<TypeTable::Field> Lowering::fieldAccessedBy(Node* node, unsigned identifier)
{
    if (!TypeTable::shared())
        return std::nullopt;
    UniquedStringImpl* name = node->graph->codeBlock()->identifier(identifier).impl();
    if (uint32_t tag = Graph::typeTagOf(node)) {
        if (auto field = TypeTable::shared()->fieldOf(tag, name))
            return field;
    }
    if (node->guard)
        return std::nullopt;
    return Graph::fieldOfTypedBase(node->use(node->opcode == op_get_by_id ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base), name);
}

LValue Lowering::layoutOf(LValue cell)
{
    return m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(cell), Structure::offsetOfKnownShape()));
}

LValue Lowering::loadTypedLayoutID(LValue cell)
{
    return m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(cell), Structure::offsetOfTypedLayoutID()));
}

TypedPointer Lowering::addressOfField(LValue object, const TypeTable::Field& field)
{
    if (field.isInObject())
        return m_out.address(m_heaps.properties.atAnyNumber(), object, JSObject::offsetOfInlineStorage() + field.slot * sizeof(EncodedJSValue));
    LValue butterfly = m_out.loadPtr(m_out.address(m_heaps.root, object, JSObject::butterflyOffset()));
    return m_out.address(m_heaps.properties.atAnyNumber(), butterfly, offsetInButterfly(firstOutOfLineOffset + (field.slot - field.inlineSlots)) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)));
}

LValue Lowering::toFieldRepresentation(Node* valueNode, LValue value, TypeTable::FieldType fieldType)
{
    if (fieldType.atoms && TypeTable::hasTypedFields())
        atomizeIfString(valueNode, value);
    if (!fieldType.isConstrained() || !TypeTable::hasTypedFields() || !mayBe(valueNode->type, TInt32))
        return value;
    if (isSubtype(valueNode->type, TNumber))
        return boxDouble(lowDouble(valueNode));
    return m_out.select(isInt32(value), boxDouble(m_out.intToDouble(unboxInt32(value))), value);
}

static Node* skipAliases(Node* node)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz) || node->isBytecode(op_type_tag))
        node = node->uses[0].node;
    return node;
}

auto Lowering::availableField(Node* base, const TypeTable::Field& field) const -> const AvailableField*
{
    base = skipAliases(base);
    for (auto& inHand : m_availableFields) {
        if (inHand.base == base && inHand.layoutID == field.first && inHand.slot == field.slot && inHand.id == field.id)
            return &inHand;
    }
    return nullptr;
}

void Lowering::recordAvailableField(Node* base, const TypeTable::Field& field, LValue value, Rep rep, LValue asJSValue, bool isWritten)
{
    base = skipAliases(base);
    // (Two nodes may be the same object, so a store invalidates the field for every base. An object's typed layout never changes,
    // so fields of other layouts are unaffected.)
    m_availableFields.removeAllMatching([&](auto& inHand) {
        return inHand.layoutID == field.first && inHand.slot == field.slot && (isWritten || inHand.base == base);
    });
    m_availableFields.append({ base, field.first, field.slot, field.id, rep, value, asJSValue });
}

// Whether the lowering of the node neither writes to an object nor runs any of the program's code. This must not claim more than
// the lowerings guarantee.
bool Lowering::preservesFields(Node* node)
{
    switch (node->kind) {
    case NodeKind::Constant:
    case NodeKind::ConstantCell:
    case NodeKind::Intrinsic:
    case NodeKind::LinkTimeConstant:
    case NodeKind::Argument:
    case NodeKind::Phi:
    case NodeKind::Proj:
    case NodeKind::GetStack:
    case NodeKind::SetStack:
    case NodeKind::Narrow:
        return true;
    case NodeKind::Guard:
        return false;
    case NodeKind::Bytecode:
        break;
    }
    if (node->guard)
        return false;
    // (Converting an object to a number or a string calls valueOf() or toString(), which is the program's code.)
    auto noOperandMayBeObject = [&] {
        for (auto& use : node->uses) {
            if (!use.node->type || mayBe(use.node->type, TAnyObject))
                return false;
        }
        return true;
    };
    switch (node->opcode) {
    case op_type_tag:
    case op_check_type:
    case op_check_tdz:
    case op_to_this:
    case op_typeof:
    case op_not:
    case op_stricteq:
    case op_nstricteq:
    case op_jstricteq:
    case op_jnstricteq:
    case op_jmp:
    case op_jtrue:
    case op_jfalse:
    case op_jeq_null:
    case op_jneq_null:
    case op_jundefined_or_null:
    case op_jnundefined_or_null:
    case op_eq_null:
    case op_neq_null:
    case op_is_undefined_or_null:
    case op_is_boolean:
    case op_is_number:
    case op_is_object:
    case op_is_callable:
    case op_is_cell_with_type:
    case op_is_empty:
    case op_loop_hint:
    case op_get_scope:
    case op_ret:
        return true;
    case op_add:
    case op_sub:
    case op_mul:
    case op_div:
    case op_mod:
    case op_pow:
    case op_negate:
    case op_inc:
    case op_dec:
    case op_bitand:
    case op_bitor:
    case op_bitxor:
    case op_bitnot:
    case op_lshift:
    case op_rshift:
    case op_urshift:
    case op_to_number:
    case op_to_numeric:
    case op_to_string:
    case op_less:
    case op_lesseq:
    case op_greater:
    case op_greatereq:
    case op_jless:
    case op_jlesseq:
    case op_jgreater:
    case op_jgreatereq:
    case op_jnless:
    case op_jnlesseq:
    case op_jngreater:
    case op_jngreatereq:
    case op_eq:
    case op_neq:
    case op_jeq:
    case op_jneq:
        return noOperandMayBeObject();
    case op_get_length: {
        Type base = node->use(node->as<OpGetLength>().m_base)->type;
        return base && isSubtype(base, TArray | TString);
    }
    case op_get_from_scope:
        switch (node->as<OpGetFromScope>().m_getPutInfo.resolveType()) {
        case ClosureVar:
        case ModuleVar:
        case GlobalLexicalVar:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

bool Lowering::isThisOfEscapingFunction(Node* node)
{
    return Graph::isThisOfEscapingFunction(node);
}

void Lowering::checkTypedLayout(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t layoutID)
{
    // According to the types in the source, only objects allocated with this typed layout reach here. That could not be proven, so
    // it is checked at run time. For example, untyped code can store any value in an array that it was passed.
    LBasicBlock isNot = newColdBlock();
    LBasicBlock is = m_out.newBlock();
    if (!isSubtype(valueNode->type, TCell)) {
        LBasicBlock cellCase = m_out.newBlock();
        m_out.branch(isCell(value), usually(cellCase), rarely(isNot));
        m_out.appendTo(cellCase);
    }
    m_out.branch(m_out.equal(loadTypedLayoutID(value), m_out.constInt32(layoutID)), usually(is), rarely(isNot));
    m_out.appendTo(isNot);
    coldCall(onBehalfOf, Entry::operationAOTCheckTypedLayout, value, m_out.constInt32(layoutID));
    m_out.jump(is);
    m_out.appendTo(is);
}

LValue Lowering::cachedCoercionFor(Node* node, uint16_t layoutID)
{
    for (;;) {
        if (node->isBytecode(op_type_tag) && node->firstLayout == layoutID) {
            if (auto it = m_coercions.find(node); it != m_coercions.end())
                return it->value;
        }
        if (node->kind != NodeKind::Narrow && !node->isBytecode(op_check_type) && !node->isBytecode(op_check_tdz) && !node->isBytecode(op_type_tag))
            return nullptr;
        node = node->uses[0].node;
    }
}

// (An object's typed layout never changes, so the result stays valid for as long as the value is live.)
LValue Lowering::coerceToTypedLayout(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t layoutID)
{
    LBasicBlock isNot = newColdBlock();
    LBasicBlock done = m_out.newBlock();
    if (!isSubtype(valueNode->type, TCell)) {
        LBasicBlock cellCase = m_out.newBlock();
        m_out.branch(isCell(value), usually(cellCase), rarely(isNot));
        m_out.appendTo(cellCase);
    }
    ValueFromBlock itself = m_out.anchor(value);
    m_out.branch(m_out.equal(loadTypedLayoutID(value), m_out.constInt32(layoutID)), usually(done), rarely(isNot));
    m_out.appendTo(isNot);
    ValueFromBlock whatItIsSeenAs = m_out.anchor(coldCallForValue(onBehalfOf, Entry::operationAOTCoerceToTypedLayout, value, m_out.constInt32(layoutID)));
    m_out.jump(done);
    m_out.appendTo(done);
    return m_out.phi(Int64, itself, whatItIsSeenAs);
}

Lowering::FieldStorage Lowering::fieldStorageFor(Node* onBehalfOf, Node* baseNode, LValue base, uint16_t layoutID)
{
    if (baseNode->hasLayoutInRange(layoutID, layoutID))
        return { base, false };
    // (An access whose base is in an untracked register has no op_type_tag node before it, so the type tag is taken from the access
    // itself.)
    uint32_t tag = Graph::typeTagOf(onBehalfOf);
    if (!TypeTable::shared()->isOpen(layoutID) || (tag && TypeTable::shared()->isTrusted(tag) && TypeTable::shared()->layoutIDOf(tag) == layoutID && !isThisOfEscapingFunction(baseNode))) {
        checkTypedLayout(onBehalfOf, baseNode, base, layoutID);
        return { base, false };
    }
    if (LValue view = cachedCoercionFor(baseNode, layoutID))
        return { view, true };
    return { coerceToTypedLayout(onBehalfOf, baseNode, base, layoutID), true };
}

// Returns zero for a value that is not a cell. An object's typed layout never changes, so once it has been loaded for a value it
// does not have to be reloaded, even after a call or a store.
LValue Lowering::loadTypedLayoutIDOrZero(Node* node, LValue value)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz))
        node = node->uses[0].node;
    if (auto it = m_loadedLayoutIDs.find(node); it != m_loadedLayoutIDs.end() && it->value.first == m_block)
        return it->value.second;
    LValue result;
    if (isSubtype(node->type, TCell))
        result = loadTypedLayoutID(value);
    else {
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock none = m_out.anchor(m_out.int32Zero);
        m_out.branch(isCell(value), usually(cellCase), rarely(continuation));
        m_out.appendTo(cellCase);
        ValueFromBlock ofCell = m_out.anchor(loadTypedLayoutID(value));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        result = m_out.phi(Int32, none, ofCell);
    }
    m_loadedLayoutIDs.set(node, std::pair { m_block, result });
    return result;
}

LValue Lowering::isOneOf(LValue layout, uint16_t first, uint16_t last)
{
    if (first == last)
        return m_out.equal(layout, m_out.constInt32(first));
    return m_out.belowOrEqual(m_out.sub(layout, m_out.constInt32(first)), m_out.constInt32(last - first));
}

void Lowering::lowerGetById(Node* node)
{
    auto bytecode = node->as<OpGetById>();
    Node* baseNode = node->use(bytecode.m_base);
    // A closed method that is called without its function object. The callee is known, so the only effect of the read is to throw
    // if the base is undefined or null.
    if (node->isReadOnlyToBeCalled) {
        if (!isSubtype(baseNode->type, TCell)) {
            LValue base = lowJSValue(baseNode);
            LBasicBlock isNone = newColdBlock();
            LBasicBlock isSomething = m_out.newBlock();
            m_out.branch(isCell(base), usually(isSomething), rarely(isNone));
            m_out.appendTo(isNone);
            coldCall(node, Entry::operationAOTCheckType, base, m_out.constInt32(MaskOtherObject), ColdCall::ChangesNothing);
            m_out.unreachable();
            m_out.appendTo(isSomething);
        }
        return;
    }
    if (node->builtinCalled) {
        lowerReadOfBuiltin(node, baseNode);
        return;
    }
    // (With Options::useAOTFunctionSplitting(), typed accesses are handled by the guards of the fast copy, and only the generic
    // copy reaches this point.)
    if (auto field = (Options::aotShapeOptimizations() & 2) && !Options::useAOTFunctionSplitting() && !Options::auditAOTTypedFields() ? fieldAccessedBy(node, bytecode.m_property) : std::nullopt) {
        // An object created by a literal in the program records its layout, and the type says which layouts have the property and
        // in which slot. Any other base, whatever its declared type, takes the generic path.
        LValue base = lowJSValue(baseNode);
        if (Options::useAOTTypedFields() && TypeTable::hasTypedFields() && field->id) {
            // The typed layout uses field IDs: the object's Structure says whether the field is in its slot. This applies whatever
            // is known about the object.
            bool allowsUndefined = field->isOptional || !field->fieldType.isConstrained() || (field->fieldType.kinds & MaskUndefined);
            Stub stub = static_cast<Stub>(static_cast<unsigned>(allowsUndefined ? Stub::ReadSlotOrUndefined0 : Stub::ReadSlot0) + field->slot);
            auto throughStub = [&]() -> LValue {
                return callStub(stub, Int64, { { base, GPRInfo::argumentGPR0 } }, { { GPRInfo::argumentGPR1, field->id } }, StubClobbers::CallerSavedRegisters, node);
            };
            const AvailableField* inHand = availableField(baseNode, *field);
            LValue previouslyReadValue = inHand ? inHand->asJSValue : nullptr;
            // (The slow path may call a getter, so forget all available fields.)
            m_availableFields.shrink(0);
            m_nodePreservesFields = true;
            if (previouslyReadValue) {
                // Nothing since the last read could have changed the field. Reuse the value, unless a read of this field has ever
                // done more than load it (Instance::fieldsWithObservableReads).
                unsigned index = field->slot << 16 | field->id;
                LBasicBlock again = newColdBlock();
                LBasicBlock continuation = m_out.newBlock();
                ValueFromBlock kept = m_out.anchor(previouslyReadValue);
                LValue bits = m_out.load8ZeroExt32(m_out.address(m_heaps.root, m_out.loadPtr(m_out.address(m_heaps.root, m_instance, Instance::offsetOfFieldsWithObservableReads())), index >> 3));
                m_out.branch(m_out.testIsZero32(bits, m_out.constInt32(1 << (index & 7))), usually(continuation), rarely(again));
                m_out.appendTo(again);
                ValueFromBlock readAgain = m_out.anchor(throughStub());
                m_out.jump(continuation);
                m_out.appendTo(continuation);
                LValue value = m_out.phi(Int64, kept, readAgain);
                setJSValue(node, value);
                recordAvailableField(baseNode, *field, value, Rep::JSValue, value, false);
                return;
            }
            // (In a loop the check is inline: it is five instructions, against two for the call of the stub.)
            if (isCompact() && (!m_block->isInLoop || m_block->isGeneric)) {
                LValue value = throughStub();
                setJSValue(node, value);
                recordAvailableField(baseNode, *field, value, Rep::JSValue, value, false);
                return;
            }
            LBasicBlock isThere = m_out.newBlock();
            LBasicBlock otherwise = newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            if (!isSubtype(baseNode->type, TCell)) {
                LBasicBlock cellCase = m_out.newBlock();
                m_out.branch(isCell(base), usually(cellCase), rarely(otherwise));
                m_out.appendTo(cellCase);
            }
            LValue whichIsThere = m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(base), Structure::offsetOfFieldIDInSlot() + field->slot * sizeof(uint16_t)));
            m_out.branch(m_out.equal(whichIsThere, m_out.constInt32(field->id)), usually(isThere), rarely(otherwise));
            m_out.appendTo(isThere);
            ValueFromBlock found = m_out.anchor(m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), base, JSObject::offsetOfInlineStorage() + field->slot * sizeof(EncodedJSValue))));
            m_out.jump(continuation);
            m_out.appendTo(otherwise);
            ValueFromBlock foundOut = m_out.anchor(throughStub());
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            LValue value = m_out.phi(Int64, found, foundOut);
            setJSValue(node, value);
            recordAvailableField(baseNode, *field, value, Rep::JSValue, value, false);
            return;
        }
        if (Options::useAOTTypedFields() && TypeTable::hasTypedFields()) {
            // The property is in its slot, or the object does not have it.
            auto [storageOfBase, mayBeStandIn] = fieldStorageFor(node, baseNode, base, field->first);
            if (!mayBeStandIn) {
                m_nodePreservesFields = true;
                if (const AvailableField* inHand = availableField(baseNode, *field)) {
                    if (inHand->asJSValue && node->rep() == Rep::JSValue)
                        setJSValue(node, inHand->asJSValue);
                    else {
                        setResult(node, inHand->value, inHand->rep);
                        if (inHand->asJSValue && node->rep() != Rep::JSValue)
                            node->loweredAsJSValue = inHand->asJSValue;
                    }
                    return;
                }
            }
            LValue valueInSlot = m_out.load64(addressOfField(storageOfBase, *field));
            bool allowsUndefined = field->isOptional || !field->fieldType.isConstrained() || (field->fieldType.kinds & MaskUndefined);
            if (mayBeStandIn) {
                RELEASE_ASSERT(field->isInObject());
                LBasicBlock slowCase = newColdBlock();
                LBasicBlock continuation = m_out.newBlock();
                Vector<ValueFromBlock, 3> results;
                results.append(m_out.anchor(valueInSlot));
                if ((field->isOptional || field->mayBeEmpty) && allowsUndefined) {
                    LBasicBlock nothingIsThere = m_out.newBlock();
                    m_out.branch(m_out.notZero64(valueInSlot), usually(continuation), rarely(nothingIsThere));
                    m_out.appendTo(nothingIsThere);
                    results.append(m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined()))));
                    m_out.branch(m_out.equal(storageOfBase, base), usually(continuation), rarely(slowCase));
                } else
                    m_out.branch(m_out.notZero64(valueInSlot), usually(continuation), rarely(slowCase));
                m_out.appendTo(slowCase);
                // (Later code assumes that the result has the field's type, so the operation either returns such a value or
                // throws.)
                uint64_t which = static_cast<uint64_t>(numberOf(bytecode.m_property)) | static_cast<uint64_t>(field->first) << 32 | static_cast<uint64_t>(field->slot) << 48 | static_cast<uint64_t>(allowsUndefined) << 56 | 1ull << 63; // (The last so that it is not taken for an address.)
                results.append(m_out.anchor(coldCallForValue(node, Entry::operationAOTGetFieldSlow, base, m_out.constInt64(which))));
                m_out.jump(continuation);
                m_out.appendTo(continuation);
                setJSValue(node, m_out.phi(Int64, results));
                return;
            }
            if (field->isOptional || (field->mayBeEmpty && allowsUndefined))
                valueInSlot = m_out.select(m_out.isZero64(valueInSlot), m_out.constInt64(JSValue::encode(jsUndefined())), valueInSlot);
            else if (field->mayBeEmpty) {
                // The type says the field is present, and later code relies on its type, so a missing field throws.
                LBasicBlock isMissing = newColdBlock();
                LBasicBlock isThere = m_out.newBlock();
                m_out.branch(m_out.notZero64(valueInSlot), usually(isThere), rarely(isMissing));
                m_out.appendTo(isMissing);
                coldCall(node, Entry::operationAOTCheckType, m_out.constInt64(JSValue::encode(jsUndefined())), m_out.constInt32(field->fieldType.kinds));
                m_out.unreachable();
                m_out.appendTo(isThere);
            }
            setJSValue(node, valueInSlot);
            recordAvailableField(baseNode, *field, node->lowered, node->rep(), valueInSlot, false);
            return;
        }
        bool resultIsTyped = Options::useAOTTypedFields() && field->fieldType.isConstrained();
        // (Only if the field's type allows undefined.)
        bool testsForLack = field->firstWithout && (Options::aotShapeOptimizations() & 8) && (!resultIsTyped || (field->fieldType.kinds & MaskUndefined));
        LBasicBlock has = m_out.newBlock();
        LBasicBlock hasNot = m_out.newBlock();
        LBasicBlock mayLack = m_out.newBlock();
        LBasicBlock otherwise = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        if (baseNode->hasLayoutInRange(field->first, field->last)) {
            m_out.jump(has);
        } else {
            // The layout the object was allocated with, whatever has happened to it since: the slot either holds the property or is
            // empty (Structure::typedLayoutID()).
            LValue layout = loadTypedLayoutIDOrZero(baseNode, base);
            if (!testsForLack)
                m_out.branch(isOneOf(layout, field->first, field->last), usually(has), rarely(otherwise));
            else {
                // (A layout ID of zero may mean that the value is not a cell.)
                LBasicBlock lacksLayout = m_out.newBlock();
                m_out.branch(isOneOf(layout, field->first, field->last), usually(has), unsure(lacksLayout));
                m_out.appendTo(lacksLayout);
                m_out.branch(m_out.notZero32(layout), usually(mayLack), rarely(otherwise));
            }
        }
        m_out.appendTo(has, mayLack);
        LValue valueInSlot = m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), base, JSObject::offsetOfInlineStorage() + field->slot * sizeof(EncodedJSValue)));
        LBasicBlock isThere = m_out.newBlock();
        m_out.branch(m_out.notZero64(valueInSlot), usually(isThere), rarely(otherwise));
        m_out.appendTo(isThere);
        ValueFromBlock found = m_out.anchor(valueInSlot);
        m_out.jump(continuation);
        // (Such an object inherits from Object.prototype, whose properties cannot change. TypeTable::fieldOf() rejects the names
        // that Object.prototype has.)
        m_out.appendTo(mayLack, hasNot);
        if (testsForLack)
            // (Whether the object lacks the property depends on its current layout, not on the one it was allocated with.)
            m_out.branch(isOneOf(layoutOf(base), field->firstWithout, field->lastWithout), unsure(hasNot), unsure(otherwise));
        else
            m_out.unreachable();
        m_out.appendTo(hasNot, otherwise);
        ValueFromBlock lacking = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
        m_out.jump(continuation);
        m_out.appendTo(otherwise, continuation);
        LValue slowCaseResult = getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property);
        if (resultIsTyped) {
            // Later code relies on the result having the field's type, so check it.
            LBasicBlock isThat = m_out.newBlock();
            LBasicBlock undecided = m_out.newBlock();
            emitTypeTests(nullptr, TTop, slowCaseResult, field->fieldType.kinds, isThat, undecided);
            m_out.appendTo(undecided);
            vmCall(node, Void, Entry::operationAOTCheckType, m_globalObject, slowCaseResult, m_out.constInt32(field->fieldType.kinds));
            m_out.jump(isThat);
            m_out.appendTo(isThat);
        }
        ValueFromBlock other = m_out.anchor(slowCaseResult);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, found, lacking, other));
        return;
    }
    setJSValue(node, getByIdCached(node, lowJSValue(baseNode), baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
}

// operation: takes the global object, the base, the identifier (in whatever form the operation expects) and the inline cache slot,
// which it fills.
LValue Lowering::getByIdCached(Node* node, LValue base, Type baseType, Entry operation, unsigned identifierOfFunction)
{
    unsigned identifier = operation == Entry::operationAOTGetByIdWellKnown ? identifierOfFunction : numberOf(identifierOfFunction);
    std::optional<Stub> stub;
    if (usesStubs && Site::fits(identifier, 0)) {
        if (operation == Entry::operationAOTGetById)
            stub = Stub::GetById;
        else if (operation == Entry::operationAOTGetByIdWellKnown)
            stub = Stub::GetByIdWellKnown;
    }
    unsigned slot = stub ? sharedSite(node, identifier) : allocateSlot();
    if (stub == Stub::GetById)
        m_graph.noteSelectorOfSite(slot, code().codeBlock()->identifier(identifierOfFunction).impl());
    auto throughStub = [&]() -> LValue {
        return callStub(*stub, Int64, { { base, GPRInfo::argumentGPR0 }, { slotAddress(slot), GPRInfo::argumentGPR1 } }, { });
    };
    if (stub && isCompact())
        return throughStub();

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock rightStructure = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (isSubtype(baseType, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, rightStructure);
    LValue word = m_out.load64(slotWord(slot, 0));
    m_out.branch(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(rightStructure), rarely(slowCase));

    m_out.appendTo(rightStructure, hit);
    ValueFromBlock fastResult;
    if (stub) {
        // Anything that takes more than a load from the base itself is left to the stub: getters, inherited properties and
        // out-of-line properties.
        m_out.branch(m_out.testIsZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isGetter | Slot::isIndirect) << 32)), usually(hit), rarely(slowCase));
        m_out.appendTo(hit, slowCase);
        LValue location = m_out.bitAnd(m_out.lShr(word, m_out.constInt32(32)), m_out.constInt64(Slot::directLocationMask));
        fastResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(base, m_out.shl(location, m_out.constInt32(3))))));
    } else {
        m_out.branch(m_out.testIsZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isGetter) << 32)), usually(hit), rarely(slowCase));

        // The property is in the base, or in a holder that every base with this structure inherits it from (see cacheGetById()).
        // (For a direct slot, the second word holds the name instead: Slot::name.)
        m_out.appendTo(hit, slowCase);
        LValue holder = m_out.loadPtr(slotWord(slot, 1));
        LValue isOnHolder = m_out.bitAnd(m_out.testNonZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isIndirect) << 32)), m_out.notNull(holder));
        fastResult = m_out.anchor(m_out.load64(cachedPropertyAddress(m_out.select(isOnHolder, holder, base), word)));
    }
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(stub ? throughStub() : vmCall(node, Int64, operation, m_globalObject, base, m_out.constInt32(identifier), slotAddress(slot)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, fastResult, slowResult);
}

void Lowering::lowerPutById(Node* node)
{
    auto bytecode = node->as<OpPutById>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* valueNode = node->use(bytecode.m_value);
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);
    uint32_t flags = (bytecode.m_flags.isDirect() ? 1 : 0) | (bytecode.m_flags.ecmaMode().isStrict() ? 2 : 0);
    LBasicBlock afterTypedStore = nullptr;
    // (A field of a typed layout that uses field IDs is stored like any other property. The generic store checks the value against
    // the field's type and caches the location.)
    if (auto field = (Options::aotShapeOptimizations() & 4) && !Options::useAOTFunctionSplitting() && !Options::auditAOTTypedFields() ? fieldAccessedBy(node, bytecode.m_property) : std::nullopt; field && !field->id) {
        // As for a read. A property in such a layout is always writable, like every property that a literal creates.
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock has = m_out.newBlock();
        LBasicBlock otherwise = Options::useAOTTypedFields() && TypeTable::hasTypedFields() ? newColdBlock() : m_out.newBlock();
        afterTypedStore = m_out.newBlock();
        if (Options::useAOTTypedFields() && TypeTable::hasTypedFields()) {
            auto [storageOfBase, mayBeStandIn] = fieldStorageFor(node, baseNode, base, field->first);
            // A value that the field's type does not accept takes the slow path, which converts or rejects it. So does a store that
            // adds the property to the object.
            bool mayBeRejected = branchUnlessAccepted(valueNode, value, field->fieldType, otherwise);
            TypedPointer slotOfField = addressOfField(storageOfBase, *field);
            LValue storedValue = toFieldRepresentation(valueNode, value, field->fieldType);
            // (If the value may be rejected, this block does not dominate what follows, so the field cannot be recorded as
            // available.)
            if (!mayBeStandIn && !mayBeRejected) {
                // (Adding a field to an object may call the runtime, but that runs none of the program's code and does not change
                // any other field.)
                m_nodePreservesFields = true;
                if (valueNode->type && isSubtype(valueNode->type, TNumber))
                    recordAvailableField(baseNode, *field, lowDouble(valueNode), Rep::Double, storedValue, true);
                else
                    recordAvailableField(baseNode, *field, storedValue, Rep::JSValue, storedValue, true);
            }
            // (A base without the typed layout is represented by a stand-in whose slots are all empty. Nothing is ever stored to
            // it.)
            if (field->isOptional || field->mayBeEmpty || mayBeStandIn) {
                RELEASE_ASSERT(!mayBeStandIn || field->isInObject());
                LBasicBlock isThere = m_out.newBlock();
                LBasicBlock isEmpty = (field->isOptional || field->mayBeEmpty) && field->isInObject() ? m_out.newBlock() : nullptr;
                m_out.branch(m_out.notZero64(m_out.load64(slotOfField)), unsure(isThere), isEmpty ? unsure(isEmpty) : rarely(otherwise));
                if (isEmpty) {
                    // Add the field to the object. The new Structure is cached from the last object with the same Structure that
                    // gained the field (Instance::fieldAdditions).
                    m_out.appendTo(isEmpty);
                    if (mayBeStandIn)
                        orElse(m_out.equal(storageOfBase, base), otherwise);
                    if (isCompact())
                        orElse(m_out.notNull(callHelper(Stub::HelperAddField, { base, storedValue, m_out.constInt32(field->slot) })), otherwise);
                    else
                        addTypedField(base, storedValue, m_out.constInt32(field->slot), otherwise);
                    // (Needed whatever the value is, because the object now refers to another Structure.)
                    storeBarrier(base);
                    m_out.jump(afterTypedStore);
                }
                m_out.appendTo(isThere);
            }
            m_out.store64(storedValue, slotOfField);
            if (mayBe(valueNode->type, TCell))
                storeBarrier(base);
            m_out.jump(afterTypedStore);
            m_out.appendTo(cellCase);
            m_out.unreachable();
            m_out.appendTo(has);
            m_out.unreachable();
            m_out.appendTo(otherwise, afterTypedStore);
        } else {
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(otherwise));
        m_out.appendTo(cellCase, has);
        m_out.branch(isOneOf(layoutOf(base), field->first, field->last), usually(has), rarely(otherwise));
        m_out.appendTo(has, otherwise);
        // (A value that the field's type does not accept is stored by the slow path, which moves the property out of the slot.)
        if (Options::useAOTTypedFields())
            branchUnlessAccepted(valueNode, value, field->fieldType.kindsOnly(), otherwise);
        m_out.store64(value, m_out.address(m_heaps.properties.atAnyNumber(), base, JSObject::offsetOfInlineStorage() + field->slot * sizeof(EncodedJSValue)));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        m_out.jump(afterTypedStore);
        m_out.appendTo(otherwise, afterTypedStore);
        }
    }
    if (!afterTypedStore && !bytecode.m_flags.isDirect() && mayBe(baseNode->type, TArray) && mayBe(valueNode->type, TInt32) && code().codeBlock()->identifier(bytecode.m_property).impl() == m_graph.vm().propertyNames->length.impl()) {
        LBasicBlock otherwise = m_out.newBlock();
        afterTypedStore = m_out.newBlock();
        if (isCompact())
            orElse(m_out.notNull(callHelper(Stub::HelperSetArrayLength, { base, value })), otherwise);
        else
            setLengthOfArray(base, value, otherwise);
        m_out.jump(afterTypedStore);
        m_out.appendTo(otherwise);
    }
    auto finish = makeScopeExit([&] {
        if (afterTypedStore) {
            m_out.jump(afterTypedStore);
            m_out.appendTo(afterTypedStore);
        }
    });
    if (isCompact() && Site::fits(numberOf(bytecode.m_property), flags)) {
        unsigned slot = sharedSite(node, numberOf(bytecode.m_property), flags);
        m_graph.noteSelectorOfSite(slot, code().codeBlock()->identifier(bytecode.m_property).impl());
        callStub(Stub::PutById, Void, { { base, GPRInfo::argumentGPR0 }, { value, GPRInfo::argumentGPR1 }, { slotAddress(slot), GPRInfo::argumentGPR2 } }, { });
        return;
    }
    unsigned slot = allocateSlot();

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock transition = m_out.newBlock();
    LBasicBlock stored = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (isSubtype(baseNode->type, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, hit);
    LValue word = m_out.load64(slotWord(slot, 0));
    m_out.branch(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(hit), rarely(slowCase));

    m_out.appendTo(hit, transition);
    LValue secondWord = m_out.load64(slotWord(slot, 1));
    if (Options::useAOTTypedFields() && TypeTable::hasTypedFields()) {
        // (Slot::fieldType is set, so the value has to be checked. The runtime does that.)
        LBasicBlock isPlain = m_out.newBlock();
        m_out.branch(m_out.isZero64(m_out.lShr(secondWord, m_out.constInt32(32))), usually(isPlain), rarely(slowCase));
        m_out.appendTo(isPlain);
    }
    m_out.store64(value, cachedPropertyAddress(base, word));
    LValue newStructureID = lowHalf(m_out, secondWord);
    m_out.branch(m_out.notZero32(newStructureID), unsure(transition), unsure(stored));

    m_out.appendTo(transition, stored);
    m_out.store32(newStructureID, base, m_heaps.JSCell_structureID);
    m_out.jump(stored);

    m_out.appendTo(stored, slowCase);
    if (mayBe(valueNode->type, TCell))
        storeBarrier(base);
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    vmCall(node, Void, Entry::operationAOTPutById, m_globalObject, base, value, m_out.constInt32(numberOf(bytecode.m_property)), slotAddress(slot), m_out.constInt32(flags));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// Returns the property as an array index and jumps to haveIndex. Jumps to notIndex if the property is not an index.
LValue Lowering::lowIndex(Node* propertyNode, LBasicBlock haveIndex, LBasicBlock notIndex)
{
    if (propertyNode->rep() == Rep::Int32) {
        LValue index = lowInt32(propertyNode);
        m_out.jump(haveIndex);
        return index;
    }
    if (propertyNode->rep() == Rep::Int64) {
        LValue wide = lowRaw(propertyNode);
        LValue index = m_out.castToInt32(wide);
        m_out.branch(m_out.belowOrEqual(wide, m_out.constInt64(INT32_MAX)), usually(haveIndex), rarely(notIndex));
        return index;
    }
    if (propertyNode->rep() == Rep::Double) {
        LValue asDouble = lowDouble(propertyNode);
        LValue index = m_out.doubleToInt32(asDouble);
        m_out.branch(m_out.doubleEqual(m_out.intToDouble(index), asDouble), usually(haveIndex), rarely(notIndex));
        return index;
    }
    LValue property = lowJSValue(propertyNode);
    LValue index = unboxInt32(property);
    m_out.branch(isInt32(property), usually(haveIndex), unsure(notIndex));
    return index;
}

void Lowering::lowerGetByVal(Node* node)
{
    auto bytecode = node->as<OpGetByVal>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* propertyNode = node->use(bytecode.m_property);
    LValue base = lowJSValue(baseNode);
    bool allowsEmpty = node->graph->readsElementsOrEmpty;

    if (auto* view = viewOf(node, baseNode)) {
        LBasicBlock inBounds = m_out.newBlock();
        LBasicBlock slowCase = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue index = lowInt64(propertyNode);
        m_out.branch(m_out.below(index, view->limit), usually(inBounds), rarely(slowCase));
        m_out.appendTo(inBounds);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, view->butterfly, index));
        ValueFromBlock fast = m_out.anchor(element);
        // (For an array with this indexing type, a hole means the element is absent. If its prototype chain had indexed properties,
        // the array would use another indexing type.)
        if (allowsEmpty)
            m_out.jump(continuation);
        else
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(slowCase));
        m_out.appendTo(slowCase);
        ValueFromBlock slow = m_out.anchor(coldCallForValue(node, allowsEmpty ? Entry::operationAOTGetElementOrEmpty : Entry::operationAOTGetByVal, base, lowJSValue(propertyNode), ColdCall::ChangesNothing));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fast, slow));
        return;
    }

    // An element read by an integer in a loop is nearly always an element of an array, and the fast path for that is barely larger
    // than the call of the stub.
    bool isByIntegerInLoop = propertyNode->isInteger() && m_block->isInLoop && !m_block->isGeneric;
    if (isCompact() && !allowsEmpty && !isByIntegerInLoop) {
        if (propertyNode->rep() == Rep::Int64)
            setJSValue(node, callBinaryStub(node, Stub::GetByValAtIndex, Int64, base, lowRaw(propertyNode)));
        else
            setJSValue(node, callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode)));
        return;
    }

    bool isOfArray = isSubtype(baseNode->type, TArray);
    // (If the base is not known to be an array, a hole proves nothing, and the runtime is called.)
    bool missingMeansAbsent = allowsEmpty && isOfArray;
    if (allowsEmpty && !isOfArray && Options::verboseAOTCompilation()) [[unlikely]] {
        dataLog("AOT: LEAN an element is read from what is not known for an array: ");
        baseNode->dump(WTF::dataFile());
        dataLogLn(m_block->isGeneric ? " (in the second copy of a loop)" : "", m_block->isInLoop ? " (in a loop)" : "");
    }
    LBasicBlock slowCase = isOfArray ? newColdBlock() : m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    // An in-bounds, non-hole element of an array-like with contiguous or int32 storage.
    if (mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock haveIndex = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, haveIndex, slowCase);

        m_out.appendTo(haveIndex, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        // Int32Shape and ContiguousShape hold JSValues; DoubleShape sits between them.
        LValue isJSValueShape = m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape)));
        m_out.branch(isJSValueShape, usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, inBounds);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), rarely(slowCase));

        m_out.appendTo(inBounds, slowCase);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        results.append(m_out.anchor(element));
        if (missingMeansAbsent)
            m_out.jump(continuation);
        else
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    if (allowsEmpty && !isOfArray)
        results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetElementOrEmpty, m_globalObject, base, lowJSValue(propertyNode))));
    else if (isOfArray)
        results.append(m_out.anchor(coldCallForValue(node, allowsEmpty ? Entry::operationAOTGetElementOrEmpty : Entry::operationAOTGetByVal, base, lowJSValue(propertyNode))));
    else if constexpr (usesStubs)
        results.append(m_out.anchor(callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode))));
    else
        results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetByVal, m_globalObject, base, lowJSValue(propertyNode))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

void Lowering::lowerPutByVal(Node* node)
{
    auto bytecode = node->as<OpPutByVal>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* propertyNode = node->use(bytecode.m_property);
    Node* valueNode = node->use(bytecode.m_value);
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);

    auto throughStub = [&] {
        bool isInteger = propertyNode->rep() == Rep::Int64;
        callStub(isInteger ? Stub::PutByValAtIndex : Stub::PutByVal, Void, { { base, GPRInfo::argumentGPR0 }, { isInteger ? lowRaw(propertyNode) : lowJSValue(propertyNode), GPRInfo::argumentGPR1 }, { value, GPRInfo::argumentGPR2 } },
            { { GPRInfo::argumentGPR3, bytecode.m_ecmaMode.isStrict() } });
    };
    if (isCompact()) {
        throughStub();
        return;
    }

    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    // The fast path stores an element that fits in contiguous storage. Growing the storage, and every other indexing shape, is left
    // to the runtime.
    if (mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock haveIndex = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock beyondLength = m_out.newBlock();
        LBasicBlock lengthen = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, haveIndex, slowCase);

        m_out.appendTo(haveIndex, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        // Copy on write storage has the shape bits of what it holds and one more bit.
        LValue indexingMode = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask | CopyOnWrite));
        m_out.branch(m_out.equal(indexingMode, m_out.constInt32(ContiguousShape)), usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, beyondLength);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), unsure(beyondLength));

        m_out.appendTo(beyondLength, lengthen);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_vectorLength)), unsure(lengthen), unsure(slowCase));

        // The elements between the old length and the index are already holes.
        m_out.appendTo(lengthen, inBounds);
        m_out.store32(m_out.add(index, m_out.int32One), butterfly, m_heaps.Butterfly_publicLength);
        m_out.jump(inBounds);

        m_out.appendTo(inBounds, slowCase);
        m_out.store64(value, m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        m_out.jump(continuation);
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    if constexpr (usesStubs)
        throughStub();
    else
        vmCall(node, Void, Entry::operationAOTPutByVal, m_globalObject, base, lowJSValue(propertyNode), value, m_out.constInt32(bytecode.m_ecmaMode.isStrict()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// ---- Scopes

B3::Variable* Lowering::variableOfEnvironment(Node* environment, unsigned offset)
{
    auto& variables = m_variablesOfEnvironments.ensure(environment, [&] {
        JSValue table = environment->graph->codeBlock()->getConstant(environment->as<OpCreateLexicalEnvironment>().m_symbolTable);
        Vector<B3::Variable*> result;
        for (unsigned i = uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize(); i--;)
            result.append(m_proc.addVariable(Int64));
        return result;
    }).iterator->value;
    RELEASE_ASSERT(offset < variables.size());
    return variables[offset];
}

LValue Lowering::ancestorScope(Node* scope, unsigned hops)
{
    LValue current = lowCell(scope);
    for (unsigned i = 0; i < hops; ++i)
        current = m_out.loadPtr(current, m_heaps.JSScope_next);
    return current;
}

LValue Lowering::scopeToResolveFrom(Node* resolve)
{
    if (resolve->environmentsPassedOver)
        return ancestorScope(resolve->scopeToStartFrom, resolve->hopsFromThere);
    return lowCell(resolve->use(resolve->as<OpResolveScope>().m_scope));
}

void Lowering::lowerResolveScope(Node* node)
{
    auto bytecode = node->as<OpResolveScope>();
    if (auto distance = m_graph.distanceOfEnvironmentResolvedTo(node)) {
        setJSValue(node, environmentAt(*distance));
        return;
    }
    if (node->scopeToStartFrom && !node->environmentsPassedOver) {
        setJSValue(node, ancestorScope(node->scopeToStartFrom, node->hopsFromThere));
        return;
    }
    LValue scope = scopeToResolveFrom(node);

    auto walk = [&](unsigned depth) {
        RELEASE_ASSERT(depth >= node->environmentsPassedOver);
        LValue current = scope;
        for (unsigned i = node->environmentsPassedOver; i < depth; ++i)
            current = m_out.loadPtr(current, m_heaps.JSScope_next);
        return current;
    };

    if (isStaticClosureVarResolveType(bytecode.m_resolveType)) {
        setJSValue(node, walk(bytecode.m_localScopeDepth + staticClosureVarHops(bytecode.m_resolveType)));
        return;
    }

    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
    if (variable.isAtStaticDepth()) {
        setJSValue(node, walk(variable.depth));
        return;
    }
    if (variable.kind == StaticVariable::Import) {
        // The environment of the exporting module. It is cached in a slot of the importing module's environment the first time it
        // is needed.
        LValue importer = walk(variable.depth);
        LValue exporter = m_out.load64(importer, m_heaps.JSLexicalEnvironment_variables[variable.import.scopeOffsetOfSlot]);
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(exporter);
        m_out.branch(m_out.isZero64(exporter), rarely(slowCase), usually(continuation));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTFillImportSlot, m_globalObject, importer, m_out.constInt32(variable.import.slot)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
        return;
    }

    unsigned extra = code().extraOfResolveScope(bytecode);
    if (isFusedWithGetFromScope(node))
        return;
    if (usesStubs && variable.kind == StaticVariable::Unresolved && Site::fits(numberOf(bytecode.m_var), extra)) {
        setJSValue(node, callStub(Stub::ResolveScope, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(sharedSite(node, numberOf(bytecode.m_var), extra)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;
    if (variable.kind == StaticVariable::Unresolved) {
        LValue cached = m_out.loadPtr(slotWord(slot, 1));
        LValue epochPlusOne = highHalf(m_out, m_out.load64(slotWord(slot, 0)));
        LValue epoch = m_out.load32(m_out.address(m_heaps.root, m_globalObject, JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()));
        results.append(m_out.anchor(cached));
        m_out.branch(m_out.equal(epochPlusOne, m_out.add(epoch, m_out.constInt32(1))), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTResolveScope, m_globalObject, scope, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(slot), m_out.constInt32(variable.isInGlobalScopes ? Site::resolvesInGlobalScopes : 0))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

bool Lowering::isFusedWithGetFromScope(Node* node)
{
    if (!usesStubs || !node->isBytecode(op_resolve_scope) || node->block != m_block || node->useCount != 1)
        return false;
    auto resolve = node->as<OpResolveScope>();
    if (isStaticClosureVarResolveType(resolve.m_resolveType) || !Site::fits(numberOf(resolve.m_var), code().extraOfResolveScope(resolve)))
        return false;
    if (resolveStatically(resolve.m_var, resolve.m_localScopeDepth, resolve.m_resolveType).kind != StaticVariable::Unresolved)
        return false;

    // This is called while lowering either of the two nodes.
    unsigned index = m_block->nodes[m_nodeIndex] == node ? m_nodeIndex : m_nodeIndex - 1;
    if (index >= m_block->nodes.size() || m_block->nodes[index] != node || index + 1 >= m_block->nodes.size())
        return false;
    Node* next = m_block->nodes[index + 1];
    if (!next->isBytecode(op_get_from_scope))
        return false;
    auto get = next->as<OpGetFromScope>();
    ResolveType type = get.m_getPutInfo.resolveType();
    return next->use(get.m_scope) == node && get.m_var == resolve.m_var
        && type != ResolvedClosureVar && type != ResolvedLazyClosureVar
        && resolveStatically(get.m_var, get.m_localScopeDepth, type).kind == StaticVariable::Unresolved;
}

void Lowering::lowerGetFromScope(Node* node)
{
    auto bytecode = node->as<OpGetFromScope>();
    if (node->promotedEnvironment) {
        setJSValue(node, m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), variableOfEnvironment(node->promotedEnvironment, node->offsetInEnvironment)));
        return;
    }
    if (Node* resolveNode = node->use(bytecode.m_scope); isFusedWithGetFromScope(resolveNode)) {
        auto resolve = resolveNode->as<OpResolveScope>();
        LValue scope = scopeToResolveFrom(resolveNode);
        unsigned site = allocateSite(resolveNode, numberOf(resolve.m_var), code().extraOfResolveScope(resolve));
        unsigned siteOfGet = allocateSite(node, numberOf(bytecode.m_var), code().extraOfGetFromScope(bytecode));
        RELEASE_ASSERT(siteOfGet == site + 1);
        setJSValue(node, callStub(Stub::GetGlobal, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(site), GPRInfo::argumentGPR1 } }, { }));
        return;
    }
    auto distance = m_graph.distanceOfEnvironmentAccessed(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    ResolveType type = bytecode.m_getPutInfo.resolveType();

    auto loadClosureVariable = [&](unsigned offset, bool mayBeLazy) {
        LValue value = m_out.load64(scope, m_heaps.JSLexicalEnvironment_variables[offset]);
        if (!mayBeLazy) {
            setJSValue(node, value);
            return;
        }
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(value);
        m_out.branch(m_out.isZero64(value), rarely(slowCase), usually(continuation));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTReadLazyClosureVar, m_globalObject, distance ? environmentAt(*distance) : scope, m_out.constInt32(offset)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
    };

    if (type == ResolvedClosureVar) {
        loadClosureVariable(bytecode.m_offset, false);
        return;
    }
    if (type == ResolvedLazyClosureVar) {
        loadClosureVariable(bytecode.m_offset, true);
        return;
    }

    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
    if (variable.kind == StaticVariable::Closure) {
        loadClosureVariable(variable.offset.offset(), variable.inModule);
        return;
    }
    if (variable.kind == StaticVariable::Import) {
        // The scope is the exporting module's environment (see lowerResolveScope()).
        loadClosureVariable(variable.offset.offset(), true);
        return;
    }

    unsigned throwIfNotFound = code().extraOfGetFromScope(bytecode);
    if (usesStubs && variable.isCachedInSlot() && Site::fits(numberOf(bytecode.m_var), throwIfNotFound)) {
        setJSValue(node, callStub(Stub::GetFromScope, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(sharedSite(node, numberOf(bytecode.m_var), throwIfNotFound)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    if (variable.isCachedInSlot()) {
        LBasicBlock hasAddress = m_out.newBlock();
        LBasicBlock noAddress = m_out.newBlock();
        LBasicBlock structureHit = m_out.newBlock();

        // In both cases the cached location is only valid for a scope with the same structure, because the name may resolve to a
        // different scope.
        LValue word = m_out.load64(slotWord(slot, 0));
        m_out.branch(m_out.equal(m_out.load32(scope, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(structureHit), rarely(slowCase));

        m_out.appendTo(structureHit, hasAddress);
        LValue address = m_out.loadPtr(slotWord(slot, 1));
        m_out.branch(m_out.notNull(address), unsure(hasAddress), unsure(noAddress));

        m_out.appendTo(hasAddress, noAddress);
        LValue value = m_out.load64(TypedPointer(m_heaps.root, address));
        results.append(m_out.anchor(value));
        m_out.branch(m_out.notZero64(value), usually(continuation), rarely(slowCase));

        m_out.appendTo(noAddress, slowCase);
        results.append(m_out.anchor(loadProperty(scope, highHalf(m_out, word))));
        m_out.jump(continuation);
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetFromScope, m_globalObject, scope, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(slot), m_out.constInt32(throwIfNotFound))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

void Lowering::lowerPutToScope(Node* node)
{
    auto bytecode = node->as<OpPutToScope>();
    if (node->promotedEnvironment) {
        m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), variableOfEnvironment(node->promotedEnvironment, node->offsetInEnvironment), lowJSValue(node->use(bytecode.m_value)));
        return;
    }
    auto distance = m_graph.distanceOfEnvironmentAccessed(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    Node* valueNode = node->use(bytecode.m_value);
    LValue value = lowJSValue(valueNode);
    ResolveType type = bytecode.m_getPutInfo.resolveType();

    std::optional<unsigned> closureOffset;
    if (type == ResolvedClosureVar)
        closureOffset = bytecode.m_offset;
    else {
        StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_symbolTableOrScopeDepth.scopeDepth(), type);
        if (variable.kind == StaticVariable::Closure && (!variable.isReadOnly || isInitialization(bytecode.m_getPutInfo.initializationMode())))
            closureOffset = variable.offset.offset();
    }

    if (closureOffset) {
        // No watchpoint has to be fired: when AOT code is present, the optimizing JITs do not constant-fold closure variables
        // (DFG::Graph::tryGetConstantClosureVar()).
        m_out.store64(value, scope, m_heaps.JSLexicalEnvironment_variables[*closureOffset]);
        if (mayBe(valueNode->type, TCell))
            storeBarrier(scope);
        return;
    }

    // What the operation needs to know about the instruction, packed to fit in a Site.
    GetPutInfo info = bytecode.m_getPutInfo;
    static_assert(static_cast<unsigned>(ThrowIfNotFound) <= 1 && static_cast<unsigned>(DoNotThrowIfNotFound) <= 1);
    unsigned how = static_cast<unsigned>(info.resolveMode()) | static_cast<unsigned>(info.initializationMode()) << 1 | info.ecmaMode().isStrict() << 3;
    RELEASE_ASSERT(static_cast<unsigned>(info.initializationMode()) <= 3);

    if (usesStubs && Site::fits(numberOf(bytecode.m_var), how)) {
        callStub(Stub::PutToScope, Void, { { scope, GPRInfo::argumentGPR0 }, { value, GPRInfo::argumentGPR1 }, { slotAddress(allocateSite(node, numberOf(bytecode.m_var), how)), GPRInfo::argumentGPR2 } }, { });
        return;
    }
    vmCall(node, Void, Entry::operationAOTPutToScope, m_globalObject, scope, value, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(allocateSlot()), m_out.constInt32(how));
}

bool Lowering::tryLowerAccess(Node* node)
{
    switch (node->opcode) {
    case op_get_by_id:
        lowerGetById(node);
        return true;
    case op_put_by_id:
        lowerPutById(node);
        return true;
    case op_get_by_val:
        lowerGetByVal(node);
        return true;
    case op_put_by_val:
        lowerPutByVal(node);
        return true;
    case op_resolve_scope:
        lowerResolveScope(node);
        return true;
    case op_get_from_scope:
        lowerGetFromScope(node);
        return true;
    case op_put_to_scope:
        lowerPutToScope(node);
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)

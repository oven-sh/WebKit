/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

#include "B3PatchpointValue.h"
#include "BytecodeStructs.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "SymbolTable.h"
#include "TypedArrayType.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

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
    LValue index = m_out.sub(m_out.constIntPtr(firstOutOfLineOffset - 2), m_out.signExt32ToPtr(offset));
    LValue outOfLineAddress = m_out.add(butterfly, m_out.shl(index, m_out.constInt32(3)));
    ValueFromBlock outOfLineResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), outOfLineAddress)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, inlineResult, outOfLineResult);
}

TypedPointer Lowering::cachedPropertyAddress(LValue object, LValue word, const AbstractHeap* heap)
{
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
    return Graph::typedBaseField(node->use(node->opcode == op_get_by_id ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base), name);
}

LValue Lowering::layoutOf(LValue cell)
{
    return m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(cell), Structure::offsetOfKnownShape()));
}

LValue Lowering::loadTypedLayoutID(LValue cell)
{
    return m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(cell), Structure::offsetOfTypedLayoutID()));
}

TypedPointer Lowering::fieldAddress(LValue object, const TypeTable::Field& field)
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

LValue Lowering::loadEffectEpoch()
{
    return m_out.load32(m_out.address(m_heaps.root, m_instance, Instance::offsetOfEffectEpoch()));
}

void Lowering::recordAvailableRead(Node* base, UniquedStringImpl* name, LValue value, LValue effectEpoch)
{
    if (m_isOnOnePathOnly)
        return;
    base = skipAliases(base);
    m_availableReads.removeAllMatching([&](auto& read) {
        return read.base == base && read.name == name;
    });
    if (m_availableReads.size() == maxAvailableReads)
        m_availableReads.removeAt(0);
    m_availableReads.append({ base, name, value, effectEpoch });
}

void Lowering::forgetReadsChangedBy(Node* node)
{
    LValue effectEpochBeforeStore = std::exchange(m_effectEpochBeforeStore, nullptr);
    bool keepsReads = std::exchange(m_nodeKeepsReads, false);
    switch (Graph::propertyEffectOf(node)) {
    case PropertyEffect::None:
    case PropertyEffect::OnSlowPathOnly:
        return;
    case PropertyEffect::StoresNamedProperty: {
        auto bytecode = node->as<OpPutById>();
        UniquedStringImpl* name = node->graph->codeBlock()->identifier(bytecode.m_property).impl();
        m_availableReads.removeAllMatching([&](auto& read) {
            return read.name == name;
        });
        if (effectEpochBeforeStore && name != m_graph.vm().propertyNames->length.impl())
            recordAvailableRead(node->use(bytecode.m_base), name, lowJSValue(node->use(bytecode.m_value)), effectEpochBeforeStore);
        return;
    }
    case PropertyEffect::Call:
        if (keepsReads)
            return;
        if (auto* effects = m_graph.propertyEffectsOfCall(node)) {
            m_availableReads.removeAllMatching([&](auto& read) {
                return effects->storedNames.contains(read.name);
            });
            if (!m_availableReads.isEmpty())
                m_graph.remark("call-keeps-property-reads"_s);
            return;
        }
        RELEASE_ASSERT(!m_graph.summary() || m_graph.codeBlock()->isConstructor() || m_graph.summary()->propertyEffects.isArbitrary);
        break;
    case PropertyEffect::Arbitrary:
        break;
    }
    m_availableReads.shrink(0);
}

auto Lowering::variablesFor(const AvailableRead& read) -> ReadVariables
{
    return m_readVariables.ensure({ read.base, read.name }, [&] {
        return ReadVariables { m_proc.addVariable(Int64), m_proc.addVariable(Int32) };
    }).iterator->value;
}

void Lowering::publishAvailableReads(BasicBlock* block)
{
    if (m_availableReads.isEmpty())
        return;
    m_availableReadsAtEndOf.set(block, m_availableReads);
    bool pathsMeetAfterwards = false;
    for (BasicBlock* successor : block->successors)
        pathsMeetAfterwards |= successor->predecessors.size() > 1 && !successor->isGeneric && !successor->isCatchEntrypoint;
    if (!pathsMeetAfterwards)
        return;
    for (auto& read : m_availableReads) {
        ReadVariables variables = variablesFor(read);
        m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), variables.value, read.value);
        m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), variables.effectEpoch, read.effectEpoch);
    }
}

void Lowering::forgetReadsChangedInLoop(BasicBlock* header)
{
    BitVector isInLoop;
    Vector<BasicBlock*, 16> worklist;
    isInLoop.set(header->index);
    for (BasicBlock* predecessor : header->predecessors) {
        if (predecessor->rpoIndex >= header->rpoIndex)
            worklist.append(predecessor);
    }
    Vector<BasicBlock*, 16> body { header };
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        if (isInLoop.set(block->index))
            continue;
        body.append(block);
        worklist.appendVector(block->predecessors);
    }
    for (BasicBlock* block : body) {
        if (block->isGeneric || block->isCatchEntrypoint || block == m_graph.root) {
            m_availableReads.shrink(0);
            return;
        }
        for (Node* node : block->nodes) {
            if (m_availableReads.isEmpty())
                return;
            if (node->isElided)
                continue;
            switch (Graph::propertyEffectOf(node)) {
            case PropertyEffect::None:
            case PropertyEffect::OnSlowPathOnly:
                continue;
            case PropertyEffect::StoresNamedProperty: {
                UniquedStringImpl* name = node->graph->codeBlock()->identifier(node->as<OpPutById>().m_property).impl();
                m_availableReads.removeAllMatching([&](auto& read) {
                    return read.name == name;
                });
                continue;
            }
            case PropertyEffect::Call:
                if (auto* effects = m_graph.propertyEffectsOfCall(node)) {
                    m_availableReads.removeAllMatching([&](auto& read) {
                        return effects->storedNames.contains(read.name);
                    });
                    continue;
                }
                break;
            case PropertyEffect::Arbitrary:
                break;
            }
            m_availableReads.shrink(0);
            return;
        }
    }
}

void Lowering::findReadsAvailableAtHeadOf(BasicBlock* block)
{
    m_availableReads.shrink(0);
    m_effectEpochBeforeStore = nullptr;
    m_nodeKeepsReads = false;
    if (block->isCatchEntrypoint || block->isGeneric || block->isReentry || block == m_graph.root)
        return;
    bool isFirst = true;
    bool allAgree = true;
    for (BasicBlock* predecessor : block->predecessors) {
        if (block->isLoopHeader && predecessor->rpoIndex >= block->rpoIndex) {
            allAgree = false;
            continue;
        }
        auto available = m_availableReadsAtEndOf.find(predecessor);
        if (available == m_availableReadsAtEndOf.end()) {
            m_availableReads.shrink(0);
            return;
        }
        if (std::exchange(isFirst, false)) {
            m_availableReads = available->value;
            continue;
        }
        m_availableReads.removeAllMatching([&](auto& read) {
            size_t index = available->value.findIf([&](auto& other) {
                return other.base == read.base && other.name == read.name;
            });
            if (index == notFound)
                return true;
            allAgree &= available->value[index] == read;
            return false;
        });
    }
    if (isFirst) {
        m_availableReads.shrink(0);
        return;
    }
    if (block->isLoopHeader)
        forgetReadsChangedInLoop(block);
    if (allAgree)
        return;
    for (auto& read : m_availableReads) {
        ReadVariables variables = variablesFor(read);
        read.value = m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), variables.value);
        read.effectEpoch = m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), variables.effectEpoch);
    }
}

auto Lowering::availableField(Node* base, const TypeTable::Field& field) const -> const AvailableField*
{
    base = skipAliases(base);
    for (auto& available : m_availableFields) {
        if (available.base == base && available.layoutID == field.first && available.slot == field.slot && available.id == field.id)
            return &available;
    }
    return nullptr;
}

void Lowering::recordAvailableField(Node* base, const TypeTable::Field& field, LValue value, Rep rep, LValue asJSValue, bool isWritten)
{
    base = skipAliases(base);
    m_availableFields.removeAllMatching([&](auto& available) {
        return available.layoutID == field.first && available.slot == field.slot && (isWritten || available.base == base);
    });
    m_availableFields.append({ base, field.first, field.slot, field.id, rep, value, asJSValue });
}

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
    auto operandsCannotBeObjects = [&] {
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
        return operandsCannotBeObjects();
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

bool Lowering::isEscapingFunctionThis(Node* node)
{
    return Graph::isEscapingFunctionThis(node);
}

void Lowering::checkTypedLayout(Node* originNode, Node* valueNode, LValue value, uint16_t layoutID)
{
    LBasicBlock isNot = newColdBlock();
    LBasicBlock is = m_out.newBlock();
    if (!isSubtype(valueNode->type, TCell)) {
        LBasicBlock cellCase = m_out.newBlock();
        m_out.branch(isCell(value), usually(cellCase), rarely(isNot));
        m_out.appendTo(cellCase);
    }
    if (valueNode->hasLayoutInRangeIfCell(layoutID, layoutID)) {
        m_graph.remark("layout-known-if-cell"_s);
        m_out.jump(is);
    } else
        m_out.branch(m_out.equal(loadTypedLayoutID(value), m_out.constInt32(layoutID)), usually(is), rarely(isNot));
    m_out.appendTo(isNot);
    coldCall(originNode, Entry::operationAOTCheckTypedLayout, value, m_out.constInt32(layoutID));
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

LValue Lowering::coerceToTypedLayout(Node* originNode, Node* valueNode, LValue value, uint16_t layoutID)
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
    ValueFromBlock coercedResult = m_out.anchor(coldCallForValue(originNode, Entry::operationAOTCoerceToTypedLayout, value, m_out.constInt32(layoutID)));
    m_out.jump(done);
    m_out.appendTo(done);
    return m_out.phi(Int64, itself, coercedResult);
}

Lowering::FieldStorage Lowering::fieldStorageFor(Node* originNode, Node* baseNode, LValue base, uint16_t layoutID)
{
    if (baseNode->hasLayoutInRange(layoutID, layoutID))
        return { base, false };
    uint32_t tag = Graph::typeTagOf(originNode);
    if (!TypeTable::shared()->isOpen(layoutID) || (tag && TypeTable::shared()->isTrusted(tag) && TypeTable::shared()->layoutIDOf(tag) == layoutID && !isEscapingFunctionThis(baseNode))) {
        checkTypedLayout(originNode, baseNode, base, layoutID);
        return { base, false };
    }
    if (LValue view = cachedCoercionFor(baseNode, layoutID))
        return { view, true };
    return { coerceToTypedLayout(originNode, baseNode, base, layoutID), true };
}

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
        ValueFromBlock cellResult = m_out.anchor(loadTypedLayoutID(value));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        result = m_out.phi(Int32, none, cellResult);
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

static void remarkBaseOfUntypedAccess(Graph& graph, Node* base)
{
    Node* producer = skipAliases(base);
    graph.remark("untyped-access-from"_s, producer->kind == NodeKind::Bytecode ? ASCIILiteral::fromLiteralUnsafe(opcodeNames[producer->opcode]) : producer->kind == NodeKind::Argument ? "argument"_s : producer->kind == NodeKind::Phi ? "phi"_s : producer->kind == NodeKind::Proj ? "projection"_s : producer->kind == NodeKind::GetStack ? "stack"_s : "constant"_s);
    Type type = base->type & ~(TOther | TEmpty);
    ASCIILiteral what = "anything"_s;
    if (type && isSubtype(type, TFinalObject)) {
        auto layouts = layoutRangeOf(type);
        what = layouts.isOne() && layouts.lowest ? "one-layout"_s : layouts.highest - layouts.lowest < 16 ? "few-layouts"_s : "plain-object"_s;
    } else if (type && isSubtype(type, TAnyObject))
        what = "object"_s;
    else if (type && !mayBe(type, TAnyObject))
        what = "primitive"_s;
    graph.remark("untyped-access-of"_s, what);
}

void Lowering::throwUnlessMadeFromFunction(Node* origin, LValue value, uint32_t function, bool trapsWhenValidating)
{
    unsigned slot = allocateSlot();
    LBasicBlock isOther = newColdBlock();
    LBasicBlock isNot = newColdBlock();
    LBasicBlock is = m_out.newBlock();
    m_out.branch(m_out.equal(value, m_out.load64(slotWord(slot, 1))), usually(is), rarely(isOther));
    m_out.appendTo(isOther);
    m_out.branch(m_out.notZero64(vmCall(origin, Int64, Entry::operationAOTIsMadeFromFunction, m_instance, value, m_out.constInt32(function), slotAddress(slot))), usually(is), rarely(isNot));
    m_out.appendTo(isNot);
    if (trapsWhenValidating && Options::validateAOTInferredTypes())
        trap();
    else {
        vmCall(origin, Void, Entry::operationAOTThrowNotAFunction, m_instance, value);
        m_out.unreachable();
    }
    m_out.appendTo(is);
}

void Lowering::lowerReadOfClosedMethod(Node* read, Node* receiver, uint32_t function)
{
    unsigned identifier = read->as<OpGetById>().m_property;
    UniquedStringImpl* name = read->graph->codeBlock()->identifier(identifier).impl();
    uint16_t layoutID = TypeTable::shared()->instanceLayoutIDFor(TypeTable::shared()->methodClassReadBy(Graph::typeTagOf(read), name));
    LValue base = nullptr;
    LBasicBlock hasLayout = nullptr;
    if (read->isReadOnlyForCall && layoutID && !TypeTable::shared()->usesFieldIDs(layoutID)) {
        if (receiver->hasLayoutInRange(layoutID, layoutID)) {
            m_graph.remark("receiver-of-closed-method-has-its-layout"_s, StringView { name });
            return;
        }
        m_graph.remark("tests-layout-of-receiver-of-closed-method"_s, StringView { name });
        base = lowJSValue(receiver);
        LBasicBlock otherwise = newColdBlock();
        hasLayout = m_out.newBlock();
        if (!isSubtype(receiver->type, TCell)) {
            LBasicBlock cellCase = m_out.newBlock();
            m_out.branch(isCell(base), usually(cellCase), rarely(otherwise));
            m_out.appendTo(cellCase);
        }
        m_out.branch(m_out.equal(loadTypedLayoutID(base), m_out.constInt32(layoutID)), usually(hasLayout), rarely(otherwise));
        m_out.appendTo(otherwise);
    } else {
        m_graph.remark("tests-identity-of-closed-method"_s, StringView { name });
        remarkBaseOfUntypedAccess(m_graph, receiver);
        base = lowJSValue(receiver);
    }
    LValue value = getByIdCached(read, base, receiver->type, Entry::operationAOTGetById, identifier);
    throwUnlessMadeFromFunction(read, value, function, false);
    if (!hasLayout) {
        setJSValue(read, value);
        return;
    }
    m_out.jump(hasLayout);
    m_out.appendTo(hasLayout);
}

void Lowering::lowerGetById(Node* node)
{
    auto bytecode = node->as<OpGetById>();
    Node* baseNode = node->use(bytecode.m_base);
    if (ImmutableIntrinsics::shared() && baseNode->type && !node->guard && !node->guarded && !node->builtinCalled && !node->isElided && !node->isReadOnlyForCall) {
        UniquedStringImpl* name = node->graph->codeBlock()->identifier(bytecode.m_property).impl();
        unsigned method = mayBe(baseNode->type, TEmpty) ? 0 : intrinsicFoundOnPrimitive(baseNode->type & ~TOther, *name);
        bool isOfNumbersOnly = !method && mayBe(baseNode->type, TNumber) && isAbsentFromObjectPrototype(name);
        if (isOfNumbersOnly)
            method = intrinsicFoundOnPrimitive(TNumber, *name);
        if (method && ImmutableIntrinsics::shared()->at(method).type == JSFunctionType) {
            m_graph.remark(isOfNumbersOnly ? "reads-method-of-number-behind-test"_s : "reads-method-of-primitive"_s, StringView { name });
            LValue known = m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[method]);
            if (!isOfNumbersOnly && !mayBe(baseNode->type, TOther)) {
                setJSValue(node, known);
                return;
            }
            LValue base = lowJSValue(baseNode);
            LBasicBlock otherwise = isOfNumbersOnly ? m_out.newBlock() : newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            ValueFromBlock quick = m_out.anchor(known);
            if (isOfNumbersOnly)
                m_out.branch(isNumber(base), unsure(continuation), unsure(otherwise));
            else
                m_out.branch(isUndefinedOrNull(baseNode), rarely(otherwise), usually(continuation));
            m_out.appendTo(otherwise);
            ValueFromBlock found = m_out.anchor(getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            setJSValue(node, m_out.phi(Int64, quick, found));
            return;
        }
    }
    if (uint32_t function = node->guard || node->guarded ? 0 : intrinsicFunctionOf(baseNode->type)) {
        if (unsigned member = intrinsicInheritedByFunction(function, *node->graph->codeBlock()->identifier(bytecode.m_property).impl())) {
            m_graph.remark("reads-member-of-function-prototype"_s, StringView { node->graph->codeBlock()->identifier(bytecode.m_property).impl() });
            LValue known = m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[member]);
            if (isSubtype(baseNode->type, TFunction)) {
                setJSValue(node, known);
                return;
            }
            LValue base = lowJSValue(baseNode);
            LBasicBlock otherwise = newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            ValueFromBlock quick = m_out.anchor(known);
            m_out.branch(m_out.equal(base, m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[function])), usually(continuation), rarely(otherwise));
            m_out.appendTo(otherwise);
            ValueFromBlock found = m_out.anchor(getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            setJSValue(node, m_out.phi(Int64, quick, found));
            return;
        }
    }
    if (node->onlyChecksConstantObject || node->slotInConstantObjectPlusOne) {
        TypeTable::Field field { };
        field.slot = node->slotInConstantObjectPlusOne - 1;
        field.inlineSlots = node->inlineCapacityOfConstantObject;
        StringView name { node->graph->codeBlock()->identifier(bytecode.m_property).impl() };
        if (node->slotInConstantObjectPlusOne)
            m_graph.remark("reads-slot-of-constant-object"_s, name);
        else if (node->replacement->kind == NodeKind::Constant && node->replacement->constant.isUndefined() && node->propertyOfConstantObjectIsAbsent)
            m_graph.remark("absent-property-is-undefined"_s, name);
        else
            m_graph.remark("folds-property-of-constant-object"_s, name);
        if (baseNode->type && !mayBe(baseNode->type, TUndefined)) {
            m_graph.remark("constant-object-is-initialized"_s);
            setJSValue(node, node->slotInConstantObjectPlusOne ? m_out.load64(fieldAddress(lowJSValue(baseNode), field)) : m_out.int64Zero);
            return;
        }
        LValue base = lowJSValue(baseNode);
        LBasicBlock isInitialized = m_out.newBlock();
        LBasicBlock isNotInitialized = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 2> results;
        m_out.branch(node->constantObjectIsNeverAllocated ? m_out.equal(base, m_out.constInt64(JSValue::encode(jsBoolean(true)))) : isCell(base), usually(isInitialized), rarely(isNotInitialized));
        m_out.appendTo(isNotInitialized);
        results.append(m_out.anchor(getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property)));
        m_out.jump(continuation);
        m_out.appendTo(isInitialized);
        if (node->slotInConstantObjectPlusOne)
            results.append(m_out.anchor(m_out.load64(fieldAddress(base, field))));
        else
            results.append(m_out.anchor(m_out.int64Zero));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, results));
        return;
    }
    if (uint32_t function = Graph::closedMethodReadBy(node)) {
        lowerReadOfClosedMethod(node, baseNode, function);
        return;
    }
    if (node->builtinCalled) {
        lowerBuiltinRead(node, baseNode);
        return;
    }
    if (lowerSizeOfMapOrSet(node, baseNode))
        return;
    if (const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared(); intrinsics && !node->isElided && Options::useUnboxedFastArrayIteration() && mayBe(baseNode->type, TArray)
        && code().codeBlock()->identifier(bytecode.m_property) == m_graph.vm().propertyNames->iteratorSymbol) {
        unsigned array = intrinsics->find(ImmutableIntrinsics::globalObject, *String("Array"_s).impl());
        unsigned prototype = array ? intrinsics->find(intrinsics->at(array).canonical, *String("prototype"_s).impl()) : 0;
        if (unsigned values = prototype ? intrinsics->find(intrinsics->at(prototype).canonical, *String("values"_s).impl()) : 0) {
            m_graph.remark("reads-iterator-method-of-array-inline"_s);
            LValue base = lowJSValue(baseNode);
            LBasicBlock isArray = m_out.newBlock();
            LBasicBlock isOtherKind = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(isCellAnd(baseNode, base, [&](LValue cell) { return isOriginalArray(cell); }), unsure(isArray), unsure(isOtherKind));
            m_out.appendTo(isArray);
            ValueFromBlock ofArray = m_out.anchor(m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[intrinsics->at(values).canonical]));
            m_out.jump(continuation);
            m_out.appendTo(isOtherKind);
            ValueFromBlock ofOtherKind = m_out.anchor(getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            setJSValue(node, m_out.phi(Int64, ofArray, ofOtherKind));
            return;
        }
    }
    if (auto field = !Options::auditAOTTypedFields() ? fieldAccessedBy(node, bytecode.m_property) : std::nullopt) {
        m_graph.remark("typed-field-read"_s, code().codeBlock()->identifier(bytecode.m_property).string());
        if (field->mayBeEmpty && !field->isOptional && !field->id && field->first == field->last && !node->graph->codeBlock()->isConstructor() && TypeTable::shared()->isFilledAtBirth(field->first, field->slot)) {
            m_graph.remark("field-is-filled-at-birth"_s, code().codeBlock()->identifier(bytecode.m_property).string());
            field->mayBeEmpty = false;
        }
        LValue base = lowJSValue(baseNode);
        if (Options::useAOTTypedFields() && TypeTable::hasTypedFields() && field->id) {
            bool allowsUndefined = field->isOptional || !field->fieldType.isConstrained() || (field->fieldType.kinds & MaskUndefined);
            Stub stub = static_cast<Stub>(static_cast<unsigned>(allowsUndefined ? Stub::ReadSlotOrUndefined0 : Stub::ReadSlot0) + field->slot);
            auto throughStub = [&]() -> LValue {
                return callStub(stub, Int64, { { base, firstStubOperandGPR } }, { { GPRInfo::argumentGPR1, field->id } }, StubClobbers::CallerSavedRegisters, node);
            };
            const AvailableField* available = availableField(baseNode, *field);
            LValue previouslyReadValue = available ? available->asJSValue : nullptr;
            m_availableFields.shrink(0);
            m_nodePreservesFields = true;
            if (previouslyReadValue) {
                unsigned index = field->slot << 16 | field->id;
                LBasicBlock again = newColdBlock();
                LBasicBlock continuation = m_out.newBlock();
                ValueFromBlock kept = m_out.anchor(previouslyReadValue);
                LValue bits = m_out.load8ZeroExt32(m_out.address(m_heaps.root, m_out.loadPtr(m_out.address(m_heaps.root, m_instance, Instance::offsetOfFieldsWithObservableReads())), index >> 3));
                m_out.branch(m_out.testIsZero32(bits, m_out.constInt32(1 << (index & 7))), usually(continuation), rarely(again));
                m_out.appendTo(again);
                ValueFromBlock rereadResult = m_out.anchor(throughStub());
                m_out.jump(continuation);
                m_out.appendTo(continuation);
                LValue value = m_out.phi(Int64, kept, rereadResult);
                setJSValue(node, value);
                recordAvailableField(baseNode, *field, value, Rep::JSValue, value, false);
                return;
            }
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
            LValue currentFieldID = m_out.load16ZeroExt32(m_out.address(m_heaps.root, structureOf(base), Structure::offsetOfFieldIDInSlot() + field->slot * sizeof(uint16_t)));
            m_out.branch(m_out.equal(currentFieldID, m_out.constInt32(field->id)), usually(isThere), rarely(otherwise));
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
            auto [baseStorage, mayBePlaceholder] = fieldStorageFor(node, baseNode, base, field->first);
            if (!mayBePlaceholder) {
                m_nodePreservesFields = true;
                if (const AvailableField* available = availableField(baseNode, *field)) {
                    if (available->asJSValue && node->rep() == Rep::JSValue)
                        setJSValue(node, available->asJSValue);
                    else {
                        setResult(node, available->value, available->rep);
                        if (available->asJSValue && node->rep() != Rep::JSValue)
                            node->loweredAsJSValue = available->asJSValue;
                    }
                    return;
                }
            }
            LValue valueInSlot = m_out.load64(fieldAddress(baseStorage, *field));
            bool allowsUndefined = field->isOptional || !field->fieldType.isConstrained() || (field->fieldType.kinds & MaskUndefined);
            auto isFilled = [&](LValue inSlot) -> LValue {
                if (allowsUndefined)
                    return m_out.notZero64(inSlot);
                LValue undefined = m_out.constInt64(JSValue::encode(jsUndefined()));
                static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined);
                if (field->fieldType.kinds & (MaskNull | MaskBoolean))
                    return m_out.bitAnd(m_out.notZero64(inSlot), m_out.notEqual(inSlot, undefined));
                return m_out.above(inSlot, undefined);
            };
            if (mayBePlaceholder) {
                RELEASE_ASSERT(field->isInObject());
                LBasicBlock slowCase = newColdBlock();
                LBasicBlock continuation = m_out.newBlock();
                Vector<ValueFromBlock, 3> results;
                results.append(m_out.anchor(valueInSlot));
                if ((field->isOptional || field->mayBeEmpty) && allowsUndefined) {
                    LBasicBlock emptyCase = m_out.newBlock();
                    m_out.branch(m_out.notZero64(valueInSlot), usually(continuation), rarely(emptyCase));
                    m_out.appendTo(emptyCase);
                    results.append(m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined()))));
                    m_out.branch(m_out.equal(baseStorage, base), usually(continuation), rarely(slowCase));
                } else
                    m_out.branch(isFilled(valueInSlot), usually(continuation), rarely(slowCase));
                m_out.appendTo(slowCase);
                uint64_t which =static_cast<uint64_t>(numberOf(bytecode.m_property)) | static_cast<uint64_t>(field->first) << 32 | static_cast<uint64_t>(field->slot) << 48 | static_cast<uint64_t>(allowsUndefined) << 56 | 1ull << 63;
                results.append(m_out.anchor(coldCallForValue(node, Entry::operationAOTGetFieldSlow, base, m_out.constInt64(which))));
                m_out.jump(continuation);
                m_out.appendTo(continuation);
                setJSValue(node, m_out.phi(Int64, results));
                return;
            }
            if (field->isOptional || (field->mayBeEmpty && allowsUndefined))
                valueInSlot = m_out.select(m_out.isZero64(valueInSlot), m_out.constInt64(JSValue::encode(jsUndefined())), valueInSlot);
            else if (field->mayBeEmpty) {
                LBasicBlock isMissing = newColdBlock();
                LBasicBlock isThere = m_out.newBlock();
                m_out.branch(isFilled(valueInSlot), usually(isThere), rarely(isMissing));
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
        bool testsAbsence = field->firstExcludedLayout && (!resultIsTyped || (field->fieldType.kinds & MaskUndefined));
        LBasicBlock has = m_out.newBlock();
        LBasicBlock absentCase = m_out.newBlock();
        LBasicBlock mayLack = m_out.newBlock();
        LBasicBlock otherwise = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        if (baseNode->hasLayoutInRange(field->first, field->last)) {
            m_out.jump(has);
        } else {
            LValue layout = loadTypedLayoutIDOrZero(baseNode, base);
            if (!testsAbsence)
                m_out.branch(isOneOf(layout, field->first, field->last), usually(has), rarely(otherwise));
            else {
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
        m_out.appendTo(mayLack, absentCase);
        if (testsAbsence)
            m_out.branch(isOneOf(layoutOf(base), field->firstExcludedLayout, field->lastExcludedLayout), unsure(absentCase), unsure(otherwise));
        else
            m_out.unreachable();
        m_out.appendTo(absentCase, otherwise);
        ValueFromBlock lacking = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
        m_out.jump(continuation);
        m_out.appendTo(otherwise, continuation);
        LValue slowCaseResult = getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property);
        if (resultIsTyped) {
            LBasicBlock matchCase = m_out.newBlock();
            LBasicBlock undecided = m_out.newBlock();
            emitTypeTests(nullptr, TTop, slowCaseResult, field->fieldType.kinds, matchCase, undecided);
            m_out.appendTo(undecided);
            vmCall(node, Void, Entry::operationAOTCheckType, m_instance, slowCaseResult, m_out.constInt32(field->fieldType.kinds));
            m_out.jump(matchCase);
            m_out.appendTo(matchCase);
        }
        ValueFromBlock other = m_out.anchor(slowCaseResult);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, found, lacking, other));
        return;
    }
    remarkBaseOfUntypedAccess(m_graph, baseNode);
    LValue base = lowJSValue(baseNode);
    UniquedStringImpl* name = code().codeBlock()->identifier(bytecode.m_property).impl();
    LValue effectEpoch = loadEffectEpoch();
    Node* object = skipAliases(baseNode);
    size_t index = m_availableReads.findIf([&](auto& read) {
        return read.base == object && read.name == name;
    });
    if (index == notFound) {
        LValue value = getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property);
        recordAvailableRead(object, name, value, effectEpoch);
        setJSValue(node, value);
        return;
    }
    m_graph.remark("reuses-property-read"_s, String(name));
    AvailableRead read = m_availableReads[index];
    LBasicBlock mayHaveChanged = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock reused = m_out.anchor(read.value);
    m_out.branch(m_out.equal(effectEpoch, read.effectEpoch), usually(continuation), rarely(mayHaveChanged));
    m_out.appendTo(mayHaveChanged);
    ValueFromBlock readAgain = m_out.anchor(getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    LValue value = m_out.phi(Int64, reused, readAgain);
    recordAvailableRead(object, name, value, effectEpoch);
    setJSValue(node, value);
}

LValue Lowering::getByIdCached(Node* node, LValue base, Type baseType, Entry operation, unsigned functionIdentifier)
{
    unsigned identifier = operation == Entry::operationAOTGetByIdWellKnown ? functionIdentifier : numberOf(functionIdentifier);
    std::optional<Stub> stub;
    if (usesDataStubs() && Site::fits(identifier, 0)) {
        if (operation == Entry::operationAOTGetById)
            stub = Stub::GetById;
        else if (operation == Entry::operationAOTGetByIdWellKnown)
            stub = Stub::GetByIdWellKnown;
    }
    unsigned slot = stub ? sharedSite(node, identifier) : allocateSlot();
    if (stub == Stub::GetById)
        m_graph.noteSiteSelector(slot, code().codeBlock()->identifier(functionIdentifier).impl());
    auto throughStub = [&]() -> LValue {
        return callStub(*stub, Int64, { { base, firstStubOperandGPR }, { slotAddress(slot), GPRInfo::argumentGPR1 } }, { });
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
        m_out.branch(m_out.testIsZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isGetter | Slot::isIndirect) << 32)), usually(hit), rarely(slowCase));
        m_out.appendTo(hit, slowCase);
        LValue location = m_out.bitAnd(m_out.lShr(word, m_out.constInt32(32)), m_out.constInt64(Slot::directLocationMask));
        fastResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(base, m_out.shl(location, m_out.constInt32(3))))));
    } else {
        m_out.branch(m_out.testIsZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isGetter) << 32)), usually(hit), rarely(slowCase));

        m_out.appendTo(hit, slowCase);
        LValue holder = m_out.loadPtr(slotWord(slot, 1));
        LValue isOnHolder = m_out.bitAnd(m_out.testNonZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isIndirect) << 32)), m_out.notNull(holder));
        fastResult = m_out.anchor(m_out.load64(cachedPropertyAddress(m_out.select(isOnHolder, holder, base), word, &m_heaps.root)));
    }
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(stub ? throughStub() : vmCall(node, Int64, operation, contextOf(operation), base, m_out.constInt32(identifier), slotAddress(slot)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, fastResult, slowResult);
}

LValue Lowering::getByIdWithThisCached(Node* node, LValue base, LValue thisValue, unsigned identifier)
{
    m_graph.remark("cached-read-with-this"_s, code().codeBlock()->identifier(identifier).string());
    unsigned slot = allocateSlot();
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock rightStructure = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, rightStructure);
    LValue word = m_out.load64(slotWord(slot, 0));
    m_out.branch(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(rightStructure), rarely(slowCase));

    m_out.appendTo(rightStructure, slowCase);
    LValue holder = m_out.loadPtr(slotWord(slot, 1));
    LValue isOnHolder = m_out.bitAnd(m_out.testNonZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isIndirect) << 32)), m_out.notNull(holder));
    ValueFromBlock fastResult = m_out.anchor(m_out.load64(cachedPropertyAddress(m_out.select(isOnHolder, holder, base), word, &m_heaps.root)));
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetByIdWithThis, m_instance, base, thisValue, m_out.constInt32(numberOf(identifier)), slotAddress(slot)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, fastResult, slowResult);
}

void Lowering::findPropertyRuns(BasicBlock* block)
{
    constexpr unsigned minimumLength = 4;
    constexpr unsigned maximumLength = 64;
    if (block->isInLoop || block->isGeneric)
        return;
    struct Base {
        bool operator==(const Base&) const = default;
        explicit operator bool() const { return node || variable; }
        Node* node { nullptr };
        Node* scope { nullptr };
        UniquedStringImpl* variable { nullptr };
    };
    auto isPureScopeRead = [&](Node* node) {
        auto bytecode = node->as<OpGetFromScope>();
        if (node->promotedEnvironment)
            return true;
        if (isFusedWithGetFromScope(node->use(bytecode.m_scope)))
            return false;
        ResolveType type = bytecode.m_getPutInfo.resolveType();
        if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar)
            return true;
        SetForScope code(m_code, node->graph);
        return resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type).kind == StaticVariable::Closure;
    };
    auto readsVariableThatNeverChanges = [&](Node* node) {
        auto bytecode = node->as<OpGetFromScope>();
        if (node->promotedEnvironment)
            return true;
        if (VariableSummaries* summaries = m_graph.variableSummaries()) {
            if (Variable variable = m_graph.variableAccessedBy(node); variable && !node->accessesLocalEnvironment && summaries->isDissolved(variable.scope))
                return true;
        }
        bool isExact = false;
        if (const KnownFunction* known = m_graph.knownFunctionReadBy(node, &isExact); known && isExact && known->isDeclaration)
            return true;
        ResolveType type = bytecode.m_getPutInfo.resolveType();
        if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar) {
            Node* scope = node->use(bytecode.m_scope);
            if (!scope->isBytecode(op_create_lexical_environment))
                return false;
            SymbolTable* symbolTable = uncheckedDowncast<SymbolTable>(scope->graph->codeBlock()->getConstant(scope->as<OpCreateLexicalEnvironment>().m_symbolTable).asCell());
            ConcurrentJSLocker locker(symbolTable->m_lock);
            auto entry = symbolTable->find(locker, node->graph->codeBlock()->identifier(bytecode.m_var).impl());
            return entry != symbolTable->end(locker) && entry->value.isReadOnly() && entry->value.scopeOffset().offset() == bytecode.m_offset;
        }
        SetForScope code(m_code, node->graph);
        StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
        return variable.kind == StaticVariable::Closure && variable.isReadOnly;
    };
    auto baseOf = [&](Node* target) -> Base {
        if (!target->isBytecode(op_get_from_scope) || !isPureScopeRead(target) || !readsVariableThatNeverChanges(target))
            return { target };
        auto bytecode = target->as<OpGetFromScope>();
        Node* scope = target->use(bytecode.m_scope);
        if (scope->isBytecode(op_resolve_scope))
            scope = scope->use(scope->as<OpResolveScope>().m_scope);
        return { nullptr, scope, target->graph->codeBlock()->identifier(bytecode.m_var).impl() };
    };
    auto handlerOf = [&](Node* node) {
        return std::pair { node->graph, node->graph->codeBlock()->handlerForBytecodeIndex(node->bytecodeIndex) };
    };
    Base base;
    PropertyRun stores;
    Vector<UniquedStringImpl*, 16> names;
    UncheckedKeyHashSet<Node*> createdSinceLastEffect;
    auto endRun = [&] {
        if (stores.size() >= minimumLength) {
            for (Node* store : stores)
                m_propertyRunOfStore.add(store, m_propertyRuns.size());
            m_propertyRuns.append(stores);
        }
        stores.shrink(0);
        names.shrink(0);
        base = { };
    };
    auto end = [&] {
        endRun();
        createdSinceLastEffect.clear();
    };
    for (Node* node : block->nodes) {
        if (node->isElided)
            continue;
        if (node->kind == NodeKind::Constant || node->kind == NodeKind::ConstantCell || node->kind == NodeKind::LinkTimeConstant || node->kind == NodeKind::GetStack)
            continue;
        if (node->kind != NodeKind::Bytecode || node->guard || node->guarded) {
            end();
            continue;
        }
        switch (node->opcode) {
        case op_new_func_exp:
        case op_new_async_func_exp:
        case op_new_generator_func_exp:
        case op_new_async_generator_func_exp:
        case op_new_object:
            createdSinceLastEffect.add(node);
            continue;
        case op_get_scope:
            continue;
        case op_resolve_scope: {
            auto bytecode = node->as<OpResolveScope>();
            ResolveType type = bytecode.m_resolveType;
            if (type == Dynamic || type == UnresolvedProperty || type == UnresolvedPropertyWithVarInjectionChecks) {
                end();
                continue;
            }
            if (isStaticClosureVarResolveType(type))
                continue;
            SetForScope code(m_code, node->graph);
            if (StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type); !variable.isAtStaticDepth() && variable.kind != StaticVariable::Import)
                endRun();
            continue;
        }
        case op_get_from_scope:
            if (!isPureScopeRead(node))
                end();
            else if (!readsVariableThatNeverChanges(node)) {
                if (!stores.isEmpty())
                    m_graph.remark("variable-read-ends-property-run"_s);
                endRun();
            }
            continue;
        case op_check_tdz:
            if (!base || baseOf(node->use(node->as<OpCheckTdz>().m_targetVirtualRegister)) != base)
                end();
            continue;
        case op_put_by_id: {
            auto bytecode = node->as<OpPutById>();
            Node* target = node->use(bytecode.m_base);
            UniquedStringImpl* name = node->graph->codeBlock()->identifier(bytecode.m_property).impl();
            if (createdSinceLastEffect.contains(target) && (bytecode.m_flags.isDirect() || (name->isSymbol() && static_cast<SymbolImpl*>(name)->isPrivate())))
                continue;
            if (Graph::typeTagOf(node) || name->isSymbol() || WTF::equal(name, "__proto__"_s)) {
                end();
                continue;
            }
            if (Base targetBase = baseOf(target); targetBase != base || names.contains(name) || stores.size() == maximumLength || handlerOf(node) != handlerOf(stores[0])) {
                if (targetBase != base)
                    end();
                else
                    endRun();
                base = targetBase;
            }
            stores.append(node);
            names.append(name);
            continue;
        }
        default:
            end();
            continue;
        }
    }
    end();
}

void Lowering::lowerPropertyRun(const PropertyRun& stores)
{
    Node* first = stores[0];
    unsigned count = stores.size();
    unsigned slot = allocateSlots(2);
    LValue base = lowJSValue(first->use(first->as<OpPutById>().m_base));
    LBasicBlock together = m_out.newBlock();
    LBasicBlock separately = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.notZero64(m_out.load64(slotWord(slot + 1, 1))), unsure(separately), unsure(together));
    m_out.appendTo(separately);
    for (Node* store : stores) {
        SetForScope currentNode(m_node, store);
        SetForScope currentCode(m_code, store->graph);
        lowerPutById(store);
        forgetReadsChangedBy(store);
    }
    m_out.jump(continuation);
    m_out.appendTo(together);
    Vector<uint32_t, 16> words { AllocationPlan::encode(0, count) };
    for (unsigned i = 0; i < count; ++i) {
        auto bytecode = stores[i]->as<OpPutById>();
        words.append(AllocationPlan::encode(numberOf(*stores[i]->graph, bytecode.m_property), bytecode.m_flags.isDirect(), bytecode.m_flags.ecmaMode().isStrict(), true));
        m_out.store64(lowJSValue(stores[i]->use(bytecode.m_value)), scratchWord(i));
        if (i)
            m_graph.spreadSites.append({ siteOf(first), i, callSiteBitsOf(stores[i]) });
    }
    m_graph.noteSitePlan(slot, WTF::move(words));
    m_graph.remark("property-run"_s, String::number(count));
    vmCall(first, Void, Entry::operationAOTPutProperties, m_instance, base, scratchAddress(), m_out.constInt32(count), slotAddress(slot));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::lowerPutById(Node* node)
{
    auto bytecode = node->as<OpPutById>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* valueNode = node->use(bytecode.m_value);
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);
    m_effectEpochBeforeStore = loadEffectEpoch();
    uint32_t flags = (bytecode.m_flags.isDirect() ? 1 : 0) | (bytecode.m_flags.ecmaMode().isStrict() ? 2 : 0);
    LBasicBlock afterTypedStore = nullptr;
    if (auto field = !Options::auditAOTTypedFields() ? fieldAccessedBy(node, bytecode.m_property) : std::nullopt; field && !field->id) {
        m_graph.remark("typed-field-write"_s, code().codeBlock()->identifier(bytecode.m_property).string());
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock has = m_out.newBlock();
        LBasicBlock otherwise = Options::useAOTTypedFields() && TypeTable::hasTypedFields() ? newColdBlock() : m_out.newBlock();
        afterTypedStore = m_out.newBlock();
        if (Options::useAOTTypedFields() && TypeTable::hasTypedFields()) {
            auto [baseStorage, mayBePlaceholder] = fieldStorageFor(node, baseNode, base, field->first);
            bool mayBeRejected = branchUnlessAccepted(valueNode, value, field->fieldType, otherwise);
            TypedPointer fieldSlot = fieldAddress(baseStorage, *field);
            LValue storedValue = toFieldRepresentation(valueNode, value, field->fieldType);
            if (!mayBePlaceholder && !mayBeRejected && bytecode.m_flags.ecmaMode().isStrict()) {
                m_nodePreservesFields = true;
                if (valueNode->type && isSubtype(valueNode->type, TNumber))
                    recordAvailableField(baseNode, *field, lowDouble(valueNode), Rep::Double, storedValue, true);
                else
                    recordAvailableField(baseNode, *field, storedValue, Rep::JSValue, storedValue, true);
            }
            if (field->isOptional || field->mayBeEmpty || mayBePlaceholder) {
                RELEASE_ASSERT(!mayBePlaceholder || field->isInObject());
                LBasicBlock isThere = m_out.newBlock();
                LBasicBlock isEmpty = (field->isOptional || field->mayBeEmpty) && field->isInObject() ? m_out.newBlock() : nullptr;
                m_out.branch(m_out.notZero64(m_out.load64(fieldSlot)), unsure(isThere), isEmpty ? unsure(isEmpty) : rarely(otherwise));
                if (isEmpty) {
                    m_out.appendTo(isEmpty);
                    if (mayBePlaceholder)
                        orElse(m_out.equal(baseStorage, base), otherwise);
                    if (isCompact())
                        orElse(m_out.notNull(callHelper(Stub::HelperAddField, { base, storedValue, m_out.constInt32(field->slot) })), otherwise);
                    else
                        addTypedField(base, storedValue, m_out.constInt32(field->slot), otherwise);
                    storeBarrier(base);
                    m_out.jump(afterTypedStore);
                }
                m_out.appendTo(isThere);
            }
            orElse(m_out.testIsZero32(m_out.load32(structureOf(baseStorage), m_heaps.Structure_bitField), m_out.constInt32(Structure::s_hasReadOnlyOrGetterSetterPropertiesExcludingProtoBits)), otherwise);
            m_out.store64(storedValue, fieldSlot);
            if (mayBe(field->fieldType.typeOfStored(valueNode->type), TCell))
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
        if (Options::useAOTTypedFields())
            branchUnlessAccepted(valueNode, value, field->fieldType.kindsOnly(), otherwise);
        m_out.store64(value, m_out.address(m_heaps.properties.atAnyNumber(), base, JSObject::offsetOfInlineStorage() + field->slot * sizeof(EncodedJSValue)));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        m_out.jump(afterTypedStore);
        m_out.appendTo(otherwise, afterTypedStore);
        }
    } else if (field && Options::useAOTTypedFields() && TypeTable::hasTypedFields() && field->isInObject() && (!isCompact() || (m_block->isInLoop && !m_block->isGeneric))) {
        m_graph.remark("typed-field-write"_s, code().codeBlock()->identifier(bytecode.m_property).string());
        LBasicBlock otherwise = newColdBlock();
        afterTypedStore = m_out.newBlock();
        if (!isSubtype(baseNode->type, TCell)) {
            LBasicBlock cellCase = m_out.newBlock();
            m_out.branch(isCell(base), usually(cellCase), rarely(otherwise));
            m_out.appendTo(cellCase);
        }
        LValue structure = structureOf(base);
        orElse(m_out.equal(m_out.load16ZeroExt32(m_out.address(m_heaps.root, structure, Structure::offsetOfFieldIDInSlot() + field->slot * sizeof(uint16_t))), m_out.constInt32(field->id)), otherwise);
        branchUnlessAccepted(valueNode, value, field->fieldType, otherwise);
        LValue storedValue = toFieldRepresentation(valueNode, value, field->fieldType);
        orElse(m_out.testIsZero32(m_out.load32(structure, m_heaps.Structure_bitField), m_out.constInt32(Structure::s_hasReadOnlyOrGetterSetterPropertiesExcludingProtoBits)), otherwise);
        m_out.store64(storedValue, m_out.address(m_heaps.properties.atAnyNumber(), base, JSObject::offsetOfInlineStorage() + field->slot * sizeof(EncodedJSValue)));
        if (mayBe(field->fieldType.typeOfStored(valueNode->type), TCell))
            storeBarrier(base);
        m_out.jump(afterTypedStore);
        m_out.appendTo(otherwise, afterTypedStore);
    }
    if (!afterTypedStore)
        remarkBaseOfUntypedAccess(m_graph, baseNode);
    if (!afterTypedStore && !bytecode.m_flags.isDirect() && mayBe(baseNode->type, TArray) && mayBe(valueNode->type, TInt32) && code().codeBlock()->identifier(bytecode.m_property).impl() == m_graph.vm().propertyNames->length.impl()) {
        LBasicBlock otherwise = m_out.newBlock();
        afterTypedStore = m_out.newBlock();
        if (isCompact())
            orElse(m_out.notNull(callHelper(Stub::HelperSetArrayLength, { base, value })), otherwise);
        else
            setArrayLength(base, value, otherwise);
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
        m_graph.noteSiteSelector(slot, code().codeBlock()->identifier(bytecode.m_property).impl());
        callStub(Stub::PutById, Void, { { base, firstStubOperandGPR }, { value, GPRInfo::argumentGPR1 }, { slotAddress(slot), GPRInfo::argumentGPR2 } }, { });
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
        LBasicBlock isPlain = m_out.newBlock();
        m_out.branch(m_out.isZero64(m_out.lShr(secondWord, m_out.constInt32(32))), usually(isPlain), rarely(slowCase));
        m_out.appendTo(isPlain);
    }
    m_out.store64(value, cachedPropertyAddress(base, word));
    LValue newStructureID = lowHalf(m_out, secondWord);
    m_out.branch(m_out.notZero32(newStructureID), unsure(transition), unsure(stored));

    m_out.appendTo(transition, stored);
    m_out.store32(newStructureID, base, m_heaps.JSCell_structureID);
    if (!mayBe(valueNode->type, TCell))
        storeBarrier(base);
    m_out.jump(stored);

    m_out.appendTo(stored, slowCase);
    if (mayBe(valueNode->type, TCell))
        storeBarrier(base);
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    vmCall(node, Void, Entry::operationAOTPutById, m_instance, base, value, m_out.constInt32(numberOf(bytecode.m_property)), slotAddress(slot), m_out.constInt32(flags));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

LValue Lowering::lowIndex(Node* propertyNode, LBasicBlock indexReady, LBasicBlock notIndex)
{
    if (propertyNode->rep() == Rep::Int32) {
        LValue index = lowInt32(propertyNode);
        m_out.jump(indexReady);
        return index;
    }
    if (propertyNode->rep() == Rep::Int64) {
        LValue wide = lowRaw(propertyNode);
        LValue index = m_out.castToInt32(wide);
        m_out.branch(m_out.belowOrEqual(wide, m_out.constInt64(INT32_MAX)), usually(indexReady), rarely(notIndex));
        return index;
    }
    if (propertyNode->rep() == Rep::Double) {
        LValue asDouble = lowDouble(propertyNode);
        LValue index = m_out.doubleToInt32(asDouble);
        m_out.branch(m_out.doubleEqual(m_out.intToDouble(index), asDouble), usually(indexReady), rarely(notIndex));
        return index;
    }
    LValue property = lowJSValue(propertyNode);
    LValue index = unboxInt32(property);
    m_out.branch(isInt32(property), usually(indexReady), unsure(notIndex));
    return index;
}

auto Lowering::constantKeyOf(Node* property) -> std::optional<ConstantKey>
{
    auto* numbers = programIdentifierIndices();
    if (!numbers || !usesDataStubs())
        return std::nullopt;
    if (property->isBytecode(op_to_property_key))
        property = property->use(property->as<OpToPropertyKey>().m_src);
    else if (property->isBytecode(op_to_property_key_or_number))
        property = property->use(property->as<OpToPropertyKeyOrNumber>().m_src);
    auto key = constantStringOf(property);
    if (!key || !key->impl()->isAtom())
        return std::nullopt;
    auto* name = static_cast<UniquedStringImpl*>(key->impl());
    if (parseIndex(*name) || isCanonicalNumericIndexString(name))
        return std::nullopt;
    unsigned identifier = numbers->get(name);
    if (!identifier)
        return std::nullopt;
    return ConstantKey { identifier, name };
}

static std::optional<JSType> typedArrayToAccessInline(const BasicBlock* block, Node* base)
{
    if (block->isGeneric || block->isRarelyExecuted)
        return std::nullopt;
    auto type = typedArrayTypeOf(base->type & ~TOther);
    if (!type || *type == Uint8ClampedArrayType || *type == Float16ArrayType || *type == BigInt64ArrayType || *type == BigUint64ArrayType)
        return std::nullopt;
    return type;
}

TypedPointer Lowering::typedArrayElement(Node* baseNode, Node* propertyNode, JSType type, LBasicBlock outOfBounds, LBasicBlock slowCase)
{
    LValue base = lowJSValue(baseNode);
    bool isKnownToBeCell = isSubtype(baseNode->type, TCell);
    LBasicBlock isNotOfType = Options::validateAOTInferredTypes() ? newColdBlock() : nullptr;
    if (!isKnownToBeCell || isNotOfType) {
        LBasicBlock cellCase = m_out.newBlock();
        m_out.branch(isCell(base), usually(cellCase), rarely(isKnownToBeCell ? isNotOfType : slowCase));
        m_out.appendTo(cellCase);
    }
    if (isNotOfType) [[unlikely]] {
        LBasicBlock isOfType = m_out.newBlock();
        m_out.branch(m_out.equal(cellType(base), m_out.constInt32(type)), usually(isOfType), rarely(isNotOfType));
        m_out.appendTo(isNotOfType);
        trap();
        m_out.appendTo(isOfType);
    }
    LValue index;
    if (propertyNode->isInteger())
        index = lowInt64(propertyNode);
    else {
        LBasicBlock indexReady = m_out.newBlock();
        LValue narrow = lowIndex(propertyNode, indexReady, slowCase);
        m_out.appendTo(indexReady);
        index = m_out.signExt32To64(narrow);
    }
    LBasicBlock hasLengthOfItsOwn = m_out.newBlock();
    LBasicBlock inBounds = m_out.newBlock();
    m_out.branch(m_out.testIsZero32(m_out.load8ZeroExt32(base, m_heaps.JSArrayBufferView_mode), m_out.constInt32(isResizableOrGrowableSharedMode)), usually(hasLengthOfItsOwn), rarely(slowCase));
    m_out.appendTo(hasLengthOfItsOwn);
    m_out.branch(m_out.below(index, m_out.loadPtr(base, m_heaps.JSArrayBufferView_length)), usually(inBounds), rarely(outOfBounds));
    m_out.appendTo(inBounds);
    return TypedPointer(m_heaps.TypedArrayProperties, m_out.add(m_out.loadPtr(base, m_heaps.JSArrayBufferView_vector), m_out.shl(index, m_out.constInt32(logElementSize(type)))));
}

void Lowering::lowerGetByVal(Node* node)
{
    auto bytecode = node->as<OpGetByVal>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* propertyNode = node->use(bytecode.m_property);
    LValue base = lowJSValue(baseNode);
    bool allowsEmpty = node->graph->readsElementsOrEmpty;

    if (auto key = allowsEmpty ? std::nullopt : constantKeyOf(propertyNode); key && Site::fits(key->identifier, 0)) {
        m_graph.remark("get-by-val-with-constant-key"_s);
        unsigned slot = sharedSite(node, key->identifier);
        m_graph.noteSiteSelector(slot, key->name);
        setJSValue(node, callStub(Stub::GetById, Int64, { { base, firstStubOperandGPR }, { slotAddress(slot), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    if (auto type = typedArrayToAccessInline(m_block, baseNode); type && !allowsEmpty && mayBe(propertyNode->type, TNumber)) {
        m_graph.remark("get-by-val-on-typed-array"_s);
        LBasicBlock outOfBounds = newColdBlock();
        LBasicBlock slowCase = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue element = loadTypedArrayElement(*type, typedArrayElement(baseNode, propertyNode, *type, outOfBounds, slowCase));
        Rep rep = element->type() == Double ? Rep::Double : element->type() == Int64 ? Rep::Int64 : Rep::Int32;
        ValueFromBlock fast = m_out.anchor(convert(element, rep, TNumber, Rep::JSValue));
        m_out.jump(continuation);
        m_out.appendTo(outOfBounds);
        ValueFromBlock absent = m_out.anchor(m_out.constInt64(JSValue::encode(jsUndefined())));
        m_out.jump(continuation);
        m_out.appendTo(slowCase);
        LValue slowResult = coldCallForValue(node, Entry::operationAOTGetByVal, base, lowJSValue(propertyNode));
        reloadArrayViews();
        ValueFromBlock slow = m_out.anchor(slowResult);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fast, absent, slow));
        return;
    }

    if (auto* view = viewOf(node, baseNode)) {
        LBasicBlock inBounds = m_out.newBlock();
        LBasicBlock slowCase = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue index = lowInt64(propertyNode);
        m_out.branch(m_out.below(index, view->limit), usually(inBounds), rarely(slowCase));
        m_out.appendTo(inBounds);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, view->butterfly, index));
        ValueFromBlock fast = m_out.anchor(element);
        std::optional<ValueFromBlock> absent;
        if (allowsEmpty) {
            LBasicBlock hole = newColdBlock();
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(hole));
            m_out.appendTo(hole);
            absent = m_out.anchor(m_out.int64Zero);
            m_out.branch(m_out.bitAnd(isOriginalArray(base), m_out.notZero32(changing32(Instance::offsetOfArraysLackInheritedElements()))), usually(continuation), rarely(slowCase));
        } else
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(slowCase));
        m_out.appendTo(slowCase);
        LValue slowResult = coldCallForValue(node, allowsEmpty ? Entry::operationAOTGetElementOrEmpty : Entry::operationAOTGetByVal, base, lowJSValue(propertyNode));
        reloadArrayViews();
        ValueFromBlock slow = m_out.anchor(slowResult);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, absent ? m_out.phi(Int64, fast, slow, *absent) : m_out.phi(Int64, fast, slow));
        return;
    }

    bool isIntegerIndexedInLoop = propertyNode->isInteger() && m_block->isInLoop && !m_block->isGeneric;
    if (isCompact() && !allowsEmpty && !isIntegerIndexedInLoop) {
        if (propertyNode->rep() == Rep::Int64)
            setJSValue(node, callBinaryStub(node, Stub::GetByValAtIndex, Int64, base, lowRaw(propertyNode)));
        else
            setJSValue(node, callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode)));
        reloadArrayViews();
        return;
    }

    bool baseIsArray = isSubtype(baseNode->type, TArray);
    bool missingMeansAbsent = allowsEmpty && baseIsArray;
    if (allowsEmpty && !baseIsArray && Options::verboseAOTCompilation()) [[unlikely]] {
        dataLog("AOT: LEAN an element is read from what is not known for an array: ");
        baseNode->dump(WTF::dataFile());
        dataLogLn(m_block->isGeneric ? " (in the second copy of a loop)" : "", m_block->isInLoop ? " (in a loop)" : "");
    }
    LBasicBlock slowCase = baseIsArray ? newColdBlock() : m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    if (mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock indexReady = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, indexReady, slowCase);

        m_out.appendTo(indexReady, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        LValue isJSValueShape = m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape)));
        m_out.branch(isJSValueShape, usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, inBounds);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), rarely(slowCase));

        m_out.appendTo(inBounds, slowCase);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        results.append(m_out.anchor(element));
        if (missingMeansAbsent) {
            LBasicBlock hole = newColdBlock();
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(hole));
            m_out.appendTo(hole);
            results.append(m_out.anchor(m_out.int64Zero));
            m_out.branch(m_out.bitAnd(isOriginalArray(base), m_out.notZero32(changing32(Instance::offsetOfArraysLackInheritedElements()))), usually(continuation), rarely(slowCase));
        } else
            m_out.branch(m_out.notZero64(element), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    LValue slowResult;
    if (allowsEmpty && !baseIsArray)
        slowResult = vmCall(node, Int64, Entry::operationAOTGetElementOrEmpty, m_instance, base, lowJSValue(propertyNode));
    else if (baseIsArray)
        slowResult = coldCallForValue(node, allowsEmpty ? Entry::operationAOTGetElementOrEmpty : Entry::operationAOTGetByVal, base, lowJSValue(propertyNode));
    else if (usesDataStubs())
        slowResult = callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode));
    else
        slowResult = vmCall(node, Int64, Entry::operationAOTGetByVal, m_instance, base, lowJSValue(propertyNode));
    reloadArrayViews();
    results.append(m_out.anchor(slowResult));
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

    if (auto type = typedArrayToAccessInline(m_block, baseNode); type && mayBe(propertyNode->type, TNumber) && mayBe(valueNode->type, TNumber)) {
        m_graph.remark("put-by-val-on-typed-array"_s);
        LBasicBlock slowCase = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        bool isFloat = *type == Float32ArrayType || *type == Float64ArrayType;
        LValue number;
        if (isSubtype(valueNode->type, TNumber))
            number = isFloat ? lowDouble(valueNode) : toInt32ForBitOp(valueNode);
        else {
            LValue boxed = lowJSValue(valueNode);
            LBasicBlock numberCase = m_out.newBlock();
            m_out.branch(isNumber(boxed), usually(numberCase), rarely(slowCase));
            m_out.appendTo(numberCase);
            if (isFloat)
                number = numberToDouble(boxed);
            else {
                LBasicBlock doubleCase = m_out.newBlock();
                LBasicBlock converted = m_out.newBlock();
                ValueFromBlock fromInt32 = m_out.anchor(unboxInt32(boxed));
                m_out.branch(isInt32(boxed), usually(converted), unsure(doubleCase));
                m_out.appendTo(doubleCase);
                ValueFromBlock fromDouble = m_out.anchor(doubleToInt32(unboxDouble(boxed)));
                m_out.jump(converted);
                m_out.appendTo(converted);
                number = m_out.phi(Int32, fromInt32, fromDouble);
            }
        }
        storeTypedArrayElement(*type, number, typedArrayElement(baseNode, propertyNode, *type, continuation, slowCase));
        m_out.jump(continuation);
        m_out.appendTo(slowCase);
        vmCall(node, Void, Entry::operationAOTPutByVal, m_instance, lowJSValue(baseNode), lowJSValue(propertyNode), lowJSValue(valueNode), m_out.constInt32(bytecode.m_ecmaMode.isStrict()));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }

    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);

    auto throughStub = [&] {
        bool isInteger = propertyNode->rep() == Rep::Int64;
        callStub(isInteger ? Stub::PutByValAtIndex : Stub::PutByVal, Void, { { base, firstStubOperandGPR }, { isInteger ? lowRaw(propertyNode) : lowJSValuePreferringInt32(propertyNode), GPRInfo::argumentGPR1 }, { value, GPRInfo::argumentGPR2 } },
            { { GPRInfo::argumentGPR3, bytecode.m_ecmaMode.isStrict() } });
    };
    if (isCompact()) {
        uint32_t flags = bytecode.m_ecmaMode.isStrict() ? 2 : 0;
        if (auto key = constantKeyOf(propertyNode); key && Site::fits(key->identifier, flags)) {
            m_graph.remark("put-by-val-with-constant-key"_s);
            unsigned slot = sharedSite(node, key->identifier, flags);
            m_graph.noteSiteSelector(slot, key->name);
            callStub(Stub::PutById, Void, { { base, firstStubOperandGPR }, { value, GPRInfo::argumentGPR1 }, { slotAddress(slot), GPRInfo::argumentGPR2 } }, { });
            return;
        }
        throughStub();
        return;
    }

    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock indexReady = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock beyondLength = m_out.newBlock();
        LBasicBlock lengthen = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, indexReady, slowCase);

        m_out.appendTo(indexReady, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        LValue indexingMode = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask | CopyOnWrite));
        m_out.branch(m_out.equal(indexingMode, m_out.constInt32(ContiguousShape)), usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, beyondLength);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), unsure(beyondLength));

        m_out.appendTo(beyondLength, lengthen);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_vectorLength)), unsure(lengthen), unsure(slowCase));

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
    if (usesDataStubs())
        throughStub();
    else
        vmCall(node, Void, Entry::operationAOTPutByVal, m_instance, base, lowJSValue(propertyNode), value, m_out.constInt32(bytecode.m_ecmaMode.isStrict()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

B3::Variable* Lowering::environmentVariable(Node* environment, unsigned offset)
{
    auto& variables = m_environmentVariables.ensure(environment, [&] {
        JSValue table = environment->graph->codeBlock()->getConstant(environment->as<OpCreateLexicalEnvironment>().m_symbolTable);
        Vector<B3::Variable*> result;
        for (unsigned i = uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize(); i--;)
            result.append(m_proc.addVariable(Int64));
        return result;
    }).iterator->value;
    RELEASE_ASSERT(offset < variables.size());
    return variables[offset];
}

LValue Lowering::readPromotedVariable(Node* environment, unsigned offset)
{
    return m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Get, m_out.origin(), environmentVariable(environment, offset));
}

void Lowering::writePromotedVariable(Node* environment, unsigned offset, LValue value)
{
    m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), environmentVariable(environment, offset), value);
}

static Node* environmentMadeBy(Graph& maker, const void* scope)
{
    for (BasicBlock* block : maker.outermost().m_rpo) {
        for (Node* node : block->nodes) {
            if (node->graph == &maker && node->isBytecode(op_create_lexical_environment) && maker.scopeIdentity(node) == scope)
                return node;
        }
    }
    return nullptr;
}

LValue Lowering::heldCapture(Graph& graph, const void* scope, unsigned offset)
{
    Graph* holder = &graph;
    while (!holder->closureFunction && holder->closureScope) {
        holder = holder->closureScope->graph;
        if (Node* environment = environmentMadeBy(*holder, scope)) {
            m_graph.remark("reads-variable-of-maker"_s);
            if (environment->isPromoted)
                return readPromotedVariable(environment, offset);
            return m_out.load64(lowCell(environment), m_heaps.JSLexicalEnvironment_variables[offset]);
        }
    }
    const FunctionSummary* summary = holder->summaryWithCaptures();
    auto index = summary ? summary->indexOfCapture(scope, offset) : std::nullopt;
    RELEASE_ASSERT_WITH_MESSAGE(index, "A variable of a scope that is in no chain is read by a function that does not hold it");
    LValue function = holder->closureFunction ? lowCell(holder->currentClosureFunction()) : callee();
    return m_out.load64(m_out.address(m_heaps.properties.atAnyNumber(), function, JSFunctionWithCaptures::offsetOfCaptures() + *index * sizeof(EncodedJSValue)));
}

static unsigned dissolvedScopesWithin(Node* scope, unsigned hops)
{
    while (scope->isBytecode(op_get_scope) && scope->graph->closureScope)
        scope = scope->graph->closureScope;
    if (scope->isBytecode(op_get_scope))
        return scope->graph->dissolvedScopesOutside(hops);
    return scope->graph->dissolvedScopesAbove(scope, hops);
}

LValue Lowering::ancestorScope(Node* scope, unsigned hops)
{
    return scopeAbove(scope, hops - dissolvedScopesWithin(scope, hops));
}

LValue Lowering::scopeAbove(Node* scope, unsigned hops)
{
    LValue current = lowCell(scope);
    for (unsigned i = 0; i < hops; ++i)
        current = m_out.loadPtr(current, m_heaps.JSScope_next);
    return current;
}

LValue Lowering::scopeToResolveFrom(Node* resolve)
{
    if (resolve->skippedEnvironments)
        return ancestorScope(resolve->scopeToStartFrom, resolve->remainingHops);
    return lowCell(resolve->use(resolve->as<OpResolveScope>().m_scope));
}

void Lowering::lowerResolveScope(Node* node)
{
    auto bytecode = node->as<OpResolveScope>();
    if (auto distance = m_graph.resolvedEnvironmentDepth(node)) {
        setJSValue(node, environmentAt(*distance));
        return;
    }
    if (node->scopeToStartFrom && !node->skippedEnvironments) {
        unsigned depth = bytecode.m_localScopeDepth;
        if (isStaticClosureVarResolveType(bytecode.m_resolveType))
            depth += staticClosureVarHops(bytecode.m_resolveType);
        else if (bytecode.m_resolveType != Dynamic) {
            if (StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType); variable.isAtStaticDepth())
                depth = variable.depth;
        }
        if (node->scopeToStartFrom->isBytecode(op_get_scope) || depth <= bytecode.m_localScopeDepth) {
            setJSValue(node, ancestorScope(node->scopeToStartFrom, node->remainingHops));
            return;
        }
        unsigned local = bytecode.m_localScopeDepth;
        unsigned alreadyOutside = depth > node->remainingHops + local ? depth - node->remainingHops - local : 0;
        unsigned dissolved = code().dissolvedScopesOutside(depth - local) - code().dissolvedScopesOutside(alreadyOutside);
        setJSValue(node, scopeAbove(node->scopeToStartFrom, node->remainingHops - dissolved));
        return;
    }
    if (VariableSummaries* summaries = m_graph.variableSummaries(); summaries && summaries->isDissolved(m_graph.scopeIdentity(node))) {
        setJSValue(node, lowCell(node->use(bytecode.m_scope)));
        return;
    }
    LValue scope = scopeToResolveFrom(node);

    auto walk = [&](unsigned depth) {
        RELEASE_ASSERT(depth >= node->skippedEnvironments);
        if (depth > bytecode.m_localScopeDepth)
            depth -= code().dissolvedScopesOutside(depth - bytecode.m_localScopeDepth);
        LValue current = scope;
        for (unsigned i = node->skippedEnvironments; i < depth; ++i)
            current = m_out.loadPtr(current, m_heaps.JSScope_next);
        return current;
    };

    if (isStaticClosureVarResolveType(bytecode.m_resolveType)) {
        setJSValue(node, walk(bytecode.m_localScopeDepth + staticClosureVarHops(bytecode.m_resolveType)));
        return;
    }

    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
    if (variable.isGlobal && !node->useCount)
        return;
    if (variable.isAtStaticDepth()) {
        setJSValue(node, walk(variable.depth));
        return;
    }
    if (variable.kind == StaticVariable::Import) {
        LValue importer = walk(variable.depth);
        LValue exporter = m_out.load64(importer, m_heaps.JSLexicalEnvironment_variables[variable.import.slotScopeOffset]);
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(exporter);
        m_out.branch(m_out.isZero64(exporter), rarely(slowCase), usually(continuation));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTFillImportSlot, m_instance, importer, m_out.constInt32(variable.import.slot)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
        return;
    }

    unsigned extra = code().resolveScopeExtra(bytecode);
    if (isFusedWithGetFromScope(node))
        return;
    if (usesDataStubs() && variable.kind == StaticVariable::Unresolved && Site::fits(numberOf(bytecode.m_var), extra)) {
        setJSValue(node, callStub(Stub::ResolveScope, Int64, { { scope, firstStubOperandGPR }, { slotAddress(sharedSite(node, numberOf(bytecode.m_var), extra)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;
    if (variable.kind == StaticVariable::Unresolved) {
        LValue cached = m_out.loadPtr(slotWord(slot, 1));
        LValue epochPlusOne = highHalf(m_out, m_out.load64(slotWord(slot, 0)));
        LValue epoch = m_out.load32(m_out.address(m_heaps.root, globalObjectHere(), JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()));
        results.append(m_out.anchor(cached));
        m_out.branch(m_out.equal(epochPlusOne, m_out.add(epoch, m_out.constInt32(1))), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTResolveScope, m_instance, scope, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(slot), m_out.constInt32(variable.isInGlobalScopes ? Site::resolvesInGlobalScopes : 0))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

bool Lowering::isFusedWithGetFromScope(Node* node)
{
    if (!usesDataStubs() || !node->isBytecode(op_resolve_scope) || node->block != m_block || node->useCount != 1)
        return false;
    auto resolve = node->as<OpResolveScope>();
    if (isStaticClosureVarResolveType(resolve.m_resolveType) || !Site::fits(numberOf(resolve.m_var), code().resolveScopeExtra(resolve)))
        return false;
    if (resolveStatically(resolve.m_var, resolve.m_localScopeDepth, resolve.m_resolveType).kind != StaticVariable::Unresolved)
        return false;

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
        setJSValue(node, readPromotedVariable(node->promotedEnvironment, node->offsetInEnvironment));
        return;
    }

    if (VariableSummaries* summaries = m_graph.variableSummaries()) {
        if (Variable variable = m_graph.variableAccessedBy(node); variable && !node->accessesLocalEnvironment && summaries->isDissolved(variable.scope)) {
            m_graph.remark("reads-capture"_s, StringView(code().codeBlock()->identifier(bytecode.m_var).impl()));
            setJSValue(node, heldCapture(code(), variable.scope, variable.offset));
            return;
        }
    }
    if (Node* resolveNode = node->use(bytecode.m_scope); isFusedWithGetFromScope(resolveNode)) {
        auto resolve = resolveNode->as<OpResolveScope>();
        LValue scope = scopeToResolveFrom(resolveNode);
        unsigned site = allocateSite(resolveNode, numberOf(resolve.m_var), code().resolveScopeExtra(resolve));
        unsigned siteOfGet = allocateSite(node, numberOf(bytecode.m_var), code().getFromScopeExtra(bytecode));
        RELEASE_ASSERT(siteOfGet == site + 1);
        m_graph.remark("reads-global"_s, StringView(code().codeBlock()->identifier(bytecode.m_var).impl()));
        setJSValue(node, callStub(Stub::GetGlobal, Int64, { { scope, firstStubOperandGPR }, { slotAddress(site), GPRInfo::argumentGPR1 } }, { }));
        return;
    }
    auto distance = m_graph.accessedEnvironmentDepth(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    ResolveType type = bytecode.m_getPutInfo.resolveType();

    auto hopsToEnvironment = [&]() -> std::optional<unsigned> {
        if (node->use(bytecode.m_scope)->isBytecode(op_get_scope))
            return 0;
        return std::nullopt;
    };
    auto loadClosureVariable = [&](unsigned offset, bool mayBeLazy) {
        if (auto hops = m_graph.mayReturnScopeVariable ? hopsToEnvironment() : std::nullopt) {
            m_graph.returnedVariable = { *hops, offset };
            m_graph.remark("returns-scope-variable"_s);
        }
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
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTReadLazyClosureVar, m_instance, distance ? environmentAt(*distance) : scope, m_out.constInt32(offset)));
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
        loadClosureVariable(variable.offset.offset(), true);
        return;
    }

    unsigned throwIfNotFound = code().getFromScopeExtra(bytecode);
    if (usesDataStubs() && variable.isCachedInSlot() && Site::fits(numberOf(bytecode.m_var), throwIfNotFound)) {
        setJSValue(node, callStub(Stub::GetFromScope, Int64, { { scope, firstStubOperandGPR }, { slotAddress(sharedSite(node, numberOf(bytecode.m_var), throwIfNotFound)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;
    if (variable.isCachedInSlot()) {
        LBasicBlock hasAddress = m_out.newBlock();
        LBasicBlock noAddress = m_out.newBlock();
        LBasicBlock structureHit = m_out.newBlock();

        LValue word = m_out.load64(slotWord(slot, 0));
        m_out.branch(m_out.equal(m_out.load32(scope, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(structureHit), rarely(slowCase));

        m_out.appendTo(structureHit, hasAddress);
        LValue address = m_out.loadPtr(slotWord(slot, 1));
        m_out.branch(m_out.notNull(address), unsure(hasAddress), unsure(noAddress));

        LBasicBlock isAddress = m_out.newBlock();
        LBasicBlock isSymbolTable = m_out.newBlock();
        LBasicBlock rightSymbolTable = m_out.newBlock();
        m_out.appendTo(hasAddress, isSymbolTable);
        static_assert(Slot::pointerIsCell == 1u << 31);
        m_out.branch(m_out.lessThan(word, m_out.int64Zero), unsure(isSymbolTable), unsure(isAddress));

        m_out.appendTo(isSymbolTable, rightSymbolTable);
        m_out.branch(m_out.equal(m_out.loadPtr(m_out.address(m_heaps.root, scope, JSSymbolTableObject::offsetOfSymbolTable())), address), usually(rightSymbolTable), rarely(slowCase));

        m_out.appendTo(rightSymbolTable, isAddress);
        LValue offsetInEnvironment = m_out.zeroExtPtr(m_out.bitAnd(highHalf(m_out, word), m_out.constInt32(Slot::offsetMask)));
        LValue variableValue = m_out.load64(TypedPointer(m_heaps.root, m_out.add(scope, m_out.add(m_out.shl(offsetInEnvironment, m_out.constInt32(3)), m_out.constIntPtr(JSLexicalEnvironment::offsetOfVariables())))));
        results.append(m_out.anchor(variableValue));
        m_out.branch(m_out.notZero64(variableValue), usually(continuation), rarely(slowCase));

        m_out.appendTo(isAddress, noAddress);
        LValue value = m_out.load64(TypedPointer(m_heaps.root, address));
        results.append(m_out.anchor(value));
        m_out.branch(m_out.notZero64(value), usually(continuation), rarely(slowCase));

        m_out.appendTo(noAddress, slowCase);
        results.append(m_out.anchor(loadProperty(scope, highHalf(m_out, word))));
        m_out.jump(continuation);
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetFromScope, m_instance, scope, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(slot), m_out.constInt32(throwIfNotFound))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

std::optional<unsigned> Lowering::offsetOfVariableStoredInline(Node* node)
{
    auto bytecode = node->as<OpPutToScope>();
    ResolveType type = bytecode.m_getPutInfo.resolveType();
    if (type == ResolvedClosureVar)
        return bytecode.m_offset;
    SetForScope code(m_code, node->graph);
    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_symbolTableOrScopeDepth.scopeDepth(), type);
    if (variable.kind == StaticVariable::Closure && (!variable.isReadOnly || isInitialization(bytecode.m_getPutInfo.initializationMode())))
        return variable.offset.offset();
    return std::nullopt;
}

bool Lowering::mayCollectOrThrow(Node* node)
{
    for (auto& use : node->uses) {
        if (use.node->kind == NodeKind::ConstantCell)
            return true;
    }
    switch (node->kind) {
    case NodeKind::Constant:
    case NodeKind::Intrinsic:
    case NodeKind::LinkTimeConstant:
    case NodeKind::Argument:
    case NodeKind::Phi:
    case NodeKind::Proj:
    case NodeKind::GetStack:
    case NodeKind::SetStack:
        return false;
    case NodeKind::Narrow:
        return !!node->fieldOrigin;
    case NodeKind::ConstantCell:
    case NodeKind::Guard:
        return true;
    case NodeKind::Bytecode:
        break;
    }
    if (node->guard)
        return true;
    switch (node->opcode) {
    case op_get_scope:
    case op_get_parent_scope:
    case op_argument_count:
    case op_get_argument:
        return false;
    case op_check_tdz:
        return mayBe(node->use(node->as<OpCheckTdz>().m_targetVirtualRegister)->type, TEmpty);
    case op_resolve_scope:
        return !isStaticClosureVarResolveType(node->as<OpResolveScope>().m_resolveType);
    case op_get_from_scope:
        return !node->promotedEnvironment && node->as<OpGetFromScope>().m_getPutInfo.resolveType() != ResolvedClosureVar;
    case op_put_to_scope:
        return !node->promotedEnvironment && !offsetOfVariableStoredInline(node);
    case op_new_func:
    case op_new_func_exp:
        return !Graph::makesNoFunctionObject(node);
    default:
        return true;
    }
}

bool Lowering::isFollowedByStoreBarrier(Node* node)
{
    Node* scope = node->use(node->as<OpPutToScope>().m_scope);
    auto distance = m_graph.accessedEnvironmentDepth(node);
    if (m_nodeIndex >= m_block->nodes.size() || m_block->nodes[m_nodeIndex] != node)
        return false;
    for (unsigned index = m_nodeIndex + 1; index < m_block->nodes.size(); ++index) {
        Node* later = m_block->nodes[index];
        if (later->isElided)
            continue;
        if (mayCollectOrThrow(later))
            return false;
        if (!later->isBytecode(op_put_to_scope) || later->promotedEnvironment)
            continue;
        auto bytecode = later->as<OpPutToScope>();
        Type stored = later->use(bytecode.m_value)->type;
        if (later->use(bytecode.m_scope) == scope && stored && isSubtype(stored, TCell) && m_graph.accessedEnvironmentDepth(later) == distance)
            return true;
    }
    return false;
}

void Lowering::lowerPutToScope(Node* node)
{
    auto bytecode = node->as<OpPutToScope>();
    if (node->promotedEnvironment) {
        writePromotedVariable(node->promotedEnvironment, node->offsetInEnvironment, lowJSValue(node->use(bytecode.m_value)));
        return;
    }
    auto distance = m_graph.accessedEnvironmentDepth(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    Node* valueNode = node->use(bytecode.m_value);
    LValue value = lowJSValue(valueNode);

    if (auto closureOffset = offsetOfVariableStoredInline(node)) {
        m_out.store64(value, scope, m_heaps.JSLexicalEnvironment_variables[*closureOffset]);
        if (mayBe(valueNode->type, TCell)) {
            if (!distance && m_newCells.contains(node->use(bytecode.m_scope)))
                m_graph.remark("no-write-barrier-for-new-environment"_s);
            else if (isFollowedByStoreBarrier(node))
                m_graph.remark("shares-write-barrier"_s);
            else if (isSubtype(valueNode->type, TCell))
                storeBarrier(scope);
            else {
                m_graph.remark("write-barrier-only-for-cell"_s);
                LBasicBlock cellCase = m_out.newBlock();
                LBasicBlock continuation = m_out.newBlock();
                m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));
                m_out.appendTo(cellCase);
                storeBarrier(scope);
                m_out.jump(continuation);
                m_out.appendTo(continuation);
            }
        }
        return;
    }

    GetPutInfo info = bytecode.m_getPutInfo;
    static_assert(static_cast<unsigned>(ThrowIfNotFound) <= 1 && static_cast<unsigned>(DoNotThrowIfNotFound) <= 1);
    unsigned how = static_cast<unsigned>(info.resolveMode()) | static_cast<unsigned>(info.initializationMode()) << 1 | info.ecmaMode().isStrict() << 3;
    RELEASE_ASSERT(static_cast<unsigned>(info.initializationMode()) <= 3);

    if (usesDataStubs() && Site::fits(numberOf(bytecode.m_var), how)) {
        callStub(Stub::PutToScope, Void, { { scope, firstStubOperandGPR }, { value, GPRInfo::argumentGPR1 }, { slotAddress(allocateSite(node, numberOf(bytecode.m_var), how)), GPRInfo::argumentGPR2 } }, { });
        return;
    }
    vmCall(node, Void, Entry::operationAOTPutToScope, m_instance, scope, value, m_out.constInt32(numberOf(bytecode.m_var)), slotAddress(allocateSlot()), m_out.constInt32(how));
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

#endif // ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

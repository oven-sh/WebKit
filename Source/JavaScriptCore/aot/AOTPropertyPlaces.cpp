/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyPlaces.h"

#if ENABLE(AOT)

#include "AOTGraph.h"
#include "AOTImage.h"
#include "BuiltinNames.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "PreciseJumpTargetsInlines.h"
#include "UnlinkedFunctionCodeBlock.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyPlaces);

static PropertyPlaces* s_propertyPlaces;
void setPropertyPlaces(PropertyPlaces* places) { s_propertyPlaces = places; }
PropertyPlaces* propertyPlaces() { return s_propertyPlaces; }

static bool nameComesBefore(UniquedStringImpl* a, UniquedStringImpl* b)
{
    if (!a || !b)
        return !a && b;
    return codePointCompareLessThan(StringView { a }, StringView { b });
}

void PropertyPlaces::note(Names&& namesInSlotOrder)
{
    while (!namesInSlotOrder.isEmpty() && !namesInSlotOrder.last())
        namesInSlotOrder.removeLast();
    if (namesInSlotOrder.isEmpty())
        return;
    Locker locker { m_lock };
    m_shapes.append(WTF::move(namesInSlotOrder));
}

void PropertyPlaces::finalize()
{
    m_numberOfBirths = m_shapes.size();
    std::ranges::sort(m_shapes, [](const Names& a, const Names& b) {
        return std::ranges::lexicographical_compare(a, b, nameComesBefore);
    });
    removeRepeatedElements(m_shapes);
    for (unsigned shape = 0; shape < m_shapes.size(); ++shape) {
        for (UniquedStringImpl* name : m_shapes[shape]) {
            if (name)
                m_holders.add(name, Holders { }).iterator->value.shapes.append(shape);
        }
    }
    m_namesInIDOrder = copyToVector(m_holders.keys());
    std::ranges::sort(m_namesInIDOrder, [&](UniquedStringImpl* a, UniquedStringImpl* b) {
        size_t holdersOfA = m_holders.find(a)->value.shapes.size();
        size_t holdersOfB = m_holders.find(b)->value.shapes.size();
        return holdersOfA != holdersOfB ? holdersOfA > holdersOfB : nameComesBefore(a, b);
    });
    for (unsigned index = 0; index < m_namesInIDOrder.size(); ++index)
        m_holders.find(m_namesInIDOrder[index])->value.indexInIDOrder = index;
}

uint16_t PropertyPlaces::nameID(UniquedStringImpl* name) const
{
    auto it = m_holders.find(name);
    if (it == m_holders.end() || !m_firstNameID)
        return 0;
    static_assert(maxPropertyNameIDInImage < Structure::firstReservedPropertyNameID);
    uint64_t id = static_cast<uint64_t>(m_firstNameID) + it->value.indexInIDOrder;
    return id <= maxPropertyNameIDInImage ? static_cast<uint16_t>(id) : 0;
}

auto PropertyPlaces::decide(UniquedStringImpl* name, std::span<UniquedStringImpl* const> namesAccessed, GuessedPlace& place) const -> Decision
{
    Decision decision = [&] {
        if (std::ranges::find(namesAccessed, name) == namesAccessed.end())
            return Decision::NoShape;
        const Vector<uint32_t>* fewestHolders = nullptr;
        for (UniquedStringImpl* accessed : namesAccessed) {
            auto it = m_holders.find(accessed);
            if (it == m_holders.end())
                return Decision::NoShape;
            if (!fewestHolders || it->value.shapes.size() < fewestHolders->size())
                fewestHolders = &it->value.shapes;
        }
        size_t slot = notFound;
        for (uint32_t shape : *fewestHolders) {
            const Names& names = m_shapes[shape];
            if (!std::ranges::all_of(namesAccessed, [&](UniquedStringImpl* accessed) { return names.contains(accessed); }))
                continue;
            size_t slotInShape = names.find(name);
            if (slot != notFound && slot != slotInShape)
                return Decision::Disagree;
            slot = slotInShape;
        }
        if (slot == notFound)
            return Decision::NoShape;
        if (slot >= Structure::numberOfSlotsWithPropertyNameIDs)
            return Decision::SlotTooHigh;
        place = { nameID(name), static_cast<uint8_t>(slot) };
        return place.nameID ? Decision::Guessed : Decision::NoNameID;
    }();
    m_decisions[static_cast<unsigned>(decision)].fetch_add(1, std::memory_order_relaxed);
    return decision;
}

void PropertyPlaces::dump(PrintStream& out) const
{
    auto count = [&](Decision decision) { return m_decisions[static_cast<unsigned>(decision)].load(std::memory_order_relaxed); };
    out.print(m_numberOfBirths, " births, ", m_shapes.size(), " shapes, ", m_namesInIDOrder.size(), " names; places guessed: ", count(Decision::Guessed));
    out.print("; not guessed: no shape ", count(Decision::NoShape), ", shapes disagree ", count(Decision::Disagree), ", slot too high ", count(Decision::SlotTooHigh), ", no name ID ", count(Decision::NoNameID));
    out.print("; names only called that no shape holds: ", m_namesOnlyCalled.load(std::memory_order_relaxed));
}

static void appendNameGivenAtBirth(VM& vm, UniquedStringImpl* name, PropertyPlaces::Names& names)
{
    if (names.contains(name))
        return;
    bool canHavePlace = !name->isSymbol() && !parseIndex(*name) && name != vm.propertyNames->underscoreProto.impl();
    names.append(canHavePlace ? name : nullptr);
}

static bool appendNamesStoredOnThis(VM& vm, UnlinkedCodeBlock* codeBlock, const PropertyPlaces::Names* namesFromFieldInitializer, PropertyPlaces::Names& names)
{
    VirtualRegister thisRegister = codeBlock->thisRegister();
    unsigned endOfConditionalCode = 0;
    for (const auto& instruction : codeBlock->instructions()) {
        OpcodeID opcode = instruction->opcodeID();
        unsigned offset = instruction.offset();
        bool isOnEveryPath = offset >= endOfConditionalCode;
        if (isBranch(opcode)) {
            bool jumpsBack = false;
            extractStoredJumpTargetsForInstruction(codeBlock, instruction, [&](int32_t relativeOffset) {
                if (relativeOffset <= 0)
                    jumpsBack = true;
                else
                    endOfConditionalCode = std::max(endOfConditionalCode, offset + static_cast<unsigned>(relativeOffset));
            });
            if (jumpsBack)
                return true;
            continue;
        }
        if (opcode == op_loop_hint || opcode == op_catch || ((isTerminal(opcode) || isThrow(opcode)) && isOnEveryPath))
            return true;
        switch (opcode) {
        case op_type_tag:
            return false;
        case op_get_by_id_direct:
            if (codeBlock->identifier(instruction->as<OpGetByIdDirect>().m_property) != vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName())
                break;
            if (!namesFromFieldInitializer || !isOnEveryPath)
                return false;
            names.appendVector(*namesFromFieldInitializer);
            break;
        case op_put_by_val_direct:
            if (instruction->as<OpPutByValDirect>().m_base == thisRegister)
                return true;
            break;
        case op_put_private_name: {
            auto bytecode = instruction->as<OpPutPrivateName>();
            if (bytecode.m_base != thisRegister || !bytecode.m_putKind.isDefine())
                break;
            if (!isOnEveryPath)
                return true;
            names.append(nullptr);
            break;
        }
        case op_put_by_id: {
            auto bytecode = instruction->as<OpPutById>();
            UniquedStringImpl* name = codeBlock->identifier(bytecode.m_property).impl();
            if (bytecode.m_base != thisRegister || names.contains(name))
                break;
            if (!isOnEveryPath)
                return true;
            appendNameGivenAtBirth(vm, name, names);
            break;
        }
        default:
            break;
        }
    }
    return true;
}

static const Node* valueAccessedThrough(const Node* node)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz) || node->isBytecode(op_type_tag))
        node = node->uses[0].node;
    return node;
}

void Graph::noteBirths()
{
    PropertyPlaces* places = propertyPlaces();
    if (!places)
        return;
    Vector<PropertyPlaces::Names> literals;
    UncheckedKeyHashMap<const Node*, unsigned> literalMadeBy;
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (node->isBytecode(op_new_object)) {
                if (auto shape = literalShape(node); shape && (shape->layoutID || !shape->slots.isEmpty()))
                    continue;
                literalMadeBy.add(node, static_cast<unsigned>(literals.size()));
                literals.append(PropertyPlaces::Names { });
                auto& stores = literalStores(node->bytecodeIndex.offset());
                for (unsigned i = 0; i < node->numberOfLiteralProperties; ++i)
                    appendNameGivenAtBirth(m_vm, m_codeBlock->identifier(m_codeBlock->instructions().at(stores[i])->as<OpPutById>().m_property).impl(), literals.last());
                continue;
            }
            if (!node->isBytecode(op_put_by_id))
                continue;
            auto bytecode = node->as<OpPutById>();
            const Node* base = valueAccessedThrough(node->use(bytecode.m_base));
            if (auto it = literalMadeBy.find(base); it != literalMadeBy.end()) {
                if (bytecode.m_flags.isDirect() || base->block == block)
                    appendNameGivenAtBirth(m_vm, m_codeBlock->identifier(bytecode.m_property).impl(), literals[it->value]);
                continue;
            }
            if (m_codeBlock->identifier(bytecode.m_property) != m_vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName())
                continue;
            const KnownFunction* constructor = functionMadeBy(base);
            const KnownFunction* initializer = functionMadeBy(node->use(bytecode.m_value));
            if (!constructor || !constructor->forConstruct || constructor->forConstruct->constructorKind() != ConstructorKind::Base || !initializer || !initializer->forCall)
                continue;
            PropertyPlaces::Names namesFromFieldInitializer;
            PropertyPlaces::Names names;
            if (appendNamesStoredOnThis(m_vm, initializer->forCall, nullptr, namesFromFieldInitializer) && appendNamesStoredOnThis(m_vm, constructor->forConstruct, &namesFromFieldInitializer, names))
                places->note(WTF::move(names));
        }
    }
    for (auto& names : literals)
        places->note(WTF::move(names));
    if (m_codeBlock->isConstructor() && m_codeBlock->constructorKind() != ConstructorKind::Extends) {
        PropertyPlaces::Names names;
        if (appendNamesStoredOnThis(m_vm, m_codeBlock, nullptr, names))
            places->note(WTF::move(names));
    }
}

void Graph::findNamesAccessed() const
{
    const PropertyPlaces& places = *propertyPlaces();
    UncheckedKeyHashSet<const Node*> readsUsedOtherThanAsCallee;
    auto noteUsesOf = [&](const Node* node) {
        if (node->kind == NodeKind::Guard)
            return;
        VirtualRegister callee;
        if (node->isBytecode(op_call))
            callee = node->as<OpCall>().m_callee;
        else if (node->isBytecode(op_call_ignore_result))
            callee = node->as<OpCallIgnoreResult>().m_callee;
        else if (node->isBytecode(op_tail_call))
            callee = node->as<OpTailCall>().m_callee;
        for (auto& use : node->uses) {
            if (use.node->isBytecode(op_get_by_id) && use.reg != callee)
                readsUsedOtherThanAsCallee.add(use.node);
        }
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* phi : block->phis)
            noteUsesOf(phi);
        for (Node* node : block->nodes)
            noteUsesOf(node);
    }
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            bool isRead = node->isBytecode(op_get_by_id);
            if (!isRead && !node->isBytecode(op_put_by_id))
                continue;
            UniquedStringImpl* name = node->graph->codeBlock()->identifier(isRead ? node->as<OpGetById>().m_property : node->as<OpPutById>().m_property).impl();
            if (isRead && !readsUsedOtherThanAsCallee.contains(node) && !places.isHeld(name)) {
                places.countNameOnlyCalled();
                continue;
            }
            auto& names = m_namesAccessedOn.add(valueAccessedThrough(node->use(isRead ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base)), Vector<UniquedStringImpl*, 4> { }).iterator->value;
            if (!names.contains(name))
                names.append(name);
        }
    }
}

std::optional<GuessedPlace> Graph::guessedPlaceOf(const Node* access) const
{
    const PropertyPlaces* places = propertyPlaces();
    if (!Options::useAOTGuessedPlaces() || !places)
        return std::nullopt;
    if (!isOutermost())
        return m_outermost->guessedPlaceOf(access);
    if (!std::exchange(m_hasFoundNamesAccessed, true))
        findNamesAccessed();
    bool isRead = access->isBytecode(op_get_by_id);
    RELEASE_ASSERT(isRead || access->isBytecode(op_put_by_id));
    UniquedStringImpl* name = access->graph->codeBlock()->identifier(isRead ? access->as<OpGetById>().m_property : access->as<OpPutById>().m_property).impl();
    auto it = m_namesAccessedOn.find(valueAccessedThrough(access->use(isRead ? access->as<OpGetById>().m_base : access->as<OpPutById>().m_base)));
    GuessedPlace place { };
    auto decision = places->decide(name, it == m_namesAccessedOn.end() ? std::span<UniquedStringImpl* const> { } : it->value.span(), place);
    static constexpr ASCIILiteral reasons[PropertyPlaces::numberOfDecisions] = { ""_s, "no-shape"_s, "disagree"_s, "slot-too-high"_s, "no-name-id"_s };
    if (decision != PropertyPlaces::Decision::Guessed) {
        m_outermost->remark("no-guess"_s, reasons[static_cast<unsigned>(decision)]);
        return std::nullopt;
    }
    m_outermost->remark("guessed-place"_s, StringView { name });
    return place;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

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

void PropertyPlaces::noteSites(const NumberOfSitesByName& numberOfSitesByName)
{
    Locker locker { m_lock };
    for (auto& [name, numberOfSites] : numberOfSitesByName)
        m_listedNames.add(name, ListedName { }).iterator->value.numberOfSites += numberOfSites;
}

void PropertyPlaces::noteConstruction(UnlinkedCodeBlock* constructor, UnlinkedCodeBlock* parentConstructor, Names&& ownNamesInSlotOrder, bool ownNamesAreAll)
{
    Locker locker { m_lock };
    m_constructions.set(constructor, Construction { parentConstructor, WTF::move(ownNamesInSlotOrder), ownNamesAreAll });
}

bool PropertyPlaces::appendNamesOfInstances(UnlinkedCodeBlock* constructor, Names& names, bool& areAll, unsigned numberOfDescendants) const
{
    auto it = m_constructions.find(constructor);
    if (it == m_constructions.end() || numberOfDescendants > maxNumberOfAncestors)
        return false;
    if (it->value.parentConstructor && !appendNamesOfInstances(it->value.parentConstructor, names, areAll, numberOfDescendants + 1))
        return false;
    if (!areAll)
        return true;
    for (UniquedStringImpl* name : it->value.ownNames) {
        if (!name || !names.contains(name))
            names.append(name);
    }
    areAll = it->value.ownNamesAreAll;
    return true;
}

void PropertyPlaces::finalize()
{
    for (UnlinkedCodeBlock* constructor : m_constructions.keys()) {
        Names names;
        bool areAll = true;
        if (appendNamesOfInstances(constructor, names, areAll))
            note(WTF::move(names));
    }
    m_numberOfBirths = m_shapes.size();
    std::ranges::sort(m_shapes, [](const Names& a, const Names& b) {
        return std::ranges::lexicographical_compare(a, b, nameComesBefore);
    });
    removeRepeatedElements(m_shapes);
    for (unsigned shape = 0; shape < m_shapes.size(); ++shape) {
        for (UniquedStringImpl* name : m_shapes[shape]) {
            if (!name)
                continue;
            m_holders.add(name, Holders { }).iterator->value.shapes.append(shape);
            m_listedNames.add(name, ListedName { });
        }
    }
    m_namesInIDOrder = copyToVector(m_listedNames.keys());
    std::ranges::sort(m_namesInIDOrder, [&](UniquedStringImpl* a, UniquedStringImpl* b) {
        unsigned sitesOfA = m_listedNames.find(a)->value.numberOfSites;
        unsigned sitesOfB = m_listedNames.find(b)->value.numberOfSites;
        return sitesOfA != sitesOfB ? sitesOfA > sitesOfB : nameComesBefore(a, b);
    });
    for (unsigned index = 0; index < m_namesInIDOrder.size(); ++index)
        m_listedNames.find(m_namesInIDOrder[index])->value.indexInIDOrder = index;
}

uint16_t PropertyPlaces::nameID(UniquedStringImpl* name) const
{
    auto it = m_listedNames.find(name);
    if (it == m_listedNames.end() || !m_firstNameID)
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
        unsigned numberOfShapes = 0;
        for (uint32_t shape : *fewestHolders) {
            const Names& names = m_shapes[shape];
            if (!std::ranges::all_of(namesAccessed, [&](UniquedStringImpl* accessed) { return names.contains(accessed); }))
                continue;
            size_t slotInShape = names.find(name);
            if (slot != notFound && slot != slotInShape)
                return Decision::Disagree;
            slot = slotInShape;
            ++numberOfShapes;
        }
        if (slot == notFound)
            return Decision::NoShape;
        if (slot >= Structure::numberOfSlotsWithPropertyNameIDs)
            return Decision::SlotTooHigh;
        place = { nameID(name), static_cast<uint8_t>(slot), static_cast<uint8_t>(std::min(numberOfShapes, 255u)) };
        if (!place.nameID)
            return Decision::NoNameID;
        if (numberOfShapes == 1)
            m_guessesFromOneShape.fetch_add(1, std::memory_order_relaxed);
        return Decision::Guessed;
    }();
    m_decisions[static_cast<unsigned>(decision)].fetch_add(1, std::memory_order_relaxed);
    return decision;
}

void PropertyPlaces::dump(PrintStream& out) const
{
    auto count = [&](Decision decision) { return m_decisions[static_cast<unsigned>(decision)].load(std::memory_order_relaxed); };
    out.print(m_numberOfBirths, " births, ", m_shapes.size(), " shapes with ", m_holders.size(), " names, ", m_namesInIDOrder.size(), " names listed; places guessed: ", count(Decision::Guessed), ", ", m_guessesFromOneShape.load(std::memory_order_relaxed), " of them from one shape");
    out.print("; not guessed: no shape ", count(Decision::NoShape), ", shapes disagree ", count(Decision::Disagree), ", slot too high ", count(Decision::SlotTooHigh), ", no name ID ", count(Decision::NoNameID));
    out.print("; names only called that no shape holds: ", m_namesOnlyCalled.load(std::memory_order_relaxed));
    if (Options::useAOTGuardsOverWholeFunctions())
        out.print("; functions with guards over the whole function: ", m_functionsWithGuards.load(std::memory_order_relaxed), " with ", m_guardsOverWholeFunctions.load(std::memory_order_relaxed), " guards, ", m_bytecodeSizeWithGuards.load(std::memory_order_relaxed), " bytes of bytecode, ", m_codeSizeWithGuards.load(std::memory_order_relaxed), " bytes of code");
}

static bool canHavePlace(VM& vm, UniquedStringImpl* name)
{
    return !name->isSymbol() && !parseIndex(*name) && name != vm.propertyNames->underscoreProto.impl();
}

static void appendNameGivenAtBirth(VM& vm, UniquedStringImpl* name, PropertyPlaces::Names& names)
{
    if (!names.contains(name))
        names.append(canHavePlace(vm, name) ? name : nullptr);
}

struct NamesStoredOnThis {
    PropertyPlaces::Names names;
    bool areAll { true };
};

static std::optional<NamesStoredOnThis> namesStoredOnThis(VM& vm, UnlinkedCodeBlock* codeBlock, const NamesStoredOnThis* fromFieldInitializer)
{
    NamesStoredOnThis result;
    VirtualRegister thisRegister = codeBlock->thisRegister();
    unsigned endOfConditionalCode = 0;
    bool isDone = false;
    auto stopAtUnknownSlots = [&] {
        isDone = true;
        result.areAll = false;
    };
    for (const auto& instruction : codeBlock->instructions()) {
        OpcodeID opcode = instruction->opcodeID();
        if (opcode == op_type_tag)
            return std::nullopt;
        if (isDone)
            continue;
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
                stopAtUnknownSlots();
            continue;
        }
        if ((isTerminal(opcode) || isThrow(opcode)) && isOnEveryPath) {
            isDone = true;
            continue;
        }
        switch (opcode) {
        case op_loop_hint:
        case op_catch:
            stopAtUnknownSlots();
            break;
        case op_get_by_id_direct:
            if (codeBlock->identifier(instruction->as<OpGetByIdDirect>().m_property) != vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName())
                break;
            if (!fromFieldInitializer || !isOnEveryPath)
                return std::nullopt;
            result.names.appendVector(fromFieldInitializer->names);
            if (!fromFieldInitializer->areAll)
                stopAtUnknownSlots();
            break;
        case op_put_by_val_direct:
            if (instruction->as<OpPutByValDirect>().m_base == thisRegister)
                stopAtUnknownSlots();
            break;
        case op_put_private_name: {
            auto bytecode = instruction->as<OpPutPrivateName>();
            if (bytecode.m_base != thisRegister || !bytecode.m_putKind.isDefine())
                break;
            if (isOnEveryPath)
                result.names.append(nullptr);
            else
                stopAtUnknownSlots();
            break;
        }
        case op_put_by_id: {
            auto bytecode = instruction->as<OpPutById>();
            UniquedStringImpl* name = codeBlock->identifier(bytecode.m_property).impl();
            if (bytecode.m_base != thisRegister || result.names.contains(name))
                break;
            if (isOnEveryPath)
                appendNameGivenAtBirth(vm, name, result.names);
            else
                stopAtUnknownSlots();
            break;
        }
        default:
            break;
        }
    }
    return result;
}

static const Node* valueAccessedThrough(const Node* node)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz) || node->isBytecode(op_type_tag))
        node = node->uses[0].node;
    return node;
}

void Graph::noteBirths()
{
    PropertyPlaces* places = m_propertyPlaces;
    if (!places)
        return;
    PropertyPlaces::NumberOfSitesByName numberOfSitesByName;
    for (const auto& instruction : m_codeBlock->instructions()) {
        unsigned identifier = 0;
        switch (instruction->opcodeID()) {
        case op_get_by_id:
            identifier = instruction->as<OpGetById>().m_property;
            break;
        case op_put_by_id:
            identifier = instruction->as<OpPutById>().m_property;
            break;
        case op_in_by_id:
            identifier = instruction->as<OpInById>().m_property;
            break;
        default:
            continue;
        }
        if (UniquedStringImpl* name = m_codeBlock->identifier(identifier).impl(); canHavePlace(m_vm, name))
            numberOfSitesByName.add(name, 0).iterator->value++;
    }
    places->noteSites(numberOfSitesByName);
    struct Literal {
        PropertyPlaces::Names names;
        bool isComplete { false };
    };
    Vector<Literal> literals;
    UncheckedKeyHashMap<const Node*, unsigned> literalMadeBy;
    struct ClassDefinition {
        UnlinkedCodeBlock* parentConstructor { nullptr };
        UnlinkedCodeBlock* fieldInitializer { nullptr };
    };
    UncheckedKeyHashMap<UnlinkedCodeBlock*, ClassDefinition> classes;
    auto classConstructedBy = [&](const Node* value) -> ClassDefinition* {
        const KnownFunction* constructor = functionMadeBy(valueAccessedThrough(value));
        return constructor && constructor->forConstruct ? &classes.add(constructor->forConstruct, ClassDefinition { }).iterator->value : nullptr;
    };
    auto literalIn = [&](const Node* value) -> Literal* {
        auto it = literalMadeBy.find(valueAccessedThrough(value));
        return it == literalMadeBy.end() ? nullptr : &literals[it->value];
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (valueAccessedThrough(node) != node)
                continue;
            VirtualRegister baseOfStoreAtBirth;
            if (node->isBytecode(op_put_by_id)) {
                auto bytecode = node->as<OpPutById>();
                if (Literal* literal = literalIn(node->use(bytecode.m_base)); literal && !literal->isComplete && (bytecode.m_flags.isDirect() || valueAccessedThrough(node->use(bytecode.m_base))->block == block)) {
                    baseOfStoreAtBirth = bytecode.m_base;
                    appendNameGivenAtBirth(m_vm, m_codeBlock->identifier(bytecode.m_property).impl(), literal->names);
                }
            } else if (node->isBytecode(op_put_by_val_direct)) {
                auto bytecode = node->as<OpPutByValDirect>();
                if (Literal* literal = literalIn(node->use(bytecode.m_base)); literal && !literal->isComplete) {
                    baseOfStoreAtBirth = bytecode.m_base;
                    if (!node->use(bytecode.m_property)->isNumberConstant())
                        literal->names.append(nullptr);
                }
            }
            if (!literals.isEmpty()) {
                for (auto& use : node->uses) {
                    if (baseOfStoreAtBirth.isValid() && use.reg == baseOfStoreAtBirth)
                        continue;
                    if (Literal* literal = literalIn(use.node))
                        literal->isComplete = true;
                }
            }
            if (node->isBytecode(op_new_object)) {
                if (auto shape = literalShape(node); shape && (shape->layoutID || !shape->slots.isEmpty()))
                    continue;
                literalMadeBy.add(node, static_cast<unsigned>(literals.size()));
                literals.append(Literal { });
                auto& stores = literalStores(node->bytecodeIndex.offset());
                for (unsigned i = 0; i < node->numberOfLiteralProperties; ++i)
                    appendNameGivenAtBirth(m_vm, m_codeBlock->identifier(m_codeBlock->instructions().at(stores[i])->as<OpPutById>().m_property).impl(), literals.last().names);
                continue;
            }
            if (node->isBytecode(op_call_ignore_result)) {
                CallOperands operands = callOperands(node->instruction);
                const Node* callee = node->use(operands.callee);
                if (callee->kind != NodeKind::LinkTimeConstant || static_cast<LinkTimeConstant>(callee->intrinsic) != LinkTimeConstant::setPrototypeDirectOrThrow || operands.argc != 2)
                    continue;
                const Node* parent = valueAccessedThrough(node->use(operands.argument(1)));
                const KnownFunction* parentConstructor = parent->isBytecode(op_get_from_scope) ? knownFunctionReadBy(parent) : functionMadeBy(parent);
                if (ClassDefinition* definition = classConstructedBy(node->use(operands.argument(0))); definition && parentConstructor)
                    definition->parentConstructor = parentConstructor->forConstruct;
                continue;
            }
            if (!node->isBytecode(op_put_by_id))
                continue;
            auto bytecode = node->as<OpPutById>();
            if (m_codeBlock->identifier(bytecode.m_property) != m_vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName())
                continue;
            const KnownFunction* initializer = functionMadeBy(node->use(bytecode.m_value));
            if (ClassDefinition* definition = classConstructedBy(node->use(bytecode.m_base)); definition && initializer)
                definition->fieldInitializer = initializer->forCall;
        }
    }
    for (auto& [constructor, definition] : classes) {
        bool isDerived = constructor->constructorKind() == ConstructorKind::Extends;
        if (isDerived ? !definition.parentConstructor : !definition.fieldInitializer)
            continue;
        std::optional<NamesStoredOnThis> fromFieldInitializer;
        if (definition.fieldInitializer) {
            fromFieldInitializer = namesStoredOnThis(m_vm, definition.fieldInitializer, nullptr);
            if (!fromFieldInitializer)
                continue;
        }
        if (auto stored = namesStoredOnThis(m_vm, constructor, fromFieldInitializer ? &*fromFieldInitializer : nullptr))
            places->noteConstruction(constructor, isDerived ? definition.parentConstructor : nullptr, WTF::move(stored->names), stored->areAll);
    }
    for (auto& literal : literals)
        places->note(WTF::move(literal.names));
    if (m_codeBlock->isConstructor() && m_codeBlock->constructorKind() != ConstructorKind::Extends) {
        if (auto stored = namesStoredOnThis(m_vm, m_codeBlock, nullptr))
            places->noteConstruction(m_codeBlock, nullptr, WTF::move(stored->names), stored->areAll);
    }
}

void Graph::findNamesAccessed() const
{
    const PropertyPlaces& places = *m_propertyPlaces;
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
    if (!isOutermost())
        return m_outermost->guessedPlaceOf(access);
    const PropertyPlaces* places = m_propertyPlaces;
    if (!Options::useAOTGuessedPlaces() || !places)
        return std::nullopt;
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
    if (place.numberOfShapes == 1)
        m_outermost->remark("guessed-place-of-one-shape"_s, StringView { name });
    return place;
}

Graph::PlacesToGuard Graph::findPlacesToGuard()
{
    PlacesToGuard places;
    if (!Options::useAOTGuardsOverWholeFunctions() || !Options::useAOTGuessedPlaces() || !m_propertyPlaces || !usesDataStubs())
        return places;
    auto refuse = [&](ASCIILiteral reason) {
        remark("no-guards-over-whole-function"_s, reason);
        reasonForNoGuards = reason;
        return PlacesToGuard { };
    };
    if (m_codeBlock->codeType() != FunctionCode)
        return refuse("is-not-function-code"_s);
    if (m_codeBlock->isConstructor())
        return refuse("is-constructor"_s);
    switch (m_codeBlock->parseMode()) {
    case SourceParseMode::NormalFunctionMode:
    case SourceParseMode::ArrowFunctionMode:
    case SourceParseMode::MethodMode:
    case SourceParseMode::GetterMode:
    case SourceParseMode::SetterMode:
        break;
    default:
        return refuse("is-generator-or-async"_s);
    }
    if (m_codeBlock->numberOfExceptionHandlers())
        return refuse("has-handler"_s);
    if (m_codeBlock->instructions().size() > Options::maximumAOTBytecodeSizeForGuardsOverWholeFunction())
        return refuse("too-large"_s);
    if (numberOfRegisterReturnValues)
        return refuse("returns-values-in-registers"_s);
    UncheckedKeyHashSet<const Node*> readsOfCallees;
    for (BasicBlock* block : m_rpo) {
        if (block->graph == this && !Options::useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting() && (block->isInProfitableLoop || (block->isInLoop && !Options::useAOTLoopSplitting())))
            return refuse("has-loop-without-calls"_s);
        for (Node* node : block->nodes) {
            if (node->graph == this && node->isElided && node->isBytecode(op_new_object))
                return refuse("does-not-allocate-object"_s);
            VirtualRegister callee;
            if (node->isBytecode(op_call))
                callee = node->as<OpCall>().m_callee;
            else if (node->isBytecode(op_call_ignore_result))
                callee = node->as<OpCallIgnoreResult>().m_callee;
            else if (node->isBytecode(op_tail_call))
                callee = node->as<OpTailCall>().m_callee;
            else if (node->isBytecode(op_construct))
                callee = node->as<OpConstruct>().m_callee;
            else
                continue;
            for (auto& use : node->uses) {
                if (use.reg == callee)
                    readsOfCallees.add(valueAccessedThrough(use.node));
            }
        }
    }
    unsigned firstOffsetBehindEnvironments = 0;
    for (const auto& instruction : m_codeBlock->instructions()) {
        if (instruction->opcodeID() == op_create_lexical_environment)
            firstOffsetBehindEnvironments = instruction.offset() + 1;
    }
    for (bool changed = !!firstOffsetBehindEnvironments; changed;) {
        changed = false;
        for (const auto& instruction : m_codeBlock->instructions()) {
            if (instruction.offset() < firstOffsetBehindEnvironments || !isBranch(instruction->opcodeID()))
                continue;
            extractStoredJumpTargetsForInstruction(m_codeBlock, instruction, [&](int32_t relativeOffset) {
                if (static_cast<int64_t>(instruction.offset()) + relativeOffset < static_cast<int64_t>(firstOffsetBehindEnvironments)) {
                    firstOffsetBehindEnvironments = instruction.offset() + 1;
                    changed = true;
                }
            });
        }
    }
    unsigned placesInFrontOfEnvironments = 0;
    BitVector isInsideLiteral;
    for (const auto& instruction : m_codeBlock->instructions()) {
        if (instruction->opcodeID() != op_new_object)
            continue;
        auto& stores = literalStores(instruction.offset());
        for (unsigned offset = instruction.offset(); !stores.isEmpty() && offset <= stores.last(); ++offset)
            isInsideLiteral.set(offset);
    }
    for (BasicBlock* block : m_rpo) {
        if (block->graph != this || block->isRarelyExecuted)
            continue;
        for (Node* node : block->nodes) {
            bool isRead = node->isBytecode(op_get_by_id);
            if ((!isRead && !node->isBytecode(op_put_by_id)) || node->graph != this || node->isElided || readsOfCallees.contains(node) || node->onlyChecksConstantObject || node->slotInConstantObjectPlusOne || node->replacement)
                continue;
            unsigned offset = node->bytecodeIndex.offset();
            if (isInsideLiteral.get(offset) || typeTagAt(offset) || isTypedFromOutside(offset) || typedFieldAccessedBy(node))
                continue;
            if (!isRead && node->as<OpPutById>().m_flags.isDirect())
                continue;
            const Node* base = node->use(isRead ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base);
            const Node* born = valueAccessedThrough(base);
            if ((base->type && !mayBe(base->type, TFinalObject)) || born->isBytecode(op_new_object) || born->isBytecode(op_create_this))
                continue;
            if (auto place = guessedPlaceOf(node)) {
                if (offset < firstOffsetBehindEnvironments)
                    ++placesInFrontOfEnvironments;
                else
                    places.add(offset, *place);
            }
        }
    }
    if (places.size() < Options::minimumAOTGuardsOverWholeFunction())
        return refuse(places.size() + placesInFrontOfEnvironments < Options::minimumAOTGuardsOverWholeFunction() ? "too-few-places"_s : "makes-environment-behind-guard"_s);
    return places;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

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
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyPlaces);

static bool nameComesBefore(UniquedStringImpl* a, UniquedStringImpl* b)
{
    if (!a || !b)
        return !a && b;
    return codePointCompareLessThan(StringView { a }, StringView { b });
}

void PropertyPlaces::note(Birth&& birth)
{
    while (!birth.names.isEmpty() && !birth.names.last())
        birth.names.removeLast();
    if (birth.names.isEmpty())
        return;
    birth.numberOfNamesGivenAtOnce = std::min(birth.numberOfNamesGivenAtOnce, static_cast<unsigned>(birth.names.size()));
    Locker locker { m_lock };
    m_births.append(WTF::move(birth));
}

void PropertyPlaces::noteLiteral(const CalleeHints* module, UnlinkedCodeBlock* codeBlock, unsigned bytecodeOffset, Names&& namesInSlotOrder, unsigned numberOfNamesGivenAtOnce)
{
    note(Birth { WTF::move(namesInSlotOrder), module, codeBlock, bytecodeOffset, numberOfNamesGivenAtOnce, false });
}

void PropertyPlaces::noteNamesAccessed(const CalleeHints* module, Vector<NamesAccessed>&& namesAccessed)
{
    Locker locker { m_lock };
    for (auto& accessed : namesAccessed) {
        if (accessed.variableReadFrom.first) {
            auto& onVariable = m_namesOnVariables.add(accessed.variableReadFrom, NamesOnVariable { }).iterator->value;
            auto add = [&](auto& names, UniquedStringImpl* name) {
                if (onVariable.areTooMany || names.contains(name))
                    return;
                if (onVariable.names.size() + onVariable.namesOnlyCalled.size() == maxNumberOfNamesOnVariable) {
                    onVariable.areTooMany = true;
                    return;
                }
                names.append(name);
            };
            for (UniquedStringImpl* name : accessed.names)
                add(onVariable.names, name);
            for (UniquedStringImpl* name : accessed.namesOnlyCalled)
                add(onVariable.namesOnlyCalled, name);
        }
        if (!accessed.names.isEmpty())
            m_namesAccessed.append({ module, WTF::move(accessed) });
    }
}

void PropertyPlaces::noteSites(const NumberOfSitesByName& numberOfSitesByName)
{
    Locker locker { m_lock };
    for (auto& [name, numberOfSites] : numberOfSitesByName)
        m_listedNames.add(name, ListedName { }).iterator->value.numberOfSites += numberOfSites;
}

void PropertyPlaces::noteConstruction(const CalleeHints* module, UnlinkedCodeBlock* constructor, UnlinkedCodeBlock* parentConstructor, Names&& ownNamesInSlotOrder, bool ownNamesAreAll, unsigned numberOfNamesGivenAtOnce)
{
    Locker locker { m_lock };
    m_constructions.set(constructor, Construction { module, parentConstructor, WTF::move(ownNamesInSlotOrder), ownNamesAreAll, numberOfNamesGivenAtOnce });
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
    for (auto& [constructor, construction] : m_constructions) {
        Birth birth { { }, construction.module, constructor, std::nullopt, construction.numberOfNamesGivenAtOnce, !!construction.parentConstructor };
        bool areAll = true;
        if (appendNamesOfInstances(constructor, birth.names, areAll))
            note(WTF::move(birth));
    }
    m_numberOfBirths = m_births.size();
    std::ranges::sort(m_births, [](const Birth& a, const Birth& b) {
        return std::ranges::lexicographical_compare(a.names, b.names, nameComesBefore);
    });
    for (auto& birth : m_births) {
        if (m_shapes.isEmpty() || m_shapes.last().names != birth.names) {
            m_shapes.append(Shape { });
            m_shapes.last().names = WTF::move(birth.names);
            m_shapes.last().numberOfNamesGivenAtOnce = birth.numberOfNamesGivenAtOnce;
        }
        Shape& shape = m_shapes.last();
        uint32_t index = static_cast<uint32_t>(m_shapes.size() - 1);
        if (birth.module)
            shape.modules.add(birth.module);
        shape.numberOfNamesGivenAtOnce = std::min(shape.numberOfNamesGivenAtOnce, birth.numberOfNamesGivenAtOnce);
        shape.isOfDerivedClass |= birth.isOfDerivedClass;
        if (birth.bytecodeOffsetOfLiteral)
            m_shapesOfLiterals.add(birth.codeBlock, Vector<std::pair<unsigned, uint32_t>, 1> { }).iterator->value.append({ *birth.bytecodeOffsetOfLiteral, index });
        else
            m_shapeOfInstances.add(birth.codeBlock, index);
    }
    m_births.clear();
    for (unsigned shape = 0; shape < m_shapes.size(); ++shape) {
        for (UniquedStringImpl* name : m_shapes[shape].names) {
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
    for (auto& [module, accessed] : m_namesAccessed) {
        bool usesNamesOnVariable = false;
        if (auto shapes = candidateShapes(module, accessed, usesNamesOnVariable); shapes.size() == 1)
            m_shapes[shapes[0]].numberOfSites += accessed.numberOfSites;
    }
    m_namesAccessed.clear();
    for (uint32_t shape = 0; shape < m_shapes.size(); ++shape) {
        if (Options::useAOTFamilies() && m_shapes[shape].numberOfSites && m_shapes[shape].numberOfNamesGivenAtOnce && !m_shapes[shape].isOfDerivedClass)
            m_shapeOfFamily.append(shape);
    }
    std::ranges::sort(m_shapeOfFamily, [&](uint32_t a, uint32_t b) {
        unsigned sitesOfA = m_shapes[a].numberOfSites;
        unsigned sitesOfB = m_shapes[b].numberOfSites;
        return sitesOfA != sitesOfB ? sitesOfA > sitesOfB : a < b;
    });
    if (m_shapeOfFamily.size() > maxNumberOfFamilies)
        m_shapeOfFamily.shrink(maxNumberOfFamilies);
    for (unsigned index = 0; index < m_shapeOfFamily.size(); ++index)
        m_shapes[m_shapeOfFamily[index]].family = static_cast<uint16_t>(index + 1);
}

uint16_t PropertyPlaces::familyOfLiteral(UnlinkedCodeBlock* codeBlock, unsigned bytecodeOffset) const
{
    auto it = m_shapesOfLiterals.find(codeBlock);
    if (it == m_shapesOfLiterals.end())
        return 0;
    for (auto& [offset, shape] : it->value) {
        if (offset == bytecodeOffset)
            return m_shapes[shape].family;
    }
    return 0;
}

uint16_t PropertyPlaces::familyOfInstances(UnlinkedCodeBlock* constructor) const
{
    auto it = m_shapeOfInstances.find(constructor);
    return it == m_shapeOfInstances.end() ? 0 : m_shapes[it->value].family;
}

std::span<UniquedStringImpl* const> PropertyPlaces::namesOfFamily(uint16_t family) const
{
    RELEASE_ASSERT(family && family <= m_shapeOfFamily.size());
    const Shape& shape = m_shapes[m_shapeOfFamily[family - 1]];
    return shape.names.span().first(shape.numberOfNamesGivenAtOnce);
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

Vector<uint32_t, 8> PropertyPlaces::shapesWithAllOf(const CalleeHints* module, const NamesAccessed& accessed, const NamesOnVariable* onVariable) const
{
    Names names;
    names.appendVector(accessed.names);
    for (UniquedStringImpl* name : accessed.namesOnlyCalled) {
        if (isHeld(name) && !names.contains(name))
            names.append(name);
    }
    if (onVariable) {
        for (UniquedStringImpl* name : onVariable->names) {
            if (!names.contains(name))
                names.append(name);
        }
        for (UniquedStringImpl* name : onVariable->namesOnlyCalled) {
            if (isHeld(name) && !names.contains(name))
                names.append(name);
        }
    }
    Vector<uint32_t, 8> result;
    const Vector<uint32_t>* fewestHolders = nullptr;
    for (UniquedStringImpl* name : names) {
        auto it = m_holders.find(name);
        if (it == m_holders.end())
            return result;
        if (!fewestHolders || it->value.shapes.size() < fewestHolders->size())
            fewestHolders = &it->value.shapes;
    }
    if (!fewestHolders)
        return result;
    bool areBornInModule = false;
    for (uint32_t index : *fewestHolders) {
        const Shape& shape = m_shapes[index];
        if (!std::ranges::all_of(names, [&](UniquedStringImpl* name) { return shape.names.contains(name); }))
            continue;
        bool isBornInModule = module && shape.modules.contains(module);
        if (isBornInModule && !areBornInModule) {
            areBornInModule = true;
            result.clear();
        }
        if (isBornInModule == areBornInModule)
            result.append(index);
    }
    return result;
}

Vector<uint32_t, 8> PropertyPlaces::candidateShapes(const CalleeHints* module, const NamesAccessed& accessed, bool& usesNamesOnVariable) const
{
    if (accessed.variableReadFrom.first) {
        auto it = m_namesOnVariables.find(accessed.variableReadFrom);
        if (it != m_namesOnVariables.end() && !it->value.areTooMany) {
            auto shapes = shapesWithAllOf(module, accessed, &it->value);
            if (!shapes.isEmpty()) {
                usesNamesOnVariable = true;
                return shapes;
            }
        }
    }
    return shapesWithAllOf(module, accessed, nullptr);
}

auto PropertyPlaces::decide(const CalleeHints* module, UniquedStringImpl* name, const NamesAccessed& accessed, GuessedPlace& place, bool& usesNamesOnVariable) const -> Decision
{
    Decision decision = [&] {
        auto shapes = candidateShapes(module, accessed, usesNamesOnVariable);
        size_t slot = notFound;
        for (uint32_t shape : shapes) {
            size_t slotInShape = m_shapes[shape].names.find(name);
            if (slotInShape == notFound)
                return Decision::NoShape;
            if (slot != notFound && slot != slotInShape)
                return Decision::Disagree;
            slot = slotInShape;
        }
        if (slot == notFound)
            return Decision::NoShape;
        if (slot >= Structure::numberOfSlotsWithPropertyNameIDs)
            return Decision::SlotTooHigh;
        place = { nameID(name), static_cast<uint8_t>(slot), static_cast<uint8_t>(std::min<size_t>(shapes.size(), 255)), 0 };
        if (!place.nameID)
            return Decision::NoNameID;
        if (usesNamesOnVariable)
            m_guessesByNamesOnVariable.fetch_add(1, std::memory_order_relaxed);
        if (shapes.size() != 1)
            return Decision::Guessed;
        m_guessesFromOneShape.fetch_add(1, std::memory_order_relaxed);
        if (const Shape& shape = m_shapes[shapes[0]]; !accessed.names.isEmpty() && slot < shape.numberOfNamesGivenAtOnce)
            place.family = shape.family;
        if (place.family)
            m_guessesWithFamily.fetch_add(1, std::memory_order_relaxed);
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
    out.print("; families: ", m_shapeOfFamily.size(), ", places guessed with a family: ", m_guessesWithFamily.load(std::memory_order_relaxed));
    out.print("; variables with names: ", m_namesOnVariables.size(), ", places guessed with the names on a variable: ", m_guessesByNamesOnVariable.load(std::memory_order_relaxed));
    if (Options::useAOTGuardsOverWholeFunctions())
        out.print("; functions with guards over the whole function: ", m_functionsWithGuards.load(std::memory_order_relaxed), " with ", m_guardsOverWholeFunctions.load(std::memory_order_relaxed), " guards, ", m_bytecodeSizeWithGuards.load(std::memory_order_relaxed), " bytes of bytecode, ", m_codeSizeWithGuards.load(std::memory_order_relaxed), " bytes of code");
}

static bool canHavePlace(VM& vm, UniquedStringImpl* name)
{
    if (name->isSymbol() || parseIndex(*name))
        return false;
    auto& names = *vm.propertyNames;
    for (const Identifier* nameOnObjectPrototype : { &names.constructor, &names.hasOwnProperty, &names.isPrototypeOf, &names.propertyIsEnumerable, &names.toLocaleString, &names.toString, &names.valueOf, &names.underscoreProto, &names.__defineGetter__, &names.__defineSetter__, &names.__lookupGetter__, &names.__lookupSetter__ }) {
        if (name == nameOnObjectPrototype->impl())
            return false;
    }
    return true;
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

static bool isKeptForLoopOverIterator(const Node* value, unsigned numberOfPhisPassed = 0)
{
    if (value->kind == NodeKind::Proj || value->kind == NodeKind::GetStack)
        return true;
    static constexpr unsigned maxNumberOfPhisPassed = 4;
    if (value->kind != NodeKind::Phi || numberOfPhisPassed == maxNumberOfPhisPassed)
        return false;
    return std::ranges::all_of(value->uses, [&](auto& use) {
        return use.node == value || isKeptForLoopOverIterator(valueAccessedThrough(use.node), numberOfPhisPassed + 1);
    });
}

template<typename Functor>
static void forEachAccessByName(VM& vm, const Vector<BasicBlock*>& blocks, const Functor& functor)
{
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
    for (BasicBlock* block : blocks) {
        for (Node* phi : block->phis)
            noteUsesOf(phi);
        for (Node* node : block->nodes)
            noteUsesOf(node);
    }
    UniquedStringImpl* nameReadToCloseIterators = vm.propertyNames->returnKeyword.impl();
    for (BasicBlock* block : blocks) {
        for (Node* node : block->nodes) {
            bool isRead = node->isBytecode(op_get_by_id);
            if (!isRead && !node->isBytecode(op_put_by_id))
                continue;
            UniquedStringImpl* name = node->graph->codeBlock()->identifier(isRead ? node->as<OpGetById>().m_property : node->as<OpPutById>().m_property).impl();
            const Node* value = valueAccessedThrough(node->use(isRead ? node->as<OpGetById>().m_base : node->as<OpPutById>().m_base));
            if (isRead && name == nameReadToCloseIterators && isKeptForLoopOverIterator(value))
                continue;
            functor(value, name, isRead && !readsUsedOtherThanAsCallee.contains(node));
        }
    }
}

static PropertyPlaces::VariableKey variableReadBy(const Node* value)
{
    if (!value->isBytecode(op_get_from_scope))
        return { nullptr, 0 };
    Variable variable = value->graph->variableAccessedBy(value);
    return { variable.scope, variable.offset };
}

static void addNameAccessed(PropertyPlaces::NamesAccessed& accessed, UniquedStringImpl* name, bool isOnlyCalled)
{
    if (isOnlyCalled) {
        if (!accessed.namesOnlyCalled.contains(name))
            accessed.namesOnlyCalled.append(name);
    } else if (!accessed.names.contains(name))
        accessed.names.append(name);
    ++accessed.numberOfSites;
}

void Graph::noteBirths()
{
    PropertyPlaces* places = m_propertyPlaces;
    if (!places)
        return;
    Vector<PropertyPlaces::NamesAccessed> namesAccessed;
    UncheckedKeyHashMap<const Node*, unsigned> indexOfValueAccessed;
    forEachAccessByName(m_vm, m_rpo, [&](const Node* value, UniquedStringImpl* name, bool isOnlyCalled) {
        unsigned index = indexOfValueAccessed.add(value, static_cast<unsigned>(namesAccessed.size())).iterator->value;
        if (index == namesAccessed.size()) {
            namesAccessed.append(PropertyPlaces::NamesAccessed { });
            namesAccessed.last().variableReadFrom = variableReadBy(value);
        }
        addNameAccessed(namesAccessed[index], name, isOnlyCalled);
    });
    places->noteNamesAccessed(m_hints, WTF::move(namesAccessed));
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
        unsigned bytecodeOffset { 0 };
        unsigned numberOfNamesGivenAtOnce { 0 };
        bool isComplete { false };
    };
    Vector<Literal> literals;
    unsigned numberOfNamesGivenToThisAtOnce = 0;
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
                auto shape = literalShape(node);
                if (shape && (shape->layoutID || !shape->slots.isEmpty()))
                    continue;
                literalMadeBy.add(node, static_cast<unsigned>(literals.size()));
                literals.append(Literal { });
                auto& stores = literalStores(node->bytecodeIndex.offset());
                for (unsigned i = 0; i < node->numberOfLiteralProperties; ++i)
                    appendNameGivenAtBirth(m_vm, m_codeBlock->identifier(m_codeBlock->instructions().at(stores[i])->as<OpPutById>().m_property).impl(), literals.last().names);
                literals.last().bytecodeOffset = node->bytecodeIndex.offset();
                if (shape)
                    literals.last().numberOfNamesGivenAtOnce = static_cast<unsigned>(literals.last().names.size());
                continue;
            }
            if (node->isBytecode(op_create_this)) {
                numberOfNamesGivenToThisAtOnce = node->numberOfLiteralProperties;
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
            places->noteConstruction(m_hints, constructor, isDerived ? definition.parentConstructor : nullptr, WTF::move(stored->names), stored->areAll, 0);
    }
    for (auto& literal : literals)
        places->noteLiteral(m_hints, m_codeBlock, literal.bytecodeOffset, WTF::move(literal.names), literal.numberOfNamesGivenAtOnce);
    if (m_codeBlock->isConstructor() && m_codeBlock->constructorKind() != ConstructorKind::Extends) {
        if (auto stored = namesStoredOnThis(m_vm, m_codeBlock, nullptr))
            places->noteConstruction(m_hints, m_codeBlock, nullptr, WTF::move(stored->names), stored->areAll, numberOfNamesGivenToThisAtOnce);
    }
}

void Graph::findNamesAccessed() const
{
    const PropertyPlaces& places = *m_propertyPlaces;
    forEachAccessByName(m_vm, m_rpo, [&](const Node* value, UniquedStringImpl* name, bool isOnlyCalled) {
        if (isOnlyCalled && !places.isHeld(name)) {
            places.countNameOnlyCalled();
            return;
        }
        auto result = m_namesAccessedOn.add(value, PropertyPlaces::NamesAccessed { });
        if (result.isNewEntry)
            result.iterator->value.variableReadFrom = variableReadBy(value);
        addNameAccessed(result.iterator->value, name, isOnlyCalled);
    });
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
    PropertyPlaces::NamesAccessed noNames;
    bool usesNamesOnVariable = false;
    auto decision = places->decide(access->graph->calleeHints(), name, it == m_namesAccessedOn.end() ? noNames : it->value, place, usesNamesOnVariable);
    static constexpr ASCIILiteral reasons[PropertyPlaces::numberOfDecisions] = { ""_s, "no-shape"_s, "disagree"_s, "slot-too-high"_s, "no-name-id"_s };
    if (decision != PropertyPlaces::Decision::Guessed) {
        m_outermost->remark("no-guess"_s, reasons[static_cast<unsigned>(decision)]);
        return std::nullopt;
    }
    m_outermost->remark("guessed-place"_s, StringView { name });
    if (place.numberOfShapes == 1)
        m_outermost->remark("guessed-place-of-one-shape"_s, StringView { name });
    if (usesNamesOnVariable)
        m_outermost->remark("guessed-place-by-names-on-variable"_s, StringView { name });
    if (place.family)
        m_outermost->remark("guessed-family"_s, makeString(static_cast<unsigned>(place.family), ':', StringView { name }));
    return place;
}

uint16_t Graph::familyBornAt(const Node* birth) const
{
    const PropertyPlaces* places = m_outermost->m_propertyPlaces;
    if (!Options::useAOTGuessedPlaces() || !places)
        return 0;
    if (birth->isBytecode(op_new_object))
        return places->familyOfLiteral(birth->graph->codeBlock(), birth->bytecodeIndex.offset());
    RELEASE_ASSERT(birth->isBytecode(op_create_this));
    return places->familyOfInstances(birth->graph->codeBlock());
}

uint16_t Graph::familyGivenAt(const Node* birth, std::span<UniquedStringImpl* const> namesGivenAtOnce) const
{
    uint16_t family = familyBornAt(birth);
    if (!family)
        return 0;
    auto names = m_outermost->m_propertyPlaces->namesOfFamily(family);
    bool givesEveryName = names.size() <= namesGivenAtOnce.size();
    for (size_t slot = 0; givesEveryName && slot < names.size(); ++slot)
        givesEveryName = !names[slot] || names[slot] == namesGivenAtOnce[slot];
    m_outermost->remark(givesEveryName ? "born-in-family"_s : "born-without-names-of-family"_s, String::number(family));
    return givesEveryName ? family : 0;
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
    if (m_codeBlock->numberOfExceptionHandlers() && !Options::useAOTGuardsOverWholeFunctionsWithHandlers())
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
    if (static_cast<uint64_t>(places.size()) * 1000 < static_cast<uint64_t>(Options::minimumAOTGuardsPerThousandBytesOverWholeFunction()) * m_codeBlock->instructions().size())
        return refuse("too-few-places-for-its-size"_s);
    Vector<std::pair<unsigned, unsigned>, 4> loopsWithoutCalls;
    for (const auto& instruction : m_codeBlock->instructions()) {
        if (!isBranch(instruction->opcodeID()))
            continue;
        extractStoredJumpTargetsForInstruction(m_codeBlock, instruction, [&](int32_t relativeOffset) {
            if (relativeOffset >= 0)
                return;
            unsigned header = instruction.offset() + relativeOffset;
            if (!blockForOffset[header] || !blockForOffset[header]->isInProfitableLoop)
                return;
            size_t index = loopsWithoutCalls.findIf([&](auto& loop) { return loop.first == header; });
            if (index == notFound)
                loopsWithoutCalls.append({ header, instruction.offset() });
            else
                loopsWithoutCalls[index].second = instruction.offset();
        });
    }
    for (auto [header, last] : loopsWithoutCalls) {
        bool holdsPlace = false;
        for (unsigned offset : places.keys())
            holdsPlace |= offset >= header && offset <= last;
        if (!holdsPlace)
            return refuse("has-loop-without-places"_s);
    }
    return places;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

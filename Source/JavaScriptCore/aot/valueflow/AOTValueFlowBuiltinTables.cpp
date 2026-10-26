/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBuiltinTables.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueIdentity.h"
#include "AOTValueSets.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr Protocols protocolsWithoutHub { Protocol::OwnKeys, Protocol::PrototypeChain, Protocol::Species };

static void appendKindsOfPrimitives(const BuiltinType& type, bool withNumbers, auto& members)
{
    static constexpr std::pair<BuiltinPrimitive, PrimitiveSet> kinds[] {
        { BuiltinPrimitive::Undefined, PrimitiveSet::Undefined }, { BuiltinPrimitive::Null, PrimitiveSet::Null }, { BuiltinPrimitive::Boolean, PrimitiveSet::False }, { BuiltinPrimitive::Boolean, PrimitiveSet::True },
        { BuiltinPrimitive::Number, PrimitiveSet::Integer }, { BuiltinPrimitive::Number, PrimitiveSet::WholeNumber }, { BuiltinPrimitive::Number, PrimitiveSet::FractionalNumber }, { BuiltinPrimitive::BigInt, PrimitiveSet::BigInt },
    };
    for (auto [listed, kind] : kinds) {
        if (type.primitives.contains(listed) && (withNumbers || listed != BuiltinPrimitive::Number))
            members.append(ValueFlowUniverse::primitive(kind));
    }
}

static ValueSetID maskOf(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker, const BuiltinType& type)
{
    if (type.isAnything())
        return ValueSets::anything;

    Vector<ValueFlowObjectID, 32> members;
    Vector<ValueSetID, 8> parts;
    appendKindsOfPrimitives(type, true, members);
    if (type.primitives.contains(BuiltinPrimitive::String))
        parts.append(universe.strings());
    else if (!type.stringLiterals.empty()) {
        members.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
        for (BuiltinName literal : type.stringLiterals)
            members.append(universe.keyConstant(program.keyOf(literal)));
    }
    if (type.primitives.contains(BuiltinPrimitive::Symbol))
        parts.append(universe.symbols());

    if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData }))
        parts.append(universe.objects());
    else {
        if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction))
            parts.append(universe.callables());
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Instance:
                parts.append(universe.instancesOf(part.builtinClass));
                break;
            case BuiltinTypePartKind::Layout:
                parts.append(universe.objectsWith(program.layoutOf(part.layout)));
                parts.append(universe.objectsThatMayBeAnything());
                break;
            case BuiltinTypePartKind::Builtin:
                universe.appendObjectsOf(part.builtin, members);
                members.append(ValueFlowUniverse::unknownObject());
                members.append(ValueFlowUniverse::untrackedObject());
                break;
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
                parts.append(universe.objects());
                break;
            case BuiltinTypePartKind::Operand:
            case BuiltinTypePartKind::Slot:
            case BuiltinTypePartKind::IteratedValues:
            case BuiltinTypePartKind::CallbackResult:
            case BuiltinTypePartKind::Prototype:
                return ValueSets::anything;
            }
        }
    }

    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

static ValueSetID membersSurelyOf(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker, const BuiltinType& type)
{
    if (type.isAnything())
        return ValueSets::anything;

    Vector<ValueFlowObjectID, 16> members;
    Vector<ValueSetID, 8> parts;
    appendKindsOfPrimitives(type, !type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) && type.integers.isEmpty(), members);
    if (type.primitives.contains(BuiltinPrimitive::String))
        parts.append(universe.strings());
    else {
        for (BuiltinName literal : type.stringLiterals)
            members.append(universe.keyConstant(program.keyOf(literal)));
    }
    if (type.primitives.contains(BuiltinPrimitive::Symbol))
        parts.append(universe.symbols());

    if (type.flags.contains(BuiltinTypeFlag::HasAnyObject))
        parts.append(universe.objects());
    else {
        if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction)) {
            parts.append(universe.closures());
            parts.append(universe.builtinFunctions());
        }
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Instance:
                if (part.pristine.isEmpty() && part.contents.empty())
                    parts.append(universe.sureInstancesOf(part.builtinClass));
                break;
            case BuiltinTypePartKind::Builtin:
                members.append(universe.objectOf(part.builtin));
                break;
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            case BuiltinTypePartKind::Layout:
            case BuiltinTypePartKind::Operand:
            case BuiltinTypePartKind::Slot:
            case BuiltinTypePartKind::IteratedValues:
            case BuiltinTypePartKind::CallbackResult:
            case BuiltinTypePartKind::Prototype:
                break;
            }
        }
    }

    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

ValueFlowBuiltinTables::ValueFlowBuiltinTables(const BuiltinTable& builtins, uint64_t mostObjectsOfTypedResult)
    : m_mostObjectsOfTypedResult(mostObjectsOfTypedResult)
{
    m_classesAtOrBelow.grow(builtins.classCount());
    for (uint32_t index = 0; index < builtins.classCount(); ++index) {
        for (BuiltinClassID current(index); current; current = builtins.at(current).parent)
            m_classesAtOrBelow[current.index()].append(BuiltinClassID(index));
    }

    auto intern = [&](const ValueFlowBehavior& behavior) {
        auto found = std::ranges::find_if(m_behaviors, [&](const ValueFlowBehavior& other) {
            return other.protocols == behavior.protocols && other.isTransitive == behavior.isTransitive
                && std::ranges::equal(other.namedMembers, behavior.namedMembers) && std::ranges::equal(other.calledMembers, behavior.calledMembers);
        });
        if (found == m_behaviors.end()) {
            m_behaviors.append(behavior);
            return ValueFlowBehaviorID(m_behaviors.size() - 1);
        }
        return ValueFlowBehaviorID(static_cast<uint32_t>(found - m_behaviors.begin()));
    };
    m_conversionToPrimitive = intern({ Protocol::ToPrimitive, false, { }, { } });
    auto behaviorFor = [&](OperandUses uses, Protocols declaredProtocols, std::span<const BuiltinName> namedMembers, std::span<const BuiltinName> calledMembers) -> ValueFlowBehaviorID {
        Protocols protocols = declaredProtocols - protocolsWithoutHub;
        if (uses == OperandUses::all() || protocols.isEmpty())
            return { };
        bool isTransitive = uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents });
        if (isTransitive && declaredProtocols.containsAll({ Protocol::OwnValues, Protocol::PrototypeChain }))
            protocols.add(Protocol::PrototypeChain);
        return intern({ protocols, isTransitive, namedMembers, calledMembers });
    };
    auto add = [&](const BuiltinParameter& parameter) {
        if (ValueFlowBehaviorID behavior = behaviorFor(parameter.uses, parameter.protocols, parameter.namedMembers, parameter.calledMembers))
            m_behaviorOfParameter.add(&parameter, behavior);
    };
    for (uint32_t index = 0; index < builtins.overloadCount(); ++index) {
        const BuiltinOverload& overload = builtins.at(BuiltinOverloadID(index));
        if (overload.receiver)
            add(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            add(parameter);
        if (overload.hasTypedResult())
            m_functionsWithTypedResults.set(builtins.canonicalID(overload.builtin).index());
    }
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (!m_functionsWithTypedResults.get(builtins.canonicalID(BuiltinID(index)).index()))
            continue;
        m_functionsWithTypedResults.set(index);
        if (BuiltinName name = builtins.at(BuiltinID(index)).name; !m_namesOfFunctionsWithTypedResults.contains(name))
            m_namesOfFunctionsWithTypedResults.append(name);
    }

    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operand = 0; operand < numberOfDeclaredOperands; ++operand) {
            const OperandInfo& info = operandInfoOf(opcode, operand);
            if (ValueFlowBehaviorID behavior = behaviorFor(info.uses, info.protocols, info.namedMembers, { }))
                m_behaviorOfOperand.add(&info, behavior);
        }
    }
}

static bool usesPutValueInto(const BuiltinTable& builtins, const BuiltinHostUses& uses, BuiltinID entry)
{
    return std::ranges::any_of(uses.list, [&](const BuiltinHostUse& use) {
        if (use.use == BuiltinHostReadUse::IsValueOf)
            return builtins.canonicalID(use.builtin) == entry;
        return use.use == BuiltinHostReadUse::Called && use.call && usesPutValueInto(builtins, use.call->result, entry);
    });
}

static FunctionID functionExportedUnder(const Program& program, ModuleID module, BuiltinName name)
{
    const Function& body = program.at(program.at(module).body);
    PropertyKeyID key = program.keyOf(name);
    FunctionID exported;
    unsigned numberOfStores = 0;
    auto take = [&](ValueID stored) {
        ++numberOfStores;
        const Value& definition = body.at(sameObjectThroughCasts(body, stored));
        if (definition.opcode == Opcode::NewClosure)
            exported = definition.function();
        if (definition.opcode != Opcode::GetModuleVariable)
            return;
        const Variable& variable = program.at(definition.variable());
        if (variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable))
            exported = variable.function;
    };
    for (const Block& block : body.blocks) {
        for (ValueID id : block.values) {
            const Value& value = body.at(id);
            if (value.opcode == Opcode::PropDefine && value.key() == key)
                take(value.operand(OperandIndex::PropDefine::value));
            if (value.opcode != Opcode::NewObject)
                continue;
            std::span<const uint32_t> fields = value.fields();
            for (size_t index = 0; index < fields.size(); ++index) {
                if (program.at(value.layout()).fields[fields[index]].key == key)
                    take(value.operand(OperandIndex::NewObject::values + index));
            }
        }
    }
    return key && numberOfStores == 1 ? exported : FunctionID();
}

BuiltinID ValueFlowBuiltinTables::entryDescribing(ValueFlowObjectID closure) const
{
    for (auto [described, entry] : m_describedClosures) {
        if (described == closure)
            return entry;
    }
    return { };
}

static BitVector findCodeThatRecordsDescribe(const Program& program)
{
    enum class NamedFunctions : uint8_t { None, AllDescribed, SomeNotDescribed };
    Vector<NamedFunctions> namedFunctionsOfModule(FillWith { }, program.modules.size(), NamedFunctions::None);
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        ModuleID module = entry.function ? program.at(entry.function).module : ModuleID();
        if (!module)
            continue;
        NamedFunctions& named = namedFunctionsOfModule[module.index()];
        named = named != NamedFunctions::SomeNotDescribed && standsBehindDescribedBuiltins(program, entry.function) ? NamedFunctions::AllDescribed : NamedFunctions::SomeNotDescribed;
    }

    BitVector isExported(program.functions.size());
    for (const Module& module : program.modules) {
        for (const ModuleExport& moduleExport : module.exports) {
            if (FunctionID function = moduleExport.variable ? program.at(moduleExport.variable).function : FunctionID())
                isExported.quickSet(function.index());
        }
    }

    BitVector code(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        bool isNamedOnlyInItsFile = function.role == FunctionRole::Library && !function.enclosingRuntimeFunction && function.module && !isExported.quickGet(index)
            && function.kind != FunctionKind::Module && function.kind != FunctionKind::CommonJsModule;
        if (standsBehindDescribedBuiltins(program, FunctionID(index)) || (isNamedOnlyInItsFile && namedFunctionsOfModule[function.module.index()] == NamedFunctions::AllDescribed))
            code.quickSet(index);
    }
    return code;
}

void ValueFlowBuiltinTables::makeSets(const Program& program, const ValueFlowUniverse& universe, ValueSets& sets, const ParallelLoops& loops, NOESCAPE const ScopedLambda<void()>& runBesideSets)
{
    ValueSetMaker maker(sets);
    const BuiltinTable& builtins = program.builtins();
    m_codeThatRecordsDescribe = findCodeThatRecordsDescribe(program);
    Vector<ValueFlowObjectID, 8> describedClosures;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID entry(index);
        if (builtins.canonicalID(entry) != entry)
            continue;
        for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(entry)) {
            ModuleID module = load.condition ? ModuleID() : moduleOf(program, load.module);
            for (size_t position = 0; module && position < load.usesOfExports.size(); ++position) {
                bool isValue = std::ranges::any_of(load.usesOfExports[position].list, [&](const BuiltinHostUse& use) {
                    return use.use == BuiltinHostReadUse::IsValueOf && use.memberPath.empty() && builtins.canonicalID(use.builtin) == entry;
                });
                bool isDescribed = std::ranges::all_of(builtins.overloadsOf(entry), [&](const BuiltinOverload& overload) {
                    return doRecordsDescribe(builtins, overload) && builtins.at(overload.result.type).parts.empty();
                });
                FunctionID function = isValue && isDescribed ? functionExportedUnder(program, module, load.exports[position]) : FunctionID();
                if (!function)
                    continue;
                m_describedClosures.append({ universe.closureOf(function), entry });
                describedClosures.append(universe.closureOf(function));
            }
        }

        bool hasValue = false;
        bool hasEveryValue = true;
        for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(entry)) {
            if (!usesPutValueInto(builtins, called.uses, entry))
                continue;
            const RuntimeFunctionEntry* function = findRuntimeFunction(program, called.function);
            hasValue = true;
            hasEveryValue &= !called.condition && function && function->function;
        }
        if (hasValue && hasEveryValue)
            m_entriesHoldingValueFromLibrary.set(index);
    }
    m_closuresThatEntriesDescribe = maker.of(describedClosures.span());

    HashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> classesWithPrototype;
    for (uint32_t index = 0; index < builtins.classCount(); ++index) {
        if (BuiltinID prototype = builtins.at(BuiltinClassID(index)).prototype)
            ++classesWithPrototype.add(builtins.canonicalID(prototype).index(), 0).iterator->value;
    }
    m_objectStandingForClass = Vector<ValueFlowObjectID>(builtins.classCount(), [&](size_t index) {
        BuiltinID prototype = builtins.at(BuiltinClassID(static_cast<uint32_t>(index))).prototype;
        return prototype && classesWithPrototype.get(builtins.canonicalID(prototype).index()) == 1 ? universe.objectOf(prototype) : ValueFlowObjectID();
    });
    auto standingForClassAndThoseBelow = [&](BuiltinClassID builtinClass) {
        Vector<ValueFlowObjectID, 8> standing;
        for (BuiltinClassID other : classesAtOrBelow(builtinClass)) {
            if (m_objectStandingForClass[other.index()])
                standing.append(m_objectStandingForClass[other.index()]);
        }
        return maker.of(standing.span());
    };

    for (uint32_t index = 0; builtins.contains(BuiltinTypeID(index)); ++index) {
        m_maskOfType.append(Typed::maskOf(program, universe, maker, builtins.at(BuiltinTypeID(index))));
        m_membersSurelyOfType.append(Typed::membersSurelyOf(program, universe, maker, builtins.at(BuiltinTypeID(index))));

        const BuiltinType& type = builtins.at(BuiltinTypeID(index));
        ValueSetID whilePristine = ValueSets::nothing;
        ValueSetID classesSurelyOfType = ValueSets::nothing;
        ValueSetID classesWhilePristine = ValueSets::nothing;
        for (const BuiltinTypePart& part : type.parts) {
            if (part.kind != BuiltinTypePartKind::Instance || !part.contents.empty())
                continue;
            if (part.pristine.isEmpty())
                classesSurelyOfType = maker.unite(classesSurelyOfType, standingForClassAndThoseBelow(part.builtinClass));
            else if (pristineTestOf(builtins, part.builtinClass) == PristineTest::MayPass) {
                whilePristine = maker.unite(whilePristine, universe.sureInstancesOf(part.builtinClass));
                classesWhilePristine = maker.unite(classesWhilePristine, standingForClassAndThoseBelow(part.builtinClass));
            }
        }
        m_membersOfTypeWhilePristine.append(whilePristine);
        m_classesSurelyOfType.append(classesSurelyOfType);
        m_classesOfTypeWhilePristine.append(classesWhilePristine);

        BuiltinType typeWithoutParts = type;
        typeWithoutParts.parts = { };
        ValueSetID maskWithoutParts = Typed::maskOf(program, universe, maker, typeWithoutParts);
        ValueSetID primitives = maker.intersect(maskWithoutParts, universe.anyPrimitive());
        if (!type.isAnything() && !type.primitives.contains(BuiltinPrimitive::String) && !type.stringLiterals.empty()) {
            ValueSetID literals = maker.intersect(maskWithoutParts, universe.keys());
            primitives = maker.unite(maker.intersect(primitives, ValueSets::complement(maker.of(ValueFlowUniverse::primitive(PrimitiveSet::String)))), literals);
        }
        m_primitivesOfType.append(primitives);
    }

    m_heap = makeUnique<ValueFlowHeapTables>(program, universe, sets, maker, loops, runBesideSets);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

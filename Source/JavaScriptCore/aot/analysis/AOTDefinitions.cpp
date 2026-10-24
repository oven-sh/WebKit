/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDefinitions.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCallGraph.h"
#include "AOTFunctionAnalyses.h"
#include "AOTKnownClosures.h"
#include "AOTValueIdentity.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isCreatedWithHolder(const Program& program, ClassSide side, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    switch (side) {
    case ClassSide::Prototype:
        return nameClass == NameClass::Constructor;
    case ClassSide::Static:
        return nameClass == NameClass::Length || nameClass == NameClass::Prototype || key == program.keyOf(BuiltinNames::name);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool capturesAtMostClassEnvironment(const Program& program, const Function& constructor, const Function& function)
{
    if (function.captures.isEmpty())
        return true;
    if (function.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = function.captures[0].type;
    return captured && captured == constructor.captures[0].type && program.at(captured).environment;
}

static bool canClassRecordCreate(const Program& program, const Function& constructor, const ClassMember& member)
{
    if (!member.key || !member.function || program.isIndexLike(member.key) || isCreatedWithHolder(program, member.side, member.key))
        return false;
    const Function& function = program.at(member.function);
    return function.name && capturesAtMostClassEnvironment(program, constructor, function);
}

static bool isClassEvaluatedOnce(const Program& program, const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.isDerived() && !layout.has(LayoutFlags::IsConstructorFunction) && isEvaluatedOnce(program, layout);
}

BuiltinID builtinParentNamedByClassRecord(const Program& program, const Layout& layout)
{
    if (layout.flow.baseState != BaseState::BuiltinClass)
        return { };
    const BuiltinTable& builtins = program.builtins();
    const BuiltinClass& baseClass = builtins.at(layout.flow.baseClass);
    BuiltinID constructor = baseClass.constructor;
    if (!constructor || !baseClass.prototype)
        return { };
    const Builtin& builtin = builtins.at(constructor);
    bool isFoundAtAnyTime = builtin.origin == BuiltinOrigin::Engine && builtin.scope == BuiltinScope::Realm && BuiltinCellKey::object(constructor).hasCell(builtins);
    return isFoundAtAnyTime ? constructor : BuiltinID();
}

static bool hasOnlyClassesAbove(const Program& program, LayoutID id)
{
    LayoutID current = id;
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (!isClassEvaluatedOnce(program, layout) || !program.at(layout.constructor).captures.isEmpty())
            return false;
        switch (layout.flow.baseState) {
        case BaseState::Layout:
            current = layout.flow.base;
            continue;
        case BaseState::None:
            return !layout.has(LayoutFlags::HasHeritage);
        case BaseState::BuiltinClass:
            return !!builtinParentNamedByClassRecord(program, layout);
        case BaseState::Unknown:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return false;
}

static bool isDefinedByClassRecord(const Program& program, LayoutID id)
{
    if (!hasOnlyClassesAbove(program, id))
        return false;
    const Layout& layout = program.at(id);
    const Function& constructor = program.at(layout.constructor);
    bool recordCreatesEveryMember = std::ranges::all_of(layout.classFlow.members, [&](const ClassMember& member) {
        return canClassRecordCreate(program, constructor, member);
    });
    if (!recordCreatesEveryMember)
        return false;
    bool capturesNothing = true;
    forEachFunctionInClassRecord(program, layout, [&](FunctionID function, FunctionReferenceKind) {
        capturesNothing &= program.at(function).captures.isEmpty();
    });
    return capturesNothing;
}

static bool isOnlyClosureOperand(const Function& function, const UseLists& useLists, ValueID id, FunctionID closureFunction)
{
    const Value& closure = function.at(id);
    return closure.opcode == Opcode::NewClosure && closure.function() == closureFunction && closure.capturesOperands().empty() && !closure.isMadeOnDemand() && useLists.useCount(id) == 1;
}

std::optional<ClassDefinitionSite> findClassDefinitionSite(const Program& program, const Function& function, const UseLists& useLists, ValueID newClass)
{
    const Value& head = function.at(newClass);
    RELEASE_ASSERT(head.opcode == Opcode::NewClass);
    if (!isDefinedByClassRecord(program, head.layout()))
        return std::nullopt;

    const Layout& layout = program.at(head.layout());
    ClassDefinitionSite site { .layout = head.layout(), .classObject = newClass };
    if (!isOnlyClosureOperand(function, useLists, head.constructorOperand(), layout.constructor))
        return std::nullopt;
    site.describedByRecord.append(head.constructorOperand());

    for (ValueID current = head.prototypeParentOperand(); current;) {
        const Value& value = function.at(current);
        bool isCastThatDoesNothing = value.opcode == Opcode::AssumeType || value.opcode == Opcode::Upcast;
        if (value.opcode != Opcode::ResolveHeritage && !isCastThatDoesNothing)
            return std::nullopt;
        unsigned otherUses = 0;
        for (const Use& use : useLists.usesOf(current)) {
            if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateType)
                site.describedByRecord.append(use.user);
            else
                ++otherUses;
        }
        if (otherUses != 1)
            return std::nullopt;
        site.describedByRecord.append(current);
        current = isCastThatDoesNothing ? value.valueOperand() : ValueID();
    }

    const Vector<ClassMember>& members = layout.classFlow.members;
    BitVector isDefinedHere(members.size());
    auto addDefinition = [&](ValueID id) {
        const Value& definition = function.at(id);
        const Value& closure = function.at(definition.functionOperand());
        if (definition.layout() != site.layout || !definition.key() || definition.computedKeyOperand() || closure.opcode != Opcode::NewClosure)
            return false;
        size_t position = members.findIf([&](const ClassMember& member) {
            return member.function == closure.function();
        });
        if (position == notFound)
            return false;
        const ClassMember& member = members[position];
        bool isAsListed = member.key == definition.key() && member.side == definition.side() && member.kind == definition.methodKind();
        if (!isAsListed || isDefinedHere.quickSet(position) || !isOnlyClosureOperand(function, useLists, definition.functionOperand(), member.function))
            return false;
        site.describedByRecord.append(definition.functionOperand());
        site.describedByRecord.append(id);
        return true;
    };

    for (size_t next = 0; next <= site.copies.size(); ++next) {
        ValueID holder = next ? site.copies[next - 1] : newClass;
        for (const Use& use : useLists.usesOf(holder)) {
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = function.at(use.user);
            if (sameValueOperandOf(user) == holder) {
                site.copies.append(use.user);
                continue;
            }
            if (user.opcode == Opcode::PutModuleVariable) {
                site.stores.append(use.user);
                continue;
            }
            if (user.opcode == Opcode::DefineMethod && use.operandIndex == OperandIndex::DefineMethod::classObject && !addDefinition(use.user))
                return std::nullopt;
        }
    }
    if (isDefinedHere.bitCount() != members.size())
        return std::nullopt;
    return site;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

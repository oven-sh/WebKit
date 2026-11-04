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
#include "AOTRowsOfLayouts.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <wtf/HashMap.h>

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
    TypeID captured = function.captures[0].written();
    return captured && captured == constructor.captures[0].written() && program.at(captured).environment;
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

bool hasOnlyClassesAndBuiltins(const Program& program, const Type& type)
{
    if (type.anyObject || type.anyFunction || type.layouts || !type.classes.isEmpty() || (type.primitives & ~Primitive::Null) || !type.isJavaScriptValue())
        return false;
    bool hasOnlyConstructors = std::ranges::all_of(type.builtins, [&](BuiltinID id) {
        const Builtin& builtin = program.builtins().at(id);
        return builtin.kind == BuiltinKind::Constructor && builtin.instanceClass && program.builtins().at(builtin.instanceClass).prototype;
    });
    return hasOnlyConstructors && std::ranges::all_of(type.functions, [&](FunctionID id) {
        FunctionKind kind = program.at(id).kind;
        return kind == FunctionKind::BaseConstructor || kind == FunctionKind::DerivedConstructor;
    });
}

BitVector findClassesWithOnlyClassesAbove(const Program& program)
{
    return answerAlongRows(program.layouts.size(), false, [&](LayoutID id) -> StepAlongRow {
        const Layout& layout = program.at(id);
        if (!isClassEvaluatedOnce(program, layout) || !program.at(layout.constructor).captures.isEmpty())
            return { false, { } };
        switch (layout.flow.baseState) {
        case BaseState::Layout:
            return { std::nullopt, layout.flow.base };
        case BaseState::None:
            return { !layout.has(LayoutFlags::HasHeritage), { } };
        case BaseState::BuiltinClass:
            return { !!builtinParentNamedByClassRecord(program, layout), { } };
        case BaseState::Unknown:
            return { false, { } };
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool isDefinedByClassRecord(const Program& program, const BitVector& classesWithOnlyClassesAbove, LayoutID id)
{
    if (!classesWithOnlyClassesAbove.quickGet(id.index()))
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

std::optional<ClassDefinitionSite> findClassDefinitionSite(const Program& program, const BitVector& classesWithOnlyClassesAbove, const Function& function, const UseLists& useLists, ValueID newClass)
{
    const Value& head = function.at(newClass);
    RELEASE_ASSERT(head.opcode == Opcode::NewClass);
    if (!isDefinedByClassRecord(program, classesWithOnlyClassesAbove, head.layout()))
        return std::nullopt;

    const Layout& layout = program.at(head.layout());
    ClassDefinitionSite site { .layout = head.layout(), .classObject = newClass };
    if (!isOnlyClosureOperand(function, useLists, head.constructorOperand(), layout.constructor))
        return std::nullopt;
    site.describedByRecord.append(head.constructorOperand());

    for (ValueID current = head.prototypeParentOperand(); current;) {
        const Value& value = function.at(current);
        bool isCastThatDoesNothing = value.opcode == Opcode::AssumeType || isCastThatProvesNothing(value.opcode);
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
        if (value.opcode == Opcode::ResolveHeritage)
            site.heritageLook = current;
        else
            site.describedByRecord.append(current);
        current = isCastThatDoesNothing ? value.valueOperand() : ValueID();
    }

    const Vector<ClassMember>& members = layout.classFlow.members;
    BitVector isDefinedHere(members.size());
    HashMap<FunctionID, size_t> firstPositionOfFunction;
    for (size_t position = 0; position < members.size(); ++position)
        firstPositionOfFunction.add(members[position].function, position);
    auto addDefinition = [&](ValueID id) {
        const Value& definition = function.at(id);
        const Value& closure = function.at(definition.functionOperand());
        if (definition.layout() != site.layout || !definition.key() || definition.computedKeyOperand() || closure.opcode != Opcode::NewClosure)
            return false;
        auto found = firstPositionOfFunction.find(closure.function());
        if (found == firstPositionOfFunction.end())
            return false;
        size_t position = found->value;
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

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTTypeRelations.h"

namespace JSC { namespace AOT { namespace Typed {

bool CalleeSummarizerForLinkEffects::indexesAreAbsentAboveInstancesOf(BuiltinClassID builtinClass) const
{
    BuiltinID holder;
    for (BuiltinClassID current = builtinClass; current && !holder; current = m_builtins.at(current).parent)
        holder = m_builtins.at(current).prototype;
    if (!holder)
        return false;
    holder = m_builtins.canonicalID(holder);
    while (true) {
        if (!m_builtins.closesIndexKeys(holder) || !m_builtins.hasLockedPrototypeLink(holder))
            return false;
        const Builtin& object = m_builtins.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = m_builtins.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

bool CalleeSummarizerForLinkEffects::instancesAreAsClassMakesThem(const LinkType& instances, PristineAspectSet aspects) const
{
    if (aspects.contains(PristineAspects::Storage) && !indexesAreAbsentAboveInstancesOf(instances.builtinClass))
        return false;
    if (m_flow.hasEscaped(instances.site))
        return false;
    if (m_flow.program.admitsReplacedBuiltins) {
        const Program& program = m_flow.program;
        BuiltinID holder;
        for (BuiltinClassID current = instances.builtinClass; current && !holder; current = m_builtins.at(current).parent)
            holder = m_builtins.at(current).prototype;
        while (holder) {
            holder = m_builtins.canonicalID(holder);
            for (BuiltinID member : m_builtins.membersOf(holder)) {
                if (program.isUnlocked(member) || program.isUnlocked(m_builtins.canonicalID(member)))
                    return false;
                if (!program.isLockLeftToProgram(member))
                    continue;
                FlowNodeID written = m_flow.findWrittenAtMember(member);
                if (!written || !m_flow.typeAt(written).isNever())
                    return false;
            }
            const Builtin& object = m_builtins.at(holder);
            holder = object.prototypeLink == BuiltinPrototypeLink::Builtin ? object.inheritsFrom : BuiltinID();
        }
    }
    FlowNodeID underUnknownNames = m_flow.findContentsOf(instances, SlotOfContents::underUnknownNames());
    return !underUnknownNames || (underUnknownNames != m_flow.unseen() && m_flow.typeAt(underUnknownNames).isNever());
}

bool CalleeSummarizerForLinkEffects::surelyTakes(const BuiltinType& accepted, const LinkType& type) const
{
    if (accepted.isAnything())
        return true;
    if (type.isNever())
        return false;

    if (type.primitives) {
        uint16_t surelyTaken = primitivesOfBuiltinType(accepted);
        if (surelyTaken & Primitive::String)
            surelyTaken |= Primitive::LiteralString;
        if (surelyTaken & Primitive::Symbol)
            surelyTaken |= Primitive::LiteralSymbol;
        if (accepted.flags.contains(BuiltinTypeFlag::HasOnlyIntegers))
            surelyTaken &= ~Primitive::Number;
        if (!accepted.primitives.contains(BuiltinPrimitive::Undefined))
            surelyTaken &= ~Primitive::Undefined;
        if (type.primitives & ~LinkType::closed(surelyTaken))
            return false;
    }

    bool takesAnyObject = accepted.flags.contains(BuiltinTypeFlag::HasAnyObject);
    bool takesAnyFunction = takesAnyObject || accepted.flags.contains(BuiltinTypeFlag::HasAnyFunction);
    if ((type.objects != LinkType::Objects::None || type.hasUnknownObjects) && !takesAnyObject)
        return false;
    if (type.functions != LinkType::Functions::None && !takesAnyFunction)
        return false;
    if (type.builtin && !takesAnyObject) {
        bool isNamed = std::ranges::any_of(accepted.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Builtin && m_builtins.canonicalID(part.builtin) == m_builtins.canonicalID(type.builtin);
        });
        if (!isNamed)
            return false;
    }

    switch (type.classes) {
    case LinkType::Classes::None:
        return true;
    case LinkType::Classes::Mixed:
        return takesAnyObject;
    case LinkType::Classes::Site:
        break;
    }
    BuiltinClassID builtinClass = type.builtinClass;
    if (takesAnyObject || (takesAnyFunction && isFunctionClass(m_builtins, builtinClass)))
        return true;
    return std::ranges::any_of(accepted.parts, [&](const BuiltinTypePart& part) {
        if (part.kind != BuiltinTypePartKind::Instance || !m_builtins.isSameOrSubclassOf(builtinClass, part.builtinClass))
            return false;
        bool isOfClassTaken = !part.isOfClassItself || part.builtinClass == builtinClass;
        return isOfClassTaken && part.contents.empty() && (part.pristine.isEmpty() || instancesAreAsClassMakesThem(type, part.pristine));
    });
}

bool CalleeSummarizerForLinkEffects::rowSurelyTakesCall(const BuiltinOverload& row, FunctionID id, const ThinSummary& summary, const SummaryCall& call) const
{
    std::span<const uint32_t> arguments = argumentsOf(summary, call);
    auto takes = [&](const BuiltinParameter& parameter) {
        if (!parameter.accepted || parameter.conversion != BuiltinConversion::None)
            return true;
        const BuiltinType& accepted = m_builtins.at(parameter.accepted);
        if (accepted.isAnything())
            return true;

        BuiltinOperand operand = parameter.operand;
        switch (operand.kind()) {
        case BuiltinOperand::Kind::Receiver: {
            if (call.receiver == noSummaryNode)
                return false;
            LinkType receiver = typeAt(id, call.receiver);
            if (call.callee == SummaryCall::Callee::Name)
                receiver.primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent);
            return surelyTakes(accepted, receiver);
        }
        case BuiltinOperand::Kind::Argument:
            if (operand.argumentIndex() < arguments.size())
                return surelyTakes(accepted, typeAt(id, arguments[operand.argumentIndex()]));
            return fitOfOmittedOperandInBuiltinType(accepted) == TypeFit::Inside;
        case BuiltinOperand::Kind::ArgumentsFrom:
            for (size_t index = operand.argumentIndex(); index < arguments.size(); ++index) {
                if (!surelyTakes(accepted, typeAt(id, arguments[index])))
                    return false;
            }
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    if (row.receiver && !takes(*row.receiver))
        return false;
    return std::ranges::all_of(row.parameters, takes);
}

std::optional<bool> CalleeSummarizerForLinkEffects::addEffectsOfRowsUpToOneThatSurelyTakes(BuiltinID builtin, FunctionID id, const ThinSummary& summary, const SummaryCall& call, CodeFound& found, Effects& effects) const
{
    if (call.isConstruction)
        return std::nullopt;
    bool hasRow = false;
    for (const BuiltinOverload& row : m_builtins.overloadsOf(builtin)) {
        if (row.mode != BuiltinCallMode::Call)
            continue;
        hasRow = true;
        if (row.isUndeclared() || !addEffectsOfRow(row, id, summary, call, found, effects))
            return false;
        if (rowSurelyTakesCall(row, id, summary, call))
            return true;
    }
    return hasRow;
}

bool CalleeSummarizerForLinkEffects::copyThatDefinesMayRunCode(FunctionID id, const SummaryChange& change) const
{
    if (change.other == noSummaryNode)
        return true;
    LinkType source = typeAt(id, change.other);
    if (!mayBeObjectForLinkEffects(source))
        return false;
    if (!holdsOnlyPlainObjectsOfNamedLayouts(source))
        return true;
    LinkType target = typeAt(id, change.subject);
    std::span<const LayoutID> layoutsOfTarget = layoutsNamedBy(target);
    bool targetIsOfNamedLayouts = !layoutsOfTarget.empty() && target.functions == LinkType::Functions::None && !mayBeOtherCallableForLinkEffects(target);
    return !targetIsOfNamedLayouts || !std::ranges::all_of(layoutsOfTarget, [&](LayoutID layout) {
        return isFoundByShapeTest(m_program.at(layout));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Members of CalleeSummarizerForLinkEffects, which AOTLinkEffects.cpp declares. That file includes this one at its foot.
// The flow asks the rows of a built-in in their order and ends at the first that surely takes what is passed: CallInLinkCalls::fitOf(), addFit(). Here the same is asked of how the flow has left things,
// so the rows behind that one say nothing of the call: `list.push(x)` on a list that is as its class makes it does what the first row of push says, and the row for whatever is like a list is not asked.
// Only "surely" is ported. A row that cannot take what is passed is asked all the same, which adds to what the call may do and takes nothing away.

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTTypeRelations.h"

namespace JSC { namespace AOT { namespace Typed {

// A read at a hole goes on upward. As CallInLinkCalls::indexesAreAbsentAbove().
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

// As CallInLinkCalls::isAsClassMakesIt(), behind the flow: whatever lays a name on such an object, defines an element by a descriptor or puts another object above it has left something under unknown names.
bool CalleeSummarizerForLinkEffects::instancesAreAsClassMakesThem(const LinkType& instances, PristineAspectSet aspects) const
{
    if (aspects.contains(PristineAspects::Storage) && !indexesAreAbsentAboveInstancesOf(instances.builtinClass))
        return false;
    if (m_flow.hasEscaped(instances.site))
        return false;
    // CallInLinkCalls::membersAboveInstancesAreWhatTableLists(), behind the flow: a member that the flow has gone by has a node, and nothing has arrived there unless a write was followed to the member.
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
                if (!written || !m_flow.typeAt(written).isNothing())
                    return false;
            }
            const Builtin& object = m_builtins.at(holder);
            holder = object.prototypeLink == BuiltinPrototypeLink::Builtin ? object.inheritsFrom : BuiltinID();
        }
    }
    // Nobody has asked for the node: nobody has stored there.
    FlowNodeID underUnknownNames = m_flow.findContentsOf(instances, SlotOfContents::underUnknownNames());
    return !underUnknownNames || (underUnknownNames != m_flow.unseen() && m_flow.typeAt(underUnknownNames).isNothing());
}

// The half of CallInLinkCalls::addFit() that says `isSurelyTaken`, part by part.
bool CalleeSummarizerForLinkEffects::surelyTakes(const BuiltinType& accepted, const LinkType& type) const
{
    if (accepted.isAnything())
        return true;
    // Nobody has followed the value.
    if (type.isNothing())
        return false;

    if (type.primitives) {
        uint16_t surelyTaken = primitivesOfBuiltinType(accepted);
        if (surelyTaken & Primitive::String)
            surelyTaken |= Primitive::LiteralString;
        if (surelyTaken & Primitive::Symbol)
            surelyTaken |= Primitive::LiteralSymbol;
        if (accepted.flags.contains(BuiltinTypeFlag::HasOnlyIntegers))
            surelyTaken &= ~Primitive::Number;
        // Written for a call that leaves the argument out.
        if (!accepted.primitives.contains(BuiltinPrimitive::Undefined))
            surelyTaken &= ~Primitive::Undefined;
        if (type.primitives & ~LinkType::closed(surelyTaken))
            return false;
    }

    bool takesAnyObject = accepted.flags.contains(BuiltinTypeFlag::HasAnyObject);
    bool takesAnyFunction = takesAnyObject || accepted.flags.contains(BuiltinTypeFlag::HasAnyFunction);
    if ((type.objects != LinkType::Objects::None || type.holdsUnseen) && !takesAnyObject)
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

// As CallInLinkCalls::fitOf() says that nothing of what is passed may lie outside. The call passes no list: addEffectsOfRows().
bool CalleeSummarizerForLinkEffects::rowSurelyTakesCall(const BuiltinOverload& row, FunctionID id, const ThinSummary& summary, const SummaryCall& call) const
{
    std::span<const uint32_t> arguments = argumentsOf(summary, call);
    auto takes = [&](const BuiltinParameter& parameter) {
        // `String(object)`: whatever is passed is converted to what the row takes, or the conversion throws. Either way the row has taken it.
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
            // On these the lookup of the name has thrown.
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

// In the place of forEachRowOf() in addEffectsOfRows(). `builtin` is canonical. Nothing: the rows of a construction are asked there, behind a question of their own.
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

// `{ ...state }`: each property is DEFINED on the target, so no setter on it or above it takes a value, whatever the names are and whatever has been put above the target.
// Only a proxy answers a definition with code, and only a proxy or a getter of its own the reading of the source. What lies beside the fields of the source was stored there: an accessor would show as GetsAccessor.
bool CalleeSummarizerForLinkEffects::copyThatDefinesMayRunCode(FunctionID id, const SummaryChange& change) const
{
    if (change.other == noSummaryNode)
        return true;
    LinkType source = typeAt(id, change.other);
    if (!mayBeObjectForLinkEffects(source))
        return false;
    if (!holdsOnlyPlainObjectsOfOneLayout(source))
        return true;
    // Of any other layout native code makes objects too, a proxy among them.
    LinkType target = typeAt(id, change.subject);
    bool targetIsOfOneLayout = target.objects == LinkType::Objects::Layout && target.functions == LinkType::Functions::None && !mayBeOtherCallableForLinkEffects(target);
    return !targetIsOfOneLayout || !isFoundByShapeTest(m_program.at(target.layout));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

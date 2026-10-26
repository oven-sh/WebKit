/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsOfProtocols.h"

#if ENABLE(AOT)

#include "AOTEffectsOfAccesses.h"
#include "AOTIR.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

void CalleeList::add(FunctionID function)
{
    insertInUniquedSortedVector(functions, function);
}

void CalleeList::add(BuiltinID builtin)
{
    insertInUniquedSortedVector(builtins, builtin);
}

void CalleeList::dump(PrintStream& out) const
{
    if (runsNothing()) {
        out.print("none"_s);
        return;
    }
    const BuiltinTable& table = BuiltinTable::singleton();
    CommaPrinter comma;
    for (FunctionID function : functions)
        out.print(comma, "f"_s, function.index());
    for (BuiltinID builtin : builtins)
        out.print(comma, table.tokenOf(builtin));
    if (!isComplete)
        out.print(comma, "?"_s);
}

CalleeList calleesOf(const Program&, const Type& callee)
{
    CalleeList result = CalleeList::none();
    if (!callee.hasObjects())
        return result;
    result.functions.append(callee.functions.span());
    result.builtins.append(callee.builtins.span());
    result.isComplete = callee.hasOnlyKnownCallees() && !callee.layouts;
    return result;
}

CalleeList calleesOf(const Program& program, CalleeSetID id)
{
    if (!id)
        return CalleeList::unknown();
    const CalleeSet& set = program.at(id);
    CalleeList result;
    result.functions.append(set.functions.span());
    result.builtins.append(set.builtins.span());
    result.isComplete = set.isComplete;
    return result;
}

namespace {

enum class FoundValue : uint8_t { IsOnlyRead, IsConverted, IsCalled };

} // anonymous namespace

static bool mayBeMemberWithoutLayout(const Program& program, const Type& type)
{
    return type.layouts && program.at(type.layouts).hasMembersWithoutLayout;
}

static bool hasOnlyLayoutObjects(const Program& program, const Type& type)
{
    return !type.anyObject && !type.anyFunction && type.classes.isEmpty() && type.functions.isEmpty() && type.builtins.isEmpty() && !mayBeMemberWithoutLayout(program, type);
}

static bool mayBeCallable(const Program& program, const Type& type)
{
    return type.mayBeFunction() || mayBeMemberWithoutLayout(program, type) || mayContainAnyCallable(program.builtins(), type);
}

static bool mayReadFromField(const Program& program, const Type& operand, PropertyKeyID key, NOESCAPE const Invocable<bool(const Type&)> auto& predicate)
{
    if (!hasOnlyLayoutObjects(program, operand))
        return true;
    return std::ranges::any_of(membersOf(program, operand), [&](LayoutID member) {
        std::optional<FieldReference> field = findField(program, member, key, BaseChain::Proven);
        if (!field)
            return program.at(member).kind == LayoutKind::ModuleNamespace;
        const DeclaredType& declared = fieldAt(program, *field).declared;
        return !declared.written() || declared.isExpected() || predicate(program.at(slotTypeOf(program, fieldAt(program, *field))));
    });
}

static bool mayFindObject(const Program& program, const Type& operand, const ProvenAccess& read)
{
    if (!read.accessors.isEmpty() || !read.methods.isEmpty() || !read.builtins.isEmpty() || read.mayFindPrototypeProperty)
        return true;
    return !read.operations.isEmpty() && mayReadFromField(program, operand, read.key, [](const Type& type) {
        return type.hasObjects();
    });
}

static bool mayFindUnlistedCallable(const Program& program, const Type& operand, const ProvenAccess& read)
{
    if (!read.accessors.isEmpty() || read.mayFindPrototypeProperty)
        return true;
    return !read.operations.isEmpty() && mayReadFromField(program, operand, read.key, [&](const Type& type) {
        return mayBeCallable(program, type);
    });
}

static void addRead(const Program& program, const Type& operand, const ProvenAccess& read, FoundValue found, CalleeList& result)
{
    if (!read.isComplete) {
        result.isComplete = false;
        return;
    }
    for (FunctionID accessor : read.accessors)
        result.add(accessor);
    switch (found) {
    case FoundValue::IsOnlyRead:
        return;
    case FoundValue::IsConverted:
        if (mayFindObject(program, operand, read))
            result.isComplete = false;
        return;
    case FoundValue::IsCalled:
        break;
    }
    for (FunctionID method : read.methods)
        result.add(method);
    for (BuiltinID builtin : read.builtins)
        result.add(builtin);
    if (mayFindUnlistedCallable(program, operand, read))
        result.isComplete = false;
}

static bool mayBeBoundFunction(const Program& program, const Type& receivers)
{
    return !receivers.classes.isEmpty() || std::ranges::any_of(receivers.functions, [&](FunctionID function) {
        return program.at(function).role == FunctionRole::BoundFunction;
    });
}

static void addHasInstanceMethod(const Program& program, const Type& receivers, ProvenAccess&& read, CalleeList& result)
{
    const BuiltinTable& table = program.builtins();
    if (!mayBeBoundFunction(program, receivers)) {
        read.builtins.removeAllMatching([&](BuiltinID builtin) {
            return table.canonicalID(builtin) == table.canonicalID(Builtins::FunctionPrototypeSymbolHasInstance);
        });
    }
    addRead(program, receivers, read, FoundValue::IsCalled, result);
}

static bool findsNothing(const ProvenAccess& read)
{
    return read.isComplete && !read.isLookupByKey && !read.mayFindPrototypeProperty && read.accessors.isEmpty() && read.methods.isEmpty() && read.builtins.isEmpty() && read.operations.isEmpty();
}

static void addTwoStepRead(const ProvenAccess& read, CalleeList& result)
{
    if (!findsNothing(read))
        result.isComplete = false;
}

static constexpr uint16_t objectCoerciblePrimitives = Primitive::Defined & ~(Primitive::UndefinedOrNull | Primitive::Absent);

static constexpr uint16_t primitiveReceiversOf(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Iterate:
    case Protocol::Named:
    case Protocol::CallsNamed:
        return objectCoerciblePrimitives;
    case Protocol::ToJSON:
        return Primitive::BigInt;
    case Protocol::OwnKeys:
    case Protocol::OwnValues:
    case Protocol::ToPrimitive:
    case Protocol::Thenable:
    case Protocol::Species:
    case Protocol::HasInstance:
    case Protocol::PrototypeChain:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr Protocols declaredProtocols = [] {
    Protocols protocols;
    for (Protocol protocol : allMembersOf<Protocol>)
        protocols.add(protocol);
    return protocols;
}();

static FoundValue valueFoundByNamed(Protocols protocols, NamedMembers namedMemberUse)
{
    switch (namedMemberUse) {
    case NamedMembers::AreOnlyInspected:
        return FoundValue::IsOnlyRead;
    case NamedMembers::AreOnlyRead:
        return protocols.contains(Protocol::ToPrimitive) ? FoundValue::IsConverted : FoundValue::IsOnlyRead;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CalleeList membersRunBy(const Program& program, const Type& operand, Protocols protocols, std::span<const BuiltinName> namedMembers, std::span<const BuiltinName> calledMembers, NamedMembers namedMemberUse)
{
    CalleeList result = CalleeList::none();
    if (!declaredProtocols.containsAll(protocols))
        result.isComplete = false;
    protocols = protocols & declaredProtocols;

    const BuiltinTable& table = program.builtins();
    BuiltinName hasInstanceMethod = table.nameOf(WellKnownSymbol::HasInstance);
    for (Protocol protocol : protocols) {
        Type receivers = operand.withoutPrimitives(static_cast<uint16_t>(~primitiveReceiversOf(protocol)));
        if (!receivers.primitives && !receivers.hasObjects())
            continue;
        auto addReadOf = [&](BuiltinName name, FoundValue found) {
            addRead(program, receivers, provenReadOf(program, receivers, name), found, result);
        };
        switch (protocol) {
        case Protocol::OwnKeys:
        case Protocol::OwnValues:
        case Protocol::PrototypeChain:
            addRead(program, receivers, provenStepOf(program, receivers, protocol), FoundValue::IsOnlyRead, result);
            break;
        case Protocol::ToPrimitive:
        case Protocol::ToJSON:
            for (BuiltinName name : table.keysReadBy(protocol))
                addReadOf(name, FoundValue::IsCalled);
            break;
        case Protocol::HasInstance:
            for (BuiltinName name : table.keysReadBy(protocol)) {
                if (name == hasInstanceMethod)
                    addHasInstanceMethod(program, receivers, provenReadOf(program, receivers, name), result);
                else
                    addReadOf(name, FoundValue::IsOnlyRead);
            }
            break;
        case Protocol::Thenable:
            for (BuiltinName name : table.keysReadBy(protocol))
                addReadOf(name, FoundValue::IsOnlyRead);
            break;
        case Protocol::Iterate:
        case Protocol::Species:
            for (BuiltinName name : table.keysReadBy(protocol))
                addTwoStepRead(provenReadOf(program, receivers, name), result);
            break;
        case Protocol::Named:
            if (namedMembers.empty())
                result.isComplete = false;
            for (BuiltinName name : namedMembers)
                addReadOf(name, valueFoundByNamed(protocols, namedMemberUse));
            break;
        case Protocol::CallsNamed:
            if (calledMembers.empty())
                result.isComplete = false;
            for (BuiltinName name : calledMembers)
                addReadOf(name, FoundValue::IsCalled);
            break;
        }
    }
    return result;
}

static constexpr Protocols protocolsWithoutKey { Protocol::OwnKeys, Protocol::OwnValues, Protocol::PrototypeChain };

static constexpr Effects readsOfMemberLookup = [] {
    Effects effects = Effects::none();
    effects.reads.add(AbstractHeapSet { AbstractHeap::ObjectFields, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks });
    return effects;
}();

Effects readsOfMemberLookups(const Type& operand, Protocols protocols)
{
    if (!operand.hasObjects() || protocolsWithoutKey.containsAll(protocols))
        return Effects::none();
    return readsOfMemberLookup;
}

bool protocolMayFindMember(const Program& program, const Type& operand, Protocol protocol)
{
    Type receivers = operand.withoutPrimitives(static_cast<uint16_t>(~primitiveReceiversOf(protocol)));
    if (!receivers.primitives && !receivers.hasObjects())
        return false;

    std::span<const BuiltinName> keys = program.builtins().keysReadBy(protocol);
    if (keys.empty())
        return true;
    return !std::ranges::all_of(keys, [&](BuiltinName name) {
        return findsNothing(provenReadOf(program, receivers, name));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

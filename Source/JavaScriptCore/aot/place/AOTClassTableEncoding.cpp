/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTFunctionKinds.h"
#include "AOTLayoutData.h"
#include "AOTRepresentation.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) == 1u << MethodData::nameBits);

static uint32_t indexOrNone(FunctionID function)
{
    return function ? function.index() : UINT32_MAX;
}

static MethodData methodDataOf(TableEncoder& encoder, const ClassMember& member, MemberCreation creation)
{
    uint32_t flags = 0;
    if (!member.key)
        flags |= methodHasComputedName;
    if (!encoder.program.at(member.function).captures.isEmpty())
        flags |= methodTakesEnvironment;
    if (creation == MemberCreation::AtDefinition)
        flags |= methodIsCreatedAtDefinition;

    MethodData data { };
    data.name = member.key ? encoder.identifierNumber(member.key) : 0;
    data.kindBits = static_cast<uint32_t>(member.kind);
    data.flags = flags;
    data.function = creation == MemberCreation::ByClassRecord ? encoder.entrypointOf(member.function) : indexOrNone(encoder.findEntrypoint(member.function));
    return data;
}

static void appendMethods(TableEncoder& encoder, LayoutID id, ClassSide side, Vector<MethodData>& methods)
{
    const Vector<ClassMember>& members = encoder.program.at(id).classFlow.members;
    const Vector<MemberCreation>& creations = encoder.slots.classCreation(id).members;
    for (size_t index = 0; index < members.size(); ++index) {
        if (members[index].side != side)
            continue;
        MemberCreation creation = index < creations.size() ? creations[index] : MemberCreation::AtDefinition;
        methods.append(methodDataOf(encoder, members[index], creation));
    }
}

static Vector<FieldData> collectStaticFields(TableEncoder& encoder, const Layout& layout)
{
    const Program& program = encoder.program;
    Vector<FieldData> fields;
    for (const StaticField& field : layout.staticFields) {
        if (!field.key)
            continue;
        if (encoder.placement.variable(field.variable).storage == VariableStorage::None)
            continue;

        TypeID type = program.at(field.variable).declared.type;
        uint16_t primitives = program.at(type ? type : TypeTable::anything).primitives;
        uint16_t flags = 0;
        if (primitives & Primitive::Undefined)
            flags |= fieldAllowsUndefined;

        fields.append(FieldData {
            .name = encoder.identifierNumber(field.key),
            .flags = flags,
            .operandIndex = 0,
            .slot = placedVariableSlot(encoder, field.variable),
            .type = encoder.typeOffset(type),
        });
    }
    return fields;
}

static Vector<MemberGuardData> collectMemberGuards(TableEncoder& encoder, LayoutID id)
{
    Vector<MemberGuardData> guards;
    for (const PlacedGuardBit& placed : encoder.placement.guardBits().ofClass(id)) {
        MemberGuardData guard { };
        guard.name = placed.key ? encoder.identifierNumber(placed.key) : 0;
        guard.kindBits = static_cast<uint32_t>(placed.kind);
        guard.sideBits = static_cast<uint32_t>(placed.side);
        guard.bit = placed.bit;
        guards.append(guard);
    }
    std::ranges::sort(guards, { }, [](const MemberGuardData& guard) {
        return std::tuple<uint32_t, uint32_t, uint32_t> { guard.sideBits, guard.kindBits, guard.name };
    });
    return guards;
}

static LayoutID capturedEnvironmentOf(const Program& program, FunctionID constructor)
{
    if (!constructor)
        return { };
    const Vector<DeclaredType>& captures = program.at(constructor).captures;
    if (captures.size() != 1 || !captures[0].type)
        return { };
    return program.at(captures[0].type).environment;
}

static ClassData classDataOf(TableEncoder& encoder, LayoutID id, FunctionID classConstructor, const ClassRecordParts& parts)
{
    const Layout& layout = encoder.program.at(id);
    LayoutID environment = capturedEnvironmentOf(encoder.program, layout.constructor);

    ClassData data { };
    bool hasProvenBase = layout.flow.baseState == BaseState::Layout;
    LayoutID expectedBase = expectedBaseOf(layout);
    data.base = hasProvenBase ? expectedBase.index() : LayoutData::noBase;
    data.predictedBase = !hasProvenBase && expectedBase ? expectedBase.index() : LayoutData::noBase;
    data.classConstructor = indexOrNone(classConstructor);
    data.environment = environment ? environment.index() : LayoutData::noBase;
    data.numberOfMethods = static_cast<uint32_t>(parts.methods.size()) - parts.numberOfStaticMethods;
    data.numberOfStaticMethods = parts.numberOfStaticMethods;
    data.numberOfStaticFields = static_cast<uint32_t>(parts.staticFields.size());
    data.numberOfMemberGuards = static_cast<uint32_t>(parts.memberGuards.size());
    for (const PlacedGuardBit& placed : encoder.placement.guardBits().ofClass(id))
        data.guardWordsOffset[static_cast<unsigned>(placed.side)] = placed.wordOffset;
    return data;
}

ClassRecordParts collectClassRecordParts(TableEncoder& encoder, LayoutID id)
{
    const Layout& layout = encoder.program.at(id);
    FunctionID classConstructor = encoder.findEntrypoint(layout.constructor);

    ClassRecordParts parts;
    if (classConstructor) {
        parts.methods.reserveInitialCapacity(layout.classFlow.members.size());
        appendMethods(encoder, id, ClassSide::Prototype, parts.methods);
        size_t numberOfPrototypeMethods = parts.methods.size();
        appendMethods(encoder, id, ClassSide::Static, parts.methods);
        parts.numberOfStaticMethods = static_cast<uint32_t>(parts.methods.size() - numberOfPrototypeMethods);
    }
    parts.staticFields = collectStaticFields(encoder, layout);
    parts.memberGuards = collectMemberGuards(encoder, id);
    parts.data = classDataOf(encoder, id, classConstructor, parts);
    return parts;
}

void encodeConstructors(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<ConstructorData> constructors;
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (FunctionID entrypoint = encoder.findEntrypoint(program.layouts[i].constructor))
            constructors.append({ entrypoint.index(), i });
    }
    std::ranges::sort(constructors, [](const ConstructorData& a, const ConstructorData& b) {
        return std::tie(a.function, a.layoutIndex) < std::tie(b.function, b.layoutIndex);
    });
    encoder.setTable(ProgramTableIndex::constructors, constructors);
}

static std::optional<unsigned> targetClosureSlotOf(TableEncoder& encoder, FunctionID id)
{
    const Program& program = encoder.program;
    const Function& bound = program.at(id);
    if (!bound.creator.as<CreatorRecord::BoundFunction>().capturesTargetClosure)
        return std::nullopt;
    if (bound.captures.isEmpty() || declaredRepresentationOf(program, bound.captures.last()) != Representation::Cell) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        return std::nullopt;
    }
    return closureCaptureSlotOf(program, bound, static_cast<unsigned>(bound.captures.size() - 1));
}

void encodeBoundFunctions(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<BoundFunctionData> boundFunctions;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        FunctionID id(i);
        const Function& function = program.at(id);
        if (function.role != FunctionRole::BoundFunction)
            continue;
        FunctionID entrypoint = encoder.findEntrypoint(id);
        if (!entrypoint || !program.at(entrypoint).hasBody())
            continue;
        const auto& record = function.creator.as<CreatorRecord::BoundFunction>();
        if (record.numberOfBoundArguments > UINT16_MAX)
            encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        BoundFunctionData data { };
        data.function = entrypoint.index();
        data.target = encoder.entrypointOf(record.target);
        data.numberOfBoundArguments = record.numberOfBoundArguments;
        std::optional<unsigned> targetClosureSlot = targetClosureSlotOf(encoder, id);
        data.targetClosureSlotPlusOne = targetClosureSlot ? *targetClosureSlot + 1 : 0;
        boundFunctions.append(data);
    }
    std::ranges::sort(boundFunctions, { }, &BoundFunctionData::function);
    encoder.setTable(ProgramTableIndex::boundFunctions, boundFunctions);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

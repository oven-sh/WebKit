/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeClosure.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <memory>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

bool includesData(TypeReader reader, const Type& type, const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
        return type.primitives & Primitive::Undefined;
    case DataNode::Kind::Null:
        return type.primitives & Primitive::Null;
    case DataNode::Kind::Hole:
        return type.primitives & Primitive::Absent;
    case DataNode::Kind::Boolean:
        return type.primitives & (node.as<DataNode::Boolean>().value ? Primitive::True : Primitive::False);
    case DataNode::Kind::Number: {
        double number = node.as<DataNode::Number>().value;
        if (isTrackedInteger(number))
            return type.primitives & Primitive::Integer && type.integers.contains(static_cast<int64_t>(number));
        return type.primitives & static_cast<uint16_t>(primitiveOfNumber(number));
    }
    case DataNode::Kind::String:
        if (type.primitives & Primitive::String)
            return true;
        return type.primitives & Primitive::LiteralString && reader.at(type.stringLiterals).contains(node.as<DataNode::String>().value);
    case DataNode::Kind::BigInt:
        return type.primitives & Primitive::BigInt;
    case DataNode::Kind::Array:
        return type.anyObject || findClassPart(reader, type, BuiltinClasses::Array);
    case DataNode::Kind::Object: {
        LayoutID layout = node.as<DataNode::Object>().layout;
        return type.anyObject || reader.at(type.layouts).contains(layout);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void collectFieldsFilledWithOtherData(const Program& program, TypeArena& arena, const DataNode& node, Vector<FieldReference>& found)
{
    switch (node.kind()) {
    case DataNode::Kind::Array:
        for (const DataNode& element : node.as<DataNode::Array>().elements)
            collectFieldsFilledWithOtherData(program, arena, element, found);
        return;
    case DataNode::Kind::Object: {
        const DataNode::Object& object = node.as<DataNode::Object>();
        const Layout* layout = object.layout && object.layout.index() < program.layouts.size() ? &program.at(object.layout) : nullptr;
        for (const DataMember& member : object.members) {
            collectFieldsFilledWithOtherData(program, arena, member.value, found);
            std::optional<uint32_t> field = layout ? layout->findField(member.key) : std::nullopt;
            if (!field)
                continue;
            const DeclaredType& declared = layout->fields[*field].declared;
            if (declared.isGuarded() && !includesData(arena, arena.at(enforceableTypeOf(arena, declared)), member.value))
                found.append({ object.layout, *field });
        }
        return;
    }
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
    case DataNode::Kind::BigInt:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool filledFieldHasResolvableOrigin(const Program& program, const Field& field)
{
    if (!field.flags.contains(FieldFlags::IsInherited))
        return true;
    if (!field.originLayout || !field.originField || field.originLayout.index() >= program.layouts.size())
        return false;
    const Vector<Field>& originFields = program.at(field.originLayout).fields;
    return *field.originField < originFields.size() && !originFields[*field.originField].flags.contains(FieldFlags::IsInherited);
}

void expectFieldsFilledWithOtherData(Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<std::unique_ptr<TypeArena>> arenasOfWorkers(loops.width());
    Vector<Vector<FieldReference>> fieldsFoundByWorkers(loops.width());
    forEachIndexInParallel(loops, static_cast<unsigned>(program.data.size()), [&](unsigned index, unsigned worker) {
        std::unique_ptr<TypeArena>& arena = arenasOfWorkers[worker];
        if (!arena)
            arena = makeUniqueWithoutFastMallocCheck<TypeArena>(program.types, ArenaKeeps::InProgram);
        collectFieldsFilledWithOtherData(program, *arena, program.data[index].root, fieldsFoundByWorkers[worker]);
    });

    for (const Vector<FieldReference>& found : fieldsFoundByWorkers) {
        for (FieldReference filled : found) {
            FieldReference declaration = filledFieldHasResolvableOrigin(program, fieldAt(program, filled)) ? declarationOf(program, filled) : filled;
            for (FieldReference field : { filled, declaration }) {
                DeclaredType& declared = program.at(field.layout).fields[field.field].declared;
                if (declared.isEstablished())
                    continue;
                declared.expect(declared.expected());
                declared.remove(PositionFlags::IsInt32Promise);
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

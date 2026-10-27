/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeClosure.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowSiteData.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTThreads.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool includesData(const Program& program, const Type& type, const DataNode& node)
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
    case DataNode::Kind::Number:
        return Type::number(node.as<DataNode::Number>().value).isSubsetOf(type, program);
    case DataNode::Kind::String:
        return Type::stringLiteral(node.as<DataNode::String>().value).isSubsetOf(type, program);
    case DataNode::Kind::BigInt:
        return type.primitives & Primitive::BigInt;
    case DataNode::Kind::Array:
        return type.anyObject || type.findClassPart(BuiltinClasses::Array) || hasMembersWithoutLayout(program, type);
    case DataNode::Kind::Object: {
        LayoutID layout = node.as<DataNode::Object>().layout;
        return type.anyObject || (layout && type.layouts && std::ranges::binary_search(program.at(type.layouts).members, layout));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void expectFieldsFilledWithOtherData(Program& program, BuildContext& build)
{
    Vector<FieldReference> fields = collectInParallel<FieldReference>(build.parallelLoops(), program.data.size(), [&](unsigned index, Vector<FieldReference>& found) {
        forEachDataNodeInPreorder(program.data[index].root, [&](uint32_t, const DataNode& node) {
            const auto* object = node.tryAs<DataNode::Object>();
            if (!object || !object->layout)
                return;
            const Layout& layout = program.at(object->layout);
            for (const DataMember& member : object->members) {
                std::optional<uint32_t> field = layout.findField(member.key);
                if (!field)
                    continue;
                const DeclaredType& declared = layout.fields[*field].declared;
                if (!declared.isExpected() && !includesData(program, program.at(slotTypeOf(program, layout.fields[*field])), member.value))
                    found.append({ object->layout, *field });
            }
        });
    });
    for (FieldReference filled : fields) {
        for (FieldReference field : { filled, declarationOf(program, filled) }) {
            DeclaredType& declared = program.at(field.layout).fields[field.field].declared;
            declared.expect(declared.expected());
            declared.remove(PositionFlags::IsInt32Promise);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

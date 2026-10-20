/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaRecords.h"
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline void forEachDataNodeInPreorder(const DataNode& root, NOESCAPE const Invocable<void(uint32_t ordinal, const DataNode&)> auto& functor)
{
    Vector<const DataNode*, 32> stack;
    stack.append(&root);
    uint32_t ordinal = 0;
    while (!stack.isEmpty()) {
        const DataNode& node = *stack.takeLast();
        functor(ordinal++, node);
        switch (node.kind()) {
        case DataNode::Kind::Array: {
            const Vector<DataNode>& elements = node.as<DataNode::Array>().elements;
            for (size_t index = elements.size(); index--;)
                stack.append(&elements[index]);
            break;
        }
        case DataNode::Kind::Object: {
            const Vector<DataMember>& members = node.as<DataNode::Object>().members;
            for (size_t index = members.size(); index--;)
                stack.append(&members[index].value);
            break;
        }
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
        case DataNode::Kind::String:
        case DataNode::Kind::BigInt:
            break;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

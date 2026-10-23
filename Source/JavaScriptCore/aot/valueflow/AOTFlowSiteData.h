/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTSchemaRecords.h"
#include "AOTVector.h"
#include <wtf/StdLibExtras.h>

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
            const auto& elements = node.as<DataNode::Array>().elements;
            for (size_t index = elements.size(); index--;)
                stack.append(&elements[index]);
            break;
        }
        case DataNode::Kind::Object: {
            const auto& members = node.as<DataNode::Object>().members;
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

// A tree without an object asks nothing of a layout, so its size alone says whether ExpandData makes allocations of it. Flow relies on the answer before the pass has run.
inline bool isExpandedForItsSizeAlone(const DataNode& root, uint64_t maxExpandedNodes)
{
    uint64_t numberOfNodes = 0;
    bool holdsObject = false;
    forEachDataNodeInPreorder(root, [&](uint32_t, const DataNode& node) {
        ++numberOfNodes;
        holdsObject |= node.is<DataNode::Object>();
    });
    return !holdsObject && numberOfNodes <= maxExpandedNodes;
}

// The read-only encoding lists the fields, which the allocation fills, before the other members, which become extra properties. Array indexes are listed before the other own keys wherever they are created.
inline bool fitsReadOnlyEncoding(const Program& program, const DataNode::Object& object)
{
    const Layout& layout = program.at(object.layout);
    bool hasNamedOtherMember = false;
    for (const DataMember& member : object.members) {
        if (layout.findField(member.key)) {
            if (hasNamedOtherMember)
                return false;
            continue;
        }
        NameClass nameClass = program.nameClassOf(member.key);
        if (program.at(member.key).kind != PropertyKeyKind::Text || nameClass == NameClass::Proto)
            return false;
        hasNamedOtherMember |= nameClass != NameClass::Index;
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

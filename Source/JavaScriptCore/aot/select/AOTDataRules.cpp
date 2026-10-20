/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTSelectionContext.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool canBeShared(const DataNode& element)
{
    switch (element.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
        return true;
    case DataNode::Kind::Hole:
    case DataNode::Kind::BigInt:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DataEncoding encodingOf(const Data& record)
{
    const auto* array = record.root.tryAs<DataNode::Array>();
    if (array && std::ranges::all_of(array->elements, canBeShared))
        return DataEncoding::CopyOnWriteArray;
    return DataEncoding::ReadOnlyTree;
}

void selectDataEncoding(SelectionContext& context)
{
    const Program& program = context.program();
    Function& function = context.pass().function;
    uint32_t count = 0;
    for (ValueID id : context.valuesWith(Opcode::Data)) {
        Value& value = function.at(id);
        value.setEncoding(encodingOf(program.at(value.data())));
        ++count;
    }
    context.didSelectForms(SelectionRuleID::DataEncoding, count);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

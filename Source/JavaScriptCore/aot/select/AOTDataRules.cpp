/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
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

// Storage in the image may have holes. Storage that the first array of the site creates has none, and it is kept for the arrays that follow: where none follows, the array has storage of its own.
static DataEncoding encodingOf(const Data& record, bool runsAtMostOnce, bool mayBeMadeOnce)
{
    const auto* array = record.root.tryAs<DataNode::Array>();
    if (array && mayBeMadeOnce && !runsAtMostOnce)
        return DataEncoding::MadeOnce;
    if (array && (isStorageOfArray(*array) || (!runsAtMostOnce && std::ranges::all_of(array->elements, canBeShared))))
        return DataEncoding::CopyOnWriteArray;
    return DataEncoding::ReadOnlyTree;
}

void selectDataEncoding(SelectionContext& context)
{
    const Program& program = context.program();
    Function& function = context.pass().function;
    uint32_t count = 0;
    for (ValueID id : context.valuesWith(Opcode::Data)) {
        Value& value = function.edit(id);
        value.setEncoding(encodingOf(program.at(value.data()), context.pass().runsAtMostOnce(context.pass().analyses.useLists().blockOf(id)), value.isNeverToldApart()));
        ++count;
    }
    context.didSelectForms(SelectionRuleID::DataEncoding, count);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

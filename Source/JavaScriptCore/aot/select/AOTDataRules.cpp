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

static bool holdsLists(const Data& record)
{
    const auto* array = record.root.tryAs<DataNode::Array>();
    return array && std::ranges::any_of(array->elements, [](const DataNode& element) { return element.is<DataNode::Array>(); });
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
        // One cell for every evaluation takes more code than the call of the stub that makes a cell around shared storage. A list of lists has no such stub.
        const Data& record = program.at(value.data());
        bool mayBeMadeOnce = value.isNeverToldApart() && (context.shapeAt(id) == CodeShape::Fast || holdsLists(record));
        value.setEncoding(encodingOf(record, context.pass().runsAtMostOnce(context.pass().analyses.useLists().blockOf(id)), mayBeMadeOnce));
        ++count;
    }
    context.didSelectForms(SelectionRuleID::DataEncoding, count);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

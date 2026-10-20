/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowKeys.h"

#if ENABLE(AOT)

#include "AOTIRPrinter.h"
#include "AOTIRProgram.h"
#include "Identifier.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

FlowKeyClass classOf(const Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        return program.nameClassOf(key) == NameClass::Index ? FlowKeyClass::Index : FlowKeyClass::String;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
        return FlowKeyClass::PrivateName;
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return FlowKeyClass::Symbol;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> arrayIndexOf(const Program& program, PropertyKeyID key)
{
    if (classOf(program, key) != FlowKeyClass::Index)
        return std::nullopt;

    std::optional<uint32_t> index = parseIndex(byteCast<Latin1Character>(program.string(program.at(key).text)));
    RELEASE_ASSERT(index && arrayIndexRange.contains(*index));
    return index;
}

static bool isNumericName(const Program& program, PropertyKeyID key)
{
    return program.at(key).kind == PropertyKeyKind::Text && program.nameClassOf(key) == NameClass::CanonicalNumericString;
}

FlowKeySet keySetOf(const Program& program, PropertyKeyID key)
{
    if (std::optional<uint32_t> index = arrayIndexOf(program, key))
        return FlowKeySet::index(Range::one(*index));

    FlowKeySet keys = FlowKeySet::name(key);
    // A read under any number is typed from the element contents, and "-1" is the key that the number -1 names.
    if (isNumericName(program, key))
        keys.indexes = arrayIndexRange;
    return keys;
}

bool FlowKeySet::unite(const FlowKeySet& other)
{
    bool didChange = names.unite(other.names);
    if (!other.indexes.isSubsetOf(indexes)) {
        indexes = indexes.unionWith(other.indexes);
        didChange = true;
    }
    if (other.anyNumericName && !anyNumericName) {
        anyNumericName = true;
        didChange = true;
    }
    if (other.anyString && !anyString) {
        anyString = true;
        didChange = true;
    }
    if (other.anySymbol && !anySymbol) {
        anySymbol = true;
        didChange = true;
    }
    return didChange;
}

bool FlowKeySet::mayName(PropertyKeyID key, const Program& program) const
{
    switch (classOf(program, key)) {
    case FlowKeyClass::String:
        return anyString || names.contains(key) || (anyNumericName && isNumericName(program, key));
    case FlowKeyClass::Index:
        return indexes.contains(*arrayIndexOf(program, key));
    case FlowKeyClass::Symbol:
        return anySymbol || names.contains(key);
    case FlowKeyClass::PrivateName:
        return names.contains(key);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FlowKeySet::isSubsetOf(const FlowKeySet& other, const Program& program) const
{
    if (anyNumericName && !other.anyNumericName)
        return false;
    if (anyString && !other.anyString)
        return false;
    if (anySymbol && !other.anySymbol)
        return false;
    if (!indexes.isSubsetOf(other.indexes))
        return false;
    return std::ranges::all_of(names, [&](PropertyKeyID name) {
        return other.mayName(name, program);
    });
}

void FlowKeySet::dump(PrintStream& out, const Program& program) const
{
    CommaPrinter comma;
    out.print("{"_s);
    for (PropertyKeyID name : names) {
        out.print(comma);
        printPropertyKey(out, program, name);
    }
    if (!indexes.isEmpty())
        out.print(comma, "["_s, indexes.minimum, ","_s, indexes.maximum, "]"_s);
    if (anyString)
        out.print(comma, "string*"_s);
    else if (anyNumericName)
        out.print(comma, "number*"_s);
    if (anySymbol)
        out.print(comma, "symbol*"_s);
    out.print("}"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

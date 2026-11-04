/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinPristine.h"

#if ENABLE(AOT)

#include "AOTInstance.h"

namespace JSC { namespace AOT { namespace Typed {

std::optional<Receiver> receiverWithTestedStructure(BuiltinClassID builtinClass)
{
    if (builtinClass == BuiltinClasses::Buffer)
        return Receiver::Buffer;
    return std::nullopt;
}

PristineTest pristineTestOf(const BuiltinTable& table, BuiltinClassID builtinClass)
{
    switch (table.at(builtinClass).pristineTest) {
    case BuiltinPristineTest::Mark:
        return table.mayHavePristineInstances(builtinClass) ? PristineTest::MayPass : PristineTest::NeverPasses;
    case BuiltinPristineTest::ClosureAsCreated:
        return PristineTest::ClosureAsCreated;
    case BuiltinPristineTest::InstanceStructure:
        return table.hasLockedPrototypeChain(builtinClass) && receiverWithTestedStructure(builtinClass) ? PristineTest::InstanceStructure : PristineTest::NeverPasses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinClassID classBelowWithTestedStructure(const BuiltinTable& table, BuiltinClassID builtinClass)
{
    bool hasClosures = pristineTestOf(table, builtinClass) == PristineTest::ClosureAsCreated;
    BuiltinClassID below = hasClosures ? BuiltinClasses::AsyncFunction : BuiltinClasses::Buffer;
    if (below == builtinClass || !table.isSameOrSubclassOf(below, builtinClass) || pristineTestOf(table, below) != (hasClosures ? PristineTest::ClosureAsCreated : PristineTest::InstanceStructure))
        return { };
    if (hasClosures && !table.hasLockedPrototypeChain(below))
        return { };
    return below;
}

PristineAspects aspectThatFindsMember(const BuiltinTable& table, BuiltinClassID receiverClass, BuiltinID member)
{
    if (table.at(member).kind != BuiltinKind::Accessor || pristineTestOf(table, receiverClass) != PristineTest::MayPass)
        return PristineAspects::Names;
    BuiltinClassID below = classBelowWithTestedStructure(table, receiverClass);
    if (!below)
        return PristineAspects::Names;
    BuiltinMemberLookup foundBelow = table.findForInstance(below, table.at(member).name);
    if (foundBelow.outcome != BuiltinMemberLookup::Outcome::Found || table.canonicalID(foundBelow.member) != table.canonicalID(member))
        return PristineAspects::Names;
    return PristineAspects::NamesKeptBelow;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

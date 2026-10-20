/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static FlowBaseResult baseFromClosure(const FlowSolution& solution, const FlowObject& closure)
{
    FlowBaseResult result;
    if (LayoutID layout = solution.program.at(closure.function).constructedClass) {
        result.state = BaseState::Layout;
        result.base = layout;
    }
    return result;
}

static FlowBaseResult baseFromBuiltin(const FlowSolution& solution, const FlowObject& object)
{
    const BuiltinTable& builtins = solution.builtins();
    BuiltinID constructor = builtins.canonicalID(object.builtin);
    BuiltinClassID instanceClass = builtins.at(constructor).instanceClass;
    BuiltinID classConstructor = instanceClass ? builtins.at(instanceClass).constructor : BuiltinID();

    FlowBaseResult result;
    if (classConstructor && builtins.canonicalID(classConstructor) == constructor) {
        result.state = BaseState::BuiltinClass;
        result.baseClass = instanceClass;
    }
    return result;
}

static FlowBaseResult baseFromObject(const FlowSolution& solution, FlowObjectID id)
{
    const FlowObject& object = solution.objects.at(id);
    switch (object.kind) {
    case FlowObjectKind::Closure:
        return baseFromClosure(solution, object);
    case FlowObjectKind::Builtin:
        return baseFromBuiltin(solution, object);
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::ModuleBuiltin:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isSameBase(const FlowBaseResult& a, const FlowBaseResult& b)
{
    return a.state == b.state && a.base == b.base && a.baseClass == b.baseClass;
}

static FlowBaseResult joinBases(const FlowSolution& solution, const FlowObjectSet& heritageObjects)
{
    std::optional<FlowBaseResult> joined;
    heritageObjects.forEach([&](FlowObjectID object) {
        FlowBaseResult base = baseFromObject(solution, object);
        if (!joined)
            joined = base;
        else if (!isSameBase(*joined, base))
            joined = FlowBaseResult { };
    });
    return joined.value_or(FlowBaseResult { });
}

static FlowBaseResult computeBaseResult(const FlowSolution& solution, LayoutID id)
{
    FlowBaseResult result;
    if (solution.program.at(id).isDerived()) {
        result.state = BaseState::Layout;
        result.base = solution.program.at(id).flow.base;
        return result;
    }
    if (!solution.program.at(id).has(LayoutFlags::HasHeritage)) {
        result.state = BaseState::None;
        return result;
    }
    if (!solution.build.isEnabled(FlowResultKind::ClassBases, id.index()))
        return result;

    FlowNodeID node = solution.graph.heritage(id);
    const FlowValue& value = solution.graph.valueOf(node);
    if (value.isEmpty())
        return result;
    if (value.hasUnknownObject()) {
        result.witness = solution.records.unknownObjectWitness(node);
        return result;
    }

    // A heritage that is a primitive other than null makes the class definition throw, so no class has it.
    bool mayBeNull = value.primitives & Primitive::Null;
    if (mayBeNull && value.mayBeObject())
        return result;
    if (mayBeNull) {
        result.state = BaseState::None;
        return result;
    }
    return joinBases(solution, value.objects);
}

static LayoutID provenBaseOf(const FlowBaseResult& result)
{
    return result.state == BaseState::Layout ? result.base : LayoutID();
}

static void invalidateCyclicBases(Vector<FlowBaseResult>& bases)
{
    enum class WalkState : uint8_t { Unvisited, OnPath, Done };

    Vector<WalkState> states(FillWith { }, bases.size(), WalkState::Unvisited);
    Vector<LayoutID, 16> path;
    for (size_t index = 0; index < bases.size(); ++index) {
        path.shrink(0);
        LayoutID current(index);
        while (current && states[current.index()] == WalkState::Unvisited) {
            states[current.index()] = WalkState::OnPath;
            path.append(current);
            current = provenBaseOf(bases[current.index()]);
        }

        bool closesCycle = current && states[current.index()] == WalkState::OnPath;
        size_t firstOnCycle = closesCycle ? path.find(current) : path.size();
        for (size_t position = 0; position < path.size(); ++position) {
            states[path[position].index()] = WalkState::Done;
            if (position >= firstOnCycle)
                bases[path[position].index()] = FlowBaseResult { };
        }
    }
}

Vector<FlowBaseResult> computeBaseResults(const FlowSolution& solution)
{
    Vector<FlowBaseResult> bases(solution.program.layouts.size());
    forEachIndexInParallel(solution.build.parallelLoops(), bases.size(), [&](unsigned index) {
        bases[index] = computeBaseResult(solution, LayoutID(index));
    });
    invalidateCyclicBases(bases);
    return bases;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

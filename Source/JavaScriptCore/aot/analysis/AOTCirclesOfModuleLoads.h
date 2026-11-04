/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// A module loads what its record requests and what a call of require or import in one of its functions names.
// A name that the build does not know finds an entry point or a file marked IsFoundByName. Two modules of one component may each begin while the other runs.
inline StronglyConnectedComponents computeCirclesOfModuleLoads(const Program& program, const ParallelLoops& loops)
{
    using ModulePair = std::pair<ModuleID, ModuleID>;
    const ModuleID anyFile(static_cast<uint32_t>(program.modules.size()));
    Vector<ModulePair> loadsByCalls = collectInParallel<ModulePair>(loops, program.functions.size(), [&](unsigned index, Vector<ModulePair>& found) {
        forEachValueListedOrNot(program, FunctionID(index), { Opcode::RequireModule, Opcode::ImportModule, Opcode::EvaluateModule, Opcode::RequireDynamic, Opcode::ImportDynamic }, [&](ValueID, const Value& value) {
            bool namesModule = value.opcode != Opcode::RequireDynamic && value.opcode != Opcode::ImportDynamic;
            if (value.opcode == Opcode::EvaluateModule)
                found.append({ value.referrer(), value.module() });
            else if (ModuleID module = program.functions[index].module)
                found.append({ module, namesModule ? value.module() : anyFile });
        });
    });
    Vector<Vector<ModuleID>> modulesLoadedBy(program.modules.size() + 1, [&](size_t index) -> Vector<ModuleID> {
        if (index == anyFile.index()) {
            Vector<ModuleID> files;
            for (size_t file : modulesThatHostAsksFor(program, true))
                files.append(ModuleID(static_cast<uint32_t>(file)));
            return files;
        }
        return program.modules[index].requests.map([](const ModuleRequest& request) { return request.target; });
    });
    for (auto [from, to] : loadsByCalls)
        modulesLoadedBy[from.index()].append(to);
    return computeStronglyConnectedComponents(modulesLoadedBy.size(), [&](unsigned index) -> const Vector<ModuleID>& { return modulesLoadedBy[index]; });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

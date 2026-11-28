/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class Closure;
class CompactTable;
class CompiledStructure;
class Environment;
class Generator;
class ModuleRecord;
class Object;
class SlimClosure;
class StringBuilderStorage;

} } // namespace JSC::AOT

#define FOR_EACH_AOT_INLINE_VISITED_CELL_CLASS(macro) \
    macro(CompiledObject, AOT::Object, InLoop) \
    macro(CompiledEnvironment, AOT::Environment, InLoop) \
    macro(CompactTable, AOT::CompactTable, InLoop) \
    macro(SlimClosure, AOT::SlimClosure, InLoop) \
    macro(CompiledClosure, AOT::Closure, InLoop)

#define FOR_EACH_JSC_AOT_DYNAMIC_ISO_SUBSPACE(macro) \
    macro(aotGeneratorSpace, cellHeapCellType, AOT::Generator) \
    macro(aotModuleRecordSpace, destructibleCellHeapCellType, AOT::ModuleRecord) \
    macro(aotStringBuilderStorageSpace, destructibleCellHeapCellType, AOT::StringBuilderStorage)

#define FOR_EACH_JSC_AOT_STRUCTURE_ISO_SUBSPACE(macro) \
    macro(compiledStructureSpace, destructibleCellHeapCellType, AOT::CompiledStructure)

#else

#define FOR_EACH_AOT_INLINE_VISITED_CELL_CLASS(macro)
#define FOR_EACH_JSC_AOT_DYNAMIC_ISO_SUBSPACE(macro)
#define FOR_EACH_JSC_AOT_STRUCTURE_ISO_SUBSPACE(macro)

#endif // ENABLE(AOT)

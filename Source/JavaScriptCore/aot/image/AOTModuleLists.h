/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTTableParts.h"
#include "AOTThreads.h"
#include <algorithm>
#include <initializer_list>
#include <utility>

namespace JSC { namespace AOT {

// The records of the modules and the lists that hang on them. Each list holds the entries of the modules one behind the other, in the order of ModuleID.
struct ModuleLists {
    void setListStarts(ModuleData& entry) const
    {
        entry.firstRequest = Number24(requests.size());
        entry.firstExport = Number24(exports.size());
        entry.firstSyntheticExport = Number24(syntheticExports.size());
        entry.firstRequiredName = Number24(requiredNames.size());
    }

    // One record more than there are modules: the last says where the lists of the last module end.
    // The public path, the display path, the body, the value, the faces of files and what the record of the module says are filled in. What follows from all the requests together (computeModuleFlags(), the lists of a link and of an evaluation) is not.
    Vector<ModuleData> modules;
    // A directory stands behind the one above it.
    Vector<ModuleDirectoryData> directories;
    Vector<ModuleDisplayPathData> displayPaths;
    Vector<ModuleRequestData> requests;
    Vector<ModuleExportData> exports;
    // Beside `exports`, entry by entry: the module that declares the variable which the entry shows, and the module of the entry where it shows none.
    // Empty unless AnswersAboutModules::mayMocksStandInPlaceOfModules: nobody asks.
    Vector<uint32_t> modulesThatDeclareExports;
    Vector<IndirectExportData> indirectExports;
    Vector<SyntheticExportData> syntheticExports;
    Vector<RequiredNameData> requiredNames;
    // Ascending by the number of the required name.
    Vector<ExportStatementData> exportStatements;
    // Ascending by module.
    Vector<AmbiguousExportData> ambiguousExports;
    // Ascending by module.
    Vector<SourceStampData> sourceStamps;
};

// Loops of which none reads what another writes, as one loop: the threads are asked for once, and a step that has one thread is a loop of one index beside the others. The first loop is begun first.
struct LoopBesideOthers {
    unsigned count;
    ScopedLambda<void(unsigned index)> body;
};
void forEachIndexOfLoopsInParallel(const ParallelLoops&, std::initializer_list<LoopBesideOthers>);

// The range of forEachRangeOfIndicesInParallel() with the number.
inline std::pair<size_t, size_t> boundsOfRangeOfIndices(unsigned range, size_t count)
{
    size_t first = static_cast<size_t>(range) * indicesInRangeOfLoop;
    return { first, std::min(count, first + indicesInRangeOfLoop) };
}

using AnswerAboutModule = AnswersAboutModules::Answer;
AnswersAboutModules askAboutModules(const TableInputs&, const ParallelLoops&);

// A module whose path lies in pieces is simple: ModuleData::isSimple().
ModuleLists collectModuleLists(TableEncoder&, const AnswersAboutModules&);
// The strings, names, variables and closure cells that collectModuleLists() asks the numbers of: one walk serves both. nameWhatModuleTablesName() calls it.
void nameWhatModuleListsName(const TableInputs&, const AnswersAboutModules&, const ParallelLoops&, NamesOfTables&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

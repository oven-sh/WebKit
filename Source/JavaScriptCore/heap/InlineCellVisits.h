/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CellVisitKind.h"
#include "DateInstance.h"
#include "FastMarkAppender.h"
#include "GetterSetter.h"
#include "JSArray.h"
#include "JSCellButterfly.h"
#include "JSCellInlines.h"
#include "JSFunctionWithFields.h"
#include "JSMap.h"
#include "JSObjectVisitInlines.h"
#include "JSPromise.h"
#include "JSPromiseReaction.h"
#include "JSSet.h"
#include "JSStringVisitInlines.h"
#include "StructureInlines.h"

namespace JSC {

template<CellVisitKind> struct InlineCellVisit;

template<typename CellType, typename... ClassesWithFields>
struct CellFieldsVisit {
    ALWAYS_INLINE static bool tryVisit(FastMarkAppender& appender, JSCell* cell, Structure* structure)
    {
        appender.appendStructure(structure);
        [[maybe_unused]] auto* typedCell = uncheckedDowncast<CellType>(cell);
        (ClassesWithFields::visitFields(typedCell, appender), ...);
        return true;
    }
};

template<typename ObjectType, typename... ClassesWithFields>
struct ObjectFieldsVisit {
    ALWAYS_INLINE static bool tryVisit(FastMarkAppender& appender, JSCell* cell, Structure* structure)
    {
        appender.appendStructure(structure);
        auto* object = uncheckedDowncast<ObjectType>(cell);
        object->visitButterfly(appender);
        (ClassesWithFields::visitFields(object, appender), ...);
        return true;
    }
};

template<> struct InlineCellVisit<CellVisitKind::String> : CellFieldsVisit<JSString, JSString> { };
template<> struct InlineCellVisit<CellVisitKind::FinalObject> : CellFieldsVisit<JSFinalObject, JSFinalObject> { };
template<> struct InlineCellVisit<CellVisitKind::Array> : ObjectFieldsVisit<JSArray> { };
template<> struct InlineCellVisit<CellVisitKind::Map> : ObjectFieldsVisit<JSMap, JSOrderedHashTable<MapTraits>> { };
template<> struct InlineCellVisit<CellVisitKind::Set> : ObjectFieldsVisit<JSSet, JSOrderedHashTable<SetTraits>> { };
template<> struct InlineCellVisit<CellVisitKind::GetterSetter> : CellFieldsVisit<GetterSetter, GetterSetter> { };
template<> struct InlineCellVisit<CellVisitKind::CellButterfly> : CellFieldsVisit<JSCellButterfly, JSCellButterfly> { };
template<> struct InlineCellVisit<CellVisitKind::FullPromiseReaction> : CellFieldsVisit<JSFullPromiseReaction, JSPromiseReaction, JSFullPromiseReaction> { };
template<> struct InlineCellVisit<CellVisitKind::SlimPromiseReaction> : CellFieldsVisit<JSSlimPromiseReaction, JSPromiseReaction, JSSlimPromiseReaction> { };
template<> struct InlineCellVisit<CellVisitKind::Promise> : ObjectFieldsVisit<JSPromise, JSPromise> { };
template<> struct InlineCellVisit<CellVisitKind::Date> : ObjectFieldsVisit<DateInstance> { };
template<> struct InlineCellVisit<CellVisitKind::Function> : ObjectFieldsVisit<JSFunction, JSCallee, JSFunction> { };
template<> struct InlineCellVisit<CellVisitKind::FunctionWithFields> : ObjectFieldsVisit<JSFunctionWithFields, JSCallee, JSFunction, JSFunctionWithFields> { };

template<> struct InlineCellVisit<CellVisitKind::Structure> {
    ALWAYS_INLINE static bool tryVisit(FastMarkAppender& appender, JSCell* cell, Structure* structure)
    {
        auto* visitedStructure = uncheckedDowncast<Structure>(cell);
        if (visitedStructure->variant() != Structure::StructureVariant::Normal)
            return false;
        appender.appendStructure(structure);
        Structure::visitFields(visitedStructure, appender);
        return true;
    }
};

} // namespace JSC

#if ENABLE(AOT)
#include "AOTHeapCellVisits.h"
#endif

namespace JSC {

template<CellVisitPlacement requestedPlacement>
ALWAYS_INLINE bool tryVisitInline(FastMarkAppender& appender, JSCell* cell, Structure* structure, CellVisitKind kind)
{
    appender.setCurrentCell(cell);
    switch (kind) {
    case CellVisitKind::MethodTable:
        return false;
#define JSC_VISIT_CELL_OF_KIND(kind, cellType, placement) \
    case CellVisitKind::kind: \
        if constexpr (CellVisitPlacement::placement == requestedPlacement) \
            return InlineCellVisit<CellVisitKind::kind>::tryVisit(appender, cell, structure); \
        else \
            return false;
    FOR_EACH_INLINE_VISITED_CELL_CLASS(JSC_VISIT_CELL_OF_KIND)
#undef JSC_VISIT_CELL_OF_KIND
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE bool tryVisitInline(FastMarkAppender& appender, JSCell* cell, Structure* structure, CellVisitKind kind)
{
    return tryVisitInline<CellVisitPlacement::InLoop>(appender, cell, structure, kind) || tryVisitInline<CellVisitPlacement::OutOfLoop>(appender, cell, structure, kind);
}

} // namespace JSC

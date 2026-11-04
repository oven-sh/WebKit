/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/AOTHeapCellClasses.h>
#include <cstdint>

namespace JSC {

class DateInstance;
class GetterSetter;
class JSArray;
class JSCellButterfly;
class JSFinalObject;
class JSFullPromiseReaction;
class JSFunction;
class JSFunctionWithFields;
class JSMap;
class JSPromise;
class JSSet;
class JSSlimPromiseReaction;
class JSString;
class Structure;

#define FOR_EACH_JSC_INLINE_VISITED_CELL_CLASS(macro) \
    macro(String, JSString, InLoop) \
    macro(FinalObject, JSFinalObject, InLoop) \
    macro(Array, JSArray, InLoop) \
    macro(Map, JSMap, InLoop) \
    macro(Set, JSSet, InLoop) \
    macro(Structure, Structure, OutOfLoop) \
    macro(GetterSetter, GetterSetter, InLoop) \
    macro(CellButterfly, JSCellButterfly, OutOfLoop) \
    macro(FullPromiseReaction, JSFullPromiseReaction, OutOfLoop) \
    macro(SlimPromiseReaction, JSSlimPromiseReaction, OutOfLoop) \
    macro(Promise, JSPromise, OutOfLoop) \
    macro(Date, DateInstance, OutOfLoop) \
    macro(Function, JSFunction, OutOfLoop) \
    macro(FunctionWithFields, JSFunctionWithFields, OutOfLoop)

#define FOR_EACH_INLINE_VISITED_CELL_CLASS(macro) \
    FOR_EACH_JSC_INLINE_VISITED_CELL_CLASS(macro) \
    FOR_EACH_AOT_INLINE_VISITED_CELL_CLASS(macro)

enum class CellVisitKind : uint8_t {
    MethodTable,
#define JSC_DECLARE_CELL_VISIT_KIND(kind, cellType, placement) kind,
    FOR_EACH_INLINE_VISITED_CELL_CLASS(JSC_DECLARE_CELL_VISIT_KIND)
#undef JSC_DECLARE_CELL_VISIT_KIND
};

enum class CellVisitPlacement : uint8_t { InLoop, OutOfLoop };

template<typename CellType>
inline constexpr CellVisitKind cellVisitKindFor = CellVisitKind::MethodTable;

#define JSC_DEFINE_CELL_VISIT_KIND_FOR(kind, cellType, placement) \
    template<> inline constexpr CellVisitKind cellVisitKindFor<cellType> = CellVisitKind::kind;
FOR_EACH_INLINE_VISITED_CELL_CLASS(JSC_DEFINE_CELL_VISIT_KIND_FOR)
#undef JSC_DEFINE_CELL_VISIT_KIND_FOR

} // namespace JSC

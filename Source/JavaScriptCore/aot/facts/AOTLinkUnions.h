/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThinSummary.h"

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;
struct LinkTables;
struct LinkType;

// ---- AOTLinkUnions.cpp

// StateOfLinkReach::bringAlive(), behind noteConstructionsOf().
void comeAliveForUnions(LinkFlow&, FunctionID);
// link(), behind writeLayoutsAndCells(): LinkTables::taggedMakers, usesThroughFamilies, LinkedLayout::onlyOwnObjectsStandBelowPrototype. It writes where unseen code does anything, and leaves the defaults in the other reading.
void writeTablesOfUnions(LinkFlow&, LinkTables&);
// Something that the layouts do not say stands below the prototype objects of the type: LinkFlow::prototypesLaidAboveOtherObjects.
void notePrototypesLaidAboveOtherObjects(LinkFlow&, const LinkType& above);
// For the scan, of a NewTemplateObject: what lies among the cooked texts.
TypeID typeOfElementsOfTemplateObject(const BodyValue& made);

// ---- AOTLinkUnionEffects.cpp

// ThinSummary::inspections: some operation of the body walks a chain or lists keys of what may be a proxy, or of what may stand below one.
bool someInspectionMayRunCode(const LinkFlow&, const LinkTables&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTFoldingOfConstants.h"
#include "AOTLinkFlow.h"
#include "AOTNameClass.h"

namespace JSC { namespace AOT { namespace Typed {

class Walker;

using DefinitionsInScanOfStrings = ScopedLambda<BodyValue(ValueID)>;

void notePathsOfModulesReadInScan(const BodyValue&, SummaryBeingMade&);
PropertyKeyID nameKeyOfAccessInScan(const Program&, PrimitiveKnownToFolding held, const BodyValue& definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf, Vector<uint32_t>& beginningsFound, StringID& beginning);
StringID textThatValueBeginsWith(const Program&, BodyValue definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf, Vector<uint32_t>& beginningsFound);
void finishListsOfStringsInScan(SummaryBeingMade&);

bool keyThatBeginsSoMayNamePrototypeLink(const Program&, const SummaryAccess&);

void fillTablesOfStringsForFacts(const Program&, const ThinSummaries&, const LinkTables& linked, const LinkTables& held, FactTables&);


} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

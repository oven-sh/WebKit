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

// ---- AOTScanStrings.cpp. What ScanOfBody tells as it reads, in the order of the stream. Each looks at the operation in hand and at records of the program, and at no other function.
using DefinitionsInScanOfStrings = ScopedLambda<BodyValue(ValueID)>;

// `import.meta`, `__filename`, `require.resolve("./a")`, the message of a require() that fails. For readOperation().
void notePathsOfModulesReadInScan(const BodyValue&, ThinSummary&);
// `let k = "kind"; node[k]`, `o[undefined]`, and `emitter["on" + kind]`. For readAccess(), of an access under a computed key.
// `held`: what primitiveMadeOfConstants() knows of the key. `definition`: of the key, behind casts. `definitionOf`: of a value of the body that was read by now, behind casts.
// The key that the access goes by in every run. Null: it goes by the value, and `beginning` is the text that the key begins with in every run, or null.
PropertyKeyID nameKeyOfAccessInScan(const Program&, PrimitiveKnownToFolding held, const BodyValue& definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf, StringID& beginning);
// `"on" + kind`, `` `on${kind}` ``, `("on" + kind) + suffix`: the written text that the value begins with in every run in which it is made. A written text begins with itself. Null: nobody knows, or the text is empty.
StringID textThatValueBeginsWith(const Program&, BodyValue definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf);
// For finish().
void finishListsOfStringsInScan(ThinSummary&);

// ---- AOTLinkStrings.cpp.
// For AccessInLinkAccesses::readKey(), of an access whose key may be a text. False: whatever the key is, it is not `__proto__`.
bool keyThatBeginsSoMayNamePrototypeLink(const Program&, const SummaryAccess&);

// ---- AOTFactsStrings.cpp.
// For computeFacts(), behind fillLayoutTablesForFacts(). It is THE writer of FactTables::layoutsThatParsersMayMake and modulesWhosePathNoOperationReads.
void fillTablesOfStringsForFacts(const Program&, const ThinSummaries&, const LinkTables& linked, const LinkTables& held, FactTables&);


} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

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

// ---- Hooks into AOTThinSummary.h. Each default claims nothing.
//   SummaryAccess:  StringID beginningOfKey;                       // null: the key may begin with anything
//   ThinSummary:    Vector<StringID> textsThatMeetOthers;
//                   struct AccessUnderConstantKey { ValueID site; PropertyKeyID key; }; Vector<AccessUnderConstantKey> accessesUnderConstantKeys;
//                   Vector<ModuleID> modulesWhosePathIsRead; bool mayReadPathOfAnyModule { true };
// ---- THE RUNG: none > one written text > any string. NOT BUILT HERE: RULINGS.md, unions-and-narrowing (d). Its design stands in `#` lines at the end of HOOKS.txt, its tests are tests/link/one-text-*.ir.
//   static constexpr uint16_t LinkType::oneText = Primitive::LiteralString;   // the bit is free in a LinkType: withoutLiteralsInLinkFlow() clears it today
//   closed(): String brings oneText . holdsOneText(primitives): (primitives & (Primitive::String | oneText)) == oneText . inNamesOfPrimitive(): oneText becomes String
//   void arrive(FlowNodeID, const LinkType&, PropertyKeyID symbol = { }, StringID text = { });
//   HashMap<uint32_t, uint32_t> m_textsAtNodes, beside m_symbolsAtNodes
//   StringID LinkFlow::textAt(FlowNodeID) const;
//   void LinkFlow::joinTextArriving(size_t index, const LinkType& before, const LinkType& arriving, LinkType& after, StringID text);
//   PropertyKeyID LinkFlow::keyOfTextAt(FlowNodeID) const;

// ---- AOTScanStrings.cpp. What ScanOfBody tells as it reads, in the order of the stream. Each looks at the operation in hand and at records of the program, and at no other function.
using DefinitionsInScanOfStrings = ScopedLambda<BodyValue(ValueID)>;

// `const kind = a ? "hex" : "dec"`. For readArgumentsOf(), of each argument of an edge. `definition`: of the argument, behind casts. Null: it is a parameter.
void noteTextThatMeetsOthersInScan(const BodyValue& definition, ThinSummary&);
// For readTerminator(). `definitionOfWhatIsReturned`: behind casts. Null: the terminator is no Return, or returns no value or a parameter.
void noteTextsThatMeetBehindTerminatorInScan(const BodyTerminator&, const BodyValue& definitionOfWhatIsReturned, ThinSummary&);
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

// For WriterOfLinkLayouts::canNameAppendedField(): `res[kSocket] = socket`, `o[Symbol.iterator] = f`, `o[Symbol.for("tag")] = 1`.
// A created symbol comes to the writer as a key only where Link has proven that the key is the one symbol of one call that runs once: LinkFlow::keyOf().
// The plan places slots by key whatever its kind, the record of the layout says which fields have symbol keys (describeFieldOfLayoutRecord(), hasFieldsWithSymbolKeys), and the run time lists them behind the texts.
// NO PRIVATE NAME. Each evaluation of a class makes names of its own, and Link counts no evaluations: fillClassTablesForFacts() counts them behind Link. And slotMayGoByTypeOfFieldForSlotPlan() takes an appended field
// for a plain property, while PrivateAccessEmitter looks for a boxed value where evaluations or realms share the layout.
constexpr bool kindOfKeyMayNameAppendedField(PropertyKeyKind kind)
{
    if (isSymbolKind(kind))
        return true;
    return kind == PropertyKeyKind::Text;
}

// ---- AOTFactsStrings.cpp.
// For computeFacts(), behind fillLayoutTablesForFacts(). It is THE writer of FactTables::layoutsThatParsersMayMake and modulesWhosePathNoOperationReads.
void fillTablesOfStringsForFacts(const Program&, const ThinSummaries&, const LinkTables& linked, const LinkTables& held, FactTables&);


} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

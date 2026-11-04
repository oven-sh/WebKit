/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTTaggedLayouts.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "AOTWireSections.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;
struct ExternalFunctionBodies;

namespace Typed {

class Program;

// From the wire's types (ir/schema/types.def) to the types of the engine. The reader of the program's tables owns one and calls its steps in the order written here.
//
// A wire type speaks of shapes, so it maps to primitives and `unknown` within bounds, and names objects only where the wire names a thing:
//   primitives, minimum, maximum    the primitive bits. Number is Integer | WholeNumber | FractionalNumber with the full range; HasIntegers adds Integer with the range written
//   strings                         stringLiterals
//   flags AnyObject, AnyFunction    anyObject, anyFunction, without seeds
//   layouts                         unnamedLayouts: the union of the families of the layouts named. A family is every layout whose objects may pass for the type
//   classes                         unnamedClasses, with the contents written. Contents name earlier types of the list
//   builtins                        objects: ObjectTable::objectOf(canonical id)
//   functions                       objects: ObjectTable::closureOf() of each function of the family, and objectOf() of its built-in members. A signature, or an empty family, is anyFunction
//   opaque, environment             the same
// Whether such a type is a proof or an expectation is not the type's business: DeclaredType and the operation that names it say so.
class WireTypeReader {
    WTF_MAKE_NONCOPYABLE(WireTypeReader);
public:
    WireTypeReader(Program&, const Wire::Sections& checkedSections, BuildContext&, const ExternalFunctionBodies* = nullptr);
    ~WireTypeReader();

    // 1. Behind strings and property keys. Two rows, side by side. False: it has reported an internal error.
    //    The family of each layout and the tagged layouts, which are new layouts.
    //    The ObjectTable of built-ins and functions, which it hands to the TypeTable, then the family of each function type.
    bool makeObjectsAndComputeFamilies();
    // How many layouts the program will have beside those of the wire: built-in layouts, namespaces, tagged layouts. Known behind step 1.
    uint32_t numberOfDerivedLayouts() const { return m_numberOfDerivedLayouts; }
    // 2. Every type of the wire's list. Behind it TypeTable::typeOfWireIndex() answers.
    void readTypes();
    // 3. Behind the reader's own reading of the wire's layout records into Program::layouts: appends the derived layouts, sets Program::declaredMembersOf, indexes and inherits fields.
    void finishLayouts();
    // 4. When every list of the program is read: ObjectTable::addObjectsOfProgram(), IDSetTable::reserveMembers() for layouts.
    // The numbers of property keys stay stable behind it: computeFacts() adds keys of its own, on one thread, and ends them.
    void finishObjects();

private:
    void makeObjects();
    bool computeDeclaredMembers();
    void planTaggedLayouts(Vector<Vector<LayoutID>>& members, const class TellingFields&);
    void computeFunctionFamilies();
    TypeParts readPartsWithoutClasses(TypeArena&, uint32_t index) const;
    bool readsAsAnything(std::span<const uint32_t> namedLayouts) const;

    Program& m_program;
    const Wire::Sections& m_sections;
    BuildContext& m_build;
    const ExternalFunctionBodies* m_externalBodies;
    uint32_t m_numberOfDerivedLayouts { 0 };
    Vector<LayoutSetID> m_declaredMembers;
    BitVector m_typeParametersWithoutFamily;
    BitVector m_viewsWithoutLayouts;
    // By function of the wire: the closures and built-ins that may pass for the function type. Null: none, so any function.
    Vector<ObjectSetID> m_functionFamilies;
    std::optional<InternalError> m_errorInFunctionFamilies;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTDiagnostics.h"
#include "AOTTaggedLayouts.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "AOTWireSections.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;
struct ExternalFunctionBodies;

namespace Typed {

class Program;

class WireTypeReader {
    WTF_MAKE_NONCOPYABLE(WireTypeReader);
public:
    WireTypeReader(Program&, const Wire::Sections& checkedSections, BuildContext&, const ExternalFunctionBodies* = nullptr);
    ~WireTypeReader();

    bool computeDeclaredMembers();
    void makeObjects();
    void computeFunctionFamilies();
    bool reportErrorInFunctionFamilies();
    uint32_t numberOfDerivedLayouts() const { return m_numberOfDerivedLayouts; }
    void readTypes();
    TypeID typeExpectedBesideViews(uint32_t wireIndex) const { return wireIndex < m_typesExpectedBesideViews.size() ? m_typesExpectedBesideViews[wireIndex] : TypeID(); }
    TypeID typeOfRecordMadeForField(uint32_t wireIndex) const { return m_typesOfRecordsMadeForFields.get(wireIndex + 1); }
    void finishLayouts();
    void findModulesNamedByBodies();
    void finishObjects();

private:
    void planTaggedLayouts(ArenasOfWorkers&, Vector<LayoutSetID>& members, const class TellingFields&);
    void findTypesExpectedBesideViews(ArenasOfWorkers&, const Vector<TypeID>& typesOfWireIndexes);
    void findTypesOfRecordsMadeForFields(ArenasOfWorkers&);
    enum class ViewsWithoutLayouts : bool { ReadAsAnything, AreLeftOut };
    TypeParts readPartsWithoutClasses(TypeArena&, uint32_t index, ViewsWithoutLayouts = ViewsWithoutLayouts::ReadAsAnything) const;
    bool readsAsAnything(std::span<const uint32_t> namedLayouts) const;

    Program& m_program;
    const Wire::Sections& m_sections;
    BuildContext& m_build;
    const ExternalFunctionBodies* m_externalBodies;
    uint32_t m_numberOfDerivedLayouts { 0 };
    Vector<LayoutSetID> m_declaredMembers;
    BitVector m_typeParametersWithoutFamily;
    BitVector m_viewsWithoutLayouts;
    Vector<TypeID> m_typesExpectedBesideViews;
    UncheckedKeyHashMap<uint32_t, TypeID> m_typesOfRecordsMadeForFields;
    Vector<ObjectSetID> m_functionFamilies;
    std::optional<InternalError> m_errorInFunctionFamilies;
    BitVector m_modulesNamedByBodies;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

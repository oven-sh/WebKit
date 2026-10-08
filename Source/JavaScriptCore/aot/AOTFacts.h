/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "Identifier.h"
#include "Options.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

namespace AOT {

class Facts {
    WTF_MAKE_TZONE_ALLOCATED(Facts);
    WTF_MAKE_NONCOPYABLE(Facts);
public:
    Facts() = default;

    JS_EXPORT_PRIVATE static void load(VM&, std::span<const uint8_t>);
    static const Facts* shared() { return s_shared; }
    static bool sourceMayHaveTags() { return s_shared || Options::useTypeTags() || Options::useAOT(); }
    static void noteFunction(UnlinkedFunctionExecutable*, StringView source, unsigned parametersStart);
    static void noteCode(UnlinkedCodeBlock*, UnlinkedFunctionExecutable*);
    std::optional<uint32_t> numberOf(UnlinkedFunctionExecutable*) const;
    std::optional<uint32_t> numberOfFunctionWithCode(UnlinkedCodeBlock*) const;
    Vector<UnlinkedFunctionExecutable*, 1> executablesOf(uint32_t function) const;

    enum class SiteKind : uint32_t { None, Function, Class, Object, Array, New, Call, Read };
    enum class ShapeKind : uint32_t { None, Record, Instance, Function, Class, Prototype, Namespace, Native, Declared, Foreign, Bound };

    enum Primitive : uint32_t {
        Undefined = 1 << 0,
        Null = 1 << 1,
        True = 1 << 2,
        False = 1 << 3,
        Int32 = 1 << 4,
        Double = 1 << 5,
        String = 1 << 6,
        Symbol = 1 << 7,
        BigInt = 1 << 8,
        IsUnknown = 1 << 16,
    };
    enum FieldFlags : uint32_t { IsPresent = 1, MayBeAccessor = 2, IsPlaced = 4 };
    enum ShapeFlags : uint32_t { IsLost = 1, IsReshaped = 2, IsKeyed = 4 };

    struct Site {
        SiteKind kind { SiteKind::None };
        uint32_t number { 0 };
        uint32_t callee { 0 };
    };
    struct TypeFact {
        uint32_t bits { 0 };
        std::span<const uint32_t> strings;
        std::span<const uint32_t> shapes;
        bool isUnknown() const { return bits & IsUnknown; }
        bool isEmpty() const { return !bits && strings.empty() && shapes.empty(); }
    };
    struct Field {
        uint32_t name;
        uint32_t type;
        uint32_t flags;
    };
    struct Shape {
        ShapeKind kind { ShapeKind::None };
        uint32_t flags { 0 };
        uint32_t name { 0 };
        uint32_t prototype { 0 };
        uint32_t of { 0 };
        uint32_t unnamed { 0 };
        std::span<const Field> fields;
        std::span<const uint32_t> slots;
    };
    struct Function {
        uint32_t shape { 0 };
        uint32_t thisType { 0 };
        uint32_t result { 0 };
        uint32_t rest { 0 };
        std::span<const uint32_t> parameters;
    };

    enum Flags : uint32_t { BuffersAreFixed = 1 };
    bool buffersAreFixed() const { return m_flags & BuffersAreFixed; }
    Site site(uint32_t tag) const { return tag && tag <= m_sites.size() ? m_sites[tag - 1] : Site { }; }
    TypeFact type(uint32_t number) const;
    Shape shape(uint32_t number) const;
    Function function(uint32_t number) const;
    unsigned numberOfShapes() const { return m_shapes.size(); }
    unsigned numberOfFunctions() const { return m_functions.size(); }
    UniquedStringImpl* name(uint32_t number) const { return number < m_names.size() ? m_names[number].impl() : nullptr; }
    const Field* field(uint32_t shape, UniquedStringImpl* name) const;
    uint32_t shapeOfInstancesOf(uint32_t constructor) const { return m_shapesOfInstances.get(constructor); }
    void noteFieldInitializer(UnlinkedCodeBlock* initializer, uint32_t typeOfThis, uint32_t function, std::span<UnlinkedCodeBlock* const> constructors) const;
    std::optional<uint32_t> typeOfThisInFieldInitializer(UnlinkedCodeBlock*) const;
    uint32_t fieldInitializerCalledBy(UnlinkedCodeBlock* constructor) const;

private:
    JS_EXPORT_PRIVATE static Facts* s_shared;
    uint32_t m_flags { 0 };
    Vector<Identifier> m_names;
    Vector<uint32_t> m_words;
    Vector<uint32_t> m_types;
    Vector<uint32_t> m_shapes;
    Vector<uint32_t> m_functions;
    Vector<Site> m_sites;
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> m_numbersOfNames;
    UncheckedKeyHashMap<uint32_t, uint32_t> m_shapesOfInstances;
    mutable Lock m_lock;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t> m_numbersOfFunctions WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<UnlinkedCodeBlock*, uint32_t> m_numbersOfFunctionsWithCode WTF_GUARDED_BY_LOCK(m_lock);
    mutable UncheckedKeyHashMap<UnlinkedCodeBlock*, uint32_t> m_typesOfThisInFieldInitializers WTF_GUARDED_BY_LOCK(m_lock);
    mutable UncheckedKeyHashMap<UnlinkedCodeBlock*, uint32_t> m_fieldInitializers WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<uint32_t, Vector<UnlinkedFunctionExecutable*, 1>, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_executables WTF_GUARDED_BY_LOCK(m_lock);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

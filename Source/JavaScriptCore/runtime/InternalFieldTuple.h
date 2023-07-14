#pragma once

#include "JSInternalFieldObjectImpl.h"

namespace JSC {

// InternalFieldTuple is a generic InternalFieldObject that currently has two internal fields
// It is used in
// - On the global object to store a read/write ref to Bun's AsyncLocalStorage context
// - Within PromiseOperations.js for AsyncLocalStorage related stuff
class InternalFieldTuple : public JSInternalFieldObjectImpl<2> {
protected:
    JS_EXPORT_PRIVATE InternalFieldTuple(VM&, Structure*);
    DECLARE_DEFAULT_FINISH_CREATION;
    DECLARE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE);

public:
    using Base = JSInternalFieldObjectImpl<numberOfInternalFields>;

    template<typename, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.internalFieldTupleSpace();
    }

    static std::array<JSValue, numberOfInternalFields> initialValues()
    {
        return { {
            jsUndefined(),
            jsUndefined(),
        } };
    }

    static InternalFieldTuple* create(JSGlobalObject* globalObject, VM& vm, Structure* structure, JSValue f1, JSValue f2)
    {
        InternalFieldTuple* fields = new (NotNull, allocateCell<InternalFieldTuple>(vm)) InternalFieldTuple(vm, structure);
        fields->finishCreation(vm);
        fields->m_internalFields[0].set(vm, fields, f1);
        fields->m_internalFields[1].set(vm, fields, f2);
        return fields;
    }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(InternalFieldTupleType, Base::StructureFlags), info());
    }

    inline JSValue getInternalField(unsigned index) const
    {
        ASSERT(index < numInternalSlots);
        return m_internalFields[index].get();
    }

    inline void putInternalField(VM& vm, unsigned index, JSValue value)
    {
        ASSERT(index < numInternalSlots);
        m_internalFields[index].set(vm, this, value);
    }

    DECLARE_EXPORT_INFO;
};

} // namespace JSC

// AbiClassifier.cpp — see AbiClassifier.hpp.

#include "Volt/BackendCore/AbiClassifier.hpp"

bool Volt::Backend::IsAggregate ( const MiddleEnd::TypeSystem::TypeStore &Store, MiddleEnd::TypeSystem::LayoutId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    return MiddleEnd::TypeSystem::KindOf( Store.Get( Id ) ) == MiddleEnd::TypeSystem::LayoutKind::Aggregate;
}

Volt::Backend::EParamClass Volt::Backend::ClassifyParam ( const MiddleEnd::TypeSystem::TypeStore &Store,
                                                          MiddleEnd::TypeSystem::LayoutId Id )
{
    return IsAggregate( Store, Id ) ? EParamClass::ByAddress : EParamClass::Scalar;
}

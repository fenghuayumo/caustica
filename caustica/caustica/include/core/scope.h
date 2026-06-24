#pragma once

#include <utility>

// Generic RAII helper — moved from editor/SampleCommon/SampleCommon.h
#define TOKEN_COMBINE1(X,Y) X##Y
#define TOKEN_COMBINE(X,Y) TOKEN_COMBINE1(X,Y)

template<typename AcquireType, typename FinalizeType>
class GenericScope
{
    FinalizeType m_finalize;
public:
    GenericScope(AcquireType&& acquire, FinalizeType&& finalize)
        : m_finalize(std::move(finalize)) { acquire(); }
    ~GenericScope() { m_finalize(); }
};

#define RAII_SCOPE(enter, leave) \
    GenericScope TOKEN_COMBINE(_generic_raii_scopevar_, __COUNTER__)([&](){ enter }, [&](){ leave });

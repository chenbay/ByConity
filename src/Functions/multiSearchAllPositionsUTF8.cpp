#include "FunctionsMultiStringPosition.h"
#include "FunctionFactory.h"
#include "MultiSearchAllPositionsImpl.h"
#include "PositionImpl.h"


namespace DB
{
namespace
{

struct NameMultiSearchAllPositionsUTF8
{
    static constexpr auto name = "multiSearchAllPositionsUTF8";
};

using FunctionMultiSearchAllPositionsUTF8
    = FunctionsMultiStringPosition<MultiSearchAllPositionsImpl<PositionCaseSensitiveUTF8>, NameMultiSearchAllPositionsUTF8>;

}

REGISTER_FUNCTION(MultiSearchAllPositionsUTF8)
{
    factory.registerFunction<FunctionMultiSearchAllPositionsUTF8>();
}

}

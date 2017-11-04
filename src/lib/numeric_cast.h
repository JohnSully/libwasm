#pragma once
#include <limits>

class numeric_cast_exception
{};

template<typename T_DST, typename T_SRC>
T_DST numeric_cast(T_SRC src)
{
	static_assert(!std::is_same<T_SRC, T_DST>::value, "Tautological cast");
	T_DST dst = static_cast<T_DST>(src);
	if (dst != src)
		throw 0;
	return dst;
}
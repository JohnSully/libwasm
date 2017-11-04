#pragma once
#include <limits>

class numeric_cast_exception
{};

template<typename T_DST, typename T_SRC>
T_DST numeric_cast_base(T_SRC src)
{
	T_DST dst = static_cast<T_DST>(src);
	if (dst != src)
		throw 0;
	return dst;
}

template<typename T_DST, typename T_SRC>
T_DST numeric_cast_dispatch(T_SRC src, std::true_type)
{
	static_assert(!std::is_same<T_SRC, T_DST>::value, "Tautological cast");
    return numeric_cast_base<T_DST>(src);
}

template<typename T_DST, typename T_SRC>
T_DST numeric_cast_dispatch(T_SRC src, std::false_type)
{
    return numeric_cast_base<T_DST>(src);
}

template<typename T_DST, bool fCheckTautoligical = true, typename T_SRC>
T_DST numeric_cast(T_SRC &&src)
{
    return numeric_cast_dispatch<T_DST, T_SRC>(src, std::integral_constant<bool, fCheckTautoligical>());
}


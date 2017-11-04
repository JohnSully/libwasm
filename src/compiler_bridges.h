#pragma once
#ifndef _MSC_VER
template <typename T, std::size_t N>
constexpr std::size_t _countof(T const (&)[N]) noexcept
{
        return N;
}
#define UNREFERENCED_PARAMETER(param) do { (void)(param); } while(0)
#endif

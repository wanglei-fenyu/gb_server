#pragma once 
#include <gbnet/buffer/buffer.h>
#include "network/io/message_meta.h"
#include <tuple>
#include <type_traits>
#include <utility>
namespace gb
{
	void GetMsgData(Meta& meta, ReadBufferPtr buffer, int meta_size, int64_t data_size, std::string& out_s);

	// ---------------------------------------------------------------------------
	// function_traits — 从任意可调用对象中提取退化后的参数类型
	// ---------------------------------------------------------------------------
	template <typename F>
	struct function_traits : function_traits<decltype(&std::decay_t<F>::operator())> {};

	template <typename R, typename... Args>
	struct function_traits<R (*)(Args...)>
	{
		using args_tuple = std::tuple<std::decay_t<Args>...>;
		static constexpr std::size_t arity = sizeof...(Args);
	};

	template <typename R, typename C, typename... Args>
	struct function_traits<R (C::*)(Args...)> : function_traits<R (*)(Args...)>
	{
	};

	template <typename R, typename C, typename... Args>
	struct function_traits<R (C::*)(Args...) const> : function_traits<R (*)(Args...)>
	{
	};

	// ---------------------------------------------------------------------------
	// tuple_tail_t<Skip, Tuple> — 跳过前Skip个元素的子元组
	// ---------------------------------------------------------------------------
	namespace detail
	{
		template <std::size_t Skip, typename Tuple, std::size_t... Is>
		auto tuple_tail_impl(std::index_sequence<Is...>)
			-> std::tuple<std::tuple_element_t<Skip + Is, Tuple>...>;
	} // namespace detail

	template <std::size_t Skip, typename Tuple>
	using tuple_tail_t = decltype(detail::tuple_tail_impl<Skip, Tuple>(
		std::make_index_sequence<std::tuple_size_v<Tuple> - Skip>{}));

}

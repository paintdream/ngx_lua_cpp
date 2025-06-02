// ngx_lua_cpp.h
// PaintDream (paintdream@paintdream.com)
// 2023-07-02
//

#pragma once

#ifdef NGX_LUA_CPP_EXPORT
	#ifdef __GNUC__
		#define NGX_LUA_CPP_API __attribute__ ((visibility ("default")))
	#else
		#define NGX_LUA_CPP_API __declspec(dllexport) // Note: actually gcc seems to also supports this syntax.
	#endif
#else
	#ifdef __GNUC__
		#define NGX_LUA_CPP_API __attribute__ ((visibility ("default")))
	#else
		#define NGX_LUA_CPP_API __declspec(dllimport) // Note: actually gcc seems to also supports this syntax.
	#endif
#endif

struct lua_State;
namespace iris {
	int ngx_lua_cpp_resume(lua_State* L, int narg);
	int ngx_lua_cpp_yield(lua_State* L, int narg);
}

#define IRIS_LUA_RESUME ngx_lua_cpp_resume
#define IRIS_LUA_YIELD ngx_lua_cpp_yield

#include "iris/iris_lua.h"
#include "iris/iris_dispatcher.h"
#include "iris/iris_coroutine.h"

namespace iris {
	using async_worker_t = iris_async_worker_t<>;
	using warp_t = iris_warp_t<async_worker_t>;
	using lua_t = iris_lua_t;
	template <typename return_t>
	using coroutine_t = iris_coroutine_t<return_t>;

	template <typename>
	struct is_non_void_iris_coroutine_instance : std::false_type {};

	template <typename return_t>
	struct is_non_void_iris_coroutine_instance<coroutine_t<return_t>> : std::bool_constant<!std::is_void_v<return_t>> {};

	template <typename return_t>
	struct is_coroutine_non_void_return_t : std::false_type {};

	template <typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(*)(args_t...)>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	template <typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(args_t...)>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	template <typename class_t, typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(class_t::*)(args_t...)>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	template <typename class_t, typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(class_t::*)(args_t...) const>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	template <typename type_t>
	struct iris_lua_traits_t<type_t, std::enable_if_t<is_coroutine_non_void_return_t<type_t>::value>> {
		using type = iris_lua_traits_t<type_t>;
		static constexpr bool value = true;

		template <auto ptr, typename executor_t, typename... args_t>
		static void to_lua(lua_State* L, std::nullptr_t, executor_t&& executor, args_t&&... args) {
			executor(L, std::forward<args_t>(args)...);
			lua_getglobal(L, "__ngx_iris_wrap_coroutine_with_returns__");
			lua_insert(L, -2);
			lua_call(L, 1, 1);
		}
	};

	struct ngx_lua_cpp_t {
	public:
		ngx_lua_cpp_t();
		~ngx_lua_cpp_t() noexcept;

		static void lua_registar(lua_t lua, iris_lua_traits_t<ngx_lua_cpp_t>);
		lua_t::optional_result_t<void> start(size_t thread_count);
		lua_t::optional_result_t<void> stop();
		bool is_running() const noexcept;
		size_t get_hardware_concurrency() const noexcept;
		// example async demo: sleep
		coroutine_t<size_t> sleep(size_t milliseconds);
		async_worker_t& get_async_worker() noexcept { return async_worker; }

	protected:
		void notify();
		void process_events();
		void stop_impl();
		friend struct ngx_hooker_t;

	protected:
		async_worker_t async_worker;
		std::unique_ptr<warp_t> script_warp;
		std::unique_ptr<warp_t::preempt_guard_t> script_warp_guard;
		size_t main_thread_index = ~(size_t)0;
	};
}


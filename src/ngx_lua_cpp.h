/*
ngx_lua_cpp.h

The MIT License (MIT)

Copyright (c) 2025 PaintDream

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

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
	struct ngx_warp_t : iris_warp_t<iris_async_worker_t<>, false, ngx_warp_t> {
		using base_t = iris_warp_t<iris_async_worker_t<>, false, ngx_warp_t>;
		template <typename... args_t>
		ngx_warp_t(args_t&&... args) : base_t(std::forward<args_t>(args)...) {}
		static ngx_warp_t* get_current() noexcept {
			return static_cast<ngx_warp_t*>(base_t::get_current());
		}

		void enter_warp() {}
		void leave_warp() {}

		size_t enter_join_warp() {
			return 0;
		}

		size_t leave_join_warp() {
			return 0;
		}

		void suspend_warp() {}
		void resume_warp() {}
		void flush_warp();
	};

	struct ngx_lua_cpp_t {
	public:
		ngx_lua_cpp_t();
		~ngx_lua_cpp_t() noexcept;

		static void lua_registar(iris_lua_t lua, iris_lua_traits_t<ngx_lua_cpp_t>);
		iris_lua_t::optional_result_t<void> start(size_t thread_count);
		iris_lua_t::optional_result_t<void> stop();
		bool is_running() const noexcept;
		size_t get_hardware_concurrency() const noexcept;
		// example async demo: sleep
		iris_coroutine_t<size_t> sleep(size_t milliseconds);
		std::shared_ptr<iris_async_worker_t<>> get_async_worker() noexcept { return async_worker; }
		
		// inspect internal
		void* __async_worker__(void* new_async_worker_ptr);

	protected:
		bool set_async_worker(std::shared_ptr<iris_async_worker_t<>> worker);
		void process_events();
		void stop_impl();
		void reset_main_warp();
		friend struct ngx_hooker_t;

	protected:
		std::shared_ptr<iris_async_worker_t<>> async_worker;
		std::unique_ptr<ngx_warp_t> main_warp;
		std::unique_ptr<ngx_warp_t::preempt_guard_t> main_warp_guard;
		size_t main_thread_index = ~(size_t)0;
	};

	template <typename>
	struct is_non_void_iris_coroutine_instance : std::false_type {};

	template <typename return_t>
	struct is_non_void_iris_coroutine_instance<iris_coroutine_t<return_t>> : std::bool_constant<!std::is_void_v<return_t>> {};

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

	extern int ngx_iris_wrap_coroutine_with_returns_key;
	template <typename type_t>
	struct iris_lua_traits_t<type_t, std::enable_if_t<is_coroutine_non_void_return_t<type_t>::value>> {
		using type = iris_lua_traits_t<type_t>;
		static constexpr bool value = true;

		template <auto ptr, typename executor_t, typename... args_t>
		static iris_lua_t::reflection_t lua_tostack(lua_State* L, std::nullptr_t, executor_t&& executor, args_t&&... args) {
			executor(L, std::forward<args_t>(args)...);
			lua_pushlightuserdata(L, &ngx_iris_wrap_coroutine_with_returns_key);
			lua_rawget(L, LUA_REGISTRYINDEX);
			lua_insert(L, -2);
			lua_call(L, 1, 1);

			return &iris_lua_t::reflection<ptr, executor_t, args_t...>;
		}
	};
}


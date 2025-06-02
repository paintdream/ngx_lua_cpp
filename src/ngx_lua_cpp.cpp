/*
ngx_lua_cpp.cpp

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

#include "ngx_lua_cpp.h"
#include "iris/iris_common.inl"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace iris {
	int ngx_iris_wrap_coroutine_with_returns_key;
	// minimal forward declaration, modify if nginx header changes
	using ngx_int_t = int;
	using ngx_uint_t = unsigned int;
	using ngx_msec_t = uintptr_t;
	struct ngx_event_t;
	struct ngx_connection_t;
	struct ngx_cycle_t;
	struct ngx_http_request_t {
		uint32_t signature;
		ngx_connection_t* connection;
		void** ctx;
		// ...
	};

	struct ngx_log_t;
	typedef uint8_t* (*ngx_log_handler_pt) (ngx_log_t* log, uint8_t* buf, size_t len);
	struct ngx_stream_session_t {
		uint32_t signature;
		ngx_connection_t* connection;
		off_t received;
		time_t start_sec;
		ngx_msec_t start_msec;
		ngx_log_handler_pt log_handler;
		void** ctx;
	};

	struct ngx_stream_lua_request_t {
		ngx_connection_t* connection;
		ngx_stream_session_t* session;
		// ...
	};

	typedef void (*ngx_event_handler_pt)(ngx_event_t* ev);
	typedef ngx_int_t(*ngx_http_handler_pt)(ngx_http_request_t* r);
	typedef void (*ngx_stream_lua_cleanup_pt)(void* data);

	struct ngx_module_t {
		uint32_t ctx_index;
		// ...
	};

	struct ngx_http_lua_co_ctx_t;
	struct ngx_stream_lua_co_ctx_t;
	struct ngx_http_lua_ctx_t;
	struct ngx_stream_lua_ctx_t;

	struct ngx_event_actions_t {
		ngx_int_t (*add)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*del)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*enable)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*disable)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*add_conn)(ngx_connection_t* c);
		ngx_int_t (*del_conn)(ngx_connection_t* c, ngx_uint_t flags);
		ngx_int_t (*notify)(ngx_event_handler_pt handler);
		ngx_int_t (*process_events)(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags);
		ngx_int_t (*init)(ngx_cycle_t* cycle, ngx_msec_t timer);
		void (*done)(ngx_cycle_t* cycle);
	};

	struct ngx_queue_t {
		ngx_queue_t* prev;
		ngx_queue_t* next;
	};

	static void ngx_queue_insert_tail(ngx_queue_t* h, ngx_queue_t* x) {
		x->prev = h->prev;
		x->prev->next = x;
		x->next = h;
		h->prev = x;
	}

	static void ngx_queue_remove(ngx_queue_t* x) {
		x->next->prev = x->prev;
		x->prev->next = x->next;
	}

	// let's go
	struct ngx_hooker_t {
		static ngx_hooker_t& get_instance() {
			static ngx_hooker_t instance;
			return instance;
		}

		static void* get_host_module_handle() {
#ifdef _WIN32
			return ::GetModuleHandleA(nullptr);
#else
			return ::dlopen(nullptr, RTLD_NOLOAD);
#endif
		}

		template <typename T>
		static void get_proc_address(T*& target, void* host, const char* name) {
			if (target == nullptr) {
#ifdef _WIN32
				target = reinterpret_cast<T*>(::GetProcAddress((HMODULE)host, name));
#else
				target = reinterpret_cast<T*>(::dlsym(host, name));
#endif
			}
		}

		ngx_hooker_t() {
			void* host = get_host_module_handle();
			get_proc_address(actions, host, "ngx_event_actions");
			get_proc_address(ngx_posted_delayed_events, host, "ngx_posted_delayed_events");
			get_proc_address(ngx_http_lua_module, host, "ngx_http_lua_module");
			get_proc_address(ngx_stream_lua_module, host, "ngx_stream_lua_module");
			get_proc_address(ngx_http_lua_get_co_ctx, host, "ngx_http_lua_get_co_ctx");
			get_proc_address(ngx_stream_lua_get_co_ctx, host, "ngx_stream_lua_get_co_ctx");

			prev_ngx_process_events = actions->process_events;
			actions->process_events = &ngx_hooker_t::proxy_ngx_process_events;
		}

		static int get_coroutine_returns(lua_State* L) {
			if (!lua_isnone(L, 1)) {
				// not actually yielded, returns all parameter as return values
				return lua_gettop(L);
			}

			lua_pushlightuserdata(L, L);
			lua_rawget(L, LUA_REGISTRYINDEX);
			if (lua_isnil(L, -1)) {
				return luaL_error(L, "No coroutine return value collected!");
			}

			lua_pushlightuserdata(L, L);
			lua_pushnil(L);
			lua_rawset(L, LUA_REGISTRYINDEX);

			int table_index = lua_absindex(L, -1);
			lua_rawgeti(L, table_index, 1);
			int nrets = lua_tointeger(L, -1);
			lua_pop(L, 1);

			for (int i = 1; i <= nrets; i++) {
				lua_rawgeti(L, table_index, i + 1);
			}

			return nrets;
		}

		void registar(iris_lua_t lua) {
			const std::string_view wrapper = 
				"local get_coroutine_returns = ..."
				"return function (func)\n"
				"	return function (...)\n"
				"		return get_coroutine_returns(func(...))\n"
				"	end\n"
				"end\n";
			lua.set_registry(static_cast<const void*>(&ngx_iris_wrap_coroutine_with_returns_key), lua.call<iris_lua_t::ref_t>(lua.load(wrapper, "=(ngx_lua_cpp)"), &get_coroutine_returns));

			lua_State* L = lua.get_state();
			lua_getglobal(L, "ngx");
			lua_getfield(L, -1, "config");
			if (lua_istable(L, -1)) {
				lua_getfield(L, -1, "subsystem");
				const char* subsystem = lua_tostring(L, -1);

				lua_pushlightuserdata(L, this);
				lua_pushboolean(L, strcmp(subsystem, "stream") == 0);
				lua_rawset(L, LUA_REGISTRYINDEX);
				lua_pop(L, 3);
			} else {
				lua_pop(L, 2);
			}
		}

		void insert(ngx_lua_cpp_t* bridge) {
			iris::iris_binary_insert(cpp_list, bridge);
		}

		void remove(ngx_lua_cpp_t* bridge) {
			auto it = iris::iris_binary_find(cpp_list.begin(), cpp_list.end(), bridge);
			if (it != cpp_list.end()) {
				cpp_list.erase(it);
			}
		}

		void notify() {
			if (notified.exchange(1, std::memory_order_relaxed) == 0) {
				if (actions->notify != nullptr) {
					actions->notify(&ngx_hooker_t::ngx_event_handler);
				}
			}
		}

		ngx_http_lua_co_ctx_t* get_http_co_ctx(lua_State* L) {
			auto* http_request = reinterpret_cast<ngx_http_request_t*>(lua_getexdata(L));
			if (http_request != nullptr) {
				ngx_http_lua_ctx_t* ctx = static_cast<ngx_http_lua_ctx_t*>(http_request->ctx[ngx_http_lua_module->ctx_index]);
				if (ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua context");
				}

				ngx_http_lua_co_ctx_t* co_ctx = ngx_http_lua_get_co_ctx(L, ctx);
				if (co_ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua coroutine context");
				}

				return co_ctx;
			} else {
				return nullptr;
			}
		}

		ngx_stream_lua_co_ctx_t* get_stream_co_ctx(lua_State* L) {
			auto* stream_request = reinterpret_cast<ngx_stream_lua_request_t*>(lua_getexdata(L));
			if (stream_request != nullptr) {
				ngx_stream_lua_ctx_t* ctx = static_cast<ngx_stream_lua_ctx_t*>(stream_request->session->ctx[ngx_stream_lua_module->ctx_index]);
				if (ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua context");
				}

				ngx_stream_lua_co_ctx_t* co_ctx = ngx_stream_lua_get_co_ctx(L, ctx);
				if (co_ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua coroutine context");
				}

				return co_ctx;
			} else {
				return nullptr;
			}
		}

		bool is_http_context(lua_State* L) {
			lua_pushlightuserdata(L, this);
			lua_rawget(L, LUA_REGISTRYINDEX);
			bool is_http = !lua_toboolean(L, -1);
			lua_pop(L, 1);

			return is_http;
		}

		int ngx_lua_cpp_yield(lua_State* L, int narg) {
			int (*func)(lua_State*) = nullptr;

			if (is_http_context(L)) {
				ngx_http_lua_co_ctx_t* http_lua_co_ctx = get_http_co_ctx(L);
				if (http_lua_co_ctx != nullptr) {
					func = require_ngx_sleep(L, http_lua_co_ctx, ngx_http_lua_yield, offset_http_co_ctx_event_queue);
					pending_lua_http_co_ctxs.push_back(http_lua_co_ctx);
				} else {
					return luaL_error(L, "Unexpected ngx_lua_cpp http context");
				}
			} else {
				ngx_stream_lua_co_ctx_t* stream_lua_co_ctx = get_stream_co_ctx(L); // assuming no stream request
				if (stream_lua_co_ctx != nullptr) {
					func = require_ngx_sleep(L, stream_lua_co_ctx, ngx_stream_lua_yield, offset_stream_co_ctx_event_queue);
					pending_stream_http_co_ctxs.push_back(stream_lua_co_ctx);
				} else {
					return luaL_error(L, "Unexpected ngx_lua_cpp stream context");
				}
			}

			lua_settop(L, 0);
			lua_pushnumber(L, 0.0);
			return func(L);
		}

		int ngx_lua_cpp_resume(lua_State* L, int nrets) {
			ngx_queue_t* p = nullptr;
			if (is_http_context(L)) {
				ngx_http_lua_co_ctx_t* http_lua_co_ctx = get_http_co_ctx(L);
				if (http_lua_co_ctx != nullptr) {
					p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(http_lua_co_ctx) + offset_http_co_ctx_event_queue);
				} else {
					return LUA_ERRERR;
				}
			} else {
				ngx_stream_lua_co_ctx_t* stream_lua_co_ctx = get_stream_co_ctx(L);
				if (stream_lua_co_ctx != nullptr) {
					p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(stream_lua_co_ctx) + offset_stream_co_ctx_event_queue);
				} else {
					return LUA_ERRERR;
				}
			}

			ngx_queue_insert_tail(ngx_posted_delayed_events, p);

			if (nrets != 0) {
				lua_pushlightuserdata(L, L);
				lua_createtable(L, nrets + 1, 0);
				lua_pushinteger(L, nrets);
				lua_rawseti(L, -2, 1);

				for (int i = 1; i <= nrets; i++) {
					lua_pushvalue(L, i);
					lua_rawseti(L, -2, i + 1);
				}

				lua_rawset(L, LUA_REGISTRYINDEX);
				lua_pop(L, nrets);
			}

			return LUA_OK;
		}

	private:
		struct GCfuncC {
			void* nextgc;
			uint8_t marked;
			uint8_t gct;
			uint8_t ffid;
			uint8_t nupvalues;
			void* env;
			void* gclist;
			void* pc;
			int (*f)(lua_State*);
		};

		auto require_ngx_sleep(lua_State* L, void* co_ctx, int (*&f)(lua_State*), int& offset) -> int (*)(lua_State*) {
			if (f != nullptr) {
				return f;
			}

			ngx_queue_t* prev = ngx_posted_delayed_events->prev;
			lua_getglobal(L, "ngx");
			lua_getfield(L, -1, "sleep");
			f = reinterpret_cast<const GCfuncC*>(lua_topointer(L, -1))->f;

			// get offset for ngx_http_lua_co_ctx_t::sleep::event_queue
			lua_pushcclosure(L, f, 0);
			lua_pushnumber(L, 0.0);
			int result = lua_pcall(L, 1, 0, 0);
			if (result != LUA_OK) {
				lua_pop(L, 1);
			}

			ngx_queue_t* current = ngx_posted_delayed_events->prev;
			if (current != prev) {
				offset = reinterpret_cast<uintptr_t>(current) - reinterpret_cast<uintptr_t>(co_ctx);
				return f;
			} else {
				return nullptr;
			}
		}

		static ngx_int_t proxy_ngx_process_events(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags) {
			return ngx_hooker_t::get_instance().process_events(cycle, timer, flags);
		}

		static void ngx_event_handler(ngx_event_t* ev) {
			return ngx_hooker_t::get_instance().event_handler(ev);
		}

		ngx_int_t process_events(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags) {
			// dig out sleep placeholder events...
			if (offset_http_co_ctx_event_queue != 0) {
				for (auto* co_ctx : pending_lua_http_co_ctxs) {
					ngx_queue_t* p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(co_ctx) + offset_http_co_ctx_event_queue);
					ngx_queue_remove(p);
				}
			}

			pending_lua_http_co_ctxs.clear();

			if (offset_stream_co_ctx_event_queue != 0) {
				for (auto* co_ctx : pending_stream_http_co_ctxs) {
					ngx_queue_t* p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(co_ctx) + offset_stream_co_ctx_event_queue);
					ngx_queue_remove(p);
				}
			}

			pending_stream_http_co_ctxs.clear();

			do {
				for (ngx_lua_cpp_t* p : cpp_list) {
					p->process_events();
				}
			} while (notified.exchange(0, std::memory_order_relaxed) == 1);

			if (actions->notify == nullptr) {
				// if target platform does not support notify(), then modify timer interval (win32).
				timer = std::min(timer, ngx_msec_t(16u));
			}

			return prev_ngx_process_events(cycle, timer, flags);
		}

		static void event_handler(ngx_event_t* ev) {}

		ngx_int_t(*prev_ngx_process_events)(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags) = nullptr;
		std::vector<ngx_lua_cpp_t*> cpp_list;
		std::atomic<size_t> notified = 0;
		ngx_event_actions_t* actions = nullptr;
		ngx_module_t* ngx_http_lua_module = nullptr;
		ngx_module_t* ngx_stream_lua_module = nullptr;
		ngx_http_lua_co_ctx_t* (*ngx_http_lua_get_co_ctx)(lua_State* L, ngx_http_lua_ctx_t* ctx) = nullptr;
		ngx_stream_lua_co_ctx_t* (*ngx_stream_lua_get_co_ctx)(lua_State* L, ngx_stream_lua_ctx_t* ctx) = nullptr;
		int (*ngx_http_lua_yield)(lua_State*) = nullptr;
		int (*ngx_stream_lua_yield)(lua_State*) = nullptr;
		std::vector<ngx_http_lua_co_ctx_t*> pending_lua_http_co_ctxs;
		std::vector<ngx_stream_lua_co_ctx_t*> pending_stream_http_co_ctxs;
		int offset_http_co_ctx_event_queue = 0;
		int offset_stream_co_ctx_event_queue = 0;
		ngx_queue_t* ngx_posted_delayed_events = nullptr;
	};

	ngx_lua_cpp_t::ngx_lua_cpp_t() : async_worker(std::make_shared<iris_async_worker_t<>>()) {
		ngx_hooker_t::get_instance().insert(this);
		reset_main_warp();

		async_worker->set_priority_task_handler([this](iris_async_worker_t<>::task_base_t* task, size_t& priority) {
			main_warp->queue_routine([this, task]() {
				async_worker->execute_task(task);
			});

			return true;
		});
	}

	void ngx_lua_cpp_t::reset_main_warp() {
		if (main_warp_guard) {
			main_warp_guard.reset();
		}

		main_warp = std::make_unique<ngx_warp_t>(*async_worker);
		main_warp_guard = std::make_unique<ngx_warp_t::preempt_guard_t>(*main_warp, 0);
	}

	ngx_lua_cpp_t::~ngx_lua_cpp_t() noexcept {
		if (is_running()) {
			stop_impl();
		}

		ngx_hooker_t::get_instance().remove(this);
	}

	iris_lua_t::optional_result_t<void> ngx_lua_cpp_t::start(size_t thread_count) {
		if (async_worker->get_current_thread_index() != ~(size_t)0) {
			return iris_lua_t::result_error_t("ngx_lua_cpp_t::start(thread_count) -> incorrect current thread, please call me in main thread.");
		}

		if (is_running()) {
			return iris_lua_t::result_error_t("ngx_lua_cpp_t::start(thread_count) -> already started.");
		}

		if (thread_count > std::thread::hardware_concurrency() * 4) {
			thread_count = std::thread::hardware_concurrency() * 4;
		}

		async_worker->resize(thread_count);
		main_thread_index = async_worker->append(std::thread()); // for main thread polling
		async_worker->start();

		if (!ngx_warp_t::is_strand) {
			reset_main_warp();
		}

		return {};
	}

	iris_lua_t::optional_result_t<void> ngx_lua_cpp_t::stop() {
		if (!is_running()) {
			return iris_lua_t::result_error_t("ngx_lua_cpp_t::stop() -> not started.");
		}

		stop_impl();
		return {};
	}

	void ngx_lua_cpp_t::stop_impl() {
		async_worker->terminate();
		async_worker->join();

		// manually polling events
		while (main_warp->poll()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		main_thread_index = ~(size_t)0;
		reset_main_warp();
	}

	bool ngx_lua_cpp_t::is_running() const noexcept {
		return !async_worker->is_terminated();
	}

	iris_coroutine_t<size_t> ngx_lua_cpp_t::sleep(size_t millseconds) {
		ngx_warp_t* current = co_await iris_switch<ngx_warp_t>(nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(millseconds));
		co_await iris_switch(current);
		co_return std::move(millseconds);
	}

	size_t ngx_lua_cpp_t::get_hardware_concurrency() const noexcept {
		return std::thread::hardware_concurrency();
	}

	void ngx_lua_cpp_t::lua_registar(iris_lua_t lua, iris_lua_traits_t<ngx_lua_cpp_t>) {
		ngx_hooker_t::get_instance().registar(lua);

		lua.set_current<&ngx_lua_cpp_t::start>("start");
		lua.set_current<&ngx_lua_cpp_t::stop>("stop");
		lua.set_current<&ngx_lua_cpp_t::is_running>("is_running");
		lua.set_current<&ngx_lua_cpp_t::get_hardware_concurrency>("get_hardware_concurrency");
		lua.set_current<&ngx_lua_cpp_t::sleep>("sleep");

		lua.set_current<&ngx_lua_cpp_t::__async_worker__>("__async_worker__");
	}

	void* ngx_lua_cpp_t::__async_worker__(void* new_async_worker_ptr) {
		if (new_async_worker_ptr != nullptr && set_async_worker(*reinterpret_cast<std::shared_ptr<iris_async_worker_t<>>*>(new_async_worker_ptr))) {
			return new_async_worker_ptr;
		} else {
			return reinterpret_cast<void*>(&async_worker);
		}
	}

	bool ngx_lua_cpp_t::set_async_worker(std::shared_ptr<iris_async_worker_t<>> worker) {
		if (is_running())
			return false;

		std::swap(async_worker, worker);
		reset_main_warp();
		return true;
	}

	void ngx_lua_cpp_t::process_events() {
		if (async_worker->get_thread_count() <= 1 || async_worker->is_terminated()) {
			async_worker->make_current(main_thread_index);
			// if there is no worker threads, try polling from main_thread
			async_worker->poll();
			async_worker->make_current(~(size_t)0);
		}

		main_warp->poll<false>();
	}

	void ngx_warp_t::flush_warp() {
		ngx_hooker_t::get_instance().notify();
	}

	int ngx_lua_cpp_resume(lua_State* L, int narg) {
		return ngx_hooker_t::get_instance().ngx_lua_cpp_resume(L, narg);
	}

	int ngx_lua_cpp_yield(lua_State* L, int narg) {
		return ngx_hooker_t::get_instance().ngx_lua_cpp_yield(L, narg);
	}
}

extern "C" NGX_LUA_CPP_API int luaopen_ngx_lua_cpp(lua_State* L) {
	return iris::iris_lua_t::forward(L, [](iris::iris_lua_t lua) {
		return lua.make_type<iris::ngx_lua_cpp_t>("ngx_lua_cpp");
	});
}


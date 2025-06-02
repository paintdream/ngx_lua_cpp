# ngx_lua_cpp
Developing OpenResty ngx_lua extensions with C++ (including coroutines). Based on [iris](https://github.com/paintdream/iris) library.

## License

ngx_lua_cpp is distributed under MIT License.

## Build

ngx_lua_cpp requires a C++ 20 compatible compiler (Visual Studio 2019+, GCC 11+, Clang 14+).

Just use CMake to generate project with CMakeLists.txt.

If you got problem in finding LuaJIT, please configure its installation path (usually it could be found at OpenResty's installation directory) to environment variable LUA_DIR.

Notice that ngx_lua_cpp is an isolated component for LuaJIT. You do not need any header files from OpenResty. All related OpenResty/Nginx declarations could be find at ngx_lua_cpp.cpp. Modify these declarations if it is incompatible with your custom OpenResty build. (Usually you needn't do this.)

## Install

Configure **nginx.conf**, add these lines to your stream/http block:

```lua
	init_worker_by_lua_block {
		package.path = package.path .. ";[[ngx_lua_cpp source directory]]/demo/?.lua"
		package.cpath = package.cpath .. ";[[ngx_lua_cpp binary directory]]/?.dll;[[ngx_lua_cpp binary directory]]/lib?.so;"
		require("init_ngx_lua_cpp")
	}
```

**init_gnx_lua_cpp.lua** is a lua module shared by all requests. It initializes a **ngx_lua_cpp** instance and return it as its module instance. 

```lua
-- init_ngx_lua_cpp.lua

local lib = require("ngx_lua_cpp")
if lib then
	local inst = lib.new()
	inst:start(4) -- thread count
	return inst
else
	ngx.log(ngx.ERR, "ngx_lua_cpp not found!")
end

```

For http, configure at **http/server** block:

```lua
	location ~ /lua.html {
		default_type text/html;
		content_by_lua_block {
			local inst = require("init_ngx_lua_cpp")
			local value = inst:sleep(40)
			ngx.say("ngx_lua_cpp http demo! Running " .. tostring(inst:is_running()) .. " | " .. tostring(value))
		}
	}
}
```

For stream, configure at **stream** block)

```lua
	server {
		listen 1234;

		content_by_lua_block {
			local inst = require("init_ngx_lua_cpp")
			local value = inst:sleep(40)
			ngx.say("ngx_lua_cpp stream demo! Running " .. tostring(inst:is_running()) .. " | " .. tostring(value))
		}
	}
```

## Usage

In ngx_lua_cpp.cpp, you could see an example function: **a coroutine-based asynchornized "sleep"**.

It is registered in ngx_lua_cpp_t::lua_registar():

```C++
void ngx_lua_cpp_t::lua_registar(lua_t lua, iris_lua_traits_t<ngx_lua_cpp_t>) {
	// ...
	lua.set_current<&ngx_lua_cpp_t::sleep>("sleep");
}
```

Here is the implementation:

```C++
coroutine_t<size_t> ngx_lua_cpp_t::sleep(size_t millseconds) {
	warp_t* current = co_await iris_switch<warp_t>(nullptr);
	std::this_thread::sleep_for(std::chrono::milliseconds(millseconds));
	co_await iris_switch(current);
	co_return std::move(millseconds);
}
```

You can use coroutines and the warp (strand) system from the iris library, which are fully compatible with OpenResty/Nginx's task scheduler.
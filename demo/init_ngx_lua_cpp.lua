-- to configure nginx.conf
--[[
	init_worker_by_lua_block {
		package.path = package.path .. ";/path/to/lua/?.lua"
		require("init_ngx_lua_cpp")
	}
]]

-- Loads the ngx_lua_cpp native library and starts one instance per worker
-- (thread pool included). The library is located with package.loadlib
-- instead of plain require() because Lua's C loader aborts with an error
-- when the first existing candidate has the wrong architecture (a 32-bit
-- nginx hitting the 64-bit DLL, or vice versa) instead of trying the next
-- cpath pattern. loadlib just returns nil + err for bad files, so we walk
-- the candidates ourselves, ordered by the current process bitness:
--   64-bit:  build64/ (Release first)
--   32-bit:  build/   (Release first, matches e.g. WinNMP's 32-bit OpenResty)
-- plus the Linux .so layouts for both cases.

local prefix = ngx.config.prefix()

local ffi_ok, ffi = pcall(require, "ffi")
local is_64bit = ffi_ok and ffi.sizeof("void *") == 8

local candidates
if is_64bit then
	candidates = {
		prefix .. "../../build64/Release/ngx_lua_cpp.dll",
		prefix .. "../../build64/Debug/ngx_lua_cpp.dll",
		prefix .. "../../build64/ngx_lua_cpp.dll",
		prefix .. "../../build/libngx_lua_cpp.so",
		prefix .. "../../build/Release/libngx_lua_cpp.so",
		prefix .. "../../build/Debug/libngx_lua_cpp.so",
	}
else
	candidates = {
		prefix .. "../../build/Release/ngx_lua_cpp.dll",
		prefix .. "../../build/Debug/ngx_lua_cpp.dll",
		prefix .. "../../build/ngx_lua_cpp.dll",
		prefix .. "../../build/libngx_lua_cpp.so",
		prefix .. "../../build/Release/libngx_lua_cpp.so",
		prefix .. "../../build/Debug/libngx_lua_cpp.so",
	}
end

local lib
for _, path in ipairs(candidates) do
	local loader, err = package.loadlib(path, "luaopen_ngx_lua_cpp")
	if loader then
		lib = loader()
		if lib then
			ngx.log(ngx.INFO, "ngx_lua_cpp loaded from ", path)
			break
		end
	end
end

if lib then
	local inst = lib.new()
	inst:start(4) -- thread count
	return inst
else
	ngx.log(ngx.ERR, "ngx_lua_cpp not found!")
end
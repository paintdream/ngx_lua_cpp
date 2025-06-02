-- to configure nginx.conf
--[[
	init_worker_by_lua_block {
		package.path = package.path .. ";/path/to/lua/?.lua"
		package.cpath = package.cpath .. ";/path/to/luabin/?.dll;/path/toluabin/lib?.so"
		require("init_ngx_lua_cpp")
	}
]]

local lib = require("ngx_lua_cpp")
if lib then
	local inst = lib.new()
	inst:start(4) -- thread count
	return inst
else
	ngx.log(ngx.ERR, "ngx_lua_cpp not found!")
end

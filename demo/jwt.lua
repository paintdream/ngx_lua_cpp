-- jwt.lua - minimal HS256 JWT on top of ngx_lua_cpp crypto primitives
--
-- Demonstrates the API-gateway flow (Demo B4):
--   inst:hmac_sha256_raw() provides the HMAC-SHA256 signature,
--   inst:base64url_encode/decode() handle the wire format, JSON comes from
--   the bundled cjson. No third-party crypto is involved.

local M = {}

local cjson = require("cjson.safe")

-- sign(payload_table, secret, ttl_seconds) -> token string
function M.sign(inst, payload, secret, ttl_seconds)
    payload = payload or {}
    local now = math.floor(ngx.time())
    payload.iat = now
    payload.exp = now + (ttl_seconds or 3600)

    local header = cjson.encode({ alg = "HS256", typ = "JWT" })
    local h = inst:base64url_encode(header)
    local p = inst:base64url_encode(cjson.encode(payload))
    local signing_input = h .. "." .. p
    local sig = inst:base64url_encode(inst:hmac_sha256_raw(secret, signing_input))
    return signing_input .. "." .. sig
end

-- verify(token, secret) -> payload_table | nil, err
function M.verify(inst, token, secret)
    if type(token) ~= "string" then
        return nil, "missing token"
    end
    local h, p, sig = token:match("^(%S+)%.(%S+)%.(%S+)$")
    if not h then
        return nil, "malformed token"
    end

    local expected = inst:base64url_encode(inst:hmac_sha256_raw(secret, h .. "." .. p))
    if expected ~= sig then
        return nil, "bad signature"
    end

    local res = inst:base64url_decode(p)
    local payload_raw, dec_err = res[1], res[2]
    if dec_err ~= "" then
        return nil, "bad payload encoding"
    end
    local ok, payload = pcall(cjson.decode, payload_raw)
    if not ok or type(payload) ~= "table" then
        return nil, "bad payload json"
    end

    local now = math.floor(ngx.time())
    if payload.exp and now >= payload.exp then
        return nil, "token expired"
    end
    if payload.iat and now < payload.iat - 60 then
        return nil, "token not yet valid"
    end
    return payload
end

return M
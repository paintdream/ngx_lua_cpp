-- webhook.lua - HMAC-signed webhook ingest (Demo B5)
--
-- The client signs the raw request body with the shared secret and sends
-- the hex digest in the X-Ngx-Signature header; this module verifies it
-- with inst:hmac_sha256() (C++ worker), then hands the payload to the
-- ngx_lua_cpp job queue. The job id lets the caller track processing.

local M = {}

local cjson = require("cjson.safe")

-- verify_signature(inst, body, signature_header, secret) -> true | false
function M.verify_signature(inst, body, signature_header, secret)
    if type(signature_header) ~= "string" or signature_header == "" then
        return false
    end
    local expected = inst:hmac_sha256(secret, body)
    -- constant-ish time compare
    local a, b = #expected, #signature_header
    if a ~= b then
        return false
    end
    local diff = 0
    for i = 1, a do
        local ca = expected:byte(i)
        local cb = signature_header:byte(i)
        if ca ~= cb then
            diff = diff + 1
        end
    end
    return diff == 0
end

-- ingest(inst, body, secret) -> (ok, job_id, error)
function M.ingest(inst, body, secret)
    local sig_header = ngx.req.get_headers()["X-Ngx-Signature"]
    if not M.verify_signature(inst, body, sig_header, secret) then
        return false, nil, "bad signature"
    end

    local ok, data = pcall(cjson.decode, body)
    if not ok or type(data) ~= "table" then
        return false, nil, "bad json body"
    end

    -- map the webhook payload onto the C++ job queue
    local kind = data.kind or "count_primes"
    local payload = data.payload or {}
    local job_id = inst:job_submit(kind, payload)
    if job_id == "" then
        return false, nil, "job_submit failed (is ngx_lua_cpp running?)"
    end
    inst:pub("webhook", job_id .. "|" .. kind .. "|accepted")
    return true, job_id
end

return M
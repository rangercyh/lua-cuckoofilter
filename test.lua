local inspect = require "inspect"
local cuckoofilter = require "cuckoofilter"

local test_case = {
    fingerprint_size = { 4, 5, 6, 7, 8, 9, 10, 12, 13, 16, 17, 23, 31, 32 },
}

function test()
    local insert_num = 50000
    for _, f in ipairs(test_case.fingerprint_size) do
        local cf = cuckoofilter.new(8190, f)
        local record = {}
        for i = 1, insert_num do
            if cf:add(tostring(i)) then
                record[#record + 1] = tostring(i)
            end
        end
        -- print(inspect(cf:info()))
        assert(cf:size() == #record, "expect size equal to record num"..#record)
        for _, v in pairs(record) do
            assert(cf:contain(v), "expect contain:"..v.."|"..f.."|"..cf:size())
            assert(cf:delete(v), "expect delete succ")
        end
        -- print(inspect(cf:info()))
        assert(cf:size() == 0, "expect size = 0")
    end
end

test()

-- local cf = cuckoofilter.new(8190, 23)
-- local a = {}
-- for i = 1, 50000 do
--     if cf:add(tostring(i)) then
--         a[#a + 1] = tostring(i)
--     end
-- end
-- for _, v in pairs(a) do
--     assert(cf:contain(v), "v not"..v)
-- end

-- local cf = cuckoofilter.new(8190, 23)
-- print("add = ", i, cf:add("1"), cf:contain("1"))
-- print(inspect(cf:info()))

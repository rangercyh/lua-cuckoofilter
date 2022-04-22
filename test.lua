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
        assert(cf:size() == #record, "expect size equal to record num"..#record)
        for _, v in pairs(record) do
            assert(cf:contain(v), "expect contain:"..v.."|"..f.."|"..cf:size())
            assert(cf:delete(v), "expect delete succ")
        end
        assert(cf:size() == 0, "expect size = 0")

        cf:reset()
        local record2 = {}
        for i = 1, insert_num do
            if cf:add_unique(tostring(i)) then
                record2[#record2 + 1] = tostring(i)
            end
        end
        assert(cf:size() == #record2, "expect size equal to record num"..#record2)
        for _, v in pairs(record2) do
            assert(cf:contain(v), "expect contain:"..v.."|"..f.."|"..cf:size())
            assert(cf:delete(v), "expect delete succ")
        end
        assert(cf:size() == 0, "expect size = 0")
    end
end

test()

-- local cf = cuckoofilter.new(-1, 12)
-- local cf = cuckoofilter.new(0, 12)
-- local cf = cuckoofilter.new(1, 3)
-- local cf = cuckoofilter.new(1, 33)

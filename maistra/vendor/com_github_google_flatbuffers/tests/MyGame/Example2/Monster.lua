-- automatically generated by the FlatBuffers compiler, do not modify

-- namespace: Example2

local flatbuffers = require('flatbuffers')

local Monster = {} -- the module
local Monster_mt = {} -- the class metatable

function Monster.New()
    local o = {}
    setmetatable(o, {__index = Monster_mt})
    return o
end
function Monster.GetRootAsMonster(buf, offset)
    if type(buf) == "string" then
        buf = flatbuffers.binaryArray.New(buf)
    end
    local n = flatbuffers.N.UOffsetT:Unpack(buf, offset)
    local o = Monster.New()
    o:Init(buf, n + offset)
    return o
end
function Monster_mt:Init(buf, pos)
    self.view = flatbuffers.view.New(buf, pos)
end
function Monster.Start(builder) builder:StartObject(0) end
function Monster.End(builder) return builder:EndObject() end

return Monster -- return the module
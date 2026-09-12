-- Does the standard library actually work on this target?
--
-- Every library the interpreter registers gets touched here, because the
-- interesting failure is not a wrong answer but a table that is nil: the
-- NuttX build compiles every opener and used to register only one of them,
-- leaving "math" and "string" absent from a Lua that otherwise ran fine.
--
-- Kept free of anything target-specific except the number model, which is
-- 32-bit here by configuration and is checked as such.

local checks = 0

local function ok(condition, what)
  if not condition then
    error("failed: " .. what, 2)
  end
  checks = checks + 1
end

ok(_VERSION == "Lua 5.4", "version")

-- Numbers: 32-bit integers and single-precision floats.
ok(math.type(1) == "integer" and math.type(1.0) == "float", "number types")
ok(math.maxinteger == 2147483647, "integer width")
ok(7 // 2 == 3 and 7 % 2 == 1 and -7 // 2 == -4, "integer arithmetic")

-- Not "2^10 == 1024": NuttX's own libm computes powf as expf(e * logf(b)),
-- which lands about two units in the last place away from an exact power and
-- prints as 1024.0 while comparing unequal to it.  Asserting exactness here
-- would be asserting something about libm, not about Lua.
ok(math.abs(2^10 - 1024) < 0.01, "exponentiation")
ok(math.abs(math.pi - 3.14159) < 0.001, "pi")
ok(math.max(3, 7) == 7 and math.floor(2.7) == 2 and math.tointeger(8.0) == 8,
   "math library")

-- Strings.
ok(string.rep("ab", 3):upper() == "ABABAB", "string methods")
ok(("%d/%s/%.2f"):format(7, "x", 1.5) == "7/x/1.50", "format")
ok(("a,b,,c"):find("b") == 3, "find")
ok(("hello world"):gsub("o", "0") == "hell0 w0rld", "gsub")

-- Tables.
local t = {3, 1, 2}
table.sort(t)
ok(table.concat(t, "-") == "1-2-3", "sort and concat")
table.insert(t, 4)
ok(table.remove(t) == 4 and #t == 3, "insert and remove")

-- utf8.
ok(string.len(utf8.char(233)) == 2 and utf8.codepoint("e") == 101, "utf8")

-- Coroutines.
local co = coroutine.create(function(a) coroutine.yield(a) return a + 1 end)
local running, first = coroutine.resume(co, 41)
local finished, second = coroutine.resume(co)
ok(running and first == 41 and finished and second == 42, "coroutines")

-- Errors.
ok(not pcall(function() error("boom") end), "pcall catches")
local caught = select(2, pcall(function() error({code = 5}) end))
ok(type(caught) == "table" and caught.code == 5, "error values")

-- Metatables.
local proxy = setmetatable({}, {__index = function() return 9 end})
ok(proxy.anything == 9, "metatables")

-- Files, through NuttX.
local path = "/tmp/lua.dat"
local handle = assert(io.open(path, "w"))
handle:write("hello ", 42, "\n")
handle:close()
handle = assert(io.open(path, "r"))
ok(handle:read("l") == "hello 42", "file round trip")
handle:close()
ok(os.remove(path), "remove")
ok(io.open(path, "r") == nil, "removed")

-- os and package.
ok(type(os.time()) == "number" and type(os.clock()) == "number", "os clock")
ok(type(package.path) == "string" and type(require) == "function", "package")

-- Garbage collection.
collectgarbage()
ok(collectgarbage("count") - 0.0 ~= 0.0, "gc")

print("LUA_CHECKS=" .. checks)

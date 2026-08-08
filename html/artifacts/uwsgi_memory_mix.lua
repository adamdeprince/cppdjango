local paths = {
  "/db",
  "/orm-in",
  "/orm-update?queries=1",
}
local request_number = 0

request = function()
  request_number = request_number + 1
  local path = paths[((request_number - 1) % #paths) + 1]
  return wrk.format("GET", path)
end

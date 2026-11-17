-- Arena: two drones on a round platform that shrinks. Move, dash, and fire a
-- knockback pulse; push the other drone off the edge to win the round.
-- First to three rounds wins.
--
-- All numbers are integers (fixed point Q16.16 via `fx`); persistent values
-- live in w.var (per player) and w.global, never in Lua variables.

local ONE = fx.ONE
local START_RADIUS = fx.milli(9000)
local MIN_RADIUS = fx.milli(3500)
local SHRINK_TICKS = 40 * 60      -- from START to MIN over 40 s
local ROUND_TICKS = 60 * 60       -- a round ends in a draw after 60 s
local ROUNDS_TO_WIN = 3
local ACCEL = fx.milli(9)         -- units per tick^2 while steering
local DASH_IMPULSE = fx.milli(320)
local DASH_COOLDOWN = 50
local PULSE_RANGE = fx.milli(2600)
local PULSE_IMPULSE = fx.milli(380)
local PULSE_COOLDOWN = 90

-- per-player vars
local DASH_CD, PULSE_CD, FACING, PULSE_FLASH, FALLEN = 0, 1, 2, 3, 4
-- globals
local ROUND_START, ROUND, PAUSE = 0, 1, 2

w.set_param("friction", fx.milli(965))
w.set_param("restitution", fx.milli(800))
w.set_param("max_speed", fx.milli(450))

local function place()
  w.set_pos(0, -fx.milli(3500), 0)
  w.set_pos(1, fx.milli(3500), 0)
  for i = 0, 1 do
    w.set_vel(i, 0, 0)
    w.set_var(i, DASH_CD, 0)
    w.set_var(i, PULSE_CD, 0)
    w.set_var(i, PULSE_FLASH, 0)
    w.set_var(i, FALLEN, 0)
  end
  w.set_var(0, FACING, 0)
  w.set_var(1, FACING, fx.TURN // 2)
  w.set_arena_radius(START_RADIUS)
  w.set_global(ROUND_START, w.frame())
end

function init()
  w.set_global(ROUND, 1)
  place()
end

-- direction code 1..8 -> angle (E, NE, N, ... in eighths of a turn)
local function dir_angle(code)
  return (code - 1) * (fx.TURN // 8)
end

function tick()
  if w.global(PAUSE) > 0 then
    w.set_global(PAUSE, w.global(PAUSE) - 1)
    if w.global(PAUSE) == 0 then place() end
    return
  end
  local elapsed = w.frame() - w.global(ROUND_START)
  -- the platform shrinks linearly
  local shrink = elapsed < SHRINK_TICKS and elapsed or SHRINK_TICKS
  w.set_arena_radius(START_RADIUS - (START_RADIUS - MIN_RADIUS) * shrink // SHRINK_TICKS)
  for i = 0, 1 do
    local input = w.input(i)
    local dir = input % 16
    for _, k in ipairs({DASH_CD, PULSE_CD, PULSE_FLASH}) do
      if w.var(i, k) > 0 then w.set_var(i, k, w.var(i, k) - 1) end
    end
    if dir >= 1 and dir <= 8 then
      local a = dir_angle(dir)
      w.set_var(i, FACING, a)
      w.add_vel(i, fx.mul(fx.cos(a), ACCEL), fx.mul(fx.sin(a), ACCEL))
    end
    local a = w.var(i, FACING)
    if (input // 16) % 2 == 1 and w.var(i, DASH_CD) == 0 then
      w.add_vel(i, fx.mul(fx.cos(a), DASH_IMPULSE), fx.mul(fx.sin(a), DASH_IMPULSE))
      w.set_var(i, DASH_CD, DASH_COOLDOWN)
    end
    if (input // 32) % 2 == 1 and w.var(i, PULSE_CD) == 0 then
      w.set_var(i, PULSE_CD, PULSE_COOLDOWN)
      w.set_var(i, PULSE_FLASH, 12)
      local j = 1 - i
      local dx, dy = w.px(j) - w.px(i), w.py(j) - w.py(i)
      local d = fx.len(dx, dy)
      if d < PULSE_RANGE and d > 0 then
        -- stronger when closer: impulse * (1 - d / range)
        local k = fx.mul(PULSE_IMPULSE, ONE - fx.div(d, PULSE_RANGE))
        w.add_vel(j, fx.mul(fx.div(dx, d), k), fx.mul(fx.div(dy, d), k))
        w.add_vel(i, -fx.mul(fx.div(dx, d), k // 4), -fx.mul(fx.div(dy, d), k // 4))  -- recoil
      end
    end
  end
end

function after_physics()
  if w.global(PAUSE) > 0 or w.phase() ~= 0 then return end
  local out = {}
  for i = 0, 1 do
    out[i + 1] = fx.len(w.px(i), w.py(i)) > w.arena_radius() + w.radius(i) // 2
  end
  local elapsed = w.frame() - w.global(ROUND_START)
  if out[1] or out[2] or elapsed >= ROUND_TICKS then
    if out[1] and not out[2] then w.add_score(1, 1); w.set_var(0, FALLEN, 1) end
    if out[2] and not out[1] then w.add_score(0, 1); w.set_var(1, FALLEN, 1) end
    for i = 0, 1 do
      if w.score(i) >= ROUNDS_TO_WIN then
        w.finish(i)
        return
      end
    end
    w.set_global(ROUND, w.global(ROUND) + 1)
    w.set_global(PAUSE, 90)  -- 1.5 s before the next round
  end
end

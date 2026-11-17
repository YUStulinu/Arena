//! The C++ engine through its C interface (engine/include/arena/arena.h).

use std::ffi::c_char;

#[repr(C)]
struct ArenaGame {
    _p: [u8; 0],
}

extern "C" {
    fn arena_game_new(script: *const c_char, len: i32, err: *mut c_char, errlen: i32) -> *mut ArenaGame;
    fn arena_game_free(g: *mut ArenaGame);
    fn arena_state_size() -> i32;
    fn arena_initial(g: *mut ArenaGame, state: *mut u8);
    fn arena_step(g: *mut ArenaGame, state: *mut u8, in0: u8, in1: u8) -> i32;
    fn arena_hash(state: *const u8) -> u64;
    fn arena_observe(state: *const u8, out: *mut i32);
}

pub const OBS: usize = 32;

/// A game (a Lua VM with the gameplay script) and one state it advances.
pub struct Game {
    g: *mut ArenaGame,
    state: Vec<u8>,
}

// a Game is only ever used by one thread at a time (each match owns its own)
unsafe impl Send for Game {}

impl Game {
    pub fn new(script: &str) -> Result<Game, String> {
        let mut err = vec![0u8; 512];
        let g = unsafe { arena_game_new(script.as_ptr() as *const c_char, script.len() as i32, err.as_mut_ptr() as *mut c_char, 512) };
        if g.is_null() {
            let end = err.iter().position(|&c| c == 0).unwrap_or(err.len());
            return Err(String::from_utf8_lossy(&err[..end]).into_owned());
        }
        let mut state = vec![0u8; unsafe { arena_state_size() } as usize];
        unsafe { arena_initial(g, state.as_mut_ptr()) };
        Ok(Game { g, state })
    }

    pub fn step(&mut self, a: u8, b: u8) -> Result<(), String> {
        match unsafe { arena_step(self.g, self.state.as_mut_ptr(), a, b) } {
            0 => Ok(()),
            _ => Err("gameplay script error".into()),
        }
    }

    pub fn hash(&self) -> u64 {
        unsafe { arena_hash(self.state.as_ptr()) }
    }

    /// frame, arena radius, phase, winner, ... (see arena.h)
    pub fn observe(&self) -> [i32; OBS] {
        let mut o = [0i32; OBS];
        unsafe { arena_observe(self.state.as_ptr(), o.as_mut_ptr()) };
        o
    }

    pub fn frame(&self) -> i32 {
        self.observe()[0]
    }
    /// None while playing, Some(winner) when the match is over
    pub fn result(&self) -> Option<i32> {
        let o = self.observe();
        (o[2] == 1).then_some(o[3])
    }
}

impl Drop for Game {
    fn drop(&mut self) {
        unsafe { arena_game_free(self.g) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn script() -> String {
        std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../engine/scripts/arena.lua")).unwrap()
    }

    #[test]
    fn reproduces_the_golden_replay() {
        // the same hashes as the g++ build and the Wasm build (engine/tests/golden.json)
        let golden = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../engine/tests/golden.json")).unwrap();
        let mut g = Game::new(&script()).unwrap();
        let mut x: u32 = 12345;
        for f in 1..=3000 {
            let mut inp = [0u8; 2];
            for v in inp.iter_mut() {
                x = x.wrapping_mul(1103515245).wrapping_add(12345);
                *v = ((x >> 16) % 64) as u8;
            }
            g.step(inp[0], inp[1]).unwrap();
            if f % 1000 == 0 {
                assert!(golden.contains(&format!("{:016x}", g.hash())), "frame {f}: {:016x} not in {golden}", g.hash());
            }
        }
    }

    #[test]
    fn script_errors_are_reported() {
        assert!(matches!(Game::new("this is not lua"), Err(e) if e.contains("gameplay")));
    }
}

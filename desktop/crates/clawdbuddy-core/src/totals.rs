//! Running totals the board shows: how long the app has been up and
//! connected to the board, and how much of that no session was working.
//!
//! The app keeps them in `<state>/totals` (`{"uptime": secs, "idle": secs}`)
//! so they outlast a restart, an update, and a reset or a flash of the board,
//! and sends them as the `ut` field. The board counts on from the last
//! figures it was sent.

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// A step between two ticks longer than this is not counted: the computer
/// was asleep, or the clock was set.
pub const MAX_STEP_SECS: i64 = 30;
/// The file is written at most this often.
pub const SAVE_EVERY_SECS: i64 = 60;

/// The two totals, in seconds.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct Totals {
    /// Time the app was running and connected to the board.
    pub uptime: u64,
    /// The part of it with no session working.
    pub idle: u64,
}

/// The `ut` field: `<uptime s>,<idle s>,<i|w>` (`i`: idle now, so the board
/// counts both on; `w`: only the uptime).
pub fn encode(t: Totals, idle_now: bool) -> String {
    format!("{},{},{}", t.uptime, t.idle, if idle_now { 'i' } else { 'w' })
}

/// Whether a `ut` field says the sessions are idle.
pub fn field_is_idle(ut: &str) -> bool {
    ut.ends_with('i')
}

/// Counts the totals up and keeps them on disk.
#[derive(Debug)]
pub struct Keeper {
    path: PathBuf,
    totals: Totals,
    last_tick: Option<i64>,
    saved: Totals,
    saved_at: i64,
}

impl Keeper {
    /// Pick up where `<dir>/totals` left off (zero when it is absent or does
    /// not read).
    pub fn load(dir: &Path) -> Self {
        let path = dir.join("totals");
        let totals: Totals =
            fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
        Keeper { path, totals, last_tick: None, saved: totals, saved_at: 0 }
    }

    /// The totals now.
    pub fn totals(&self) -> Totals {
        self.totals
    }

    /// Count the time since the last tick: as uptime while `connected`, and
    /// as idle too while no session works.
    pub fn tick(&mut self, now: i64, connected: bool, idle: bool) {
        let step = self.last_tick.map_or(0, |last| now - last);
        self.last_tick = Some(now);
        if connected && (1..=MAX_STEP_SECS).contains(&step) {
            self.totals.uptime += step as u64;
            if idle {
                self.totals.idle += step as u64;
            }
        }
        if self.totals != self.saved && !(0..SAVE_EVERY_SECS).contains(&(now - self.saved_at)) {
            self.save(now);
        }
    }

    /// Write the file now (also done on a timer by [`Keeper::tick`]).
    pub fn save(&mut self, now: i64) {
        let Ok(text) = serde_json::to_string(&self.totals) else { return };
        let tmp = self.path.with_extension("tmp");
        if fs::write(&tmp, text + "\n").and_then(|()| fs::rename(&tmp, &self.path)).is_ok() {
            self.saved = self.totals;
        }
        self.saved_at = now;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_connected_time_and_the_idle_part_of_it() {
        let dir = std::env::temp_dir().join(format!("clawdbuddy-totals-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mut k = Keeper::load(&dir);
        assert_eq!(k.totals(), Totals::default());
        k.tick(1000, true, true); // the first tick has nothing before it
        k.tick(1002, true, true);
        k.tick(1004, true, false);
        k.tick(1006, false, true); // not connected: neither counts
        assert_eq!(k.totals(), Totals { uptime: 4, idle: 2 });
        // The computer slept: the gap is not uptime.
        k.tick(9000, true, true);
        k.tick(9001, true, true);
        assert_eq!(k.totals(), Totals { uptime: 5, idle: 3 });
        assert_eq!(encode(k.totals(), true), "5,3,i");
        assert!(field_is_idle("5,3,i") && !field_is_idle("5,3,w"));
        // Written on the first change, then at most once a minute; a fresh
        // start reads what was written.
        assert_eq!(Keeper::load(&dir).totals(), Totals { uptime: 4, idle: 2 });
        k.tick(9000 + SAVE_EVERY_SECS, true, false);
        assert_eq!(Keeper::load(&dir).totals(), Totals { uptime: 5, idle: 3 }, "the long step is not counted");
        k.save(0);
        fs::write(dir.join("totals"), "junk").unwrap();
        assert_eq!(Keeper::load(&dir).totals(), Totals::default());
        let _ = fs::remove_dir_all(&dir);
    }
}

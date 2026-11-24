//! Running totals the board shows: how long the app has been up and
//! connected to the board, and how much of that no session was working.
//!
//! The app keeps them in `<state>/totals` (`{"uptime": secs, "idle": secs}`)
//! so they outlast a restart, an update, and a reset or a flash of the board,
//! and sends them as the `ut` field. The board counts on from the last
//! figures it was sent.
//!
//! The same file holds the tokens spent, this week and ever (the `tk`
//! field), added up from what each session's status line counted.

use std::collections::BTreeMap;
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

/// Tokens spent, as the sessions' status lines counted them
/// ([`crate::statusline::digest_after`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Tokens {
    /// Since [`week_start`].
    pub week: u64,
    /// Ever.
    pub total: u64,
}

/// The `tk` field: `<this week>,<ever>`.
pub fn encode_tokens(t: Tokens) -> String {
    format!("{},{}", t.week, t.total)
}

/// The Monday 00:00 that starts the week `local_epoch` (local wall-clock
/// seconds) is in, in the same terms.
pub fn week_start(local_epoch: i64) -> i64 {
    let day = local_epoch.div_euclid(86_400);
    // Day 0 (1 January 1970) was a Thursday.
    (day - (day + 3).rem_euclid(7)) * 86_400
}

/// A session not heard from for this long is forgotten: its status-line
/// file is long gone ([`crate::statusline::NUMBERS_TTL_SECS`]).
const SEEN_TTL_SECS: i64 = 24 * 3600;

/// The file. Every key may be absent (an older app wrote only the first two).
#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(default)]
struct Saved {
    uptime: u64,
    idle: u64,
    /// Tokens ever, and this week.
    tokens: u64,
    week_tokens: u64,
    /// The week `week_tokens` is of ([`week_start`]).
    week_start: i64,
    /// Per session: the count already added, and when it was last seen.
    seen: BTreeMap<String, (u64, i64)>,
}

/// Counts the totals up and keeps them on disk.
#[derive(Debug)]
pub struct Keeper {
    path: PathBuf,
    now: Saved,
    last_tick: Option<i64>,
    saved: Saved,
    saved_at: i64,
}

impl Keeper {
    /// Pick up where `<dir>/totals` left off (zero when it is absent or does
    /// not read).
    pub fn load(dir: &Path) -> Self {
        let path = dir.join("totals");
        let now: Saved = fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
        Keeper { path, saved: now.clone(), now, last_tick: None, saved_at: 0 }
    }

    /// The totals now.
    pub fn totals(&self) -> Totals {
        Totals { uptime: self.now.uptime, idle: self.now.idle }
    }

    /// The tokens now.
    pub fn tokens(&self) -> Tokens {
        Tokens { week: self.now.week_tokens, total: self.now.tokens }
    }

    /// Count the time since the last tick: as uptime while `connected`, and
    /// as idle too while no session works.
    pub fn tick(&mut self, now: i64, connected: bool, idle: bool) {
        let step = self.last_tick.map_or(0, |last| now - last);
        self.last_tick = Some(now);
        if connected && (1..=MAX_STEP_SECS).contains(&step) {
            self.now.uptime += step as u64;
            if idle {
                self.now.idle += step as u64;
            }
        }
        if self.now != self.saved && !(0..SAVE_EVERY_SECS).contains(&(now - self.saved_at)) {
            self.save(now);
        }
    }

    /// Add what the sessions have spent since they were last looked at.
    /// `sessions`: each one's count so far (its status-line file's);
    /// `week`: [`week_start`] of now. A new week empties the week's count.
    /// A session whose count went down started again from nothing.
    pub fn count_tokens<'a>(&mut self, now: i64, week: i64, sessions: impl IntoIterator<Item = (&'a str, u64)>) {
        if self.now.week_start != week {
            self.now.week_start = week;
            self.now.week_tokens = 0;
        }
        for (sid, count) in sessions {
            let before = self.now.seen.get(sid).map_or(0, |(c, _)| *c);
            let more = if count >= before { count - before } else { count };
            self.now.tokens = self.now.tokens.saturating_add(more);
            self.now.week_tokens = self.now.week_tokens.saturating_add(more);
            self.now.seen.insert(sid.to_owned(), (count, now));
        }
        self.now.seen.retain(|_, (_, at)| now - *at <= SEEN_TTL_SECS);
    }

    /// Write the file now (also done on a timer by [`Keeper::tick`]).
    pub fn save(&mut self, now: i64) {
        let Ok(text) = serde_json::to_string(&self.now) else { return };
        let tmp = self.path.with_extension("tmp");
        if fs::write(&tmp, text + "\n").and_then(|()| fs::rename(&tmp, &self.path)).is_ok() {
            self.saved = self.now.clone();
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

    #[test]
    fn weeks_start_on_monday() {
        // 2026-11-23 was a Monday: local epoch 1_795_392_000 is its 00:00.
        let monday = 1_795_392_000;
        assert_eq!(monday % 86_400, 0);
        assert_eq!(week_start(monday), monday);
        assert_eq!(week_start(monday + 6 * 86_400 + 86_399), monday, "Sunday night");
        assert_eq!(week_start(monday - 1), monday - 7 * 86_400);
        assert_eq!(week_start(monday + 7 * 86_400), monday + 7 * 86_400);
    }

    #[test]
    fn tokens_are_added_once_and_the_week_starts_again() {
        let dir = std::env::temp_dir().join(format!("clawdbuddy-tokens-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        // A file from before tokens were counted.
        fs::write(dir.join("totals"), r#"{"uptime":10,"idle":4}"#).unwrap();
        let mut k = Keeper::load(&dir);
        assert_eq!((k.totals(), k.tokens()), (Totals { uptime: 10, idle: 4 }, Tokens::default()));
        k.count_tokens(100, 7, [("a", 1000), ("b", 50)]);
        k.count_tokens(110, 7, [("a", 1000), ("b", 80)]);
        assert_eq!(k.tokens(), Tokens { week: 1080, total: 1080 });
        // A restart adds nothing twice.
        k.save(110);
        let mut k = Keeper::load(&dir);
        k.count_tokens(120, 7, [("a", 1200), ("b", 80)]);
        assert_eq!(k.tokens(), Tokens { week: 1280, total: 1280 });
        // A new week; a session whose file was lost counts from nothing.
        k.count_tokens(130, 8, [("a", 300)]);
        assert_eq!(k.tokens(), Tokens { week: 300, total: 1580 });
        assert_eq!(encode_tokens(k.tokens()), "300,1580");
        // Sessions not listed for a while are kept (a folder that could not
        // be read must not add everything again), then forgotten.
        k.count_tokens(140, 8, []);
        k.count_tokens(150, 8, [("b", 90)]);
        assert_eq!(k.tokens().total, 1590);
        k.count_tokens(150 + SEEN_TTL_SECS + 1, 8, []);
        k.count_tokens(150 + SEEN_TTL_SECS + 2, 8, [("b", 40)]);
        assert_eq!(k.tokens().total, 1630);
        let _ = fs::remove_dir_all(&dir);
    }
}

//! Where the daemon keeps its files.
//!
//! One state dir holds everything (config, hook files, events log, BLE
//! cache, logs, app state): `~/.config/clawdbuddy` on Linux and macOS,
//! `%LOCALAPPDATA%\ClawdBuddy` on Windows. The hook (`clawdbuddy hook`)
//! resolves the same dir, so the two always agree.

use std::path::{Path, PathBuf};

/// All filesystem locations the daemon touches. Build one with
/// [`Paths::for_user`] in production or [`Paths::under`] in tests.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Paths {
    /// The user's home dir; `~` in config values expands to this.
    pub home: PathBuf,
    /// The state dir: hook files, events log, BLE cache, logs, app state.
    pub state_dir: PathBuf,
    /// The key = value config file (in the state dir).
    pub config_file: PathBuf,
    /// Firmware downloads and update-check state.
    pub cache_dir: PathBuf,
}

impl Paths {
    /// The real per-user locations.
    pub fn for_user() -> Self {
        let home = dirs::home_dir().unwrap_or_else(|| PathBuf::from("."));
        let state_dir = if cfg!(windows) {
            std::env::var_os("LOCALAPPDATA")
                .map(PathBuf::from)
                .unwrap_or_else(|| home.join("AppData").join("Local"))
                .join("ClawdBuddy")
        } else {
            home.join(".config").join("clawdbuddy")
        };
        let config_file = state_dir.join("config");
        let cache_dir = dirs::cache_dir().unwrap_or_else(|| home.join(".cache")).join("clawdbuddy");
        Paths { home, state_dir, config_file, cache_dir }
    }

    /// Carry over the state dir from the app's former name (Clawdmeter),
    /// once. Called by the app and the hook when they start, never by
    /// [`Paths::for_user`] itself: building the paths must not move files.
    pub fn adopt_legacy(&self) {
        let name = if cfg!(windows) { "Clawdmeter" } else { "clawdmeter" };
        adopt_legacy_dir(&self.state_dir.with_file_name(name), &self.state_dir);
    }

    /// Everything under one root (tests, portable installs).
    pub fn under(root: &Path) -> Self {
        let state_dir = root.join("state");
        Paths {
            home: root.to_path_buf(),
            config_file: state_dir.join("config"),
            state_dir,
            cache_dir: root.join("cache"),
        }
    }

    /// `events.log`, appended by the hook in event mode:
    /// `<unix_ts> <HookEventName> <session_id>` per line.
    pub fn events_log(&self) -> PathBuf {
        self.state_dir.join("events.log")
    }

    /// The attention alert file (`{"m": message, "s": session id}`).
    pub fn attention_file(&self) -> PathBuf {
        self.state_dir.join("attention")
    }

    /// Cached device address (one line).
    pub fn ble_address_file(&self) -> PathBuf {
        self.state_dir.join("ble-address")
    }

    /// Each account's usage-endpoint block (backoff), kept across restarts.
    pub fn usage_blocks_file(&self) -> PathBuf {
        self.state_dir.join("usage-blocks.json")
    }

    /// Downloaded firmware images.
    pub fn firmware_cache(&self) -> PathBuf {
        self.cache_dir.join("firmware")
    }

    /// Expand a leading `~` / `~/` against [`Paths::home`].
    pub fn expand_tilde(&self, raw: &str) -> PathBuf {
        expand_tilde(&self.home, raw)
    }
}

/// Move `legacy` (the state dir under the app's former name, Clawdmeter) to
/// `current` when only the former exists. Errors are ignored: the app then
/// starts with a fresh folder.
pub fn adopt_legacy_dir(legacy: &Path, current: &Path) {
    if legacy != current && legacy.is_dir() && !current.exists() {
        let _ = std::fs::rename(legacy, current);
    }
}

/// Expand a leading `~` (alone, `~/x` or `~\x`) and `$HOME` against `home`.
pub fn expand_tilde(home: &Path, raw: &str) -> PathBuf {
    if raw == "~" || raw == "$HOME" {
        return home.to_path_buf();
    }
    for prefix in ["~/", "~\\", "$HOME/"] {
        if let Some(rest) = raw.strip_prefix(prefix) {
            return home.join(rest);
        }
    }
    PathBuf::from(raw)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tilde_expansion() {
        let h = Path::new("/home/u");
        assert_eq!(expand_tilde(h, "~"), PathBuf::from("/home/u"));
        assert_eq!(expand_tilde(h, "~/.claude-work"), PathBuf::from("/home/u/.claude-work"));
        assert_eq!(expand_tilde(h, "$HOME/.claude"), PathBuf::from("/home/u/.claude"));
        assert_eq!(expand_tilde(h, "/abs/dir"), PathBuf::from("/abs/dir"));
        assert_eq!(expand_tilde(h, "~other"), PathBuf::from("~other"));
    }

    #[test]
    fn the_former_names_folder_is_adopted_once() {
        let root = std::env::temp_dir().join(format!("cb-paths-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let (old, new) = (root.join("clawdmeter"), root.join("clawdbuddy"));
        std::fs::create_dir_all(&old).unwrap();
        std::fs::write(old.join("config"), "theme = dark\n").unwrap();
        adopt_legacy_dir(&old, &new);
        assert_eq!(std::fs::read_to_string(new.join("config")).unwrap(), "theme = dark\n");
        assert!(!old.exists());
        // A later folder under the old name never replaces the current one.
        std::fs::create_dir_all(&old).unwrap();
        std::fs::write(old.join("config"), "theme = light\n").unwrap();
        adopt_legacy_dir(&old, &new);
        assert_eq!(std::fs::read_to_string(new.join("config")).unwrap(), "theme = dark\n");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn under_layout() {
        let p = Paths::under(Path::new("/t"));
        assert_eq!(p.events_log(), PathBuf::from("/t/state/events.log"));
        assert_eq!(p.config_file, PathBuf::from("/t/state/config"));
        assert_eq!(p.ble_address_file(), PathBuf::from("/t/state/ble-address"));
    }

    #[cfg(not(windows))]
    #[test]
    fn user_layout_is_one_dir() {
        let p = Paths::for_user();
        assert!(p.state_dir.ends_with(".config/clawdbuddy"));
        assert_eq!(p.config_file, p.state_dir.join("config"));
    }
}

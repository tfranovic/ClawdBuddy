//! A tiny stderr logger (`[HH:MM:SS] message`, with a `--- date ---` line at
//! the start and whenever the day changes), so the
//! binary doesn't carry env_logger's regex engine. Level from `CLAWDBUDDY_LOG`
//! (`error|warn|info|debug|trace`, default `info`).

use log::{Level, LevelFilter, Log, Metadata, Record};

/// The log file is rolled to `<path>.1` when it reaches this size, so an app
/// left running for weeks keeps at most twice this.
const ROLL_BYTES: u64 = 4 * 1024 * 1024;

struct Sink {
    file: Option<std::fs::File>,
    path: Option<std::path::PathBuf>,
    written: u64,
    /// The day the last line was written on.
    day: String,
}

impl Sink {
    fn write(&mut self, line: &str) {
        use std::io::Write as _;
        if self.written >= ROLL_BYTES {
            if let Some(path) = &self.path {
                let _ = std::fs::rename(path, path.with_extension("log.1"));
                self.file = std::fs::File::create(path).ok();
                self.written = 0;
                self.day.clear();
            }
        }
        let day = chrono::Local::now().format("%Y-%m-%d").to_string();
        let mark = (day != self.day).then(|| format!("--- {day} ---"));
        self.day = day;
        for l in mark.as_deref().into_iter().chain([line]) {
            eprintln!("{l}");
            if let Some(f) = &mut self.file {
                let _ = writeln!(f, "{l}");
                self.written += l.len() as u64 + 1;
            }
        }
    }
}

struct Stderr {
    level: LevelFilter,
    sink: std::sync::Mutex<Sink>,
}

impl Log for Stderr {
    fn enabled(&self, m: &Metadata) -> bool {
        m.level() <= self.level && (m.target().starts_with("clawdbuddy") || m.level() <= Level::Warn)
    }

    fn log(&self, r: &Record) {
        if !self.enabled(r.metadata()) {
            return;
        }
        let t = chrono::Local::now().format("%H:%M:%S");
        let lvl = match r.level() {
            Level::Error => "error: ",
            Level::Warn => "warning: ",
            _ => "",
        };
        let line = format!("[{t}] {lvl}{}", r.args());
        if let Ok(mut sink) = self.sink.lock() {
            sink.write(&line);
        }
    }

    fn flush(&self) {}
}

/// Install the logger once (stderr only).
pub fn init() {
    install(None, None);
}

/// Log to stderr and to `path` as well (the tray app has no terminal). The
/// previous log is kept as `<path>.1`, and the file is rolled there again
/// when it grows large.
pub fn init_with_file(path: &std::path::Path) {
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::rename(path, path.with_extension("log.1"));
    install(std::fs::File::create(path).ok(), Some(path.to_owned()));
}

fn install(file: Option<std::fs::File>, path: Option<std::path::PathBuf>) {
    let level = match std::env::var("CLAWDBUDDY_LOG").unwrap_or_default().to_ascii_lowercase().as_str() {
        "error" => LevelFilter::Error,
        "warn" => LevelFilter::Warn,
        "debug" => LevelFilter::Debug,
        "trace" => LevelFilter::Trace,
        "off" => LevelFilter::Off,
        _ => LevelFilter::Info,
    };
    let logger: &'static Stderr = Box::leak(Box::new(Stderr {
        level,
        sink: std::sync::Mutex::new(Sink { file, path, written: 0, day: String::new() }),
    }));
    if log::set_logger(logger).is_ok() {
        log::set_max_level(level);
    }
}

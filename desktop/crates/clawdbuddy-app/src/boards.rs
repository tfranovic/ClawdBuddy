//! What the app knows about each supported board: names, a short summary,
//! where to buy one, and its illustration (`assets/boards/<env>.svg`, drawn
//! for the app; bundled into the binary by [`crate::ui::Assets`]).

use clawdbuddy_core::buttons::Button;

/// One supported board.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BoardInfo {
    /// PlatformIO env.
    pub env: &'static str,
    /// Short name.
    pub name: &'static str,
    /// Full product name.
    pub product: &'static str,
    /// Panel width, px.
    pub width: u32,
    /// Panel height, px.
    pub height: u32,
    /// Where to buy one.
    pub buy_url: &'static str,
    /// One-line summary.
    pub summary: &'static str,
    /// SoC.
    pub chip: &'static str,
    /// Has a PWR button the firmware reads (`BoardCaps::has_pwr_button`):
    /// a short press runs its action (`k3`, default brightness); hold 3 s
    /// and release to clear the pairing. The LCD-4's KEY/PWR is a hardware
    /// reset; it clears the pairing with a 5 s hold on the screen while the
    /// pairing hint shows.
    pub pwr_button: bool,
    /// The buttons the app can remap, in the order they sit on the board (left
    /// to right, or top to bottom), each with the name the Device page gives
    /// it: the one on its case or silkscreen, or where it is. The primary and
    /// secondary buttons match the firmware's `BoardCaps::button_count`, PWR
    /// [`Self::pwr_button`].
    pub buttons: &'static [(Button, &'static str)],
}

/// Every hardware env in `firmware/platformio.ini` (not `sim` / `native`).
pub const BOARDS: [BoardInfo; 1] = [BoardInfo {
    env: "waveshare_lcd_4",
    name: "LCD 4\"",
    product: "Waveshare ESP32-S3-Touch-LCD-4",
    width: 480,
    height: 480,
    buy_url: "https://www.waveshare.com/esp32-s3-touch-lcd-4.htm",
    summary: "480×480 · RGB TFT",
    chip: "ESP32-S3",
    pwr_button: false,
    buttons: &[(Button::Primary, "BOOT")],
}];

/// Look up a board by env.
pub fn board(env: &str) -> Option<&'static BoardInfo> {
    BOARDS.iter().find(|b| b.env == env)
}

impl BoardInfo {
    /// The illustration's asset path (see [`crate::ui::Assets`]).
    pub fn illustration(&self) -> String {
        format!("boards/{}.svg", self.env)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The firmware board folder an env builds from: variant envs share their
    /// base board's folder (`<env>_landscape`, and the LCD-2.8C built from the
    /// LCD-2.1's folder with `BOARD_LCD28C`).
    fn firmware_dir(env: &str) -> &str {
        match env {
            "waveshare_lcd_28c" => "waveshare_lcd_21",
            env => env.trim_end_matches("_landscape"),
        }
    }

    #[test]
    fn every_firmware_env_is_listed() {
        let ini = include_str!("../../../../firmware/platformio.ini");
        for line in ini.lines() {
            if let Some(env) = line.strip_prefix("[env:").and_then(|l| l.strip_suffix(']')) {
                // The pet builds report the env of the board they extend.
                if env == "sim" || env == "native" {
                    continue;
                }
                assert!(board(env).is_some(), "{env} missing from BOARDS");
            }
        }
    }

    /// The primary and secondary buttons in `buttons` match the board's
    /// `button_count` in its `caps.cpp` (`1 + BOARD_HAS_SECONDARY_BUTTON`
    /// read from its `board.h`).
    #[test]
    fn button_counts_match_the_firmware() {
        let boards = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/src/boards");
        let define = |text: &str, name: &str| -> Option<u8> {
            text.lines().find_map(|l| {
                l.trim().strip_prefix("#define ")?.strip_prefix(name)?.split_whitespace().next()?.parse().ok()
            })
        };
        for b in BOARDS {
            let dir = boards.join(firmware_dir(b.env));
            let caps = std::fs::read_to_string(dir.join("caps.cpp")).unwrap();
            let value = caps.split(".button_count").nth(1).and_then(|r| r.split(',').next()).unwrap();
            let firmware = if value.contains("BOARD_HAS_SECONDARY_BUTTON") {
                let board_h = std::fs::read_to_string(dir.join("board.h")).unwrap();
                1 + define(&board_h, "BOARD_HAS_SECONDARY_BUTTON").unwrap()
            } else {
                value.trim_start_matches([' ', '=']).trim().parse().unwrap()
            };
            let remappable = b.buttons.iter().filter(|(k, _)| *k != Button::Pwr).count();
            assert_eq!(remappable, usize::from(firmware), "{}", b.env);
            // A board whose input HAL never reports a button held has none to remap.
            let input = std::fs::read_to_string(dir.join("input.cpp")).unwrap();
            let squashed: String = input.split_whitespace().collect();
            if squashed.contains("input_hal_is_held(InputButtonbtn){(void)btn;returnfalse;}") {
                assert_eq!(firmware, 0, "{}: button_count counts a button input_hal never reports", b.env);
            }
        }
    }

    /// `pwr_button` matches `has_pwr_button` in the board's `caps.cpp`: the
    /// Device page offers a PWR field (`k3`) only where the firmware reads one.
    #[test]
    fn pwr_buttons_match_the_firmware() {
        let boards = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/src/boards");
        for b in BOARDS {
            let caps = std::fs::read_to_string(boards.join(firmware_dir(b.env)).join("caps.cpp")).unwrap();
            let value = caps.split(".has_pwr_button").nth(1).and_then(|r| r.split(',').next()).unwrap();
            let firmware = match value.trim_start_matches([' ', '=']).trim() {
                "true" => true,
                "false" => false,
                other => panic!("{}: has_pwr_button = {other:?}", b.env),
            };
            assert_eq!(b.pwr_button, firmware, "{}", b.env);
        }
    }

    /// Each button once, named, PWR listed exactly where the firmware reads
    /// one, and a secondary only beside a primary (`k2` is the second of
    /// `button_count`).
    #[test]
    fn button_layouts_are_consistent() {
        for b in BOARDS {
            let has = |k: Button| b.buttons.iter().filter(|(x, _)| *x == k).count();
            for k in Button::ALL {
                assert!(has(k) <= 1, "{}: {k:?} listed twice", b.env);
            }
            assert_eq!(has(Button::Pwr) == 1, b.pwr_button, "{}", b.env);
            assert!(has(Button::Secondary) <= has(Button::Primary), "{}", b.env);
            assert!(b.buttons.iter().all(|(_, name)| !name.is_empty()), "{}", b.env);
        }
    }

    #[test]
    fn every_board_has_art_and_a_shop() {
        for b in BOARDS {
            assert!(crate::ui::Assets::get(&b.illustration()).is_some(), "{} has no illustration", b.env);
            assert!(b.buy_url.starts_with("https://"), "{}", b.env);
        }
    }
}

//! Bounded log fragments for the gateway's 255-byte syslog receive limit.

use crate::async_logging::AsyncOutput;
pub use crate::async_logging::WorkerGuard;
use std::io::{self, Write};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::OnceLock;

// The pinned uClibc reserves at most 64 bytes plus the tag for its header.
// 174 + 64 + "mthread-matter".len() + terminating NUL = 253.
const BODY_BYTES: usize = 174;
const FRAGMENT_BYTES: usize = 150;
static NEXT_ENTRY: AtomicU32 = AtomicU32::new(1);

pub(crate) struct SyslogWriter {
    priority: i32,
    buffer: Vec<u8>,
    entry: Option<u32>,
    part: u32,
    #[cfg(test)]
    messages: Vec<String>,
}

impl SyslogWriter {
    pub(crate) fn new() -> Self {
        Self {
            priority: libc::LOG_INFO,
            buffer: Vec::with_capacity(BODY_BYTES),
            entry: None,
            part: 0,
            #[cfg(test)]
            messages: Vec::new(),
        }
    }

    fn send(&mut self, message: &str) {
        debug_assert!(message.len() <= BODY_BYTES);
        #[cfg(test)]
        self.messages.push(message.to_owned());
        #[cfg(not(test))]
        {
            use std::ffi::{c_char, c_int, CString};
            unsafe extern "C" {
                fn syslog(priority: c_int, format: *const c_char, ...);
            }
            // NULs were sanitized before splitting, so conversion cannot fail.
            let message = CString::new(message).expect("Sanitized syslog message");
            unsafe {
                syslog(self.priority, b"%s\0".as_ptr().cast(), message.as_ptr());
            }
        }
    }

    fn prefix_length(bytes: &[u8], limit: usize) -> usize {
        let mut end = bytes.len().min(limit);
        // Keep a UTF-8 character's continuation bytes with its leading byte.
        while end > 0 && end < bytes.len() && bytes[end] & 0xc0 == 0x80 {
            end -= 1;
        }
        if end == 0 {
            bytes.len().min(limit)
        } else {
            end
        }
    }

    fn fragment(&mut self, bytes: &[u8], final_part: bool) {
        let entry = *self
            .entry
            .get_or_insert_with(|| NEXT_ENTRY.fetch_add(1, Ordering::Relaxed));
        let text = String::from_utf8_lossy(bytes).replace('\0', "?");
        let mut rest = text.as_str();
        while !rest.is_empty() {
            let end = Self::prefix_length(rest.as_bytes(), FRAGMENT_BYTES);
            let marker = if final_part && end == rest.len() {
                '='
            } else {
                '+'
            };
            self.send(&format!(
                "[{entry:08x}/{:08x}{marker}] {}",
                self.part,
                &rest[..end]
            ));
            self.part = self.part.wrapping_add(1);
            rest = &rest[end..];
        }
    }

    fn continue_entry(&mut self) {
        let end = Self::prefix_length(&self.buffer, FRAGMENT_BYTES);
        let bytes: Vec<u8> = self.buffer.drain(..end).collect();
        self.fragment(&bytes, false);
    }

    fn finish_entry(&mut self) {
        if !self.buffer.is_empty() {
            let bytes = std::mem::take(&mut self.buffer);
            let text = String::from_utf8_lossy(&bytes).replace('\0', "?");
            if self.entry.is_none() && text.len() <= BODY_BYTES {
                self.send(&text);
            } else {
                self.fragment(&bytes, true);
            }
            self.buffer = Vec::with_capacity(BODY_BYTES);
        }
        self.entry = None;
        self.part = 0;
    }
}

impl Write for SyslogWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        for &byte in bytes {
            if byte == b'\n' {
                self.finish_entry();
            } else {
                if self.buffer.len() == BODY_BYTES {
                    self.continue_entry();
                }
                self.buffer.push(byte);
            }
        }
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.finish_entry();
        Ok(())
    }
}

impl Drop for SyslogWriter {
    fn drop(&mut self) {
        self.finish_entry();
    }
}

/// OpenThread can call libc exit directly on RCP failure, bypassing logger
/// shutdown. Emit critical diagnostics synchronously before that can happen.
pub fn critical_thread_diagnostic(message: &str) {
    emit(
        libc::LOG_ERR,
        format!("ERROR thread_diagnostics: {message}\n").as_bytes(),
    );
}

/// Panic-abort builds cannot drain the asynchronous logger. Send the source
/// location synchronously, without potentially sensitive panic payloads.
pub fn install_panic_diagnostics() {
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let mut writer = SyslogWriter::new();
        writer.priority = libc::LOG_ERR;
        if let Some(location) = info.location() {
            let _ = writeln!(
                writer,
                "Matter panic at {}:{}:{}; process aborting",
                location.file(),
                location.line(),
                location.column()
            );
        } else {
            let _ = writeln!(
                writer,
                "Matter panic; source location unavailable; process aborting"
            );
        }
        let _ = writer.flush();
        previous(info);
    }));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reconstructed(messages: &[String]) -> String {
        messages
            .iter()
            .map(|message| {
                if message.starts_with('[') {
                    &message[21..]
                } else {
                    message.as_str()
                }
            })
            .collect()
    }

    #[test]
    fn short_entries_keep_their_format() {
        let mut writer = SyslogWriter::new();
        writer.write_all(b"ready\n\nnext\0entry\n").unwrap();
        assert_eq!(writer.messages, ["ready", "next?entry"]);
    }

    #[test]
    fn byte_limits_and_long_entries_preserve_every_byte() {
        for length in [173, 174, 175, 255, 1024, 4096, 8192, 65536] {
            let text = "x".repeat(length);
            let mut writer = SyslogWriter::new();
            writer.write_all(text.as_bytes()).unwrap();
            writer.flush().unwrap();
            assert_eq!(reconstructed(&writer.messages), text);
            assert!(writer
                .messages
                .iter()
                .all(|message| message.len() <= BODY_BYTES));
            assert!(writer
                .messages
                .iter()
                .all(|message| message.len() + 64 + 14 + 1 <= 255));
            if length > BODY_BYTES {
                let entry = &writer.messages[0][1..9];
                for (index, message) in writer.messages.iter().enumerate() {
                    assert_eq!(&message[1..9], entry);
                    assert_eq!(&message[10..18], format!("{index:08x}"));
                    assert_eq!(
                        &message[18..19],
                        if index + 1 == writer.messages.len() {
                            "="
                        } else {
                            "+"
                        }
                    );
                }
            }
        }
    }

    #[test]
    fn utf8_survives_every_write_boundary() {
        let text = "aé€🦀".repeat(1000);
        for size in [1, 2, 3, 7, 155, 156, 179, 180, 181] {
            let mut writer = SyslogWriter::new();
            for bytes in text.as_bytes().chunks(size) {
                writer.write_all(bytes).unwrap();
                assert!(writer.buffer.len() <= BODY_BYTES);
            }
            writer.flush().unwrap();
            assert_eq!(reconstructed(&writer.messages), text);
            assert!(writer
                .messages
                .iter()
                .all(|message| message.len() <= BODY_BYTES));
        }
    }

    #[test]
    fn invalid_utf8_stays_bounded_and_is_sanitized() {
        let mut writer = SyslogWriter::new();
        writer.write_all(&[0xff; 1000]).unwrap();
        writer.flush().unwrap();
        assert_eq!(reconstructed(&writer.messages), "�".repeat(1000));
        assert!(writer
            .messages
            .iter()
            .all(|message| message.len() <= BODY_BYTES));
    }

    #[test]
    fn entry_ids_distinguish_interleaved_writers_and_flush_is_idempotent() {
        let mut first = SyslogWriter::new();
        let mut second = SyslogWriter::new();
        first.write_all(&[b'a'; 181]).unwrap();
        second.write_all(&[b'b'; 181]).unwrap();
        first.flush().unwrap();
        second.flush().unwrap();
        assert_ne!(&first.messages[0][1..9], &second.messages[0][1..9]);
        let count = first.messages.len();
        first.flush().unwrap();
        assert_eq!(first.messages.len(), count);
        first.write_all(b"short\n").unwrap();
        assert_eq!(first.messages.last().unwrap(), "short");
    }
}

struct Logger;
struct LogOutput {
    output: AsyncOutput,
    level: log::LevelFilter,
    raw_frames: bool,
}
static OUTPUT: OnceLock<LogOutput> = OnceLock::new();
const RAW_TARGET: &str = "matter_raw";
static LOGGER: Logger = Logger;
static INITIALIZED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub fn initialized() -> bool {
    INITIALIZED.load(Ordering::Relaxed)
}

pub fn parse_level(value: &str) -> Result<log::LevelFilter, &'static str> {
    match value {
        "off" => Ok(log::LevelFilter::Off),
        "error" => Ok(log::LevelFilter::Error),
        "warn" => Ok(log::LevelFilter::Warn),
        "info" => Ok(log::LevelFilter::Info),
        "debug" => Ok(log::LevelFilter::Debug),
        "trace" => Ok(log::LevelFilter::Trace),
        _ => Err("invalid log level (expected off|error|warn|info|debug|trace)"),
    }
}

fn priority(level: log::Level) -> i32 {
    match level {
        log::Level::Error => libc::LOG_ERR,
        log::Level::Warn => libc::LOG_WARNING,
        log::Level::Info => libc::LOG_INFO,
        log::Level::Debug | log::Level::Trace => libc::LOG_DEBUG,
    }
}

impl log::Log for Logger {
    fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
        OUTPUT.get().map_or_else(
            || metadata.level() <= log::max_level(),
            |output| {
                enabled(
                    metadata.level(),
                    metadata.target(),
                    output.level,
                    output.raw_frames,
                )
            },
        )
    }
    fn log(&self, record: &log::Record<'_>) {
        if self.enabled(record.metadata()) {
            let Some(output) = OUTPUT.get() else {
                return;
            };
            let mut writer = output.output.entry(priority(record.level()));
            let _ = writeln!(
                writer,
                "{} {}: {}",
                record.level(),
                record.target(),
                record.args()
            );
        }
    }
    fn flush(&self) {}
}

fn enabled(level: log::Level, target: &str, normal: log::LevelFilter, raw: bool) -> bool {
    level <= normal || (raw && target == RAW_TARGET)
}

pub(crate) fn emit(priority: i32, entry: &[u8]) {
    let mut writer = SyslogWriter::new();
    writer.priority = priority;
    let _ = writer.write_all(entry);
    let _ = writer.flush();
}

pub fn init(
    level: log::LevelFilter,
    raw_frames: bool,
) -> Result<WorkerGuard, Box<dyn std::error::Error>> {
    unsafe {
        libc::openlog(
            b"mthread-matter\0".as_ptr().cast(),
            libc::LOG_PID,
            libc::LOG_DAEMON,
        );
    }
    let (output, guard) = AsyncOutput::start()?;
    OUTPUT
        .set(LogOutput {
            output,
            level,
            raw_frames,
        })
        .map_err(|_| "logger already initialized")?;
    log::set_logger(&LOGGER).map_err(|_| "logger already installed")?;
    log::set_max_level(if raw_frames {
        log::LevelFilter::Trace
    } else {
        level
    });
    INITIALIZED.store(true, Ordering::Relaxed);
    Ok(guard)
}

pub fn packet_received(peer: &impl std::fmt::Display, bytes: &[u8]) {
    if log::log_enabled!(target: RAW_TARGET, log::Level::Trace) {
        log::trace!(target: RAW_TARGET, "RX unix_us={} peer={peer} length={} payload={:02X?}", packet_time(), bytes.len(), bytes);
    }
}

pub fn packet_sent(peer: &impl std::fmt::Display, bytes: &[u8], result: i32) {
    if log::log_enabled!(target: RAW_TARGET, log::Level::Trace) {
        log::trace!(target: RAW_TARGET, "TX unix_us={} peer={peer} length={} result={result} payload={:02X?}", packet_time(), bytes.len(), bytes);
    }
}

fn packet_time() -> u128 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_micros()
}

#[cfg(test)]
mod logger_tests {
    use super::*;
    use log::Log;
    #[test]
    fn packet_capture_does_not_enable_general_trace() {
        assert!(enabled(
            log::Level::Trace,
            RAW_TARGET,
            log::LevelFilter::Info,
            true
        ));
        assert!(!enabled(
            log::Level::Trace,
            "rs_matter",
            log::LevelFilter::Info,
            true
        ));
        assert!(enabled(
            log::Level::Info,
            "rs_matter",
            log::LevelFilter::Info,
            true
        ));
        assert!(!enabled(
            log::Level::Trace,
            RAW_TARGET,
            log::LevelFilter::Info,
            false
        ));
        assert!(enabled(
            log::Level::Trace,
            RAW_TARGET,
            log::LevelFilter::Trace,
            false
        ));
    }
    #[test]
    fn levels_filter_and_map_to_syslog() {
        for (name, level) in [
            ("off", log::LevelFilter::Off),
            ("error", log::LevelFilter::Error),
            ("warn", log::LevelFilter::Warn),
            ("info", log::LevelFilter::Info),
            ("debug", log::LevelFilter::Debug),
            ("trace", log::LevelFilter::Trace),
        ] {
            assert_eq!(parse_level(name), Ok(level));
            log::set_max_level(level);
            for severity in [
                log::Level::Error,
                log::Level::Warn,
                log::Level::Info,
                log::Level::Debug,
                log::Level::Trace,
            ] {
                assert_eq!(
                    LOGGER.enabled(&log::Metadata::builder().level(severity).build()),
                    severity <= level
                );
            }
        }
        for bad in ["", "verbose", "INFO", "info,rs_matter=trace"] {
            assert!(parse_level(bad).is_err());
        }
        assert_eq!(priority(log::Level::Error), libc::LOG_ERR);
        assert_eq!(priority(log::Level::Warn), libc::LOG_WARNING);
        assert_eq!(priority(log::Level::Info), libc::LOG_INFO);
        assert_eq!(priority(log::Level::Debug), libc::LOG_DEBUG);
        assert_eq!(priority(log::Level::Trace), libc::LOG_DEBUG);
    }
}

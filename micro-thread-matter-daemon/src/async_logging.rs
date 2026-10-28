//! Whole-entry, bounded handoff: protocol tasks never call libc syslog.

use std::io::{self, Write};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::{self, SyncSender};
use std::sync::Arc;
use std::thread::{self, JoinHandle};

// At most 4 MiB of queued text, plus the entry being emitted. Long frames
// remain intact; overload drops whole entries rather than orphaning fragments.
const QUEUE_DEPTH: usize = 256;
const MAX_ENTRY_BYTES: usize = 16 * 1024;

enum Message {
    Entry(i32, Vec<u8>),
    Shutdown,
}

#[derive(Clone)]
pub(crate) struct AsyncOutput {
    sender: SyncSender<Message>,
    dropped: Arc<AtomicU32>,
}

pub(crate) struct EntryWriter {
    output: AsyncOutput,
    bytes: Vec<u8>,
    oversized: bool,
    priority: i32,
}

pub struct WorkerGuard {
    output: AsyncOutput,
    thread: Option<JoinHandle<()>>,
}

impl AsyncOutput {
    pub(crate) fn start() -> io::Result<(Self, WorkerGuard)> {
        Self::start_with_sink(super::logging::emit)
    }

    fn start_with_sink<F>(mut sink: F) -> io::Result<(Self, WorkerGuard)>
    where
        F: FnMut(i32, &[u8]) + Send + 'static,
    {
        let (sender, receiver) = mpsc::sync_channel(QUEUE_DEPTH);
        let dropped = Arc::new(AtomicU32::new(0));
        let worker_dropped = dropped.clone();
        let thread = thread::Builder::new()
            .name("matter-log".into())
            .stack_size(128 * 1024)
            .spawn(move || {
                while let Ok(message) = receiver.recv() {
                    let shutdown = matches!(message, Message::Shutdown);
                    match message {
                        Message::Entry(priority, entry) => sink(priority, &entry),
                        Message::Shutdown => {}
                    }
                    let lost = worker_dropped.swap(0, Ordering::Relaxed);
                    if lost != 0 {
                        sink(libc::LOG_WARNING, format!("WARN mthread-matter: dropped {lost} complete log entries (queue full or entry exceeded {MAX_ENTRY_BYTES} bytes)\n").as_bytes());
                    }
                    if shutdown {
                        break;
                    }
                }
            })?;
        let output = Self { sender, dropped };
        let guard = WorkerGuard {
            output: output.clone(),
            thread: Some(thread),
        };
        Ok((output, guard))
    }

    pub(crate) fn entry(&self, priority: i32) -> EntryWriter {
        EntryWriter {
            output: self.clone(),
            bytes: Vec::new(),
            oversized: false,
            priority,
        }
    }
}

impl Write for EntryWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if !self.oversized {
            if bytes.len() > MAX_ENTRY_BYTES - self.bytes.len() {
                self.oversized = true;
                self.bytes.clear();
            } else {
                self.bytes.extend_from_slice(bytes);
            }
        }
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        // tracing owns one writer per event. Keep it whole until Drop, including
        // if its formatter flushes in the middle of an entry.
        Ok(())
    }
}

impl Drop for EntryWriter {
    fn drop(&mut self) {
        if self.oversized {
            self.output.dropped.fetch_add(1, Ordering::Relaxed);
        } else if !self.bytes.is_empty()
            && self
                .output
                .sender
                .try_send(Message::Entry(
                    self.priority,
                    std::mem::take(&mut self.bytes),
                ))
                .is_err()
        {
            self.output.dropped.fetch_add(1, Ordering::Relaxed);
        }
    }
}

impl Drop for WorkerGuard {
    fn drop(&mut self) {
        // Only shutdown waits for the sink: normal event writes use try_send.
        let _ = self.output.sender.send(Message::Shutdown);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocked_sink_does_not_block_producer_and_reports_whole_entry_loss() {
        let (entered_tx, entered_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let (saved_tx, saved_rx) = mpsc::channel();
        let mut first = true;
        let (output, guard) = AsyncOutput::start_with_sink(move |_, entry| {
            if first {
                first = false;
                entered_tx.send(()).unwrap();
                release_rx.recv().unwrap();
            }
            saved_tx.send(entry.to_vec()).unwrap();
        })
        .unwrap();
        output.entry(libc::LOG_INFO).write_all(b"first\n").unwrap();
        entered_rx.recv().unwrap();
        for n in 0..QUEUE_DEPTH {
            write!(output.entry(libc::LOG_INFO), "frame {n}\n").unwrap();
        }
        output
            .entry(libc::LOG_INFO)
            .write_all(b"dropped frame\n")
            .unwrap();
        assert_eq!(output.dropped.load(Ordering::Relaxed), 1);
        release_tx.send(()).unwrap();
        drop(guard);
        let saved: Vec<_> = saved_rx.try_iter().collect();
        assert_eq!(saved.len(), QUEUE_DEPTH + 2);
        assert_eq!(saved[0], b"first\n");
        assert!(String::from_utf8_lossy(&saved[1]).contains("dropped 1 complete log entries"));
        for n in 0..QUEUE_DEPTH {
            assert_eq!(saved[n + 2], format!("frame {n}\n").as_bytes());
        }
    }

    #[test]
    fn flush_preserves_entry_and_oversized_entry_is_dropped_whole() {
        let (saved_tx, saved_rx) = mpsc::channel();
        let (output, guard) = AsyncOutput::start_with_sink(move |_, entry| {
            saved_tx.send(entry.to_vec()).unwrap();
        })
        .unwrap();
        let mut huge = output.entry(libc::LOG_INFO);
        huge.write_all(&vec![b'x'; MAX_ENTRY_BYTES]).unwrap();
        huge.write_all(b"overflow").unwrap();
        drop(huge);
        let mut entry = output.entry(libc::LOG_INFO);
        entry.write_all(b"head ").unwrap();
        entry.flush().unwrap();
        entry.write_all(b"tail\n").unwrap();
        drop(entry);
        drop(guard);
        let saved: Vec<_> = saved_rx.try_iter().collect();
        assert_eq!(saved.len(), 2);
        assert_eq!(saved[0], b"head tail\n");
        assert!(String::from_utf8_lossy(&saved[1]).contains("dropped 1 complete log entries"));
    }

    #[test]
    fn entries_retain_syslog_severity() {
        let (tx, rx) = mpsc::channel();
        let (output, guard) = AsyncOutput::start_with_sink(move |priority, bytes| {
            tx.send((priority, bytes.to_vec())).unwrap();
        })
        .unwrap();
        output
            .entry(libc::LOG_WARNING)
            .write_all(b"warning\n")
            .unwrap();
        output.entry(libc::LOG_ERR).write_all(b"error\n").unwrap();
        drop(guard);
        assert_eq!(
            rx.recv().unwrap(),
            (libc::LOG_WARNING, b"warning\n".to_vec())
        );
        assert_eq!(rx.recv().unwrap(), (libc::LOG_ERR, b"error\n".to_vec()));
    }
}

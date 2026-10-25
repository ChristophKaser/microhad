//! Durable time-zone intent and bounded Matter DST schedules.
use crate::store::PrivateStore;
use chrono::{Offset, TimeZone};
use chrono_tz::OffsetComponents;
use std::time::{Duration, Instant};

const KEY: u16 = 0x1004;
pub const EPOCH: u64 = 946_684_800;
const DAY: u64 = 86400;

pub fn zone(name: &str) -> Result<chrono_tz::Tz, ()> {
    name.parse().map_err(|_| ())
}
fn offset(zone: chrono_tz::Tz, unix: u64) -> i32 {
    zone.timestamp_opt(unix as i64, 0)
        .unwrap()
        .offset()
        .fix()
        .local_minus_utc()
}
#[derive(Debug)]
pub struct Interval {
    pub offset: i32,
    pub start: u64,
    pub end: u64,
}
pub struct Schedule {
    pub base: i32,
    pub intervals: Vec<Interval>,
}

/// Use the current standard offset plus relative adjustments. This also handles
/// political offset changes without requiring a second TimeZone table slot.
pub fn schedule(name: &str, now: u64, capacity: u8) -> Result<Schedule, ()> {
    let zone = zone(name)?;
    if now < EPOCH || now > 253_000_000_000 || capacity == 0 {
        return Err(());
    }
    let base = zone
        .timestamp_opt(now as i64, 0)
        .unwrap()
        .offset()
        .base_utc_offset()
        .num_seconds() as i32;
    let horizon = now + 730 * DAY;
    let mut cursor = now;
    let mut start = 0;
    let mut current = offset(zone, now);
    let mut intervals = Vec::new();
    // IANA civil-time transitions are separated by more than six hours.
    while cursor < horizon && intervals.len() < usize::from(capacity.min(16)) {
        let next = (cursor + 6 * 3600).min(horizon);
        if offset(zone, next) != current {
            let mut low = cursor;
            let mut high = next;
            while high - low > 1 {
                let middle = low + (high - low) / 2;
                if offset(zone, middle) == current {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            let boundary = (high - EPOCH) * 1_000_000;
            intervals.push(Interval {
                offset: current - base,
                start,
                end: boundary,
            });
            start = boundary;
            current = offset(zone, high);
            cursor = high;
        } else {
            cursor = next;
        }
    }
    if intervals.len() < usize::from(capacity.min(16)) {
        intervals.push(Interval {
            offset: current - base,
            start,
            end: (horizon - EPOCH) * 1_000_000,
        });
    }
    Ok(Schedule { base, intervals })
}

pub struct Preference {
    pub node: u64,
    pub endpoint: u16,
    pub name: String,
    pub next: Instant,
    failures: u8,
}
pub struct Preferences {
    store: PrivateStore,
    pub entries: Vec<Preference>,
}
impl Preferences {
    pub fn open(store: PrivateStore) -> std::io::Result<Self> {
        let mut bytes = [0; 4096];
        let mut entries = Vec::new();
        if let Some(size) = store.get(KEY, &mut bytes)? {
            let records: Vec<(u64, u16, String)> = serde_json::from_slice(&bytes[..size])
                .map_err(|_| std::io::Error::other("invalid time-zone preferences"))?;
            if records.len() > crate::catalog::MAX_NODES {
                return Err(std::io::Error::other("too many time-zone preferences"));
            }
            for (node, endpoint, name) in records {
                if zone(&name).is_err()
                    || entries
                        .iter()
                        .any(|p: &Preference| p.node == node && p.endpoint == endpoint)
                {
                    return Err(std::io::Error::other("invalid time-zone preference"));
                }
                entries.push(Preference {
                    node,
                    endpoint,
                    name,
                    next: Instant::now(),
                    failures: 0,
                });
            }
        }
        Ok(Self { store, entries })
    }
    pub fn set(
        &mut self,
        node: u64,
        endpoint: u16,
        name: &str,
        now: Instant,
    ) -> std::io::Result<()> {
        if zone(name).is_err() {
            return Err(std::io::Error::other("unknown time zone"));
        }
        let mut records: Vec<_> = self
            .entries
            .iter()
            .map(|p| (p.node, p.endpoint, p.name.clone()))
            .collect();
        if let Some(record) = records.iter_mut().find(|p| p.0 == node && p.1 == endpoint) {
            record.2 = name.to_owned();
        } else {
            records.push((node, endpoint, name.to_owned()));
        }
        if !self
            .entries
            .iter()
            .any(|p| p.node == node && p.endpoint == endpoint && p.name == name)
        {
            self.save(&records)?;
        }
        if let Some(p) = self
            .entries
            .iter_mut()
            .find(|p| p.node == node && p.endpoint == endpoint)
        {
            p.name = name.to_owned();
            p.next = now;
            p.failures = 0;
        } else {
            self.entries.push(Preference {
                node,
                endpoint,
                name: name.to_owned(),
                next: now,
                failures: 0,
            });
        }
        Ok(())
    }
    fn save(&self, records: &[(u64, u16, String)]) -> std::io::Result<()> {
        let bytes = serde_json::to_vec(records)?;
        if records.len() > crate::catalog::MAX_NODES || bytes.len() > 4096 {
            return Err(std::io::Error::other(
                "time-zone preference capacity exceeded",
            ));
        }
        self.store.set(
            KEY,
            if records.is_empty() {
                None
            } else {
                Some(&bytes)
            },
        )
    }
    pub fn remove(&mut self, node: u64) -> std::io::Result<()> {
        if !self.entries.iter().any(|p| p.node == node) {
            return Ok(());
        }
        let records: Vec<_> = self
            .entries
            .iter()
            .filter(|p| p.node != node)
            .map(|p| (p.node, p.endpoint, p.name.clone()))
            .collect();
        self.save(&records)?;
        self.entries.retain(|p| p.node != node);
        Ok(())
    }
    pub fn completed(&mut self, node: u64, endpoint: u16, success: bool, now: Instant, delay: u64) {
        if let Some(p) = self
            .entries
            .iter_mut()
            .find(|p| p.node == node && p.endpoint == endpoint)
        {
            p.failures = if success {
                0
            } else {
                p.failures.saturating_add(1)
            };
            let seconds = if success {
                delay.min(DAY).max(1)
            } else {
                (60u64 << p.failures.saturating_sub(1).min(6)).min(3600)
            };
            p.next = now + Duration::from_secs(seconds);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn unix(s: &str) -> u64 {
        chrono::DateTime::parse_from_rfc3339(s).unwrap().timestamp() as u64
    }
    #[test]
    fn berlin_transitions_and_matter_units() {
        let s = schedule("Europe/Berlin", unix("2026-03-29T00:59:59Z"), 3).unwrap();
        assert_eq!(s.base, 3600);
        assert_eq!(
            s.intervals[0].end,
            (unix("2026-03-29T01:00:00Z") - EPOCH) * 1_000_000
        );
        assert_eq!(s.intervals[1].offset, 3600);
        assert_eq!(
            s.intervals[1].end,
            (unix("2026-10-25T01:00:00Z") - EPOCH) * 1_000_000
        );
        assert_eq!(s.intervals[2].offset, 0);
        for p in s.intervals.windows(2) {
            assert_eq!(p[0].end, p[1].start);
        }
        assert_eq!(
            schedule("Europe/Berlin", unix("2026-10-25T01:00:00Z"), 1)
                .unwrap()
                .base,
            3600
        );
        assert_eq!(
            schedule("Europe/Berlin", unix("2026-03-29T01:00:00Z"), 1)
                .unwrap()
                .base,
            3600
        );
    }
    #[test]
    fn transition_instants_select_the_new_offset() {
        for (time, expected) in [
            ("2026-03-29T00:59:59Z", 3600),
            ("2026-03-29T01:00:00Z", 7200),
            ("2026-10-25T00:59:59Z", 7200),
            ("2026-10-25T01:00:00Z", 3600),
        ] {
            let s = schedule("Europe/Berlin", unix(time), 1).unwrap();
            assert_eq!(s.base + s.intervals[0].offset, expected);
        }
    }
    #[test]
    fn zones_capacity_and_expiry() {
        let now = unix("2026-01-01T00:00:00Z");
        let utc = schedule("UTC", now, 5).unwrap();
        assert_eq!(utc.intervals.len(), 1);
        assert_eq!(utc.intervals[0].offset, 0);
        let south = schedule("Australia/Sydney", now, 2).unwrap();
        assert_eq!(south.intervals[0].offset, 3600);
        assert_eq!(south.intervals[1].offset, 0);
        let half = schedule("Australia/Lord_Howe", now, 2).unwrap();
        assert_eq!(half.intervals[0].offset, 1800);
        assert_eq!(half.intervals[1].offset, 0);
        assert_eq!(
            schedule("Europe/Berlin", now, 1).unwrap().intervals.len(),
            1
        );
        assert!(schedule("Europe/Berlin", now, 0).is_err());
        assert!(schedule("invalid", now, 2).is_err());
        assert!(schedule("UTC", 0, 2).is_err());
    }
    #[test]
    fn durable_intent_retry_and_no_periodic_flash_writes() {
        let directory =
            std::env::temp_dir().join(format!("matter-time-preferences-{}", std::process::id()));
        let now = Instant::now();
        {
            let store = PrivateStore::open(&directory).unwrap();
            let mut prefs = Preferences::open(store).unwrap();
            prefs.set(2, 0, "Europe/Berlin", now).unwrap();
            let before = std::fs::read(directory.join("credentials.bin")).unwrap();
            prefs.completed(2, 0, false, now, 86400);
            assert_eq!(prefs.entries[0].next.duration_since(now).as_secs(), 60);
            for _ in 0..10 {
                prefs.completed(2, 0, false, now, 86400);
            }
            assert_eq!(prefs.entries[0].next.duration_since(now).as_secs(), 3600);
            prefs.completed(2, 0, true, now, 15);
            assert_eq!(prefs.entries[0].next.duration_since(now).as_secs(), 15);
            prefs.completed(2, 0, true, now, 999999);
            assert_eq!(prefs.entries[0].next.duration_since(now).as_secs(), DAY);
            prefs.set(2, 0, "Europe/Berlin", now).unwrap();
            assert_eq!(
                before,
                std::fs::read(directory.join("credentials.bin")).unwrap()
            );
        }
        {
            let store = PrivateStore::open(&directory).unwrap();
            let mut prefs = Preferences::open(store.clone()).unwrap();
            assert_eq!(prefs.entries.len(), 1);
            assert_eq!(prefs.entries[0].name, "Europe/Berlin");
            assert!(prefs.entries[0].next <= Instant::now());
            assert!(prefs.set(2, 0, "invalid", now).is_err());
            prefs.remove(2).unwrap();
            assert!(Preferences::open(store).unwrap().entries.is_empty());
        }
        std::fs::remove_dir_all(directory).unwrap();
    }
}

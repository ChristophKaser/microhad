//! Single-owner commissioning lifetime, including the actual owned future.
//! Session IDs identify jobs; IPC correlations identify individual calls.
use crate::ble::BleRelay;
use crate::control::{valid_pin, CommissionStart, Id};
use rs_matter::crypto::{TryCryptoRng, TryRng};
use rs_matter::transport::network::BtAddr;
use serde::Serialize;
use std::cell::Cell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll};
use std::time::{Duration, Instant};
use zeroize::Zeroizing;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    WaitingBle,
    Pase,
    Provisioning,
    WaitingThread,
    Case,
    Completing,
    Complete,
    Cancelled,
    Failed,
    TimedOut,
}
impl Phase {
    fn terminal(self) -> bool {
        self >= Self::Complete
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionError {
    Malformed,
    NotReady,
    Busy,
    StaleSession,
    StaleGeneration,
    Timeout,
    Cancelled,
    Transport,
    Storage,
    Capacity,
    Accessory,
}
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Ticket {
    pub session: Id,
    pub generation: u32,
}
#[derive(Clone)]
pub struct Progress(Rc<Cell<Phase>>);
impl Progress {
    pub fn set(&self, phase: Phase) {
        self.0.set(phase);
    }
}
#[derive(Serialize)]
pub struct Snapshot {
    pub session: Id,
    pub state: Phase,
    pub deadline_remaining_seconds: u16,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<SessionError>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub node: Option<Id>,
}
// These contain a passcode or opaque BTP data and deliberately have no Debug.
pub struct Attempt {
    pub ticket: Ticket,
    pub pin: Zeroizing<u32>,
    pub timeout_seconds: u16,
    pub country_code: String,
    pub regulatory_location: crate::control::RegulatoryLocation,
    pub progress: Progress,
}
#[derive(Serialize)]
pub struct Write {
    pub session: Id,
    pub generation: u32,
    pub write: u32,
    #[serde(serialize_with = "crate::responses::serialize_hex")]
    pub data: Vec<u8>,
}
struct Session {
    id: Id,
    pin: Option<Zeroizing<u32>>,
    deadline: Instant,
    country_code: String,
    regulatory_location: crate::control::RegulatoryLocation,
    phase: Phase,
    progress: Progress,
    generation: Option<u32>,
    mtu: u16,
    indication: u32,
    error: Option<SessionError>,
    node: Option<Id>,
}
type Task<'a> = Pin<Box<dyn Future<Output = Result<Id, SessionError>> + 'a>>;
pub struct Sessions<'a> {
    relay: &'a BleRelay,
    current: Option<Session>,
    task: Option<Task<'a>>,
    exhausted: bool,
}
impl<'a> Sessions<'a> {
    pub fn new(relay: &'a BleRelay) -> Self {
        Self {
            relay,
            current: None,
            task: None,
            exhausted: false,
        }
    }
    fn finish(&mut self, phase: Phase, error: Option<SessionError>, node: Option<Id>) {
        // Drop the future before clearing BTP or destroying its credentials.
        self.task = None;
        if self.relay.invalidate().is_err() {
            self.exhausted = true;
        }
        if let Some(session) = self.current.as_mut() {
            session.pin = None;
            session.phase = phase;
            session.progress.set(phase);
            session.generation = None;
            session.error = error;
            session.node = node;
        }
    }
    pub fn tick(&mut self, now: Instant) {
        if self
            .current
            .as_ref()
            .is_some_and(|s| !s.phase.terminal() && now >= s.deadline)
        {
            self.finish(Phase::TimedOut, Some(SessionError::Timeout), None);
        }
    }
    fn active(&mut self, id: Id, now: Instant) -> Result<&mut Session, SessionError> {
        self.tick(now);
        let session = self
            .current
            .as_mut()
            .filter(|s| s.id == id)
            .ok_or(SessionError::StaleSession)?;
        if session.phase.terminal() {
            return Err(match session.phase {
                Phase::TimedOut => SessionError::Timeout,
                Phase::Cancelled => SessionError::Cancelled,
                _ => SessionError::StaleSession,
            });
        }
        Ok(session)
    }
    pub fn start<R: TryRng + TryCryptoRng>(
        &mut self,
        request: CommissionStart,
        entropy: &mut R,
        now: Instant,
    ) -> Result<Snapshot, SessionError> {
        self.tick(now);
        if self.exhausted {
            return Err(SessionError::Capacity);
        }
        if self.current.as_ref().is_some_and(|s| !s.phase.terminal()) {
            return Err(SessionError::Busy);
        }
        if !valid_pin(request.pin)
            || request.discriminator > 4095
            || !(30..=900).contains(&request.timeout_seconds)
        {
            return Err(SessionError::Malformed);
        }
        let previous = self.current.as_ref().map(|s| s.id);
        let mut id = None;
        for _ in 0..4 {
            let candidate = Id(entropy
                .try_next_u64()
                .map_err(|_| SessionError::Transport)?);
            if candidate.0 != 0 && Some(candidate) != previous {
                id = Some(candidate);
                break;
            }
        }
        let id = id.ok_or(SessionError::Capacity)?;
        self.current = Some(Session {
            id,
            pin: Some(Zeroizing::new(request.pin)),
            deadline: now + Duration::from_secs(u64::from(request.timeout_seconds)),
            country_code: request.country_code,
            regulatory_location: request.regulatory_location,
            phase: Phase::WaitingBle,
            progress: Progress(Rc::new(Cell::new(Phase::WaitingBle))),
            generation: None,
            mtu: 0,
            indication: 1,
            error: None,
            node: None,
        });
        self.snapshot(id, now)
    }
    pub fn snapshot(&mut self, id: Id, now: Instant) -> Result<Snapshot, SessionError> {
        self.tick(now);
        let session = self
            .current
            .as_ref()
            .filter(|s| s.id == id)
            .ok_or(SessionError::StaleSession)?;
        let remaining = if session.phase.terminal() {
            0
        } else {
            let duration = session.deadline.saturating_duration_since(now);
            (duration.as_secs() + u64::from(duration.subsec_nanos() != 0)).min(900) as u16
        };
        Ok(Snapshot {
            session: id,
            state: session.phase,
            deadline_remaining_seconds: remaining,
            error: session.error,
            node: session.node,
        })
    }
    pub fn current_snapshot(&mut self, now: Instant) -> Option<Snapshot> {
        let id = self.current.as_ref()?.id;
        self.snapshot(id, now).ok()
    }
    pub fn fail(&mut self, error: SessionError) {
        if self
            .current
            .as_ref()
            .is_some_and(|session| !session.phase.terminal())
        {
            self.finish(Phase::Failed, Some(error), None);
        }
    }
    pub fn cancel(&mut self, id: Id, now: Instant) -> Result<Snapshot, SessionError> {
        self.tick(now);
        let session = self
            .current
            .as_ref()
            .filter(|s| s.id == id)
            .ok_or(SessionError::StaleSession)?;
        if !session.phase.terminal() {
            self.finish(Phase::Cancelled, Some(SessionError::Cancelled), None);
        }
        self.snapshot(id, now)
    }
    pub fn attach(
        &mut self,
        id: Id,
        peer: BtAddr,
        mtu: u16,
        now: Instant,
    ) -> Result<Ticket, SessionError> {
        let session = self.active(id, now)?;
        if !matches!(session.phase, Phase::WaitingBle | Phase::Pase) {
            return Err(SessionError::Busy);
        }
        if !(23..=517).contains(&mtu) {
            return Err(SessionError::Malformed);
        }
        // Cancellation of an old attempt happens before BTP connection replacement.
        self.task = None;
        let generation = match self.relay.connect(peer, mtu) {
            Ok(generation) => generation,
            Err(error) => {
                let code = if error.code() == rs_matter::error::ErrorCode::NoSpace {
                    self.exhausted = true;
                    SessionError::Capacity
                } else {
                    SessionError::Transport
                };
                self.finish(Phase::Failed, Some(code), None);
                return Err(code);
            }
        };
        let Ok(generation) = u32::try_from(generation) else {
            self.exhausted = true;
            self.finish(Phase::Failed, Some(SessionError::Capacity), None);
            return Err(SessionError::Capacity);
        };
        let session = self.current.as_mut().unwrap();
        session.phase = Phase::Pase;
        // A fresh progress cell prevents a retired attempt from publishing state.
        session.progress = Progress(Rc::new(Cell::new(Phase::Pase)));
        session.generation = Some(generation);
        session.indication = 1;
        session.mtu = mtu;
        Ok(Ticket {
            session: id,
            generation,
        })
    }
    fn check_ticket(&mut self, ticket: Ticket, now: Instant) -> Result<&mut Session, SessionError> {
        let session = self.active(ticket.session, now)?;
        if session.generation != Some(ticket.generation) {
            return Err(SessionError::StaleGeneration);
        }
        Ok(session)
    }
    /// Keep one zeroizing passcode for bounded reconnects during PASE, and move
    /// a zeroizing copy into each owned attempt. Terminal states clear both.
    pub fn attempt(&mut self, ticket: Ticket, now: Instant) -> Result<Attempt, SessionError> {
        if self.task.is_some() {
            return Err(SessionError::Busy);
        }
        let session = self.check_ticket(ticket, now)?;
        let pin = Zeroizing::new(**session.pin.as_ref().ok_or(SessionError::Busy)?);
        Ok(Attempt {
            ticket,
            pin,
            timeout_seconds: session.deadline.saturating_duration_since(now)
                .as_secs()
                .clamp(30, 900) as u16,
            country_code: session.country_code.clone(),
            regulatory_location: session.regulatory_location,
            progress: session.progress.clone(),
        })
    }
    pub fn install(
        &mut self,
        ticket: Ticket,
        task: impl Future<Output = Result<Id, SessionError>> + 'a,
        now: Instant,
    ) -> Result<(), SessionError> {
        self.check_ticket(ticket, now)?;
        if self.task.is_some() {
            return Err(SessionError::Busy);
        }
        self.task = Some(Box::pin(task));
        Ok(())
    }
    pub fn indication(
        &mut self,
        ticket: Ticket,
        sequence: u32,
        data: &[u8],
        now: Instant,
    ) -> Result<(), SessionError> {
        let session = self.check_ticket(ticket, now)?;
        if sequence != session.indication {
            return Err(SessionError::StaleGeneration);
        }
        if data.is_empty() || data.len() > usize::from((session.mtu - 3).min(244)) {
            return Err(SessionError::Malformed);
        }
        let Some(next) = sequence.checked_add(1) else {
            self.finish(Phase::Failed, Some(SessionError::Capacity), None);
            return Err(SessionError::Capacity);
        };
        if self
            .relay
            .indication(u64::from(ticket.generation), data)
            .is_err()
        {
            self.finish(Phase::Failed, Some(SessionError::Transport), None);
            return Err(SessionError::Transport);
        }
        self.current.as_mut().unwrap().indication = next;
        Ok(())
    }
    pub fn write_complete(
        &mut self,
        ticket: Ticket,
        write: u32,
        success: bool,
        now: Instant,
    ) -> Result<(), SessionError> {
        self.check_ticket(ticket, now)?;
        if let Err(error) = self
            .relay
            .write_complete(u64::from(ticket.generation), write, success)
        {
            if error.code() == rs_matter::error::ErrorCode::InvalidState {
                return Err(SessionError::StaleGeneration);
            }
            let code = if error.code() == rs_matter::error::ErrorCode::RxTimeout {
                SessionError::Timeout
            } else {
                SessionError::Transport
            };
            self.finish(Phase::Failed, Some(code), None);
            return Err(code);
        }
        Ok(())
    }
    pub fn next_write(&mut self, now: Instant) -> Result<Option<Write>, SessionError> {
        self.tick(now);
        let Some(session) = self.current.as_ref().filter(|s| !s.phase.terminal()) else {
            return Ok(None);
        };
        let Some(generation) = session.generation else {
            return Ok(None);
        };
        let id = session.id;
        match self.relay.next_write(u64::from(generation)) {
            Ok(packet) => Ok(packet.map(|p| Write {
                session: id,
                generation,
                write: p.sequence,
                data: p.bytes,
            })),
            Err(error) => {
                let code = if error.code() == rs_matter::error::ErrorCode::RxTimeout {
                    SessionError::Timeout
                } else {
                    SessionError::Transport
                };
                self.finish(Phase::Failed, Some(code), None);
                Err(code)
            }
        }
    }
    pub fn poll(&mut self, now: Instant, context: &mut Context<'_>) {
        self.tick(now);
        if self.current.as_ref().is_none_or(|s| s.phase.terminal()) {
            return;
        }
        if self.relay.check_timeout().is_err() {
            self.finish(Phase::Failed, Some(SessionError::Timeout), None);
            return;
        }
        let result = self.task.as_mut().map(|task| task.as_mut().poll(context));
        // A synchronous crypto poll can cross the wall-clock deadline.
        self.tick(now.max(Instant::now()));
        if self.current.as_ref().is_none_or(|s| s.phase.terminal()) {
            return;
        }
        let session = self.current.as_mut().unwrap();
        let phase = session.progress.0.get();
        if phase < session.phase || phase.terminal() {
            self.finish(Phase::Failed, Some(SessionError::Transport), None);
            return;
        }
        session.phase = phase;
        match result {
            Some(Poll::Ready(Ok(node))) if node.0 != 0 && phase == Phase::Completing => {
                self.finish(Phase::Complete, None, Some(node))
            }
            Some(Poll::Ready(Ok(_))) => {
                self.finish(Phase::Failed, Some(SessionError::Transport), None)
            }
            Some(Poll::Ready(Err(error))) => self.finish(Phase::Failed, Some(error), None),
            _ => (),
        }
    }
}
impl Drop for Sessions<'_> {
    fn drop(&mut self) {
        self.task = None;
        let _ = self.relay.invalidate();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::control::{Request, Schema};
    use crate::entropy::OsEntropy;
    use std::task::Waker;
    fn start(sessions: &mut Sessions<'_>, now: Instant) -> Id {
        let request = CommissionStart {
            schema: Schema,
            discriminator: 3840,
            pin: 20202021,
            timeout_seconds: 30,
            country_code: "DE".to_owned(),
            regulatory_location: crate::control::RegulatoryLocation::Indoor,
        };
        sessions
            .start(request, &mut OsEntropy::open().unwrap(), now)
            .unwrap()
            .session
    }
    struct Pending(Rc<Cell<u32>>);
    impl Future for Pending {
        type Output = Result<Id, SessionError>;
        fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<Self::Output> {
            Poll::Pending
        }
    }
    impl Drop for Pending {
        fn drop(&mut self) {
            self.0.set(self.0.get() + 1);
        }
    }
    #[test]
    fn one_job_cancel_drops_future_and_old_sessions_are_stale() {
        let relay = BleRelay::new();
        let mut sessions = Sessions::new(&relay);
        let now = Instant::now();
        let id = start(&mut sessions, now);
        let Request::CommissionStart(request) = Request::decode(
            3,
            br#"{"schema":1,"discriminator":1,"pin":20202021,"timeout_seconds":30,"country_code":"DE","regulatory_location":"indoor"}"#,
        )
        .unwrap() else {
            unreachable!()
        };
        assert!(matches!(
            sessions.start(request, &mut OsEntropy::open().unwrap(), now),
            Err(SessionError::Busy)
        ));
        let ticket = sessions.attach(id, BtAddr([1; 6]), 23, now).unwrap();
        let dropped = Rc::new(Cell::new(0));
        sessions
            .install(ticket, Pending(dropped.clone()), now)
            .unwrap();
        assert!(matches!(
            sessions.cancel(id, now).unwrap().state,
            Phase::Cancelled
        ));
        assert_eq!(dropped.get(), 1);
        assert!(sessions.current.as_ref().unwrap().pin.is_none());
        assert!(matches!(
            sessions.cancel(id, now).unwrap().state,
            Phase::Cancelled
        ));
        let new = start(&mut sessions, now);
        assert!(id != new);
        assert!(matches!(
            sessions.cancel(id, now),
            Err(SessionError::StaleSession)
        ));
        assert!(matches!(
            sessions.indication(ticket, 1, &[1], now),
            Err(SessionError::StaleSession)
        ));
    }
    #[test]
    fn monotonic_deadline_is_not_extended_and_timeout_drops_task() {
        let relay = BleRelay::new();
        let mut sessions = Sessions::new(&relay);
        let now = Instant::now();
        let id = start(&mut sessions, now);
        let ticket = sessions.attach(id, BtAddr([1; 6]), 23, now).unwrap();
        let dropped = Rc::new(Cell::new(0));
        sessions
            .install(ticket, Pending(dropped.clone()), now)
            .unwrap();
        assert_eq!(
            sessions
                .snapshot(id, now + Duration::from_secs(29))
                .unwrap()
                .deadline_remaining_seconds,
            1
        );
        let snapshot = sessions
            .snapshot(id, now + Duration::from_secs(30))
            .unwrap();
        assert!(matches!(snapshot.state, Phase::TimedOut));
        assert_eq!(dropped.get(), 1);
        assert!(relay.network(u64::from(ticket.generation)).is_err());
        let json = serde_json::to_string(&snapshot).unwrap();
        assert!(!json.contains("20202021"));
        assert!(json.contains("timed_out"));
        assert!(start(&mut sessions, now + Duration::from_secs(31)) != id);
    }
    #[test]
    fn reconnect_drops_old_task_and_stale_callbacks_leave_new_generation_alive() {
        let relay = BleRelay::new();
        let mut sessions = Sessions::new(&relay);
        let now = Instant::now();
        let id = start(&mut sessions, now);
        let old = sessions.attach(id, BtAddr([1; 6]), 23, now).unwrap();
        let old_attempt = sessions.attempt(old, now).unwrap();
        assert_eq!(*old_attempt.pin, 20202021);
        let dropped = Rc::new(Cell::new(0));
        sessions
            .install(old, Pending(dropped.clone()), now)
            .unwrap();
        let old_progress = sessions.current.as_ref().unwrap().progress.clone();
        let new = sessions.attach(id, BtAddr([2; 6]), 247, now).unwrap();
        assert_eq!(dropped.get(), 1);
        assert!(new.generation > old.generation);
        assert_eq!(*sessions.attempt(new, now).unwrap().pin, 20202021);
        assert!(matches!(
            sessions.indication(old, 1, &[1], now),
            Err(SessionError::StaleGeneration)
        ));
        assert!(matches!(
            sessions.indication(new, 2, &[1], now),
            Err(SessionError::StaleGeneration)
        ));
        assert!(matches!(
            sessions.write_complete(old, 1, true, now),
            Err(SessionError::StaleGeneration)
        ));
        old_progress.set(Phase::Failed);
        sessions.poll(now, &mut Context::from_waker(Waker::noop()));
        assert!(matches!(
            sessions.snapshot(id, now).unwrap().state,
            Phase::Pase
        ));
        let packet = sessions.next_write(now).unwrap().unwrap();
        assert!(sessions.next_write(now).unwrap().is_none());
        assert!(matches!(
            sessions.write_complete(new, packet.write + 1, true, now),
            Err(SessionError::StaleGeneration)
        ));
        sessions
            .write_complete(new, packet.write, true, now)
            .unwrap();
        assert!(relay.network(u64::from(new.generation)).is_ok());
    }
    #[test]
    fn explicit_completion_and_failure_states_retire_sensitive_attempt() {
        for success in [true, false] {
            let relay = BleRelay::new();
            let mut sessions = Sessions::new(&relay);
            let now = Instant::now();
            let id = start(&mut sessions, now);
            let ticket = sessions.attach(id, BtAddr([1; 6]), 23, now).unwrap();
            let attempt = sessions.attempt(ticket, now).unwrap();
            let progress = attempt.progress.clone();
            sessions
                .install(
                    ticket,
                    async move {
                        assert_eq!(*attempt.pin, 20202021);
                        progress.set(Phase::Completing);
                        if success {
                            Ok(Id(123))
                        } else {
                            Err(SessionError::Accessory)
                        }
                    },
                    now,
                )
                .unwrap();
            sessions.poll(now, &mut Context::from_waker(Waker::noop()));
            let snapshot = sessions.snapshot(id, now).unwrap();
            assert!(
                snapshot.state
                    == if success {
                        Phase::Complete
                    } else {
                        Phase::Failed
                    }
            );
            assert!(sessions.task.is_none());
            assert!(sessions.current.as_ref().unwrap().pin.is_none());
            assert!(relay.network(u64::from(ticket.generation)).is_err());
        }
    }
    #[test]
    fn backwards_phase_and_failed_gatt_write_fail_closed() {
        let relay = BleRelay::new();
        let mut sessions = Sessions::new(&relay);
        let now = Instant::now();
        let id = start(&mut sessions, now);
        sessions.attach(id, BtAddr([1; 6]), 23, now).unwrap();
        sessions
            .current
            .as_ref()
            .unwrap()
            .progress
            .set(Phase::WaitingBle);
        sessions.poll(now, &mut Context::from_waker(Waker::noop()));
        assert!(matches!(
            sessions.snapshot(id, now).unwrap().state,
            Phase::Failed
        ));
        let id = start(&mut sessions, now);
        let ticket = sessions.attach(id, BtAddr([1; 6]), 23, now).unwrap();
        let packet = sessions.next_write(now).unwrap().unwrap();
        assert!(matches!(
            sessions.write_complete(ticket, packet.write, false, now),
            Err(SessionError::Transport)
        ));
        assert!(matches!(
            sessions.snapshot(id, now).unwrap().state,
            Phase::Failed
        ));
    }
}

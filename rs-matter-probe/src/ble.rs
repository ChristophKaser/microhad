//! Gateway-owned BTP with a phone carrying C1 writes and C2 indications.
//! Connection generations reject callbacks from an old phone/GATT connection.
use rs_matter::error::{Error, ErrorCode};
use rs_matter::transport::network::{btp::Btp, BtAddr};
use std::cell::{Cell, RefCell};
use std::time::{Duration, Instant};

#[derive(Clone, Copy)]
struct Connection {
    generation: u64,
    peer: BtAddr,
    mtu: u16,
}
struct PendingWrite {
    sequence: u32,
    started: Instant,
}

// Intentionally no Debug/Display: a GATT packet can carry encrypted commissioning data.
pub struct GattWrite {
    pub connection: u64,
    pub sequence: u32,
    pub bytes: Vec<u8>,
}

pub struct BleRelay {
    btp: Btp,
    connection: Cell<Option<Connection>>,
    generation: Cell<u64>,
    sequence: Cell<u32>,
    pending: RefCell<Option<PendingWrite>>,
}
impl Default for BleRelay {
    fn default() -> Self {
        Self::new()
    }
}
impl BleRelay {
    pub const fn new() -> Self {
        Self {
            btp: Btp::new(),
            connection: Cell::new(None),
            generation: Cell::new(0),
            sequence: Cell::new(0),
            pending: RefCell::new(None),
        }
    }
    /// Call only once C2 indications are enabled and the phone reports its ATT MTU.
    /// The owner cancels any previous commissioning future before reconnecting.
    pub fn connect(&self, peer: BtAddr, mtu: u16) -> Result<u64, Error> {
        if !(23..=517).contains(&mtu) {
            return Err(ErrorCode::InvalidData.into());
        }
        let generation = self
            .generation
            .get()
            .checked_add(1)
            .ok_or(ErrorCode::NoSpace)?;
        self.clear();
        self.generation.set(generation);
        self.sequence.set(0);
        self.connection.set(Some(Connection {
            generation,
            peer,
            mtu,
        }));
        self.btp.set_initiator(true);
        Ok(generation)
    }
    fn active(&self, generation: u64) -> Result<Connection, Error> {
        self.connection
            .get()
            .filter(|c| c.generation == generation)
            .ok_or_else(|| ErrorCode::InvalidState.into())
    }
    fn clear(&self) {
        self.connection.set(None);
        *self.pending.borrow_mut() = None;
        self.btp.reset();
    }
    pub fn disconnect(&self, generation: u64) -> Result<(), Error> {
        self.active(generation)?;
        self.clear();
        Ok(())
    }
    /// Drop transport state and invalidate callbacks before a new attempt.
    pub fn invalidate(&self) -> Result<(), Error> {
        self.clear();
        let next = self
            .generation
            .get()
            .checked_add(1)
            .ok_or(ErrorCode::NoSpace)?;
        self.generation.set(next);
        Ok(())
    }
    /// Check an in-flight write without consuming another outgoing packet.
    pub fn check_timeout(&self) -> Result<(), Error> {
        let expired = self
            .pending
            .borrow()
            .as_ref()
            .is_some_and(|p| p.started.elapsed() >= Duration::from_secs(10));
        if expired {
            self.clear();
            return Err(ErrorCode::RxTimeout.into());
        }
        Ok(())
    }
    pub fn network(&self, generation: u64) -> Result<&Btp, Error> {
        self.active(generation)?;
        Ok(&self.btp)
    }
    /// Accept only characteristic C2 indication payloads from the current phone connection.
    pub fn indication(&self, generation: u64, data: &[u8]) -> Result<(), Error> {
        let c = self.active(generation)?;
        // The pinned BTP implementation supports at most 244-byte segments.
        if data.is_empty() || data.len() > usize::from((c.mtu - 3).min(244)) {
            return Err(ErrorCode::InvalidData.into());
        }
        if let Err(error) = self.btp.process_incoming(Some(c.mtu), c.peer, data) {
            self.clear();
            return Err(error);
        }
        Ok(())
    }
    /// One C1 write with response may be in flight. Do not request another until
    /// the phone's onCharacteristicWrite callback acknowledges this sequence.
    pub fn next_write(&self, generation: u64) -> Result<Option<GattWrite>, Error> {
        let c = self.active(generation)?;
        let pending_expired = self
            .pending
            .borrow()
            .as_ref()
            .map(|pending| pending.started.elapsed() >= Duration::from_secs(10));
        if let Some(expired) = pending_expired {
            if expired {
                self.clear();
                return Err(ErrorCode::RxTimeout.into());
            }
            return Ok(None);
        }
        let sequence = self
            .sequence
            .get()
            .checked_add(1)
            .ok_or(ErrorCode::NoSpace)?;
        let mut bytes = [0; 512];
        let length = self.btp.process_outgoing(Some(c.mtu), &mut bytes)?;
        if length == 0 {
            return Ok(None);
        }
        self.sequence.set(sequence);
        *self.pending.borrow_mut() = Some(PendingWrite {
            sequence,
            started: Instant::now(),
        });
        Ok(Some(GattWrite {
            connection: generation,
            sequence,
            bytes: bytes[..length].to_vec(),
        }))
    }
    pub fn write_complete(
        &self,
        generation: u64,
        sequence: u32,
        success: bool,
    ) -> Result<(), Error> {
        self.active(generation)?;
        if self.pending.borrow().as_ref().map(|p| p.sequence) != Some(sequence) {
            return Err(ErrorCode::InvalidState.into());
        }
        let expired =
            self.pending.borrow().as_ref().unwrap().started.elapsed() >= Duration::from_secs(10);
        if expired {
            self.clear();
            return Err(ErrorCode::RxTimeout.into());
        }
        *self.pending.borrow_mut() = None;
        if !success {
            self.clear();
            return Err(ErrorCode::NoNetworkInterface.into());
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::future::Future;
    use core::pin::pin;
    use core::task::{Context, Poll, Waker};

    fn ready<T>(future: impl Future<Output = T>) -> T {
        let mut future = pin!(future);
        match future
            .as_mut()
            .poll(&mut Context::from_waker(Waker::noop()))
        {
            Poll::Ready(result) => result,
            Poll::Pending => panic!("fixture expected immediate completion"),
        }
    }
    fn transfer(relay: &BleRelay, id: u64, accessory: &Btp, mtu: u16) -> bool {
        let Some(packet) = relay.next_write(id).unwrap() else {
            return false;
        };
        assert!(relay.next_write(id).unwrap().is_none());
        accessory
            .process_incoming(Some(mtu), BtAddr([0xaa; 6]), &packet.bytes)
            .unwrap();
        relay.write_complete(id, packet.sequence, true).unwrap();
        true
    }
    fn reply(relay: &BleRelay, id: u64, accessory: &Btp, mtu: u16) -> bool {
        let mut bytes = [0; 512];
        let length = accessory.process_outgoing(Some(mtu), &mut bytes).unwrap();
        if length == 0 {
            return false;
        }
        relay.indication(id, &bytes[..length]).unwrap();
        true
    }
    fn receive(
        relay: &BleRelay,
        id: u64,
        accessory: &Btp,
        mtu: u16,
        reverse: bool,
    ) -> (Vec<u8>, BtAddr) {
        let mut bytes = [0; 1500];
        let receiver = if reverse {
            relay.network(id).unwrap()
        } else {
            accessory
        };
        let (length, peer) = {
            let mut future = pin!(receiver.recv(&mut bytes));
            let deadline = Instant::now() + Duration::from_secs(10);
            loop {
                let moved = transfer(relay, id, accessory, mtu) | reply(relay, id, accessory, mtu);
                if let Poll::Ready(result) = future
                    .as_mut()
                    .poll(&mut Context::from_waker(Waker::noop()))
                {
                    break result.unwrap();
                }
                assert!(Instant::now() < deadline, "BTP delivery timed out");
                if !moved {
                    std::thread::sleep(Duration::from_millis(1));
                }
            }
        };
        (bytes[..length].to_vec(), peer)
    }
    #[test]
    fn fragments_and_reassembles_both_directions_at_minimum_and_large_mtu() {
        for mtu in [23, 247] {
            let relay = BleRelay::new();
            let id = relay.connect(BtAddr([0xbb; 6]), mtu).unwrap();
            let accessory = Btp::new();
            let payload: Vec<u8> = (0..1024).map(|i| i as u8).collect();
            ready(relay.network(id).unwrap().send(&payload, BtAddr([0xbb; 6]))).unwrap();
            let (bytes, peer) = receive(&relay, id, &accessory, mtu, false);
            assert_eq!(peer, BtAddr([0xaa; 6]));
            assert_eq!(bytes, payload);
            ready(accessory.send(&payload, BtAddr([0xaa; 6]))).unwrap();
            let (bytes, peer) = receive(&relay, id, &accessory, mtu, true);
            assert_eq!(peer, BtAddr([0xbb; 6]));
            assert_eq!(bytes, payload);
        }
    }
    #[test]
    fn expired_phone_write_resets_connection() {
        let relay = BleRelay::new();
        let id = relay.connect(BtAddr([1; 6]), 23).unwrap();
        relay.next_write(id).unwrap().unwrap();
        relay.pending.borrow_mut().as_mut().unwrap().started =
            Instant::now() - Duration::from_secs(11);
        assert!(relay.next_write(id).is_err());
        assert!(relay.network(id).is_err());
    }
    #[test]
    fn stale_callbacks_and_failed_writes_cannot_corrupt_new_connection() {
        let relay = BleRelay::new();
        assert!(relay.connect(BtAddr([1; 6]), 22).is_err());
        let old = relay.connect(BtAddr([1; 6]), 23).unwrap();
        let packet = relay.next_write(old).unwrap().unwrap();
        assert!(relay
            .write_complete(old, packet.sequence + 1, true)
            .is_err());
        let new = relay.connect(BtAddr([2; 6]), 247).unwrap();
        assert!(relay.indication(old, &[1]).is_err());
        assert!(relay.disconnect(old).is_err());
        assert!(relay.write_complete(old, packet.sequence, true).is_err());
        let packet = relay.next_write(new).unwrap().unwrap();
        assert!(relay.write_complete(new, packet.sequence, false).is_err());
        assert!(relay.network(new).is_err());
    }
}

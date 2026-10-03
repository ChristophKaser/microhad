//! Single-threaded rs-matter network adapter for OpenThread internal IPv6/UDP.
use super::commissioning::ThreadDataset;
use core::marker::PhantomData;
use core::task::{Poll, Waker};
use rs_matter::error::{Error, ErrorCode};
use rs_matter::transport::network::{
    Address, IpAddr, NetworkMulticast, NetworkReceive, NetworkSend, SocketAddr, SocketAddrV6,
};
use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::CString;
use std::future::poll_fn;
use std::rc::Rc;

#[repr(C)]
#[derive(Clone, Copy)]
struct OtAddress {
    ip: [u8; 16],
    port: u16,
}
unsafe extern "C" {
    fn gatewayOtInit(url: *const core::ffi::c_char) -> i32;
    fn gatewayOtInitOnChannel(url: *const core::ffi::c_char, channel: u8) -> i32;
    fn gatewayOtDisable() -> i32;
    fn gatewayOtDeinit();
    fn gatewayOtReady() -> bool;
    fn gatewayOtPoll(timeout_us: u32) -> i32;
    fn gatewayOtSetDataset(bytes: *const u8, length: u16) -> i32;
    fn gatewayOtOpen(
        port: u16,
        callback: extern "C" fn(*mut core::ffi::c_void, *const OtAddress, *const u8, u16),
        context: *mut core::ffi::c_void,
    ) -> i32;
    fn gatewayOtClose(id: i32) -> i32;
    fn gatewayOtSend(id: i32, peer: *const OtAddress, bytes: *const u8, length: u16) -> i32;
    fn gatewayOtOwnAddress(address: *mut OtAddress) -> i32;
    fn gatewayOtMulticast(bytes: *const u8, join: bool) -> i32;
}
fn checked(result: i32) -> Result<(), Error> {
    if result == 0 {
        Ok(())
    } else {
        Err(ErrorCode::NoNetworkInterface.into())
    }
}

/// Owns the one OpenThread instance. Rc marker forbids crossing threads.
/// Caller establishes a private, persistent working directory before initialization.
pub struct OpenThread {
    _url: CString,
    _single_thread: PhantomData<Rc<()>>,
}
impl OpenThread {
    pub fn init(url: &str) -> Result<Self, Error> {
        let url = CString::new(url).map_err(|_| ErrorCode::InvalidData)?;
        checked(unsafe { gatewayOtInit(url.as_ptr()) })?;
        Ok(Self {
            _url: url,
            _single_thread: PhantomData,
        })
    }
    pub fn ready(&self) -> bool {
        unsafe { gatewayOtReady() }
    }
    /// Production startup: caller has observed the current coordinator channel.
    /// Saved datasets cannot restore before the validated authority record is applied.
    pub fn init_on_channel(url: &str, channel: u8) -> Result<Self, Error> {
        let url = CString::new(url).map_err(|_| ErrorCode::InvalidData)?;
        checked(unsafe { gatewayOtInitOnChannel(url.as_ptr(), channel) })?;
        Ok(Self {
            _url: url,
            _single_thread: PhantomData,
        })
    }
    pub fn disable(&self) -> Result<(), Error> {
        checked(unsafe { gatewayOtDisable() })
    }
    pub fn poll(&self, timeout_us: u32) -> Result<(), Error> {
        checked(unsafe { gatewayOtPoll(timeout_us) })
    }
    pub fn set_dataset(&self, bytes: &[u8], zigbee_channel: u8) -> Result<(), Error> {
        ThreadDataset::parse(bytes, zigbee_channel)?;
        checked(unsafe { gatewayOtSetDataset(bytes.as_ptr(), bytes.len() as u16) })
    }
    pub fn own_address(&self, port: u16) -> Result<Address, Error> {
        let mut address = OtAddress {
            ip: [0; 16],
            port: 0,
        };
        checked(unsafe { gatewayOtOwnAddress(&mut address) })?;
        Ok(Address::Udp(SocketAddr::V6(SocketAddrV6::new(
            address.ip.into(),
            port,
            0,
            0,
        ))))
    }
    pub fn bind(&self, port: u16) -> Result<ThreadNetwork<'_>, Error> {
        let mut queue = Box::new(RefCell::new(Queue {
            packets: VecDeque::new(),
            waker: None,
        }));
        let id = unsafe { gatewayOtOpen(port, receive, (&mut *queue as *mut RefCell<Queue>).cast()) };
        if id < 0 {
            return Err(ErrorCode::NoNetworkInterface.into());
        }
        Ok(ThreadNetwork {
            _owner: self,
            id,
            queue,
        })
    }
}
impl Drop for OpenThread {
    fn drop(&mut self) {
        unsafe { gatewayOtDeinit() };
    }
}

struct Queue {
    packets: VecDeque<(Vec<u8>, Address)>,
    waker: Option<Waker>,
}
extern "C" fn receive(
    context: *mut core::ffi::c_void,
    peer: *const OtAddress,
    data: *const u8,
    length: u16,
) {
    // Called synchronously by this owner's poll/send; storage lives until otUdpClose.
    let cell = unsafe { &*context.cast::<RefCell<Queue>>() };
    let peer = unsafe { *peer };
    let bytes = unsafe { core::slice::from_raw_parts(data, length as usize) };
    let mut queue = cell.borrow_mut();
    if queue.packets.len() >= 8 {
        return;
    } // Bounded loss; Matter MRP retries UDP packets.
    let address = Address::Udp(SocketAddr::V6(SocketAddrV6::new(
        peer.ip.into(),
        peer.port,
        0,
        0,
    )));
    queue.packets.push_back((bytes.to_vec(), address));
    let waker = queue.waker.take();
    drop(queue);
    if let Some(waker) = waker {
        waker.wake();
    }
}

pub struct ThreadNetwork<'a> {
    _owner: &'a OpenThread,
    id: i32,
    queue: Box<RefCell<Queue>>,
}
impl Drop for ThreadNetwork<'_> {
    fn drop(&mut self) {
        unsafe { gatewayOtClose(self.id) };
    }
}
impl NetworkSend for &ThreadNetwork<'_> {
    async fn send_to(&mut self, data: &[u8], addr: Address) -> Result<(), Error> {
        let Address::Udp(SocketAddr::V6(address)) = addr else {
            return Err(ErrorCode::InvalidData.into());
        };
        if data.len() > 1232 || address.port() == 0 {
            return Err(ErrorCode::InvalidData.into());
        }
        let peer = OtAddress {
            ip: address.ip().octets(),
            port: address.port(),
        };
        checked(unsafe { gatewayOtSend(self.id, &peer, data.as_ptr(), data.len() as u16) })
    }
}
impl NetworkReceive for &ThreadNetwork<'_> {
    async fn wait_available(&mut self) -> Result<(), Error> {
        poll_fn(|cx| {
            let mut queue = self.queue.borrow_mut();
            if !queue.packets.is_empty() {
                Poll::Ready(Ok(()))
            } else {
                queue.waker = Some(cx.waker().clone());
                Poll::Pending
            }
        })
        .await
    }
    async fn recv_from(&mut self, buffer: &mut [u8]) -> Result<(usize, Address), Error> {
        let (data, address) = self
            .queue
            .borrow_mut()
            .packets
            .pop_front()
            .ok_or(ErrorCode::InvalidState)?;
        if data.len() > buffer.len() {
            return Err(ErrorCode::BufferTooSmall.into());
        }
        buffer[..data.len()].copy_from_slice(&data);
        Ok((data.len(), address))
    }
}
impl NetworkMulticast for &ThreadNetwork<'_> {
    async fn join(&mut self, addr: IpAddr) -> Result<(), Error> {
        let IpAddr::V6(address) = addr else {
            return Err(ErrorCode::InvalidData.into());
        };
        checked(unsafe { gatewayOtMulticast(address.octets().as_ptr(), true) })
    }
    async fn leave(&mut self, addr: IpAddr) -> Result<(), Error> {
        let IpAddr::V6(address) = addr else {
            return Err(ErrorCode::InvalidData.into());
        };
        checked(unsafe { gatewayOtMulticast(address.octets().as_ptr(), false) })
    }
}

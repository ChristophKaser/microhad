//! Single-threaded rs-matter network adapter for OpenThread internal IPv6/UDP.
use super::commissioning::ThreadDataset;
use core::marker::PhantomData;
use core::task::{Poll, Waker};
use rs_matter::error::{Error, ErrorCode};
use rs_matter::transport::network::{
    Address, IpAddr, NetworkMulticast, NetworkReceive, NetworkSend, SocketAddr, SocketAddrV6,
};
use std::cell::{Cell, RefCell};
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
    fn gatewayOtSetDiagnosticLog(callback: Option<extern "C" fn(u8, *const core::ffi::c_char)>);
    fn gatewayOtSetBindingDiagnostics(enabled: bool);
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
    fn gatewayOtDiscoveryStart() -> i32;
    fn gatewayOtDnsStart(label: *const core::ffi::c_char, token: *mut u32) -> i32;
    fn gatewayOtDnsPoll(token: u32, answer: *mut DnsAnswer) -> i32;
    fn gatewayOtDnsRelease(token: u32);
    fn gatewayOtBrowseStart(fabric: u64, next_node: u64, token: *mut u32) -> i32;
    fn gatewayOtBrowsePoll(token: u32, result: *mut BrowseResult) -> i32;
    fn gatewayOtBrowseRelease(token: u32);
}
fn checked(result: i32) -> Result<(), Error> {
    if result == 0 {
        Ok(())
    } else {
        Err(ErrorCode::NoNetworkInterface.into())
    }
}

extern "C" fn thread_diagnostic(level: u8, message: *const core::ffi::c_char) {
    if message.is_null() {
        return;
    }
    // The adapter passes a bounded, NUL-terminated buffer valid for this call.
    let message = unsafe { std::ffi::CStr::from_ptr(message) }.to_string_lossy();
    log::log!(target: "thread_diagnostics", if level == 2 { log::Level::Info } else { log::Level::Debug }, "{message}");
}

/// Owns the one OpenThread instance. Rc marker forbids crossing threads.
/// Caller establishes a private, persistent working directory before initialization.
pub struct OpenThread {
    _url: CString,
    _single_thread: PhantomData<Rc<()>>,
    binding_diagnostics: Cell<bool>,
}
impl OpenThread {
    pub fn init(url: &str) -> Result<Self, Error> {
        let url = CString::new(url).map_err(|_| ErrorCode::InvalidData)?;
        unsafe {
            gatewayOtSetDiagnosticLog(Some(thread_diagnostic));
        }
        checked(unsafe { gatewayOtInit(url.as_ptr()) })?;
        Ok(Self {
            _url: url,
            _single_thread: PhantomData,
            binding_diagnostics: Cell::new(false),
        })
    }
    pub fn ready(&self) -> bool {
        unsafe { gatewayOtReady() }
    }
    /// Production startup: caller has observed the current coordinator channel.
    /// Saved datasets cannot restore before the validated authority record is applied.
    pub fn init_on_channel(url: &str, channel: u8) -> Result<Self, Error> {
        let url = CString::new(url).map_err(|_| ErrorCode::InvalidData)?;
        unsafe {
            gatewayOtSetDiagnosticLog(Some(thread_diagnostic));
        }
        checked(unsafe { gatewayOtInitOnChannel(url.as_ptr(), channel) })?;
        Ok(Self {
            _url: url,
            _single_thread: PhantomData,
            binding_diagnostics: Cell::new(false),
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
    pub(crate) fn diagnostic_level(&self) -> log::Level {
        if self.binding_diagnostics.get() {
            log::Level::Info
        } else {
            log::Level::Debug
        }
    }
    pub fn set_binding_diagnostics(&self, enabled: bool) {
        if self.binding_diagnostics.replace(enabled) != enabled {
            unsafe {
                gatewayOtSetBindingDiagnostics(enabled);
            }
        }
    }
    pub fn start_discovery(&self) -> Result<(), Error> {
        checked(unsafe { gatewayOtDiscoveryStart() })
    }
    pub async fn resolve_service(&self, label: &str) -> Result<DnsAnswer, Error> {
        let label = CString::new(label).map_err(|_| ErrorCode::InvalidData)?;
        let mut token = 0;
        let result = unsafe { gatewayOtDnsStart(label.as_ptr(), &mut token) };
        if result != 0 {
            log::log!(target: "thread_diagnostics", if self.binding_diagnostics.get() { log::Level::Info } else { log::Level::Debug },
                "DNS start rejected label={} ot_error={result}", label.to_string_lossy());
        }
        checked(result)?;
        let _request = DnsRequest {
            _owner: self,
            token,
        };
        let mut answer = DnsAnswer::empty();
        loop {
            match unsafe { gatewayOtDnsPoll(token, &mut answer) } {
                0 => return Ok(answer),
                1 => embassy_time::Timer::after_millis(10).await,
                error => {
                    log::log!(target: "thread_diagnostics", if self.binding_diagnostics.get() { log::Level::Info } else { log::Level::Debug },
                        "DNS poll failed token={token} ot_error={error}");
                    return Err(ErrorCode::NotFound.into());
                }
            }
        }
    }
    /// Browse operational Matter service labels and retain only this fabric's
    /// IDs below the old allocator high-water mark.
    pub async fn browse_operational_nodes(
        &self,
        fabric: u64,
        next_node: u64,
    ) -> Result<Vec<u64>, Error> {
        let mut token = 0;
        checked(unsafe { gatewayOtBrowseStart(fabric, next_node, &mut token) })?;
        let _request = BrowseRequest {
            _owner: self,
            token,
        };
        let mut result = BrowseResult::empty();
        loop {
            match unsafe { gatewayOtBrowsePoll(token, &mut result) } {
                0 => {
                    if usize::from(result.count) > result.nodes.len() || result.overflow {
                        return Err(ErrorCode::NoSpace.into());
                    }
                    let mut nodes = result.nodes[..usize::from(result.count)].to_vec();
                    nodes.sort_unstable();
                    nodes.dedup();
                    return Ok(nodes);
                }
                1 => embassy_time::Timer::after_millis(10).await,
                _ => return Err(ErrorCode::NotFound.into()),
            }
        }
    }
    pub fn bind(&self, port: u16) -> Result<ThreadNetwork<'_>, Error> {
        let mut queue = Box::new(RefCell::new(Queue {
            packets: VecDeque::new(),
            waker: None,
        }));
        let id =
            unsafe { gatewayOtOpen(port, receive, (&mut *queue as *mut RefCell<Queue>).cast()) };
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
        unsafe {
            gatewayOtDeinit();
            gatewayOtSetDiagnosticLog(None);
        };
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
    let address = Address::Udp(SocketAddr::V6(SocketAddrV6::new(
        peer.ip.into(),
        peer.port,
        0,
        0,
    )));
    #[cfg(feature = "controller-service")]
    crate::logging::packet_received(&address, bytes);
    let mut queue = cell.borrow_mut();
    if queue.packets.len() >= 8 {
        return;
    } // Bounded loss; Matter MRP retries UDP packets.
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
        let result = unsafe { gatewayOtSend(self.id, &peer, data.as_ptr(), data.len() as u16) };
        #[cfg(feature = "controller-service")]
        crate::logging::packet_sent(&address, data, result);
        checked(result)
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

/// Fixed C ABI response; no callback may retain pointers to this value.
#[repr(C)]
#[derive(Clone)]
pub struct DnsAnswer {
    pub addresses: [[u8; 16]; 4],
    pub ttl: u32,
    pub port: u16,
    pub txt_length: u16,
    pub count: u8,
    pub txt: [u8; 256],
}
impl DnsAnswer {
    pub fn empty() -> Self {
        Self {
            addresses: [[0; 16]; 4],
            ttl: 0,
            port: 0,
            txt_length: 0,
            count: 0,
            txt: [0; 256],
        }
    }
}

#[repr(C)]
struct BrowseResult {
    count: u16,
    overflow: bool,
    reserved: u8,
    nodes: [u64; 64],
}
impl BrowseResult {
    fn empty() -> Self {
        Self {
            count: 0,
            overflow: false,
            reserved: 0,
            nodes: [0; 64],
        }
    }
}
struct DnsRequest<'a> {
    _owner: &'a OpenThread,
    token: u32,
}
impl Drop for DnsRequest<'_> {
    fn drop(&mut self) {
        unsafe { gatewayOtDnsRelease(self.token) };
    }
}
struct BrowseRequest<'a> {
    _owner: &'a OpenThread,
    token: u32,
}
impl Drop for BrowseRequest<'_> {
    fn drop(&mut self) {
        unsafe { gatewayOtBrowseRelease(self.token) };
    }
}

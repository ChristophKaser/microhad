//! Exercise cancellation against the same patched rs-matter used by the service.
//! The packet sink is synthetic: no sockets, gateway, or radio are involved.
#![cfg(feature = "controller-service")]

use std::{cell::Cell, future::Future, num::NonZeroU8, pin::pin, task::{Context, Poll}};
use rs_matter::{
    crypto::test_only_crypto,
    dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET},
    error::{Error, ErrorCode},
    im::{OpCode, PROTO_ID_INTERACTION_MODEL},
    transport::{
        exchange::{Exchange, MessageMeta},
        network::{Address, NetworkSend, NoNetwork},
        session::{NocCatIds, ReservedSession, SessionMode},
    },
    utils::init::InitMaybeUninit,
    Matter,
};

struct PacketSink<'a>(&'a Cell<usize>);
impl NetworkSend for PacketSink<'_> {
    async fn send_to(&mut self, _: &[u8], _: Address) -> Result<(), Error> {
        self.0.set(self.0.get() + 1);
        Ok(())
    }
}

fn open<'a>(matter: &'a Matter<'a>, context: &mut Context<'_>) -> Exchange<'a> {
    let mut opening = pin!(Exchange::initiate(matter, test_only_crypto(), NonZeroU8::new(1).unwrap(), 2));
    match opening.as_mut().poll(context) {
        Poll::Ready(Ok(exchange)) => exchange,
        _ => panic!("the preinstalled session must be immediately usable"),
    }
}

fn cancellation_case(unacknowledged: bool) {
    let mut memory = Box::new(std::mem::MaybeUninit::uninit());
    let matter = memory.init_with(Matter::init(&TEST_DEV_DET, TEST_DEV_COMM, &TEST_DEV_ATT, 5540));
    let mut session = ReservedSession::reserve_now(matter, test_only_crypto()).unwrap();
    session.update(1, 2, 1, 1, Address::Udp("127.0.0.1:5540".parse().unwrap()),
        SessionMode::Case { fab_idx: NonZeroU8::new(1).unwrap(), cat_ids: NocCatIds::default() },
        None, None, None, None).unwrap();
    session.complete();
    let mut context = Context::from_waker(std::task::Waker::noop());
    let mut abandoned = open(matter, &mut context);
    let other = open(matter, &mut context);
    let packets = Cell::new(0);
    let mut transport = pin!(matter.run(test_only_crypto(), PacketSink(&packets), NoNetwork, NoNetwork));
    if unacknowledged {
        {
            let mut sending = pin!(abandoned.send(
                MessageMeta::new(PROTO_ID_INTERACTION_MODEL, OpCode::ReadRequest as u8, true),
                &[0x15, 0x18],
            ));
            for _ in 0..8 {
                assert!(sending.as_mut().poll(&mut context).is_pending());
                assert!(transport.as_mut().poll(&mut context).is_pending());
            }
            assert!(packets.get() > 0, "a reliable packet must actually reach the synthetic sink");
        }
        assert!(abandoned.pending_retrans().unwrap());
    } else {
        assert!(!abandoned.pending_retrans().unwrap());
    }
    drop(abandoned);
    for _ in 0..8 {
        assert!(transport.as_mut().poll(&mut context).is_pending());
    }
    let sessions = matter.with_state(|state| state.sessions.iter().count());
    if unacknowledged {
        assert_eq!(sessions, 0, "an abandoned reliable transmission requires session cleanup");
        assert_eq!(other.pending_retrans().unwrap_err().code(), ErrorCode::NoSession);
    } else {
        assert_eq!(sessions, 1, "clean exchange cancellation must preserve the shared session");
        assert!(!other.pending_retrans().unwrap());
    }
}

#[test]
fn clean_cancellation_preserves_other_exchange() {
    cancellation_case(false);
}

#[test]
fn unacknowledged_cancellation_cleans_up_shared_session() {
    cancellation_case(true);
}

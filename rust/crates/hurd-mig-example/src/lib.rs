/*!
 * A minimal `#![no_std]` Hurd translator component, split so the logic is
 * testable.
 *
 * This library holds everything that is worth testing and nothing that is
 * specific to one process:
 *
 *  - [`message`]  -- `mach_msg_header_t` layout plus the validation that has to
 *    happen *before* a buffer reaches `mach_msg()`;
 *  - [`protocol`] -- MIG-shaped message identifiers, in the subsystem/number
 *    layout MIG itself uses;
 *  - [`transport`] -- the message channel, with an in-process implementation for
 *    the host and a real Mach one behind the `mach` feature;
 *  - [`server`] -- the dispatcher.
 *
 * The binary in `main.rs` adds the two things that only make sense for a
 * process: a `#[global_allocator]` forwarding to the C runtime, and the entry
 * point.
 *
 * # Why the split
 *
 * A `#![no_std] #![no_main]` crate cannot be unit-tested the usual way: it
 * defines its own `#[panic_handler]` and `main`, both of which collide with the
 * test harness. Moving the logic into a library sidesteps that entirely -- the
 * library stays `no_std` in every build, and `cargo test` exercises it with the
 * harness in charge.
 */

// The library is always no_std; only the *binary* needs the cfg(test) dance,
// because that is where the panic handler and entry point live.
#![no_std]

extern crate alloc;

pub mod message;
pub mod protocol;
pub mod server;
pub mod transport;

pub use message::{Message, MessageError};
pub use protocol::Request;
pub use server::{Outcome, Translator};
pub use transport::{Loopback, Transport, TransportError};

#[cfg(test)]
mod tests {
    //! End-to-end paths, exercised the way a client sees them.

    use super::*;

    /// The full exchange over the loopback transport.
    ///
    /// Requests are seeded through `push_raw`, so this also exercises the
    /// validation boundary a real transport would go through.
    #[test]
    fn full_exchange_over_loopback() {
        let mut t = Translator::new("hurd-mig-example");
        let mut chan = Loopback::new();

        for req in [
            Request::ping(1, 7),
            Request::echo(1, 7, 42),
            Request::uptime(1, 7),
        ] {
            chan.push_raw(req.to_bytes().as_slice())
                .expect("a well-formed request must be accepted");
        }

        t.serve_all(&mut chan, 8);
        assert_eq!(t.served(), 3);

        // The kernel plays its part: move the replies from outbox to inbox,
        // preserving order.
        assert_eq!(chan.deliver(), 3);
        assert_eq!(chan.recv().expect("reply").id(), protocol::MSG_PONG);
        let echo_reply = chan.recv().expect("reply");
        assert_eq!(echo_reply.id(), protocol::MSG_ECHO_REPLY);
        assert_eq!(echo_reply.body_u32(), Some(42));
        assert_eq!(chan.recv().expect("reply").id(), protocol::MSG_UPTIME_REPLY);
    }

    /// A client that sends garbage must still get an answer, or it hangs
    /// forever waiting for a reply that is never coming.
    #[test]
    fn garbage_still_gets_a_reply() {
        let mut t = Translator::new("hurd-mig-example");
        // local_port = 1: the client asked for replies on port 1.
        let alien = Message::new(9, 1, 0x1234_0001, alloc::vec![]).expect("valid message");
        let (reply, outcome) = t.dispatch(&alien);
        assert!(matches!(outcome, Outcome::Rejected(_)));
        assert_eq!(reply.id(), protocol::MSG_ERROR_REPLY);
        assert_eq!(
            reply.remote_port(),
            1,
            "even an error reply must go back to the client's port"
        );
    }

    /// A message that does not survive validation never reaches the dispatcher.
    #[test]
    fn invalid_input_never_reaches_the_dispatcher() {
        let mut chan = Loopback::new();
        assert!(chan.push_raw(&[0u8; 8]).is_err());
        let mut t = Translator::new("t");
        assert_eq!(t.serve_all(&mut chan, 4), Outcome::Idle);
        assert_eq!(t.served(), 0, "nothing may have been served");
        assert_eq!(chan.pending_in(), 0, "rejected bytes must not be queued");
    }
}

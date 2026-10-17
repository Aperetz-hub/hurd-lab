/*
 * The translator loop: a MIG-shaped request/reply dispatcher.
 *
 * This is the part that would run for the lifetime of the component on Hurd.
 * It is deliberately free of Mach specifics so it can be exercised on the host:
 * it takes a [`Transport`] and does nothing else.
 *
 * The dispatcher is total: every input produces either a reply or an error
 * reply, and it never panics on client data. That is the property that makes a
 * server component reviewable -- there is no path where a malformed message
 * silently skips the reply and wedges the client.
 */

use alloc::string::String;
use alloc::vec::Vec;

use crate::message::{Message, MessageError};
use crate::protocol;
use crate::transport::{Transport, TransportError};

/// Result of one dispatch, kept separate from the message so the loop can log
/// what happened without re-decoding the reply.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// A normal reply was produced.
    Replied,
    /// The request was malformed; the reply carries the description length as a
    /// Mach error code, since a translator must answer something.
    Rejected(MessageError),
    /// Nothing was dispatched: the transport had nothing to give.
    Idle,
    /// The transport itself failed.
    Failed(TransportError),
}

/// The component: name, identity, and the state a translator accumulates.
#[derive(Debug)]
pub struct Translator {
    /// Name reported to clients, as a real translator would publish it.
    name: String,
    /// Monotonic counter standing in for "time served" in this build.
    ///
    /// A real translator answers `UPTIME` from the kernel clock; using a request
    /// counter keeps the host build free of clock dependencies while preserving
    /// the "monotonic, never decreasing" property clients rely on.
    served: u64,
}

impl Translator {
    /// A translator with an empty history.
    pub fn new(name: &str) -> Self {
        Self {
            name: String::from(name),
            served: 0,
        }
    }

    /// The component's name.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Number of requests successfully served.
    pub fn served(&self) -> u64 {
        self.served
    }

    /// Build the reply for one request.
    ///
    /// Exposed separately from [`serve_once`] so a caller can dispatch a message
    /// it already holds without going through a transport.
    pub fn dispatch(&mut self, request: &Message) -> (Message, Outcome) {
        // Mach reply-port convention: the reply's remote port is the request's
        // *local* port. When a client sets `msgh_local_port` to 0 -- which it
        // normally does -- Mach substitutes the implicit reply port, i.e. the
        // port the request arrived from. Copying the value verbatim is
        // therefore correct in both cases: 0 in, 0 out.
        let reply_port = request.header().local_port;

        if !protocol::is_ours(request.id()) {
            return (
                protocol::error_reply(reply_port, 0, KERN_INVALID_ARGUMENT),
                Outcome::Rejected(MessageError::UnknownSubsystem { id: request.id() }),
            );
        }
        if protocol::is_reply(request.id()) {
            // A translator never sends itself a reply; treat it as a bad request
            // rather than answering, which would risk an infinite exchange.
            return (
                protocol::error_reply(reply_port, 0, KERN_INVALID_ARGUMENT),
                Outcome::Rejected(MessageError::UnknownMessage { id: request.id() }),
            );
        }

        let outcome = match request.id() {
            protocol::MSG_PING => {
                let reply = Message::new(reply_port, 0, protocol::MSG_PONG, Vec::new())
                    .expect("PONG carries no body");
                return (reply, Outcome::Replied);
            }
            protocol::MSG_ECHO => match request.body_u32() {
                Some(value) => {
                    let reply = Message::with_u32(reply_port, 0, protocol::MSG_ECHO_REPLY, value)
                        .expect("a 4-byte body cannot overflow");
                    return (reply, Outcome::Replied);
                }
                None => Outcome::Rejected(MessageError::SizeMismatch {
                    declared: request.header().size,
                    available: request.body().len(),
                }),
            },
            protocol::MSG_UPTIME => {
                let reply =
                    Message::with_u64(reply_port, 0, protocol::MSG_UPTIME_REPLY, self.served)
                        .expect("an 8-byte body cannot overflow");
                return (reply, Outcome::Replied);
            }
            other => Outcome::Rejected(MessageError::UnknownMessage { id: other }),
        };

        // Only reached by the rejection paths above.
        let reply = protocol::error_reply(reply_port, 0, KERN_INVALID_ARGUMENT);
        (reply, outcome)
    }

    /// Take one request from `transport`, dispatch it, and queue the reply.
    ///
    /// Returns what happened so the caller can decide whether to keep looping.
    pub fn serve_once<T: Transport>(&mut self, transport: &mut T) -> Outcome {
        let request = match transport.recv() {
            Ok(msg) => msg,
            Err(TransportError::WouldBlock) => return Outcome::Idle,
            Err(other) => return Outcome::Failed(other),
        };

        let (reply, outcome) = self.dispatch(&request);
        if matches!(outcome, Outcome::Replied) {
            self.served = self.served.saturating_add(1);
        }

        // A failed reply is still worth reporting: the client is waiting, and a
        // silent drop is exactly the failure mode we are trying to avoid.
        if let Err(err) = transport.send(reply) {
            return Outcome::Failed(err);
        }
        outcome
    }

    /// Drain the transport until it is empty or something goes wrong.
    ///
    /// Bounded by the number of queued messages on purpose: a real main loop
    /// never returns, but this one must terminate so it can be a test.
    pub fn serve_all<T: Transport>(&mut self, transport: &mut T, max_iterations: usize) -> Outcome {
        let mut last = Outcome::Idle;
        for _ in 0..max_iterations {
            match self.serve_once(transport) {
                Outcome::Idle => return last,
                other => last = other,
            }
        }
        last
    }
}

/// Mach error used for "this request makes no sense".
///
/// Spelled out here rather than imported so the crate builds with the `runtime`
/// feature off; the value is `KERN_INVALID_ARGUMENT` from
/// `<mach/kern_return.h>`, and both refusal paths use it so a client cannot
/// distinguish "not your subsystem" from "unknown message" by the error code.
const KERN_INVALID_ARGUMENT: i32 = 4;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transport::Loopback;

    #[test]
    fn ping_is_answered_with_pong() {
        let mut t = Translator::new("hurd-mig-example");
        // (remote_port=11, local_port=22): the client asks for replies on 22.
        let (reply, outcome) = t.dispatch(&protocol::Request::ping(11, 22));
        assert_eq!(outcome, Outcome::Replied);
        assert_eq!(reply.id(), protocol::MSG_PONG);
        assert_eq!(
            reply.remote_port(),
            22,
            "the reply must go to msgh_local_port"
        );
    }

    #[test]
    fn a_zero_local_port_keeps_the_implicit_reply_port() {
        // Clients normally send local_port = 0 and let Mach route the reply.
        // Copying the field verbatim must preserve that, not invent a port.
        let mut t = Translator::new("hurd-mig-example");
        let (reply, outcome) = t.dispatch(&protocol::Request::ping(11, 0));
        assert_eq!(outcome, Outcome::Replied);
        assert_eq!(reply.remote_port(), 0);
    }

    #[test]
    fn echo_returns_the_value_unchanged() {
        let mut t = Translator::new("hurd-mig-example");
        let (reply, outcome) = t.dispatch(&protocol::Request::echo(3, 0, 0x1234_5678));
        assert_eq!(outcome, Outcome::Replied);
        assert_eq!(reply.id(), protocol::MSG_ECHO_REPLY);
        assert_eq!(reply.body_u32(), Some(0x1234_5678));
    }

    #[test]
    fn uptime_is_monotonic_and_counts_served_requests() {
        let mut t = Translator::new("hurd-mig-example");
        let (first, _) = t.dispatch(&protocol::Request::uptime(1, 0));
        assert_eq!(first.body_u64(), Some(0));

        let mut chan = Loopback::new();
        chan.push_raw(protocol::Request::ping(1, 0).to_bytes().as_slice())
            .expect("well formed");
        assert_eq!(t.serve_once(&mut chan), Outcome::Replied);

        let (second, _) = t.dispatch(&protocol::Request::uptime(1, 0));
        assert_eq!(second.body_u64(), Some(1));
        assert!(
            second.body_u64() >= first.body_u64(),
            "uptime must never go back"
        );
    }

    #[test]
    fn a_foreign_subsystem_is_refused_not_answered() {
        let mut t = Translator::new("hurd-mig-example");
        let alien = Message::new(4, 0, 0x1234_0001, Vec::new()).unwrap();
        let (reply, outcome) = t.dispatch(&alien);
        assert!(matches!(outcome, Outcome::Rejected(_)));
        assert_eq!(reply.id(), protocol::MSG_ERROR_REPLY);
    }

    #[test]
    fn an_unknown_message_in_our_subsystem_is_refused() {
        let mut t = Translator::new("hurd-mig-example");
        let alien = Message::new(4, 0, protocol::SUBSYSTEM | 0x007f, Vec::new()).unwrap();
        let (_, outcome) = t.dispatch(&alien);
        assert_eq!(
            outcome,
            Outcome::Rejected(MessageError::UnknownMessage {
                id: protocol::SUBSYSTEM | 0x007f
            })
        );
    }

    #[test]
    fn a_reply_sent_to_us_is_never_answered() {
        // Otherwise two peers would trade replies forever.
        let mut t = Translator::new("hurd-mig-example");
        let (reply, outcome) =
            t.dispatch(&Message::new(4, 0, protocol::MSG_PONG, Vec::new()).unwrap());
        assert!(matches!(outcome, Outcome::Rejected(_)));
        assert_eq!(reply.id(), protocol::MSG_ERROR_REPLY);
    }

    #[test]
    fn a_wrong_width_echo_body_is_refused() {
        let mut t = Translator::new("hurd-mig-example");
        // Right id, wrong body width.
        let bad = Message::new(4, 0, protocol::MSG_ECHO, alloc::vec![1, 2, 3]).unwrap();
        let (reply, outcome) = t.dispatch(&bad);
        assert!(matches!(outcome, Outcome::Rejected(_)));
        assert_eq!(reply.id(), protocol::MSG_ERROR_REPLY);
    }

    #[test]
    fn serve_all_drains_the_queue_and_counts() {
        let mut t = Translator::new("hurd-mig-example");
        let mut chan = Loopback::new();
        for req in [
            protocol::Request::ping(1, 0),
            protocol::Request::echo(1, 0, 7),
            protocol::Request::uptime(1, 0),
        ] {
            chan.push_raw(req.to_bytes().as_slice())
                .expect("well formed");
        }
        let last = t.serve_all(&mut chan, 16);
        assert_eq!(last, Outcome::Replied);
        assert_eq!(t.served(), 3);
        // The three replies went out, not back into our own inbox: a server loop
        // that ate its own replies would keep running.
        assert_eq!(chan.pending_out(), 3);
        assert_eq!(chan.pending_in(), 0);
    }

    #[test]
    fn serving_an_empty_channel_is_idle_not_an_error() {
        let mut t = Translator::new("hurd-mig-example");
        let mut chan = Loopback::new();
        assert_eq!(t.serve_all(&mut chan, 4), Outcome::Idle);
        assert_eq!(t.served(), 0);
    }
}

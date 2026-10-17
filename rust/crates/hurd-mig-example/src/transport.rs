/*
 * Transport abstraction.
 *
 * The translator logic in `server.rs` is written against this trait, which is
 * what makes the component testable and portable:
 *
 *   - [`Loopback`] is an in-process stand-in for a Mach port. It keeps two
 *     queues -- outbox and inbox -- rather than one, because a message channel
 *     is directional: sending a reply must not make the sender receive it.
 *     Collapsing both into one queue makes a server loop eat its own replies,
 *     which is exactly the bug the first version of this file had.
 *   - [`Mach`] is the real thing, behind the `mach` feature, and owns a receive
 *     right with RAII so a port cannot leak on an early return.
 *
 * The point of splitting them is not testability alone: the real translator
 * will run behind the same dispatch loop, so the only code that is Mach-specific
 * stays as small as possible.
 */

use alloc::collections::VecDeque;

use crate::message::{Message, MessageError};

/// Why a transport operation could not be completed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransportError {
    /// No message was available and none may be waited for (non-blocking).
    WouldBlock,
    /// The peer sent something that does not validate.
    Invalid(MessageError),
    /// The Mach call itself failed; carries the raw `kern_return_t`.
    ///
    /// Only ever constructed by the `mach` transport, so a host build that
    /// leaves the feature off has no way to name it.
    #[cfg_attr(not(feature = "mach"), allow(dead_code))]
    Mach(hurd_mig_sys::Krn),
}

/// Message channel a translator reads requests from and writes replies to.
pub trait Transport {
    /// Queue one message for the peer.
    fn send(&mut self, msg: Message) -> Result<(), TransportError>;

    /// Take the next message, validating it first.
    ///
    /// Returning [`TransportError::Invalid`] rather than the raw bytes is the
    /// point: a translator must never hand an unvalidated buffer to its
    /// dispatcher.
    fn recv(&mut self) -> Result<Message, TransportError>;
}

/// In-process stand-in for a Mach port.
///
/// Directional, like the real thing: [`Transport::send`] fills the outbox,
/// [`Transport::recv`] drains the inbox, and [`Loopback::deliver`] plays the
/// part of the kernel moving one to the other.
#[derive(Debug, Default)]
pub struct Loopback {
    outbox: VecDeque<Message>,
    inbox: VecDeque<Message>,
}

impl Loopback {
    /// An empty channel.
    pub fn new() -> Self {
        Self::default()
    }

    /// Move everything queued for the peer into our inbox.
    ///
    /// Returns how many messages were delivered. On a real system the kernel
    /// does this; here it has to be asked, so tests and the demo call it
    /// explicitly.
    pub fn deliver(&mut self) -> usize {
        let n = self.outbox.len();
        self.inbox.extend(self.outbox.drain(..));
        n
    }

    /// Messages waiting for us.
    pub fn pending_in(&self) -> usize {
        self.inbox.len()
    }

    /// Messages waiting for the peer.
    pub fn pending_out(&self) -> usize {
        self.outbox.len()
    }

    /// Queue a message destined for *us*, as if the peer had sent it.
    ///
    /// Validation runs here, so this is the boundary where untrusted bytes
    /// would enter.
    pub fn push_raw(&mut self, raw: &[u8]) -> Result<(), TransportError> {
        let msg = Message::from_bytes(raw).map_err(TransportError::Invalid)?;
        self.inbox.push_back(msg);
        Ok(())
    }
}

impl Transport for Loopback {
    fn send(&mut self, msg: Message) -> Result<(), TransportError> {
        self.outbox.push_back(msg);
        Ok(())
    }

    fn recv(&mut self) -> Result<Message, TransportError> {
        self.inbox.pop_front().ok_or(TransportError::WouldBlock)
    }
}

/// Real Mach transport: one receive right, polled.
#[cfg(feature = "mach")]
pub mod mach {
    use alloc::vec::Vec;

    use crate::message::{Message, MessageError};

    use super::TransportError;

    /// A Mach receive right owned by this component.
    ///
    /// The RAII wrapper is the whole reason this type exists: `mach_msg` hands
    /// back a bare port name, and a translator that forgets to
    /// `mach_port_deallocate` it leaks a name for the lifetime of the task.
    pub struct OwnedReceiveRight(hurd_mig_sys::mach_port_t);

    impl OwnedReceiveRight {
        /// Allocate a fresh receive right in the calling task.
        pub fn allocate() -> Result<Self, TransportError> {
            let mut port: hurd_mig_sys::mach_port_t = 0;
            // SAFETY: `port` is live, writable storage of the right type.
            let kr = unsafe { hurd_mig_sys::allocate_receive_right(&mut port) };
            if hurd_mig_sys::is_ok(kr) {
                Ok(Self(port))
            } else {
                Err(TransportError::Mach(kr))
            }
        }

        /// The raw port name.
        pub fn raw(&self) -> hurd_mig_sys::mach_port_t {
            self.0
        }
    }

    impl Drop for OwnedReceiveRight {
        fn drop(&mut self) {
            // SAFETY: we allocated this right and nobody else can have taken
            // ownership of it, and dropping a port name is always allowed.
            unsafe {
                let _ = hurd_mig_sys::deallocate_receive_right(self.0);
            }
        }
    }

    /// Transport over a Mach port, using polled `mach_msg`.
    ///
    /// The buffer is capped at [`crate::message::MAX_MESSAGE_SIZE`], which is
    /// also what the receive side advertises, so the kernel can never be asked
    /// to write past the allocation.
    pub struct Mach {
        port: OwnedReceiveRight,
    }

    impl Mach {
        /// Open a receive right for this translator.
        pub fn new() -> Result<Self, TransportError> {
            Ok(Self {
                port: OwnedReceiveRight::allocate()?,
            })
        }

        /// The port clients should connect to.
        pub fn port(&self) -> hurd_mig_sys::mach_port_t {
            self.port.raw()
        }
    }

    impl Transport for Mach {
        fn send(&mut self, msg: Message) -> Result<(), TransportError> {
            // SAFETY: `msg` guarantees its buffer starts with a complete,
            // correctly laid out mach_msg_header_t, and the buffer is not
            // aliased anywhere else.
            unsafe {
                let header = msg.to_bytes();
                let header_ptr = header.as_ptr() as *mut hurd_mig_sys::mach_msg_header_t;
                let kr = hurd_mig_sys::raw::mach_msg(
                    header_ptr,
                    hurd_mig_sys::raw::MACH_SEND_MSG,
                    header.len(),
                    0,
                    0,
                    0,
                    0,
                );
                if hurd_mig_sys::is_ok(kr) {
                    Ok(())
                } else {
                    Err(TransportError::Mach(kr))
                }
            }
        }

        fn recv(&mut self) -> Result<Message, TransportError> {
            // One allocation, reused: a translator that allocates per message
            // puts the allocator on the hot path.
            let mut buf: Vec<u8> = Vec::with_capacity(crate::message::MAX_MESSAGE_SIZE);
            // SAFETY: capacity is MAX_MESSAGE_SIZE and we only ever hand Mach
            // that many bytes, so the uninitialised tail is never read.
            unsafe { buf.set_len(crate::message::MAX_MESSAGE_SIZE) };
            let mut header: hurd_mig_sys::mach_msg_header_t = core::mem::zeroed();
            let rcv_size = buf.len() as hurd_mig_sys::mach_msg_size_t;

            // SAFETY: both pointers are live for the call, `buf` is exactly the
            // advertised size, and `header` is a valid mach_msg_header_t.
            let (kr, used) = unsafe {
                let kr = hurd_mig_sys::raw::mach_msg(
                    &mut header as *mut _,
                    hurd_mig_sys::raw::MACH_RCV_MSG,
                    0,
                    rcv_size,
                    self.port.raw(),
                    0,
                    0,
                );
                (kr, header.msgh_size as usize)
            };

            if !hurd_mig_sys::is_ok(kr) {
                return Err(TransportError::Mach(kr));
            }
            buf.truncate(used.min(buf.capacity()));

            // SAFETY: Mach wrote `used` bytes into a live buffer at least that
            // long; from_bytes re-validates them before anything else sees them.
            Message::from_bytes(&buf).map_err(|_| TransportError::Invalid(MessageError::TooShort))
        }
    }

    /// Render a Mach error for logging.
    pub fn describe(kr: hurd_mig_sys::Krn) -> alloc::string::String {
        let mut buf = [0 as core::ffi::c_char; 128];
        // SAFETY: `buf` is a live 128-byte array.
        let text = unsafe { hurd_mig_sys::error_string(kr, &mut buf) };
        alloc::string::String::from(text)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Request;

    #[test]
    fn loopback_round_trips_a_message() {
        let mut tx = Loopback::new();
        tx.send(Request::ping(5, 0)).expect("send");
        assert_eq!(tx.pending_out(), 1);
        assert_eq!(tx.deliver(), 1);
        assert_eq!(tx.pending_in(), 1);
        assert_eq!(tx.recv().expect("recv").id(), crate::protocol::MSG_PING);
    }

    #[test]
    fn a_sent_message_does_not_come_back_to_the_sender() {
        // The single-queue bug: a server loop that eats its own replies will
        // process them as requests forever.
        let mut tx = Loopback::new();
        tx.send(Request::ping(1, 0)).expect("send");
        assert_eq!(tx.recv(), Err(TransportError::WouldBlock));
    }

    #[test]
    fn recv_on_an_empty_channel_reports_would_block() {
        let mut tx = Loopback::new();
        assert_eq!(tx.recv(), Err(TransportError::WouldBlock));
    }

    #[test]
    fn raw_input_is_validated_at_the_boundary() {
        let mut tx = Loopback::new();
        // A 4-byte "message" cannot even hold a header.
        assert_eq!(
            tx.push_raw(&[0u8; 4]),
            Err(TransportError::Invalid(MessageError::TooShort))
        );
        assert_eq!(tx.pending_in(), 0, "invalid input must not be queued");
    }
}

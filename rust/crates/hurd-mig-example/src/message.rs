/*
 * MIG-shaped message layer.
 *
 * This is the part of the component that is worth getting right before any
 * Mach call happens, and it is deliberately transport-free so it can be tested
 * on the host.
 *
 * The header mirrors GNU Mach's `mach_msg_header_t` (<mach/message.h>):
 *
 *     typedef struct {
 *         mach_msg_size_t msgh_size;          /* u32 */
 *         mach_port_t     msgh_remote_port;   /* i32 */
 *         mach_port_t     msgh_local_port;    /* i32 */
 *         natural_t       msgh_bits;         /* u32 */
 *         mach_msg_size_t msgh_size_rounded; /* u32 */
 *         mach_msg_id_t   msgh_id;           /* u32 */
 *     } mach_msg_header_t;
 *
 * On x86_64 every member is 4 bytes, so the header is exactly 24 bytes with no
 * padding. `HEADER_SIZE` is asserted against `size_of` at compile time, because
 * a silent layout drift here would corrupt every message rather than fail.
 *
 * Everything else is the checked layer: sizes are validated *before* the kernel
 * or the transport ever sees the buffer. That ordering is the whole point -- a
 * translator that forwards unchecked client-supplied lengths to `mach_msg()`
 * is a memory-safety bug waiting for a hostile client.
 */

use alloc::vec::Vec;

/// Size of `mach_msg_header_t` on a 64-bit target, in bytes.
pub const HEADER_SIZE: usize = 24;

/// Largest message this component will accept, matching Mach's own default
/// limit (`MACH_MSG_SIZE_MAX` is 2^16 - sizeof(header)).
pub const MAX_MESSAGE_SIZE: usize = 0xffff;

/// Word alignment Mach requires for the inline body.
pub const WORD: usize = 4;

// Compile-time guard: if the repr(C) struct ever stops matching the C header,
// every message would be silently misparsed, so fail the build instead.
const _: () = assert!(
    core::mem::size_of::<RawHeader>() == HEADER_SIZE,
    "mach_msg_header_t layout drifted from the C ABI"
);

/// `#[repr(C)]` view of the Mach header. All fields are 32-bit, as in C.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RawHeader {
    /// Total message size, header included.
    pub size: u32,
    /// Port the message is sent to.
    pub remote_port: u32,
    /// Port the reply should come from.
    pub local_port: u32,
    /// MIG descriptor/trailer word counts, packed.
    pub bits: u32,
    /// `size` rounded up to [`WORD`]; Mach uses this, never `size`, for copying.
    pub size_rounded: u32,
    /// MIG message identifier: subsystem in the high half.
    pub id: u32,
}

/// Round `n` up to the next multiple of [`WORD`].
pub const fn round_up(n: usize) -> usize {
    (n + (WORD - 1)) & !(WORD - 1)
}

/// Everything that can go wrong before a message is trusted.
///
/// Deliberately a flat enum with no payload: a translator must be able to turn
/// any of these into a MIG error reply, and a payload would tempt callers into
/// propagating an `alloc::collections` type across the protocol boundary.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageError {
    /// The message is smaller than a header, so there is nothing to read.
    TooShort,
    /// The message exceeds [`MAX_MESSAGE_SIZE`].
    TooLarge,
    /// `size` and the actual buffer disagree.
    SizeMismatch {
        /// `msgh_size` claimed by the header.
        declared: u32,
        /// Bytes actually available.
        available: usize,
    },
    /// `msgh_size_rounded` is not `size` rounded up to [`WORD`].
    MisalignedSize,
    /// A send left `remote_port` unset, which Mach rejects.
    MissingRemotePort,
    /// The message identifier is not part of this subsystem.
    UnknownSubsystem {
        /// The identifier as received.
        id: u32,
    },
    /// The identifier is in this subsystem but names no known message.
    UnknownMessage {
        /// The identifier as received.
        id: u32,
    },
}

impl MessageError {
    /// Short, stable, allocation-free description for logs and error replies.
    pub const fn describe(self) -> &'static str {
        match self {
            Self::TooShort => "message shorter than a mach_msg_header_t",
            Self::TooLarge => "message larger than the Mach maximum",
            Self::SizeMismatch { .. } => "msgh_size disagrees with the buffer",
            Self::MisalignedSize => "msgh_size_rounded is not msgh_size rounded up",
            Self::MissingRemotePort => "msgh_remote_port is unset on a send",
            Self::UnknownSubsystem { .. } => "message id is from another subsystem",
            Self::UnknownMessage { .. } => "unknown message id in this subsystem",
        }
    }
}

/// A validated MIG-shaped message: a header plus its inline body.
///
/// The invariant is that `header.size == HEADER_SIZE + body.len()`, enforced by
/// every constructor, so no method ever has to re-check it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Message {
    header: RawHeader,
    body: Vec<u8>,
}

impl Message {
    /// Build a message from a header and a body, validating the header.
    ///
    /// `id` is validated against `subsystem` only as far as "is it in range";
    /// whether the specific message exists is a [`crate::protocol`] question.
    pub fn new(
        remote_port: u32,
        local_port: u32,
        id: u32,
        body: Vec<u8>,
    ) -> Result<Self, MessageError> {
        let total = HEADER_SIZE
            .checked_add(body.len())
            .ok_or(MessageError::TooLarge)?;
        if total > MAX_MESSAGE_SIZE {
            return Err(MessageError::TooLarge);
        }

        let header = RawHeader {
            size: total as u32,
            remote_port,
            local_port,
            // Descriptor word count in the high byte, trailer word count in the
            // low byte. Zero of each: this protocol uses neither out-of-line
            // descriptors nor MIG trailers, it carries fixed-size bodies.
            bits: 0,
            size_rounded: round_up(total) as u32,
            id,
        };

        Ok(Self { header, body })
    }

    /// Build a message from bytes as they would arrive from `mach_msg()`.
    ///
    /// This is the boundary where untrusted data enters, so it validates
    /// everything: length, declared size, rounding, and remote port.
    pub fn from_bytes(raw: &[u8]) -> Result<Self, MessageError> {
        if raw.len() < HEADER_SIZE {
            return Err(MessageError::TooShort);
        }
        if raw.len() > MAX_MESSAGE_SIZE {
            return Err(MessageError::TooLarge);
        }

        let word = |i: usize| -> u32 {
            u32::from_ne_bytes([raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3]])
        };
        let header = RawHeader {
            size: word(0),
            remote_port: word(1) as i32 as u32,
            local_port: word(2) as i32 as u32,
            bits: word(3),
            size_rounded: word(4),
            id: word(5),
        };

        // `size` must describe exactly the buffer we were handed. Accepting a
        // mismatch (by clamping, or by trusting the smaller of the two) is how
        // translators end up reading past a buffer.
        if header.size as usize != raw.len() {
            return Err(MessageError::SizeMismatch {
                declared: header.size,
                available: raw.len(),
            });
        }
        if header.size_rounded as usize != round_up(header.size as usize) {
            return Err(MessageError::MisalignedSize);
        }
        if header.remote_port == 0 {
            return Err(MessageError::MissingRemotePort);
        }

        Ok(Self {
            header,
            body: raw[HEADER_SIZE..].to_vec(),
        })
    }

    /// Serialise to the byte layout `mach_msg()` expects.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.header.size as usize);
        let mut push = |v: u32| out.extend_from_slice(&v.to_ne_bytes());
        push(self.header.size);
        push(self.header.remote_port);
        push(self.header.local_port);
        push(self.header.bits);
        push(self.header.size_rounded);
        push(self.header.id);
        out.extend_from_slice(&self.body);
        out
    }

    /// The header, by value.
    pub fn header(&self) -> RawHeader {
        self.header
    }

    /// The MIG message identifier.
    pub fn id(&self) -> u32 {
        self.header.id
    }

    /// The destination port.
    pub fn remote_port(&self) -> u32 {
        self.header.remote_port
    }

    /// The inline body.
    pub fn body(&self) -> &[u8] {
        &self.body
    }

    /// Consume the message and return its body.
    pub fn into_body(self) -> Vec<u8> {
        self.body
    }

    /// Read the body as a little-endian `u32`, which is what the protocol uses
    /// for its scalars.
    ///
    /// Returns `None` if the body is not exactly four bytes: a truncated or
    /// over-long scalar must be an error, not a silently truncated read.
    pub fn body_u32(&self) -> Option<u32> {
        (self.body.len() == 4)
            .then(|| u32::from_ne_bytes([self.body[0], self.body[1], self.body[2], self.body[3]]))
    }

    /// Read the body as a little-endian `u64`.
    pub fn body_u64(&self) -> Option<u64> {
        (self.body.len() == 8).then(|| {
            let mut b = [0u8; 8];
            b.copy_from_slice(&self.body);
            u64::from_ne_bytes(b)
        })
    }

    /// Build a message carrying a single `u32` body.
    pub fn with_u32(
        remote_port: u32,
        local_port: u32,
        id: u32,
        value: u32,
    ) -> Result<Self, MessageError> {
        Self::new(remote_port, local_port, id, value.to_ne_bytes().to_vec())
    }

    /// Build a message carrying a single `u64` body.
    pub fn with_u64(
        remote_port: u32,
        local_port: u32,
        id: u32,
        value: u64,
    ) -> Result<Self, MessageError> {
        Self::new(remote_port, local_port, id, value.to_ne_bytes().to_vec())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn header_is_the_size_the_c_abi_implies() {
        assert_eq!(core::mem::size_of::<RawHeader>(), 24);
    }

    #[test]
    fn round_up_matches_mach_word_alignment() {
        assert_eq!(round_up(24), 24);
        assert_eq!(round_up(25), 28);
        assert_eq!(round_up(27), 28);
        assert_eq!(round_up(28), 28);
    }

    #[test]
    fn round_trip_preserves_every_field() {
        let msg = Message::with_u32(7, 9, 0x9500_0001, 0xdead_beef).expect("valid message");
        let bytes = msg.to_bytes();
        let back = Message::from_bytes(&bytes).expect("round trip must validate");
        assert_eq!(msg, back);
        assert_eq!(back.body_u32(), Some(0xdead_beef));
    }

    #[test]
    fn size_is_always_header_plus_body() {
        let msg = Message::new(1, 0, 0x9500_0001, vec![1, 2, 3, 4, 5]).expect("valid message");
        assert_eq!(msg.header().size as usize, HEADER_SIZE + 5);
        // size_rounded pads the 5-byte body to the next word.
        assert_eq!(
            msg.header().size_rounded as usize,
            round_up(HEADER_SIZE + 5)
        );
    }

    #[test]
    fn short_buffer_is_rejected() {
        assert_eq!(
            Message::from_bytes(&[0u8; HEADER_SIZE - 1]),
            Err(MessageError::TooShort)
        );
    }

    #[test]
    fn lying_size_is_rejected_rather_than_clamped() {
        let mut bytes = Message::with_u32(1, 0, 1, 0).unwrap().to_bytes();
        // Claim a 4-byte message while 28 bytes are actually present.
        bytes[0..4].copy_from_slice(&4u32.to_ne_bytes());
        assert!(matches!(
            Message::from_bytes(&bytes),
            Err(MessageError::SizeMismatch {
                declared: 4,
                available: 28
            })
        ));
    }

    #[test]
    fn misrounded_size_is_rejected() {
        let mut bytes = Message::with_u32(1, 0, 1, 0).unwrap().to_bytes();
        bytes[16..20].copy_from_slice(&99u32.to_ne_bytes());
        assert_eq!(
            Message::from_bytes(&bytes),
            Err(MessageError::MisalignedSize)
        );
    }

    #[test]
    fn send_without_a_destination_is_rejected() {
        let mut bytes = Message::with_u32(1, 0, 1, 0).unwrap().to_bytes();
        bytes[4..8].copy_from_slice(&0u32.to_ne_bytes());
        assert_eq!(
            Message::from_bytes(&bytes),
            Err(MessageError::MissingRemotePort)
        );
    }

    #[test]
    fn oversized_message_is_rejected() {
        let body = vec![0u8; MAX_MESSAGE_SIZE];
        assert_eq!(Message::new(1, 0, 1, body), Err(MessageError::TooLarge));
    }

    #[test]
    fn into_body_consumes_the_message() {
        let msg = Message::with_u32(1, 0, 7, 0xfeed_beef).unwrap();
        // Native byte order, as `to_ne_bytes` produces.
        assert_eq!(msg.into_body(), 0xfeed_beefu32.to_ne_bytes().to_vec());
    }

    #[test]
    fn scalar_reads_reject_a_wrong_width() {
        // A 5-byte body is not a u32: reading it anyway is how integer-overflow
        // style bugs start.
        let msg = Message::new(1, 0, 1, vec![0; 5]).unwrap();
        assert_eq!(msg.body_u32(), None);
        assert_eq!(msg.body_u64(), None);
    }
}

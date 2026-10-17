/*
 * The wire protocol: MIG-shaped message identifiers and request/reply bodies.
 *
 * Real MIG identifiers pack a subsystem in the high 16 bits and a message
 * number in the low 16, with the reply numbers offset from the request numbers.
 * This protocol follows that convention closely enough to be instructive while
 * staying small:
 *
 *     msgh_id = 0x9500_0000 | (kind << 8) | detail
 *     ^^^^^^^^ subsystem        ^^^^^^^^^^^^ message
 *
 * Replies deliberately reuse the *same* subsystem but a distinct kind
 * (`KIND_REPLY`), so a dispatcher can never confuse a reply for a request.
 */

use alloc::vec::Vec;

use crate::message::Message;

/// Subsystem base of this component's MIG interface.
///
/// 0x9500 is in the range MIG reserves for out-of-band / user-defined
/// subsystems, which keeps it clear of the kernel's own.
pub const SUBSYSTEM: u32 = 0x9500_0000;

/// Mask for the subsystem half of an identifier.
pub const SUBSYSTEM_MASK: u32 = 0xffff_0000;

/// Requests.
pub const KIND_REQUEST: u32 = 0x00;
/// Replies.
pub const KIND_REPLY: u32 = 0x01;

/// `PING`: no body. Answered with `PONG`.
pub const MSG_PING: u32 = SUBSYSTEM | (KIND_REQUEST << 8) | 0x01;
/// `PONG`: no body.
pub const MSG_PONG: u32 = SUBSYSTEM | (KIND_REPLY << 8) | 0x01;

/// `ECHO`: one `u32` body, returned unchanged.
pub const MSG_ECHO: u32 = SUBSYSTEM | (KIND_REQUEST << 8) | 0x02;
/// `ECHO` reply: one `u32` body.
pub const MSG_ECHO_REPLY: u32 = SUBSYSTEM | (KIND_REPLY << 8) | 0x02;

/// `UPTIME`: no body. Answered with the translator's monotonic request count.
pub const MSG_UPTIME: u32 = SUBSYSTEM | (KIND_REQUEST << 8) | 0x03;
/// `UPTIME` reply: one `u64` body.
pub const MSG_UPTIME_REPLY: u32 = SUBSYSTEM | (KIND_REPLY << 8) | 0x03;

/// Generic error reply: one `i32` body holding a `kern_return_t`.
///
/// The trailing `0x00` is the detail field. It is spelled out even though it
/// adds nothing, because the whole table is meant to be read as
/// `subsystem | kind | detail` and dropping it here would make this one line
/// look like it belongs to a different scheme.
#[allow(clippy::identity_op)]
pub const MSG_ERROR_REPLY: u32 = SUBSYSTEM | (KIND_REPLY << 8) | 0x00;

/// `true` when `id` belongs to this component.
pub const fn is_ours(id: u32) -> bool {
    id & SUBSYSTEM_MASK == SUBSYSTEM
}

/// `true` when `id` is a reply rather than a request.
pub const fn is_reply(id: u32) -> bool {
    is_ours(id) && (id >> 8) & 0xff == KIND_REPLY
}

/// The [`Message`] constructors for this protocol, kept next to the ids so the
/// two cannot drift apart.
pub struct Request;

impl Request {
    /// `PING`, with no body.
    pub fn ping(remote_port: u32, local_port: u32) -> Message {
        Message::new(remote_port, local_port, MSG_PING, Vec::new())
            .expect("a PING carries no body and cannot overflow")
    }

    /// `ECHO` carrying `value`.
    pub fn echo(remote_port: u32, local_port: u32, value: u32) -> Message {
        Message::with_u32(remote_port, local_port, MSG_ECHO, value)
            .expect("a 4-byte body cannot overflow the message limit")
    }

    /// `UPTIME`, with no body.
    pub fn uptime(remote_port: u32, local_port: u32) -> Message {
        Message::new(remote_port, local_port, MSG_UPTIME, Vec::new())
            .expect("an UPTIME carries no body and cannot overflow")
    }
}

/// Build the reply for an error condition.
pub fn error_reply(remote_port: u32, local_port: u32, kern_return: i32) -> Message {
    Message::with_u32(remote_port, local_port, MSG_ERROR_REPLY, kern_return as u32)
        .expect("a 4-byte body cannot overflow the message limit")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_id_is_in_our_subsystem() {
        for id in [
            MSG_PING,
            MSG_PONG,
            MSG_ECHO,
            MSG_ECHO_REPLY,
            MSG_UPTIME,
            MSG_UPTIME_REPLY,
            MSG_ERROR_REPLY,
        ] {
            assert!(is_ours(id), "id {id:#010x} should be ours");
        }
    }

    #[test]
    fn requests_and_replies_are_disjoint() {
        for id in [MSG_PING, MSG_ECHO, MSG_UPTIME] {
            assert!(!is_reply(id), "request {id:#010x} misclassified as a reply");
        }
        for id in [MSG_PONG, MSG_ECHO_REPLY, MSG_UPTIME_REPLY, MSG_ERROR_REPLY] {
            assert!(is_reply(id), "reply {id:#010x} misclassified as a request");
        }
    }

    #[test]
    fn a_foreign_id_is_not_ours() {
        assert!(!is_ours(0x1234_0001));
        assert!(!is_reply(0x1234_0001));
    }

    #[test]
    fn ids_are_unique() {
        let ids = [
            MSG_PING,
            MSG_PONG,
            MSG_ECHO,
            MSG_ECHO_REPLY,
            MSG_UPTIME,
            MSG_UPTIME_REPLY,
            MSG_ERROR_REPLY,
        ];
        for (i, a) in ids.iter().enumerate() {
            for b in ids.iter().skip(i + 1) {
                assert_ne!(a, b, "duplicate message id {a:#010x}");
            }
        }
    }
}

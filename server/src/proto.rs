//! The wire protocol (binary, little endian), the same over UDP and WebSocket.
//!
//! client -> server
//!   HELLO   1, match u32, player u8, token u32
//!   INPUT   2, match u32, player u8, have_remote u32, first u32, n u8, n inputs,
//!           k u8, k x (frame u32, hash u64)
//!           have_remote: how many of the opponent's inputs the client has (so the
//!           server knows what to resend); then the client's inputs from `first`
//!           (redundant: everything the server may lack), and checksums of
//!           confirmed frames
//! server -> client
//!   WELCOME 10, match u32, player u8, delay u8
//!   START   11
//!   INPUTS  12, have_yours u32, first u32, n u8, n inputs (the opponent's)
//!   RESULT  13, winner i8 (-1 draw/forfeit), desyncs u32
//!   ERROR   14, utf-8 message

pub const HELLO: u8 = 1;
pub const INPUT: u8 = 2;
pub const WELCOME: u8 = 10;
pub const START: u8 = 11;
pub const INPUTS: u8 = 12;
pub const RESULT: u8 = 13;
pub const ERROR: u8 = 14;

#[derive(Debug, Clone, PartialEq)]
pub enum ClientMsg {
    Hello { match_id: u32, player: u8, token: u32 },
    Input { match_id: u32, player: u8, have_remote: u32, first: u32, inputs: Vec<u8>, checksums: Vec<(u32, u64)> },
}

struct Reader<'a> {
    b: &'a [u8],
    p: usize,
}

impl<'a> Reader<'a> {
    fn u8(&mut self) -> Option<u8> {
        let v = *self.b.get(self.p)?;
        self.p += 1;
        Some(v)
    }
    fn u32(&mut self) -> Option<u32> {
        let s = self.b.get(self.p..self.p + 4)?;
        self.p += 4;
        Some(u32::from_le_bytes(s.try_into().ok()?))
    }
    fn u64(&mut self) -> Option<u64> {
        let s = self.b.get(self.p..self.p + 8)?;
        self.p += 8;
        Some(u64::from_le_bytes(s.try_into().ok()?))
    }
    fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        let s = self.b.get(self.p..self.p + n)?;
        self.p += n;
        Some(s)
    }
}

/// Parse a client message; None for anything malformed (it is dropped).
pub fn parse_client(b: &[u8]) -> Option<ClientMsg> {
    let mut r = Reader { b, p: 0 };
    match r.u8()? {
        HELLO => Some(ClientMsg::Hello { match_id: r.u32()?, player: r.u8()?, token: r.u32()? }),
        INPUT => {
            let match_id = r.u32()?;
            let player = r.u8()?;
            let have_remote = r.u32()?;
            let first = r.u32()?;
            let n = r.u8()? as usize;
            let inputs = r.bytes(n)?.to_vec();
            let k = r.u8()? as usize;
            let mut checksums = Vec::with_capacity(k);
            for _ in 0..k {
                checksums.push((r.u32()?, r.u64()?));
            }
            Some(ClientMsg::Input { match_id, player, have_remote, first, inputs, checksums })
        }
        _ => None,
    }
}

#[cfg(test)]
pub fn encode_client(m: &ClientMsg) -> Vec<u8> {
    let mut v = Vec::new();
    match m {
        ClientMsg::Hello { match_id, player, token } => {
            v.push(HELLO);
            v.extend_from_slice(&match_id.to_le_bytes());
            v.push(*player);
            v.extend_from_slice(&token.to_le_bytes());
        }
        ClientMsg::Input { match_id, player, have_remote, first, inputs, checksums } => {
            v.push(INPUT);
            v.extend_from_slice(&match_id.to_le_bytes());
            v.push(*player);
            v.extend_from_slice(&have_remote.to_le_bytes());
            v.extend_from_slice(&first.to_le_bytes());
            v.push(inputs.len() as u8);
            v.extend_from_slice(inputs);
            v.push(checksums.len() as u8);
            for (f, h) in checksums {
                v.extend_from_slice(&f.to_le_bytes());
                v.extend_from_slice(&h.to_le_bytes());
            }
        }
    }
    v
}

pub fn welcome(match_id: u32, player: u8, delay: u8) -> Vec<u8> {
    let mut v = vec![WELCOME];
    v.extend_from_slice(&match_id.to_le_bytes());
    v.push(player);
    v.push(delay);
    v
}

pub fn inputs(have_yours: u32, first: u32, inputs: &[u8]) -> Vec<u8> {
    let mut v = vec![INPUTS];
    v.extend_from_slice(&have_yours.to_le_bytes());
    v.extend_from_slice(&first.to_le_bytes());
    v.push(inputs.len() as u8);
    v.extend_from_slice(inputs);
    v
}

pub fn result(winner: i8, desyncs: u32) -> Vec<u8> {
    let mut v = vec![RESULT, winner as u8];
    v.extend_from_slice(&desyncs.to_le_bytes());
    v
}

pub fn error(msg: &str) -> Vec<u8> {
    let mut v = vec![ERROR];
    v.extend_from_slice(msg.as_bytes());
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_and_garbage() {
        let m = ClientMsg::Input { match_id: 7, player: 1, have_remote: 30, first: 28, inputs: vec![1, 2, 3], checksums: vec![(30, 0xdead_beef)] };
        assert_eq!(parse_client(&encode_client(&m)), Some(m));
        let h = ClientMsg::Hello { match_id: 3, player: 0, token: 99 };
        assert_eq!(parse_client(&encode_client(&h)), Some(h));
        let full = encode_client(&ClientMsg::Input { match_id: 1, player: 0, have_remote: 0, first: 0, inputs: vec![5; 10], checksums: vec![] });
        for cut in 0..full.len() {
            assert_eq!(parse_client(&full[..cut]), None, "truncated at {cut}");
        }
        assert_eq!(parse_client(&[99, 1, 2]), None);
    }
}

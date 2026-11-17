//! A minimal WebSocket server side (RFC 6455) for the browser clients:
//! the opening handshake (SHA-1 and base64 written here), binary frames,
//! ping/pong and close. Messages are the same bytes as over UDP.

use std::io::{self, Read, Write};
use std::net::TcpStream;

pub fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64) * 8;
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[4 * i..4 * i + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = h;
        for (i, &wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, v) in h.iter_mut().zip([a, b, c, d, e]) {
            *x = x.wrapping_add(v);
        }
    }
    let mut out = [0u8; 20];
    for (i, v) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
    }
    out
}

pub fn base64(data: &[u8]) -> String {
    const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for k in 0..4 {
            if k <= c.len() {
                s.push(T[(n >> (18 - 6 * k) & 63) as usize] as char);
            } else {
                s.push('=');
            }
        }
    }
    s
}

pub fn accept_key(key: &str) -> String {
    base64(&sha1(format!("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key.trim()).as_bytes()))
}

/// Read the HTTP upgrade request and answer it; Err if it is not a WebSocket request.
pub fn handshake(s: &mut TcpStream) -> io::Result<()> {
    let mut req = Vec::new();
    let mut b = [0u8; 1];
    while !req.ends_with(b"\r\n\r\n") {
        if s.read(&mut b)? == 0 || req.len() > 8192 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "bad request"));
        }
        req.push(b[0]);
    }
    let text = String::from_utf8_lossy(&req);
    let key = text
        .lines()
        .find_map(|l| l.split_once(':').filter(|(k, _)| k.trim().eq_ignore_ascii_case("sec-websocket-key")).map(|(_, v)| v.trim().to_string()))
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "no Sec-WebSocket-Key"))?;
    write!(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n", accept_key(&key))
}

pub enum Frame {
    Binary(Vec<u8>),
    Ping(Vec<u8>),
    Close,
    Other,
}

/// Read one frame (client frames are masked); fragmented messages are not used by the client.
pub fn read_frame(s: &mut TcpStream) -> io::Result<Frame> {
    let mut h = [0u8; 2];
    s.read_exact(&mut h)?;
    let opcode = h[0] & 0x0f;
    let masked = h[1] & 0x80 != 0;
    let mut len = (h[1] & 0x7f) as u64;
    if len == 126 {
        let mut e = [0u8; 2];
        s.read_exact(&mut e)?;
        len = u16::from_be_bytes(e) as u64;
    } else if len == 127 {
        let mut e = [0u8; 8];
        s.read_exact(&mut e)?;
        len = u64::from_be_bytes(e);
    }
    if len > 1 << 20 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "frame too large"));
    }
    let mut mask = [0u8; 4];
    if masked {
        s.read_exact(&mut mask)?;
    }
    let mut data = vec![0u8; len as usize];
    s.read_exact(&mut data)?;
    if masked {
        for (i, v) in data.iter_mut().enumerate() {
            *v ^= mask[i % 4];
        }
    }
    Ok(match opcode {
        2 => Frame::Binary(data),
        8 => Frame::Close,
        9 => Frame::Ping(data),
        _ => Frame::Other,
    })
}

/// A server frame (unmasked) with opcode 2 (binary), 8 (close) or 10 (pong).
pub fn frame(opcode: u8, data: &[u8]) -> Vec<u8> {
    let mut f = vec![0x80 | opcode];
    match data.len() {
        n if n < 126 => f.push(n as u8),
        n if n < 65536 => {
            f.push(126);
            f.extend_from_slice(&(n as u16).to_be_bytes());
        }
        n => {
            f.push(127);
            f.extend_from_slice(&(n as u64).to_be_bytes());
        }
    }
    f.extend_from_slice(data);
    f
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rfc_examples() {
        // RFC 6455 section 1.3
        assert_eq!(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
        assert_eq!(base64(b"fo"), "Zm8=");
        let h = sha1(b"abc");
        assert_eq!(h.iter().map(|b| format!("{b:02x}")).collect::<String>(), "a9993e364706816aba3e25717850c26c9cd0d89d");
        assert_eq!(&frame(2, &[1, 2, 3]), &[0x82, 3, 1, 2, 3]);
    }
}

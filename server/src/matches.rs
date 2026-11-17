//! Matches: the relay of inputs between the two players, the server's own
//! simulation of the confirmed frames (to check the clients' checksums and to
//! know the result), and timeouts. Transport-independent: messages come in
//! with a Peer and go out through the outbox.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use crate::engine::Game;
use crate::proto::{self, ClientMsg};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Peer {
    Udp(SocketAddr),
    Ws(u64),
}

pub const MAX_PER_PACKET: usize = 64;
pub const SILENCE_FORFEIT: Duration = Duration::from_secs(10);
pub const UNJOINED_EXPIRY: Duration = Duration::from_secs(120);

#[derive(Debug, Clone, PartialEq)]
pub struct MatchResult {
    pub match_id: u32,
    pub winner: i8, // 0, 1, or -1 (no result: a draw, a forfeit by both, or expired)
    pub forfeit: bool,
    pub frames: u32,
    pub desyncs: u32,
    pub checksums_compared: u32,
    pub bytes_in: [u64; 2],
    pub bytes_out: [u64; 2],
    pub seconds: f64,
}

pub struct Match {
    tokens: [u32; 2],
    delay: u8,
    peers: [Option<Peer>; 2],
    inputs: [Vec<u8>; 2],
    have_remote: [u32; 2], // what each client has of its opponent's inputs
    game: Game,
    server_hashes: HashMap<u32, u64>,
    pending: Vec<(u32, u64)>, // client checksums for frames the server has not reached
    desyncs: u32,
    compared: u32,
    last_seen: [Instant; 2],
    created: Instant,
    started: Option<Instant>,
    bytes_in: [u64; 2],
    bytes_out: [u64; 2],
    done: bool,
    result_msg: Option<Vec<u8>>, // RESULT, kept to resend
}

pub struct Server {
    script: String,
    pub matches: HashMap<u32, Match>,
    by_peer: HashMap<Peer, (u32, usize)>,
    pub outbox: Vec<(Peer, Vec<u8>)>,
    pub results: Vec<MatchResult>,
}

impl Server {
    pub fn new(script: String) -> Result<Server, String> {
        Game::new(&script)?; // fail early on a broken script
        Ok(Server { script, matches: HashMap::new(), by_peer: HashMap::new(), outbox: Vec::new(), results: Vec::new() })
    }

    pub fn create_match(&mut self, id: u32, tokens: [u32; 2], delay: u8, now: Instant) -> Result<(), String> {
        if self.matches.contains_key(&id) {
            return Err(format!("match {id} exists"));
        }
        let game = Game::new(&self.script)?;
        self.matches.insert(
            id,
            Match {
                tokens,
                delay,
                peers: [None, None],
                inputs: [Vec::new(), Vec::new()],
                have_remote: [0, 0],
                game,
                server_hashes: HashMap::new(),
                pending: Vec::new(),
                desyncs: 0,
                compared: 0,
                last_seen: [now, now],
                created: now,
                started: None,
                bytes_in: [0, 0],
                bytes_out: [0, 0],
                done: false,
                result_msg: None,
            },
        );
        Ok(())
    }

    fn send(&mut self, m: u32, player: usize, data: Vec<u8>) {
        if let Some(mt) = self.matches.get_mut(&m) {
            if let Some(p) = mt.peers[player] {
                mt.bytes_out[player] += data.len() as u64;
                self.outbox.push((p, data));
            }
        }
    }

    pub fn handle(&mut self, peer: Peer, data: &[u8], now: Instant) {
        let Some(msg) = proto::parse_client(data) else { return };
        match msg {
            ClientMsg::Hello { match_id, player, token } => {
                let p = player as usize;
                let ok = p < 2 && self.matches.get(&match_id).map_or(false, |m| m.tokens[p] == token && !m.done);
                if !ok {
                    self.outbox.push((peer, proto::error("unknown match, player or token")));
                    return;
                }
                let m = self.matches.get_mut(&match_id).unwrap();
                m.peers[p] = Some(peer);
                m.last_seen[p] = now;
                m.bytes_in[p] += data.len() as u64;
                let delay = m.delay;
                let both = m.peers.iter().all(|x| x.is_some());
                let start_now = both && m.started.is_none();
                if start_now {
                    m.started = Some(now);
                }
                self.by_peer.insert(peer, (match_id, p));
                self.send(match_id, p, proto::welcome(match_id, player, delay));
                if both {
                    // (again) to both: a lost START is repaired by the next HELLO
                    self.send(match_id, 0, vec![proto::START, delay]);
                    self.send(match_id, 1, vec![proto::START, delay]);
                }
            }
            ClientMsg::Input { match_id, player, have_remote, first, inputs, checksums } => {
                let p = player as usize;
                if self.by_peer.get(&peer) != Some(&(match_id, p)) {
                    return; // not this peer's seat
                }
                let Some(m) = self.matches.get_mut(&match_id) else { return };
                if let Some(r) = m.result_msg.clone() {
                    // the result was lost on the way: send it again to whoever still plays
                    m.last_seen[p] = now;
                    self.send(match_id, p, r);
                    return;
                }
                m.last_seen[p] = now;
                m.bytes_in[p] += data.len() as u64;
                m.have_remote[p] = m.have_remote[p].max(have_remote.min(m.inputs[1 - p].len() as u32));
                for (k, &v) in inputs.iter().enumerate() {
                    let f = first as usize + k;
                    if f == m.inputs[p].len() {
                        m.inputs[p].push(v);
                    }
                }
                m.pending.extend(checksums);
                // the server's own simulation of every frame whose inputs are both known
                let confirmed = m.inputs[0].len().min(m.inputs[1].len());
                let mut finished = false;
                while (m.game.frame() as usize) < confirmed && !finished {
                    let f = m.game.frame() as usize;
                    if m.game.step(m.inputs[0][f], m.inputs[1][f]).is_err() {
                        finished = true;
                        break;
                    }
                    let nf = m.game.frame() as u32;
                    if nf % 30 == 0 {
                        m.server_hashes.insert(nf, m.game.hash());
                    }
                    if m.game.result().is_some() {
                        finished = true;
                    }
                }
                let frame_now = m.game.frame() as u32;
                let mut still = Vec::new();
                for (f, h) in m.pending.drain(..) {
                    if let Some(&sh) = m.server_hashes.get(&f) {
                        m.compared += 1;
                        if sh != h {
                            m.desyncs += 1;
                        }
                    } else if f >= frame_now {
                        still.push((f, h));
                    }
                }
                m.pending = still;
                // relay: the opponent gets this player's inputs from where it said it has them
                let q = 1 - p;
                let from = m.have_remote[q] as usize;
                let to = (from + MAX_PER_PACKET).min(m.inputs[p].len());
                let pkt = proto::inputs(m.inputs[q].len() as u32, from as u32, &m.inputs[p][from..to]);
                self.send(match_id, q, pkt);
                if finished {
                    self.finish(match_id, now, None);
                }
            }
        }
    }

    /// End a match: the game's result, or `forfeit` (the winner by forfeit, -1 for none).
    fn finish(&mut self, id: u32, now: Instant, forfeit: Option<i8>) {
        let Some(m) = self.matches.get_mut(&id) else { return };
        if m.done {
            return;
        }
        m.done = true;
        let winner = forfeit.unwrap_or_else(|| m.game.result().map(|w| w as i8).unwrap_or(-1));
        let r = MatchResult {
            match_id: id,
            winner,
            forfeit: forfeit.is_some(),
            frames: m.game.frame() as u32,
            desyncs: m.desyncs,
            checksums_compared: m.compared,
            bytes_in: m.bytes_in,
            bytes_out: m.bytes_out,
            seconds: m.started.map_or(0.0, |s| (now - s).as_secs_f64()),
        };
        let desyncs = m.desyncs;
        m.result_msg = Some(proto::result(winner, desyncs));
        for p in 0..2 {
            self.send(id, p, proto::result(winner, desyncs));
        }
        self.results.push(r);
    }

    /// Forfeits after a long silence, expired matches nobody joined, cleanup of finished ones.
    pub fn tick(&mut self, now: Instant) {
        let mut forfeits = Vec::new();
        let mut remove = Vec::new();
        for (id, m) in &mut self.matches {
            if m.done {
                if now - m.last_seen[0].max(m.last_seen[1]) > Duration::from_secs(5) {
                    remove.push(*id);
                }
                continue;
            }
            match m.started {
                None if now - m.created > UNJOINED_EXPIRY => forfeits.push((*id, -1i8)),
                Some(_) => {
                    let silent: Vec<usize> = (0..2).filter(|&p| now - m.last_seen[p] > SILENCE_FORFEIT).collect();
                    match silent.len() {
                        1 => forfeits.push((*id, 1 - silent[0] as i8)),
                        2 => forfeits.push((*id, -1)),
                        _ => {}
                    }
                }
                None => {}
            }
        }
        for (id, winner) in forfeits {
            self.finish(id, now, Some(winner));
        }
        for id in remove {
            if let Some(m) = self.matches.remove(&id) {
                for p in m.peers.iter().flatten() {
                    self.by_peer.remove(p);
                }
            }
        }
    }

    pub fn disconnect(&mut self, peer: Peer) {
        self.by_peer.remove(&peer);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn script() -> String {
        std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../engine/scripts/arena.lua")).unwrap()
    }

    fn input(m: u32, p: u8, have: u32, first: u32, inputs: Vec<u8>, ck: Vec<(u32, u64)>) -> Vec<u8> {
        proto::encode_client(&ClientMsg::Input { match_id: m, player: p, have_remote: have, first, inputs, checksums: ck })
    }

    #[test]
    fn relays_simulates_and_detects_desyncs() {
        let now = Instant::now();
        let mut s = Server::new(script()).unwrap();
        s.create_match(1, [11, 22], 2, now).unwrap();
        let a = Peer::Ws(1);
        let b = Peer::Ws(2);
        s.handle(a, &proto::encode_client(&ClientMsg::Hello { match_id: 1, player: 0, token: 11 }), now);
        s.handle(b, &proto::encode_client(&ClientMsg::Hello { match_id: 1, player: 1, token: 99 }), now); // wrong token
        assert!(s.outbox.iter().any(|(p, d)| *p == b && d[0] == proto::ERROR));
        s.handle(b, &proto::encode_client(&ClientMsg::Hello { match_id: 1, player: 1, token: 22 }), now);
        assert!(s.outbox.iter().any(|(p, d)| *p == a && d == &vec![proto::START, 2]));
        s.outbox.clear();
        // 60 frames of inputs from both; b claims the right hash for frame 30, a a wrong one
        let mut reference = Game::new(&script()).unwrap();
        for _ in 0..30 {
            reference.step(3, 5).unwrap();
        }
        let right = reference.hash();
        s.handle(a, &input(1, 0, 0, 0, vec![3; 60], vec![(30, right ^ 1)]), now);
        // relayed to b: a's inputs from 0
        let (to, pkt) = s.outbox.pop().unwrap();
        assert_eq!(to, b);
        assert_eq!(&pkt[..1], &[proto::INPUTS]);
        assert_eq!(pkt[9], 60);
        s.handle(b, &input(1, 1, 60, 0, vec![5; 60], vec![(30, right)]), now);
        let m = &s.matches[&1];
        assert_eq!(m.game.frame(), 60);
        assert_eq!(m.compared, 2);
        assert_eq!(m.desyncs, 1);
        // a goes silent: b wins by forfeit
        s.tick(now + SILENCE_FORFEIT + Duration::from_secs(1));
        assert_eq!(s.results.len(), 1);
        assert!(s.results[0].forfeit);
    }

    #[test]
    fn a_lost_result_is_sent_again() {
        let now = Instant::now();
        let mut s = Server::new(script()).unwrap();
        s.create_match(3, [1, 2], 2, now).unwrap();
        let (a, b) = (Peer::Ws(1), Peer::Ws(2));
        s.handle(a, &proto::encode_client(&ClientMsg::Hello { match_id: 3, player: 0, token: 1 }), now);
        s.handle(b, &proto::encode_client(&ClientMsg::Hello { match_id: 3, player: 1, token: 2 }), now);
        s.tick(now + SILENCE_FORFEIT + Duration::from_secs(1)); // both silent: the match ends
        s.outbox.clear();
        s.handle(a, &input(3, 0, 0, 0, vec![1], vec![]), now + SILENCE_FORFEIT + Duration::from_secs(2));
        assert!(s.outbox.iter().any(|(p, d)| *p == a && d[0] == proto::RESULT));
    }

    #[test]
    fn unknown_peers_cannot_inject_inputs() {
        let now = Instant::now();
        let mut s = Server::new(script()).unwrap();
        s.create_match(5, [1, 2], 2, now).unwrap();
        s.handle(Peer::Ws(9), &input(5, 0, 0, 0, vec![1; 10], vec![]), now);
        assert!(s.matches[&5].inputs[0].is_empty());
    }
}

//! arena-server: the match server.
//!
//!   arena-server --script engine/scripts/arena.lua [--udp 0.0.0.0:7700] [--ws 0.0.0.0:7701]
//!                [--control 127.0.0.1:7702] [--report 127.0.0.1:7703]
//!
//! Players connect over UDP (native clients, bots) or WebSocket (browsers).
//! The matchmaker creates matches with a text datagram on the control port:
//!   CREATE <match> <token0> <token1> <delay>   ->  OK <match> | ERR <reason>
//! and receives every result as a JSON datagram on the report address.

mod engine;
mod matches;
mod proto;
mod ws;

use std::collections::HashMap;
use std::io::Write;
use std::net::{TcpListener, TcpStream, UdpSocket};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use matches::{MatchResult, Peer, Server};

struct Shared {
    server: Mutex<Server>,
    ws_out: Mutex<HashMap<u64, Sender<Vec<u8>>>>,
    udp: UdpSocket,
    report: Option<String>,
    report_sock: UdpSocket,
}

fn arg(args: &[String], name: &str, default: &str) -> String {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned().unwrap_or_else(|| default.to_string())
}

fn result_json(r: &MatchResult) -> String {
    format!(
        "{{\"match\": {}, \"winner\": {}, \"forfeit\": {}, \"frames\": {}, \"desyncs\": {}, \"checksums\": {}, \"bytes_in\": [{}, {}], \"bytes_out\": [{}, {}], \"seconds\": {:.3}}}",
        r.match_id, r.winner, r.forfeit, r.frames, r.desyncs, r.checksums_compared, r.bytes_in[0], r.bytes_in[1], r.bytes_out[0], r.bytes_out[1], r.seconds
    )
}

/// Deliver what the match logic queued, and report finished matches.
fn flush(sh: &Shared, s: &mut Server) {
    for (peer, data) in s.outbox.drain(..) {
        match peer {
            Peer::Udp(addr) => {
                let _ = sh.udp.send_to(&data, addr);
            }
            Peer::Ws(id) => {
                if let Some(tx) = sh.ws_out.lock().unwrap().get(&id) {
                    let _ = tx.send(ws::frame(2, &data));
                }
            }
        }
    }
    for r in s.results.drain(..) {
        let line = result_json(&r);
        println!("{line}");
        let _ = std::io::stdout().flush();
        if let Some(addr) = &sh.report {
            let _ = sh.report_sock.send_to(line.as_bytes(), addr);
        }
    }
}

fn serve_ws(sh: Arc<Shared>, mut stream: TcpStream, id: u64) {
    if ws::handshake(&mut stream).is_err() {
        return;
    }
    let _ = stream.set_nodelay(true);
    let (tx, rx) = channel::<Vec<u8>>();
    sh.ws_out.lock().unwrap().insert(id, tx.clone());
    let mut writer = stream.try_clone().expect("clone stream");
    let w = thread::spawn(move || {
        for f in rx {
            if writer.write_all(&f).is_err() {
                break;
            }
        }
    });
    loop {
        match ws::read_frame(&mut stream) {
            Ok(ws::Frame::Binary(data)) => {
                let mut s = sh.server.lock().unwrap();
                s.handle(Peer::Ws(id), &data, Instant::now());
                flush(&sh, &mut s);
            }
            Ok(ws::Frame::Ping(p)) => {
                let _ = tx.send(ws::frame(10, &p));
            }
            Ok(ws::Frame::Other) => {}
            Ok(ws::Frame::Close) | Err(_) => break,
        }
    }
    let _ = tx.send(ws::frame(8, &[]));
    sh.ws_out.lock().unwrap().remove(&id);
    sh.server.lock().unwrap().disconnect(Peer::Ws(id));
    drop(tx);
    let _ = w.join();
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let script_path = arg(&args, "--script", "engine/scripts/arena.lua");
    let script = std::fs::read_to_string(&script_path).unwrap_or_else(|e| panic!("cannot read {script_path}: {e}"));
    let server = Server::new(script).unwrap_or_else(|e| panic!("{e}"));
    let udp = UdpSocket::bind(arg(&args, "--udp", "0.0.0.0:7700")).expect("bind udp");
    udp.set_read_timeout(Some(Duration::from_millis(20))).unwrap();
    let report = args.iter().any(|a| a == "--report").then(|| arg(&args, "--report", ""));
    let sh = Arc::new(Shared {
        server: Mutex::new(server),
        ws_out: Mutex::new(HashMap::new()),
        udp: udp.try_clone().unwrap(),
        report,
        report_sock: UdpSocket::bind("0.0.0.0:0").unwrap(),
    });
    eprintln!("arena-server: udp {}, ws {}, control {}", udp.local_addr().unwrap(), arg(&args, "--ws", "0.0.0.0:7701"), arg(&args, "--control", "127.0.0.1:7702"));

    // the control port (matchmaker -> server)
    let control = UdpSocket::bind(arg(&args, "--control", "127.0.0.1:7702")).expect("bind control");
    {
        let sh = sh.clone();
        thread::spawn(move || {
            let mut buf = [0u8; 512];
            loop {
                let Ok((n, from)) = control.recv_from(&mut buf) else { continue };
                let text = String::from_utf8_lossy(&buf[..n]).to_string();
                let f: Vec<&str> = text.split_whitespace().collect();
                let reply = match f.as_slice() {
                    ["CREATE", id, t0, t1, delay] => match (id.parse(), t0.parse(), t1.parse(), delay.parse()) {
                        (Ok(id), Ok(t0), Ok(t1), Ok(d)) => match sh.server.lock().unwrap().create_match(id, [t0, t1], d, Instant::now()) {
                            Ok(()) => format!("OK {id}"),
                            Err(e) => format!("ERR {e}"),
                        },
                        _ => "ERR bad numbers".to_string(),
                    },
                    _ => "ERR unknown command".to_string(),
                };
                let _ = control.send_to(reply.as_bytes(), from);
            }
        });
    }
    // WebSocket clients
    {
        let sh = sh.clone();
        let listener = TcpListener::bind(arg(&args, "--ws", "0.0.0.0:7701")).expect("bind ws");
        thread::spawn(move || {
            let mut next = 1u64;
            for stream in listener.incoming().flatten() {
                let sh = sh.clone();
                let id = next;
                next += 1;
                thread::spawn(move || serve_ws(sh, stream, id));
            }
        });
    }
    // UDP clients, and the periodic timeouts
    let mut buf = [0u8; 2048];
    let mut last_tick = Instant::now();
    loop {
        if let Ok((n, from)) = udp.recv_from(&mut buf) {
            let mut s = sh.server.lock().unwrap();
            s.handle(Peer::Udp(from), &buf[..n], Instant::now());
            flush(&sh, &mut s);
        }
        if last_tick.elapsed() > Duration::from_millis(250) {
            last_tick = Instant::now();
            let mut s = sh.server.lock().unwrap();
            s.tick(Instant::now());
            flush(&sh, &mut s);
        }
    }
}

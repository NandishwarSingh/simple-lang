use std::sync::mpsc;
use std::thread;

fn main() {
    let (ping_tx, ping_rx) = mpsc::sync_channel::<i64>(1);
    let (pong_tx, pong_rx) = mpsc::sync_channel::<i64>(1);
    thread::spawn(move || loop {
        let v = ping_rx.recv().unwrap();
        if v == -1 {
            return;
        }
        pong_tx.send(v + 1).unwrap();
    });
    let mut total: i64 = 0;
    for i in 0..100000i64 {
        ping_tx.send(i).unwrap();
        total += pong_rx.recv().unwrap();
    }
    ping_tx.send(-1).unwrap();
    println!("{}", total);
}

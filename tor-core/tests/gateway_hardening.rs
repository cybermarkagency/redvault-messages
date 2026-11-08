// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! The SOCKS gate as a whole (not just the handshake): where it listens, how
//! its credentials are handled, and that it can't be used as an open proxy.
//! Uses the test-hooks blocked network, so no Tor traffic leaves the machine.
#![cfg(feature = "test-hooks")]

use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream as StdTcpStream, UdpSocket};
use std::path::PathBuf;
use std::time::{Duration, Instant};

use redvault_tor::{Gateway, GatewayConfig};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

const ONION: &str = "2gzyxa5ihm7nsggfxnu52rck2vv4rvmdlkiu3zzui5du4xyclen53wid.onion";

fn temp_dir(name: &str) -> PathBuf {
    let mut bytes = [0u8; 8];
    getrandom::fill(&mut bytes).unwrap();
    let dir = std::env::temp_dir().join(format!("rvtor-test-{name}-{:x}", u64::from_le_bytes(bytes)));
    std::fs::create_dir_all(&dir).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700)).unwrap();
    }
    dir
}

async fn blocked_gateway(name: &str) -> Gateway {
    let root = temp_dir(name);
    Gateway::start(GatewayConfig {
        state_dir: root.join("state"),
        cache_dir: root.join("cache"),
        allowed_targets: vec![format!("{ONION}:80")],
        test_blocked_network: true,
    })
    .await
    .expect("gateway starts")
}

/// This machine's non-loopback address, if it has one (no packet is sent).
fn lan_address() -> Option<IpAddr> {
    let socket = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).ok()?;
    socket.connect(("192.0.2.1", 9)).ok()?;
    let ip = socket.local_addr().ok()?.ip();
    (!ip.is_loopback() && !ip.is_unspecified()).then_some(ip)
}

async fn auth(port: u16, user: &str, pass: &str) -> (TcpStream, [u8; 2]) {
    let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).await.unwrap();
    s.write_all(&[5, 1, 2]).await.unwrap();
    let mut choice = [0u8; 2];
    s.read_exact(&mut choice).await.unwrap();
    let mut msg = vec![1, user.len() as u8];
    msg.extend_from_slice(user.as_bytes());
    msg.push(pass.len() as u8);
    msg.extend_from_slice(pass.as_bytes());
    s.write_all(&msg).await.unwrap();
    let mut result = [0u8; 2];
    s.read_exact(&mut result).await.unwrap();
    (s, result)
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn listens_on_loopback_only() {
    let gateway = blocked_gateway("loopback").await;
    let endpoint = gateway.endpoint().clone();
    assert_eq!(endpoint.host, "127.0.0.1");
    assert!(StdTcpStream::connect_timeout(&SocketAddr::from((Ipv4Addr::LOCALHOST, endpoint.port)), Duration::from_secs(2)).is_ok());
    if let Some(lan) = lan_address() {
        let reached = StdTcpStream::connect_timeout(&SocketAddr::new(lan, endpoint.port), Duration::from_secs(2));
        assert!(reached.is_err(), "the gate answered on {lan}");
    } else {
        eprintln!("no non-loopback address on this machine: LAN check skipped");
    }
    gateway.stop();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn credentials_are_random_long_and_never_printed() {
    let a = blocked_gateway("creds-a").await;
    let b = blocked_gateway("creds-b").await;
    let (ea, eb) = (a.endpoint().clone(), b.endpoint().clone());
    assert_eq!(ea.username.len(), 32);
    assert_eq!(ea.password.len(), 32);
    assert_ne!(ea.password, eb.password);
    assert_ne!(ea.username, eb.username);
    let printed = format!("{ea:?}");
    assert!(!printed.contains(&ea.password) && !printed.contains(&ea.username), "{printed}");
    a.stop();
    b.stop();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn wrong_credentials_are_refused_slowly() {
    let gateway = blocked_gateway("wrong").await;
    let endpoint = gateway.endpoint().clone();
    let started = Instant::now();
    let (_s, result) = auth(endpoint.port, &endpoint.username, &"0".repeat(32)).await;
    assert_eq!(result, [1, 1]);
    assert!(started.elapsed() >= Duration::from_millis(450), "refusal came back in {:?}", started.elapsed());
    let (_s, result) = auth(endpoint.port, &endpoint.username, &endpoint.password).await;
    assert_eq!(result, [1, 0]);
    assert_eq!(gateway.status().stats.refused_auth, 1);
    gateway.stop();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn is_not_an_open_proxy_even_with_the_right_password() {
    let gateway = blocked_gateway("open").await;
    let endpoint = gateway.endpoint().clone();
    for (host, port) in [("example.com", 80u16), ("www.redvaultsystems.com", 443), (ONION, 443), ("localhost", 22)] {
        let (mut s, result) = auth(endpoint.port, &endpoint.username, &endpoint.password).await;
        assert_eq!(result, [1, 0]);
        let mut req = vec![5, 1, 0, 3, host.len() as u8];
        req.extend_from_slice(host.as_bytes());
        req.extend_from_slice(&port.to_be_bytes());
        s.write_all(&req).await.unwrap();
        let mut rep = [0u8; 10];
        s.read_exact(&mut rep).await.unwrap();
        assert_eq!(rep[1], 0x02, "{host}:{port} must be refused by the allowlist");
    }
    assert_eq!(gateway.status().stats.refused_target, 4);
    assert_eq!(gateway.status().stats.tor_streams_opened, 0);
    gateway.stop();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn stopping_closes_the_port() {
    let gateway = blocked_gateway("stop").await;
    let port = gateway.endpoint().port;
    gateway.stop();
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert!(TcpStream::connect((Ipv4Addr::LOCALHOST, port)).await.is_err(), "port still open after stop");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn can_start_again_on_the_same_directories_after_stopping() {
    let root = temp_dir("restart");
    let config = || GatewayConfig {
        state_dir: root.join("state"),
        cache_dir: root.join("cache"),
        allowed_targets: vec![format!("{ONION}:80")],
        test_blocked_network: true,
    };
    let first = Gateway::start(config()).await.expect("first start");
    first.stop();
    tokio::time::sleep(Duration::from_millis(300)).await;
    let second = Gateway::start(config()).await.expect("second start");
    let status = second.status();
    assert_ne!(status.detail, "Tor is stopped", "{status:?}");
    second.stop();
}

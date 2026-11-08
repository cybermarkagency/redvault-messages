// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Test probe for the gateway (Windows, Linux): starts it, prints status
//! changes as JSON lines on stderr, and optionally makes HTTP requests through
//! the SOCKS gate the way an app would (socks5h: the hostname goes to the
//! gate, nothing is resolved locally).
//!
//! rv-tor-probe --state DIR --cache DIR --target HOST.onion:80
//!              [--get PATH]... [--wait SECONDS] [--blocked-network]
//!
//! Each --get prints one JSON line on stdout: {"path","status","bytes"} or
//! {"path","error"}. Exit code 0 only if every --get got an HTTP response.

use std::net::Ipv4Addr;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use redvault_tor::{Gateway, GatewayConfig, SocksEndpoint, TorState};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

struct Args {
    state: PathBuf,
    cache: PathBuf,
    target: String,
    gets: Vec<String>,
    wait: u64,
    blocked: bool,
}

fn parse_args() -> Result<Args, String> {
    let mut args = Args {
        state: PathBuf::new(),
        cache: PathBuf::new(),
        target: String::new(),
        gets: Vec::new(),
        wait: 300,
        blocked: false,
    };
    let mut it = std::env::args().skip(1);
    while let Some(flag) = it.next() {
        if flag == "--blocked-network" {
            args.blocked = true;
            continue;
        }
        let value = it.next().ok_or_else(|| format!("{flag} needs a value"))?;
        match flag.as_str() {
            "--state" => args.state = value.into(),
            "--cache" => args.cache = value.into(),
            "--target" => args.target = value,
            "--get" => args.gets.push(value),
            "--wait" => args.wait = value.parse().map_err(|_| "bad --wait")?,
            other => return Err(format!("unknown flag {other}")),
        }
    }
    if args.state.as_os_str().is_empty() || args.cache.as_os_str().is_empty() || args.target.is_empty() {
        return Err("--state, --cache and --target are required".into());
    }
    Ok(args)
}

/// SOCKS5 CONNECT with username/password, hostname address type (like libcurl's socks5h).
async fn socks_connect(endpoint: &SocksEndpoint, host: &str, port: u16) -> std::io::Result<TcpStream> {
    let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, endpoint.port)).await?;
    s.write_all(&[5, 1, 2]).await?;
    let mut choice = [0u8; 2];
    s.read_exact(&mut choice).await?;
    if choice != [5, 2] {
        return Err(std::io::Error::other("gate refused auth method"));
    }
    let mut auth = vec![1, endpoint.username.len() as u8];
    auth.extend_from_slice(endpoint.username.as_bytes());
    auth.push(endpoint.password.len() as u8);
    auth.extend_from_slice(endpoint.password.as_bytes());
    s.write_all(&auth).await?;
    let mut result = [0u8; 2];
    s.read_exact(&mut result).await?;
    if result != [1, 0] {
        return Err(std::io::Error::other("gate refused credentials"));
    }
    let mut req = vec![5, 1, 0, 3, host.len() as u8];
    req.extend_from_slice(host.as_bytes());
    req.extend_from_slice(&port.to_be_bytes());
    s.write_all(&req).await?;
    let mut rep = [0u8; 10];
    s.read_exact(&mut rep).await?;
    if rep[1] != 0 {
        return Err(std::io::Error::other(format!("gate reply code {}", rep[1])));
    }
    Ok(s)
}

async fn http_get(endpoint: &SocksEndpoint, target: &str, path: &str) -> Result<(u16, usize), String> {
    let (host, port) = target.rsplit_once(':').ok_or("bad target")?;
    let port: u16 = port.parse().map_err(|_| "bad port")?;
    let mut s = socks_connect(endpoint, host, port).await.map_err(|e| e.to_string())?;
    let request = format!("GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n");
    s.write_all(request.as_bytes()).await.map_err(|e| e.to_string())?;
    let mut response = Vec::new();
    s.read_to_end(&mut response).await.map_err(|e| e.to_string())?;
    let text = String::from_utf8_lossy(&response);
    let status = text
        .split_whitespace()
        .nth(1)
        .and_then(|code| code.parse::<u16>().ok())
        .ok_or("no HTTP status line")?;
    Ok((status, response.len()))
}

#[tokio::main(flavor = "multi_thread", worker_threads = 2)]
async fn main() {
    let args = match parse_args() {
        Ok(args) => args,
        Err(message) => {
            eprintln!("rv-tor-probe: {message}");
            std::process::exit(2);
        }
    };
    if let Ok(level) = std::env::var("RV_TOR_LOG") {
        tracing_subscriber::fmt()
            .with_env_filter(tracing_subscriber::EnvFilter::new(level))
            .with_writer(std::io::stderr)
            .with_ansi(false)
            .init();
    }
    let gateway = Gateway::start(GatewayConfig {
        state_dir: args.state,
        cache_dir: args.cache,
        allowed_targets: vec![args.target.clone()],
        #[cfg(feature = "test-hooks")]
        test_blocked_network: args.blocked,
    })
    .await
    .unwrap_or_else(|e| {
        eprintln!("rv-tor-probe: {e}");
        std::process::exit(1);
    });
    #[cfg(not(feature = "test-hooks"))]
    if args.blocked {
        eprintln!("rv-tor-probe: --blocked-network needs the test-hooks feature");
        std::process::exit(2);
    }

    let started = Instant::now();
    let mut last = String::new();
    loop {
        let status = gateway.status();
        let line = serde_json::to_string(&serde_json::json!({
            "t": started.elapsed().as_secs(),
            "state": status.state,
            "progress": status.progress,
            "detail": status.detail,
        }))
        .unwrap_or_default();
        let key = format!("{:?}{}{}", status.state, status.progress, status.detail);
        if key != last {
            eprintln!("{line}");
            last = key;
        }
        if status.state == TorState::Connected {
            break;
        }
        if started.elapsed() > Duration::from_secs(args.wait) {
            break;
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }

    let mut all_ok = true;
    for path in &args.gets {
        let began = Instant::now();
        let line = match http_get(gateway.endpoint(), &args.target, path).await {
            Ok((status, bytes)) => serde_json::json!({
                "path": path, "status": status, "bytes": bytes, "ms": began.elapsed().as_millis() as u64
            }),
            Err(error) => {
                all_ok = false;
                serde_json::json!({ "path": path, "error": error, "ms": began.elapsed().as_millis() as u64 })
            }
        };
        println!("{line}");
    }
    let final_status = gateway.status();
    eprintln!("{}", serde_json::to_string(&final_status).unwrap_or_default());
    gateway.stop();
    std::process::exit(if all_ok { 0 } else { 1 });
}

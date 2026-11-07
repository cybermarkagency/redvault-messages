//! The loopback SOCKS5 gate the apps' HTTP clients use (libcurl `socks5h://`,
//! OkHttp `Proxy.Type.SOCKS`).
//!
//! It is deliberately strict, so a mistake in an app turns into a visible
//! failure instead of a leak:
//! - username/password auth (RFC 1929) with random per-run credentials is the
//!   only method; other local processes can't borrow the port;
//! - CONNECT only (no BIND, no UDP ASSOCIATE);
//! - hostnames only: an IPv4/IPv6 literal means the app resolved the name
//!   itself, outside Tor, so it is refused;
//! - only the exact onion `host:port` pairs on the allowlist.
//!
//! Streams are opened with Arti, so there is no code path here that opens a
//! direct connection to anything.

use std::future::Future;
use std::io;
use std::sync::Arc;
use std::time::Duration;

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::Semaphore;
use tokio::task::JoinSet;
use tokio::time::timeout;

use crate::policy::TargetPolicy;
use crate::status::{Counter, StatusCell};

/// The whole SOCKS handshake must finish within this.
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(10);
/// First contact with an onion service can take a while (descriptor fetch,
/// introduction, rendezvous); later streams reuse the circuit.
pub(crate) const CONNECT_TIMEOUT: Duration = Duration::from_secs(90);
/// The apps open a handful of connections; this only bounds local abuse.
const MAX_CONNECTIONS: usize = 128;
/// Every wrong username/password waits this long before the refusal: with
/// 128-bit credentials guessing is hopeless anyway, this just makes trying
/// expensive for another local process.
const AUTH_FAILURE_DELAY: Duration = Duration::from_millis(500);

const REP_SUCCEEDED: u8 = 0x00;
const REP_HOST_UNREACHABLE: u8 = 0x04;
const REP_NOT_ALLOWED: u8 = 0x02;
const REP_COMMAND_NOT_SUPPORTED: u8 = 0x07;
const REP_ADDRESS_TYPE_NOT_SUPPORTED: u8 = 0x08;

/// Opens a stream to an allowed onion `host:port`. The real one is Arti; tests use a fake.
pub(crate) trait Connector: Send + Sync + 'static {
    type Stream: AsyncRead + AsyncWrite + Unpin + Send + 'static;
    fn connect(&self, host: String, port: u16) -> impl Future<Output = io::Result<Self::Stream>> + Send;
}

pub(crate) struct SocksContext<C> {
    pub connector: C,
    pub policy: TargetPolicy,
    pub username: String,
    pub password: String,
    pub status: Arc<StatusCell>,
}

impl<C> Drop for SocksContext<C> {
    fn drop(&mut self) {
        zeroize::Zeroize::zeroize(&mut self.username);
        zeroize::Zeroize::zeroize(&mut self.password);
    }
}

/// Accepts connections until the task is aborted (aborting also drops every open stream).
pub(crate) async fn serve<C: Connector>(listener: TcpListener, ctx: Arc<SocksContext<C>>) {
    let limit = Arc::new(Semaphore::new(MAX_CONNECTIONS));
    let mut tasks = JoinSet::new();
    loop {
        let (socket, peer) = match listener.accept().await {
            Ok(accepted) => accepted,
            Err(_) => {
                tokio::time::sleep(Duration::from_millis(100)).await;
                continue;
            }
        };
        // The listener is bound to 127.0.0.1, so this can't happen; checked anyway.
        if !peer.ip().is_loopback() {
            continue;
        }
        let Ok(permit) = limit.clone().try_acquire_owned() else {
            continue;
        };
        ctx.status.count(Counter::SocksConnection);
        let ctx = Arc::clone(&ctx);
        tasks.spawn(async move {
            let _permit = permit;
            let _ = handle(socket, ctx).await;
        });
        while tasks.try_join_next().is_some() {}
    }
}

struct Request {
    host: String,
    port: u16,
}

async fn handle<C: Connector>(mut client: TcpStream, ctx: Arc<SocksContext<C>>) -> io::Result<()> {
    client.set_nodelay(true).ok();
    let request = match timeout(HANDSHAKE_TIMEOUT, negotiate(&mut client, &ctx)).await {
        Ok(Ok(Some(request))) => request,
        Ok(Ok(None)) | Err(_) => return Ok(()),
        Ok(Err(error)) => return Err(error),
    };
    let connected = timeout(CONNECT_TIMEOUT, ctx.connector.connect(request.host, request.port)).await;
    let mut upstream = match connected {
        Ok(Ok(stream)) => {
            ctx.status.on_stream(true);
            stream
        }
        Ok(Err(_)) | Err(_) => {
            ctx.status.on_stream(false);
            reply(&mut client, REP_HOST_UNREACHABLE).await?;
            return Ok(());
        }
    };
    reply(&mut client, REP_SUCCEEDED).await?;
    let _ = tokio::io::copy_bidirectional(&mut client, &mut upstream).await;
    Ok(())
}

/// Runs the SOCKS5 handshake. `None` means the request was refused (the
/// refusal has been sent) or the peer isn't speaking SOCKS5.
async fn negotiate<C>(client: &mut TcpStream, ctx: &SocksContext<C>) -> io::Result<Option<Request>> {
    let mut greeting = [0u8; 2];
    client.read_exact(&mut greeting).await?;
    if greeting[0] != 5 {
        return Ok(None);
    }
    let mut methods = vec![0u8; usize::from(greeting[1])];
    client.read_exact(&mut methods).await?;
    if !methods.contains(&0x02) {
        ctx.status.count(Counter::RefusedAuth);
        client.write_all(&[5, 0xFF]).await?;
        return Ok(None);
    }
    client.write_all(&[5, 0x02]).await?;

    // RFC 1929 username/password.
    if client.read_u8().await? != 1 {
        return Ok(None);
    }
    let username = read_short(client).await?;
    let password = read_short(client).await?;
    let user_ok = constant_time_eq(&username, ctx.username.as_bytes());
    let pass_ok = constant_time_eq(&password, ctx.password.as_bytes());
    if !(user_ok & pass_ok) {
        ctx.status.count(Counter::RefusedAuth);
        tokio::time::sleep(AUTH_FAILURE_DELAY).await;
        client.write_all(&[1, 1]).await?;
        return Ok(None);
    }
    client.write_all(&[1, 0]).await?;

    let mut head = [0u8; 4];
    client.read_exact(&mut head).await?;
    if head[0] != 5 {
        return Ok(None);
    }
    // Read the whole request before answering: closing with unread bytes
    // makes the OS reset the connection, and the client would never see why.
    let name = match head[3] {
        0x03 => Some(read_short(client).await?),
        0x01 => {
            client.read_exact(&mut [0u8; 4]).await?;
            None
        }
        0x04 => {
            client.read_exact(&mut [0u8; 16]).await?;
            None
        }
        _ => {
            reply(client, REP_ADDRESS_TYPE_NOT_SUPPORTED).await?;
            return Ok(None);
        }
    };
    let port = client.read_u16().await?;
    if head[1] != 0x01 {
        ctx.status.count(Counter::RefusedCommand);
        reply(client, REP_COMMAND_NOT_SUPPORTED).await?;
        return Ok(None);
    }
    let Some(name) = name else {
        // An IPv4/IPv6 literal: the name was resolved outside Tor.
        ctx.status.count(Counter::RefusedAddressType);
        reply(client, REP_ADDRESS_TYPE_NOT_SUPPORTED).await?;
        return Ok(None);
    };
    let host = match String::from_utf8(name) {
        Ok(host) if ctx.policy.allows(&host, port) => host.to_ascii_lowercase(),
        _ => {
            ctx.status.count(Counter::RefusedTarget);
            reply(client, REP_NOT_ALLOWED).await?;
            return Ok(None);
        }
    };
    Ok(Some(Request { host, port }))
}

async fn read_short(client: &mut TcpStream) -> io::Result<Vec<u8>> {
    let len = client.read_u8().await?;
    let mut bytes = vec![0u8; usize::from(len)];
    client.read_exact(&mut bytes).await?;
    Ok(bytes)
}

async fn reply(client: &mut TcpStream, code: u8) -> io::Result<()> {
    client.write_all(&[5, code, 0, 1, 0, 0, 0, 0, 0, 0]).await?;
    client.flush().await
}

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::status::TorState;
    use std::net::Ipv4Addr;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use tokio::io::DuplexStream;

    const ONION: &str = "2gzyxa5ihm7nsggfxnu52rck2vv4rvmdlkiu3zzui5du4xyclen53wid.onion";

    /// Stands in for Arti: an in-memory echo service, or a failure.
    struct FakeConnector {
        calls: Arc<AtomicUsize>,
        fail: bool,
    }

    impl Connector for FakeConnector {
        type Stream = DuplexStream;
        fn connect(&self, host: String, port: u16) -> impl Future<Output = io::Result<DuplexStream>> + Send {
            self.calls.fetch_add(1, Ordering::SeqCst);
            let fail = self.fail;
            async move {
                assert_eq!((host.as_str(), port), (ONION, 80), "only allowed targets reach the connector");
                if fail {
                    return Err(io::Error::other("tor unavailable"));
                }
                let (ours, mut theirs) = tokio::io::duplex(1024);
                tokio::spawn(async move {
                    let mut buf = [0u8; 64];
                    while let Ok(n) = theirs.read(&mut buf).await {
                        if n == 0 || theirs.write_all(&buf[..n]).await.is_err() {
                            break;
                        }
                    }
                });
                Ok(ours)
            }
        }
    }

    struct Harness {
        port: u16,
        calls: Arc<AtomicUsize>,
        status: Arc<StatusCell>,
    }

    async fn start(fail: bool) -> Harness {
        let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
        let port = listener.local_addr().unwrap().port();
        let calls = Arc::new(AtomicUsize::new(0));
        let status = Arc::new(StatusCell::default());
        status.on_bootstrap(1.0, true, "ready".into(), None);
        let ctx = Arc::new(SocksContext {
            connector: FakeConnector { calls: Arc::clone(&calls), fail },
            policy: TargetPolicy::parse(&[format!("{ONION}:80")]).unwrap(),
            username: "u".repeat(32),
            password: "p".repeat(32),
            status: Arc::clone(&status),
        });
        tokio::spawn(serve(listener, ctx));
        Harness { port, calls, status }
    }

    async fn authed(port: u16, password: &str) -> (TcpStream, [u8; 2]) {
        let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).await.unwrap();
        s.write_all(&[5, 1, 0x02]).await.unwrap();
        let mut choice = [0u8; 2];
        s.read_exact(&mut choice).await.unwrap();
        assert_eq!(choice, [5, 0x02]);
        let user = "u".repeat(32);
        let mut auth = vec![1, user.len() as u8];
        auth.extend_from_slice(user.as_bytes());
        auth.push(password.len() as u8);
        auth.extend_from_slice(password.as_bytes());
        s.write_all(&auth).await.unwrap();
        let mut result = [0u8; 2];
        s.read_exact(&mut result).await.unwrap();
        (s, result)
    }

    async fn connect_domain(s: &mut TcpStream, host: &str, port: u16) -> u8 {
        let mut req = vec![5, 1, 0, 3, host.len() as u8];
        req.extend_from_slice(host.as_bytes());
        req.extend_from_slice(&port.to_be_bytes());
        s.write_all(&req).await.unwrap();
        let mut rep = [0u8; 10];
        s.read_exact(&mut rep).await.unwrap();
        rep[1]
    }

    #[tokio::test]
    async fn allowed_onion_target_is_proxied() {
        let h = start(false).await;
        let (mut s, auth) = authed(h.port, &"p".repeat(32)).await;
        assert_eq!(auth, [1, 0]);
        assert_eq!(connect_domain(&mut s, &ONION.to_ascii_uppercase(), 80).await, REP_SUCCEEDED);
        s.write_all(b"ping").await.unwrap();
        let mut echo = [0u8; 4];
        s.read_exact(&mut echo).await.unwrap();
        assert_eq!(&echo, b"ping");
        assert_eq!(h.calls.load(Ordering::SeqCst), 1);
        assert_eq!(h.status.snapshot().stats.tor_streams_opened, 1);
    }

    #[tokio::test]
    async fn no_auth_method_is_refused() {
        let h = start(false).await;
        let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, h.port)).await.unwrap();
        s.write_all(&[5, 1, 0x00]).await.unwrap();
        let mut choice = [0u8; 2];
        s.read_exact(&mut choice).await.unwrap();
        assert_eq!(choice, [5, 0xFF]);
        assert_eq!(h.status.snapshot().stats.refused_auth, 1);
    }

    #[tokio::test]
    async fn wrong_password_is_refused() {
        let h = start(false).await;
        let (_s, auth) = authed(h.port, &"x".repeat(32)).await;
        assert_eq!(auth, [1, 1]);
        assert_eq!(h.calls.load(Ordering::SeqCst), 0);
    }

    #[tokio::test]
    async fn clearnet_and_other_onions_are_refused() {
        let h = start(false).await;
        for (host, port) in [
            ("www.redvaultsystems.com", 443),
            ("localhost", 3000),
            (ONION, 443),
            ("vww6ybal4bd7szmgncyruucpgfkqahzddi37ktceo3ah7ngmcopnpyyd.onion", 80),
        ] {
            let (mut s, _) = authed(h.port, &"p".repeat(32)).await;
            assert_eq!(connect_domain(&mut s, host, port).await, REP_NOT_ALLOWED, "{host}:{port}");
        }
        assert_eq!(h.calls.load(Ordering::SeqCst), 0, "nothing reached the connector");
        assert_eq!(h.status.snapshot().stats.refused_target, 4);
    }

    #[tokio::test]
    async fn ip_literals_are_refused() {
        let h = start(false).await;
        let (mut s, _) = authed(h.port, &"p".repeat(32)).await;
        s.write_all(&[5, 1, 0, 1, 104, 21, 0, 1, 0, 80]).await.unwrap();
        let mut rep = [0u8; 10];
        s.read_exact(&mut rep).await.unwrap();
        assert_eq!(rep[1], REP_ADDRESS_TYPE_NOT_SUPPORTED);

        let (mut s, _) = authed(h.port, &"p".repeat(32)).await;
        let mut v6 = vec![5, 1, 0, 4];
        v6.extend_from_slice(&[0u8; 15]);
        v6.push(1);
        v6.extend_from_slice(&80u16.to_be_bytes());
        s.write_all(&v6).await.unwrap();
        s.read_exact(&mut rep).await.unwrap();
        assert_eq!(rep[1], REP_ADDRESS_TYPE_NOT_SUPPORTED);
        assert_eq!(h.status.snapshot().stats.refused_address_type, 2);
        assert_eq!(h.calls.load(Ordering::SeqCst), 0);
    }

    #[tokio::test]
    async fn bind_and_udp_are_refused() {
        let h = start(false).await;
        for command in [0x02u8, 0x03] {
            let (mut s, _) = authed(h.port, &"p".repeat(32)).await;
            let mut req = vec![5, command, 0, 3, ONION.len() as u8];
            req.extend_from_slice(ONION.as_bytes());
            req.extend_from_slice(&80u16.to_be_bytes());
            s.write_all(&req).await.unwrap();
            let mut rep = [0u8; 10];
            s.read_exact(&mut rep).await.unwrap();
            assert_eq!(rep[1], REP_COMMAND_NOT_SUPPORTED);
        }
        assert_eq!(h.calls.load(Ordering::SeqCst), 0);
    }

    #[tokio::test]
    async fn tor_failure_fails_closed_and_turns_unavailable() {
        let h = start(true).await;
        for _ in 0..2 {
            let (mut s, _) = authed(h.port, &"p".repeat(32)).await;
            assert_eq!(connect_domain(&mut s, ONION, 80).await, REP_HOST_UNREACHABLE);
        }
        let status = h.status.snapshot();
        assert_eq!(status.state, TorState::Unavailable);
        assert_eq!(status.stats.tor_streams_failed, 2);
    }

    #[tokio::test]
    async fn socks4_is_ignored() {
        let h = start(false).await;
        let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, h.port)).await.unwrap();
        s.write_all(&[4, 1, 0, 80, 1, 2, 3, 4, 0]).await.unwrap();
        let mut buf = [0u8; 8];
        let n = s.read(&mut buf).await.unwrap_or(0);
        assert_eq!(n, 0, "closed without a reply");
        assert_eq!(h.calls.load(Ordering::SeqCst), 0);
    }
}

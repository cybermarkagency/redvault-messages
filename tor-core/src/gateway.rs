//! The gateway: an embedded Arti client plus the loopback SOCKS gate.

use std::io;
use std::net::Ipv4Addr;
use std::path::PathBuf;
use std::sync::Arc;

use arti_client::config::TorClientConfigBuilder;
use arti_client::{BootstrapBehavior, DataStream, DormantMode, StreamPrefs, TorClient};
use futures::StreamExt;
use serde::Serialize;
use tokio::net::TcpListener;
use tokio::task::JoinHandle;
use tor_rtcompat::tokio::TokioRustlsRuntime;
use zeroize::{Zeroize, ZeroizeOnDrop};

use crate::policy::{PolicyError, TargetPolicy};
use crate::socks::{self, Connector, SocksContext};
use crate::status::{Status, StatusCell};

#[derive(Debug, Clone)]
pub struct GatewayConfig {
    /// Arti's persistent state (guards, keys). App-private storage.
    pub state_dir: PathBuf,
    /// Arti's directory cache. App-private storage.
    pub cache_dir: PathBuf,
    /// `"<v3>.onion:<port>"` entries: the only places the gate will connect to.
    pub allowed_targets: Vec<String>,
    /// Test only: point Arti at an unreachable directory cache so it can never
    /// bootstrap (a censored or offline network). Needs an empty cache_dir.
    #[cfg(feature = "test-hooks")]
    pub test_blocked_network: bool,
}

/// Where the app's HTTP client connects: `socks5h://username:password@host:port`.
///
/// The credentials are 128-bit random per run, held in memory only, wiped
/// when dropped, and never printed (`Debug` redacts them). The host is always
/// 127.0.0.1: the gate never listens on another interface.
#[derive(Clone, Serialize, Zeroize, ZeroizeOnDrop)]
pub struct SocksEndpoint {
    pub host: String,
    pub port: u16,
    pub username: String,
    pub password: String,
}

impl std::fmt::Debug for SocksEndpoint {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SocksEndpoint")
            .field("host", &self.host)
            .field("port", &self.port)
            .field("username", &"[redacted]")
            .field("password", &"[redacted]")
            .finish()
    }
}

#[derive(Debug, thiserror::Error)]
pub enum GatewayError {
    #[error("bad routing policy: {0}")]
    Policy(#[from] PolicyError),
    #[error("bad Tor configuration: {0}")]
    Config(String),
    #[error("couldn't start Tor: {0}")]
    Tor(String),
    #[error("couldn't open the local SOCKS port: {0}")]
    Io(#[from] io::Error),
}

pub struct Gateway {
    client: Arc<TorClient<TokioRustlsRuntime>>,
    endpoint: SocksEndpoint,
    status: Arc<StatusCell>,
    tasks: Vec<JoinHandle<()>>,
}

struct TorConnector {
    client: Arc<TorClient<TokioRustlsRuntime>>,
    prefs: StreamPrefs,
}

impl Connector for TorConnector {
    type Stream = DataStream;
    fn connect(&self, host: String, port: u16) -> impl std::future::Future<Output = io::Result<DataStream>> + Send {
        let client = Arc::clone(&self.client);
        let prefs = self.prefs.clone();
        async move {
            client
                .connect_with_prefs((host.as_str(), port), &prefs)
                .await
                .map_err(|e| io::Error::other(e.to_string()))
        }
    }
}

impl Gateway {
    /// Starts Arti (bootstrapping in the background) and the SOCKS gate. Must
    /// be called inside a multi-threaded Tokio runtime, which keeps running the
    /// gateway after this returns. Returns as soon as the SOCKS port is open:
    /// until Tor is ready, requests through it simply wait or fail.
    pub async fn start(config: GatewayConfig) -> Result<Gateway, GatewayError> {
        // rustls needs a process-wide provider; a second call is a harmless no-op.
        let _ = rustls::crypto::ring::default_provider().install_default();

        let policy = TargetPolicy::parse(&config.allowed_targets)?;

        let mut builder = TorClientConfigBuilder::from_directories(&config.state_dir, &config.cache_dir);
        #[cfg(feature = "test-hooks")]
        if config.test_blocked_network {
            let mut fallback = arti_client::config::dir::FallbackDir::builder();
            fallback
                .rsa_identity([23; 20].into())
                .ed_identity([99; 32].into())
                .orports()
                .push("127.0.0.1:9".parse().expect("static address"));
            builder.tor_network().set_fallback_caches(vec![fallback]);
        }
        let tor_config = builder.build().map_err(|e| GatewayError::Config(e.to_string()))?;

        let runtime = TokioRustlsRuntime::current().map_err(|e| GatewayError::Tor(e.to_string()))?;
        let client = TorClient::with_runtime(runtime)
            .config(tor_config)
            .bootstrap_behavior(BootstrapBehavior::Manual)
            .create_unbootstrapped()
            .map_err(|e| GatewayError::Tor(e.to_string()))?;

        let status = Arc::new(StatusCell::default());
        let mut tasks = Vec::new();

        let mut events = client.bootstrap_events();
        let events_status = Arc::clone(&status);
        tasks.push(tokio::spawn(async move {
            while let Some(event) = events.next().await {
                // "Disabled" only means bootstrap() hasn't been called yet (it is, right below).
                let not_started = event
                    .blocked()
                    .is_some_and(|b| matches!(b.kind(), arti_client::status::BlockageKind::Disabled));
                if not_started {
                    continue;
                }
                let blocked = event.blocked().map(|b| b.to_string());
                events_status.on_bootstrap(event.as_frac(), event.ready_for_traffic(), event.to_string(), blocked);
            }
        }));

        let bootstrap_client = Arc::clone(&client);
        let bootstrap_status = Arc::clone(&status);
        tasks.push(tokio::spawn(async move {
            if let Err(error) = bootstrap_client.bootstrap().await {
                bootstrap_status.on_fatal(format!("Tor couldn't bootstrap: {error}"));
            }
        }));

        // Loopback only: the gate must never be reachable from the network.
        let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).await?;
        let local = listener.local_addr()?;
        if !local.ip().is_loopback() {
            return Err(GatewayError::Io(io::Error::other("the SOCKS gate must listen on loopback only")));
        }
        let port = local.port();
        let endpoint = SocksEndpoint {
            host: Ipv4Addr::LOCALHOST.to_string(),
            port,
            username: random_hex(16),
            password: random_hex(16),
        };

        let mut prefs = StreamPrefs::new();
        prefs.connect_to_onion_services(arti_client::config::BoolOrAuto::Explicit(true));
        let ctx = Arc::new(SocksContext {
            connector: TorConnector { client: Arc::clone(&client), prefs },
            policy,
            username: endpoint.username.clone(),
            password: endpoint.password.clone(),
            status: Arc::clone(&status),
        });
        tasks.push(tokio::spawn(socks::serve(listener, ctx)));

        Ok(Gateway { client, endpoint, status, tasks })
    }

    pub fn endpoint(&self) -> &SocksEndpoint {
        &self.endpoint
    }

    pub fn status(&self) -> Status {
        self.status.snapshot()
    }

    /// Dormant while the app is in the background with nothing to send
    /// (Android): background tasks pause, the next request wakes it.
    pub fn set_dormant(&self, dormant: bool) {
        self.client.set_dormant(if dormant { DormantMode::Soft } else { DormantMode::Normal });
    }

    /// Closes the SOCKS port and every open stream. Requests fail from here on.
    pub fn stop(mut self) {
        self.shutdown();
    }

    fn shutdown(&mut self) {
        for task in self.tasks.drain(..) {
            task.abort();
        }
        self.status.on_stopped();
    }
}

impl Drop for Gateway {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn random_hex(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    getrandom::fill(&mut buf).expect("the OS random number generator failed");
    buf.iter().map(|b| format!("{b:02x}")).collect()
}

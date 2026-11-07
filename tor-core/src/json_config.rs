//! What the C ABI and JNI share: a JSON config, and a handle that owns the
//! Tokio runtime the gateway runs on.

use std::path::PathBuf;
use std::sync::Once;
use std::time::Duration;

use serde::Deserialize;

use crate::gateway::{Gateway, GatewayConfig};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct JsonConfig {
    state_dir: PathBuf,
    cache_dir: PathBuf,
    allowed_targets: Vec<String>,
    /// "error" | "warn" | "info" | "debug": Arti's log (addresses redacted) on stderr.
    #[serde(default)]
    log: Option<String>,
    #[serde(default)]
    test_blocked_network: bool,
}

pub(crate) struct Handle {
    runtime: tokio::runtime::Runtime,
    gateway: Option<Gateway>,
}

static LOGGING: Once = Once::new();

impl Handle {
    pub(crate) fn start(json: &str) -> Result<Handle, String> {
        let parsed: JsonConfig = serde_json::from_str(json).map_err(|e| format!("bad config: {e}"))?;
        if let Some(level) = parsed.log.clone() {
            LOGGING.call_once(|| {
                let _ = tracing_subscriber::fmt()
                    .with_env_filter(tracing_subscriber::EnvFilter::new(level))
                    .with_writer(std::io::stderr)
                    .with_ansi(false)
                    .try_init();
            });
        }
        #[cfg(not(feature = "test-hooks"))]
        if parsed.test_blocked_network {
            return Err("testBlockedNetwork needs a build with the test-hooks feature".to_owned());
        }
        let config = GatewayConfig {
            state_dir: parsed.state_dir,
            cache_dir: parsed.cache_dir,
            allowed_targets: parsed.allowed_targets,
            #[cfg(feature = "test-hooks")]
            test_blocked_network: parsed.test_blocked_network,
        };
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("redvault-tor")
            .enable_all()
            .build()
            .map_err(|e| format!("couldn't start the Tor runtime: {e}"))?;
        let gateway = runtime.block_on(Gateway::start(config)).map_err(|e| e.to_string())?;
        Ok(Handle { runtime, gateway: Some(gateway) })
    }

    pub(crate) fn endpoint_json(&self) -> String {
        match &self.gateway {
            Some(gateway) => serde_json::to_string(gateway.endpoint()).unwrap_or_default(),
            None => "null".to_owned(),
        }
    }

    pub(crate) fn status_json(&self) -> String {
        match &self.gateway {
            Some(gateway) => serde_json::to_string(&gateway.status()).unwrap_or_default(),
            None => r#"{"state":"unavailable","progress":0,"detail":"Tor is stopped"}"#.to_owned(),
        }
    }

    pub(crate) fn set_dormant(&self, dormant: bool) {
        if let Some(gateway) = &self.gateway {
            let _guard = self.runtime.enter();
            gateway.set_dormant(dormant);
        }
    }

    pub(crate) fn stop(mut self) {
        if let Some(gateway) = self.gateway.take() {
            let _guard = self.runtime.enter();
            gateway.stop();
        }
        self.runtime.shutdown_timeout(Duration::from_secs(3));
    }
}

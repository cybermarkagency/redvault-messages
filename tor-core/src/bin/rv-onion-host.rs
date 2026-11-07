//! Test only: publishes a throwaway v3 onion service (Arti) whose port 80
//! forwards to a local TCP port, so the gateway can be tested end to end over
//! the real Tor network without touching the production server.
//!
//! Production should host the onion service with C tor on the server (see
//! docs/tor-routing-plan.md); this exists so tests need nothing installed.
//!
//! rv-onion-host --state DIR --cache DIR --forward PORT [--address-file FILE]

use std::net::Ipv4Addr;
use std::path::PathBuf;
use std::str::FromStr;

use arti_client::config::TorClientConfigBuilder;
use arti_client::TorClient;
use futures::StreamExt;
use tokio::net::TcpStream;
use tor_cell::relaycell::msg::Connected;
use tor_hsservice::config::OnionServiceConfigBuilder;
use tor_hsservice::{HsNickname, StreamRequest};
use tor_proto::stream::IncomingStreamRequest;
use tor_rtcompat::tokio::TokioRustlsRuntime;

struct Args {
    state: PathBuf,
    cache: PathBuf,
    forward: u16,
    address_file: Option<PathBuf>,
}

fn parse_args() -> Result<Args, String> {
    let mut state = None;
    let mut cache = None;
    let mut forward = None;
    let mut address_file = None;
    let mut it = std::env::args().skip(1);
    while let Some(flag) = it.next() {
        let value = it.next().ok_or_else(|| format!("{flag} needs a value"))?;
        match flag.as_str() {
            "--state" => state = Some(PathBuf::from(value)),
            "--cache" => cache = Some(PathBuf::from(value)),
            "--forward" => forward = Some(value.parse::<u16>().map_err(|e| e.to_string())?),
            "--address-file" => address_file = Some(PathBuf::from(value)),
            other => return Err(format!("unknown flag {other}")),
        }
    }
    Ok(Args {
        state: state.ok_or("--state is required")?,
        cache: cache.ok_or("--cache is required")?,
        forward: forward.ok_or("--forward is required")?,
        address_file,
    })
}

async fn forward(request: StreamRequest, port: u16) {
    let begin_port = match request.request() {
        IncomingStreamRequest::Begin(begin) => begin.port(),
        _ => {
            let _ = request.shutdown_circuit();
            return;
        }
    };
    if begin_port != 80 {
        let _ = request.shutdown_circuit();
        return;
    }
    let Ok(mut local) = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).await else {
        let _ = request.shutdown_circuit();
        return;
    };
    let Ok(mut onion) = request.accept(Connected::new_empty()).await else {
        return;
    };
    let _ = tokio::io::copy_bidirectional(&mut onion, &mut local).await;
}

#[tokio::main(flavor = "multi_thread", worker_threads = 2)]
async fn main() {
    let args = match parse_args() {
        Ok(args) => args,
        Err(message) => {
            eprintln!("rv-onion-host: {message}");
            std::process::exit(2);
        }
    };
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::new(
            std::env::var("RV_TOR_LOG").unwrap_or_else(|_| "warn".to_owned()),
        ))
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .init();
    let _ = rustls::crypto::ring::default_provider().install_default();

    let config = TorClientConfigBuilder::from_directories(&args.state, &args.cache)
        .build()
        .expect("tor config");
    let runtime = TokioRustlsRuntime::current().expect("tokio runtime");
    let client = TorClient::with_runtime(runtime)
        .config(config)
        .create_bootstrapped()
        .await
        .expect("bootstrap");
    eprintln!("rv-onion-host: Tor bootstrapped");

    let nickname = HsNickname::from_str("redvault-poc").expect("nickname");
    let service_config = OnionServiceConfigBuilder::default()
        .nickname(nickname)
        .build()
        .expect("onion service config");
    let (service, requests) = client
        .launch_onion_service(service_config)
        .expect("launch onion service")
        .expect("service enabled");
    let address = service
        .onion_address()
        .map(|id| safelog::DisplayRedacted::display_unredacted(&id).to_string())
        .expect("onion address");
    println!("{address}");
    if let Some(file) = &args.address_file {
        std::fs::write(file, &address).expect("write address file");
    }

    let mut status_events = service.status_events();
    tokio::spawn(async move {
        while let Some(status) = status_events.next().await {
            eprintln!("rv-onion-host: service state {:?}", status.state());
        }
    });

    let mut streams = tor_hsservice::handle_rend_requests(requests);
    while let Some(request) = streams.next().await {
        tokio::spawn(forward(request, args.forward));
    }
}

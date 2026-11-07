// Interop check between the desktop call engine and a real browser.
//
// Serves a page on 127.0.0.1 that runs the same WebRTC steps as the web app
// (ui/src/state/CallContext.tsx: full gathering, then one offer and one
// answer) with the browser's fake microphone and camera. Open it in a
// browser started with --use-fake-device-for-media-stream
// --use-fake-ui-for-media-stream; the page and this tool then exchange real
// audio and video both ways and report what each received.
//
//   redvault_rtc_interop [browser-calls|desktop-calls] [port]
//
// Exits 0 when both directions carried audio and video.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>

#include "redvault/crypto/call_crypto.hpp"
#include "redvault/rtc/call_media.hpp"

using namespace redvault::rtc;

namespace {

const char* kPage = R"HTML(<!doctype html><html><body><pre id=log></pre><script>
const log = (m) => { document.getElementById('log').textContent += m + '\n'; };
const mode = new URLSearchParams(location.search).get('mode');
function gathered(pc) {
  return new Promise((resolve) => {
    if (pc.iceGatheringState === 'complete') return resolve();
    pc.addEventListener('icegatheringstatechange', () => pc.iceGatheringState === 'complete' && resolve());
    setTimeout(resolve, 6000);
  });
}
async function stats(pc) {
  const out = { audioPackets: 0, audioEnergy: 0, videoFrames: 0, videoWidth: 0, state: pc.connectionState };
  (await pc.getStats()).forEach((r) => {
    if (r.type === 'inbound-rtp' && r.kind === 'audio') { out.audioPackets = r.packetsReceived; out.audioEnergy = r.totalAudioEnergy || 0; }
    if (r.type === 'inbound-rtp' && r.kind === 'video') { out.videoFrames = r.framesDecoded || 0; out.videoWidth = r.frameWidth || 0; }
  });
  return out;
}
(async () => {
  try {
    const media = await navigator.mediaDevices.getUserMedia({ audio: true, video: { width: 640, height: 480 } });
    const [mic] = media.getAudioTracks(); const [camera] = media.getVideoTracks();
    const pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });
    pc.onconnectionstatechange = () => log('state ' + pc.connectionState);
    const remote = new MediaStream();
    const player = document.createElement('audio');
    player.autoplay = true;
    document.body.appendChild(player);
    pc.ontrack = (e) => { remote.addTrack(e.track); player.srcObject = remote; player.play().catch(() => {}); };
    if (mode === 'browser-calls') {
      pc.addTransceiver(mic, { direction: 'sendrecv', streams: [media] });
      pc.addTransceiver(camera, { direction: 'sendrecv', streams: [media] });
      await pc.setLocalDescription(await pc.createOffer());
      await gathered(pc);
      const answer = await (await fetch('/offer', { method: 'POST', body: pc.localDescription.sdp })).text();
      await pc.setRemoteDescription({ type: 'answer', sdp: answer });
    } else {
      const offer = await (await fetch('/offer')).text();
      await pc.setRemoteDescription({ type: 'offer', sdp: offer });
      for (const t of pc.getTransceivers()) {
        t.direction = 'sendrecv';
        await t.sender.replaceTrack(t.receiver.track.kind === 'audio' ? mic : camera);
      }
      await pc.setLocalDescription(await pc.createAnswer());
      await gathered(pc);
      await fetch('/answer', { method: 'POST', body: pc.localDescription.sdp });
    }
    await new Promise((r) => setTimeout(r, 7000));
    await fetch('/stats', { method: 'POST', body: JSON.stringify(await stats(pc)) });
  } catch (e) {
    await fetch('/stats', { method: 'POST', body: JSON.stringify({ error: String(e) }) });
  }
})();
</script></body></html>)HTML";

struct Shared {
    std::mutex m;
    std::condition_variable cv;
    std::string offerForBrowser;
    std::string answerFromBrowser;
    std::string stats;
};

#ifdef _WIN32
using Socket = SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
void closeSocket(Socket s) { ::close(s); }
#endif

std::string readRequest(Socket s, std::string& method, std::string& path) {
    std::string data;
    char buf[65536];
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) return {};
        data.append(buf, static_cast<std::size_t>(n));
        headerEnd = data.find("\r\n\r\n");
    }
    const auto sp1 = data.find(' ');
    const auto sp2 = data.find(' ', sp1 + 1);
    method = data.substr(0, sp1);
    path = data.substr(sp1 + 1, sp2 - sp1 - 1);
    std::size_t length = 0;
    if (const auto cl = data.find("Content-Length: "); cl != std::string::npos) length = std::stoul(data.substr(cl + 16));
    else if (const auto cl2 = data.find("content-length: "); cl2 != std::string::npos) length = std::stoul(data.substr(cl2 + 16));
    std::string body = data.substr(headerEnd + 4);
    while (body.size() < length) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        body.append(buf, static_cast<std::size_t>(n));
    }
    return body;
}

void respond(Socket s, const std::string& type, const std::string& body) {
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n";
    send(s, head.data(), static_cast<int>(head.size()), 0);
    send(s, body.data(), static_cast<int>(body.size()), 0);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "browser-calls";
    const int port = argc > 2 ? std::stoi(argv[2]) : 8931;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::atomic<int> framesFromBrowser{0};
    std::atomic<int> widthFromBrowser{0};
    std::atomic<bool> connected{false};
    Config config;
    config.relayOnly = false;
    CallMedia media(config, Callbacks{[&](ConnectionState s) {
                                          if (s == ConnectionState::Connected) connected = true;
                                          std::cerr << "desktop state " << static_cast<int>(s) << "\n";
                                      },
                                      [&](const VideoFrame& f) {
                                          widthFromBrowser = f.width;
                                          ++framesFromBrowser;
                                      }});

    Shared shared;
    if (mode == "desktop-calls") shared.offerForBrowser = media.createOffer();

    Socket server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(server, 8) != 0) {
        std::cerr << "can't listen on " << port << "\n";
        return 2;
    }
    std::cerr << "open http://127.0.0.1:" << port << "/?mode=" << mode << "\n";

    std::thread http([&] {
        for (;;) {
            Socket c = accept(server, nullptr, nullptr);
            if (c == static_cast<Socket>(-1)) return;
            std::string method, path;
            const std::string body = readRequest(c, method, path);
            if (path.rfind("/?", 0) == 0 || path == "/") {
                respond(c, "text/html", kPage);
            } else if (path == "/offer" && method == "GET") {
                respond(c, "text/plain", shared.offerForBrowser);
            } else if (path == "/offer" && method == "POST") {
                std::string answer;
                try {
                    answer = media.acceptOffer(body);
                } catch (const std::exception& e) {
                    std::cerr << "acceptOffer failed: " << e.what() << "\n";
                }
                respond(c, "text/plain", answer);
            } else if (path == "/answer" && method == "POST") {
                media.setAnswer(body);
                respond(c, "text/plain", "ok");
            } else if (path == "/stats" && method == "POST") {
                respond(c, "text/plain", "ok");
                std::lock_guard lock(shared.m);
                shared.stats = body;
                shared.cv.notify_all();
            } else {
                respond(c, "text/plain", "");
            }
            closeSocket(c);
        }
    });
    http.detach();

    // Send a tone and a moving picture for as long as the browser listens.
    std::thread sender([&] {
        std::vector<std::int16_t> pcm(kFrameSamples);
        int frame = 0;
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(40)) {
            if (connected) {
                for (int i = 0; i < kFrameSamples; ++i) {
                    const double t = (frame * kFrameSamples + i) / static_cast<double>(kSampleRate);
                    pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(9000 * std::sin(2 * std::numbers::pi * 440 * t));
                }
                media.sendAudio(pcm.data());
                if (frame % 3 == 0) {
                    VideoFrame v = VideoFrame::blank(640, 360);
                    for (int y = 0; y < 360; ++y)
                        for (int x = 0; x < 640; ++x) v.y()[y * 640 + x] = static_cast<std::uint8_t>((x + frame * 3) & 0xFF);
                    media.sendVideo(v);
                }
                ++frame;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    std::unique_lock lock(shared.m);
    const bool gotStats = shared.cv.wait_for(lock, std::chrono::seconds(45), [&] { return !shared.stats.empty(); });
    lock.unlock();
    std::vector<std::int16_t> heard(kFrameSamples * 5);
    media.readAudio(heard.data(), heard.size());
    std::cout << "browser saw: " << (gotStats ? shared.stats : "(nothing)") << "\n";
    std::cout << "desktop saw: audioPackets=" << media.audioPacketsReceived() << " videoFrames=" << framesFromBrowser
              << " videoWidth=" << widthFromBrowser << "\n";
    std::cout << "sas: " << redvault::crypto::calls::sdpFingerprint(media.localSdp()) << "\n";
    const bool ok = gotStats && shared.stats.find("\"error\"") == std::string::npos && shared.stats.find("\"audioPackets\":0") == std::string::npos &&
                    shared.stats.find("\"videoFrames\":0,") == std::string::npos && media.audioPacketsReceived() > 50 && framesFromBrowser > 10;
    std::cout << (ok ? "INTEROP OK" : "INTEROP FAILED") << "\n";
    media.close();
    sender.detach();
    std::_Exit(ok ? 0 : 1);
}

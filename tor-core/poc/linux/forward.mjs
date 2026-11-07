// Forwards one TCP port on the WSL virtual-network address of this Windows PC to a loopback port, so a program inside
// WSL can reach the test harness's control port (WSL2 can't reach the Windows loopback). Test use only: it listens on
// the WSL-only address, never on 0.0.0.0.
//   node forward.mjs <listen-ip> <listen-port> <target-port>
import net from "node:net";
const [listenIp, listenPort, targetPort] = process.argv.slice(2);
net
  .createServer((client) => {
    const upstream = net.connect(Number(targetPort), "127.0.0.1");
    client.pipe(upstream).pipe(client);
    const close = () => { client.destroy(); upstream.destroy(); };
    client.on("error", close); upstream.on("error", close);
  })
  .listen(Number(listenPort), listenIp, () => console.log(`forwarding ${listenIp}:${listenPort} -> 127.0.0.1:${targetPort}`));

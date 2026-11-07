package com.redvaultsystems.tor;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Authenticator;
import java.net.InetSocketAddress;
import java.net.PasswordAuthentication;
import java.net.Proxy;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Android test probe (run with app_process): starts the gateway through JNI,
 * waits for Tor, then makes HTTP requests through the SOCKS gate exactly the
 * way OkHttp does with a SOCKS proxy (java.net.Socket(Proxy), an unresolved
 * address, credentials from java.net.Authenticator). Also checks the two
 * refusals an app relies on: no credentials, and a clearnet host.
 *
 * Probe <state dir> <cache dir> <onion host> <path>...
 * One JSON line per step on stdout.
 */
public final class Probe {
    private static String field(String json, String name) {
        Matcher m = Pattern.compile("\"" + name + "\":(\"([^\"]*)\"|([0-9]+))").matcher(json);
        if (!m.find()) return null;
        return m.group(2) != null ? m.group(2) : m.group(3);
    }

    private static String get(Proxy proxy, String host, String path) throws Exception {
        try (Socket socket = new Socket(proxy)) {
            socket.setSoTimeout(120_000);
            // Unresolved: the hostname goes to the gate, never to Android's resolver.
            socket.connect(InetSocketAddress.createUnresolved(host, 80), 120_000);
            OutputStream out = socket.getOutputStream();
            out.write(("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n")
                    .getBytes(StandardCharsets.US_ASCII));
            out.flush();
            InputStream in = socket.getInputStream();
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) body.write(buf, 0, n);
            String response = body.toString("UTF-8");
            return response.isEmpty() ? "empty" : response.substring(0, response.indexOf('\r') > 0 ? response.indexOf('\r') : response.length());
        }
    }

    private static void line(String step, boolean ok, String detail, long began) {
        System.out.println("{\"step\":\"" + step + "\",\"ok\":" + ok + ",\"ms\":" + (System.currentTimeMillis() - began)
                + ",\"detail\":\"" + detail.replace("\\", "\\\\").replace("\"", "'") + "\"}");
        System.out.flush();
    }

    public static void main(String[] args) throws Exception {
        NativeTor.load();
        String config = "{\"stateDir\":\"" + args[0] + "\",\"cacheDir\":\"" + args[1]
                + "\",\"allowedTargets\":[\"" + args[2] + ":80\"]}";
        long began = System.currentTimeMillis();
        long handle = NativeTor.nativeStart(config);
        String endpoint = NativeTor.nativeEndpoint(handle);
        final int port = Integer.parseInt(field(endpoint, "port"));
        final String user = field(endpoint, "username");
        final String pass = field(endpoint, "password");
        line("tor_start", true, "socks port " + port, began);

        began = System.currentTimeMillis();
        String last = "";
        StringBuilder transitions = new StringBuilder();
        String state = "";
        while (System.currentTimeMillis() - began < 240_000) {
            String status = NativeTor.nativeStatus(handle);
            state = field(status, "state");
            String key = state + " " + field(status, "progress");
            if (!key.equals(last)) {
                transitions.append(key).append(", ");
                last = key;
            }
            if ("connected".equals(state)) break;
            Thread.sleep(250);
        }
        line("tor_connected", "connected".equals(state), transitions.toString(), began);

        Proxy proxy = new Proxy(Proxy.Type.SOCKS, new InetSocketAddress("127.0.0.1", port));

        // Without credentials the gate refuses (other apps on the phone can't borrow it).
        began = System.currentTimeMillis();
        Authenticator.setDefault(null);
        try {
            get(proxy, args[2], "/health");
            line("refuse_without_credentials", false, "request went through", began);
        } catch (Exception e) {
            line("refuse_without_credentials", true, e.toString(), began);
        }

        // What the app does: answer the gate's SOCKS5 password request, only for the gate itself.
        Authenticator.setDefault(new Authenticator() {
            @Override
            protected PasswordAuthentication getPasswordAuthentication() {
                if (getRequestingPort() == port && "SOCKS5".equals(getRequestingProtocol())) {
                    return new PasswordAuthentication(user, pass.toCharArray());
                }
                return null;
            }
        });

        boolean allOk = true;
        for (int i = 3; i < args.length; i++) {
            began = System.currentTimeMillis();
            try {
                String statusLine = get(proxy, args[2], args[i]);
                line("get " + args[i], statusLine.startsWith("HTTP/1.1"), statusLine, began);
            } catch (Exception e) {
                allOk = false;
                line("get " + args[i], false, e.toString(), began);
            }
        }

        began = System.currentTimeMillis();
        try {
            get(proxy, "www.redvaultsystems.com", "/");
            line("refuse_clearnet_host", false, "clearnet request went through", began);
        } catch (Exception e) {
            line("refuse_clearnet_host", true, e.toString(), began);
        }

        String finalStatus = NativeTor.nativeStatus(handle);
        NativeTor.nativeStop(handle);
        began = System.currentTimeMillis();
        try {
            get(proxy, args[2], "/health");
            line("fail_closed_after_stop", false, "request went through after stop", began);
        } catch (Exception e) {
            line("fail_closed_after_stop", true, e.toString(), began);
        }
        System.out.println(finalStatus);
        System.exit(allOk ? 0 : 1);
    }
}

package com.redvaultsystems.e2ee;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

/**
 * Android test driver: one JSON request per line on stdin, one response per line
 * on stdout, through the JNI binding (run on a device with app_process, or on a
 * desktop JVM).
 */
public final class Driver {
    public static void main(String[] args) throws Exception {
        NativeE2ee.load();
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        PrintWriter out = new PrintWriter(new OutputStreamWriter(System.out, StandardCharsets.UTF_8), true);
        String line;
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty()) continue;
            out.println(NativeE2ee.call(line));
        }
    }
}

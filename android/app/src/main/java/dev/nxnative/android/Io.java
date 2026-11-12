package dev.nxnative.android;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;

/**
 * The whole-of-a-stream reads the launcher needs, without the Java 9 methods Android's libcore only grew in API 33 (`InputStream.readAllBytes`, `readNBytes`,
 * `transferTo`): the launcher runs on Android 12 (SDK 31), where those throw NoSuchMethodError - the GPU driver fetch was the one a user hit ("no virtual method
 * readAllBytes() ... in class Ljava/io/InputStream", shown as "GPU driver not installed").
 */
final class Io {
    private Io() {}

    /** Every byte of the stream. */
    static byte[] all(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[1 << 16];
        for (int n; (n = in.read(b)) > 0;) out.write(b, 0, n);
        return out.toByteArray();
    }

    /** The first n bytes, or fewer at the end of the stream. */
    static byte[] upTo(InputStream in, int n) throws IOException {
        byte[] out = new byte[n];
        int got = 0;
        while (got < n) {
            int k = in.read(out, got, n - got);
            if (k < 0) break;
            got += k;
        }
        return got == n ? out : Arrays.copyOf(out, got);
    }
}

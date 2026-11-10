package dev.nxnative.android;

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/**
 * AdbControl on the JVM, driven by dm/tools/tests/test_android_adbcontrol.py. Commands, one per run:
 * <pre>
 * allowed &lt;play 0|1&gt; &lt;setting 0|1&gt;  → "allowed true|false"
 * asked [&lt;extra&gt;...]                 → "asked &lt;the control extras among them, comma-separated&gt;"
 * extras                              → "extras &lt;AdbControl.EXTRAS, comma-separated&gt;"
 * </pre>
 */
public final class AdbControlJvmTest {
    public static void main(String[] a) {
        switch (a[0]) {
            case "allowed": System.out.println("allowed " + AdbControl.allowed(a[1].equals("1"), a[2].equals("1"))); break;
            case "asked": {
                Set<String> has = new HashSet<>(Arrays.asList(a).subList(1, a.length));
                System.out.println("asked " + String.join(",", AdbControl.asked(has::contains)));
                break;
            }
            case "extras": System.out.println("extras " + String.join(",", AdbControl.EXTRAS)); break;
            default: throw new IllegalArgumentException(a[0]);
        }
    }
}

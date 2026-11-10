package dev.nxnative.android;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Which gamepad is player 1 (GameActivity's input devices; pure Java, so PadOrderJvmTest checks it on the JVM). Android reports more than controllers as gamepads:
 * a phone's own input devices (Xiaomi and POCO phones have "uinput-xiaomi", which is neither virtual nor silent about gamepad sources) took player 1 by connecting
 * first, and a game that reads only player 1 ignored the real controller on player 2 (issues #42, #62). What Android reports decides, never the device's name:
 * <ul>
 * <li>A device's rank (rank()): the device chosen as player 1 (the in-game panel's Player 1, by its descriptor); then an external device (USB, Bluetooth:
 *     InputDevice.isExternal) with gamepad buttons and a stick; another external gamepad; a built-in one with gamepad buttons, a stick and a vendor ID; the rest.</li>
 * <li>External devices and the chosen one get a player when they connect, best rank first (at the start the order of ids within a rank). The others get one only
 *     from their first input (a gamepad button, or a stick or trigger pushed past half: wakes()), so a device that never sends any takes no player and does not hide
 *     the on-screen controls. A built-in controller (a handheld's) gets its player from its first press.</li>
 * <li>A device that gets a player takes player 1 when player 1 is free or held by a device of a worse rank (Host.first: they trade players); one that gets it by
 *     its input also when player 1's device is of the same rank and no controller (BUILT_IN, OTHER): the device the player uses wins over one that woke by itself.</li>
 * </ul>
 */
final class PadOrder {
    /** What Android reports of an input device (InputDevice): its id, descriptor (stable across reconnects and restarts), name, isExternal, gamepad buttons plus X / Y axes, vendor ID */
    static final class Pad {
        final int id; final String key, name; final boolean external, full; final int vendor;
        Pad(int id, String key, String name, boolean external, boolean full, int vendor) {
            this.id = id; this.key = key == null ? "" : key; this.name = name == null ? "" : name; this.external = external; this.full = full; this.vendor = vendor;
        }
    }

    /** The host's players (Native.deviceAdded / deviceFirst / devicePlayer) */
    interface Host {
        int add(Pad p);     // the player it gets (0..7), -1 with every player taken
        int first(Pad p);   // it becomes player 1 (player 1's device takes its old player): 0, or -1
        int player(int id); // -1: none
    }

    static final int CHOSEN = 0, CONTROLLER = 1, EXTERNAL = 2, BUILT_IN = 3, OTHER = 4;

    static int rank(Pad p, String chosen) {
        if (chosen != null && !chosen.isEmpty() && chosen.equals(p.key)) return CHOSEN;
        if (p.external) return p.full ? CONTROLLER : EXTERNAL;
        return p.full && p.vendor != 0 ? BUILT_IN : OTHER;
    }

    /** Gets a player when it connects (else from its first input) */
    static boolean atConnect(Pad p, String chosen) { return rank(p, chosen) <= EXTERNAL; }

    /** One joystick event's values: true when one is pushed past half (a stick, trigger or hat), not a resting stick's noise */
    static boolean wakes(float[] values, int n) {
        for (int i = 0; i < n; i++) if (Math.abs(values[i]) > 0.5f) return true;
        return false;
    }

    private final Host host;
    private final Map<Integer, Pad> pads = new LinkedHashMap<>();   // gamepad-like devices connected
    private final Set<Integer> playing = new LinkedHashSet<>();   // those that have a player
    private String chosen;

    PadOrder(Host host, String chosen) { this.host = host; this.chosen = chosen == null ? "" : chosen; }

    /** The devices connected at the start: best rank first, then by id */
    void start(List<Pad> list) {
        List<Pad> l = new ArrayList<>(list);
        l.sort(Comparator.<Pad>comparingInt(p -> rank(p, chosen)).thenComparingInt(p -> p.id));
        for (Pad p : l) connected(p);
    }

    /** A gamepad-like device connected; true when it has a player now */
    boolean connected(Pad p) {
        if (pads.containsKey(p.id) && playing.contains(p.id)) return true;
        pads.put(p.id, p);
        return atConnect(p, chosen) && join(p, false);
    }

    void removed(int id) { pads.remove(id); playing.remove(id); }

    /** Whether the device's events go to the host: it has a player */
    boolean plays(int id) { return playing.contains(id); }

    /** Input from a gamepad-like device (a pad button, or wakes()): it gets a player if it had none. True when it has one */
    boolean active(Pad p) {
        if (playing.contains(p.id)) return true;
        pads.put(p.id, p);
        return join(p, true);
    }

    private boolean join(Pad p, boolean byInput) {
        if (host.add(p) < 0) return false;
        playing.add(p.id);
        takeFirst(p, byInput);
        return true;
    }

    /** Player 1 for p when it is free or its device ranks worse; joining by its input, also when player 1's device is no controller (BUILT_IN or OTHER) */
    private void takeFirst(Pad p, boolean byInput) {
        if (host.player(p.id) == 0) return;
        Pad one = holderOfFirst();
        int r1 = one == null ? Integer.MAX_VALUE : rank(one, chosen), r = rank(p, chosen);
        if (r1 > r || byInput && r1 >= BUILT_IN && r1 == r) host.first(p);
    }

    private Pad holderOfFirst() {
        for (int id : playing) if (host.player(id) == 0) return pads.get(id);
        return null;
    }

    /** The gamepad-like devices connected, for the Player 1 choice */
    List<Pad> known() { return new ArrayList<>(pads.values()); }

    /** The device on player 1, null for none */
    Pad first() { return holderOfFirst(); }

    /** The Player 1 choice: a descriptor ("" = automatic). The chosen device, if it is connected, becomes player 1; automatic gives it to the best-ranked one */
    void choose(String key) {
        chosen = key == null ? "" : key;
        Pad best = null;
        for (Pad p : pads.values()) {
            if (!playing.contains(p.id) && !atConnect(p, chosen)) continue;
            if (best == null || rank(p, chosen) < rank(best, chosen)) best = p;
        }
        if (best == null) return;
        if (!playing.contains(best.id)) join(best, false); else takeFirst(best, false);
    }
}

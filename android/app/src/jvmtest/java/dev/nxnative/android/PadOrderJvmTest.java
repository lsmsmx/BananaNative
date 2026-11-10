package dev.nxnative.android;

import java.util.Arrays;

/**
 * PadOrder on the JVM (dm/tools/tests/test_android_padorder.py) against a stand-in for the host's players (pad_slots.h: the lowest free player on connecting,
 * pad_slot_first trading player 1). Devices as a POCO F7 Ultra with a GameSir X3 Pro (issues #42, #62; the ids from its log, uinput-xiaomi's other facts
 * assumed both ways), a handheld's built-in pad and others. Prints "ok" or throws
 */
public final class PadOrderJvmTest {
    private static void check(boolean c, String what) { if (!c) throw new AssertionError(what); }

    /** The host's players: owner[p] is the device id on player p + 1, -1 for none */
    static final class Slots implements PadOrder.Host {
        final int[] owner = new int[8];
        int firsts;
        Slots() { Arrays.fill(owner, -1); }
        @Override public int add(PadOrder.Pad p) {
            int had = player(p.id); if (had >= 0) return had;
            for (int i = 0; i < owner.length; i++) if (owner[i] < 0) { owner[i] = p.id; return i; }
            return -1;
        }
        @Override public int first(PadOrder.Pad p) {
            int at = add(p); if (at < 0) return -1;
            firsts++;
            owner[at] = owner[0]; owner[0] = p.id;
            return 0;
        }
        @Override public int player(int id) { for (int i = 0; i < owner.length; i++) if (owner[i] == id) return i; return -1; }
    }

    static final PadOrder.Pad XIAOMI = new PadOrder.Pad(3, "d-uinput", "uinput-xiaomi", false, false, 0),            // the phone's own: built in, no vendor
            XIAOMI_FULL = new PadOrder.Pad(3, "d-uinput", "uinput-xiaomi", false, true, 0),                            // the same, should it report buttons and a stick
            GAMESIR = new PadOrder.Pad(22, "d-gamesir", "Sony Computer Entertainment GameSir-X3 Pro", true, true, 0x054c),
            XBOX = new PadOrder.Pad(30, "d-xbox", "Xbox Wireless Controller", true, true, 0x045e),
            BUILT_IN = new PadOrder.Pad(5, "d-builtin", "Handheld Controller", false, true, 0x2020),
            KEYBOARD = new PadOrder.Pad(40, "d-kbd", "Bluetooth keyboard", true, false, 0x05ac);

    public static void main(String[] args) {
        check(PadOrder.rank(GAMESIR, "") == PadOrder.CONTROLLER && PadOrder.rank(XIAOMI, "") == PadOrder.OTHER && PadOrder.rank(XIAOMI_FULL, "") == PadOrder.OTHER, "ranks");
        check(PadOrder.rank(BUILT_IN, "") == PadOrder.BUILT_IN && PadOrder.rank(KEYBOARD, "") == PadOrder.EXTERNAL && PadOrder.rank(XIAOMI, "d-uinput") == PadOrder.CHOSEN, "ranks 2");
        check(!PadOrder.wakes(new float[]{0.1f, -0.2f, 0.5f}, 3) && PadOrder.wakes(new float[]{0f, -0.8f}, 2) && !PadOrder.wakes(new float[]{0f, 0.9f}, 1), "wakes");

        // #42 / #62: the phone's device has the lower id and was player 1; now the controller is, and the phone's device has no player until it sends input
        Slots h = new Slots(); PadOrder o = new PadOrder(h, "");
        o.start(Arrays.asList(XIAOMI, GAMESIR));
        check(h.player(22) == 0 && h.player(3) == -1 && !o.plays(3) && o.plays(22), "the controller is player 1, the phone's device none");
        check(o.first() == GAMESIR && o.known().size() == 2, "first, known");
        check(o.active(XIAOMI) && h.player(3) == 1 && h.player(22) == 0, "the phone's device sending input: player 2, the controller stays player 1");

        h = new Slots(); o = new PadOrder(h, "");   // the same if it reports buttons and a stick
        o.start(Arrays.asList(XIAOMI_FULL, GAMESIR));
        check(h.player(22) == 0 && h.player(3) == -1, "full uinput: the controller is player 1");

        // No controller: the phone's device woke, then a controller connects: it takes player 1
        h = new Slots(); o = new PadOrder(h, "");
        o.start(Arrays.asList(XIAOMI));
        check(h.player(3) == -1, "alone, silent: no player");
        o.active(XIAOMI);
        check(h.player(3) == 0, "alone, input: player 1");
        o.connected(GAMESIR);
        check(h.player(22) == 0 && h.player(3) == 1, "a controller connecting takes player 1 from it");
        o.connected(XBOX);
        check(h.player(30) == 2 && h.player(22) == 0, "a second controller is player 3, not player 1");
        o.removed(22); h.owner[0] = -1;   // the host frees its player (nxhost_input_device_removed)
        o.connected(GAMESIR);
        check(h.player(22) == 0, "back: player 1 free again");

        // Two external controllers at the start: the order of their ids
        h = new Slots(); o = new PadOrder(h, "");
        o.start(Arrays.asList(XBOX, KEYBOARD, GAMESIR, XIAOMI));
        check(h.player(22) == 0 && h.player(30) == 1 && h.player(40) == 2 && h.player(3) == -1, "controllers by id, then other external devices");
        check(h.firsts == 0, "nothing traded at the start in rank order");

        // A handheld: its built-in pad gets player 1 from its first press, also over the phone's device that woke first
        h = new Slots(); o = new PadOrder(h, "");
        o.start(Arrays.asList(BUILT_IN, XIAOMI));
        check(h.player(5) == -1 && h.player(3) == -1, "built in: no player before input");
        o.active(XIAOMI); o.active(BUILT_IN);
        check(h.player(5) == 0 && h.player(3) == 1, "the built-in pad takes player 1 from the phone's device");
        o.connected(GAMESIR);
        check(h.player(22) == 0, "an external controller still comes first");

        // Two devices that are no controller: the one used second takes player 1 (no controller on player 1)
        PadOrder.Pad other = new PadOrder.Pad(9, "d-other", "Other", false, false, 0);
        h = new Slots(); o = new PadOrder(h, "");
        o.start(Arrays.asList(XIAOMI, other));
        o.active(XIAOMI); o.active(other);
        check(h.player(9) == 0 && h.player(3) == 1, "used by the player: player 1");

        // The Player 1 choice: by descriptor, at the start and at once; automatic again leaves a controller where it is
        h = new Slots(); o = new PadOrder(h, "d-xbox");
        o.start(Arrays.asList(GAMESIR, XBOX));
        check(h.player(30) == 0 && h.player(22) == 1, "chosen: player 1 at the start");
        o.choose("d-gamesir");
        check(h.player(22) == 0 && h.player(30) == 1, "chosen now: they trade");
        o.choose("");
        check(h.player(22) == 0, "automatic: two controllers, nothing moves");
        o.choose("d-uinput");   // not connected: nothing changes
        check(h.player(22) == 0, "a chosen device not connected");
        o.connected(XIAOMI);
        check(h.player(3) == 0 && h.player(22) == 2, "the chosen device connects: player 1, even built in");
        o.choose("");
        check(h.player(22) == 0 || h.player(30) == 0, "automatic again: a controller on player 1");

        h = new Slots(); o = new PadOrder(h, "");   // choosing a silent device gives it a player
        o.start(Arrays.asList(XIAOMI, GAMESIR));
        o.choose("d-uinput");
        check(h.player(3) == 0 && h.player(22) == 1 && o.plays(3), "a silent device chosen: player 1");
        System.out.println("ok");
    }
}

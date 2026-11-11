# Donkey Kong Bananza's own code, keyed by the NSO build ID (the rewriter writes these into the pack's main).
CUTS = {
    # v458752: the game draws its scene at 80 % of the window and scales the rest up itself. At a scene's start it sets its frame buffer scale to a base (1.0)
    # times a per-mode table entry (0.8 here), and the render targets and the upscaler's target are the window times that scale (main+0x790b0). With the two
    # products replaced by the base alone, the scene is drawn at the window's full size, so the Android launcher's Dynamic resolution "Off" is the native size
    # and its percentages are of the window (probe.c drs_install steers the range below that).
    'e25c90c1c136638885659fd55bf8a03b': [
        (0x30f8f4, '2008201e', '2040201e', 'frame buffer scale: fmul s0, s1, s0 → fmov s0, s1 (the base, 1.0, instead of base × 0.8)'),
        (0x30f904, '0008211e', '1f2003d5', 'frame buffer scale: the second fmul → nop (the height keeps the base too)'),
    ],
}

# Observation gates. v458752: sead's frame-rate controller's sdiv of 60 by its interval (sdiv w8, w8, w21; the controller in x20), where the host makes the rate the
# measured one (hle_fpslock.c sead_rate; gate OBS_SEAD_RATE = 27 with w8, w21 and x20 in its upper bits, nso_code.py sead_param; the profile opts in with fps_decouple sead).
# The rewriter finds the same place by the controller's code (nso_code.py sead_find), and in any other build (the base game v0: main+0x1eadf0, not a profile build, so no
# row here); this row names it for v458752 whatever the search finds (an override, and a fallback should it stop finding it)
sead_why = "sead's frame-rate controller: 60 / interval (w8 / w21, the controller in x20); the host makes it the measured rate (hle_fpslock.c sead_rate)"
OBS = {
    'e25c90c1c136638885659fd55bf8a03b': [(0x204b20, 27 | (8 | 21 << 5 | 20 << 10) << 8, '1ad50d08', sead_why)],   # v458752
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': "frame buffer scale: the scene at the window's full size instead of 80 %", 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own per-mode table entry, multiplied in at two fixed offsets; a function fingerprint of the scene setup (the two fmul after the table load) could carry it over the game's updates"},
    {'what': "sead's frame-rate controller's place for v458752, named whatever the search finds", 'now': 'build', 'goal': 'build', 'purpose': 'frame rate',
     'why': "an override and fallback of the rewriter's search (nso_code.py sead_find), which finds the same place in every build of this game by the controller's code",
     'checked': 'v0 and v458752 by disassembly, 2026-11-10: the search finds main+0x1eadf0 and main+0x204b20, nso_fnmap.py maps one to the other exactly'},
]

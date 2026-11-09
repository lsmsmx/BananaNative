// The modules an Android pack was made with: the "modules" row of <pack>/pack.tsv (scripts/pack_android.py writes it: the names of the modules it rewrote, separated by spaces).
// A game whose registered builds have different module sets (Mario Kart World: 1.8.0 has a subsdk1 that 1.0.0 lacks) lists every module in its profile, and a dump packs those of its own build.
#pragma once
#include <stdio.h>
#include <string.h>

// Fills names (at most max pointers into buf, split in place) from <dir>/pack.tsv and returns how many; 0 when there is no pack.tsv, no "modules" row, an empty row, more names than max, or a name
// that is not a plain module name (a '/', or a leading '.'): the caller then takes the profile's list
static inline int pack_modules(const char *dir, char *buf, size_t bufn, const char **names, int max) {
    char path[1024], line[1024];
    if (snprintf(path, sizeof path, "%s/pack.tsv", dir) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "modules\t", 8)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line + 8) >= bufn) break;
        strcpy(buf, line + 8);
        for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) {
            if (n >= max || strchr(t, '/') || t[0] == '.') { n = 0; break; }
            names[n++] = t;
        }
        break;
    }
    fclose(f);
    return n;
}

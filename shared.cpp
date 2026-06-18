/*
 * shared.cpp — Single translation unit for weapon table helpers declared in shared.h.
 * Keeps slot sizes, damage values, and artifact flags consistent for arbiter/hip/asp.
 */
#include "shared.h"

#include <stddef.h>

static const cr_weapon_def_t k_weapon_defs[] = {
    {CR_WEAPON_SOLAR_CORE, "Solar Core", 10, 95, 1},
    {CR_WEAPON_LUNAR_BLADE, "Lunar Blade", 10, 90, 1},
    {CR_WEAPON_IRON_HALBERD, "Iron Halberd", 7, 55, 0},
    {CR_WEAPON_VENOM_DAGGER, "Venom Dagger", 4, 30, 0},
    {CR_WEAPON_THUNDERSTAFF, "Thunderstaff", 6, 50, 0},
    {CR_WEAPON_OBSIDIAN_AXE, "Obsidian Axe", 5, 45, 0},
    {CR_WEAPON_FROSTBOW, "Frostbow", 6, 48, 0},
    {CR_WEAPON_SPLINTER_STICK, "Splinter Stick", 2, 12, 0},
    {CR_WEAPON_ECLIPSE_RELIC, "Eclipse Relic", 3, 60, 1},
};

/* Read-only array pointer; optionally writes entry count into count_out. */
const cr_weapon_def_t *cr_get_weapon_defs(int *count_out) {
    if (count_out != NULL) {
        *count_out = (int)(sizeof(k_weapon_defs) / sizeof(k_weapon_defs[0]));
    }
    return k_weapon_defs;
}

/* Contiguous cells required in cr_inventory_t.slots for this weapon type. */
int cr_weapon_slot_size(cr_weapon_type_t weapon) {
    int count = 0;
    const cr_weapon_def_t *defs = cr_get_weapon_defs(&count);
    for (int i = 0; i < count; ++i) {
        if (defs[i].type == weapon) {
            return defs[i].slot_size;
        }
    }
    return 0;
}

/* Base damage for USE_WEAPON; arbiter may fall back to entity->damage if zero. */
int cr_weapon_damage(cr_weapon_type_t weapon) {
    int count = 0;
    const cr_weapon_def_t *defs = cr_get_weapon_defs(&count);
    for (int i = 0; i < count; ++i) {
        if (defs[i].type == weapon) {
            return defs[i].damage;
        }
    }
    return 0;
}

/* 1 if this weapon maps to a unique world artifact (Solar/Lunar/Eclipse). */
int cr_is_artifact_weapon(cr_weapon_type_t weapon) {
    int count = 0;
    const cr_weapon_def_t *defs = cr_get_weapon_defs(&count);
    for (int i = 0; i < count; ++i) {
        if (defs[i].type == weapon) {
            return defs[i].is_artifact;
        }
    }
    return 0;
}

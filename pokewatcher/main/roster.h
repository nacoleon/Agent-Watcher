#ifndef POKEWATCHER_ROSTER_H
#define POKEWATCHER_ROSTER_H

#include <stdint.h>
#include <stdbool.h>

#define PW_MAX_ROSTER_SIZE   20
#define PW_POKEMON_ID_LEN    32
#define PW_POKEMON_NAME_LEN  32

typedef struct {
    char id[PW_POKEMON_ID_LEN];
    char name[PW_POKEMON_NAME_LEN];
    char sprite_sheet[64];
    char frame_manifest[64];
    char evolves_to[PW_POKEMON_ID_LEN];
    uint32_t evolution_hours;
} pw_pokemon_def_t;

typedef struct {
    char id[PW_POKEMON_ID_LEN];
    uint32_t evolution_seconds;
} pw_roster_entry_t;

typedef struct {
    pw_roster_entry_t entries[PW_MAX_ROSTER_SIZE];
    int count;
    char active_id[PW_POKEMON_ID_LEN];
} pw_roster_t;

void pw_roster_init(void);
pw_roster_t pw_roster_get(void);
bool pw_roster_add(const char *pokemon_id);
bool pw_roster_remove(const char *pokemon_id);
bool pw_roster_set_active(const char *pokemon_id);
const char *pw_roster_get_active_id(void);
void pw_roster_update_evolution(uint32_t total_seconds);
bool pw_pokemon_load_def(const char *pokemon_id, pw_pokemon_def_t *def);
int pw_pokemon_scan_available(char ids[][PW_POKEMON_ID_LEN], int max_count);
bool pw_roster_evolve_active(void);
void pw_roster_save(void);

#endif

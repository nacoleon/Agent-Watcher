#include "roster.h"
#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "pw_roster";
static pw_roster_t s_roster;

void pw_roster_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PW_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_blob(handle, "roster", &s_roster, sizeof(pw_roster_t));
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Roster saved (count=%d, active=%s)", s_roster.count, s_roster.active_id);
}

static void roster_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved roster found, starting fresh");
        memset(&s_roster, 0, sizeof(pw_roster_t));
        return;
    }
    size_t len = sizeof(pw_roster_t);
    err = nvs_get_blob(handle, "roster", &s_roster, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(pw_roster_t)) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Roster blob size mismatch (%zu vs %zu), resetting", len, sizeof(pw_roster_t));
        } else {
            ESP_LOGI(TAG, "No saved roster found, starting fresh");
        }
        memset(&s_roster, 0, sizeof(pw_roster_t));
    } else {
        ESP_LOGI(TAG, "Roster loaded (count=%d, active=%s)", s_roster.count, s_roster.active_id);
    }
}

bool pw_pokemon_load_def(const char *pokemon_id, pw_pokemon_def_t *def)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s/pokemon.json", PW_SD_POKEMON_DIR, pokemon_id);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize > 2048) {
        ESP_LOGE(TAG, "pokemon.json too large: %ld", fsize);
        fclose(f);
        return false;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse %s", path);
        return false;
    }

    memset(def, 0, sizeof(pw_pokemon_def_t));

    cJSON *id_j = cJSON_GetObjectItem(root, "id");
    cJSON *name_j = cJSON_GetObjectItem(root, "name");
    cJSON *sprite_j = cJSON_GetObjectItem(root, "sprite_sheet");
    cJSON *frames_j = cJSON_GetObjectItem(root, "frame_manifest");
    cJSON *evolves_j = cJSON_GetObjectItem(root, "evolves_to");
    cJSON *hours_j = cJSON_GetObjectItem(root, "evolution_hours");

    if (!id_j || !name_j || !sprite_j || !frames_j) {
        ESP_LOGE(TAG, "Missing required fields in %s", path);
        cJSON_Delete(root);
        return false;
    }

    strncpy(def->id, id_j->valuestring, PW_POKEMON_ID_LEN - 1);
    strncpy(def->name, name_j->valuestring, PW_POKEMON_NAME_LEN - 1);
    strncpy(def->sprite_sheet, sprite_j->valuestring, sizeof(def->sprite_sheet) - 1);
    strncpy(def->frame_manifest, frames_j->valuestring, sizeof(def->frame_manifest) - 1);

    if (evolves_j && cJSON_IsString(evolves_j)) {
        strncpy(def->evolves_to, evolves_j->valuestring, PW_POKEMON_ID_LEN - 1);
    }
    if (hours_j && cJSON_IsNumber(hours_j)) {
        def->evolution_hours = (uint32_t)hours_j->valuedouble;
    } else {
        def->evolution_hours = PW_DEFAULT_EVOLUTION_HOURS;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded Pokemon def: %s (%s)", def->name, def->id);
    return true;
}

int pw_pokemon_scan_available(char ids[][PW_POKEMON_ID_LEN], int max_count)
{
    DIR *dir = opendir(PW_SD_POKEMON_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open pokemon directory: %s", PW_SD_POKEMON_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            char check_path[300];
            snprintf(check_path, sizeof(check_path), "%s/%s/pokemon.json", PW_SD_POKEMON_DIR, entry->d_name);
            struct stat st;
            if (stat(check_path, &st) == 0) {
                strncpy(ids[count], entry->d_name, PW_POKEMON_ID_LEN - 1);
                ids[count][PW_POKEMON_ID_LEN - 1] = '\0';
                count++;
            }
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Found %d Pokemon on SD card", count);
    return count;
}

void pw_roster_init(void)
{
    roster_load();
}

pw_roster_t pw_roster_get(void)
{
    return s_roster;
}

bool pw_roster_add(const char *pokemon_id)
{
    if (s_roster.count >= PW_MAX_ROSTER_SIZE) {
        ESP_LOGW(TAG, "Roster full");
        return false;
    }

    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, pokemon_id) == 0) {
            ESP_LOGW(TAG, "%s already in roster", pokemon_id);
            return false;
        }
    }

    pw_pokemon_def_t def;
    if (!pw_pokemon_load_def(pokemon_id, &def)) {
        return false;
    }

    pw_roster_entry_t *entry = &s_roster.entries[s_roster.count];
    strncpy(entry->id, pokemon_id, PW_POKEMON_ID_LEN - 1);
    entry->evolution_seconds = 0;
    s_roster.count++;

    if (s_roster.count == 1) {
        strncpy(s_roster.active_id, pokemon_id, PW_POKEMON_ID_LEN - 1);
    }

    pw_roster_save();
    ESP_LOGI(TAG, "Added %s to roster (count=%d)", pokemon_id, s_roster.count);
    return true;
}

bool pw_roster_remove(const char *pokemon_id)
{
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, pokemon_id) == 0) {
            for (int j = i; j < s_roster.count - 1; j++) {
                s_roster.entries[j] = s_roster.entries[j + 1];
            }
            s_roster.count--;

            if (strcmp(s_roster.active_id, pokemon_id) == 0) {
                if (s_roster.count > 0) {
                    strncpy(s_roster.active_id, s_roster.entries[0].id, PW_POKEMON_ID_LEN - 1);
                } else {
                    s_roster.active_id[0] = '\0';
                }
            }

            pw_roster_save();
            ESP_LOGI(TAG, "Removed %s from roster", pokemon_id);
            return true;
        }
    }
    ESP_LOGW(TAG, "%s not found in roster", pokemon_id);
    return false;
}

bool pw_roster_set_active(const char *pokemon_id)
{
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, pokemon_id) == 0) {
            strncpy(s_roster.active_id, pokemon_id, PW_POKEMON_ID_LEN - 1);
            pw_roster_save();
            ESP_LOGI(TAG, "Active Pokemon set to %s", pokemon_id);
            return true;
        }
    }
    ESP_LOGW(TAG, "%s not in roster", pokemon_id);
    return false;
}

const char *pw_roster_get_active_id(void)
{
    if (s_roster.active_id[0] == '\0') {
        return NULL;
    }
    return s_roster.active_id;
}

void pw_roster_update_evolution(uint32_t total_seconds)
{
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, s_roster.active_id) == 0) {
            s_roster.entries[i].evolution_seconds = total_seconds;
            return;
        }
    }
}

bool pw_roster_evolve_active(void)
{
    const char *active_id = pw_roster_get_active_id();
    if (!active_id) return false;

    pw_pokemon_def_t current_def;
    if (!pw_pokemon_load_def(active_id, &current_def)) return false;

    if (current_def.evolves_to[0] == '\0') {
        ESP_LOGI(TAG, "%s is a final form, no evolution", active_id);
        return false;
    }

    pw_pokemon_def_t evolved_def;
    if (!pw_pokemon_load_def(current_def.evolves_to, &evolved_def)) {
        ESP_LOGE(TAG, "Evolved form %s not found on SD card", current_def.evolves_to);
        return false;
    }

    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, active_id) == 0) {
            strncpy(s_roster.entries[i].id, current_def.evolves_to, PW_POKEMON_ID_LEN - 1);
            s_roster.entries[i].evolution_seconds = 0;
            strncpy(s_roster.active_id, current_def.evolves_to, PW_POKEMON_ID_LEN - 1);
            pw_roster_save();
            ESP_LOGI(TAG, "Evolution: %s -> %s", active_id, current_def.evolves_to);
            return true;
        }
    }

    return false;
}

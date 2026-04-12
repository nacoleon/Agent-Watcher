#include "sprite_loader.h"
#include "config.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "pw_sprite";

static bool load_frame_manifest(const char *pokemon_id, pw_sprite_data_t *sprite)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s/frames.json", PW_SD_POKEMON_DIR, pokemon_id);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse frames.json");
        return false;
    }

    cJSON *fw = cJSON_GetObjectItem(root, "frame_width");
    cJSON *fh = cJSON_GetObjectItem(root, "frame_height");
    if (!fw || !fh) {
        ESP_LOGE(TAG, "Missing frame_width/frame_height in frames.json");
        cJSON_Delete(root);
        return false;
    }
    sprite->frame_width = (uint16_t)fw->valuedouble;
    sprite->frame_height = (uint16_t)fh->valuedouble;

    cJSON *anims = cJSON_GetObjectItem(root, "animations");
    sprite->animation_count = 0;
    cJSON *anim_item = NULL;
    cJSON_ArrayForEach(anim_item, anims) {
        if (sprite->animation_count >= PW_MAX_ANIMATIONS) break;

        pw_animation_t *anim = &sprite->animations[sprite->animation_count];
        strncpy(anim->name, anim_item->string, PW_ANIM_NAME_LEN - 1);

        cJSON *loop_j = cJSON_GetObjectItem(anim_item, "loop");
        anim->loop = loop_j ? cJSON_IsTrue(loop_j) : true;

        cJSON *frames_arr = cJSON_GetObjectItem(anim_item, "frames");
        anim->frame_count = 0;
        cJSON *frame_j = NULL;
        cJSON_ArrayForEach(frame_j, frames_arr) {
            if (anim->frame_count >= PW_MAX_FRAMES_PER_ANIM) break;
            cJSON *fx = cJSON_GetObjectItem(frame_j, "x");
            cJSON *fy = cJSON_GetObjectItem(frame_j, "y");
            if (!fx || !fy) continue;
            anim->frames[anim->frame_count].x = (uint16_t)fx->valuedouble;
            anim->frames[anim->frame_count].y = (uint16_t)fy->valuedouble;
            anim->frame_count++;
        }
        sprite->animation_count++;
    }

    cJSON *mood_map = cJSON_GetObjectItem(root, "mood_animations");
    if (mood_map) {
        const char *mood_keys[] = {"excited", "happy", "curious", "lonely", "sleepy", "overjoyed"};
        for (int i = 0; i < 6; i++) {
            cJSON *val = cJSON_GetObjectItem(mood_map, mood_keys[i]);
            if (val && cJSON_IsString(val)) {
                strncpy(sprite->mood_anim_names[i], val->valuestring, PW_ANIM_NAME_LEN - 1);
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d animations, frame size %dx%d",
             sprite->animation_count, sprite->frame_width, sprite->frame_height);
    return true;
}

static bool load_sprite_sheet(const char *pokemon_id, pw_sprite_data_t *sprite)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s/overworld.raw", PW_SD_POKEMON_DIR, pokemon_id);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open sprite sheet: %s", path);
        return false;
    }

    uint16_t dims[2];
    fread(dims, sizeof(uint16_t), 2, f);
    sprite->sheet_width = dims[0];
    sprite->sheet_height = dims[1];

    size_t data_size = sprite->sheet_width * sprite->sheet_height * 2;
    sprite->sprite_sheet_data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM);
    if (!sprite->sprite_sheet_data) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM for sprite sheet", data_size);
        fclose(f);
        return false;
    }

    size_t read = fread(sprite->sprite_sheet_data, 1, data_size, f);
    fclose(f);

    if (read != data_size) {
        ESP_LOGE(TAG, "Sprite sheet read incomplete: %zu/%zu", read, data_size);
        heap_caps_free(sprite->sprite_sheet_data);
        sprite->sprite_sheet_data = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Loaded sprite sheet %"PRIu32"x%"PRIu32" (%zu bytes)", sprite->sheet_width, sprite->sheet_height, data_size);
    return true;
}

bool pw_sprite_load(const char *pokemon_id, pw_sprite_data_t *sprite)
{
    memset(sprite, 0, sizeof(pw_sprite_data_t));
    if (!load_frame_manifest(pokemon_id, sprite)) return false;
    if (!load_sprite_sheet(pokemon_id, sprite)) return false;
    return true;
}

void pw_sprite_free(pw_sprite_data_t *sprite)
{
    if (sprite->sprite_sheet_data) {
        heap_caps_free(sprite->sprite_sheet_data);
        sprite->sprite_sheet_data = NULL;
    }
}

const pw_animation_t *pw_sprite_get_mood_anim(const pw_sprite_data_t *sprite, pw_mood_t mood)
{
    if (mood >= 6) return NULL;
    const char *anim_name = sprite->mood_anim_names[mood];
    if (anim_name[0] == '\0') return NULL;

    for (int i = 0; i < sprite->animation_count; i++) {
        if (strcmp(sprite->animations[i].name, anim_name) == 0) {
            return &sprite->animations[i];
        }
    }
    return NULL;
}

uint16_t *pw_sprite_extract_frame_scaled(const pw_sprite_data_t *sprite,
                                          const pw_frame_coord_t *coord,
                                          uint16_t scale)
{
    if (!sprite->sprite_sheet_data) return NULL;

    uint16_t dst_w = sprite->frame_width * scale;
    uint16_t dst_h = sprite->frame_height * scale;
    size_t buf_size = dst_w * dst_h * sizeof(uint16_t);

    uint16_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    const uint16_t *sheet = (const uint16_t *)sprite->sprite_sheet_data;

    for (uint16_t dy = 0; dy < dst_h; dy++) {
        uint16_t sy = coord->y + (dy / scale);
        if (sy >= sprite->sheet_height) sy = sprite->sheet_height - 1;
        for (uint16_t dx = 0; dx < dst_w; dx++) {
            uint16_t sx = coord->x + (dx / scale);
            if (sx >= sprite->sheet_width) sx = sprite->sheet_width - 1;
            buf[dy * dst_w + dx] = sheet[sy * sprite->sheet_width + sx];
        }
    }

    return buf;
}

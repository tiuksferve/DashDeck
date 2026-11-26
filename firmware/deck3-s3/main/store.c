#include "store.h"

#include <limits.h>

#include "nvs.h"

#define NS "deck3"

int32_t store_get(const char *key, int32_t def)
{
    nvs_handle_t h;
    int32_t v = def;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, key, &v) != ESP_OK) v = def;
        nvs_close(h);
    }
    return v;
}

void store_set(const char *key, int32_t v)
{
    if (store_get(key, INT32_MIN) == v) return;   // poupa a flash
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

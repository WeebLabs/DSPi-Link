/*
 * web_assets.c - looks embedded web files up by path. See web_assets.h.
 */

#include "web_assets.h"

#include <string.h>

const web_asset_t *web_assets_find(const char *path)
{
    for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
        if (strcmp(WEB_ASSETS[i].path, path) == 0) {
            return &WEB_ASSETS[i];
        }
    }
    return NULL;
}

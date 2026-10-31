/* components/config_mgr/radio_region.c — 射频地区菜单。见 radio_region.h。 */
#include <string.h>
#include "radio_region.h"

static const radio_region_menu_t MENU[] = {
    { "EU", RADIO_REGION_CLASS_CE },       /* 欧盟 / 英国 / 瑞士等 ETSI 地区；NCP 表里是 CE 别名 */
    { "US", RADIO_REGION_CLASS_FCC },      /* United States */
    { "CA", RADIO_REGION_CLASS_FCC },      /* Canada */
    { "ZZ", RADIO_REGION_CLASS_UNKNOWN },  /* 其他地区：ISO 3166 用户自定义码，NCP 表里没有 → 8 dBm */
};

const radio_region_menu_t *radio_region_menu(size_t *count)
{
    if (count) *count = sizeof(MENU) / sizeof(MENU[0]);
    return MENU;
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

void radio_region_normalize(char *cc)
{
    if (!cc) return;
    for (int i = 0; i < RADIO_REGION_CODE_LEN && cc[i]; i++) cc[i] = upper(cc[i]);
}

static const radio_region_menu_t *lookup(const char *cc)
{
    if (!cc || strlen(cc) != RADIO_REGION_CODE_LEN) return NULL;
    char a = upper(cc[0]), b = upper(cc[1]);
    for (size_t i = 0; i < sizeof(MENU) / sizeof(MENU[0]); i++) {
        if (MENU[i].code[0] == a && MENU[i].code[1] == b) return &MENU[i];
    }
    return NULL;
}

bool radio_region_code_valid(const char *cc)
{
    if (!cc) return false;
    if (cc[0] == '\0') return true;          /* 未设置 */
    return lookup(cc) != NULL;
}

radio_region_class_t radio_region_class_of(const char *cc)
{
    if (!cc || cc[0] == '\0') return RADIO_REGION_CLASS_NONE;
    const radio_region_menu_t *m = lookup(cc);
    return m ? m->cls : RADIO_REGION_CLASS_UNKNOWN;
}

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "../../userland/apps/settings/config.h"
#include "../../userland/apps/shared/ui_input.h"

int main(void)
{
    unsigned int previous = 0;
    assert(uk_mouse_press(&previous, 1) == 1);
    assert(uk_mouse_press(&previous, 1) == 0); /* held motion */
    assert(uk_mouse_press(&previous, 3) == 2); /* second button */
    assert(uk_mouse_press(&previous, 0) == 0);
    assert(uk_mouse_press(&previous, 1) == 1); /* next click, even after early return */

    unsigned char address[4] = {99, 99, 99, 99};
    assert(settings_parse_ipv4("192.168.1.50", address) == 0);
    assert(address[0] == 192 && address[3] == 50);
    const char *invalid[] = {"", "1.2.3", "1.2.3.4.5", "256.0.0.1", "-1.2.3.4",
                             "1..2.3", "1.2.3.4junk", "1.2.3.4 ", "999999999999.1.2.3"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        unsigned char before[4];
        memcpy(before, address, sizeof(address));
        assert(settings_parse_ipv4(invalid[i], address) < 0);
        assert(memcmp(address, before, sizeof(address)) == 0);
    }
    assert(settings_parse_ipv4("0.0.0.0", address) == 0);
    assert(settings_parse_ipv4("255.255.255.255", address) == 0);

    const char *original = "# custom desktop\n[theme]\nwallpaper=/my/image.raw\ntheme_id=10\n"
                           "[display]\nvsync=1\nvsync=1\nfps=144\n[panel]\nautohide=1\n";
    char changed[1024], again[1024];
    assert(settings_config_set(original, changed, sizeof(changed), "display", "vsync", "0") > 0);
    assert(strstr(changed, "wallpaper=/my/image.raw\n"));
    assert(strstr(changed, "fps=144\n"));
    assert(strstr(changed, "autohide=1\n"));
    assert(strstr(changed, "vsync=0\n"));
    assert(!strstr(changed, "vsync=1"));
    assert(settings_config_set(changed, again, sizeof(again), "theme", "theme_id", "2") > 0);
    assert(!strstr(again, "theme_id=10"));
    assert(strstr(again, "theme_id=2\n"));
    assert(settings_config_set("[theme]\nwallpaper=custom", changed, sizeof(changed), "theme", "theme_id", "3") > 0);
    assert(strcmp(changed, "[theme]\nwallpaper=custom\ntheme_id=3\n") == 0);
    assert(settings_config_set("[panel]\nfps=60\n", changed, sizeof(changed), "display", "vsync", "0") > 0);
    assert(strstr(changed, "[display]\nvsync=0\n"));
    assert(settings_config_set(original, changed, 8, "display", "vsync", "0") < 0);
    assert(settings_config_set("[other]\nvsync=1\n[display]\n", changed, sizeof(changed), "display", "vsync", "0") > 0);
    assert(strstr(changed, "[other]\nvsync=1\n[display]\nvsync=0\n"));
    puts("Settings parser, configuration preservation and mouse transitions: PASS");
    return 0;
}

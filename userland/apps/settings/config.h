#pragma once

/* Requires string.h. Keep parsing independent of devices for host testing. */
static inline int settings_parse_ipv4(const char *s, unsigned char out[4])
{
    unsigned char parsed[4];
    for (int i = 0; i < 4; i++) {
        unsigned int value = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (unsigned int)(*s++ - '0');
            if (++digits > 3 || value > 255) return -1;
        }
        if (!digits) return -1;
        parsed[i] = (unsigned char)value;
        if (i < 3) { if (*s++ != '.') return -1; }
        else if (*s) return -1;
    }
    memcpy(out, parsed, sizeof(parsed));
    return 0;
}

/* Replace all occurrences of a key in one INI section, preserving comments,
 * other sections and user preferences. Refuse overflow before modifying data. */
static inline int settings_config_set(const char *input, char *output, size_t cap,
                                      const char *section, const char *key,
                                      const char *value)
{
    size_t used = 0, key_len = strlen(key), section_len = strlen(section);
    int inside = !section_len, found_section = !section_len, written = 0;
    const char *line = input;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[0] == '[') {
            if (inside && !written) {
                size_t n = key_len + strlen(value) + 2;
                if (used + n >= cap) return -1;
                memcpy(output + used, key, key_len); used += key_len;
                output[used++] = '=';
                memcpy(output + used, value, strlen(value)); used += strlen(value);
                output[used++] = '\n'; written = 1;
            }
            inside = len >= section_len + 2 && line[section_len + 1] == ']' &&
                     strncmp(line + 1, section, section_len) == 0;
            if (inside) found_section = 1;
        }
        if (inside && len > key_len && line[key_len] == '=' &&
            strncmp(line, key, key_len) == 0) {
            if (!written) {
                size_t n = key_len + strlen(value) + 2;
                if (used + n >= cap) return -1;
                memcpy(output + used, key, key_len); used += key_len;
                output[used++] = '=';
                memcpy(output + used, value, strlen(value)); used += strlen(value);
                output[used++] = '\n'; written = 1;
            }
        } else {
            if (used + len + 1 >= cap) return -1;
            memcpy(output + used, line, len); used += len;
            output[used++] = '\n';
        }
        line = end ? end + 1 : line + len;
    }
    if (!written) {
        if (!found_section) {
            if (used + section_len + 3 >= cap) return -1;
            output[used++] = '[';
            memcpy(output + used, section, section_len); used += section_len;
            output[used++] = ']'; output[used++] = '\n';
        }
        size_t n = key_len + strlen(value) + 2;
        if (used + n >= cap) return -1;
        memcpy(output + used, key, key_len); used += key_len;
        output[used++] = '=';
        memcpy(output + used, value, strlen(value)); used += strlen(value);
        output[used++] = '\n';
    }
    output[used] = '\0';
    return (int)used;
}

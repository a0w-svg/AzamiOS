/* ============================================================================
 * AzamiOS Userspace — POSIX tsort Utility (main.c)
 * File: userland/apps/tsort/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_NODES 1024
#define MAX_NAME 128

typedef struct {
    char name[MAX_NAME];
    int in_degree;
    int visited;
} Node;

static Node g_nodes[MAX_NODES];
static int g_node_count = 0;
static int g_adj[MAX_NODES][MAX_NODES];

static int get_or_add_node(const char *name)
{
    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].name, name) == 0) return i;
    }
    if (g_node_count >= MAX_NODES) {
        fprintf(stderr, "tsort: maximum node limit (%d) exceeded\n", MAX_NODES);
        exit(1);
    }
    int idx = g_node_count++;
    strncpy(g_nodes[idx].name, name, MAX_NAME - 1);
    g_nodes[idx].name[MAX_NAME - 1] = '\0';
    g_nodes[idx].in_degree = 0;
    g_nodes[idx].visited = 0;
    return idx;
}

static int read_token(FILE *fp, char *buf, size_t max)
{
    int c;
    while ((c = fgetc(fp)) != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
        /* skip whitespace */
    }
    if (c == EOF) return 0;

    size_t i = 0;
    buf[i++] = (char)c;
    while ((c = fgetc(fp)) != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        if (i + 1 < max) buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return 1;
}

int main(int argc, char *argv[])
{
    FILE *fp = stdin;
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: tsort [FILE]\n"
               "Write totally ordered list consistent with the partial ordering in FILE.\n");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "-") != 0) {
        fp = fopen(argv[1], "r");
        if (!fp) {
            perror(argv[1]);
            return 1;
        }
    }

    char u_name[MAX_NAME], v_name[MAX_NAME];
    while (read_token(fp, u_name, sizeof(u_name)) && read_token(fp, v_name, sizeof(v_name))) {
        int u = get_or_add_node(u_name);
        int v = get_or_add_node(v_name);
        if (u != v && !g_adj[u][v]) {
            g_adj[u][v] = 1;
            g_nodes[v].in_degree++;
        }
    }

    if (fp != stdin) fclose(fp);

    /* Kahn's algorithm for topological sorting */
    int output_count = 0;
    while (output_count < g_node_count) {
        int found = -1;
        for (int i = 0; i < g_node_count; i++) {
            if (!g_nodes[i].visited && g_nodes[i].in_degree == 0) {
                found = i;
                break;
            }
        }

        if (found == -1) {
            /* Cycle detected */
            for (int i = 0; i < g_node_count; i++) {
                if (!g_nodes[i].visited) {
                    fprintf(stderr, "tsort: %s: input contains a loop:\n", argv[0]);
                    fprintf(stderr, "tsort: %s\n", g_nodes[i].name);
                    found = i;
                    break;
                }
            }
        }

        printf("%s\n", g_nodes[found].name);
        g_nodes[found].visited = 1;
        output_count++;

        for (int j = 0; j < g_node_count; j++) {
            if (g_adj[found][j]) {
                g_adj[found][j] = 0;
                g_nodes[j].in_degree--;
            }
        }
    }

    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    int value;
    struct Node *next;
} Node;

int main(void) {
    printf("=== Dynamic Memory & Linked List Test ===\n");
    Node *head = NULL;

    printf("Pushing 10 elements to linked list...\n");
    for (int i = 1; i <= 10; i++) {
        Node *n = (Node *)malloc(sizeof(Node));
        if (!n) {
            printf("Allocation failed at index %d\n", i);
            return 1;
        }
        n->value = i * 10;
        n->next = head;
        head = n;
    }

    printf("Traversing linked list: ");
    Node *curr = head;
    while (curr) {
        printf("%d -> ", curr->value);
        curr = curr->next;
    }
    printf("NULL\n");

    printf("Testing realloc...\n");
    char *dyn_str = (char *)malloc(16);
    strcpy(dyn_str, "AzamiOS");
    dyn_str = (char *)realloc(dyn_str, 64);
    strcat(dyn_str, " Memory Management OK");
    printf("String result: %s\n", dyn_str);
    free(dyn_str);

    /* Free linked list */
    while (head) {
        Node *next = head->next;
        free(head);
        head = next;
    }
    printf("Memory cleanup successful.\n");
    return 0;
}

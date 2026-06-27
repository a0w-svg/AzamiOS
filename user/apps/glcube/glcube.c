/**
 * glcube.c — AzamiOS Standalone Fullscreen 3D OpenGL Demonstration
 */
#include <stdio.h>
#include <stdlib.h>
#include <gui.h>
#include <GL/gl.h>

void _start(void) {
    init_graphics();

    glViewport(0, 0, 640, 480);

    float angle_cube = 0.0f;
    float angle_pyr  = 0.0f;
    uint32_t frames  = 0;

    for (;;) {
        if (has_char()) {
            /* Any key exit demo */
            (void)getchar();
            break;
        }

        glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(60.0f, 640.0f / 480.0f, 1.0f, 100.0f);

        glMatrixMode(GL_MODELVIEW);

        /* 1. Draw Rotating Cube on the Left */
        glLoadIdentity();
        glTranslatef(-1.8f, 0.0f, -6.0f);
        glRotatef(angle_cube * 0.7f, 1.0f, 0.0f, 0.0f);
        glRotatef(angle_cube, 0.0f, 1.0f, 0.0f);

        glBegin(GL_QUADS);
        /* Front Face */
        glColor3f(1.0f, 0.2f, 0.2f); glVertex3f(-1.0f, -1.0f,  1.0f);
        glColor3f(1.0f, 0.9f, 0.1f); glVertex3f( 1.0f, -1.0f,  1.0f);
        glColor3f(0.9f, 1.0f, 0.9f); glVertex3f( 1.0f,  1.0f,  1.0f);
        glColor3f(0.9f, 0.2f, 0.9f); glVertex3f(-1.0f,  1.0f,  1.0f);

        /* Back Face */
        glColor3f(0.1f, 0.9f, 0.9f); glVertex3f(-1.0f, -1.0f, -1.0f);
        glColor3f(0.1f, 0.2f, 0.9f); glVertex3f(-1.0f,  1.0f, -1.0f);
        glColor3f(0.2f, 0.9f, 0.2f); glVertex3f( 1.0f,  1.0f, -1.0f);
        glColor3f(0.9f, 0.9f, 0.9f); glVertex3f( 1.0f, -1.0f, -1.0f);

        /* Top Face */
        glColor3f(0.2f, 0.9f, 0.2f); glVertex3f(-1.0f,  1.0f, -1.0f);
        glColor3f(0.1f, 0.9f, 0.9f); glVertex3f(-1.0f,  1.0f,  1.0f);
        glColor3f(0.9f, 0.9f, 0.9f); glVertex3f( 1.0f,  1.0f,  1.0f);
        glColor3f(0.9f, 0.9f, 0.1f); glVertex3f( 1.0f,  1.0f, -1.0f);

        /* Bottom Face */
        glColor3f(0.1f, 0.2f, 0.9f); glVertex3f(-1.0f, -1.0f, -1.0f);
        glColor3f(0.9f, 0.9f, 0.9f); glVertex3f( 1.0f, -1.0f, -1.0f);
        glColor3f(0.9f, 0.2f, 0.2f); glVertex3f( 1.0f, -1.0f,  1.0f);
        glColor3f(0.6f, 0.1f, 0.8f); glVertex3f(-1.0f, -1.0f,  1.0f);

        /* Right Face */
        glColor3f(0.9f, 0.9f, 0.1f); glVertex3f( 1.0f, -1.0f, -1.0f);
        glColor3f(0.2f, 0.9f, 0.2f); glVertex3f( 1.0f,  1.0f, -1.0f);
        glColor3f(0.9f, 0.9f, 0.9f); glVertex3f( 1.0f,  1.0f,  1.0f);
        glColor3f(0.9f, 0.5f, 0.1f); glVertex3f( 1.0f, -1.0f,  1.0f);

        /* Left Face */
        glColor3f(0.9f, 0.2f, 0.9f); glVertex3f(-1.0f, -1.0f, -1.0f);
        glColor3f(0.6f, 0.1f, 0.6f); glVertex3f(-1.0f, -1.0f,  1.0f);
        glColor3f(0.1f, 0.9f, 0.9f); glVertex3f(-1.0f,  1.0f,  1.0f);
        glColor3f(0.1f, 0.5f, 0.9f); glVertex3f(-1.0f,  1.0f, -1.0f);
        glEnd();

        /* 2. Draw Rotating Pyramid on the Right */
        glLoadIdentity();
        glTranslatef(1.8f, 0.0f, -6.0f);
        glRotatef(angle_pyr, 0.0f, 1.0f, 0.0f);
        glRotatef(angle_pyr * 0.5f, 0.0f, 0.0f, 1.0f);

        glBegin(GL_TRIANGLES);
        /* Front */
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.2f,  0.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(-1.0f, -1.0f,  1.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f( 1.0f, -1.0f,  1.0f);

        /* Right */
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.2f,  0.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f( 1.0f, -1.0f,  1.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f( 1.0f, -1.0f, -1.0f);

        /* Back */
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.2f,  0.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f( 1.0f, -1.0f, -1.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f(-1.0f, -1.0f, -1.0f);

        /* Left */
        glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 0.0f,  1.2f,  0.0f);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex3f(-1.0f, -1.0f, -1.0f);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(-1.0f, -1.0f,  1.0f);
        glEnd();

        /* UI Text */
        draw_rect(0, 0, 640, 24, 0x00101828);
        draw_text(16, 6, "AzamiGL Standalone Fullscreen 3D Demo  |  Press ANY KEY to exit", 0xFFFFFFFF, 0x00101828);

        gfx_flip();

        angle_cube += 4.0f;
        angle_pyr  -= 5.0f;
        frames++;

        for (volatile int k = 0; k < 100; k++);
    }

    exit(0);
}

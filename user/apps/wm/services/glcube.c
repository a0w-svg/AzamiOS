/**
 * glcube.c — AzamiGL 3D Rotating Cube Service for Window Manager
 */
#include "../wm.h"
#include <GL/gl.h>

static void glcube_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)blink;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    if (bw <= 0 || bh <= 0) return;

    /* 1. Set viewport and clear buffers */
    glViewport(bx, by, bw, bh);
    glClearColor(0.08f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* 2. Setup projection matrix */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0f, (float)bw / (float)bh, 1.0f, 100.0f);

    /* 3. Setup modelview matrix and rotations */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -4.5f);

    float angle_y = (float)(frame_cnt * 3) + (float)(t->second * 30);
    float angle_x = angle_y * 0.7f;
    float angle_z = angle_y * 0.4f;
    glRotatef(angle_x, 1.0f, 0.0f, 0.0f);
    glRotatef(angle_y, 0.0f, 1.0f, 0.0f);
    glRotatef(angle_z, 0.0f, 0.0f, 1.0f);

    /* 4. Draw vibrant Gouraud shaded 3D RGB Cube */
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

    /* 5. Draw UI text overlay */
    draw_text(bx + 8, by + 8, "AzamiGL 3D Hardware/Software Renderer", COL_TEXT_WHITE, 0x00141A29);
}

void glcube_service_init(void) {
    static const wm_service_t glcube_srv = {
        WIN_GLCUBE,
        "3D OpenGL Demo",
        NULL,
        NULL,
        glcube_render,
        NULL
    };
    wm_register_service(&glcube_srv);
}

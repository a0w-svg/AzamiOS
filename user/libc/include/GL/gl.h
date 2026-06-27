/**
 * gl.h — AzamiGL Embedded Software OpenGL 1.1 Header
 */
#ifndef _GL_H
#define _GL_H

#include <stdint.h>
#include <stdbool.h>

typedef float    GLfloat;
typedef int      GLint;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;

#define GL_FALSE 0
#define GL_TRUE  1

/* Primitives */
#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_TRIANGLES      0x0004
#define GL_QUADS          0x0007

/* Matrix Modes */
#define GL_MODELVIEW      0x1700
#define GL_PROJECTION     0x1701

/* Buffers */
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100

/* Capabilities */
#define GL_DEPTH_TEST     0x0B71

/* Function declarations */
void glViewport(int x, int y, int width, int height);
void glClearColor(float red, float green, float blue, float alpha);
void glClear(GLenum mask);
void glEnable(GLenum cap);
void glDisable(GLenum cap);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glPushMatrix(void);
void glPopMatrix(void);

void glTranslatef(float x, float y, float z);
void glRotatef(float angle, float x, float y, float z);
void glScalef(float x, float y, float z);

void glOrtho(float left, float right, float bottom, float top, float zNear, float zFar);
void gluPerspective(float fovy, float aspect, float zNear, float zFar);

void glBegin(GLenum mode);
void glEnd(void);
void glColor3f(float red, float green, float blue);
void glVertex3f(float x, float y, float z);

#endif /* _GL_H */

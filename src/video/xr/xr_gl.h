/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file xr_gl.h GLES 3 helpers of the OpenXR video driver: shaders for the screen copy and the pointer ray. */

#ifndef VIDEO_XR_GL_H
#define VIDEO_XR_GL_H

#include <GLES3/gl3.h>

namespace xrgl {

/** Column-major 4x4 matrix. */
struct Mat4 {
	float m[16];
};

/** Asymmetric perspective projection from OpenXR field-of-view angles (radians). */
Mat4 Projection(float left, float right, float down, float up, float near_z, float far_z);
/** View matrix: inverse of the eye pose (orientation quaternion x,y,z,w and position). */
Mat4 View(const float q[4], const float p[3]);
Mat4 Multiply(const Mat4 &a, const Mat4 &b);

/**
 * Copies the game frame (BGRA bytes in an sRGB texture) into the bound framebuffer,
 * top row first, with optional brightness/contrast.
 */
class ScreenCopy {
public:
	bool Init();
	void Draw(GLuint texture, float brightness, float contrast);
	void Destroy();
private:
	GLuint program = 0;
	GLint image = -1, brightness_loc = -1, contrast_loc = -1;
};

/** Draws flat-coloured triangles in world space (pointer ray and marker). */
class ColorDraw {
public:
	bool Init();
	void Draw(const Mat4 &mvp, const float *xyz, int vertex_count, GLenum mode, const float rgba[4]);
	void Destroy();
private:
	GLuint program = 0;
	GLint mvp_loc = -1, color_loc = -1;
};

} // namespace xrgl

#endif /* VIDEO_XR_GL_H */

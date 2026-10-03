/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file xr_gl.cpp GLES 3 helpers of the OpenXR video driver. */

#include "../../stdafx.h"
#include "../../debug.h"
#include "xr_gl.h"

#include <cmath>

#include "../../safeguards.h"

namespace xrgl {

Mat4 Projection(float left, float right, float down, float up, float near_z, float far_z)
{
	const float l = std::tan(left), r = std::tan(right), d = std::tan(down), u = std::tan(up);
	const float w = r - l, h = u - d;
	Mat4 p{};
	p.m[0] = 2.f / w;
	p.m[5] = 2.f / h;
	p.m[8] = (r + l) / w;
	p.m[9] = (u + d) / h;
	p.m[10] = -(far_z + near_z) / (far_z - near_z);
	p.m[11] = -1.f;
	p.m[14] = -(2.f * far_z * near_z) / (far_z - near_z);
	return p;
}

Mat4 View(const float q[4], const float p[3])
{
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	/* Rotation matrix of the pose, rows r0..r2. The view matrix is its transpose with the translated position. */
	const float r[3][3] = {
		{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
		{2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
		{2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
	};
	Mat4 v{};
	for (int col = 0; col < 3; ++col) {
		for (int row = 0; row < 3; ++row) v.m[col * 4 + row] = r[col][row];
	}
	for (int row = 0; row < 3; ++row) {
		v.m[12 + row] = -(r[0][row] * p[0] + r[1][row] * p[1] + r[2][row] * p[2]);
	}
	v.m[15] = 1.f;
	return v;
}

Mat4 Multiply(const Mat4 &a, const Mat4 &b)
{
	Mat4 c{};
	for (int col = 0; col < 4; ++col) {
		for (int row = 0; row < 4; ++row) {
			float sum = 0.f;
			for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
			c.m[col * 4 + row] = sum;
		}
	}
	return c;
}

static GLuint BuildProgram(const char *vertex, const char *fragment)
{
	const char *sources[2] = {vertex, fragment};
	const GLenum types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
	GLuint program = glCreateProgram();
	bool valid = true;
	for (int i = 0; i < 2; ++i) {
		GLuint shader = glCreateShader(types[i]);
		glShaderSource(shader, 1, &sources[i], nullptr);
		glCompileShader(shader);
		GLint ok = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[1024] = {};
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			Debug(driver, 0, "xr: shader compile failed: {}", log);
			valid = false;
		}
		glAttachShader(program, shader);
		glDeleteShader(shader);
	}
	glLinkProgram(program);
	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!valid || !linked) {
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

bool ScreenCopy::Init()
{
	/* One fullscreen triangle. */
	static const char *vertex =
		"#version 300 es\n"
		"void main() {\n"
		"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	/*
	 * 1:1 copy with exact texel addressing (interpolated mediump coordinates pick wrong columns at
	 * 2560 px). Texture row 0 is the top of the game screen. The game writes BGRA bytes; an sRGB
	 * texture decodes to linear and the sRGB swapchain encodes again.
	 */
	static const char *fragment =
		"#version 300 es\n"
		"precision mediump float;\n"
		"uniform sampler2D image;\n"
		"uniform float brightness;\n"
		"uniform float contrast;\n"
		"out vec4 color;\n"
		"void main() {\n"
		"  ivec2 size = textureSize(image, 0);\n"
		"  ivec2 p = ivec2(gl_FragCoord.xy);\n"
		"  vec3 c = texelFetch(image, ivec2(p.x, size.y - 1 - p.y), 0).bgr;\n"
		"  c = clamp(((c - 0.5) * contrast + 0.5) * brightness, 0.0, 1.0);\n"
		"  color = vec4(c, 1.0);\n"
		"}\n";
	this->program = BuildProgram(vertex, fragment);
	if (this->program == 0) return false;
	this->image = glGetUniformLocation(this->program, "image");
	this->brightness_loc = glGetUniformLocation(this->program, "brightness");
	this->contrast_loc = glGetUniformLocation(this->program, "contrast");
	return true;
}

void ScreenCopy::Draw(GLuint texture, float brightness, float contrast)
{
	glUseProgram(this->program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(this->image, 0);
	glUniform1f(this->brightness_loc, brightness);
	glUniform1f(this->contrast_loc, contrast);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
}

void ScreenCopy::Destroy()
{
	if (this->program != 0) glDeleteProgram(this->program);
	this->program = 0;
}

bool ColorDraw::Init()
{
	static const char *vertex =
		"#version 300 es\n"
		"uniform mat4 mvp;\n"
		"layout(location = 0) in vec3 position;\n"
		"void main() { gl_Position = mvp * vec4(position, 1.0); }\n";
	static const char *fragment =
		"#version 300 es\n"
		"precision mediump float;\n"
		"uniform vec4 color;\n"
		"out vec4 result;\n"
		"void main() { result = color; }\n";
	this->program = BuildProgram(vertex, fragment);
	if (this->program == 0) return false;
	this->mvp_loc = glGetUniformLocation(this->program, "mvp");
	this->color_loc = glGetUniformLocation(this->program, "color");
	return true;
}

void ColorDraw::Draw(const Mat4 &mvp, const float *xyz, int vertex_count, GLenum mode, const float rgba[4])
{
	glUseProgram(this->program);
	glUniformMatrix4fv(this->mvp_loc, 1, GL_FALSE, mvp.m);
	glUniform4fv(this->color_loc, 1, rgba);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, xyz);
	glDrawArrays(mode, 0, vertex_count);
	glDisableVertexAttribArray(0);
	glUseProgram(0);
}

void ColorDraw::Destroy()
{
	if (this->program != 0) glDeleteProgram(this->program);
	this->program = 0;
}

} // namespace xrgl

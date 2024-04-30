#pragma once

#define GLSL(version, ...) "#version " #version "\n" #__VA_ARGS__
char *blit_vertex_shader = GLSL(460,
layout(location = 1) out vec2 texcoord;
layout(push_constant) uniform BlitData {
	uvec2 viewport_dimensions;
	int rect_x1;
	int rect_y1;
	int rect_x2;
	int rect_y2;
	ivec2 origin;
};

void main() {
	mat4 projection = mat4(
		vec4(2.0f / viewport_dimensions.x, 0.0f, 0.0f, 0.0f),
		vec4(0.0f, 2.0f / viewport_dimensions.y, 0.0f, 0.0f),
		vec4(0.0f, 0.0f, 0.0f, 0.0f),
		vec4(-1.0f, -1.0f, 0.0f, 1.0f)
	);

	if (gl_VertexIndex == 0) {
		gl_Position = projection * vec4(rect_x1 + origin.x, rect_y1 + origin.y, 0.0f, 1.0f);
		texcoord = vec2(rect_x1, rect_y1);
	} else if (gl_VertexIndex == 1) {
		gl_Position = projection * vec4(rect_x2 + origin.x, rect_y1 + origin.y, 0.0f, 1.0f);
		texcoord = vec2(rect_x2, rect_y1);
	} else if (gl_VertexIndex == 2) {
		gl_Position = projection * vec4(rect_x1 + origin.x, rect_y2 + origin.y, 0.0f, 1.0f);
		texcoord = vec2(rect_x1, rect_y2);
	} else if (gl_VertexIndex == 3) {
		gl_Position = projection * vec4(rect_x2 + origin.x, rect_y2 + origin.y, 0.0f, 1.0f);
		texcoord = vec2(rect_x2, rect_y2);
	}
}
);

char *blit_fragment_shader = GLSL(460,
layout(location = 0) out vec4 color;
layout(location = 1) in vec2 texcoord;
layout(binding = 0) uniform sampler2D tex;

void main() {
	color = texture(tex, texcoord / textureSize(tex, 0));
}
);

char *fill_vertex_shader = GLSL(460,
layout(push_constant) uniform FillData {
	uvec2 viewport_dimensions;
	int rect_x1;
	int rect_y1;
	int rect_x2;
	int rect_y2;
};

void main() {
	mat4 projection = mat4(
		vec4(2.0f / viewport_dimensions.x, 0.0f, 0.0f, 0.0f),
		vec4(0.0f, 2.0f / viewport_dimensions.y, 0.0f, 0.0f),
		vec4(0.0f, 0.0f, 0.0f, 0.0f),
		vec4(-1.0f, -1.0f, 0.0f, 1.0f)
	);

	if (gl_VertexIndex == 0) {
		gl_Position = projection * vec4(rect_x1, rect_y1, 0.0f, 1.0f);
	} else if (gl_VertexIndex == 1) {
		gl_Position = projection * vec4(rect_x2, rect_y1, 0.0f, 1.0f);
	} else if (gl_VertexIndex == 2) {
		gl_Position = projection * vec4(rect_x1, rect_y2, 0.0f, 1.0f);
	} else if (gl_VertexIndex == 3) {
		gl_Position = projection * vec4(rect_x2, rect_y2, 0.0f, 1.0f);
	}
}
);

char *fill_fragment_shader = GLSL(460,
layout(location = 0) out vec4 out_color;
layout(push_constant) uniform FillData {
	layout(offset = 32) vec4 in_color;
};

void main() {
	out_color = in_color;
}
);
#undef GLSL

#version 300 es

#ifdef GL_ES
precision highp float;
#endif

uniform highp sampler2D image;

layout(shared) uniform ColorMatrix {
  vec4 color_vec_y;
  vec4 color_vec_u;
  vec4 color_vec_v;
  vec2 range_y;
  vec2 range_uv;
};

in vec2 tex;
layout(location = 0) out float color;

void main()
{
	vec3 rgb = texture(image, tex).rgb;
	float y = dot(color_vec_y, vec4(rgb, 1.0));

	color = clamp(floor(y), 0.0, range_y.y) * range_y.x;
}
// Stencil shadow pass fragment shader. The original path had no fragment
// program (color writes are masked; only stencil matters). Core profiles
// and Vulkan want a fragment shader anyway, so emit the engine color.

#include "renderparms.glsl"

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = u_color;
}

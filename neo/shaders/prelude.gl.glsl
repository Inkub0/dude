#version 330 core

// OpenGL 3.3 core prelude. Bindings are assigned at link/init time
// (glUniformBlockBinding, glUniform1i for samplers), so the binding
// macros expand to nothing beyond the std140 layout.
#define UBO_BINDING(b) layout(std140)
#define SAMPLER_BINDING(b)
// GLSL 330 doesn't allow location qualifiers on varyings (matched by name)
#define VARY(loc)
// Transpiled-ARB fragment.position: GL's gl_FragCoord is already bottom-up (the origin the
// ARB programs assume), so no flip here. (Vulkan flips — see prelude.vk.glsl.)
#define RB_WPOS gl_FragCoord

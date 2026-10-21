#version 450
// One fragment shader for every draw: vertex colour × texel. Untextured geometry samples the atlas's white
// texel, so solids, gradients, glow sprites and images share this shader (blend mode is pipeline state).
layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
  outColor = vColor * texture(tex, vUv);
}

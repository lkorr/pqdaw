#version 450
// Draw-list vertex shader: pixel-space positions → clip space via a push-constant transform.
layout(push_constant) uniform Push {
  vec2 scale;      // 2 / target size
  vec2 translate;  // -1, -1
} pc;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;  // R8G8B8A8_UNORM, straight alpha

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main() {
  vUv = inUv;
  vColor = inColor;
  gl_Position = vec4(inPos * pc.scale + pc.translate, 0.0, 1.0);
}

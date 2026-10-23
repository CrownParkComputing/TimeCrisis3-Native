#version 450
layout(set=0,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 fragUV;
layout(location=1) in vec4 fragColor;
layout(location=0) out vec4 outputColor;
void main() { outputColor=fragColor*texture(atlas,fragUV); }

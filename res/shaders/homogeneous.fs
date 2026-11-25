#version 330 core

in vec3 v_position;

uniform vec3 u_background_color;
uniform float u_abs_coeff;
uniform vec3 u_aabb_min;
uniform vec3 u_aabb_max;
uniform vec3 u_local_camera_position;

out vec4 FragColor;

vec2 intersectAABB(vec3 rayOrigin, vec3 rayDir, vec3 boxMin, vec3 boxMax) {
    vec3 tMin = (boxMin - rayOrigin) / rayDir;
    vec3 tMax = (boxMax - rayOrigin) / rayDir;
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar = min(min(t2.x, t2.y), t2.z);
    return vec2(tNear, tFar);
}

void main()
{
	vec3 rayOrigin = u_local_camera_position;
	vec3 rayDir = normalize(v_position - rayOrigin);
	
	float entry_dist = intersectAABB(rayOrigin, rayDir, u_aabb_min, u_aabb_max).x;
	float exit_dist = intersectAABB(rayOrigin, rayDir, u_aabb_min, u_aabb_max).y;

	float optical_thickness = (exit_dist - entry_dist) * u_abs_coeff;
	float T = exp( - optical_thickness);
	vec3 B = u_background_color;
	vec3 L = B * T;

	FragColor = vec4(L, 1.0);
}
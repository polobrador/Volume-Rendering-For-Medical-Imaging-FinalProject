#version 330 core

in vec3 v_position;

uniform vec3 u_background_color;
uniform float u_abs_coeff;
uniform vec3 u_aabb_min;
uniform vec3 u_aabb_max;
uniform vec3 u_local_camera_position;
uniform float u_step_size;
uniform float u_window_center;
uniform float u_window_width;
uniform vec3 u_emission_color;
uniform sampler3D u_volume_texture;
uniform sampler3D u_padding_texture;

out vec4 FragColor;

vec2 intersectAABB(vec3 rayOrigin, vec3 rayDir, vec3 boxMin, vec3 boxMax)
{
    vec3 tMin = (boxMin - rayOrigin) / rayDir;
    vec3 tMax = (boxMax - rayOrigin) / rayDir;
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    return vec2(max(max(t1.x, t1.y), t1.z), min(min(t2.x, t2.y), t2.z));
}

float windowValue(float value)
{
    float width = max(u_window_width, 1.0);
    if (width <= 1.0)
        return value <= u_window_center - 0.5 ? 0.0 : 1.0;

    float lower = u_window_center - 0.5 - (width - 1.0) * 0.5;
    float upper = u_window_center - 0.5 + (width - 1.0) * 0.5;
    if (value <= lower)
        return 0.0;
    if (value > upper)
        return 1.0;
    return (value - (u_window_center - 0.5)) / (width - 1.0) + 0.5;
}

void main()
{
    vec3 rayOrigin = u_local_camera_position;
    vec3 rayDir = normalize(v_position - rayOrigin);
    vec2 hit = intersectAABB(rayOrigin, rayDir, u_aabb_min, u_aabb_max);
    float entryDist = max(hit.x, 0.0);
    float exitDist = hit.y;
    if (exitDist <= entryDist)
        discard;

    float stepSize = max(u_step_size, 0.0001);
    float opticalThickness = 0.0;
    float rayMarch = entryDist + 0.5 * stepSize;
    vec3 accumulatedEmission = vec3(0.0);
    float transmittance = 1.0;

    while (rayMarch < exitDist)
    {
        vec3 samplePosition = rayOrigin + rayMarch * rayDir;
        vec3 uvw = (samplePosition - u_aabb_min) / (u_aabb_max - u_aabb_min);
        if (texture(u_padding_texture, uvw).r < 0.5)
        {
            float modalityValue = texture(u_volume_texture, uvw).r;
            float density = windowValue(modalityValue);
            float sampleAbsorption = density * u_abs_coeff;
            opticalThickness += sampleAbsorption * stepSize;
            transmittance = exp(-opticalThickness);
            accumulatedEmission += sampleAbsorption * u_emission_color *
                transmittance * stepSize;
            if (transmittance < 0.01)
                break;
        }
        rayMarch += stepSize;
    }

    vec3 color = u_background_color * transmittance + accumulatedEmission;
    FragColor = vec4(color, 1.0);
}

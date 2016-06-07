#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in float inLodBias;
layout (location = 3) in vec3 inViewVec;
layout (location = 4) in vec3 inLightVec;

layout (set = 0, binding = 1) uniform UBO
{
	float roughness; 
	float F0; 
	float k; 
	vec4 color;
} material;

layout (set = 1, binding = 2) uniform samplerCube samplerColor;

layout (location = 0) out vec4 outFragColor;

// Cook-Torrance BRDF
// Based on http://ruh.li/GraphicsCookTorrance.html
void main() 
{
    vec3 normal = normalize(inNormal);  
    float NdotL = max(dot(normal, inLightVec), 0.0);
    
    float spec = 0.0;
	float geoAtt = 0.0;
	float roughness = 0.0;
	
    if(NdotL > 0.0)
    {
		// Calculate some intermediate values
        vec3 eyeDir = normalize(inViewVec);		
        vec3 halfVec = normalize(inLightVec + eyeDir);
        float NdotH = max(dot(normal, halfVec), 0.0); 
        float NdotV = max(dot(normal, eyeDir), 0.0); 
        float VdotH = max(dot(eyeDir, halfVec), 0.0);
        float rSquare = material.roughness * material.roughness;
        
        // Geometric attenuation
        float NH2 = 2.0 * NdotH;
        float g1 = (NH2 * NdotV) / VdotH;
        float g2 = (NH2 * NdotL) / VdotH;
        geoAtt = min(1.0, min(g1, g2));
     
        // Roughness (or: microfacet distribution function) - beckmann distribution function
        float r1 = 1.0 / ( 4.0 * rSquare * pow(NdotH, 4.0));
        float r2 = (NdotH * NdotH - 1.0) / (rSquare * NdotH * NdotH);
        roughness = r1 * exp(r2);
        
        // Fresnel (schlick approximation)
        float fresnel = pow(1.0 - VdotH, 5.0);
        fresnel *= (1.0 - material.F0);
        fresnel += material.F0;
        
        spec = (fresnel * geoAtt * roughness) / (NdotV * NdotL * 3.14);
    }
    
    outFragColor = vec4(material.color.rgb * NdotL * (material.k + spec * (1.0 - material.k)), 1.0);
	//outFragColor = vec4(vec3(fresnel), 1.0);		
}
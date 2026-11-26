#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/matrix.hpp>

#include "../framework/camera.h"
#include "mesh.h"
#include "texture.h"
#include "shader.h"
#include <openvdbReader.h>
#include <grid.h>
#include <accessor.h>

class Material {
public:

	Shader* shader = NULL;
	Texture* texture = NULL;
	glm::vec4 color;

	virtual void setUniforms(Camera* camera, glm::mat4 model) = 0;
	virtual void render(Mesh* mesh, glm::mat4 model, Camera* camera) = 0;
	virtual void renderInMenu() = 0;
};

class VolumeMaterial : public Material {
public:

	VolumeMaterial(glm::vec4 color = glm::vec4(1.f));
	~VolumeMaterial();

	float abs_coeff = 0.5;
	float step_size = 0.5;
	int shader_index = 0;
	float scat_coeff = 0.5;
	float g = 0.0;
	bool dicomMode = false;
	GLuint dicomIntensityTexture = 0;
	GLuint dicomPaddingTexture = 0;
	float dicomWindowCenter = 40.f;
	float dicomWindowWidth = 350.f;
	float dicomStepSize = 0.004f;
	Shader* vdbShader = NULL;
	Shader* dicomShader = NULL;

	void setUniforms(Camera* camera, glm::mat4 model) override;
	void render(Mesh* mesh, glm::mat4 model, Camera* camera) override;
	void useDicomVolume(GLuint intensityTexture, GLuint paddingTexture,
		float windowCenter, float windowWidth, float stepSize);
	void useVdbVolume();

	void loadVDB(std::string file_path);
	void estimate3DTexture(easyVDB::OpenVDBReader* vdbReader);



	void renderInMenu() override;
};

class FlatMaterial : public Material {
public:

	FlatMaterial(glm::vec4 color = glm::vec4(1.f));
	~FlatMaterial();

	void setUniforms(Camera* camera, glm::mat4 model);
	void render(Mesh* mesh, glm::mat4 model, Camera* camera);
	void renderInMenu();
};

class WireframeMaterial : public FlatMaterial {
public:

	WireframeMaterial();
	~WireframeMaterial();

	void render(Mesh* mesh, glm::mat4 model, Camera* camera);
};

class StandardMaterial : public Material {
public:

	bool first_pass = false;

	bool show_normals = false;
	Shader* base_shader = NULL;
	Shader* normal_shader = NULL;

	StandardMaterial(glm::vec4 color = glm::vec4(1.f));
	~StandardMaterial();

	void setUniforms(Camera* camera, glm::mat4 model);
	void render(Mesh* mesh, glm::mat4 model, Camera* camera);
	void renderInMenu();
};
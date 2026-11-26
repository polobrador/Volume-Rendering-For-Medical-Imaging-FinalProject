#pragma once

#include "framework/includes.h"
#include "framework/camera.h"
#include "framework/scenenode.h"
#include "framework/light.h"
#include "dicom/dicom_metadata_reader.h"

#include <array>
#include <glm/vec2.hpp>

class VolumeMaterial;

class Application
{
public:
	static Application* instance;
	
	static Camera* camera;
	std::vector<SceneNode*> node_list;
	SceneNode* volumeNode = nullptr;
	VolumeMaterial* volumeMaterial = nullptr;
	glm::vec4 ambient_light;
	glm::vec3 background_color;
	std::vector<Light*> light_list;

	int window_width;
	int window_height;

	bool flag_grid;
	bool flag_wireframe;

	bool close = false;
	bool dragging;
	glm::vec2 mousePosition;
	glm::vec2 lastMousePosition;
	std::array<char, 1024> dicomFolderPath{};
	DicomFolderInspection dicomFolderInspection;
	std::string dicomFolderStatus;
	int selectedDicomAcquisition = 0;
	bool dicomFolderLoaded = false;
	DicomSlicePixels dicomSlicePixels;
	DicomVolume dicomVolume;
	std::string dicomSliceStatus;
	std::string dicomVolumeStatus;
	std::string dicomGpuTextureStatus;
	GLuint dicomPreviewTexture = 0;
	GLuint dicomVolumeTexture = 0;
	GLuint dicomPaddingTexture = 0;
	bool dicomSliceReady = false;
	bool dicomVolumeReady = false;
	bool dicomVolumeUploaded = false;
	int dicomPreviewAcquisition = -1;
	int dicomPreviewSliceIndex = -1;
	float dicomWindowCenter = 40.f;
	float dicomWindowWidth = 350.f;
	double dicomGpuTextureMiB = 0.0;

	void init(GLFWwindow* window);
	void update(float dt);
	void render();
	void renderGUI();
	void shutdown();
	~Application();

	void onKeyDown(int key, int scancode);
	void onKeyUp(int key, int scancode);
	void onRightMouseDown();
	void onRightMouseUp();
	void onLeftMouseDown();
	void onLeftMouseUp();
	void onMiddleMouseDown();
	void onMiddleMouseUp();
	void onMousePosition(double xpos, double ypos);
	void onScroll(double xOffset, double yOffset);
	void onWindowSize(int width, int height);

private:
	void loadDicomPreviewSlice(const DicomAcquisitionSummary& acquisition, int sliceIndex);
	void updateDicomPreviewTexture();
	void releaseDicomVolumeTexture();
	void uploadDicomVolumeTexture();
};
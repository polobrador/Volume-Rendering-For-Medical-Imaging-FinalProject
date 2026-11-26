#include "application.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

bool render_wireframe = false;
Camera* Application::camera = nullptr;

void Application::init(GLFWwindow* window)
{
    this->instance = this;
    glfwGetFramebufferSize(window, &this->window_width, &this->window_height);

    // OpenGL flags
    glEnable(GL_CULL_FACE); // render both sides of every triangle
    glEnable(GL_DEPTH_TEST); // check the occlusions using the Z buffer

    // Create camera
    this->camera = new Camera();
    this->camera->lookAt(glm::vec3(1.f, 1.5f, 4.f), glm::vec3(0.f, 0.0f, 0.f), glm::vec3(0.f, 1.f, 0.f));
    this->camera->setPerspective(60.f, this->window_width / (float)this->window_height, 0.1f, 500.f); // set the projection to perspective

    this->flag_grid = true;
    this->flag_wireframe = false;

    this->ambient_light = glm::vec4(0.75f, 0.75f, 0.75f, 1.f);
    this->background_color = glm::vec3(0.1f, 0.1f, 0.1f);

    /* ADD NODES TO THE SCENE */
    /*
    SceneNode* example = new SceneNode("Example Node");
    example->mesh = Mesh::Get("res/meshes/sphere.obj");
    example->material = new StandardMaterial();
    this->node_list.push_back(example);
    */

    // Add the volume node
    SceneNode* volume = new SceneNode("Volume Node");
    volume->mesh = Mesh::Get("res/meshes/cube.obj");
    this->volumeMaterial = new VolumeMaterial();
    volume->material = this->volumeMaterial;
    this->volumeNode = volume;
    this->node_list.push_back(volume);

	// Add light source to list of nodes and lights
	Light* light_source = new Light(glm::vec3(-1.5f, 2.0f, 2.0f));
	this->node_list.push_back(light_source);
	this->light_list.push_back(light_source);

}


void Application::update(float dt)
{
    // Mouse update
    glm::vec2 delta = this->lastMousePosition - this->mousePosition;
    if (this->dragging) {
        this->camera->orbit(-delta.x * dt, delta.y * dt);
    }
    this->lastMousePosition = this->mousePosition;
}

void Application::render()
{
    // Set the clear color (the background color)
    glClearColor(background_color.x, background_color.y, background_color.z, 1.0f);

    // Clear the window and the depth buffer
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set flags
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    for (unsigned int i = 0; i < this->node_list.size(); i++)
    {
        this->node_list[i]->render(this->camera);

        if (this->flag_wireframe) this->node_list[i]->renderWireframe(this->camera);
    }

    // Draw the floor grid
    if (this->flag_grid) drawGrid();
}

void Application::renderGUI()
{
    if (ImGui::TreeNodeEx("DICOM CT Folder Inspection", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputText("Folder path", this->dicomFolderPath.data(), this->dicomFolderPath.size());
        ImGui::SameLine();
        if (ImGui::Button("Inspect"))
        {
            DicomFolderInspection inspection;
            std::string errorMessage;
            if (DicomMetadataReader::inspectFolder(this->dicomFolderPath.data(), inspection, errorMessage))
            {
                this->dicomFolderInspection = std::move(inspection);
                this->selectedDicomAcquisition = 0;
                this->dicomFolderStatus.clear();
                this->dicomFolderLoaded = true;
                this->dicomSliceReady = false;
                this->dicomPreviewAcquisition = -1;
                this->dicomPreviewSliceIndex = -1;
                this->dicomSliceStatus.clear();
                this->dicomVolume = {};
                this->dicomVolumeStatus.clear();
                this->dicomVolumeReady = false;
                this->releaseDicomVolumeTexture();
            }
            else
            {
                this->dicomFolderStatus = std::move(errorMessage);
                this->dicomFolderLoaded = false;
                this->dicomVolume = {};
                this->dicomVolumeReady = false;
                this->dicomVolumeStatus.clear();
                this->releaseDicomVolumeTexture();
            }
        }

        if (!this->dicomFolderStatus.empty())
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s", this->dicomFolderStatus.c_str());

        if (this->dicomFolderLoaded)
        {
            ImGui::Text("CT acquisition groups: %zu | unreadable files: %zu | non-CT files: %zu",
                this->dicomFolderInspection.acquisitions.size(),
                this->dicomFolderInspection.unreadableFiles,
                this->dicomFolderInspection.nonCtFiles);

            std::vector<const char*> acquisitionLabels;
            acquisitionLabels.reserve(this->dicomFolderInspection.acquisitions.size());
            for (const DicomAcquisitionSummary& acquisition : this->dicomFolderInspection.acquisitions)
                acquisitionLabels.push_back(acquisition.label.c_str());

            if (!acquisitionLabels.empty())
            {
                this->selectedDicomAcquisition = std::clamp(
                    this->selectedDicomAcquisition, 0,
                    static_cast<int>(acquisitionLabels.size()) - 1);
                const bool acquisitionChanged = ImGui::Combo("Acquisition",
                    &this->selectedDicomAcquisition, acquisitionLabels.data(),
                    static_cast<int>(acquisitionLabels.size()));

                if (acquisitionChanged)
                {
                    this->dicomVolume = {};
                    this->dicomVolumeStatus.clear();
                    this->dicomVolumeReady = false;
                    this->releaseDicomVolumeTexture();
                }

                const DicomAcquisitionSummary& selected =
                    this->dicomFolderInspection.acquisitions[this->selectedDicomAcquisition];
                ImGui::Text("Slices: %zu | Image size: %u x %u",
                    selected.sliceCount, selected.columns, selected.rows);
                ImGui::Text("Pixel spacing (row, column): %.6f, %.6f mm",
                    selected.rowSpacingMm, selected.columnSpacingMm);
                if (selected.minimumSliceSpacingMm > 0.0)
                    ImGui::Text("Slice spacing (min / mean / max): %.4f / %.4f / %.4f mm",
                        selected.minimumSliceSpacingMm,
                        selected.meanSliceSpacingMm,
                        selected.maximumSliceSpacingMm);
                ImGui::Text("Geometry ready for a regular volume: %s",
                    selected.hasConsistentGeometry ? "yes" : "no - inspect warnings before loading");
                if (selected.duplicatePositionCount > 0)
                    ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f),
                        "Repeated slice positions: %zu", selected.duplicatePositionCount);
                if (selected.multiFrameInstanceCount > 0)
                    ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f),
                        "Multi-frame instances not supported yet: %zu", selected.multiFrameInstanceCount);
                if (selected.hasConsistentGeometry)
                {
                    const double voxelCount = static_cast<double>(selected.rows) *
                        selected.columns * selected.sliceCount;
                    const double estimatedMiB = voxelCount *
                        (sizeof(float) + sizeof(unsigned char)) / (1024.0 * 1024.0);
                    ImGui::Text("Full-resolution CPU memory estimate: %.1f MiB", estimatedMiB);
                    if (ImGui::Button("Assemble CPU volume"))
                    {
                        DicomVolume assembled;
                        std::string errorMessage;
                        if (DicomMetadataReader::assembleVolume(selected, assembled, errorMessage))
                        {
                            this->dicomVolume = std::move(assembled);
                            this->dicomVolumeStatus.clear();
                            this->dicomVolumeReady = true;
                            this->releaseDicomVolumeTexture();
                        }
                        else
                        {
                            this->dicomVolume = {};
                            this->dicomVolumeStatus = std::move(errorMessage);
                            this->dicomVolumeReady = false;
                            this->releaseDicomVolumeTexture();
                        }
                    }
                }
                else
                {
                    ImGui::TextWrapped("Volume assembly is disabled because this acquisition does not have validated regular geometry.");
                }
                if (!this->dicomVolumeStatus.empty())
                    ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f),
                        "%s", this->dicomVolumeStatus.c_str());
                if (this->dicomVolumeReady)
                {
                    ImGui::Text("CPU volume assembled: %u x %u x %u voxels",
                        this->dicomVolume.width, this->dicomVolume.height,
                        this->dicomVolume.depth);
                    ImGui::Text("Voxel spacing (column, row, slice): %.6f, %.6f, %.6f mm",
                        this->dicomVolume.columnSpacingMm,
                        this->dicomVolume.rowSpacingMm,
                        std::sqrt(
                            this->dicomVolume.sliceStepVectorMm[0] *
                                this->dicomVolume.sliceStepVectorMm[0] +
                            this->dicomVolume.sliceStepVectorMm[1] *
                                this->dicomVolume.sliceStepVectorMm[1] +
                            this->dicomVolume.sliceStepVectorMm[2] *
                                this->dicomVolume.sliceStepVectorMm[2]));
                    if (this->dicomVolume.hasNonPaddingVoxels)
                        ImGui::Text("Modality value range (excluding padding): %.1f to %.1f",
                            this->dicomVolume.minimumValue, this->dicomVolume.maximumValue);
                    else
                        ImGui::Text("The assembled volume contains only padding voxels.");

                    GLint maximumTextureSize = 0;
                    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &maximumTextureSize);
                    const bool dimensionsFitGpu =
                        maximumTextureSize > 0 &&
                        this->dicomVolume.width <= static_cast<unsigned int>(maximumTextureSize) &&
                        this->dicomVolume.height <= static_cast<unsigned int>(maximumTextureSize) &&
                        this->dicomVolume.depth <= static_cast<unsigned int>(maximumTextureSize);
                    const double requiredGpuMiB =
                        (static_cast<double>(this->dicomVolume.modalityValues.size()) *
                            sizeof(float) + this->dicomVolume.paddingMask.size()) /
                        (1024.0 * 1024.0);
                    ImGui::Text("GPU texture estimate (R32F values + R8 padding mask): %.1f MiB",
                        requiredGpuMiB);
                    ImGui::Text("Maximum 3D texture dimension: %d", maximumTextureSize);
                    if (dimensionsFitGpu)
                    {
                        if (ImGui::Button("Upload 3D texture to GPU"))
                            this->uploadDicomVolumeTexture();
                    }
                    else
                    {
                        ImGui::TextWrapped("This volume exceeds the GPU's maximum 3D texture dimension.");
                    }
                    if (this->dicomVolumeUploaded)
                    {
                        ImGui::Text("GPU upload complete (%.1f MiB); GL_LINEAR intensity filtering enabled.",
                            this->dicomGpuTextureMiB);
                        if (this->volumeMaterial->dicomMode)
                        {
                            if (ImGui::Button("Use VDB bunny"))
                            {
                                this->volumeMaterial->useVdbVolume();
                                this->volumeNode->model = glm::mat4(1.f);
                            }
                        }
                        else if (ImGui::Button("Render CT volume"))
                        {
                            const std::array<double, 3> columnVector = {
                                this->dicomVolume.orientation[0] *
                                    this->dicomVolume.columnSpacingMm * this->dicomVolume.width,
                                this->dicomVolume.orientation[1] *
                                    this->dicomVolume.columnSpacingMm * this->dicomVolume.width,
                                this->dicomVolume.orientation[2] *
                                    this->dicomVolume.columnSpacingMm * this->dicomVolume.width
                            };
                            const std::array<double, 3> rowVector = {
                                this->dicomVolume.orientation[3] *
                                    this->dicomVolume.rowSpacingMm * this->dicomVolume.height,
                                this->dicomVolume.orientation[4] *
                                    this->dicomVolume.rowSpacingMm * this->dicomVolume.height,
                                this->dicomVolume.orientation[5] *
                                    this->dicomVolume.rowSpacingMm * this->dicomVolume.height
                            };
                            const std::array<double, 3>& sliceVector =
                                this->dicomVolume.sliceStepVectorMm;
                            const double columnLength = std::sqrt(
                                columnVector[0] * columnVector[0] +
                                columnVector[1] * columnVector[1] +
                                columnVector[2] * columnVector[2]);
                            const double rowLength = std::sqrt(
                                rowVector[0] * rowVector[0] +
                                rowVector[1] * rowVector[1] +
                                rowVector[2] * rowVector[2]);
                            const double sliceLength = std::sqrt(
                                sliceVector[0] * sliceVector[0] +
                                sliceVector[1] * sliceVector[1] +
                                sliceVector[2] * sliceVector[2]) * this->dicomVolume.depth;
                            const double maximumExtent =
                                std::max({ columnLength, rowLength, sliceLength });
                            const float modelScale = static_cast<float>(2.0 / maximumExtent);
                            glm::mat4 model(1.f);
                            model[0] = glm::vec4(
                                static_cast<float>(columnVector[0]) * modelScale * 0.5f,
                                static_cast<float>(columnVector[1]) * modelScale * 0.5f,
                                static_cast<float>(columnVector[2]) * modelScale * 0.5f, 0.f);
                            model[1] = glm::vec4(
                                static_cast<float>(rowVector[0]) * modelScale * 0.5f,
                                static_cast<float>(rowVector[1]) * modelScale * 0.5f,
                                static_cast<float>(rowVector[2]) * modelScale * 0.5f, 0.f);
                            model[2] = glm::vec4(
                                static_cast<float>(sliceVector[0] * this->dicomVolume.depth) *
                                    modelScale * 0.5f,
                                static_cast<float>(sliceVector[1] * this->dicomVolume.depth) *
                                    modelScale * 0.5f,
                                static_cast<float>(sliceVector[2] * this->dicomVolume.depth) *
                                    modelScale * 0.5f, 0.f);
                            const unsigned int maximumDimension = std::max({
                                this->dicomVolume.width,
                                this->dicomVolume.height,
                                this->dicomVolume.depth });
                            this->volumeMaterial->useDicomVolume(
                                this->dicomVolumeTexture,
                                this->dicomPaddingTexture,
                                this->dicomWindowCenter,
                                this->dicomWindowWidth,
                                2.f / static_cast<float>(maximumDimension));
                            this->volumeNode->model = model;
                        }
                    }
                    if (!this->dicomGpuTextureStatus.empty())
                        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f),
                            "%s", this->dicomGpuTextureStatus.c_str());
                }
                if (acquisitionChanged || this->dicomPreviewAcquisition != this->selectedDicomAcquisition)
                    this->loadDicomPreviewSlice(selected, 0);

                if (!selected.orderedFiles.empty())
                {
                    int selectedSlice = this->dicomPreviewAcquisition == this->selectedDicomAcquisition &&
                        this->dicomPreviewSliceIndex >= 0
                        ? this->dicomPreviewSliceIndex + 1 : 1;
                    if (ImGui::SliderInt("Slice", &selectedSlice, 1,
                        static_cast<int>(selected.orderedFiles.size())))
                        this->loadDicomPreviewSlice(selected, selectedSlice - 1);

                    if (this->dicomSliceReady)
                    {
                        ImGui::InputFloat("Window center", &this->dicomWindowCenter, 1.f, 10.f, "%.1f");
                        ImGui::InputFloat("Window width", &this->dicomWindowWidth, 1.f, 10.f, "%.1f");
                        if (ImGui::Button("Apply window"))
                            this->updateDicomPreviewTexture();
                        ImGui::SameLine();
                        ImGui::Text("Modality values: slope/intercept applied");
                        if (!this->dicomSliceStatus.empty())
                            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f),
                                "%s", this->dicomSliceStatus.c_str());
                        const float availableWidth = ImGui::GetContentRegionAvail().x;
                        const float imageWidth = std::max(1.f, std::min(availableWidth,
                            static_cast<float>(this->dicomSlicePixels.columns)));
                        const float imageHeight = imageWidth * this->dicomSlicePixels.rows /
                            this->dicomSlicePixels.columns;
                        ImGui::Image((ImTextureID)(intptr_t)this->dicomPreviewTexture,
                            ImVec2(imageWidth, imageHeight));
                    }
                    else if (!this->dicomSliceStatus.empty())
                    {
                        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f),
                            "%s", this->dicomSliceStatus.c_str());
                    }
                }
                ImGui::TextWrapped("The CT volume can be rendered after upload. Use VDB bunny to return to the original VDB volume.");
            }
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::ColorEdit3("Ambient light", (float*)&this->ambient_light);
        ImGui::ColorEdit3("Background color", (float*)&this->background_color);

        if (ImGui::TreeNode("Camera")) {
            this->camera->renderInMenu();
            ImGui::TreePop();
        }

        unsigned int count = 0;
        std::stringstream ss;
        for (auto& node : this->node_list) {
            ss << count;
            if (ImGui::TreeNode(node->name.c_str())) {
                node->renderInMenu();
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }
}

void Application::loadDicomPreviewSlice(const DicomAcquisitionSummary& acquisition, int sliceIndex)
{
	this->dicomSliceReady = false;
	this->dicomPreviewAcquisition = this->selectedDicomAcquisition;
	this->dicomPreviewSliceIndex = sliceIndex;
	if (sliceIndex < 0 || static_cast<size_t>(sliceIndex) >= acquisition.orderedFiles.size())
	{
		this->dicomSliceStatus = "The selected slice index is outside this acquisition.";
		return;
	}

	DicomSlicePixels decoded;
	std::string errorMessage;
	if (!DicomMetadataReader::decodeMonochromeSlice(
		acquisition.orderedFiles[static_cast<size_t>(sliceIndex)], decoded, errorMessage))
	{
		this->dicomSliceStatus = std::move(errorMessage);
		return;
	}

	this->dicomSlicePixels = std::move(decoded);
	this->dicomWindowCenter = static_cast<float>(this->dicomSlicePixels.windowCenter);
	this->dicomWindowWidth = static_cast<float>(this->dicomSlicePixels.windowWidth);
	this->dicomSliceStatus.clear();
	this->dicomSliceReady = true;
	this->updateDicomPreviewTexture();
}

void Application::updateDicomPreviewTexture()
{
	if (!this->dicomSliceReady || this->dicomSlicePixels.columns == 0 ||
		this->dicomSlicePixels.rows == 0)
		return;
	if (!std::isfinite(this->dicomWindowCenter) ||
		!std::isfinite(this->dicomWindowWidth) || this->dicomWindowWidth <= 0.f)
	{
		this->dicomSliceStatus = "Window center must be finite and width must be greater than zero.";
		return;
	}

	const size_t pixelCount = this->dicomSlicePixels.modalityValues.size();
	std::vector<unsigned char> rgba(pixelCount * 4);
	const double center = this->dicomWindowCenter;
	const double width = std::max(1.0, static_cast<double>(this->dicomWindowWidth));
	const double lower = center - 0.5 - (width - 1.0) * 0.5;
	const double upper = center - 0.5 + (width - 1.0) * 0.5;
	for (size_t index = 0; index < pixelCount; ++index)
	{
		double normalized = width <= 1.0
			? (this->dicomSlicePixels.modalityValues[index] <= center - 0.5 ? 0.0 : 1.0)
			: (this->dicomSlicePixels.modalityValues[index] <= lower ? 0.0
				: this->dicomSlicePixels.modalityValues[index] > upper ? 1.0
				: ((this->dicomSlicePixels.modalityValues[index] - (center - 0.5)) /
					(width - 1.0) + 0.5));
		if (this->dicomSlicePixels.monochrome1)
			normalized = 1.0 - normalized;
		if (this->dicomSlicePixels.paddingMask[index])
			normalized = 0.0;

		const unsigned char gray = static_cast<unsigned char>(
			std::clamp(std::lround(normalized * 255.0), 0L, 255L));
		rgba[index * 4] = gray;
		rgba[index * 4 + 1] = gray;
		rgba[index * 4 + 2] = gray;
		rgba[index * 4 + 3] = 255;
	}

	if (this->dicomPreviewTexture == 0)
		glGenTextures(1, &this->dicomPreviewTexture);
	if (this->dicomPreviewTexture == 0)
	{
		this->dicomSliceStatus = "OpenGL could not allocate a texture for the DICOM preview.";
		this->dicomSliceReady = false;
		return;
	}
	glBindTexture(GL_TEXTURE_2D, this->dicomPreviewTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
		static_cast<GLsizei>(this->dicomSlicePixels.columns),
		static_cast<GLsizei>(this->dicomSlicePixels.rows),
		0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
	this->dicomSliceStatus.clear();
}

void Application::releaseDicomVolumeTexture()
{
	if (this->volumeMaterial && this->volumeMaterial->dicomMode)
	{
		this->volumeMaterial->useVdbVolume();
		if (this->volumeNode)
			this->volumeNode->model = glm::mat4(1.f);
	}
	const GLuint textures[] = { this->dicomVolumeTexture, this->dicomPaddingTexture };
	glDeleteTextures(2, textures);
	this->dicomVolumeTexture = 0;
	this->dicomPaddingTexture = 0;
	this->dicomVolumeUploaded = false;
	this->dicomGpuTextureMiB = 0.0;
	this->dicomGpuTextureStatus.clear();
}

void Application::uploadDicomVolumeTexture()
{
	if (!this->dicomVolumeReady ||
		this->dicomVolume.modalityValues.empty() ||
		this->dicomVolume.width == 0 ||
		this->dicomVolume.height == 0 ||
		this->dicomVolume.depth == 0)
	{
		this->dicomGpuTextureStatus = "Assemble a valid CPU volume before uploading it.";
		return;
	}

	GLint maximumTextureSize = 0;
	glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &maximumTextureSize);
	if (maximumTextureSize <= 0 ||
		this->dicomVolume.width > static_cast<unsigned int>(maximumTextureSize) ||
		this->dicomVolume.height > static_cast<unsigned int>(maximumTextureSize) ||
		this->dicomVolume.depth > static_cast<unsigned int>(maximumTextureSize))
	{
		this->dicomGpuTextureStatus = "The volume dimensions exceed the GPU's maximum 3D texture size.";
		return;
	}

	const GLenum existingError = glGetError();
	if (existingError != GL_NO_ERROR)
	{
		this->dicomGpuTextureStatus = "OpenGL already has an outstanding error; resolve it before uploading the CT volume.";
		return;
	}

	this->releaseDicomVolumeTexture();
	GLuint textures[2] = {};
	glGenTextures(2, textures);
	if (textures[0] == 0 || textures[1] == 0)
	{
		glDeleteTextures(2, textures);
		this->dicomGpuTextureStatus = "OpenGL could not allocate texture objects for the CT volume and padding mask.";
		return;
	}

	glBindTexture(GL_TEXTURE_3D, textures[0]);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F,
		static_cast<GLsizei>(this->dicomVolume.width),
		static_cast<GLsizei>(this->dicomVolume.height),
		static_cast<GLsizei>(this->dicomVolume.depth),
		0, GL_RED, GL_FLOAT, this->dicomVolume.modalityValues.data());
	glBindTexture(GL_TEXTURE_3D, textures[1]);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_R8,
		static_cast<GLsizei>(this->dicomVolume.width),
		static_cast<GLsizei>(this->dicomVolume.height),
		static_cast<GLsizei>(this->dicomVolume.depth),
		0, GL_RED, GL_UNSIGNED_BYTE, this->dicomVolume.paddingMask.data());
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_3D, 0);

	const GLenum uploadError = glGetError();
	if (uploadError != GL_NO_ERROR)
	{
		glDeleteTextures(2, textures);
		this->dicomGpuTextureStatus = uploadError == GL_OUT_OF_MEMORY
			? "The GPU ran out of memory while allocating or uploading the full-resolution CT texture."
			: "OpenGL reported an error while uploading the CT 3D texture.";
		return;
	}

	this->dicomVolumeTexture = textures[0];
	this->dicomPaddingTexture = textures[1];
	this->dicomVolumeUploaded = true;
	this->dicomGpuTextureMiB =
		(static_cast<double>(this->dicomVolume.modalityValues.size()) *
			sizeof(float) + this->dicomVolume.paddingMask.size()) /
		(1024.0 * 1024.0);
	this->dicomGpuTextureStatus.clear();
}

Application::~Application()
{
	if (this->dicomPreviewTexture != 0)
		glDeleteTextures(1, &this->dicomPreviewTexture);
	this->releaseDicomVolumeTexture();
}

void Application::shutdown() { }

// keycodes: https://www.glfw.org/docs/3.3/group__keys.html
void Application::onKeyDown(int key, int scancode)
{
    switch (key) {
    case GLFW_KEY_ESCAPE: // quit
        close = true;
        break;
    case GLFW_KEY_R:
        Shader::ReloadAll();
        break;
    }
}

// keycodes: https://www.glfw.org/docs/3.3/group__keys.html
void Application::onKeyUp(int key, int scancode)
{
    switch (key) {
    case GLFW_KEY_T:
        std::cout << "T released" << std::endl;
        break;
    }
}

void Application::onRightMouseDown()
{
    this->dragging = true;
    this->lastMousePosition = this->mousePosition;
}

void Application::onRightMouseUp()
{
    this->dragging = false;
    this->lastMousePosition = this->mousePosition;
}

void Application::onLeftMouseDown()
{
    this->dragging = true;
    this->lastMousePosition = this->mousePosition;
}

void Application::onLeftMouseUp()
{
    this->dragging = false;
    this->lastMousePosition = this->mousePosition;
}

void Application::onMiddleMouseDown() { }

void Application::onMiddleMouseUp() { }

void Application::onMousePosition(double xpos, double ypos) { }

void Application::onScroll(double xOffset, double yOffset)
{
    int min = (int)this->camera->min_fov;
    int max = (int)this->camera->max_fov;

    if (yOffset < 0) {
        this->camera->fov += 4.f;
        if (this->camera->fov > max) {
            this->camera->fov = (float)max;
        }
    }
    else {
        this->camera->fov -= 4.f;
        if (this->camera->fov < min) {
            this->camera->fov = (float)min;
        }
    }
    this->camera->updateProjectionMatrix();
}

void Application::onWindowSize(int width, int height)
{
    glViewport(0, 0, width, height);
    this->camera->setAspectRatio(width / (float)height);
    this->camera->updateProjectionMatrix();
}
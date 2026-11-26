#pragma once

#include <filesystem>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

struct DicomAcquisitionSummary
{
	std::string label;
	size_t sliceCount = 0;
	unsigned int rows = 0;
	unsigned int columns = 0;
	std::array<double, 6> orientation{};
	std::array<double, 3> originMm{};
	std::array<double, 3> sliceStepVectorMm{};
	double rowSpacingMm = 0.0;
	double columnSpacingMm = 0.0;
	double minimumSliceSpacingMm = 0.0;
	double meanSliceSpacingMm = 0.0;
	double maximumSliceSpacingMm = 0.0;
	bool hasRegularSliceSpacing = false;
	bool hasConsistentGeometry = false;
	size_t duplicatePositionCount = 0;
	size_t multiFrameInstanceCount = 0;
	std::vector<std::filesystem::path> orderedFiles;
};

struct DicomFolderInspection
{
	std::vector<DicomAcquisitionSummary> acquisitions;
	size_t unreadableFiles = 0;
	size_t nonCtFiles = 0;
};

struct DicomSlicePixels
{
	unsigned int rows = 0;
	unsigned int columns = 0;
	std::array<double, 3> originMm{};
	std::array<double, 6> orientation{};
	double rowSpacingMm = 0.0;
	double columnSpacingMm = 0.0;
	bool hasGeometry = false;
	double windowCenter = 0.0;
	double windowWidth = 0.0;
	bool monochrome1 = false;
	std::vector<float> modalityValues;
	std::vector<unsigned char> paddingMask;
};

struct DicomVolume
{
	unsigned int width = 0;
	unsigned int height = 0;
	unsigned int depth = 0;
	std::array<double, 3> originMm{};
	std::array<double, 6> orientation{};
	std::array<double, 3> sliceStepVectorMm{};
	double rowSpacingMm = 0.0;
	double columnSpacingMm = 0.0;
	double minimumValue = 0.0;
	double maximumValue = 0.0;
	bool hasNonPaddingVoxels = false;
	std::vector<float> modalityValues;
	std::vector<unsigned char> paddingMask;
};

class DicomMetadataReader
{
public:
	static bool inspectFolder(
		const std::filesystem::path& folderPath,
		DicomFolderInspection& inspection,
		std::string& errorMessage);

	static bool decodeMonochromeSlice(
		const std::filesystem::path& filePath,
		DicomSlicePixels& slice,
		std::string& errorMessage);

	static bool assembleVolume(
		const DicomAcquisitionSummary& acquisition,
		DicomVolume& volume,
		std::string& errorMessage);

	static bool writeReport(
		const std::filesystem::path& inputPath,
		const std::filesystem::path& reportPath,
		std::string& errorMessage);
};

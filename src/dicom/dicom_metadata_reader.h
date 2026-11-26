#pragma once

#include <filesystem>
#include <string>

class DicomMetadataReader
{
public:
	static bool writeReport(
		const std::filesystem::path& inputPath,
		const std::filesystem::path& reportPath,
		std::string& errorMessage);
};

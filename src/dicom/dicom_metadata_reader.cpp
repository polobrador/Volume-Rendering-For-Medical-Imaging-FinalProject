#include "dicom_metadata_reader.h"

#include <gdcmReader.h>
#include <gdcmStringFilter.h>
#include <gdcmTag.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace
{
	struct MetadataField
	{
		uint16_t group;
		uint16_t element;
		const char* name;
		const char* purpose;
	};

	const MetadataField metadataFields[] = {
		{ 0x0008, 0x0008, "Image Type", "Identifies the image's role and acquisition plane." },
		{ 0x0008, 0x0060, "Modality", "Identifies the imaging modality, such as CT." },
		{ 0x0008, 0x103e, "Series Description", "Describes the type or purpose of this image series." },
		{ 0x0018, 0x0050, "Slice Thickness", "Describes reconstructed slice thickness; it is not necessarily the distance between slices." },
		{ 0x0018, 0x0088, "Spacing Between Slices", "Describes the distance between adjacent slice centers when supplied." },
		{ 0x0018, 0x1030, "Protocol Name", "Describes the acquisition protocol." },
		{ 0x0020, 0x0012, "Acquisition Number", "Can distinguish separate acquisitions included in a series." },
		{ 0x0020, 0x0011, "Series Number", "Scanner-assigned display number for the series." },
		{ 0x0020, 0x0013, "Instance Number", "Scanner-assigned image number; it is not used to determine spatial order." },
		{ 0x0020, 0x0032, "Image Position (Patient)", "Gives this image plane's origin in patient coordinates, in millimeters." },
		{ 0x0020, 0x0037, "Image Orientation (Patient)", "Gives the row and column direction cosines in patient coordinates." },
		{ 0x0020, 0x0100, "Temporal Position Identifier", "Can distinguish repeated time points within a series." },
		{ 0x0028, 0x0002, "Samples per Pixel", "Shows how many samples each pixel contains." },
		{ 0x0028, 0x0004, "Photometric Interpretation", "Describes how pixel samples map to grayscale or color." },
		{ 0x0028, 0x0008, "Number of Frames", "If present, gives the number of frames in this DICOM instance." },
		{ 0x0028, 0x0010, "Rows", "Number of pixel rows in the image." },
		{ 0x0028, 0x0011, "Columns", "Number of pixel columns in the image." },
		{ 0x0028, 0x0030, "Pixel Spacing", "Physical spacing in millimeters, ordered row spacing then column spacing." },
		{ 0x0028, 0x0100, "Bits Allocated", "Number of bits used to store each pixel sample." },
		{ 0x0028, 0x0101, "Bits Stored", "Number of meaningful bits in each stored pixel sample." },
		{ 0x0028, 0x0102, "High Bit", "Position of the most significant stored pixel bit." },
		{ 0x0028, 0x0103, "Pixel Representation", "0 means unsigned pixels; 1 means signed pixels." },
		{ 0x0028, 0x0120, "Pixel Padding Value", "Marks a stored pixel value used for padding rather than anatomy." },
		{ 0x0028, 0x0121, "Pixel Padding Range Limit", "If present, gives the other end of the pixel padding range." },
		{ 0x0028, 0x1050, "Window Center", "Suggested display intensity center; it does not change stored pixel values." },
		{ 0x0028, 0x1051, "Window Width", "Suggested display intensity range; it does not change stored pixel values." },
		{ 0x0028, 0x1052, "Rescale Intercept", "Used with Rescale Slope to convert stored values to modality values." },
		{ 0x0028, 0x1053, "Rescale Slope", "Modality value = stored value x slope + intercept." },
		{ 0x0028, 0x1054, "Rescale Type", "Names the units of rescaled values, for example HU." }
	};

	const gdcm::Tag seriesInstanceUidTag(0x0020, 0x000e);
	const gdcm::Tag modalityTag(0x0008, 0x0060);

	struct SliceMetadata
	{
		std::string seriesKey;
		std::map<std::string, std::string> values;
		std::vector<double> position;
		std::vector<double> orientation;
		double sliceCoordinate = 0.0;
		bool hasSliceCoordinate = false;
	};

	std::string makePrintable(const std::string& value)
	{
		std::string printable;
		printable.reserve(value.size());
		for (unsigned char character : value)
			printable.push_back(std::isprint(character) || character == '\\' ? static_cast<char>(character) : ' ');

		const size_t first = printable.find_first_not_of(" \t");
		if (first == std::string::npos)
			return {};
		const size_t last = printable.find_last_not_of(" \t");
		return printable.substr(first, last - first + 1);
	}

	std::string tagValue(const gdcm::StringFilter& stringFilter,
		const gdcm::DataSet& dataSet, const gdcm::Tag& tag)
	{
		if (!dataSet.FindDataElement(tag))
			return {};
		return makePrintable(stringFilter.ToString(tag));
	}

	std::vector<double> parseComponents(const std::string& value)
	{
		std::vector<double> components;
		std::stringstream stream(value);
		std::string component;
		while (std::getline(stream, component, '\\'))
		{
			try
			{
				size_t parsedLength = 0;
				const double parsed = std::stod(component, &parsedLength);
				if (parsedLength != component.size() || !std::isfinite(parsed))
					return {};
				components.push_back(parsed);
			}
			catch (const std::exception&)
			{
				return {};
			}
		}
		return components;
	}

	std::string tagKey(const MetadataField& field)
	{
		return std::to_string(field.group) + ":" + std::to_string(field.element);
	}

	std::string valueFor(const SliceMetadata& slice, const MetadataField& field)
	{
		const auto value = slice.values.find(tagKey(field));
		return value == slice.values.end() ? std::string() : value->second;
	}

	std::string valueFor(const SliceMetadata& slice, uint16_t group, uint16_t element)
	{
		const std::string key = std::to_string(group) + ":" + std::to_string(element);
		const auto value = slice.values.find(key);
		return value == slice.values.end() ? std::string() : value->second;
	}

	std::string displayValue(const std::string& value)
	{
		return value.empty() ? "<not present>" : value;
	}

	bool readSlice(const std::filesystem::path& path, SliceMetadata& slice)
	{
		std::set<gdcm::Tag> selectedTags = {
			seriesInstanceUidTag,
			modalityTag
		};
		for (const MetadataField& field : metadataFields)
			selectedTags.emplace(field.group, field.element);

		gdcm::Reader reader;
		const std::string nativePath = path.string();
		reader.SetFileName(nativePath.c_str());
		if (!reader.ReadSelectedTags(selectedTags))
			return false;

		const gdcm::File& file = reader.GetFile();
		const gdcm::DataSet& dataSet = file.GetDataSet();
		gdcm::StringFilter stringFilter;
		stringFilter.SetFile(file);

		slice.seriesKey = tagValue(stringFilter, dataSet, seriesInstanceUidTag);
		if (slice.seriesKey.empty())
			slice.seriesKey = path.parent_path().string();

		for (const MetadataField& field : metadataFields)
		{
			const gdcm::Tag tag(field.group, field.element);
			slice.values.emplace(tagKey(field), tagValue(stringFilter, dataSet, tag));
		}

		slice.position = parseComponents(valueFor(slice, 0x0020, 0x0032));
		slice.orientation = parseComponents(valueFor(slice, 0x0020, 0x0037));
		if (slice.position.size() == 3 && slice.orientation.size() == 6)
		{
			const double normalX = slice.orientation[1] * slice.orientation[5] -
				slice.orientation[2] * slice.orientation[4];
			const double normalY = slice.orientation[2] * slice.orientation[3] -
				slice.orientation[0] * slice.orientation[5];
			const double normalZ = slice.orientation[0] * slice.orientation[4] -
				slice.orientation[1] * slice.orientation[3];
			slice.sliceCoordinate = slice.position[0] * normalX +
				slice.position[1] * normalY + slice.position[2] * normalZ;
			slice.hasSliceCoordinate = std::isfinite(slice.sliceCoordinate);
		}

		return true;
	}

	std::string formatTag(const MetadataField& field, const std::string& value)
	{
		std::ostringstream formatted;
		formatted << '[' << std::uppercase << std::hex << std::setfill('0')
			<< std::setw(4) << field.group << ',' << std::setw(4) << field.element
			<< std::dec << "] " << field.name << ": " << displayValue(value);
		return formatted.str();
	}

	void writeAcquisitionGroup(std::ostringstream& report, std::vector<SliceMetadata>& slices,
		size_t groupNumber)
	{
		const auto firstPositioned = std::find_if(slices.begin(), slices.end(),
			[](const SliceMetadata& slice) { return slice.hasSliceCoordinate; });
		const bool canSortSpatially = firstPositioned != slices.end();
		if (canSortSpatially)
		{
			for (SliceMetadata& slice : slices)
			{
				slice.hasSliceCoordinate = slice.position.size() == 3 &&
					slice.orientation.size() == 6;
				if (!slice.hasSliceCoordinate)
					continue;

				const auto& orientation = firstPositioned->orientation;
				const double normalX = orientation[1] * orientation[5] - orientation[2] * orientation[4];
				const double normalY = orientation[2] * orientation[3] - orientation[0] * orientation[5];
				const double normalZ = orientation[0] * orientation[4] - orientation[1] * orientation[3];
				slice.sliceCoordinate = slice.position[0] * normalX +
					slice.position[1] * normalY + slice.position[2] * normalZ;
			}
		}

		std::stable_sort(slices.begin(), slices.end(), [canSortSpatially](
			const SliceMetadata& left, const SliceMetadata& right)
		{
			if (canSortSpatially && left.hasSliceCoordinate && right.hasSliceCoordinate)
				return left.sliceCoordinate < right.sliceCoordinate;
			if (left.hasSliceCoordinate != right.hasSliceCoordinate)
				return left.hasSliceCoordinate;
			return left.seriesKey < right.seriesKey;
		});

		report << "\nCT acquisition group " << groupNumber << "\n";
		report << "-----------------------\n";
		report << "CT instances: " << slices.size() << '\n';
		report << "Slice order: " << (canSortSpatially
			? "ascending patient-space position projected onto the slice normal"
			: "patient-space position unavailable; spatial sorting was not possible") << "\n\n";

		for (size_t fieldIndex = 0; fieldIndex < std::size(metadataFields); ++fieldIndex)
		{
			const MetadataField& field = metadataFields[fieldIndex];
			const std::string firstValue = valueFor(slices.front(), field);
			std::set<std::string> distinctValues;
			for (const SliceMetadata& slice : slices)
				distinctValues.insert(valueFor(slice, field));

			if (field.group == 0x0020 && field.element == 0x0013)
				continue;

			if (distinctValues.size() == 1)
			{
				report << formatTag(field, firstValue) << '\n'
					<< "    Use: " << field.purpose << '\n';
			}
			else
			{
				report << formatTag(field, "<varies by slice; listed below>")
					<< "\n    Use: " << field.purpose << '\n';
			}
		}

		std::vector<double> uniqueCoordinates;
		size_t slicesWithoutPosition = 0;
		for (const SliceMetadata& slice : slices)
		{
			if (!slice.hasSliceCoordinate)
			{
				++slicesWithoutPosition;
				continue;
			}
			if (uniqueCoordinates.empty() ||
				std::abs(slice.sliceCoordinate - uniqueCoordinates.back()) > 0.01)
				uniqueCoordinates.push_back(slice.sliceCoordinate);
		}

		const size_t duplicatePositions = slices.size() - slicesWithoutPosition - uniqueCoordinates.size();
		report << "\nUnique slice positions: " << uniqueCoordinates.size()
			<< ", duplicate-position instances: " << duplicatePositions
			<< ", instances without usable position: " << slicesWithoutPosition << '\n';
		if (duplicatePositions > 0)
			report << "Warning: repeated slice locations are present. This series may contain multiple acquisitions; do not stack every instance into one volume without separating them.\n";
		if (slicesWithoutPosition > 0)
			report << "Warning: some slices could not be spatially positioned.\n";

		for (const auto& geometryTag : {
			std::pair<uint16_t, uint16_t>{ 0x0028, 0x0010 },
			{ 0x0028, 0x0011 }, { 0x0028, 0x0030 }, { 0x0020, 0x0037 } })
		{
			std::set<std::string> values;
			for (const SliceMetadata& slice : slices)
				values.insert(valueFor(slice, geometryTag.first, geometryTag.second));
			if (values.size() > 1)
				report << "Warning: image geometry tag [" << std::hex << geometryTag.first << ','
					<< geometryTag.second << std::dec << "] varies across this series.\n";
		}

		std::vector<double> sliceGaps;
		for (size_t index = 1; index < uniqueCoordinates.size(); ++index)
			sliceGaps.push_back(uniqueCoordinates[index] - uniqueCoordinates[index - 1]);
		if (!sliceGaps.empty())
		{
			const auto [minimumGap, maximumGap] = std::minmax_element(sliceGaps.begin(), sliceGaps.end());
			double totalGap = 0.0;
			for (double gap : sliceGaps)
				totalGap += gap;
			const double meanGap = totalGap / sliceGaps.size();
			report << "\nSlice spacing from patient positions (mm): min "
				<< *minimumGap << ", mean " << meanGap << ", max " << *maximumGap << '\n';
			if (*maximumGap - *minimumGap > std::max(0.01, meanGap * 0.01))
				report << "Warning: slice spacing varies; inspect the per-slice positions before building a volume.\n";
		}

		report << "\nPer-slice details (sorted spatially when positions are available)\n"
			<< "Slice | Acquisition | Temporal Position | Instance Number | Position (patient mm) | Normal Position (mm) | Rescale Slope | Rescale Intercept\n";
		for (size_t index = 0; index < slices.size(); ++index)
		{
			const SliceMetadata& slice = slices[index];
			report << index + 1 << " | " << displayValue(valueFor(slice, 0x0020, 0x0012))
				<< " | " << displayValue(valueFor(slice, 0x0020, 0x0100))
				<< " | " << displayValue(valueFor(slice, 0x0020, 0x0013))
				<< " | " << displayValue(valueFor(slice, 0x0020, 0x0032)) << " | ";
			if (slice.hasSliceCoordinate)
				report << slice.sliceCoordinate;
			else
				report << "<unavailable>";
			report << " | " << displayValue(valueFor(slice, 0x0028, 0x1053))
				<< " | " << displayValue(valueFor(slice, 0x0028, 0x1052)) << '\n';
		}

		bool hasVaryingTags = false;
		for (size_t fieldIndex = 0; fieldIndex < std::size(metadataFields); ++fieldIndex)
		{
			const MetadataField& field = metadataFields[fieldIndex];
			if ((field.group == 0x0020 && (field.element == 0x0012 || field.element == 0x0013 ||
				field.element == 0x0032 || field.element == 0x0100)) ||
				(field.group == 0x0008 && field.element == 0x0008))
				continue;

			std::set<std::string> distinctValues;
			for (const SliceMetadata& slice : slices)
				distinctValues.insert(valueFor(slice, field));
			if (distinctValues.size() < 2)
				continue;

			hasVaryingTags = true;
			report << "\nValues for varying tag " << formatTag(field, "<varies>") << '\n';
			for (size_t index = 0; index < slices.size(); ++index)
				report << "  Slice " << index + 1 << ": "
					<< displayValue(valueFor(slices[index], field)) << '\n';
		}
		if (!hasVaryingTags)
			report << "\nNo other selected metadata values vary across slices.\n";
	}
}

bool DicomMetadataReader::writeReport(
	const std::filesystem::path& inputPath,
	const std::filesystem::path& reportPath,
	std::string& errorMessage)
{
	errorMessage.clear();
	if (inputPath.empty() || reportPath.empty())
	{
		errorMessage = "Both the DICOM input file/folder and report output path are required.";
		return false;
	}

	std::error_code pathError;
	const auto absoluteInputPath = std::filesystem::absolute(inputPath, pathError);
	if (pathError)
	{
		errorMessage = "Could not resolve the DICOM input path.";
		return false;
	}
	const auto absoluteReportPath = std::filesystem::absolute(reportPath, pathError);
	if (pathError)
	{
		errorMessage = "Could not resolve the report output path.";
		return false;
	}

	const bool isDirectory = std::filesystem::is_directory(inputPath, pathError);
	if (pathError || (!isDirectory && !std::filesystem::is_regular_file(inputPath, pathError)))
	{
		errorMessage = "The input path must be an existing DICOM file or folder.";
		return false;
	}

	std::vector<std::filesystem::path> candidateFiles;
	if (isDirectory)
	{
		std::filesystem::recursive_directory_iterator iterator(inputPath, pathError);
		const std::filesystem::recursive_directory_iterator end;
		if (pathError)
		{
			errorMessage = "Could not open the input folder.";
			return false;
		}
		for (; iterator != end; iterator.increment(pathError))
		{
			if (pathError)
			{
				errorMessage = "Could not scan every file in the input folder.";
				return false;
			}
			if (!iterator->is_regular_file(pathError))
				continue;
			const auto candidatePath = std::filesystem::absolute(iterator->path(), pathError);
			if (pathError)
			{
				errorMessage = "Could not resolve a file path in the input folder.";
				return false;
			}
			if (candidatePath.lexically_normal() != absoluteReportPath.lexically_normal())
				candidateFiles.push_back(iterator->path());
		}
		if (pathError)
		{
			errorMessage = "Could not scan every file in the input folder.";
			return false;
		}
	}
	else
	{
		if (absoluteInputPath.lexically_normal() == absoluteReportPath.lexically_normal())
		{
			errorMessage = "The report output path must not overwrite the DICOM input file.";
			return false;
		}
		candidateFiles.push_back(inputPath);
	}

	std::sort(candidateFiles.begin(), candidateFiles.end());

	std::map<std::string, std::vector<SliceMetadata>> acquisitionGroups;
	std::set<std::string> seriesKeys;
	size_t unreadableFiles = 0;
	size_t nonCtFiles = 0;
	for (const auto& candidatePath : candidateFiles)
	{
		SliceMetadata slice;
		if (!readSlice(candidatePath, slice))
		{
			++unreadableFiles;
			continue;
		}

		const auto modality = slice.values.find(tagKey(metadataFields[1]));
		if (modality == slice.values.end() || modality->second != "CT")
		{
			++nonCtFiles;
			continue;
		}

		const std::string acquisitionNumber = valueFor(slice, 0x0020, 0x0012);
		const std::string temporalPosition = valueFor(slice, 0x0020, 0x0100);
		const std::string groupKey = slice.seriesKey + '\x1f' +
			(acquisitionNumber.empty() ? "<unspecified>" : acquisitionNumber) + '\x1f' +
			(temporalPosition.empty() ? "<unspecified>" : temporalPosition);
		seriesKeys.insert(slice.seriesKey);
		acquisitionGroups[groupKey].push_back(std::move(slice));
	}

	if (acquisitionGroups.empty())
	{
		errorMessage = "No readable CT DICOM instances were found in the selected input.";
		if (unreadableFiles > 0)
			errorMessage += " Files that could not be read as DICOM: " + std::to_string(unreadableFiles) + ".";
		return false;
	}

	std::ostringstream report;
	report << "DICOM CT Metadata Report\n"
		<< "========================\n"
		<< "Patient-identifying fields and Pixel Data are excluded.\n"
		<< "Input: " << (isDirectory ? "folder (searched recursively)" : "single file") << '\n'
		<< "CT instances analyzed: ";
	size_t totalCtInstances = 0;
	for (const auto& entry : acquisitionGroups)
		totalCtInstances += entry.second.size();
	report << totalCtInstances << '\n'
		<< "Distinct CT series found: " << seriesKeys.size() << '\n'
		<< "Acquisition groups found (Series UID + acquisition + temporal position): "
		<< acquisitionGroups.size() << '\n';
	if (unreadableFiles > 0)
		report << "Files skipped because they were not readable DICOM: " << unreadableFiles << '\n';
	if (nonCtFiles > 0)
		report << "Readable non-CT DICOM files skipped: " << nonCtFiles << '\n';
	report << "Series and acquisition identifiers are used internally for grouping; Series Instance UID is omitted from this report.\n";

	size_t groupNumber = 1;
	for (auto& entry : acquisitionGroups)
		writeAcquisitionGroup(report, entry.second, groupNumber++);

	std::ofstream output(reportPath, std::ios::out | std::ios::trunc);
	if (!output)
	{
		errorMessage = "Could not open the report output path for writing.";
		return false;
	}

	output << report.str();
	if (!output)
	{
		errorMessage = "An error occurred while writing the metadata report.";
		return false;
	}

	return true;
}

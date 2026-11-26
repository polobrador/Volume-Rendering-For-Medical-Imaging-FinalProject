#include "dicom_metadata_reader.h"

#include <gdcmImageReader.h>
#include <gdcmReader.h>
#include <gdcmStringFilter.h>
#include <gdcmTag.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
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
		std::filesystem::path path;
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
		slice.path = path;
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

	double parseScalar(const std::string& value, double fallback = 0.0)
	{
		try
		{
			size_t parsedLength = 0;
			const double parsed = std::stod(value, &parsedLength);
			return parsedLength == value.size() && std::isfinite(parsed) ? parsed : fallback;
		}
		catch (const std::exception&)
		{
			return fallback;
		}
	}

	bool sameComponents(const std::vector<double>& left,
		const std::vector<double>& right, double tolerance)
	{
		if (left.size() != right.size())
			return false;
		for (size_t index = 0; index < left.size(); ++index)
		{
			if (std::abs(left[index] - right[index]) > tolerance)
				return false;
		}
		return true;
	}

	std::string acquisitionGroupKey(const SliceMetadata& slice)
	{
		const std::string acquisition = valueFor(slice, 0x0020, 0x0012);
		const std::string temporalPosition = valueFor(slice, 0x0020, 0x0100);
		return slice.seriesKey + '\x1f' +
			(acquisition.empty() ? "<unspecified>" : acquisition) + '\x1f' +
			(temporalPosition.empty() ? "<unspecified>" : temporalPosition);
	}

	std::string makeAcquisitionLabel(const std::vector<SliceMetadata>& slices)
	{
		const SliceMetadata& first = slices.front();
		std::ostringstream label;
		label << "CT";
		const std::string seriesNumber = valueFor(first, 0x0020, 0x0011);
		const std::string description = valueFor(first, 0x0008, 0x103e);
		const std::string protocol = valueFor(first, 0x0018, 0x1030);
		const std::string acquisition = valueFor(first, 0x0020, 0x0012);
		const std::string temporalPosition = valueFor(first, 0x0020, 0x0100);
		if (!seriesNumber.empty())
			label << " series " << seriesNumber;
		if (!description.empty())
			label << " - " << description;
		else if (!protocol.empty())
			label << " - " << protocol;
		label << " - acquisition " << displayValue(acquisition);
		if (!temporalPosition.empty())
			label << ", time " << temporalPosition;
		label << " (" << slices.size() << " slices)";
		return label.str();
	}

	DicomAcquisitionSummary summarizeAcquisition(std::vector<SliceMetadata>& slices)
	{
		DicomAcquisitionSummary summary;
		summary.sliceCount = slices.size();
		summary.label = makeAcquisitionLabel(slices);
		if (slices.empty())
			return summary;

		const auto firstPositioned = std::find_if(slices.begin(), slices.end(),
			[](const SliceMetadata& slice) { return slice.hasSliceCoordinate; });
		const bool hasOrientation = firstPositioned != slices.end() &&
			firstPositioned->orientation.size() == 6;
		const std::vector<double> referenceOrientation =
			hasOrientation ? firstPositioned->orientation : std::vector<double>();
		std::stable_sort(slices.begin(), slices.end(), [hasOrientation, &referenceOrientation](
			const SliceMetadata& left, const SliceMetadata& right)
		{
			if (hasOrientation && left.position.size() == 3 && right.position.size() == 3)
			{
				const double normalX = referenceOrientation[1] * referenceOrientation[5] -
					referenceOrientation[2] * referenceOrientation[4];
				const double normalY = referenceOrientation[2] * referenceOrientation[3] -
					referenceOrientation[0] * referenceOrientation[5];
				const double normalZ = referenceOrientation[0] * referenceOrientation[4] -
					referenceOrientation[1] * referenceOrientation[3];
				const double leftCoordinate = left.position[0] * normalX +
					left.position[1] * normalY + left.position[2] * normalZ;
				const double rightCoordinate = right.position[0] * normalX +
					right.position[1] * normalY + right.position[2] * normalZ;
				return leftCoordinate < rightCoordinate;
			}
			return left.seriesKey < right.seriesKey;
		});

		const SliceMetadata& first = slices.front();
		const double rowsValue = parseScalar(valueFor(first, 0x0028, 0x0010));
		const double columnsValue = parseScalar(valueFor(first, 0x0028, 0x0011));
		summary.rows = rowsValue > 0.0 ? static_cast<unsigned int>(rowsValue) : 0;
		summary.columns = columnsValue > 0.0 ? static_cast<unsigned int>(columnsValue) : 0;
		const std::vector<double> pixelSpacing =
			parseComponents(valueFor(first, 0x0028, 0x0030));
		if (pixelSpacing.size() == 2)
		{
			summary.rowSpacingMm = pixelSpacing[0];
			summary.columnSpacingMm = pixelSpacing[1];
		}
		if (hasOrientation)
		{
			for (size_t index = 0; index < summary.orientation.size(); ++index)
				summary.orientation[index] = referenceOrientation[index];
		}
		if (first.position.size() == 3)
		{
			for (size_t index = 0; index < summary.originMm.size(); ++index)
				summary.originMm[index] = first.position[index];
		}

		bool consistentGeometry = hasOrientation && first.position.size() == 3 &&
			summary.rows > 0 && summary.columns > 0 &&
			summary.rowSpacingMm > 0.0 && summary.columnSpacingMm > 0.0;
		std::vector<double> coordinates;
		std::vector<double> referenceSpacing = pixelSpacing;
		size_t multiFrameCount = 0;
		for (const SliceMetadata& slice : slices)
		{
			const double frameCount = parseScalar(valueFor(slice, 0x0028, 0x0008), 1.0);
			if (frameCount > 1.0)
				++multiFrameCount;

			consistentGeometry = consistentGeometry &&
				parseScalar(valueFor(slice, 0x0028, 0x0010)) == summary.rows &&
				parseScalar(valueFor(slice, 0x0028, 0x0011)) == summary.columns &&
				sameComponents(slice.orientation, referenceOrientation, 1e-4) &&
				sameComponents(parseComponents(valueFor(slice, 0x0028, 0x0030)),
					referenceSpacing, 1e-5) &&
				slice.position.size() == 3;

			summary.orderedFiles.push_back(slice.path);
			if (slice.hasSliceCoordinate)
				coordinates.push_back(slice.sliceCoordinate);
		}
		summary.multiFrameInstanceCount = multiFrameCount;

		std::sort(coordinates.begin(), coordinates.end());
		std::vector<double> uniqueCoordinates;
		for (double coordinate : coordinates)
		{
			if (uniqueCoordinates.empty() ||
				std::abs(coordinate - uniqueCoordinates.back()) > 0.01)
				uniqueCoordinates.push_back(coordinate);
			else
				++summary.duplicatePositionCount;
		}

		std::vector<double> gaps;
		for (size_t index = 1; index < uniqueCoordinates.size(); ++index)
			gaps.push_back(uniqueCoordinates[index] - uniqueCoordinates[index - 1]);
		if (!gaps.empty())
		{
			const auto [minimumGap, maximumGap] = std::minmax_element(gaps.begin(), gaps.end());
			double gapSum = 0.0;
			for (double gap : gaps)
				gapSum += gap;
			summary.minimumSliceSpacingMm = *minimumGap;
			summary.maximumSliceSpacingMm = *maximumGap;
			summary.meanSliceSpacingMm = gapSum / gaps.size();
			summary.hasRegularSliceSpacing =
				*maximumGap - *minimumGap <= std::max(0.01, summary.meanSliceSpacingMm * 0.01);
			consistentGeometry = consistentGeometry && summary.hasRegularSliceSpacing;
			if (uniqueCoordinates.size() >= 2 && referenceOrientation.size() == 6)
			{
				const double normalX = referenceOrientation[1] * referenceOrientation[5] -
					referenceOrientation[2] * referenceOrientation[4];
				const double normalY = referenceOrientation[2] * referenceOrientation[3] -
					referenceOrientation[0] * referenceOrientation[5];
				const double normalZ = referenceOrientation[0] * referenceOrientation[4] -
					referenceOrientation[1] * referenceOrientation[3];
				summary.sliceStepVectorMm = { normalX * summary.meanSliceSpacingMm,
					normalY * summary.meanSliceSpacingMm, normalZ * summary.meanSliceSpacingMm };
			}
		}
		else
		{
			consistentGeometry = false;
		}

		consistentGeometry = consistentGeometry &&
			summary.duplicatePositionCount == 0 &&
			summary.multiFrameInstanceCount == 0 &&
			uniqueCoordinates.size() == slices.size();
		summary.hasConsistentGeometry = consistentGeometry;
		return summary;
	}
}

bool DicomMetadataReader::inspectFolder(
	const std::filesystem::path& folderPath,
	DicomFolderInspection& inspection,
	std::string& errorMessage)
{
	errorMessage.clear();
	if (folderPath.empty())
	{
		errorMessage = "Enter a folder path containing DICOM files.";
		return false;
	}

	std::error_code pathError;
	if (!std::filesystem::is_directory(folderPath, pathError) || pathError)
	{
		errorMessage = "The input path is not a readable folder.";
		return false;
	}

	DicomFolderInspection result;
	std::map<std::string, std::vector<SliceMetadata>> groups;
	std::filesystem::recursive_directory_iterator iterator(folderPath, pathError);
	const std::filesystem::recursive_directory_iterator end;
	if (pathError)
	{
		errorMessage = "Could not open the selected folder.";
		return false;
	}

	for (; iterator != end; iterator.increment(pathError))
	{
		if (pathError)
		{
			errorMessage = "Could not scan every file in the selected folder.";
			return false;
		}
		if (!iterator->is_regular_file(pathError))
			continue;
		const std::string extension = iterator->path().extension().string();
		if (extension == ".txt" || extension == ".TXT" ||
			iterator->path().filename() == ".DS_Store")
			continue;

		SliceMetadata slice;
		if (!readSlice(iterator->path(), slice))
		{
			++result.unreadableFiles;
			continue;
		}
		if (valueFor(slice, 0x0008, 0x0060) != "CT")
		{
			++result.nonCtFiles;
			continue;
		}

		groups[acquisitionGroupKey(slice)].push_back(std::move(slice));
	}
	if (pathError)
	{
		errorMessage = "Could not scan every file in the selected folder.";
		return false;
	}
	if (groups.empty())
	{
		errorMessage = "No readable CT DICOM instances were found in this folder.";
		return false;
	}

	result.acquisitions.reserve(groups.size());
	for (auto& group : groups)
		result.acquisitions.push_back(summarizeAcquisition(group.second));

	std::sort(result.acquisitions.begin(), result.acquisitions.end(),
		[](const DicomAcquisitionSummary& left, const DicomAcquisitionSummary& right)
		{
			return left.label < right.label;
		});

	inspection = std::move(result);
	return true;
}

bool DicomMetadataReader::decodeMonochromeSlice(
	const std::filesystem::path& filePath,
	DicomSlicePixels& slice,
	std::string& errorMessage)
{
	errorMessage.clear();
	gdcm::ImageReader reader;
	const std::string nativePath = filePath.string();
	reader.SetFileName(nativePath.c_str());
	if (!reader.Read())
	{
		errorMessage = "GDCM could not read image pixels from the selected DICOM file.";
		return false;
	}

	const gdcm::Image& image = reader.GetImage();
	const gdcm::PixelFormat& pixelFormat = image.GetPixelFormat();
	gdcm::StringFilter stringFilter;
	stringFilter.SetFile(reader.GetFile());
	const gdcm::DataSet& dataSet = reader.GetFile().GetDataSet();
	const double numberOfFrames = parseScalar(
		tagValue(stringFilter, dataSet, gdcm::Tag(0x0028, 0x0008)), 1.0);
	if (numberOfFrames > 1.0 ||
		(image.GetNumberOfDimensions() > 2 && image.GetDimension(2) > 1))
	{
		errorMessage = "Multi-frame DICOM images are not supported in this slice-preview step.";
		return false;
	}
	if (pixelFormat.GetSamplesPerPixel() != 1)
	{
		errorMessage = "Only single-sample monochrome DICOM images are supported.";
		return false;
	}
	if (pixelFormat.GetPixelRepresentation() > 1)
	{
		errorMessage = "The DICOM Pixel Representation value is invalid.";
		return false;
	}
	const unsigned int bitsAllocated = pixelFormat.GetBitsAllocated();
	const unsigned int bitsStored = pixelFormat.GetBitsStored();
	const unsigned int highBit = pixelFormat.GetHighBit();
	if ((bitsAllocated != 8 && bitsAllocated != 16) ||
		bitsStored == 0 || bitsStored > bitsAllocated || highBit >= bitsAllocated ||
		highBit + 1 < bitsStored)
	{
		errorMessage = "Only 8-bit or 16-bit DICOM pixel samples are supported.";
		return false;
	}

	const std::string photometric = tagValue(
		stringFilter, dataSet, gdcm::Tag(0x0028, 0x0004));
	if (photometric != "MONOCHROME1" && photometric != "MONOCHROME2")
	{
		errorMessage = "Only MONOCHROME1 and MONOCHROME2 DICOM images are supported.";
		return false;
	}

	const unsigned int rows = image.GetRows();
	const unsigned int columns = image.GetColumns();
	if (rows == 0 || columns == 0 ||
		static_cast<size_t>(columns) > std::numeric_limits<size_t>::max() / rows)
	{
		errorMessage = "The DICOM image has invalid dimensions.";
		return false;
	}
	const size_t pixelCount = static_cast<size_t>(rows) * columns;
	const size_t bytesPerSample = bitsAllocated / 8;
	if (pixelCount > std::numeric_limits<size_t>::max() / bytesPerSample)
	{
		errorMessage = "The DICOM image dimensions exceed the supported buffer size.";
		return false;
	}
	const size_t bufferLength = pixelCount * bytesPerSample;
	if (image.GetBufferLength() < bufferLength)
	{
		errorMessage = "The DICOM Pixel Data buffer is shorter than its image dimensions require.";
		return false;
	}

	std::vector<char> pixelBuffer(bufferLength);
	if (!image.GetBuffer(pixelBuffer.data()))
	{
		errorMessage = "GDCM could not decode the DICOM Pixel Data.";
		return false;
	}

	const double slope = image.GetSlope();
	const double intercept = image.GetIntercept();
	if (!std::isfinite(slope) || !std::isfinite(intercept))
	{
		errorMessage = "The DICOM rescale slope or intercept is not a finite number.";
		return false;
	}

	const std::vector<double> centers =
		parseComponents(tagValue(stringFilter, dataSet, gdcm::Tag(0x0028, 0x1050)));
	const std::vector<double> widths =
		parseComponents(tagValue(stringFilter, dataSet, gdcm::Tag(0x0028, 0x1051)));
	const std::vector<double> paddingValues =
		parseComponents(tagValue(stringFilter, dataSet, gdcm::Tag(0x0028, 0x0120)));
	const std::vector<double> paddingLimits =
		parseComponents(tagValue(stringFilter, dataSet, gdcm::Tag(0x0028, 0x0121)));

	DicomSlicePixels decoded;
	decoded.rows = rows;
	decoded.columns = columns;
	decoded.monochrome1 = photometric == "MONOCHROME1";
	decoded.modalityValues.resize(pixelCount);
	decoded.paddingMask.assign(pixelCount, 0);

	const unsigned int lowBit = highBit + 1 - bitsStored;
	const std::uint32_t storedMask = (std::uint32_t{ 1 } << bitsStored) - 1;
	const std::uint32_t signBit = std::uint32_t{ 1 } << (bitsStored - 1);
	double minimumValue = std::numeric_limits<double>::max();
	double maximumValue = std::numeric_limits<double>::lowest();
	for (size_t index = 0; index < pixelCount; ++index)
	{
		std::uint32_t rawValue = 0;
		if (bitsAllocated == 8)
		{
			rawValue = static_cast<unsigned char>(pixelBuffer[index]);
		}
		else
		{
			std::uint16_t sample = 0;
			std::memcpy(&sample, pixelBuffer.data() + index * bytesPerSample, sizeof(sample));
			rawValue = sample;
		}

		const std::uint32_t storedBits = (rawValue >> lowBit) & storedMask;
		std::int32_t storedValue = static_cast<std::int32_t>(storedBits);
		if (pixelFormat.GetPixelRepresentation() == 1 && (storedBits & signBit))
			storedValue -= static_cast<std::int32_t>(std::uint32_t{ 1 } << bitsStored);

		bool isPadding = !paddingValues.empty() && storedValue == paddingValues.front();
		if (!paddingValues.empty() && !paddingLimits.empty())
		{
			const double lower = std::min(paddingValues.front(), paddingLimits.front());
			const double upper = std::max(paddingValues.front(), paddingLimits.front());
			isPadding = storedValue >= lower && storedValue <= upper;
		}

		const double modalityValue = storedValue * slope + intercept;
		decoded.modalityValues[index] = static_cast<float>(modalityValue);
		decoded.paddingMask[index] = isPadding ? 1 : 0;
		if (!isPadding)
		{
			minimumValue = std::min(minimumValue, modalityValue);
			maximumValue = std::max(maximumValue, modalityValue);
		}
	}

	if (!centers.empty() && std::isfinite(centers.front()))
		decoded.windowCenter = centers.front();
	else
		decoded.windowCenter = minimumValue <= maximumValue
			? (minimumValue + maximumValue) * 0.5 : 0.0;
	if (!widths.empty() && std::isfinite(widths.front()) && widths.front() > 0.0)
		decoded.windowWidth = widths.front();
	else
		decoded.windowWidth = minimumValue <= maximumValue
			? std::max(1.0, maximumValue - minimumValue) : 1.0;

	slice = std::move(decoded);
	return true;
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

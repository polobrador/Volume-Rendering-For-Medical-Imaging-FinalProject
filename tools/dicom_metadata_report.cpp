#include "dicom/dicom_metadata_reader.h"

#include <algorithm>
#include <iostream>
#include <limits>

int main(int argc, char* argv[])
{
	if (argc == 3 && std::string(argv[1]) == "--inspect-folder")
	{
		DicomFolderInspection inspection;
		std::string errorMessage;
		if (!DicomMetadataReader::inspectFolder(argv[2], inspection, errorMessage))
		{
			std::cerr << "DICOM folder inspection failed: " << errorMessage << '\n';
			return 1;
		}

		std::cout << "CT acquisitions found: " << inspection.acquisitions.size()
			<< " (headers only; pixel data not decoded)\n";
		for (const DicomAcquisitionSummary& acquisition : inspection.acquisitions)
		{
			std::cout << "- " << acquisition.label
				<< "; size " << acquisition.columns << 'x' << acquisition.rows
				<< "; pixel spacing " << acquisition.rowSpacingMm << " x "
				<< acquisition.columnSpacingMm << " mm";
			if (acquisition.minimumSliceSpacingMm > 0.0)
				std::cout << "; slice spacing " << acquisition.minimumSliceSpacingMm
					<< " to " << acquisition.maximumSliceSpacingMm << " mm";
			std::cout << "; regular geometry "
				<< (acquisition.hasConsistentGeometry ? "yes" : "no") << '\n';
		}
		std::cout << "Unreadable files: " << inspection.unreadableFiles
			<< "; non-CT files: " << inspection.nonCtFiles << '\n';
		return 0;
	}

	if (argc == 3 && std::string(argv[1]) == "--inspect-slice")
	{
		DicomSlicePixels slice;
		std::string errorMessage;
		if (!DicomMetadataReader::decodeMonochromeSlice(argv[2], slice, errorMessage))
		{
			std::cerr << "DICOM slice decoding failed: " << errorMessage << '\n';
			return 1;
		}

		float minimumValue = std::numeric_limits<float>::max();
		float maximumValue = std::numeric_limits<float>::lowest();
		bool hasNonPaddingPixels = false;
		for (size_t index = 0; index < slice.modalityValues.size(); ++index)
		{
			if (slice.paddingMask[index])
				continue;
			hasNonPaddingPixels = true;
			minimumValue = std::min(minimumValue, slice.modalityValues[index]);
			maximumValue = std::max(maximumValue, slice.modalityValues[index]);
		}
		std::cout << "Decoded monochrome slice: " << slice.columns << 'x' << slice.rows << '\n'
			<< "Rescaled modality-value range (excluding padding): ";
		if (hasNonPaddingPixels)
			std::cout << minimumValue << " to " << maximumValue << '\n';
		else
			std::cout << "all pixels are padding\n";
		std::cout << "Suggested window center/width: "
			<< slice.windowCenter << " / " << slice.windowWidth << '\n';
		return 0;
	}

	if (argc != 3)
	{
		std::cerr << "Usage:\n"
			<< "  DICOMMetadataReport <input.dcm-or-folder> <output.txt>\n"
			<< "  DICOMMetadataReport --inspect-folder <folder>\n"
			<< "  DICOMMetadataReport --inspect-slice <file.dcm>\n";
		return 2;
	}

	std::string errorMessage;
	if (!DicomMetadataReader::writeReport(argv[1], argv[2], errorMessage))
	{
		std::cerr << "DICOM metadata report failed: " << errorMessage << '\n';
		return 1;
	}

	std::cout << "Wrote technical metadata report to " << argv[2] << '\n';
	return 0;
}

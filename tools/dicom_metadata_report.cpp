#include "dicom/dicom_metadata_reader.h"

#include <iostream>

int main(int argc, char* argv[])
{
	if (argc != 3)
	{
		std::cerr << "Usage: DICOMMetadataReport <input.dcm-or-folder> <output.txt>\n";
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

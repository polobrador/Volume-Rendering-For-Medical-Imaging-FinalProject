# Agent Timeline

This file records project changes made with the coding agent and why they were made. Add a dated entry whenever future changes are made.

## 2026-11-26

### Clarifying GDCM installation for new clones

- Documented that cloning the repository does not install GDCM and that CMake requires a separately installed development package through `find_package(GDCM REQUIRED)`.
- Clarified the supported package-install examples and the `GDCM_DIR` override, and distinguished the small ImGuizmo files CMake downloads from GDCM.
- Kept GDCM as an external prerequisite rather than fetching/building it implicitly, because its native build is platform-dependent and can be lengthy.

### DICOM metadata report reader and command-line tool

- Added a standalone GDCM-backed reader that reports selected DICOM metadata to a text file.
- Added GDCM discovery/linking and a `DICOMMetadataReport` command-line target, then documented how to build and run the tool.
- Kept direct patient identifiers out of the report so it is safer to use for technical inspection and later browsing.
- Added report details for image dimensions, spacing, orientation, rescale values, window settings, and pixel format because those fields explain how DICOM values should later be interpreted and rendered.

### Whole-folder CT series analysis

- Extended the report tool to recursively scan folders and group CT instances by series, acquisition number, and temporal position.
- Ordered slices using patient-space positions rather than filenames or instance numbers, and reported repeated positions and geometry variation.
- Added this because a CT volume commonly consists of many single-frame DICOM files, and mixing separate acquisitions can create an invalid stack.
- Analyzed the supplied `series-000003` folder: it contains two acquisition groups with 217 and 284 slices.

### In-app folder inspection

- Added structured acquisition summaries and a **DICOM CT Folder Inspection** panel for choosing a folder and viewing acquisition geometry.
- Added a command-line header inspection mode and documented both inspection workflows.
- Kept this stage header-only so the series grouping and geometry can be understood before introducing pixel decoding or rendering.

### Single-slice decoding and 2D preview

- Added GDCM pixel decoding for single-frame, single-sample monochrome 8-bit and 16-bit DICOM images.
- Converted stored pixel samples to modality values using the DICOM rescale slope and intercept; tracked pixel padding separately.
- Added MONOCHROME1/MONOCHROME2 handling and DICOM linear window center/width mapping for grayscale display.
- Added acquisition and slice selection plus a 2D preview to the in-app panel, with controls for changing window center and width.
- Added `DICOMMetadataReport --inspect-slice <file.dcm>` to check decoding and summarize dimensions, rescaled value range, and suggested window settings without opening the GUI.
- This is the next contained step toward volume rendering: it verifies that one 2D image can be decoded and interpreted before stacking slices or uploading a 3D texture.
- At this stage no 3D volume assembly, trilinear sampling, or DICOM volume rendering had been added; the existing VDB renderer remained unchanged.

### Full-resolution CPU volume assembly

- Added an acquisition loader that decodes every ordered slice and stores modality values and a padding mask in contiguous `(column, row, slice)` order.
- Validated each decoded slice against the inspected dimensions, orientation, in-plane pixel spacing, and expected patient-space position on the volume grid; this prevents stacking changed or geometrically inconsistent files.
- Calculated the full 3D slice-step vector from actual patient positions, so regular acquisitions with a consistent in-plane shift are represented correctly rather than assuming slices move only along the orientation normal.
- Added GUI and command-line actions to assemble an acquisition and report dimensions, physical spacing, non-padding value range, and CPU storage estimate. Allocation failures are reported without silently reducing resolution.
- This step creates and validates the CPU-side volume needed before GPU upload and interpolation; it does not alter the VDB renderer or perform 3D rendering.

### Uploading the CT volume as a 3D GPU texture

- Added an explicit GUI action to upload the assembled modality-value array into a full-resolution `GL_R32F` OpenGL 3D texture, with `GL_LINEAR` filtering and clamp-to-edge addressing; uploaded the padding mask separately as `GL_R8` so padding information is preserved for later rendering.
- Added GPU maximum-dimension checks, an estimated texture-memory readout, and visible OpenGL/memory-allocation error messages so unsupported sizes do not silently downsample.
- Release the uploaded texture when its source acquisition/CPU volume changes and when the application shuts down, preventing stale GPU data and resource leaks.
- This separates GPU storage from rendering integration; the VDB bunny remains active unless explicitly switched to the uploaded CT.

### Rendering the uploaded CT volume

- Added a DICOM ray-marching shader that maps rescaled CT modality values through the current window center/width and treats the uploaded padding mask as empty samples.
- Added source switching in `VolumeMaterial`: the DICOM texture can be rendered with the CT shader, and the original VDB shader/texture remains available through **Use VDB bunny**.
- Built the volume model transform from DICOM row/column orientation, in-plane spacing, and the measured slice-step vector so the proxy cube reflects acquisition orientation and physical aspect ratio.
- Set a voxel-informed starting ray step and exposed a DICOM step-size control; this connects the already-uploaded `GL_LINEAR` intensity texture to hardware 3D linear sampling while keeping the renderer separate from the VDB path.

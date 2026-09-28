#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    std::cerr << operation << " failed: 0x" << std::hex
              << static_cast<unsigned long>(result) << std::dec << '\n';
    std::exit(1);
  }
}

std::string GuidString(const GUID& guid) {
  wchar_t value[64]{};
  StringFromGUID2(guid, value, static_cast<int>(std::size(value)));
  int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr,
                                  nullptr);
  std::string result(static_cast<size_t>(std::max(0, count - 1)), '\0');
  if (count > 1) {
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr,
                        nullptr);
  }
  return result;
}

void WriteBmp(const std::filesystem::path& path, uint32_t width,
              uint32_t height, const uint8_t* pixels, long stride) {
  const uint32_t row_bytes = width * 4;
  const uint32_t image_bytes = row_bytes * height;

  BITMAPFILEHEADER file_header{};
  file_header.bfType = 0x4D42;
  file_header.bfOffBits = sizeof(file_header) + sizeof(BITMAPINFOHEADER);
  file_header.bfSize = file_header.bfOffBits + image_bytes;

  BITMAPINFOHEADER info_header{};
  info_header.biSize = sizeof(info_header);
  info_header.biWidth = static_cast<LONG>(width);
  info_header.biHeight = -static_cast<LONG>(height);
  info_header.biPlanes = 1;
  info_header.biBitCount = 32;
  info_header.biCompression = BI_RGB;
  info_header.biSizeImage = image_bytes;

  std::ofstream output(path, std::ios::binary);
  if (!output) {
    std::cerr << "Unable to create " << path.string() << '\n';
    std::exit(1);
  }
  output.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
  output.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));
  for (uint32_t y = 0; y < height; ++y) {
    output.write(reinterpret_cast<const char*>(pixels + y * stride), row_bytes);
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    std::wcerr << L"Usage: mf_video_frame_extract <video> <output-prefix> "
                  L"<time-seconds> [time-seconds ...]\n";
    return 2;
  }

  Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
  Check(MFStartup(MF_VERSION), "MFStartup");

  ComPtr<IMFAttributes> attributes;
  Check(MFCreateAttributes(&attributes, 2), "MFCreateAttributes");
  Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE),
        "enable video processing");

  ComPtr<IMFSourceReader> reader;
  Check(MFCreateSourceReaderFromURL(argv[1], attributes.Get(), &reader),
        "MFCreateSourceReaderFromURL");
  Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE),
        "disable streams");
  Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE),
        "enable video stream");

  ComPtr<IMFMediaType> native_type;
  Check(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                   &native_type),
        "GetNativeMediaType");
  GUID native_subtype{};
  Check(native_type->GetGUID(MF_MT_SUBTYPE, &native_subtype),
        "native subtype");
  UINT32 native_width = 0;
  UINT32 native_height = 0;
  Check(MFGetAttributeSize(native_type.Get(), MF_MT_FRAME_SIZE, &native_width,
                           &native_height),
        "native frame size");
  UINT32 frame_rate_numerator = 0;
  UINT32 frame_rate_denominator = 0;
  MFGetAttributeRatio(native_type.Get(), MF_MT_FRAME_RATE,
                      &frame_rate_numerator, &frame_rate_denominator);

  PROPVARIANT duration_value;
  PropVariantInit(&duration_value);
  Check(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                         MF_PD_DURATION, &duration_value),
        "duration");
  const LONGLONG duration = duration_value.vt == VT_UI8
                                ? static_cast<LONGLONG>(duration_value.uhVal.QuadPart)
                                : 0;
  PropVariantClear(&duration_value);

  std::cout << "native_subtype=" << GuidString(native_subtype) << '\n'
            << "native_size=" << native_width << 'x' << native_height << '\n'
            << "native_frame_rate=" << frame_rate_numerator << '/'
            << frame_rate_denominator << '\n'
            << "duration_100ns=" << duration << '\n'
            << "duration_seconds=" << std::fixed << std::setprecision(6)
            << static_cast<double>(duration) / 10000000.0 << '\n';

  ComPtr<IMFMediaType> output_type;
  Check(MFCreateMediaType(&output_type), "MFCreateMediaType");
  Check(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
        "output major type");
  Check(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32),
        "output subtype");
  Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                    nullptr, output_type.Get()),
        "SetCurrentMediaType RGB32");
  Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                    &output_type),
        "GetCurrentMediaType");
  UINT32 width = 0;
  UINT32 height = 0;
  Check(MFGetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, &width, &height),
        "output frame size");
  std::cout << "output_size=" << width << 'x' << height << '\n';

  const std::filesystem::path prefix(argv[2]);
  for (int argument = 3; argument < argc; ++argument) {
    const double requested_seconds = std::stod(argv[argument]);
    PROPVARIANT seek_position;
    PropVariantInit(&seek_position);
    seek_position.vt = VT_I8;
    seek_position.hVal.QuadPart =
        static_cast<LONGLONG>(requested_seconds * 10000000.0);
    Check(reader->SetCurrentPosition(GUID_NULL, seek_position),
          "SetCurrentPosition");
    PropVariantClear(&seek_position);

    ComPtr<IMFSample> sample;
    LONGLONG timestamp = 0;
    const LONGLONG requested_timestamp =
        static_cast<LONGLONG>(requested_seconds * 10000000.0);
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
      sample.Reset();
      DWORD stream_index = 0;
      DWORD flags = 0;
      Check(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                               &stream_index, &flags, &timestamp, &sample),
            "ReadSample");
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        break;
      }
      if (sample && timestamp >= requested_timestamp) {
        break;
      }
    }
    if (!sample || timestamp < requested_timestamp) {
      std::cerr << "No sample at " << requested_seconds << " seconds\n";
      continue;
    }

    ComPtr<IMFMediaBuffer> buffer;
    Check(sample->ConvertToContiguousBuffer(&buffer),
          "ConvertToContiguousBuffer");
    BYTE* bytes = nullptr;
    DWORD current_length = 0;
    Check(buffer->Lock(&bytes, nullptr, &current_length), "buffer Lock");
    const long stride = static_cast<long>(width * 4);
    if (current_length < static_cast<DWORD>(stride) * height) {
      buffer->Unlock();
      std::cerr << "Short RGB buffer at " << requested_seconds << " seconds\n";
      continue;
    }

    std::wstringstream suffix;
    suffix << L"_" << std::fixed << std::setprecision(3) << requested_seconds
           << L"s.bmp";
    const std::filesystem::path output_path = prefix.wstring() + suffix.str();
    WriteBmp(output_path, width, height, bytes, stride);
    Check(buffer->Unlock(), "buffer Unlock");
    std::cout << "frame requested=" << requested_seconds
              << " sample=" << static_cast<double>(timestamp) / 10000000.0
              << " output=" << output_path.string() << '\n';
  }

  MFShutdown();
  CoUninitialize();
  return 0;
}

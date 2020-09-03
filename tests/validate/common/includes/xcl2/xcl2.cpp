/**
 * Copyright (C) 2020 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
#include "xcl2.hpp"
#include <climits>
#include <sys/stat.h>
#if defined(_WINDOWS)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace xcl {
std::vector<cl::Device> get_devices(const std::string &vendor_name) {
  size_t i;
  cl_int err;
  std::vector<cl::Platform> platforms;
  OCL_CHECK(err, err = cl::Platform::get(&platforms));
  cl::Platform platform;
  for (i = 0; i < platforms.size(); i++) {
    platform = platforms[i];
    OCL_CHECK(err, std::string platformName =
                       platform.getInfo<CL_PLATFORM_NAME>(&err));
    if (platformName == vendor_name) {
      std::cout << "Found Platform" << std::endl;
      std::cout << "Platform Name: " << platformName.c_str() << std::endl;
      break;
    }
  }
  if (i == platforms.size()) {
    std::cout << "Error: Failed to find Xilinx platform" << std::endl;
    exit(EXIT_FAILURE);
  }
  // Getting ACCELERATOR Devices and selecting 1st such device
  std::vector<cl::Device> devices;
  OCL_CHECK(err,
            err = platform.getDevices(CL_DEVICE_TYPE_ACCELERATOR, &devices));
  return devices;
}

std::vector<cl::Device> get_xil_devices() { return get_devices("Xilinx"); }

std::vector<unsigned char>
read_binary_file(const std::string &xclbin_file_name) {
  std::cout << "INFO: Reading " << xclbin_file_name << std::endl;
  FILE *fp;
  if ((fp = fopen(xclbin_file_name.c_str(), "r")) == nullptr) {
    printf("ERROR: %s xclbin not available please build\n",
           xclbin_file_name.c_str());
    exit(EXIT_FAILURE);
  }
  fclose(fp);
  // Loading XCL Bin into char buffer
  std::cout << "Loading: '" << xclbin_file_name.c_str() << "'\n";
  std::ifstream bin_file(xclbin_file_name.c_str(), std::ifstream::binary);
  bin_file.seekg(0, bin_file.end);
  auto nb = bin_file.tellg();
  bin_file.seekg(0, bin_file.beg);
  std::vector<unsigned char> buf;
  buf.resize(nb);
  bin_file.read(reinterpret_cast<char *>(buf.data()), nb);
  return buf;
}

bool is_emulation() {
  bool ret = false;
  char *xcl_mode = getenv("XCL_EMULATION_MODE");
  if (xcl_mode != nullptr) {
    ret = true;
  }
  return ret;
}

bool is_hw_emulation() {
  bool ret = false;
  char *xcl_mode = getenv("XCL_EMULATION_MODE");
  if ((xcl_mode != nullptr) && !strcmp(xcl_mode, "hw_emu")) {
    ret = true;
  }
  return ret;
}

bool is_xpr_device(const char *device_name) {
  const char *output = strstr(device_name, "xpr");

  if (output == nullptr) {
    return false;
  } else {
    return true;
  }
}
}; // namespace xcl

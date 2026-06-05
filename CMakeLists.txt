cmake_minimum_required(VERSION 3.24)

project(SpecForge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)

# First-commit cleanup phase: validate the target dependency stack without
# declaring product code. The first implementation slice will add the Win32/DX11
# executable target and link d3d11/dxgi.

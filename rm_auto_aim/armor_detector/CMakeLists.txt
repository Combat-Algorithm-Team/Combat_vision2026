cmake_minimum_required(VERSION 3.10)
project(armor_detector)

## Use C++14
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

## By adding -Wall and -Werror, the compiler does not ignore warnings anymore,
## enforcing cleaner code.
add_definitions(-Wall -Werror)
add_definitions(-O3)


## Export compile commands for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#######################
## Find dependencies ##
#######################

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/cmake_modules)

find_package(ament_cmake_auto REQUIRED)
find_package(OpenCV REQUIRED)
find_package(TBB REQUIRED COMPONENTS tbb)
find_package(OpenVINO QUIET CONFIG)

# 手动设置 G2O 库路径
set(G2O_LIBRARIES
    /usr/local/lib/libg2o_core.so
    /usr/local/lib/libg2o_stuff.so
    /usr/local/lib/libg2o_solver_csparse.so
    /usr/local/lib/libg2o_types_sba.so
    /usr/local/lib/libg2o_types_slam3d.so
    /usr/local/lib/libg2o_solver_dense.so
)
set(G2O_INCLUDE_DIRS /usr/local/include)

if(OpenVINO_FOUND)
  set(OPENVINO_TARGET openvino::runtime)
else()
  set(OPENVINO_LEGACY_ROOT /opt/intel/openvino_2024.6.0)
  set(OpenVINO_INCLUDE_DIRS ${OPENVINO_LEGACY_ROOT}/runtime/include)
  set(OPENVINO_LIBRARIES ${OPENVINO_LEGACY_ROOT}/runtime/lib/intel64/libopenvino.so)

  if(NOT EXISTS ${OpenVINO_INCLUDE_DIRS}/openvino/openvino.hpp OR NOT EXISTS ${OPENVINO_LIBRARIES})
    message(FATAL_ERROR "OpenVINO not found. Install the OpenVINO APT package so CMake can use openvino::runtime, or provide a legacy toolkit at ${OPENVINO_LEGACY_ROOT}.")
  endif()
endif()

find_package(fmt REQUIRED)
find_package(Sophus REQUIRED)
ament_auto_find_build_dependencies()

###########
## Build ##
###########

ament_auto_add_library(${PROJECT_NAME} SHARED
  DIRECTORY src
)

target_include_directories(${PROJECT_NAME} PUBLIC
${OpenCV_INCLUDE_DIRS}
${G2O_INCLUDE_DIRS}
${Sophus_INCLUDE_DIRS}
${EIGEN3_INCLUDE_DIRS}
)

if(NOT OpenVINO_FOUND)
  target_include_directories(${PROJECT_NAME} PUBLIC
    ${OpenVINO_INCLUDE_DIRS}
  )
endif()

target_link_libraries(${PROJECT_NAME}
${OpenCV_LIBS}
${G2O_LIBRARIES}
fmt::fmt
tbb
)

if(OpenVINO_FOUND)
  target_link_libraries(${PROJECT_NAME}
    ${OPENVINO_TARGET}
  )
else()
  target_link_libraries(${PROJECT_NAME}
    ${OPENVINO_LIBRARIES}
  )
endif()


rclcpp_components_register_node(${PROJECT_NAME}
  PLUGIN fyt::auto_aim::ArmorDetectorNode
  EXECUTABLE armor_detector_node
)

#############
## Testing ##
#############

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  list(APPEND AMENT_LINT_AUTO_EXCLUDE
    ament_cmake_copyright
    ament_cmake_uncrustify
    ament_cmake_cpplint
    ament_cmake_lint_cmake
  )
  ament_lint_auto_find_test_dependencies()

  find_package(ament_cmake_gtest)
  ament_add_gtest(test_detector test/test_detector.cpp)
  target_link_libraries(test_detector ${PROJECT_NAME})

endif()

#############
## Install ##
#############

ament_auto_package(
  INSTALL_TO_SHARE
  docs
  model
)

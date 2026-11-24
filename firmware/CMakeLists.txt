cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(portal_bridge)

# Load balancer: lwIP calls pb_route_hook() (main/lb.c) to pick the output
# interface for forwarded packets.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_options(${lwip_lib} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}/main/lwip_hook")
target_compile_definitions(${lwip_lib} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"pb_lwip_hooks.h\"")

find_path(MPP_INCLUDE_DIR rk_mpi.h PATH_SUFFIXES rockchip REQUIRED)
find_library(MPP_LIBRARY rockchip_mpp REQUIRED)

add_library(video_transport STATIC video_transport.cpp)
target_include_directories(video_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(wd_tx tx_main.cpp)
find_path(RGA_INCLUDE_DIR im2d.h PATH_SUFFIXES rga REQUIRED)
find_library(RGA_LIBRARY rga REQUIRED)
target_include_directories(wd_tx PRIVATE ${MPP_INCLUDE_DIR} ${RGA_INCLUDE_DIR})
target_link_libraries(wd_tx PRIVATE video_transport ${MPP_LIBRARY} ${RGA_LIBRARY})

if(ENABLE_DRM_TARGETS)
    add_executable(wd_rx rx_main.cpp)
    find_library(DRM_LIBRARY drm REQUIRED)
    find_path(DRM_INCLUDE_DIR xf86drm.h REQUIRED)
    find_path(DRM_KERNEL_INCLUDE_DIR drm.h PATH_SUFFIXES libdrm REQUIRED)
    target_include_directories(wd_rx PRIVATE
        ${MPP_INCLUDE_DIR} ${DRM_INCLUDE_DIR} ${DRM_KERNEL_INCLUDE_DIR})
    target_link_libraries(wd_rx PRIVATE video_transport ${MPP_LIBRARY} ${DRM_LIBRARY})
    install(TARGETS wd_rx RUNTIME DESTINATION bin)
endif()

install(TARGETS wd_tx RUNTIME DESTINATION bin)

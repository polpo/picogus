# PGDFS: DOS file system redirector service for the USB drive.
# Linked into every firmware that carries the USB host stack (USB_MSC).
add_library(dfs INTERFACE)

target_sources(dfs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/dfs_transport.c
    ${CMAKE_CURRENT_LIST_DIR}/dfs_server.c
    ${CMAKE_CURRENT_LIST_DIR}/dfs_fs.c
)

target_include_directories(dfs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

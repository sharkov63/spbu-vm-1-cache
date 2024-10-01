project(spbu-vm-1-cache)

add_subdirectory(fmt)

add_executable(measure
  Main.cc
)
target_link_libraries(measure fmt::fmt-header-only)

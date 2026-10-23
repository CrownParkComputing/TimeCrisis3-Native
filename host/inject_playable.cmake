# Loaded by the upstream Pcsx2 project; wait until its core targets exist.
cmake_language(EVAL CODE
  "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL include [[${CMAKE_CURRENT_LIST_DIR}/playable.cmake]])")

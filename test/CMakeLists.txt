enable_testing()

add_test(
  NAME xeus-lix-kernel-tests
  COMMAND ${Python3_EXECUTABLE} -m unittest discover --start-directory test --verbose
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# set the JUPYTER_PATH environment variable, so it can find the kernelspec file.
set_tests_properties(
  xeus-lix-kernel-tests
  PROPERTIES
    ENVIRONMENT "JUPYTER_PATH=${CMAKE_BINARY_DIR}/share/jupyter"
)

set(BIN_DIR ${CMAKE_BINARY_DIR})
set(TEST_DIR ${CMAKE_BINARY_DIR}/tests)
set(CTS_BIN ${CMAKE_CURRENT_SOURCE_DIR}/cts/bin)
set(CTS_LIB ${CMAKE_CURRENT_SOURCE_DIR}/cts/lib)
set(ENV{TEST_DIR} ${TEST_DIR})
set(MPI_OPTIONS=" --map-by socket --bind-to core ")
set(HYBRID_OPTIONS=" --map-by socket --bind-to core ")
set(MPIOPENMP_OPTIONS=" --map-by core --bind-to socket ")

#add_test(clamr_tests clamr_tests)
#add_test(scaling_tests scaling_tests)

set(CTEST_ENVIRONMENT "PRECISION_TYPE=${PRECISION_TYPE}")
if (OpenCL_FOUND)
  set(CTEST_ENVIRONMENT "HAVE_OPENCL=1;${CTEST_ENVIRONMENT}")
endif (OpenCL_FOUND)
if (OPENMP_FOUND)
  set(CTEST_ENVIRONMENT "HAVE_OPENMP=1;${CTEST_ENVIRONMENT}")
endif (OPENMP_FOUND)
if (MPI_FOUND)
  set(CTEST_ENVIRONMENT "HAVE_MPI=1;${CTEST_ENVIRONMENT}")
endif (MPI_FOUND)

file(GLOB TESTFILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*/*.ctest")
foreach(TESTFILE ${TESTFILES})
    if ( ${TESTFILE} MATCHES ".*gpu.*" AND NOT OpenCL_FOUND ) 
      continue()
    endif ()
    if ( ${TESTFILE} MATCHES ".*checkall.*" AND NOT (OpenCL_FOUND AND MPI_FOUND)) 
      continue()
    endif ()
    #message("DEBUG : add_test(${TESTFILE} working directory ${TEST_DIR} testdriver ${CMAKE_CURRENT_SOURCE_DIR}/${TESTFILE})")
    add_test(NAME ${TESTFILE} WORKING_DIRECTORY ${TEST_DIR} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/${TESTFILE})
    set_tests_properties(${TESTFILE} PROPERTIES TIMEOUT 500)
    set_tests_properties(${TESTFILE} PROPERTIES ENVIRONMENT "${CTEST_ENVIRONMENT}")
endforeach()

add_custom_target(clamr_tests 
                  COMMAND echo "Running clamr tests" &&
                          make clamr_clean_tests &&
                          env BIN_DIR=${BIN_DIR} 
                              CTS_BIN=${CTS_BIN}
                              PRECISION_TYPE="_${PRECISION_TYPE}"
                          ${CTS_BIN}/cts -cts clamr.cts clamr.suite &&
                          fgrep SPEEDUP testing/*/*.out
                  DEPENDS ${BIN_DIR}/clamr_cpuonly ${BIN_DIR}/clamr_mpionly
                          ${BIN_DIR}/clamr_mpicheck ${BIN_DIR}/clamr_checkall
                          ${BIN_DIR}/clamr_openmponly ${BIN_DIR}/clamr_mpiopenmponly
                          ${BIN_DIR}/clamr_gpuonly ${BIN_DIR}/clamr_gpucheck
                          ${BIN_DIR}/clamr)

add_custom_target(clamr_nobatch_tests 
                  COMMAND echo "Running clamr nobatch tests" &&
                          make clamr_clean_tests &&
                          env BIN_DIR=${BIN_DIR} 
                              CTS_BIN=${CTS_BIN}
                              PRECISION_TYPE="_${PRECISION_TYPE}"
                              MPI_OPTIONS=${MPI_OPTIONS}
                              HYBRID_OPTIONS=${HYBRID_OPTIONS}
                              MPIOPENMP_OPTIONS=${MPIOPENMP_OPTIONS}
                          ${CTS_BIN}/cts --nobatch --maxthreads 1 -cts clamr.cts clamr.suite &&
                          fgrep SPEEDUP testing/*/*.out
                  DEPENDS ${BIN_DIR}/clamr_cpuonly ${BIN_DIR}/clamr_mpionly
                          ${BIN_DIR}/clamr_mpicheck ${BIN_DIR}/clamr_checkall
                          ${BIN_DIR}/clamr_openmponly ${BIN_DIR}/clamr_mpiopenmponly
                          ${BIN_DIR}/clamr_gpuonly ${BIN_DIR}/clamr_gpucheck
                          ${BIN_DIR}/clamr)

add_custom_target(clamr_clean_tests
                  COMMAND echo "Removing previous tests" && rm -rf ${TEST_DIR}/testing)

add_custom_target(scaling_tests 
                  COMMAND echo "Running scaling tests" &&
                          make clamr_clean_tests &&
                          env BIN_DIR=${BIN_DIR} 
                              CTS_BIN=${CTS_BIN}
                              PRECISION_TYPE="_${PRECISION_TYPE}"
                          ${CTS_BIN}/cts -cts clamr.cts scaling.suite &&
                          fgrep SPEEDUP testing/*/*.out
                  DEPENDS ${BIN_DIR}/clamr_cpuonly ${BIN_DIR}/clamr_mpionly
                          ${BIN_DIR}/clamr_mpicheck ${BIN_DIR}/clamr_checkall
                          ${BIN_DIR}/clamr_openmponly ${BIN_DIR}/clamr_mpiopenmponly
                          ${BIN_DIR}/clamr_gpuonly ${BIN_DIR}/clamr_gpucheck
                          ${BIN_DIR}/clamr)

add_custom_target(inspxe_tests 
                  COMMAND echo "Running Intel thread inspector tests" &&
                          make clamr_clean_tests &&
                          env BIN_DIR=${BIN_DIR} 
                              CTS_BIN=${CTS_BIN}
                              PRECISION_TYPE="_${PRECISION_TYPE}"
                          ${CTS_BIN}/cts -cts clamr.cts inspxe.suite
                  DEPENDS ${BIN_DIR}/clamr_cpuonly ${BIN_DIR}/clamr_mpionly
                          ${BIN_DIR}/clamr_mpicheck ${BIN_DIR}/clamr_checkall
                          ${BIN_DIR}/clamr_openmponly ${BIN_DIR}/clamr_mpiopenmponly
                          ${BIN_DIR}/clamr_gpuonly ${BIN_DIR}/clamr_gpucheck
                          ${BIN_DIR}/clamr)

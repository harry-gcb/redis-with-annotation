include_directories(${PROJECT_SOURCE_DIR}/src/)
aux_source_directory(${PROJECT_SOURCE_DIR}/module/helloworld HELLOWORLD)

add_library(helloworld SHARED ${HELLOWORLD})
set_target_properties(helloworld PROPERTIES 
    PREFIX "" 
    OUTPUT_NAME "helloworld")

install(TARGETS helloworld 
    LIBRARY DESTINATION ${PROJECT_SOURCE_DIR}/module
    PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_WRITE GROUP_READ)

# install(TARGETS myExe mySharedLib myStaticLib
#     RUNTIME DESTINATION bin
#     LIBRARY DESTINATION lib
#     ARCHIVE DESTINATION lib/static)
# install(TARGETS mySharedLib DESTINATION /some/full/path)
add_executable(ring_insite EXCLUDE_FROM_ALL ring.cpp insite_adapter.cpp)
add_dependencies(examples ring_insite)

find_package(cpprestsdk REQUIRED)
target_link_libraries(ring_insite PRIVATE arbor arborenv arbor-sup ext-tclap ext-json cpprestsdk::cpprest)

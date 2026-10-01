find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

# Linked as objects, not an archive: strong import implementations here must
# replace the weak defaults in the generated imports.cpp.
add_library(cafe_runtime OBJECT
    src/memory.cpp
    src/loader.cpp
    src/dispatch.cpp
    src/diagnostics.cpp)
target_include_directories(cafe_runtime PUBLIC include)
target_compile_features(cafe_runtime PUBLIC cxx_std_20)
target_link_libraries(cafe_runtime PUBLIC cafe_rpx OpenSSL::Crypto)
target_compile_options(cafe_runtime PRIVATE -Wall -Wextra -Wpedantic)

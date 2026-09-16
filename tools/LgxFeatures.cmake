# What the linked liblgx offers. lgx_extract_assets (assets-only extraction) is newer
# than logos-package d96801f; without it contracts come out of a variant extraction.
include(CheckCXXSourceCompiles)

# bridge_lgx_features(<lgx include dir> <liblgx path> <out var>): <out var> is 1 or 0.
function(bridge_lgx_features include_dir library out_var)
    set(CMAKE_REQUIRED_INCLUDES "${include_dir}")
    set(CMAKE_REQUIRED_LIBRARIES "${library}")
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles([=[
        #include <lgx.h>
        int main() { return lgx_extract_assets(lgx_load(""), "").success ? 0 : 1; }
    ]=] BRIDGE_LGX_HAS_EXTRACT_ASSETS)
    if(BRIDGE_LGX_HAS_EXTRACT_ASSETS)
        set(${out_var} 1 PARENT_SCOPE)
        message(STATUS "liblgx has lgx_extract_assets: contracts come from an assets-only extraction")
    else()
        set(${out_var} 0 PARENT_SCOPE)
        message(STATUS "liblgx lacks lgx_extract_assets: contracts come from a variant extraction")
    endif()
endfunction()

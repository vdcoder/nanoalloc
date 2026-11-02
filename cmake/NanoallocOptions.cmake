# Shared compiler settings for nanoalloc targets.

# Warnings for the library itself: strict, including implicit conversions.
function(nanoalloc_apply_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
            -Wcast-align -Wundef)
    endif()
    _nanoalloc_apply_werror(${target})
endfunction()

# Warnings for tests, benchmarks and examples.
function(nanoalloc_apply_warnings target)
    if(MSVC)
        # C4324: padding added for over-aligned test types (intended).
        target_compile_options(${target} PRIVATE /W4 /permissive- /wd4324)
        # Allow portable C library calls such as std::getenv.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual)
    endif()
    _nanoalloc_apply_werror(${target})
endfunction()

function(_nanoalloc_apply_werror target)
    if(NANOALLOC_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Enables the sanitizers listed in NANOALLOC_SANITIZE (a ;- or ,-separated
# list such as "address;undefined") for all targets created afterwards in the
# calling directory and its subdirectories. MSVC only supports "address".
macro(nanoalloc_enable_sanitizers)
    if(NANOALLOC_SANITIZE)
        string(REPLACE "," ";" _nanoalloc_sanitizers "${NANOALLOC_SANITIZE}")
        if(MSVC)
            if("address" IN_LIST _nanoalloc_sanitizers)
                # MSVC's AddressSanitizer is incompatible with /RTC run-time checks.
                foreach(_nanoalloc_flags_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG)
                    string(REGEX REPLACE "/RTC[1csu]+" ""
                           ${_nanoalloc_flags_var} "${${_nanoalloc_flags_var}}")
                endforeach()
                add_compile_options(/fsanitize=address /Zi)
                add_link_options(/INCREMENTAL:NO)
            endif()
        else()
            list(JOIN _nanoalloc_sanitizers "," _nanoalloc_joined)
            add_compile_options(-fsanitize=${_nanoalloc_joined}
                                -fno-omit-frame-pointer -fno-sanitize-recover=all)
            add_link_options(-fsanitize=${_nanoalloc_joined})
        endif()
        message(STATUS "nanoalloc: sanitizers enabled: ${NANOALLOC_SANITIZE}")
    endif()
endmacro()

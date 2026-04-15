macro( build_libmpeg2 )

if( WIN32 OR MINGW )

    # On Windows/MinGW, libmpeg2 is pre-built from bundled source by the
    # CI workflow (see cmake-Windows64-romlogger.yml) and installed to /mingw64.
    # We look for the static library (.a) because the MSYS2 DLL import
    # library does not export symbols in a compatible way.
    find_library( MPEG2_LIB NAMES libmpeg2.a mpeg2 REQUIRED
        HINTS /mingw64/lib $ENV{MINGW_PREFIX}/lib )
    find_path( MPEG2_INC NAMES mpeg2.h PATH_SUFFIXES mpeg2dec REQUIRED
        HINTS /mingw64/include $ENV{MINGW_PREFIX}/include )

    # find_path already points to the dir containing mpeg2.h directly
    set( MPEG2_INCLUDE_DIRS "${MPEG2_INC}" )
    set( MPEG2_LIBRARIES    "${MPEG2_LIB}" )
    set( MPEG2_FOUND ON )

    # Dummy target so add_dependencies(vldp libmpeg2) does not fail
    add_custom_target( libmpeg2 )

    message( STATUS "Using pre-built libmpeg2: ${MPEG2_LIB}" )
    message( STATUS "libmpeg2 include dir: ${MPEG2_INCLUDE_DIRS}" )

else()

    if( CMAKE_CROSSCOMPILING )
        string( REGEX MATCH "([-A-Za-z0-9\\._]+)-(gcc|cc)$" RESULT ${CMAKE_C_COMPILER} )
        string( REGEX REPLACE "-(gcc|cc)$" "" RESULT ${RESULT} )
        set( CONFIGURE_ARGS "--host=${RESULT}" )
    endif()

    externalproject_add( libmpeg2
        PREFIX ${CMAKE_CURRENT_BINARY_DIR}/3rdparty
        URL ../../../src/3rdparty/libmpeg2/libmpeg2-master.tgz
        URL_HASH SHA256=ada613ca604ac5442349facce1fa3eb1398c4aaebaf0fc6053bd9615566497c9
        CONFIGURE_COMMAND autoreconf -f -i && <SOURCE_DIR>/configure ${CONFIGURE_ARGS} --quiet --prefix=${CMAKE_CURRENT_BINARY_DIR}/3rdparty --disable-shared --enable-static --disable-sdl
        BUILD_IN_SOURCE 1
        BUILD_COMMAND make V=0
        INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/3rdparty
        INSTALL_COMMAND make LIBTOOLFLAGS=--silent install
        ${DOWNLOAD_ARGS}
    )

    set( MPEG2_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/3rdparty/include/mpeg2dec )
    set( MPEG2_LIBRARIES ${CMAKE_CURRENT_BINARY_DIR}/3rdparty/lib/libmpeg2.a )
    set( MPEG2_FOUND ON )

endif()

endmacro( build_libmpeg2 )

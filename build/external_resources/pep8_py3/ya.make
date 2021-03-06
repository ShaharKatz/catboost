RESOURCES_LIBRARY()



IF (NOT HOST_OS_DARWIN AND NOT HOST_OS_LINUX AND NOT HOST_OS_WINDOWS)
    MESSAGE(FATAL_ERROR Unsupported host platform for PEP8_PY3)
ENDIF()

DECLARE_EXTERNAL_HOST_RESOURCES_BUNDLE(
    PEP8_PY3
    sbr:1387597678 FOR DARWIN
    sbr:1387598201 FOR LINUX
    sbr:1387597915 FOR WIN32
)

END()

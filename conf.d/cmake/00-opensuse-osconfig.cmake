message(STATUS "*** Notice: OpenSuSe LUA-5.3+DynApi")
list(APPEND PKG_REQUIRED_LIST lua>=5.3)
set(CMAKE_INSTALL_PREFIX $ENV{HOME}/opt)
set(USE_EFENCE 0)

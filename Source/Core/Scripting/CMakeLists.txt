add_library(scripting
  ScriptingEngine.cpp
  ScriptingEngine.h
  ScriptList.cpp
  ScriptList.h
  Python/coroutine.cpp
  Python/coroutine.h
  Python/PyScriptingBackend.cpp
  Python/PyScriptingBackend.h
  Python/Modules/controllermodule.cpp
  Python/Modules/controllermodule.h
  Python/Modules/debugmodule.cpp
  Python/Modules/debugmodule.h
  Python/Modules/doliomodule.cpp
  Python/Modules/doliomodule.h
  Python/Modules/dolphinmodule.cpp
  Python/Modules/dolphinmodule.h
  Python/Modules/eventmodule.cpp
  Python/Modules/eventmodule.h
  Python/Modules/guimodule.cpp
  Python/Modules/guimodule.h
  Python/Modules/memorymodule.cpp
  Python/Modules/memorymodule.h
  Python/Modules/registersmodule.cpp
  Python/Modules/registersmodule.h
  Python/Modules/savestatemodule.cpp
  Python/Modules/savestatemodule.h
  Python/Modules/utilmodule.cpp
  Python/Modules/utilmodule.h
  Python/Utils/as_py_func.h
  Python/Utils/convert.h
  Python/Utils/fmt.h
  Python/Utils/invoke.h
  Python/Utils/module.h
  Python/Utils/object_wrapper.cpp
  Python/Utils/object_wrapper.h
)

find_package(Python3 REQUIRED COMPONENTS Development Interpreter)

get_filename_component(DOLPHIN_PYTHON_HOME "${Python3_EXECUTABLE}" DIRECTORY)
set(DOLPHIN_PYTHON_DLLS "${DOLPHIN_PYTHON_HOME}/DLLs")
set(DOLPHIN_PYTHON_SITE_PACKAGES "${DOLPHIN_PYTHON_HOME}/Lib/site-packages")
set(DOLPHIN_PYTHON_ZIP "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.zip")

file(TO_CMAKE_PATH "${DOLPHIN_PYTHON_HOME}" DOLPHIN_PYTHON_HOME)
file(TO_CMAKE_PATH "${DOLPHIN_PYTHON_DLLS}" DOLPHIN_PYTHON_DLLS)
file(TO_CMAKE_PATH "${Python3_STDLIB}" DOLPHIN_PYTHON_STDLIB)
file(TO_CMAKE_PATH "${DOLPHIN_PYTHON_SITE_PACKAGES}" DOLPHIN_PYTHON_SITE_PACKAGES)
file(TO_CMAKE_PATH "${Python3_RUNTIME_LIBRARY}" DOLPHIN_PYTHON_RUNTIME_LIBRARY)

set(DOLPHIN_PYTHON_RUNTIME_LIBRARY "${DOLPHIN_PYTHON_RUNTIME_LIBRARY}" CACHE INTERNAL
    "Python runtime DLL used by Dolphin scripting")
set(DOLPHIN_PYTHON_RUNTIME_FILENAME "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.dll"
    CACHE INTERNAL "Python runtime DLL filename used by Dolphin scripting")

target_include_directories(scripting PRIVATE ${Python3_INCLUDE_DIRS})
target_compile_definitions(scripting PRIVATE
  DOLPHIN_PYTHON_HOME="${DOLPHIN_PYTHON_HOME}"
  DOLPHIN_PYTHON_DLLS="${DOLPHIN_PYTHON_DLLS}"
  DOLPHIN_PYTHON_STDLIB="${DOLPHIN_PYTHON_STDLIB}"
  DOLPHIN_PYTHON_SITE_PACKAGES="${DOLPHIN_PYTHON_SITE_PACKAGES}"
  DOLPHIN_PYTHON_ZIP="${DOLPHIN_PYTHON_ZIP}"
)
# TODO imgui shouldn't be here
target_link_libraries(scripting PRIVATE Python3::Python imgui core nfd)

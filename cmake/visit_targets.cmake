
# Обходит ВСЕ таргеты, определённые в пределах вашего проекта
# и вызывает переданный визитор с аргументами:
#   <name> <type> <binary_dir>
macro(visit_project_targets visitor)
    if(NOT COMMAND ${visitor})
        message(FATAL_ERROR "Visitor '${visitor}' не найден (ни функция, ни макрос).")
    endif()

    # Внутренняя рекурсивная функция
    macro(_collect dir)
        # Таргеты в текущей директории
        get_property(local_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)

        foreach(tgt IN LISTS local_targets)
            if(NOT TARGET "${tgt}")
                continue()
            endif()

            get_target_property(tgt_type "${tgt}" TYPE)
            # Пропускаем служебные цели
            if(tgt_type STREQUAL "UTILITY")
                continue()
            endif()

            # Где объявлен таргет
            get_target_property(src_dir "${tgt}" SOURCE_DIR)
            if(NOT src_dir)
                continue()
            endif()

            # Оставляем только таргеты, чьи исходники внутри ${CMAKE_SOURCE_DIR}
            # (кроссплатформенно, без регэкспов и слешей)
            file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${src_dir}")
            if(_rel MATCHES "^[.][.]")  # вне корня проекта
                continue()
            endif()

            # Бинарная директория, соответствующая исходной директории таргета
            get_property(bin_dir DIRECTORY "${src_dir}" PROPERTY BINARY_DIR)

            # Вызываем визитор: <name> <type> <binary_dir>
            cmake_language(CALL ${visitor} "${tgt}" "${tgt_type}" "${bin_dir}")
        endforeach()

        # Рекурсивно обрабатываем поддиректории
        get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        foreach(sub IN LISTS subdirs)
            _collect("${sub}")
        endforeach()
    endmacro()

    _collect("${CMAKE_SOURCE_DIR}")
endmacro()


function(generate_launch_json_file)
  set(launch_file "${CMAKE_SOURCE_DIR}/.vscode/launch.json")

  if(EXISTS ${launch_file})
    message(STATUS ".vscode/launch.json already exists, skipping creation.")
    return()
  endif()

  message(STATUS "Creating .vscode/launch.json with target configurations...")

  set(launch_configurations "")

  macro(append_launch_configuration name type binary_dir)
    if(${type} STREQUAL "EXECUTABLE")
        string(REGEX REPLACE "/$" "" binary_dir_clean "${binary_dir}")
        if(WIN32)
          set(exec_path "${binary_dir_clean}/${name}.exe")
        else()
          set(exec_path "${binary_dir_clean}/${name}")
        endif()

        set(config_entry
          "        {
                \"name\": \"${name} (${CMAKE_BUILD_TYPE})\",
                \"type\": \"cppvsdbg\",
                \"request\": \"launch\",
                \"program\": \"${exec_path}\",
                \"args\": [],
                \"stopAtEntry\": false,
                \"cwd\": \"\${fileDirname}\",
                \"environment\": [],
                \"console\": \"externalTerminal\"
            }"
        )

        if(launch_configurations STREQUAL "")
          set(launch_configurations "${config_entry}")
        else()
          set(launch_configurations "${launch_configurations},\n${config_entry}")
        endif()

    endif()
  endmacro()

  visit_project_targets(append_launch_configuration)

  file(WRITE ${launch_file}
"{
    \"version\": \"0.2.0\",
    \"configurations\": [
${launch_configurations}
    ]
}"
  )
endfunction()

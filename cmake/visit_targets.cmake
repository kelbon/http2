
# Обходит ВСЕ таргеты, определённые в пределах вашего проекта
# и вызывает переданный визитор с аргументами:
#   <name> <type> <binary_dir>
# WHAT_VISIT may be BUILDSYSTEM_TARGETS or IMPORTED_TARGETS
macro(visit_project_targets visitor WHAT_VISIT)
    if(NOT COMMAND ${visitor})
        message(FATAL_ERROR "Visitor '${visitor}' не найден (ни функция, ни макрос).")
    endif()

    # Внутренняя рекурсивная функция
    macro(_collect dir)
        # Таргеты в текущей директории
        get_property(local_targets DIRECTORY "${dir}" PROPERTY ${WHAT_VISIT})

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
  file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/.vscode")

  message(STATUS "Processing .vscode/launch.json with target configurations...")

  set(new_configs "")

  macro(append_launch_configuration name type binary_dir)
    if(${type} STREQUAL "EXECUTABLE")
        string(REGEX REPLACE "/$" "" binary_dir_clean "${binary_dir}")
        if(WIN32)
          set(exec_path "${binary_dir_clean}/${name}.exe")
        else()
          set(exec_path "${binary_dir_clean}/${name}")
        endif()
        
    if(WIN32)
        set(config_json
"{
    \"name\": \"${name} (${CMAKE_BUILD_TYPE})\",
    \"type\": \"cppvsdbg\",
    \"request\": \"launch\",
    \"program\": \"${exec_path}\",
    \"args\": [],
    \"stopAtEntry\": false,
    \"cwd\": \"${binary_dir_clean}\",
    \"environment\": [],
    \"console\": \"externalTerminal\"
}"
        )
    else()
        set(config_json
"{
    \"name\": \"${name} (${CMAKE_BUILD_TYPE})\",
    \"type\": \"cppdbg\",
    \"request\": \"launch\",
    \"program\": \"${exec_path}\",
    \"args\": [],
    \"stopAtEntry\": false,
    \"cwd\": \"${binary_dir_clean}\",
    \"environment\": [],
    \"externalConsole\": false,
    \"MIMode\": \"gdb\",
    \"setupCommands\": [
        {
            \"description\": \"Enable pretty-printing for gdb\",
            \"text\": \"-enable-pretty-printing\",
            \"ignoreFailures\": true
        },
        {
            \"description\": \"Set Disassembly Flavor to Intel\",
            \"text\": \"-gdb-set disassembly-flavor intel\",
            \"ignoreFailures\": true
        }
    ]
}"
        )
    endif()

        list(APPEND new_configs "${config_json}")
    endif()
  endmacro()

  visit_project_targets(append_launch_configuration BUILDSYSTEM_TARGETS)

  if(NOT new_configs)
    message(STATUS "No executable targets found, nothing to add to launch.json.")
    return()
  endif()

  if(NOT EXISTS ${launch_file})
    message(STATUS "Creating new .vscode/launch.json...")
    string(JOIN ",\n    " configs_joined ${new_configs})
    file(WRITE ${launch_file}
"{
    \"version\": \"0.2.0\",
    \"configurations\": [
    ${configs_joined}
    ]
}"
    )
  else()
    message(STATUS "Adding new configurations to existing .vscode/launch.json if not present...")
    file(READ ${launch_file} existing_content)
    set(existing_json "${existing_content}")

    # Get configurations array
    string(JSON configs_json ERROR_VARIABLE err GET "${existing_json}" "configurations")
    if(err)
      message(WARNING "Failed to get 'configurations' from existing launch.json: ${err}. Assuming empty array.")
      set(configs_json "[]")
    endif()

    # Check type
    string(JSON conf_type TYPE "${existing_json}" "configurations")
    if(NOT conf_type STREQUAL "ARRAY")
      message(WARNING "'configurations' is not an array in existing launch.json. Resetting to empty array.")
      set(configs_json "[]")
    endif()

    string(JSON num_configs LENGTH "${configs_json}")

    set(existing_names "")
    if(num_configs GREATER 0)
      math(EXPR last_index "${num_configs} - 1")
      foreach(i RANGE 0 ${last_index})
        string(JSON cfg_json GET "${configs_json}" ${i})
        string(JSON cfg_name ERROR_VARIABLE name_err GET "${cfg_json}" "name")
        if(name_err)
          message(WARNING "Configuration at index ${i} missing 'name': ${name_err}. Skipping.")
          continue()
        endif()
        list(APPEND existing_names "${cfg_name}")
      endforeach()
    endif()

    set(added FALSE)
    foreach(new_cfg_json IN LISTS new_configs)
      string(JSON new_name ERROR_VARIABLE new_name_err GET "${new_cfg_json}" "name")
      if(new_name_err)
        message(WARNING "New configuration missing 'name': ${new_name_err}. Skipping.")
        continue()
      endif()
      if(NOT "${new_name}" IN_LIST existing_names)
        string(JSON configs_json SET "${configs_json}" ${num_configs} "${new_cfg_json}")
        math(EXPR num_configs "${num_configs} + 1")
        list(APPEND existing_names "${new_name}")
        set(added TRUE)
      endif()
    endforeach()

    if(added)
      string(JSON updated_json SET "${existing_json}" "configurations" "${configs_json}")
      file(WRITE ${launch_file} "${updated_json}")
      message(STATUS "New configurations added successfully.")
    else()
      message(STATUS "All configurations already exist, no changes made.")
    endif()
  endif()
endfunction()

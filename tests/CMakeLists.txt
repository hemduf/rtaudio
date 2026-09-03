include_directories(..)
if (WIN32)
    include_directories(../include)

    function(copy_runtime_deps target)
        if (WIN32)
            add_custom_command(
                TARGET ${target}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}>
                    $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS
            )
        endif()
    endfunction()
endif (WIN32)

list(GET LIB_TARGETS 0 LIBRTAUDIO)

if(EMSCRIPTEN)
    add_executable(webaudio_api webaudio_api.cpp)
    target_link_libraries(webaudio_api ${LIBRTAUDIO} ${LINKLIBS})
    add_test(NAME webaudio_api COMMAND webaudio_api)

    # Keep every interactive WebAudio example in this list. CI builds the
    # aggregate `webaudio_examples` target so adding an example here makes it
    # part of the WebAssembly build validation automatically.
    set(WEBAUDIO_EXAMPLE_TARGETS)

    # Interactive browser counterpart to playsaw.cpp. Emscripten uses the
    # checked-in HTML file as its shell and produces webaudio_playsaw.html,
    # webaudio_playsaw.js and webaudio_playsaw.wasm in the build directory.
    add_executable(webaudio_playsaw webaudio_playsaw.cpp)
    target_link_libraries(webaudio_playsaw ${LIBRTAUDIO} ${LINKLIBS})
    set_target_properties(webaudio_playsaw PROPERTIES SUFFIX ".html")
    list(APPEND WEBAUDIO_EXAMPLE_TARGETS webaudio_playsaw)

    # The shell calls these functions through Module._*. Make that public JS
    # contract explicit at link time so optimized builds cannot remove or
    # rename the entry points. Keep _main because EXPORTED_FUNCTIONS replaces
    # the default export list. The WebAudio bootstrap is part of the library's
    # JavaScript integration API and is called by Module.RtAudioWeb.initialize().
    set(WEBAUDIO_PLAYSAW_EXPORTS
        _main,
        _rtaudio_web_initialize_context,
        _rtaudio_webaudio_playsaw_start,
        _rtaudio_webaudio_playsaw_stop,
        _rtaudio_webaudio_playsaw_set_frequency,
        _rtaudio_webaudio_playsaw_set_gain,
        _rtaudio_webaudio_playsaw_frequency,
        _rtaudio_webaudio_playsaw_gain,
        _rtaudio_webaudio_playsaw_meter_left,
        _rtaudio_webaudio_playsaw_meter_right,
        _rtaudio_webaudio_playsaw_is_running,
        _rtaudio_webaudio_playsaw_sample_rate,
        _rtaudio_webaudio_playsaw_buffer_frames)
    string(REPLACE ";" "" WEBAUDIO_PLAYSAW_EXPORTS "${WEBAUDIO_PLAYSAW_EXPORTS}")

    target_link_options(webaudio_playsaw PRIVATE
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/webaudio_playsaw.html"
        "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/webaudio_playsaw_graph.js"
        "-sEXIT_RUNTIME=0"
        "-sEXPORTED_FUNCTIONS=${WEBAUDIO_PLAYSAW_EXPORTS}"
    )

    add_custom_target(webaudio_examples)
    add_dependencies(webaudio_examples ${WEBAUDIO_EXAMPLE_TARGETS})
    return()
endif()

add_executable(audioprobe audioprobe.cpp)
target_link_libraries(audioprobe ${LIBRTAUDIO} ${LINKLIBS})
copy_runtime_deps(audioprobe) # just one to copy the dll in the tests/ dir

add_executable(playsaw playsaw.cpp)
target_link_libraries(playsaw ${LIBRTAUDIO} ${LINKLIBS})

add_executable(playraw playraw.cpp)
target_link_libraries(playraw ${LIBRTAUDIO} ${LINKLIBS})

add_executable(record record.cpp)
target_link_libraries(record ${LIBRTAUDIO} ${LINKLIBS})

add_executable(duplex duplex.cpp)
target_link_libraries(duplex ${LIBRTAUDIO} ${LINKLIBS})

add_executable(apinames apinames.cpp)
target_link_libraries(apinames ${LIBRTAUDIO} ${LINKLIBS})

add_executable(testall testall.cpp)
target_link_libraries(testall ${LIBRTAUDIO} ${LINKLIBS})

add_executable(teststops teststops.cpp)
target_link_libraries(teststops ${LIBRTAUDIO} ${LINKLIBS})

add_test(NAME apinames COMMAND apinames)

find_package(Threads REQUIRED)
llama_add_compile_flags()

if(NOT DEFINED LLAMA_CPP_SOURCE_DIR OR LLAMA_CPP_SOURCE_DIR STREQUAL "")
    set(LLAMA_CPP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/llama.cpp")
endif()
if(NOT DEFINED LLAMA_CPP_BINARY_DIR OR LLAMA_CPP_BINARY_DIR STREQUAL "")
    set(LLAMA_CPP_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/llama.cpp")
endif()

set(AGENT_REQUIRED_SOURCES
    agent.cpp
    agent-loop.cpp
    tool-registry.cpp
    permission.cpp
    permission-async.cpp
    skills/skills-manager.cpp
    agents-md/agents-md-manager.cpp
    subagent/subagent-types.cpp
    subagent/subagent-display.cpp
    subagent/subagent-runner.cpp
    subagent/subagent-output.cpp
    tools/tool-read.cpp
    tools/tool-write.cpp
    tools/tool-edit.cpp
    tools/tool-glob.cpp
    tools/tool-task.cpp
)

if(NOT WIN32)
    list(APPEND AGENT_REQUIRED_SOURCES
        tools/tool-bash.cpp
    )
endif()

if(NOT WIN32)
    list(APPEND AGENT_REQUIRED_SOURCES
        mcp/mcp-client.cpp
        mcp/mcp-server-manager.cpp
        mcp/mcp-tool-wrapper.cpp
    )
endif()

set(AGENT_MISSING_SOURCES "")
foreach(src IN LISTS AGENT_REQUIRED_SOURCES)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${src}")
        list(APPEND AGENT_MISSING_SOURCES "${src}")
    endif()
endforeach()

if(NOT AGENT_MISSING_SOURCES)
    set(TARGET llama-agent)
    add_executable(${TARGET} ${AGENT_REQUIRED_SOURCES})
    target_link_libraries(${TARGET} PRIVATE server-context common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(${TARGET} PRIVATE cxx_std_17)
    target_include_directories(${TARGET} PRIVATE ${LLAMA_CPP_SOURCE_DIR}/tools/server)
    if(LLAMA_TOOLS_INSTALL)
        install(TARGETS ${TARGET} RUNTIME)
    endif()
endif()

if(LLAMA_HTTPLIB)
    set(AGENT_SERVER_REQUIRED_SOURCES
        server/agent-server.cpp
        server/agent-session.cpp
        server/agent-routes.cpp
        agent-loop.cpp
        tool-registry.cpp
        permission.cpp
        permission-async.cpp
        skills/skills-manager.cpp
        agents-md/agents-md-manager.cpp
        subagent/subagent-types.cpp
        subagent/subagent-display.cpp
        subagent/subagent-runner.cpp
        subagent/subagent-output.cpp
        tools/tool-read.cpp
        tools/tool-write.cpp
        tools/tool-edit.cpp
        tools/tool-glob.cpp
        tools/tool-task.cpp
        ${LLAMA_CPP_SOURCE_DIR}/tools/server/server-http.cpp
        ${LLAMA_CPP_SOURCE_DIR}/tools/server/server-models.cpp
    )

    if(NOT WIN32)
        list(APPEND AGENT_SERVER_REQUIRED_SOURCES
            tools/tool-bash.cpp
        )
    endif()

    if(NOT WIN32)
        list(APPEND AGENT_SERVER_REQUIRED_SOURCES
            mcp/mcp-client.cpp
            mcp/mcp-server-manager.cpp
            mcp/mcp-tool-wrapper.cpp
        )
    endif()

    set(AGENT_SERVER_MISSING_SOURCES "")
    foreach(src IN LISTS AGENT_SERVER_REQUIRED_SOURCES)
        if(NOT EXISTS "${src}" AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${src}")
            list(APPEND AGENT_SERVER_MISSING_SOURCES "${src}")
        endif()
    endforeach()

    if(NOT AGENT_SERVER_MISSING_SOURCES)
        set(AGENT_SERVER_TARGET llama-agent-server)
        add_executable(${AGENT_SERVER_TARGET} ${AGENT_SERVER_REQUIRED_SOURCES})
        target_link_libraries(${AGENT_SERVER_TARGET} PRIVATE server-context common cpp-httplib ${CMAKE_THREAD_LIBS_INIT})
        target_compile_features(${AGENT_SERVER_TARGET} PRIVATE cxx_std_17)
        target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${LLAMA_CPP_SOURCE_DIR}/tools/server)
        target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
        target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${LLAMA_CPP_BINARY_DIR}/tools/server)
        add_dependencies(${AGENT_SERVER_TARGET} llama-server)
        if(WIN32)
            target_link_libraries(${AGENT_SERVER_TARGET} PRIVATE ws2_32)
        endif()
        if(LLAMA_TOOLS_INSTALL)
            install(TARGETS ${AGENT_SERVER_TARGET} RUNTIME)
        endif()
    endif()

    set(AGENT_SDK_LIB_REQUIRED_SOURCES
        sdk/http-agent.cpp
        sdk/mcp-manager.cpp
        sdk/prompt-builder.cpp
        sdk/tool-task-sdk.cpp
        tool-registry.cpp
        permission.cpp
        permission-async.cpp
        skills/skills-manager.cpp
        agents-md/agents-md-manager.cpp
        subagent/subagent-types.cpp
        tools/tool-read.cpp
        tools/tool-write.cpp
        tools/tool-edit.cpp
        tools/tool-glob.cpp
    )

    if(NOT WIN32)
        list(APPEND AGENT_SDK_LIB_REQUIRED_SOURCES
            tools/tool-bash.cpp
            mcp/mcp-client.cpp
            mcp/mcp-server-manager.cpp
            mcp/mcp-tool-wrapper.cpp
        )
    endif()

    set(AGENT_SDK_LIB_MISSING_SOURCES "")
    foreach(src IN LISTS AGENT_SDK_LIB_REQUIRED_SOURCES)
        if(NOT EXISTS "${src}" AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${src}")
            list(APPEND AGENT_SDK_LIB_MISSING_SOURCES "${src}")
        endif()
    endforeach()

    if(NOT AGENT_SDK_LIB_MISSING_SOURCES)
        set(AGENT_SDK_LIB_TARGET llama-agent-sdk-lib)
        add_library(${AGENT_SDK_LIB_TARGET} STATIC ${AGENT_SDK_LIB_REQUIRED_SOURCES})
        target_link_libraries(${AGENT_SDK_LIB_TARGET} PUBLIC server-context common cpp-httplib ${CMAKE_THREAD_LIBS_INIT})
        target_compile_features(${AGENT_SDK_LIB_TARGET} PUBLIC cxx_std_17)
        target_include_directories(${AGENT_SDK_LIB_TARGET} PUBLIC ${CMAKE_CURRENT_LIST_DIR})
        target_include_directories(${AGENT_SDK_LIB_TARGET} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/sdk)
        target_include_directories(${AGENT_SDK_LIB_TARGET} PUBLIC ${CMAKE_SOURCE_DIR})
        target_include_directories(${AGENT_SDK_LIB_TARGET} PUBLIC ${LLAMA_CPP_SOURCE_DIR}/tools/server)
        if(WIN32)
            target_link_libraries(${AGENT_SDK_LIB_TARGET} PUBLIC ws2_32)
        endif()
        if(LLAMA_TOOLS_INSTALL)
            install(TARGETS ${AGENT_SDK_LIB_TARGET} ARCHIVE)
        endif()

        set(AGENT_SDK_TARGET llama-agent-sdk)
        add_executable(${AGENT_SDK_TARGET} sdk/sdk-cli.cpp)
        target_link_libraries(${AGENT_SDK_TARGET} PRIVATE ${AGENT_SDK_LIB_TARGET})
        target_compile_features(${AGENT_SDK_TARGET} PRIVATE cxx_std_17)
        if(LLAMA_TOOLS_INSTALL)
            install(TARGETS ${AGENT_SDK_TARGET} RUNTIME)
        endif()
    endif()
endif()

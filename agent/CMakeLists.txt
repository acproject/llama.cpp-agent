find_package(Threads REQUIRED)
llama_add_compile_flags()

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
    tools/tool-bash.cpp
    tools/tool-read.cpp
    tools/tool-write.cpp
    tools/tool-edit.cpp
    tools/tool-glob.cpp
    tools/tool-task.cpp
)

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
    target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/tools/server)
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
        tools/tool-bash.cpp
        tools/tool-read.cpp
        tools/tool-write.cpp
        tools/tool-edit.cpp
        tools/tool-glob.cpp
        tools/tool-task.cpp
        ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/tools/server/server-http.cpp
    )

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
        target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/tools/server)
        target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
        target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_BINARY_DIR}/third_party/llama.cpp/tools/server)
        add_dependencies(${AGENT_SERVER_TARGET} llama-server)
        if(WIN32)
            target_link_libraries(${AGENT_SERVER_TARGET} PRIVATE ws2_32)
        endif()
        if(LLAMA_TOOLS_INSTALL)
            install(TARGETS ${AGENT_SERVER_TARGET} RUNTIME)
        endif()
    endif()
endif()

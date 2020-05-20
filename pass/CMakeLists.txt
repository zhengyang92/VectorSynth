add_llvm_library(vectorsynth MODULE pass.cpp)

if (CLANG_PLUGIN)
  message(STATUS "Clang plugin support enabled")
  set_target_properties(vectorsynth PROPERTIES
      COMPILE_FLAGS "-DCLANG_PLUGIN"
  )
endif()

target_link_libraries(vectorsynth PRIVATE ${VECTORSYNTH_LIBS_LLVM} ${Z3_LIBRARIES}
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs>)

if(APPLE)
    set_target_properties(vectorsynth PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif(APPLE)

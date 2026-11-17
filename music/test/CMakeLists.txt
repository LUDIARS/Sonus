snd_add_test(music_test
  SOURCES music_smoke_test.cpp music_timeline_test.cpp
  LINK sonus_music sonus_mocks)
# Unit tests reach the module's private headers (time line math).
target_include_directories(music_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

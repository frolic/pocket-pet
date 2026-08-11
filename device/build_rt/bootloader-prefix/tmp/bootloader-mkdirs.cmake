# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/kevin/esp/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "/Users/kevin/esp/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader"
  "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader-prefix"
  "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader-prefix/tmp"
  "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader-prefix/src/bootloader-stamp"
  "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader-prefix/src"
  "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/kevin/Projects/frolic/pocket-pikachu/.claude/worktrees/sleep-handoff-docs-d67e58/device/build_rt/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()

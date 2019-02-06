#   Copyright 2016 Comcast Cable Communications Management, LLC
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

set(PROJ_CIMPMSG cimpmsg)

file(GLOB HEADERS cimpmsg.h cimpmsg_log.h)
set(SOURCES cimpmsg.c)

add_library(${PROJ_CIMPMSG} STATIC ${HEADERS} ${SOURCES})
add_library(${PROJ_CIMPMSG}.shared SHARED ${HEADERS} ${SOURCES})
set_target_properties(${PROJ_CIMPMSG}.shared PROPERTIES OUTPUT_NAME ${PROJ_CIMPMSG})

if (BUILD_YOCTO)
target_link_libraries(${PROJ_CIMPMSG}.shared anl pthread m)
endif (BUILD_YOCTO)

install (TARGETS ${PROJ_CIMPMSG} DESTINATION lib${LIB_SUFFIX})
install (TARGETS ${PROJ_CIMPMSG}.shared DESTINATION lib${LIB_SUFFIX})
install (FILES cimpmsg.h DESTINATION include/${PROJ_CIMPMSG})
install (FILES cimpmsg_log.h DESTINATION include/${PROJ_CIMPMSG})

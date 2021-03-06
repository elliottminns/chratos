add_executable (chratos_node
	daemon.cpp
	daemon.hpp
	entry.cpp)

target_link_libraries (chratos_node
	node
	secure
	argon2
	Boost::boost
	${PLATFORM_LIBS}
)

target_compile_definitions(chratos_node
	PRIVATE
		-DRAIBLOCKS_VERSION_MAJOR=${CPACK_PACKAGE_VERSION_MAJOR}
		-DRAIBLOCKS_VERSION_MINOR=${CPACK_PACKAGE_VERSION_MINOR})

set_target_properties (chratos_node
	PROPERTIES
		COMPILE_FLAGS
			"-DQT_NO_KEYWORDS -DBOOST_ASIO_HAS_STD_ARRAY=1")

if (RAIBLOCKS_GUI AND NOT APPLE)
	install(TARGETS chratos_node
			RUNTIME DESTINATION bin
			LIBRARY DESTINATION lib
			ARCHIVE DESTINATION lib
	)
endif()

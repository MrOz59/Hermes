# linux specific target definitions

# The card broker reaches the Hermes-KMS driver's configfs group, which is
# root's, so it is a program of its own rather than anything the streaming host
# links: a few hundred lines over one socket, with none of Hermes' dependencies
# and no reason to grow any.
if(SUNSHINE_BUILD_CARD_BROKER)
    add_executable(hermes-kms-card-broker "${CMAKE_SOURCE_DIR}/tools/hermes-kms-card-broker.cpp")
    set_target_properties(hermes-kms-card-broker PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON)
    target_compile_options(hermes-kms-card-broker PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
endif()

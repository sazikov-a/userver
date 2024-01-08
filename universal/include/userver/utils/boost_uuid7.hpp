#pragma once

/// @file userver/utils/boost_uuid7.hpp
/// @brief @copybrief utils::generators::GenerateBoostUuid7()

#include <boost/uuid/uuid.hpp>

USERVER_NAMESPACE_BEGIN

namespace utils {

/// Generators
namespace generators {

/// @brief Generates UUID v7
///
/// See
/// https://datatracker.ietf.org/doc/html/draft-ietf-uuidrev-rfc4122bis#name-uuid-version-7
boost::uuids::uuid GenerateBoostUuid7();

}  // namespace generators

}  // namespace utils

USERVER_NAMESPACE_END

#pragma once

/// @file storages/postgres/io/integral_types.hpp
/// @brief Integral types I/O support

#include <boost/endian/conversion.hpp>

#include <type_traits>

#include <storages/postgres/exceptions.hpp>
#include <storages/postgres/io/buffer_io_base.hpp>
#include <storages/postgres/io/traits.hpp>
#include <storages/postgres/io/type_mapping.hpp>

namespace storages::postgres::io {

namespace detail {

template <std::size_t Size>
struct IntegralType;

template <>
struct IntegralType<2> {
  using type = Smallint;
};

template <>
struct IntegralType<4> {
  using type = Integer;
};

template <>
struct IntegralType<8> {
  using type = Bigint;
};

template <std::size_t Size>
struct IntegralBySizeParser {
  using IntType = typename IntegralType<Size>::type;
  constexpr static std::size_t size = Size;

  static IntType ParseBuffer(const FieldBuffer& buf) {
    IntType i;
    std::memcpy(&i, buf.buffer, size);
    return boost::endian::big_to_native(i);
  }
};

template <typename T>
struct IntegralBinaryParser : BufferParserBase<T> {
  using BaseType = BufferParserBase<T>;
  using BaseType::BaseType;

  void operator()(const FieldBuffer& buf) {
    switch (buf.length) {
      case 2:
        this->value = IntegralBySizeParser<2>::ParseBuffer(buf);
        break;
      case 4:
        this->value = IntegralBySizeParser<4>::ParseBuffer(buf);
        break;
      case 8:
        this->value = IntegralBySizeParser<8>::ParseBuffer(buf);
        break;
      default:
        throw InvalidInputBufferSize{buf.length, "for an integral value type"};
    }
  }
};

template <typename T>
struct IntegralBinaryFormatter {
  static constexpr std::size_t size = sizeof(T);
  using BySizeType = typename IntegralType<size>::type;

  T value;

  explicit IntegralBinaryFormatter(T val) : value{val} {}
  template <typename Buffer>
  void operator()(const UserTypes&, Buffer& buf) const {
    buf.reserve(buf.size() + size);
    auto tmp = boost::endian::native_to_big(static_cast<BySizeType>(value));
    const char* p = reinterpret_cast<char const*>(&tmp);
    const char* e = p + size;
    std::copy(p, e, std::back_inserter(buf));
  }

  /// Write the value to char buffer, the buffer MUST be already resized
  template <typename Iterator>
  void operator()(Iterator buffer) const {
    auto tmp = boost::endian::native_to_big(static_cast<BySizeType>(value));
    const char* p = reinterpret_cast<char const*>(&tmp);
    const char* e = p + size;
    std::copy(p, e, buffer);
  }
};

// 64bit architectures have two types for 64bit integers, this is the second one
using AltBigint =
    std::conditional_t<std::is_same_v<Bigint, long>, long long, long>;
static_assert(sizeof(AltBigint) == sizeof(Bigint));

}  // namespace detail

//@{
/** @name 2 byte integer */
template <>
struct BufferParser<Smallint> : detail::IntegralBinaryParser<Smallint> {
  explicit BufferParser(Smallint& val) : IntegralBinaryParser(val) {}
};

template <>
struct BufferFormatter<Smallint> : detail::IntegralBinaryFormatter<Smallint> {
  explicit BufferFormatter(Smallint val) : IntegralBinaryFormatter(val) {}
};
//@}

//@{
/** @name 4 byte integer */
template <>
struct BufferParser<Integer> : detail::IntegralBinaryParser<Integer> {
  explicit BufferParser(Integer& val) : IntegralBinaryParser(val) {}
};

template <>
struct BufferFormatter<Integer> : detail::IntegralBinaryFormatter<Integer> {
  explicit BufferFormatter(Integer val) : IntegralBinaryFormatter(val) {}
};
//@}

//@{
/** @name 8 byte integer */
template <>
struct BufferParser<Bigint> : detail::IntegralBinaryParser<Bigint> {
  explicit BufferParser(Bigint& val) : IntegralBinaryParser(val) {}
};

template <>
struct BufferFormatter<Bigint> : detail::IntegralBinaryFormatter<Bigint> {
  explicit BufferFormatter(Bigint val) : IntegralBinaryFormatter(val) {}
};

/// @cond
template <>
struct BufferParser<detail::AltBigint>
    : detail::IntegralBinaryParser<detail::AltBigint> {
  explicit BufferParser(detail::AltBigint& val) : IntegralBinaryParser(val) {}
};

template <>
struct BufferFormatter<detail::AltBigint>
    : detail::IntegralBinaryFormatter<detail::AltBigint> {
  explicit BufferFormatter(detail::AltBigint val)
      : IntegralBinaryFormatter(val) {}
};
/// @endcond
//@}

//@{
/** @name boolean */
template <>
struct BufferParser<bool> {
  bool& value;
  explicit BufferParser(bool& val) : value{val} {}
  void operator()(const FieldBuffer& buf);
};

template <>
struct BufferFormatter<bool> {
  bool value;
  explicit BufferFormatter(bool val) : value(val) {}
  template <typename Buffer>
  void operator()(const UserTypes&, Buffer& buf) const {
    buf.push_back(value ? 1 : 0);
  }
};
//@}

//@{
/** @name C++ to PostgreSQL mapping for integral types */
template <>
struct CppToSystemPg<Smallint> : PredefinedOid<PredefinedOids::kInt2> {};
template <>
struct CppToSystemPg<Integer> : PredefinedOid<PredefinedOids::kInt4> {};
template <>
struct CppToSystemPg<Bigint> : PredefinedOid<PredefinedOids::kInt8> {};
template <>
struct CppToSystemPg<bool> : PredefinedOid<PredefinedOids::kBoolean> {};
//@}

}  // namespace storages::postgres::io

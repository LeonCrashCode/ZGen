/* Like std::ofstream but without being incredibly slow.  Backed by a raw fd.
 * Does not support many data types.  Currently, it's targeted at writing ARPA
 * files quickly.
 */
#ifndef UTIL_FAKE_OFSTREAM_H
#define UTIL_FAKE_OFSTREAM_H

#include "shiftreduce/ngram/util/double-conversion/double-conversion.h"
#include "shiftreduce/ngram/util/double-conversion/utils.h"
#include "shiftreduce/ngram/util/file.hh"
#include "shiftreduce/ngram/util/scoped.hh"
#include "shiftreduce/ngram/util/string_piece.hh"

#define BOOST_LEXICAL_CAST_ASSUME_C_LOCALE
#include <boost/lexical_cast.hpp>

namespace util {
class FakeOFStream {
  public:
    // Does not take ownership of out.
    // Allows default constructor, but must call SetFD.
    explicit FakeOFStream(int out = -1, std::size_t buffer_size = 1048576)
      : buf_(util::MallocOrThrow(buffer_size)),
        builder_(static_cast<char*>(buf_.get()), buffer_size),
        // Mostly the default but with inf instead.  And no flags.
        convert_(double_conversion::DoubleToStringConverter::NO_FLAGS, "inf", "NaN", 'e', -6, 21, 6, 0),
        fd_(out),
        buffer_size_(buffer_size) {}

    ~FakeOFStream() {
      if (buf_.get()) Flush();
    }

    void SetFD(int to) {
      if (builder_.position()) Flush();
      fd_ = to;
    }

    FakeOFStream &operator<<(float value) {
      // Odd, but this is the largest number found in the comments.
      EnsureRemaining(double_conversion::DoubleToStringConverter::kMaxPrecisionDigits + 8);
      convert_.ToShortestSingle(value, &builder_);
      return *this;
    }

    FakeOFStream &operator<<(double value) {
      EnsureRemaining(double_conversion::DoubleToStringConverter::kMaxPrecisionDigits + 8);
      convert_.ToShortest(value, &builder_);
      return *this;
    }

    FakeOFStream &operator<<(StringPiece str) {
      if (str.size() > buffer_size_) {
        Flush();
        util::WriteOrThrow(fd_, str.data(), str.size());
      } else {
        EnsureRemaining(str.size());
        builder_.AddSubstring(str.data(), str.size());
      }
      return *this;
    }

    // Inefficient!  TODO: more efficient implementation
    FakeOFStream &operator<<(unsigned value) {
      return *this << boost::lexical_cast<std::string>(value);
    }

    FakeOFStream &operator<<(char c) {
      EnsureRemaining(1);
      builder_.AddCharacter(c);
      return *this;
    }

    // Note this does not sync.
    void Flush() {
      util::WriteOrThrow(fd_, buf_.get(), builder_.position());
      builder_.Reset();
    }

    // Not necessary, but does assure the data is cleared.
    void Finish() {
      Flush();
      // It will segfault trying to null terminate otherwise.
      builder_.Finalize();
      buf_.reset();
      util::FSyncOrThrow(fd_);
    }

  private:
    void EnsureRemaining(std::size_t amount) {
      if (static_cast<std::size_t>(builder_.size() - builder_.position()) <= amount) {
        Flush();
      }
    }

    util::scoped_malloc buf_;
    double_conversion::StringBuilder builder_;
    double_conversion::DoubleToStringConverter convert_;
    int fd_;
    const std::size_t buffer_size_;
};

} // namespace

#endif

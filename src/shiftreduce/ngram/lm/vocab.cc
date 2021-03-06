#include "shiftreduce/ngram/lm/vocab.hh"

#include "shiftreduce/ngram/lm/binary_format.hh"
#include "shiftreduce/ngram/lm/enumerate_vocab.hh"
#include "shiftreduce/ngram/lm/lm_exception.hh"
#include "shiftreduce/ngram/lm/config.hh"
#include "shiftreduce/ngram/lm/weights.hh"
#include "shiftreduce/ngram/util/exception.hh"
#include "shiftreduce/ngram/util/file.hh"
#include "shiftreduce/ngram/util/joint_sort.hh"
#include "shiftreduce/ngram/util/murmur_hash.hh"
#include "shiftreduce/ngram/util/probing_hash_table.hh"

#include <string>

#include <string.h>

namespace lm {
namespace ngram {

namespace detail {
uint64_t HashForVocab(const char *str, std::size_t len) {
  // This proved faster than Boost's hash in speed trials: total load time Murmur 67090000, Boost 72210000
  // Chose to use 64A instead of native so binary format will be portable across 64 and 32 bit.  
  return util::MurmurHash64A(str, len, 0);
}
} // namespace detail

namespace {
// Normally static initialization is a bad idea but MurmurHash is pure arithmetic, so this is ok.  
const uint64_t kUnknownHash = detail::HashForVocab("<unk>", 5);
// Sadly some LMs have <UNK>.  
const uint64_t kUnknownCapHash = detail::HashForVocab("<UNK>", 5);

void ReadWords(int fd, EnumerateVocab *enumerate, WordIndex expected_count, uint64_t offset) {
  util::SeekOrThrow(fd, offset);
  // Check that we're at the right place by reading <unk> which is always first.
  char check_unk[6];
  util::ReadOrThrow(fd, check_unk, 6);
  UTIL_THROW_IF(
      memcmp(check_unk, "<unk>", 6),
      FormatLoadException, 
      "Vocabulary words are in the wrong place.  This could be because the binary file was built with stale gcc and old kenlm.  Stale gcc, including the gcc distributed with RedHat and OS X, has a bug that ignores pragma pack for template-dependent types.  New kenlm works around this, so you'll save memory but have to rebuild any binary files using the probing data structure.");
  if (!enumerate) return;
  enumerate->Add(0, "<unk>");

  // Read all the words after unk.
  const std::size_t kInitialRead = 16384;
  std::string buf;
  buf.reserve(kInitialRead + 100);
  buf.resize(kInitialRead);
  WordIndex index = 1; // Read <unk> already.
  while (true) {
    std::size_t got = util::ReadOrEOF(fd, &buf[0], kInitialRead);
    if (got == 0) break;
    buf.resize(got);
    while (buf[buf.size() - 1]) {
      char next_char;
      util::ReadOrThrow(fd, &next_char, 1);
      buf.push_back(next_char);
    }
    // Ok now we have null terminated strings.  
    for (const char *i = buf.data(); i != buf.data() + buf.size();) {
      std::size_t length = strlen(i);
      enumerate->Add(index++, StringPiece(i, length));
      i += length + 1 /* null byte */;
    }
  }

  UTIL_THROW_IF(expected_count != index, FormatLoadException, "The binary file has the wrong number of words at the end.  This could be caused by a truncated binary file.");
}

} // namespace

WriteWordsWrapper::WriteWordsWrapper(EnumerateVocab *inner) : inner_(inner) {}
WriteWordsWrapper::~WriteWordsWrapper() {}

void WriteWordsWrapper::Add(WordIndex index, const StringPiece &str) {
  if (inner_) inner_->Add(index, str);
  buffer_.append(str.data(), str.size());
  buffer_.push_back(0);
}

SortedVocabulary::SortedVocabulary() : begin_(NULL), end_(NULL), enumerate_(NULL) {}

uint64_t SortedVocabulary::Size(uint64_t entries, const Config &/*config*/) {
  // Lead with the number of entries.  
  return sizeof(uint64_t) + sizeof(uint64_t) * entries;
}

void SortedVocabulary::SetupMemory(void *start, std::size_t allocated, std::size_t entries, const Config &config) {
  assert(allocated >= Size(entries, config));
  // Leave space for number of entries.  
  begin_ = reinterpret_cast<uint64_t*>(start) + 1;
  end_ = begin_;
  saw_unk_ = false;
}

void SortedVocabulary::Relocate(void *new_start) {
  std::size_t delta = end_ - begin_;
  begin_ = reinterpret_cast<uint64_t*>(new_start) + 1;
  end_ = begin_ + delta;
}

void SortedVocabulary::ConfigureEnumerate(EnumerateVocab *to, std::size_t max_entries) {
  enumerate_ = to;
  if (enumerate_) {
    enumerate_->Add(0, "<unk>");
    strings_to_enumerate_.resize(max_entries);
  }
}

WordIndex SortedVocabulary::Insert(const StringPiece &str) {
  uint64_t hashed = detail::HashForVocab(str);
  if (hashed == kUnknownHash || hashed == kUnknownCapHash) {
    saw_unk_ = true;
    return 0;
  }
  *end_ = hashed;
  if (enumerate_) {
    void *copied = string_backing_.Allocate(str.size());
    memcpy(copied, str.data(), str.size());
    strings_to_enumerate_[end_ - begin_] = StringPiece(static_cast<const char*>(copied), str.size());
  }
  ++end_;
  // This is 1 + the offset where it was inserted to make room for unk.  
  return end_ - begin_;
}

void SortedVocabulary::FinishedLoading(ProbBackoff *reorder_vocab) {
  if (enumerate_) {
    if (!strings_to_enumerate_.empty()) {
      util::PairedIterator<ProbBackoff*, StringPiece*> values(reorder_vocab + 1, &*strings_to_enumerate_.begin());
      util::JointSort(begin_, end_, values);
    }
    for (WordIndex i = 0; i < static_cast<WordIndex>(end_ - begin_); ++i) {
      // <unk> strikes again: +1 here.  
      enumerate_->Add(i + 1, strings_to_enumerate_[i]);
    }
    strings_to_enumerate_.clear();
    string_backing_.FreeAll();
  } else {
    util::JointSort(begin_, end_, reorder_vocab + 1);
  }
  SetSpecial(Index("<s>"), Index("</s>"), 0);
  // Save size.  Excludes UNK.  
  *(reinterpret_cast<uint64_t*>(begin_) - 1) = end_ - begin_;
  // Includes UNK.
  bound_ = end_ - begin_ + 1;
}

void SortedVocabulary::LoadedBinary(bool have_words, int fd, EnumerateVocab *to, uint64_t offset) {
  end_ = begin_ + *(reinterpret_cast<const uint64_t*>(begin_) - 1);
  SetSpecial(Index("<s>"), Index("</s>"), 0);
  bound_ = end_ - begin_ + 1;
  if (have_words) ReadWords(fd, to, bound_, offset);
}

namespace {
const unsigned int kProbingVocabularyVersion = 0;
} // namespace

namespace detail {
struct ProbingVocabularyHeader {
  // Lowest unused vocab id.  This is also the number of words, including <unk>.  
  unsigned int version;
  WordIndex bound;
};
} // namespace detail

ProbingVocabulary::ProbingVocabulary() : enumerate_(NULL) {}

uint64_t ProbingVocabulary::Size(uint64_t entries, float probing_multiplier) {
  return ALIGN8(sizeof(detail::ProbingVocabularyHeader)) + Lookup::Size(entries, probing_multiplier);
}

uint64_t ProbingVocabulary::Size(uint64_t entries, const Config &config) {
  return Size(entries, config.probing_multiplier);
}

void ProbingVocabulary::SetupMemory(void *start, std::size_t allocated) {
  header_ = static_cast<detail::ProbingVocabularyHeader*>(start);
  lookup_ = Lookup(static_cast<uint8_t*>(start) + ALIGN8(sizeof(detail::ProbingVocabularyHeader)), allocated);
  bound_ = 1;
  saw_unk_ = false;
}

void ProbingVocabulary::Relocate(void *new_start) {
  header_ = static_cast<detail::ProbingVocabularyHeader*>(new_start);
  lookup_.Relocate(static_cast<uint8_t*>(new_start) + ALIGN8(sizeof(detail::ProbingVocabularyHeader)));
}

void ProbingVocabulary::ConfigureEnumerate(EnumerateVocab *to, std::size_t /*max_entries*/) {
  enumerate_ = to;
  if (enumerate_) {
    enumerate_->Add(0, "<unk>");
  }
}

WordIndex ProbingVocabulary::Insert(const StringPiece &str) {
  uint64_t hashed = detail::HashForVocab(str);
  // Prevent unknown from going into the table.  
  if (hashed == kUnknownHash || hashed == kUnknownCapHash) {
    saw_unk_ = true;
    return 0;
  } else {
    if (enumerate_) enumerate_->Add(bound_, str);
    lookup_.Insert(ProbingVocabularyEntry::Make(hashed, bound_));
    return bound_++;
  }
}

void ProbingVocabulary::FinishedLoading() {
  lookup_.FinishedInserting();
  header_->bound = bound_;
  header_->version = kProbingVocabularyVersion;
  SetSpecial(Index("<s>"), Index("</s>"), 0);
}

void ProbingVocabulary::LoadedBinary(bool have_words, int fd, EnumerateVocab *to, uint64_t offset) {
  UTIL_THROW_IF(header_->version != kProbingVocabularyVersion, FormatLoadException, "The binary file has probing version " << header_->version << " but the code expects version " << kProbingVocabularyVersion << ".  Please rerun build_binary using the same version of the code.");
  bound_ = header_->bound;
  SetSpecial(Index("<s>"), Index("</s>"), 0);
  if (have_words) ReadWords(fd, to, bound_, offset);
}

void MissingUnknown(const Config &config) throw(SpecialWordMissingException) {
  switch(config.unknown_missing) {
    case SILENT:
      return;
    case COMPLAIN:
      if (config.messages) *config.messages << "The ARPA file is missing <unk>.  Substituting log10 probability " << config.unknown_missing_logprob << "." << std::endl;
      break;
    case THROW_UP:
      UTIL_THROW(SpecialWordMissingException, "The ARPA file is missing <unk> and the model is configured to throw an exception.");
  }
}

void MissingSentenceMarker(const Config &config, const char *str) throw(SpecialWordMissingException) {
  switch (config.sentence_marker_missing) {
    case SILENT:
      return;
    case COMPLAIN:
      if (config.messages) *config.messages << "Missing special word " << str << "; will treat it as <unk>.";
      break;
    case THROW_UP:
      UTIL_THROW(SpecialWordMissingException, "The ARPA file is missing " << str << " and the model is configured to reject these models.  Run build_binary -s to disable this check.");
  }
}

} // namespace ngram
} // namespace lm
